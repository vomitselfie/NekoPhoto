// The background-removal harness (docs/background-removal-review.md, item 10).
//
//   matte_tool run <image.png> <model.onnx|none> <outdir> [--refine R] [--band B] [--contrast C] [--shift S]
//                  [--no-cleanup] [--no-decontaminate] [--raw] [--detail N] [--mask <mask.png>]
//                  [--prompt x,y[,label] ...]   (a click-to-select model: label 1 subject, 0 not, 2/3 box corners)
//       runs the pipeline and writes every stage: mask.png (the model's), matte.png (after the panel),
//       trimap.png, chosenF.png, chosenB.png, pairAlpha.png (what the band saw and chose), cutout.png (the
//       layer with its new alpha and edge colours), composite.png (over green).
//   matte_tool eval <dir> <model.onnx|none> [the same options] [--suffix _alpha] [--limit N]
//                   [--categories <aim_category_type.json>]
//       scores every <name>.png with a <name><suffix>.png ground-truth alpha (8-bit grey): SAD (/1000),
//       MSE, MAD, Grad (first Gaussian derivative, sigma 1.4, /1000) and Conn (/1000), as GFM's evaluate.py
//       defines them, for the raw mask and for the refined matte; with AIM-500's category file, means per
//       category and per type as well. With "none" as the model, --mask names a mask file (run) or a mask
//       suffix (eval, default _mask) to start from instead of the model.
#include "compositor/png.h"
#include "compositor/subject.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <map>
#include <fstream>
#include <string>
#include <vector>

using namespace compositor;
namespace fs = std::filesystem;

namespace {

struct Options {
    MatteSettings settings;
    std::string mask, suffix = "_alpha", maskSuffix = "_mask", categories;
    bool refine = true;
    int detail = 0;   // windows of the detail pass; 0 = coarse pass only
    int limit = 0;    // eval: at most this many images (0 = all)
    bool flip = false;   // average the model's prediction with the mirrored image's
    std::vector<PointPrompt> prompts;   // run: click prompts for a prompt model instead of the whole-image pass
};

Options parse(int argc, char** argv, int from) {
    Options o;
    o.settings.matting = 0;
    for (int i = from; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&] { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
        if (a == "--refine") o.settings.refineEdges = std::stod(next());
        else if (a == "--band") o.settings.matting = std::stod(next());
        else if (a == "--contrast") o.settings.contrast = std::stod(next());
        else if (a == "--shift") o.settings.shiftEdge = std::stod(next());
        else if (a == "--no-cleanup") o.settings.cleanup = false;
        else if (a == "--no-decontaminate") o.settings.decontaminate = false;
        else if (a == "--raw") o.refine = false;
        else if (a == "--detail") o.detail = std::stoi(next());
        else if (a == "--mask") o.mask = next();
        else if (a == "--suffix") o.suffix = next();
        else if (a == "--mask-suffix") o.maskSuffix = next();
        else if (a == "--categories") o.categories = next();
        else if (a == "--limit") o.limit = std::stoi(next());
        else if (a == "--flip") o.flip = true;
        else if (a == "--highpass") o.settings.highPass = true;
        else if (a == "--sidewindows") o.settings.sideWindows = true;
        else if (a == "--narrow") o.settings.narrowBand = true;
        else if (a == "--prompt") {
            PointPrompt pt;
            if (std::sscanf(next().c_str(), "%lf,%lf,%d", &pt.x, &pt.y, &pt.label) >= 2) o.prompts.push_back(pt);
        }
        else std::fprintf(stderr, "ignored: %s\n", a.c_str());
    }
    return o;
}

/// A mask or ground-truth alpha from any PNG: the strict 8-bit grey reader first, else the red channel of the
/// decoded image (some datasets store their alphas as palette or RGB files).
std::shared_ptr<GrayImage> readAlpha(const std::string& path, std::string* error) {
    std::string strictError;
    if (auto gray = readPngGray(path, &strictError)) return gray;
    auto image = readPngImage(path, error);
    if (!image) return nullptr;
    auto gray = std::make_shared<GrayImage>(image->width(), image->height());
    for (int y = 0; y < image->height(); y++)
        for (int x = 0; x < image->width(); x++) {
            const uint8_t* p = image->pixel(x, y);
            gray->at(x, y) = p[3] ? uint8_t(std::min(255, p[0] * 255 / p[3])) : 0;
        }
    return gray;
}

std::shared_ptr<GrayImage> maskFor(const Image& image, const std::string& model, const std::string& maskPath, int detail, bool flip, const std::vector<PointPrompt>& prompts, std::string* error) {
    if (model != "none") {
        if (!subjectModelSupported()) { *error = "this build has no OpenCV"; return nullptr; }
        if (!prompts.empty()) return subjectFromPrompts(image, model, prompts, error);
        return detail > 0 ? subjectMaskDetailed(image, model, nullptr, detail, error) : subjectMask(image, model, error, flip);
    }
    if (maskPath.empty()) { *error = "no model and no --mask"; return nullptr; }
    auto mask = readAlpha(maskPath, error);
    if (mask && (mask->width() != image.width() || mask->height() != image.height())) { *error = "mask size differs from the image"; return nullptr; }
    return mask;
}

// ---- Metrics (GFM core/evaluate.py; alpha in 0..1) -----------------------------------------------------

struct Scores { double sad = 0, mse = 0, mad = 0, grad = 0, conn = 0; int count = 0; };

std::vector<float> levelsOf(const GrayImage& g) {
    std::vector<float> out(g.byteCount());
    for (size_t i = 0; i < out.size(); i++) out[i] = g.data()[i] / 255.0f;
    return out;
}

/// Gradient magnitude from Gaussian first-derivative filters, sigma 1.4, truncated at 4 sigma (13 taps).
std::vector<float> gradientMagnitude(const std::vector<float>& a, int w, int h) {
    const double sigma = 1.4;
    const int radius = int(std::ceil(4 * sigma));
    std::vector<double> g(size_t(2 * radius + 1)), dg(size_t(2 * radius + 1));
    double sum = 0;
    for (int i = -radius; i <= radius; i++) { g[size_t(i + radius)] = std::exp(-i * i / (2 * sigma * sigma)); sum += g[size_t(i + radius)]; }
    for (auto& v : g) v /= sum;
    for (int i = -radius; i <= radius; i++) dg[size_t(i + radius)] = -i / (sigma * sigma) * g[size_t(i + radius)];
    auto conv = [&](const std::vector<float>& src, const std::vector<double>& kx, const std::vector<double>& ky) {
        std::vector<float> tmp(src.size()), out(src.size());
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                double v = 0;
                for (int i = -radius; i <= radius; i++) v += kx[size_t(i + radius)] * src[size_t(y) * w + size_t(std::clamp(x + i, 0, w - 1))];
                tmp[size_t(y) * w + size_t(x)] = float(v);
            }
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                double v = 0;
                for (int i = -radius; i <= radius; i++) v += ky[size_t(i + radius)] * tmp[size_t(std::clamp(y + i, 0, h - 1)) * w + size_t(x)];
                out[size_t(y) * w + size_t(x)] = float(v);
            }
        return out;
    };
    std::vector<float> dx = conv(a, dg, g), dy = conv(a, g, dg), out(a.size());
    for (size_t i = 0; i < out.size(); i++) out[i] = std::sqrt(dx[i] * dx[i] + dy[i] * dy[i]);
    return out;
}

/// The connectivity error's per-pixel phi: alpha minus the highest threshold at which the pixel joined the
/// largest connected region shared by both maps, penalised past 0.15.
std::vector<float> connectivityPhi(const std::vector<float>& pred, const std::vector<float>& truth, int w, int h, bool forPred) {
    const size_t n = size_t(w) * h;
    std::vector<float> level(n, -1.0f);   // the threshold at which each pixel left omega; -1 = still inside
    std::vector<uint8_t> inside(n), visited(n), largest(n);
    std::vector<int32_t> stack;
    float previous = 0;
    for (int step = 1; step <= 10; step++) {
        const float t = step / 10.0f;
        // The largest 4-connected component of (pred >= t) & (truth >= t).
        for (size_t i = 0; i < n; i++) { inside[i] = pred[i] >= t && truth[i] >= t; visited[i] = 0; largest[i] = 0; }
        int bestSize = 0;
        std::vector<int32_t> best;
        for (size_t s = 0; s < n; s++) {
            if (!inside[s] || visited[s]) continue;
            std::vector<int32_t> component;
            stack.assign(1, int32_t(s));
            visited[s] = 1;
            while (!stack.empty()) {
                int32_t i = stack.back(); stack.pop_back();
                component.push_back(i);
                int x = i % w, y = i / w;
                const int32_t around[4] = {x > 0 ? i - 1 : -1, x < w - 1 ? i + 1 : -1, y > 0 ? i - w : -1, y < h - 1 ? i + w : -1};
                for (int32_t q : around) if (q >= 0 && inside[q] && !visited[q]) { visited[q] = 1; stack.push_back(q); }
            }
            if (int(component.size()) > bestSize) { bestSize = int(component.size()); best.swap(component); }
        }
        for (int32_t i : best) largest[size_t(i)] = 1;
        for (size_t i = 0; i < n; i++) if (level[i] < 0 && !largest[i]) level[i] = previous;
        previous = t;
    }
    for (size_t i = 0; i < n; i++) if (level[i] < 0) level[i] = 1;
    std::vector<float> phi(n);
    const std::vector<float>& a = forPred ? pred : truth;
    for (size_t i = 0; i < n; i++) { float d = a[i] - level[i]; phi[i] = 1 - (d >= 0.15f ? d : 0); }
    return phi;
}

Scores score(const GrayImage& pred, const GrayImage& truth) {
    const int w = pred.width(), h = pred.height();
    std::vector<float> p = levelsOf(pred), g = levelsOf(truth);
    Scores s;
    double sad = 0, sq = 0;
    for (size_t i = 0; i < p.size(); i++) { double d = std::fabs(p[i] - g[i]); sad += d; sq += d * d; }
    s.sad = sad / 1000;
    s.mse = sq / double(p.size());
    s.mad = sad / double(p.size());
    std::vector<float> gp = gradientMagnitude(p, w, h), gg = gradientMagnitude(g, w, h);
    double grad = 0;
    for (size_t i = 0; i < gp.size(); i++) grad += double(gp[i] - gg[i]) * double(gp[i] - gg[i]);
    s.grad = grad / 1000;
    std::vector<float> phiP = connectivityPhi(p, g, w, h, true), phiG = connectivityPhi(p, g, w, h, false);
    double conn = 0;
    for (size_t i = 0; i < phiP.size(); i++) conn += std::fabs(phiP[i] - phiG[i]);
    s.conn = conn / 1000;
    s.count = 1;
    return s;
}

int runMode(int argc, char** argv) {
    if (argc < 5) { std::fprintf(stderr, "usage: matte_tool run <image.png> <model.onnx|none> <outdir> [options]\n"); return 2; }
    const std::string imagePath = argv[2], model = argv[3], outDir = argv[4];
    Options o = parse(argc, argv, 5);
    std::string error;
    auto image = readPngImage(imagePath, &error);
    if (!image) { std::fprintf(stderr, "%s: %s\n", imagePath.c_str(), error.c_str()); return 1; }
    fs::create_directories(outDir);
    auto out = [&](const std::string& name) { return (fs::path(outDir) / name).string(); };
    if (o.detail > 0 && model != "none") {
        if (auto coarse = subjectMask(*image, model, &error)) writePngGray(out("coarse.png"), *coarse);
    }
    auto mask = maskFor(*image, model, o.mask, o.detail, o.flip, o.prompts, &error);
    if (!mask) { std::fprintf(stderr, "mask: %s\n", error.c_str()); return 1; }
    writePngGray(out("mask.png"), *mask);
    MatteSettings s = o.settings.normalized();
    std::shared_ptr<GrayImage> matte = o.refine ? refineMatte(*mask, *image, s, 0) : mask;
    writePngGray(out("matte.png"), *matte);
    if (o.refine && s.matting > 0) {
        MatteDebug debug;
        auto refined = s.refineEdges > 0 ? guidedRefine(*mask, *image, s.refineEdges, 0) : mask;
        auto band = matteBand(*refined, *image, s.matting, 0, mask.get(), &debug, s.narrowBand);
        writePngGray(out("band.png"), *band);
        if (debug.trimap) { writePngGray(out("trimap.png"), *debug.trimap); writePngGray(out("pairAlpha.png"), *debug.pairAlpha); writePngImage(out("chosenF.png"), *debug.chosenF); writePngImage(out("chosenB.png"), *debug.chosenB); }
    }
    auto pixels = (o.refine && s.decontaminate) ? estimateForeground(*image, *matte) : std::make_shared<Image>(*image);
    // The cutout: the pixels with the matte as alpha, and a composite over green.
    auto cutout = std::make_shared<Image>(*pixels), composite = std::make_shared<Image>(*pixels);
    for (int y = 0; y < image->height(); y++)
        for (int x = 0; x < image->width(); x++) {
            const unsigned k = matte->at(x, y);
            uint8_t* c = cutout->pixel(x, y);
            for (int i = 0; i < 4; i++) c[i] = uint8_t((c[i] * k + 127) / 255);
            const uint8_t green[3] = {34, 170, 68};
            uint8_t* m = composite->pixel(x, y);
            for (int i = 0; i < 3; i++) m[i] = uint8_t(c[i] + (green[i] * (255 - c[3]) + 127) / 255);
            m[3] = 255;
        }
    writePngImage(out("cutout.png"), *cutout);
    writePngImage(out("composite.png"), *composite);
    std::printf("wrote %s\n", outDir.c_str());
    return 0;
}

int evalMode(int argc, char** argv) {
    if (argc < 4) { std::fprintf(stderr, "usage: matte_tool eval <dir> <model.onnx|none> [options]\n"); return 2; }
    const std::string dir = argv[2], model = argv[3];
    Options o = parse(argc, argv, 4);
    // AIM-500's category file: {"o_...": {"category": "animal", "type": "SO"}, ...}, read without a JSON library.
    std::map<std::string, std::string> categoryOf, typeOf;
    if (!o.categories.empty()) {
        std::ifstream in(o.categories);
        std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        size_t pos = 0;
        while ((pos = text.find("\"o_", pos)) != std::string::npos) {
            const size_t end = text.find('"', pos + 1);
            const std::string name = text.substr(pos + 1, end - pos - 1);
            auto field = [&](const char* key) {
                const size_t k = text.find(std::string("\"") + key + "\"", end);
                if (k == std::string::npos) return std::string();
                const size_t q = text.find('"', text.find(':', k) + 1);
                return text.substr(q + 1, text.find('"', q + 1) - q - 1);
            };
            categoryOf[name] = field("category");
            typeOf[name] = field("type");
            pos = end;
        }
    }
    std::map<std::string, std::pair<Scores, Scores>> byCategory, byType;
    Scores rawTotal, refinedTotal;
    std::vector<fs::path> files;
    for (const auto& entry : fs::directory_iterator(dir)) {
        const std::string name = entry.path().filename().string();
        if (entry.path().extension() != ".png" || name.find(o.suffix + ".png") != std::string::npos || name.find(o.maskSuffix + ".png") != std::string::npos) continue;
        files.push_back(entry.path());
    }
    std::sort(files.begin(), files.end());
    if (o.limit > 0 && int(files.size()) > o.limit) files.resize(size_t(o.limit));
    std::printf("%-28s %8s %8s %8s %8s %8s   %8s %8s %8s %8s %8s\n", "image", "rawSAD", "rawMSE", "rawGrad", "rawConn", "rawMAD", "SAD", "MSE", "Grad", "Conn", "MAD");
    for (const fs::path& file : files) {
        const std::string stem = file.stem().string();
        const std::string truthPath = (file.parent_path() / (stem + o.suffix + ".png")).string();
        if (!fs::exists(truthPath)) continue;
        std::string error;
        auto image = readPngImage(file.string(), &error);
        auto truth = readAlpha(truthPath, &error);
        if (!image || !truth || truth->width() != image->width() || truth->height() != image->height()) { std::fprintf(stderr, "%s: %s\n", stem.c_str(), error.empty() ? "size mismatch" : error.c_str()); continue; }
        const std::string maskPath = (file.parent_path() / (stem + o.maskSuffix + ".png")).string();
        auto mask = maskFor(*image, model, model == "none" ? maskPath : o.mask, o.detail, o.flip, o.prompts, &error);
        if (!mask) { std::fprintf(stderr, "%s: %s\n", stem.c_str(), error.c_str()); continue; }
        auto matte = refineMatte(*mask, *image, o.settings, 0);
        Scores raw = score(*mask, *truth), refined = score(*matte, *truth);
        std::printf("%-28s %8.2f %8.5f %8.2f %8.2f %8.4f   %8.2f %8.5f %8.2f %8.2f %8.4f\n", stem.c_str(), raw.sad, raw.mse, raw.grad, raw.conn, raw.mad, refined.sad, refined.mse, refined.grad, refined.conn, refined.mad);
        auto add = [](Scores& t, const Scores& s) { t.sad += s.sad; t.mse += s.mse; t.mad += s.mad; t.grad += s.grad; t.conn += s.conn; t.count++; };
        add(rawTotal, raw); add(refinedTotal, refined);
        if (categoryOf.count(stem)) { add(byCategory[categoryOf[stem]].first, raw); add(byCategory[categoryOf[stem]].second, refined); add(byType[typeOf[stem]].first, raw); add(byType[typeOf[stem]].second, refined); }
        std::fflush(stdout);
    }
    auto meanRow = [](const std::string& label, const Scores& r, const Scores& f) {
        const double n = std::max(1, r.count);
        std::printf("%-28s %8.2f %8.5f %8.2f %8.2f %8.4f   %8.2f %8.5f %8.2f %8.2f %8.4f\n", (label + " (" + std::to_string(r.count) + ")").c_str(), r.sad / n, r.mse / n, r.grad / n, r.conn / n, r.mad / n, f.sad / n, f.mse / n, f.grad / n, f.conn / n, f.mad / n);
    };
    if (rawTotal.count) {
        for (const auto& [name, scores] : byCategory) meanRow("  " + name, scores.first, scores.second);
        for (const auto& [name, scores] : byType) meanRow("  type " + name, scores.first, scores.second);
        meanRow("mean", rawTotal, refinedTotal);
    } else std::printf("no <name>.png with <name>%s.png pairs in %s\n", o.suffix.c_str(), dir.c_str());
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc >= 2 && std::strcmp(argv[1], "run") == 0) return runMode(argc, argv);
    if (argc >= 2 && std::strcmp(argv[1], "eval") == 0) return evalMode(argc, argv);
    std::fprintf(stderr, "usage: matte_tool run <image.png> <model.onnx|none> <outdir> [options]\n       matte_tool eval <dir> <model.onnx|none> [options]\n");
    return 2;
}
