// The background-removal harness: scores mattes against image/alpha pairs.
//
//   matte_tool run <image.png> <model.onnx|none> <outdir> [--refine R] [--band B] [--contrast C] [--shift S]
//                  [--no-cleanup] [--no-decontaminate] [--raw] [--detail N] [--mask <mask.png>]
//                  [--prompt x,y[,label] ...]   (a click-to-select model: label 1 subject, 0 not, 2/3 box corners)
//       runs the pipeline and writes every stage: mask.png (the model's), matte.png (after the panel),
//       trimap.png, chosenF.png, chosenB.png, pairAlpha.png (what the band saw and chose), uncertainty.png and
//       uncertainty-overlay.png (the band's residual, bright where the two-colour model explains a pixel badly),
//       cutout.png (the layer with its new alpha and edge colours), composite.png (over green).
//   matte_tool eval <dir> <model.onnx|none> [the same options] [--suffix _alpha] [--limit N]
//                   [--categories <aim_category_type.json>] [--mask-cache <dir>] [--jobs N]
//       scores every <name>.png with a <name><suffix>.png ground-truth alpha (8-bit grey): SAD (/1000),
//       MSE, MAD, Grad (first Gaussian derivative, sigma 1.4, /1000) and Conn (/1000), as GFM's evaluate.py
//       defines them, for the raw mask and for the refined matte; with AIM-500's category file, means per
//       category and per type as well. With "none" as the model, --mask names a mask file (run) or a mask
//       suffix (eval, default _mask) to start from instead of the model. --mask-cache keeps the model's masks
//       (and reads them back on the next run). Where a <name>_fg.png true foreground exists (synthetic sets), the
//       estimated edge colours are scored too: FG, their mean error where the truth is soft, and Comp, the
//       error of the cutout composited over white and black in linear light.
//   Solve space: --linear decodes sRGB to linear light before matting and foreground estimation; --gamma (the
//   default) solves on the stored values. --fg-gamma and --fg-linear set the foreground estimation's space alone.
//   matte_tool synth <outdir> [--size N]
//       writes synthetic composites made in linear light from known F, B and alpha (dark hair on bright, white
//       fur on dark, saturated pairs, soft gradients, antialiased shapes, a translucent veil): <name>.png,
//       <name>_alpha.png, <name>_fg.png and a coarse <name>_mask.png standing in for a model's.
//   matte_tool residual <dir> <model.onnx|none> [the eval options] [--tile N]
//       how well the band's uncertainty predicts where the matte is wrong: per-pixel Pearson and Spearman
//       correlation with |alpha - truth| in the band, the AUC of finding errors over 0.1, and per tile of N pixels
//       (default 128) the correlation of the tile's summed uncertainty with its refined and raw SAD, beside the
//       count of the coarse mask's soft pixels (what ranks the detail pass's windows today).
#include "compositor/blur.h"
#include "compositor/png.h"
#include "compositor/matte.h"
#include "compositor/scribble.h"
#include "compositor/subject.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <iterator>
#include <map>
#include <fstream>
#include <string>
#include <sstream>
#include <mutex>
#include <random>
#include <thread>
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
    std::string maskCache;   // eval: the model's masks are kept here and read back
    int jobs = 1;            // eval: images scored side by side
    int tile = 128;          // residual: the tile size
    int size = 480;          // synth: the scenes' size
    int foregroundSpace = 0; // eval: -1 foreground estimation on stored values, 1 in linear light, 0 as the matting
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
        else if (a == "--gamma") o.settings.decode = MatteTransfer::identity();
        else if (a == "--linear") o.settings.decode = MatteTransfer::srgb();
        else if (a == "--mask-cache") o.maskCache = next();
        else if (a == "--fg-gamma") o.foregroundSpace = -1;
        else if (a == "--fg-linear") o.foregroundSpace = 1;
        else if (a == "--jobs") o.jobs = std::max(1, std::stoi(next()));
        else if (a == "--tile") o.tile = std::max(8, std::stoi(next()));
        else if (a == "--size") o.size = std::max(64, std::stoi(next()));
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

std::shared_ptr<GrayImage> maskFor(const Image& image, const std::string& model, const std::string& maskPath, int detail, bool flip, const std::vector<PointPrompt>& prompts, std::string* error, const std::string& cachePath = {}) {
    if (model != "none") {
        if (!subjectModelSupported()) { *error = "this build has no OpenCV"; return nullptr; }
        if (!prompts.empty()) return subjectFromPrompts(image, model, prompts, error);
        // The whole-image pass is cached; the detail pass starts from the cached coarse mask.
        std::shared_ptr<GrayImage> coarse;
        if (!cachePath.empty() && fs::exists(cachePath)) coarse = readPngGray(cachePath, nullptr);
        if (coarse && (coarse->width() != image.width() || coarse->height() != image.height())) coarse.reset();
        if (!coarse) {
            coarse = subjectMask(image, model, error, flip);
            if (coarse && !cachePath.empty()) writePngGray(cachePath, *coarse);
        }
        if (!coarse || detail <= 0) return coarse;
        return subjectMaskDetailed(image, model, coarse.get(), detail, error);
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
    MatteDebug debug;
    const AlphaPlane plane = o.refine ? refineMatte(AlphaPlane(*mask), *image, s, 0, &debug) : AlphaPlane(*mask);
    std::shared_ptr<GrayImage> matte = plane.toGray();
    writePngGray(out("matte.png"), *matte);
    if (debug.trimap) {
        writePngGray(out("trimap.png"), *debug.trimap); writePngGray(out("pairAlpha.png"), *debug.pairAlpha); writePngImage(out("chosenF.png"), *debug.chosenF); writePngImage(out("chosenB.png"), *debug.chosenB);
        // The uncertainty: grey, 1 and above white; and over the image, dimmed, from dark red to yellow.
        GrayImage heat(image->width(), image->height(), 0);
        Image overlay(image->width(), image->height());
        for (int y = 0; y < image->height(); y++)
            for (int x = 0; x < image->width(); x++) {
                const float u = std::clamp(debug.uncertainty.at(x, y), 0.0f, 1.0f);
                heat.at(x, y) = uint8_t(std::lround(u * 255));
                const uint8_t* p = image->pixel(x, y);
                uint8_t* q = overlay.pixel(x, y);
                const float grey = (0.299f * p[0] + 0.587f * p[1] + 0.114f * p[2]) * 0.35f;
                const float t = std::sqrt(u);   // small residuals visible too
                q[0] = uint8_t(std::lround(grey * (1 - t) + 255 * t));
                q[1] = uint8_t(std::lround(grey * (1 - t) + 255 * std::max(0.0f, t * 2 - 1) * t));
                q[2] = uint8_t(std::lround(grey * (1 - t)));
                q[3] = 255;
            }
        writePngGray(out("uncertainty.png"), heat);
        writePngImage(out("uncertainty-overlay.png"), overlay);
    }
    auto pixels = (o.refine && s.decontaminate) ? estimateForeground(*image, plane, s.decode) : std::make_shared<Image>(*image);
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

/// The image files of a folder that have a ground truth beside them, sorted; at most `limit` (0 = all).
std::vector<fs::path> pairFiles(const std::string& dir, const Options& o) {
    std::vector<fs::path> files;
    for (const auto& entry : fs::directory_iterator(dir)) {
        const std::string name = entry.path().filename().string();
        if (entry.path().extension() != ".png") continue;
        bool skip = false;
        for (const std::string& suffix : {o.suffix, o.maskSuffix, std::string("_fg")}) skip = skip || (name.size() > suffix.size() + 4 && name.compare(name.size() - suffix.size() - 4, suffix.size(), suffix) == 0);
        if (skip || !fs::exists(entry.path().parent_path() / (entry.path().stem().string() + o.suffix + ".png"))) continue;
        files.push_back(entry.path());
    }
    std::sort(files.begin(), files.end());
    if (o.limit > 0 && int(files.size()) > o.limit) files.resize(size_t(o.limit));
    return files;
}

/// Runs `body(index)` for every index on `jobs` threads.
template <class Body>
void forEachJob(size_t count, int jobs, Body body) {
    std::atomic<size_t> next{0};
    std::vector<std::thread> workers;
    for (int t = 0; t < jobs; t++)
        workers.emplace_back([&] { for (size_t i; (i = next++) < count;) body(i); });
    for (auto& w : workers) w.join();
}

/// One image of an eval set: the image, its truth, and the mask to start from.
struct Loaded { std::shared_ptr<Image> image; std::shared_ptr<GrayImage> truth, mask; std::string stem; };

bool loadPair(const fs::path& file, const std::string& model, const Options& o, Loaded& out) {
    out.stem = file.stem().string();
    std::string error;
    out.image = readPngImage(file.string(), &error);
    out.truth = readAlpha((file.parent_path() / (out.stem + o.suffix + ".png")).string(), &error);
    if (!out.image || !out.truth || out.truth->width() != out.image->width() || out.truth->height() != out.image->height()) { std::fprintf(stderr, "%s: %s\n", out.stem.c_str(), error.empty() ? "size mismatch" : error.c_str()); return false; }
    const std::string maskPath = (file.parent_path() / (out.stem + o.maskSuffix + ".png")).string();
    const std::string cache = o.maskCache.empty() || model == "none" ? std::string() : (fs::path(o.maskCache) / (out.stem + ".png")).string();
    out.mask = maskFor(*out.image, model, model == "none" ? maskPath : o.mask, o.detail, o.flip, o.prompts, &error, cache);
    if (!out.mask) { std::fprintf(stderr, "%s: %s\n", out.stem.c_str(), error.c_str()); return false; }
    return true;
}

/// The estimated edge colours against the true foreground (straight sRGB, opaque): FG, the mean channel error
/// where both the true and the refined alpha are soft (an opaque pixel keeps its colour: its error is the alpha's),
/// weighted by how much of the pixel the refined matte shows; Comp, the cutout (estimated colours, refined alpha) against the true one (true
/// colours, true alpha), each composited over white and over black as the app composites (on the stored
/// values), mean channel error wherever either alpha is soft.
struct ForegroundScore { double fg = 0, comp = 0; };

ForegroundScore scoreForeground(const Image& estimated, const GrayImage& matte, const Image& trueF, const GrayImage& truth) {
    double fg = 0, comp = 0, wf = 0;
    long nc = 0;
    for (int y = 0; y < truth.height(); y++)
        for (int x = 0; x < truth.width(); x++) {
            const int at = truth.at(x, y), ap = matte.at(x, y);
            const uint8_t *e = estimated.pixel(x, y), *t = trueF.pixel(x, y);
            if (at > 5 && at < 250 && ap > 5 && ap < 250) {   // where colour was estimated, weighted by how much of it shows
                const double weight = std::min(at, ap) / 255.0;
                for (int c = 0; c < 3; c++) fg += weight * std::fabs(e[c] - t[c]) / 255.0;
                wf += 3 * weight;
            }
            if ((at > 0 && at < 255) || (ap > 0 && ap < 255)) {
                for (double bg : {0.0, 255.0})
                    for (int c = 0; c < 3; c++) {
                        const double ce = (ap * e[c] + (255 - ap) * bg) / 255.0, ct = (at * t[c] + (255 - at) * bg) / 255.0;
                        comp += std::fabs(ce - ct) / 255.0;
                    }
                nc += 6;
            }
        }
    return {wf > 0 ? fg / wf : 0, nc ? comp / double(nc) : 0};
}

std::map<std::string, std::pair<std::string, std::string>> readCategories(const std::string& path) {
    // AIM-500's category file: {"o_...": {"category": "animal", "type": "SO"}, ...}, read without a JSON library.
    std::map<std::string, std::pair<std::string, std::string>> out;
    if (path.empty()) return out;
    std::ifstream in(path);
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
        out[name] = {field("category"), field("type")};
        pos = end;
    }
    return out;
}

int evalMode(int argc, char** argv) {
    if (argc < 4) { std::fprintf(stderr, "usage: matte_tool eval <dir> <model.onnx|none> [options]\n"); return 2; }
    const std::string dir = argv[2], model = argv[3];
    Options o = parse(argc, argv, 4);
    if (!o.maskCache.empty()) fs::create_directories(o.maskCache);
    const auto categories = readCategories(o.categories);
    const std::vector<fs::path> files = pairFiles(dir, o);
    struct Row { bool ok = false; std::string stem; Scores raw, refined; ForegroundScore fore; bool haveFore = false; double ms = 0; };
    std::vector<Row> rows(files.size());
    std::mutex print;
    forEachJob(files.size(), o.jobs, [&](size_t k) {
        Loaded in;
        if (!loadPair(files[k], model, o, in)) return;
        Row& row = rows[k];
        row.stem = in.stem;
        const auto t0 = std::chrono::steady_clock::now();
        const AlphaPlane plane = refineMatte(AlphaPlane(*in.mask), *in.image, o.settings, 0);
        row.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        auto matte = plane.toGray();
        row.raw = score(*in.mask, *in.truth);
        row.refined = score(*matte, *in.truth);
        const fs::path fgPath = files[k].parent_path() / (in.stem + "_fg.png");
        if (fs::exists(fgPath)) {
            if (auto trueF = readPngImage(fgPath.string())) {
                const MatteTransfer space = o.foregroundSpace < 0 ? MatteTransfer::identity() : o.foregroundSpace > 0 ? MatteTransfer::srgb() : o.settings.normalized().decode;
                auto estimated = o.settings.decontaminate ? estimateForeground(*in.image, plane, space) : std::make_shared<Image>(*in.image);
                row.fore = scoreForeground(*estimated, *matte, *trueF, *in.truth);
                row.haveFore = true;
            }
        }
        row.ok = true;
        std::lock_guard<std::mutex> lock(print);
        std::fprintf(stderr, "\r%zu / %zu", k + 1, files.size());
    });
    std::fprintf(stderr, "\n");
    const bool anyFore = std::any_of(rows.begin(), rows.end(), [](const Row& r) { return r.haveFore; });
    std::printf("%-28s %8s %8s %8s %8s %8s   %8s %8s %8s %8s %8s%s\n", "image", "rawSAD", "rawMSE", "rawGrad", "rawConn", "rawMAD", "SAD", "MSE", "Grad", "Conn", "MAD", anyFore ? "       FG     Comp" : "");
    struct Total { Scores raw, refined; ForegroundScore fore; int foreCount = 0; };
    std::map<std::string, Total> byCategory, byType;
    Total total;
    double ms = 0;
    auto add = [](Total& t, const Row& r) {
        auto sum = [](Scores& a, const Scores& s) { a.sad += s.sad; a.mse += s.mse; a.mad += s.mad; a.grad += s.grad; a.conn += s.conn; a.count++; };
        sum(t.raw, r.raw); sum(t.refined, r.refined);
        if (r.haveFore) { t.fore.fg += r.fore.fg; t.fore.comp += r.fore.comp; t.foreCount++; }
    };
    for (const Row& r : rows) {
        if (!r.ok) continue;
        std::printf("%-28s %8.2f %8.5f %8.2f %8.2f %8.4f   %8.2f %8.5f %8.2f %8.2f %8.4f", r.stem.c_str(), r.raw.sad, r.raw.mse, r.raw.grad, r.raw.conn, r.raw.mad, r.refined.sad, r.refined.mse, r.refined.grad, r.refined.conn, r.refined.mad);
        if (r.haveFore) std::printf(" %8.4f %8.4f", r.fore.fg, r.fore.comp);
        std::printf("\n");
        add(total, r);
        ms += r.ms;
        auto c = categories.find(r.stem);
        if (c != categories.end()) { add(byCategory[c->second.first], r); add(byType[c->second.second], r); }
    }
    auto meanRow = [&](const std::string& label, const Total& t) {
        const double n = std::max(1, t.raw.count);
        std::printf("%-28s %8.2f %8.5f %8.2f %8.2f %8.4f   %8.2f %8.5f %8.2f %8.2f %8.4f", (label + " (" + std::to_string(t.raw.count) + ")").c_str(), t.raw.sad / n, t.raw.mse / n, t.raw.grad / n, t.raw.conn / n, t.raw.mad / n,
                    t.refined.sad / n, t.refined.mse / n, t.refined.grad / n, t.refined.conn / n, t.refined.mad / n);
        if (t.foreCount) std::printf(" %8.4f %8.4f", t.fore.fg / t.foreCount, t.fore.comp / t.foreCount);
        std::printf("\n");
    };
    if (total.raw.count) {
        for (const auto& [name, t] : byCategory) meanRow("  " + name, t);
        for (const auto& [name, t] : byType) meanRow("  type " + name, t);
        meanRow("mean", total);
        std::printf("refinement: %.0f ms per image on average (%d jobs)\n", ms / total.raw.count, o.jobs);
    } else std::printf("no <name>.png with <name>%s.png pairs in %s\n", o.suffix.c_str(), dir.c_str());
    return 0;
}

// ---- Synthetic composites in linear light --------------------------------------------------------------

float srgbEncode(float v) { return fromLinear(MatteTransfer::srgb(), v); }

int synthMode(int argc, char** argv) {
    if (argc < 3) { std::fprintf(stderr, "usage: matte_tool synth <outdir> [--size N]\n"); return 2; }
    const std::string outDir = argv[2];
    Options o = parse(argc, argv, 3);
    fs::create_directories(outDir);
    const int N = o.size, S = 4;   // S x S supersamples per pixel
    struct Colour { float r, g, b; };
    // A colour with smooth variation: `base` plus up to `amount` of three low-frequency waves (linear light).
    auto field = [](Colour base, float amount, unsigned seed) {
        std::mt19937 rng(seed);
        std::uniform_real_distribution<float> u(0, 6.2832f);
        const float p[6] = {u(rng), u(rng), u(rng), u(rng), u(rng), u(rng)};
        return [=](float x, float y) {
            const float w0 = std::sin(x / 37 + p[0]) * std::cos(y / 53 + p[1]), w1 = std::sin((x + y) / 29 + p[2]), w2 = std::cos(x / 71 - y / 43 + p[3]);
            auto mix = [&](float v, float a, float b) { return std::clamp(v * (1 + amount * (a * 0.6f + b * 0.4f)), 0.0f, 1.0f); };
            return Colour{mix(base.r, w0, w1), mix(base.g, w1, w2), mix(base.b, w2, w0)};
        };
    };
    // Coverage from a supersampled canvas.
    struct Canvas {
        int n, s;
        std::vector<uint8_t> hit;
        Canvas(int n_, int s_) : n(n_), s(s_), hit(size_t(n_ * s_) * size_t(n_ * s_), 0) {}
        void disc(float cx, float cy, float r) {   // pixel units
            const int x0 = std::max(0, int((cx - r) * s)), x1 = std::min(n * s - 1, int((cx + r) * s) + 1);
            const int y0 = std::max(0, int((cy - r) * s)), y1 = std::min(n * s - 1, int((cy + r) * s) + 1);
            for (int y = y0; y <= y1; y++)
                for (int x = x0; x <= x1; x++) {
                    const float dx = (x + 0.5f) / s - cx, dy = (y + 0.5f) / s - cy;
                    if (dx * dx + dy * dy <= r * r) hit[size_t(y) * size_t(n * s) + size_t(x)] = 1;
                }
        }
        void fill(const std::function<bool(float, float)>& inside) {
            for (int y = 0; y < n * s; y++) for (int x = 0; x < n * s; x++) if (inside((x + 0.5f) / s, (y + 0.5f) / s)) hit[size_t(y) * size_t(n * s) + size_t(x)] = 1;
        }
        std::vector<float> coverage() const {
            std::vector<float> a(size_t(n) * n, 0);
            for (int y = 0; y < n * s; y++) for (int x = 0; x < n * s; x++) a[size_t(y / s) * n + size_t(x / s)] += hit[size_t(y) * size_t(n * s) + size_t(x)];
            for (float& v : a) v /= float(s * s);
            return a;
        }
    };
    // A body with strands running out of it: `count` curved strands `width` pixels wide.
    auto hairy = [&](unsigned seed, int count, float minWidth, float maxWidth) {
        Canvas c(N, S);
        const float cx = N * 0.42f, cy = N * 0.55f, r = N * 0.26f;
        c.disc(cx, cy, r);
        std::mt19937 rng(seed);
        std::uniform_real_distribution<float> u(0, 1);
        for (int k = 0; k < count; k++) {
            const float angle = -2.2f + 2.6f * u(rng), width = minWidth + (maxWidth - minWidth) * u(rng) * u(rng);
            const float length = N * (0.12f + 0.3f * u(rng)), bend = (u(rng) - 0.5f) * 1.6f;
            float x = cx + (r - 3) * std::cos(angle), y = cy + (r - 3) * std::sin(angle), a = angle;
            for (float t = 0; t < length; t += 0.2f) {
                a += bend * 0.2f / length;
                x += 0.2f * std::cos(a); y += 0.2f * std::sin(a);
                c.disc(x, y, width / 2);
            }
        }
        return c.coverage();
    };
    struct Scene { std::string name; std::vector<float> alpha; std::function<Colour(float, float)> f, b; };
    std::vector<Scene> scenes;
    scenes.push_back({"dark-hair-on-bright", hairy(1, 90, 0.5f, 2.5f), field({0.035f, 0.022f, 0.015f}, 0.4f, 11), field({0.62f, 0.72f, 0.85f}, 0.25f, 12)});
    scenes.push_back({"dark-hair-on-bright-2", hairy(2, 140, 0.4f, 1.8f), field({0.05f, 0.03f, 0.02f}, 0.5f, 13), field({0.8f, 0.75f, 0.6f}, 0.2f, 14)});
    scenes.push_back({"white-fur-on-dark", hairy(3, 120, 0.5f, 2.5f), field({0.85f, 0.82f, 0.78f}, 0.15f, 15), field({0.02f, 0.025f, 0.04f}, 0.5f, 16)});
    scenes.push_back({"white-fur-on-dark-2", hairy(4, 160, 0.4f, 1.8f), field({0.7f, 0.72f, 0.75f}, 0.2f, 17), field({0.05f, 0.03f, 0.02f}, 0.5f, 18)});
    scenes.push_back({"saturated-red-on-green", hairy(5, 90, 0.6f, 3.0f), field({0.75f, 0.03f, 0.02f}, 0.3f, 19), field({0.03f, 0.45f, 0.05f}, 0.3f, 20)});
    scenes.push_back({"saturated-blue-on-orange", hairy(6, 90, 0.6f, 3.0f), field({0.02f, 0.05f, 0.7f}, 0.3f, 21), field({0.8f, 0.3f, 0.02f}, 0.3f, 22)});
    {
        // Soft gradients: a disc whose edge fades over 30 pixels (defocus, motion), and a wide linear ramp.
        std::vector<float> a(size_t(N) * N), b(size_t(N) * N);
        for (int y = 0; y < N; y++) for (int x = 0; x < N; x++) {
            const float d = std::hypot(x + 0.5f - N * 0.5f, y + 0.5f - N * 0.5f);
            a[size_t(y) * N + size_t(x)] = std::clamp((N * 0.3f - d) / 30 + 0.5f, 0.0f, 1.0f);
            b[size_t(y) * N + size_t(x)] = std::clamp((N * 0.6f - (x + 0.5f)) / (N * 0.25f), 0.0f, 1.0f);
        }
        scenes.push_back({"soft-gradient-disc", a, field({0.3f, 0.2f, 0.5f}, 0.2f, 23), field({0.7f, 0.7f, 0.6f}, 0.2f, 24)});
        scenes.push_back({"soft-gradient-ramp", b, field({0.02f, 0.02f, 0.03f}, 0.3f, 25), field({0.9f, 0.85f, 0.7f}, 0.1f, 26)});
    }
    {
        // Antialiased hard shapes: a five-pointed star and a disc.
        Canvas c(N, S);
        const float cx = N * 0.4f, cy = N * 0.5f, R = N * 0.3f, r = N * 0.13f;
        c.fill([&](float x, float y) {
            const float dx = x - cx, dy = y - cy, d = std::hypot(dx, dy);
            const float a = std::atan2(dy, dx) + 1.5708f, sector = 1.2566f;
            const float t = std::fabs(std::fmod(a + 10 * sector, sector) - sector / 2) / (sector / 2);   // 0 at a point, 1 between
            return d < R * (1 - t) + r * t;
        });
        c.disc(N * 0.78f, N * 0.3f, N * 0.12f);
        const auto shapes = c.coverage();
        scenes.push_back({"aa-shapes", shapes, field({0.6f, 0.1f, 0.1f}, 0.1f, 27), field({0.1f, 0.2f, 0.6f}, 0.1f, 28)});
        scenes.push_back({"aa-shapes-dark-on-bright", shapes, field({0.01f, 0.012f, 0.015f}, 0.2f, 29), field({0.85f, 0.85f, 0.8f}, 0.1f, 30)});
    }
    {
        // A translucent veil: a smooth patch of opacity 0.2..0.7 beside an opaque body.
        std::vector<float> a(size_t(N) * N);
        for (int y = 0; y < N; y++) for (int x = 0; x < N; x++) {
            const float d = std::hypot(x + 0.5f - N * 0.35f, y + 0.5f - N * 0.5f);
            const float body = std::clamp(N * 0.2f - d + 0.5f, 0.0f, 1.0f);
            const float veil = (x > N * 0.45f && x < N * 0.85f && y > N * 0.25f && y < N * 0.75f) ? 0.45f + 0.25f * std::sin(x / 23.0f) * std::cos(y / 31.0f) : 0.0f;
            a[size_t(y) * N + size_t(x)] = std::max(body, veil);
        }
        scenes.push_back({"translucent-veil", a, field({0.8f, 0.8f, 0.85f}, 0.1f, 31), field({0.1f, 0.3f, 0.15f}, 0.4f, 32)});
    }
    for (const Scene& sc : scenes) {
        Image image(N, N), fore(N, N);
        GrayImage alpha(N, N);
        for (int y = 0; y < N; y++)
            for (int x = 0; x < N; x++) {
                const float a = sc.alpha[size_t(y) * N + size_t(x)];
                const Colour f = sc.f(x + 0.5f, y + 0.5f), b = sc.b(x + 0.5f, y + 0.5f);
                uint8_t* p = image.pixel(x, y);
                uint8_t* q = fore.pixel(x, y);
                const float lin[3] = {a * f.r + (1 - a) * b.r, a * f.g + (1 - a) * b.g, a * f.b + (1 - a) * b.b}, fl[3] = {f.r, f.g, f.b};
                for (int c = 0; c < 3; c++) { p[c] = uint8_t(std::lround(srgbEncode(lin[c]) * 255)); q[c] = uint8_t(std::lround(srgbEncode(fl[c]) * 255)); }
                p[3] = q[3] = 255;
                alpha.at(x, y) = uint8_t(std::lround(a * 255));
            }
        // The coarse mask a model might give: fine structure (under ~4 px) lost, the edge a little soft.
        GrayImage coarse = alpha;
        gaussianBlur(coarse, 3);
        for (size_t i = 0; i < coarse.byteCount(); i++) coarse.data()[i] = coarse.data()[i] >= 128 ? 255 : 0;
        gaussianBlur(coarse, 1.5);
        const std::string base = (fs::path(outDir) / sc.name).string();
        writePngImage(base + ".png", image);
        writePngGray(base + "_alpha.png", alpha);
        writePngImage(base + "_fg.png", fore);
        writePngGray(base + "_mask.png", coarse);
    }
    std::printf("wrote %zu scenes to %s\n", scenes.size(), outDir.c_str());
    return 0;
}

// ---- Does the band's uncertainty find the errors? -------------------------------------------------------

/// Pearson correlation of two samples.
double pearson(const std::vector<float>& a, const std::vector<float>& b) {
    const size_t n = a.size();
    if (n < 2) return 0;
    double ma = 0, mb = 0;
    for (size_t i = 0; i < n; i++) { ma += a[i]; mb += b[i]; }
    ma /= double(n); mb /= double(n);
    double sab = 0, saa = 0, sbb = 0;
    for (size_t i = 0; i < n; i++) { const double da = a[i] - ma, db = b[i] - mb; sab += da * db; saa += da * da; sbb += db * db; }
    return saa > 0 && sbb > 0 ? sab / std::sqrt(saa * sbb) : 0;
}

std::vector<float> ranks(const std::vector<float>& v) {
    std::vector<size_t> order(v.size());
    for (size_t i = 0; i < order.size(); i++) order[i] = i;
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return v[a] < v[b]; });
    std::vector<float> r(v.size());
    for (size_t i = 0; i < order.size();) {   // ties share their mean rank
        size_t j = i;
        while (j + 1 < order.size() && v[order[j + 1]] == v[order[i]]) j++;
        for (size_t k = i; k <= j; k++) r[order[k]] = float(i + j) / 2;
        i = j + 1;
    }
    return r;
}

double spearman(const std::vector<float>& a, const std::vector<float>& b) { return pearson(ranks(a), ranks(b)); }

/// The area under the ROC curve of `score` for telling positives (`label`) from negatives.
double auc(const std::vector<float>& score, const std::vector<uint8_t>& label) {
    const std::vector<float> r = ranks(score);
    double sum = 0, pos = 0;
    for (size_t i = 0; i < r.size(); i++) if (label[i]) { sum += r[i] + 1; pos++; }
    const double neg = double(r.size()) - pos;
    if (pos == 0 || neg == 0) return 0.5;
    return (sum - pos * (pos + 1) / 2) / (pos * neg);
}

int residualMode(int argc, char** argv) {
    if (argc < 4) { std::fprintf(stderr, "usage: matte_tool residual <dir> <model.onnx|none> [options]\n"); return 2; }
    const std::string dir = argv[2], model = argv[3];
    Options o = parse(argc, argv, 4);
    if (!o.maskCache.empty()) fs::create_directories(o.maskCache);
    if (o.settings.matting <= 0) { std::fprintf(stderr, "residual needs matting: pass --band\n"); return 2; }
    const std::vector<fs::path> files = pairFiles(dir, o);
    // Per pixel (one band pixel in 8, to keep the sample small): uncertainty, the refined alpha's softness (1 at
    // 0.5, 0 at 0 or 1), the coarse mask's softness, and the error.
    std::vector<float> pu, psoft, pcoarse, perr;
    // Per tile: summed uncertainty, the coarse mask's soft pixels (what ranks the detail windows today), a
    // boundary complexity (pixels where the refined alpha's gradient is steep), the raw and refined SAD.
    struct Tile { float u, soft, edges, rawSad, sad; };
    std::vector<std::vector<Tile>> tilesOf(files.size());
    std::mutex lock;
    forEachJob(files.size(), o.jobs, [&](size_t k) {
        Loaded in;
        if (!loadPair(files[k], model, o, in)) return;
        MatteDebug debug;
        const AlphaPlane plane = refineMatte(AlphaPlane(*in.mask), *in.image, o.settings, 0, &debug);
        if (!debug.trimap || debug.trimap->width() != plane.width) return;   // the band ran below full size
        const int w = plane.width, h = plane.height, T = o.tile;
        std::vector<float> u, soft, coarse, err;
        const int tw = (w + T - 1) / T, th = (h + T - 1) / T;
        std::vector<Tile> tiles(size_t(tw) * size_t(th), Tile{0, 0, 0, 0, 0});
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                const float a = plane.at(x, y), t = in.truth->at(x, y) / 255.0f, m = in.mask->at(x, y) / 255.0f;
                Tile& tile = tiles[size_t(y / T) * size_t(tw) + size_t(x / T)];
                tile.rawSad += std::fabs(m - t);
                tile.sad += std::fabs(a - t);
                if (in.mask->at(x, y) > 25 && in.mask->at(x, y) < 230) tile.soft += 1;
                if (x + 1 < w && y + 1 < h && std::fabs(plane.at(x + 1, y) - a) + std::fabs(plane.at(x, y + 1) - a) > 0.1f) tile.edges += 1;
                if (debug.trimap->at(x, y) != 128) continue;
                const float uncertainty = debug.uncertainty.at(x, y);
                tile.u += uncertainty;
                if ((x + y * 3) % 8) continue;
                u.push_back(uncertainty); soft.push_back(1 - std::fabs(2 * a - 1)); coarse.push_back(1 - std::fabs(2 * m - 1)); err.push_back(std::fabs(a - t));
            }
        std::vector<Tile> kept;
        for (const Tile& tile : tiles) if (tile.u > 0 || tile.soft > 0) kept.push_back(tile);
        std::lock_guard<std::mutex> guard(lock);
        tilesOf[k] = std::move(kept);
        pu.insert(pu.end(), u.begin(), u.end()); psoft.insert(psoft.end(), soft.begin(), soft.end()); pcoarse.insert(pcoarse.end(), coarse.begin(), coarse.end()); perr.insert(perr.end(), err.begin(), err.end());
        std::fprintf(stderr, "\r%zu / %zu", k + 1, files.size());
    });
    std::fprintf(stderr, "\n");
    std::vector<uint8_t> wrong(perr.size());
    for (size_t i = 0; i < perr.size(); i++) wrong[i] = perr[i] > 0.1f;
    std::vector<float> both(pu.size());
    for (size_t i = 0; i < pu.size(); i++) both[i] = pu[i] + psoft[i];
    std::printf("band pixels sampled: %zu, with error over 0.1: %.1f%%\n", perr.size(), 100.0 * double(std::count(wrong.begin(), wrong.end(), 1)) / double(std::max<size_t>(1, wrong.size())));
    std::printf("%-34s %9s %9s %9s\n", "per pixel, against |alpha - truth|", "Pearson", "Spearman", "AUC>0.1");
    auto line = [&](const char* name, const std::vector<float>& v) { std::printf("%-34s %9.3f %9.3f %9.3f\n", name, pearson(v, perr), spearman(v, perr), auc(v, wrong)); };
    line("uncertainty", pu);
    line("refined alpha softness", psoft);
    line("coarse mask softness", pcoarse);
    line("uncertainty + softness", both);
    // Tiles, pooled over the images.
    std::vector<float> tu, tsoft, tedges, traw, tsad, tcombo;
    for (const auto& list : tilesOf)
        for (const Tile& t : list) { tu.push_back(t.u); tsoft.push_back(t.soft); tedges.push_back(t.edges); traw.push_back(t.rawSad); tsad.push_back(t.sad); }
    // The detail pass's ranking with the uncertainty added: each signal scaled by its pooled mean so that they weigh alike.
    auto meanOf = [](const std::vector<float>& v) { double s = 0; for (float x : v) s += x; return v.empty() ? 1.0 : std::max(1e-9, s / double(v.size())); };
    const double mu = meanOf(tu), ms = meanOf(tsoft), me = meanOf(tedges);
    for (size_t i = 0; i < tu.size(); i++) tcombo.push_back(float(tsoft[i] / ms + tu[i] / mu + tedges[i] / me));
    std::printf("\n%zu tiles of %d px with band or soft pixels\n%-34s %9s %9s %9s %9s\n", tu.size(), o.tile, "per tile", "raw P", "raw S", "SAD P", "SAD S");
    auto tileLine = [&](const char* name, const std::vector<float>& v) { std::printf("%-34s %9.3f %9.3f %9.3f %9.3f\n", name, pearson(v, traw), spearman(v, traw), pearson(v, tsad), spearman(v, tsad)); };
    tileLine("summed uncertainty", tu);
    tileLine("coarse soft pixels (today)", tsoft);
    tileLine("boundary complexity", tedges);
    tileLine("soft + uncertainty + complexity", tcombo);
    // The windows question: per image, the share of the coarse mask's error that the top two tiles by each ranking hold.
    double shareSoft = 0, shareU = 0, shareCombo = 0, shareBest = 0;
    int images = 0;
    for (const auto& list : tilesOf) {
        if (list.size() < 3) continue;
        double total = 0;
        for (const Tile& t : list) total += t.rawSad;
        if (total <= 0) continue;
        auto topTwo = [&](auto key) {
            std::vector<size_t> order(list.size());
            for (size_t i = 0; i < order.size(); i++) order[i] = i;
            std::partial_sort(order.begin(), order.begin() + 2, order.end(), [&](size_t a, size_t b) { return key(list[a]) > key(list[b]); });
            return (list[order[0]].rawSad + list[order[1]].rawSad) / total;
        };
        shareSoft += topTwo([](const Tile& t) { return t.soft; });
        shareU += topTwo([](const Tile& t) { return t.u; });
        shareCombo += topTwo([&](const Tile& t) { return t.soft / ms + t.u / mu + t.edges / me; });
        shareBest += topTwo([](const Tile& t) { return t.rawSad; });
        images++;
    }
    if (images) std::printf("\nshare of the coarse mask's error in the top two tiles (%d images): soft %.3f, uncertainty %.3f, combined %.3f, oracle %.3f\n",
                            images, shareSoft / images, shareU / images, shareCombo / images, shareBest / images);
    return 0;
}

/// Synthetic strokes from a ground-truth alpha: 1 on a cross through the solid core of the subject (pixels
/// whose whole 31-pixel box is opaque), 2 on a 10-pixel border band where the truth is transparent.
/// Empty when the subject has no solid core.
GrayImage strokesFrom(const GrayImage& truth) {
    const int w = truth.width(), h = truth.height(), r = 15;
    std::vector<int> sum(size_t(w + 1) * size_t(h + 1), 0);
    auto at = [&](int x, int y) -> int& { return sum[size_t(y) * size_t(w + 1) + size_t(x)]; };
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) at(x + 1, y + 1) = (truth.at(x, y) >= 250) + at(x, y + 1) + at(x + 1, y) - at(x, y);
    auto core = [&](int x, int y) {
        if (x < r || y < r || x >= w - r || y >= h - r) return false;
        return at(x + r + 1, y + r + 1) - at(x - r, y + r + 1) - at(x + r + 1, y - r) + at(x - r, y - r) == (2 * r + 1) * (2 * r + 1);
    };
    GrayImage labels(w, h, 0);
    long long cx = 0, cy = 0, n = 0;
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) if (core(x, y)) { cx += x; cy += y; n++; }
    if (!n) return GrayImage();
    const int mx = int(cx / n), my = int(cy / n);
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
        if ((std::abs(y - my) <= 2 || std::abs(x - mx) <= 2) && core(x, y)) labels.at(x, y) = 1;
        else if ((x < 10 || y < 10 || x >= w - 10 || y >= h - 10) && truth.at(x, y) <= 5) labels.at(x, y) = 2;
    }
    return labels;
}

std::vector<int> intList(const std::string& text) {
    std::vector<int> out;
    std::stringstream in(text);
    for (std::string item; std::getline(in, item, ',');) out.push_back(std::atoi(item.c_str()));
    return out;
}

int scribbleMode(int argc, char** argv) {
    if (argc < 3) { std::fprintf(stderr, "usage: matte_tool scribble <dir> [options]\n"); return 2; }
    const std::string dir = argv[2];
    std::string suffix = "_alpha";
    int limit = 0;
    std::vector<int> sizes{300, 450, 600}, iterations{1, 2, 3};
    int refine = 8;
    for (int i = 3; i + 1 < argc; i += 2) {
        const std::string key = argv[i], value = argv[i + 1];
        if (key == "--suffix") suffix = value;
        else if (key == "--limit") limit = std::atoi(value.c_str());
        else if (key == "--sizes") sizes = intList(value);
        else if (key == "--iterations") iterations = intList(value);
        else if (key == "--refine") refine = std::atoi(value.c_str());
    }
    std::vector<fs::path> files;
    for (const auto& entry : fs::directory_iterator(dir))
        if (entry.path().extension() == ".png" && entry.path().filename().string().find(suffix + ".png") == std::string::npos) files.push_back(entry.path());
    std::sort(files.begin(), files.end());
    // An even spread over the set rather than its first names.
    if (limit > 0 && int(files.size()) > limit) {
        std::vector<fs::path> spread;
        for (int i = 0; i < limit; i++) spread.push_back(files[size_t(i) * files.size() / size_t(limit)]);
        files = spread;
    }
    struct Config { int size, iterations; double iou = 0, ms = 0, worstMs = 0, refinedIou = 0, refinedSad = 0; int count = 0; };
    struct Result { double iou = -1, ms = 0, refinedIou = 0, refinedSad = 0; };
    MatteSettings refinement;
    refinement.refineEdges = refine;
    refinement.contrast = 25;
    refinement.matting = 0;
    refinement.cleanup = true;
    refinement.decontaminate = false;
    auto iouOf = [](const GrayImage& a, const GrayImage& b) {
        long long both = 0, either = 0;
        for (int y = 0; y < a.height(); y++) for (int x = 0; x < a.width(); x++) {
            const bool p = a.at(x, y) >= 128, q = b.at(x, y) >= 128;
            both += p && q; either += p || q;
        }
        return either ? double(both) / double(either) : 1.0;
    };
    std::vector<Config> configs;
    for (int s : sizes) for (int it : iterations) configs.push_back({s, it});
    std::vector<std::vector<Result>> results(files.size(), std::vector<Result>(configs.size()));
    std::atomic<size_t> next{0};
    // GrabCut is single-threaded, so the images run side by side; a few threads keep the timings honest.
    std::vector<std::thread> workers;
    for (int t = 0; t < 6; t++)
        workers.emplace_back([&] {
            for (size_t f; (f = next++) < files.size();) {
                const std::string stem = files[f].stem().string();
                auto image = readPngImage(files[f].string());
                auto truth = readAlpha((files[f].parent_path() / (stem + suffix + ".png")).string(), nullptr);
                if (!image || !truth || truth->width() != image->width() || truth->height() != image->height()) continue;
                GrayImage labels = strokesFrom(*truth);
                if (labels.isEmpty()) continue;
                for (size_t c = 0; c < configs.size(); c++) {
                    auto t0 = std::chrono::steady_clock::now();
                    auto coverage = scribbleSelection(*image, labels, configs[c].size, configs[c].iterations, nullptr);
                    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
                    if (!coverage) continue;
                    auto refined = refineMatte(*coverage, *image, refinement, 0);
                    results[f][c] = {iouOf(*coverage, *truth), ms, iouOf(*refined, *truth), score(*refined, *truth).sad};
                }
            }
        });
    for (auto& w : workers) w.join();
    for (size_t f = 0; f < files.size(); f++)
        for (size_t c = 0; c < configs.size(); c++) {
            const Result& r = results[f][c];
            if (r.iou < 0) continue;
            configs[c].iou += r.iou;
            configs[c].ms += r.ms;
            configs[c].worstMs = std::max(configs[c].worstMs, r.ms);
            configs[c].refinedIou += r.refinedIou;
            configs[c].refinedSad += r.refinedSad;
            configs[c].count++;
        }
    std::printf("%6s %10s %8s %10s %10s %12s %12s %6s\n", "size", "iterations", "IoU", "mean ms", "worst ms", "refined IoU", "refined SAD", "n");
    for (const Config& c : configs) {
        const double n = std::max(1, c.count);
        std::printf("%6d %10d %8.4f %10.0f %10.0f %12.4f %12.2f %6d\n", c.size, c.iterations, c.iou / n, c.ms / n, c.worstMs, c.refinedIou / n, c.refinedSad / n, c.count);
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc >= 2 && std::strcmp(argv[1], "scribble") == 0) return scribbleMode(argc, argv);
    if (argc >= 2 && std::strcmp(argv[1], "run") == 0) return runMode(argc, argv);
    if (argc >= 2 && std::strcmp(argv[1], "eval") == 0) return evalMode(argc, argv);
    if (argc >= 2 && std::strcmp(argv[1], "synth") == 0) return synthMode(argc, argv);
    if (argc >= 2 && std::strcmp(argv[1], "residual") == 0) return residualMode(argc, argv);
    std::fprintf(stderr, "usage: matte_tool run <image.png> <model.onnx|none> <outdir> [options]\n       matte_tool eval <dir> <model.onnx|none> [options]\n"
                         "       matte_tool synth <outdir> [--size N]\n       matte_tool residual <dir> <model.onnx|none> [options]\n       matte_tool scribble <dir> [options]\n");
    return 2;
}
