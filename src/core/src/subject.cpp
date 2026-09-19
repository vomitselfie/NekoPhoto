// Remove Background's model side: one ONNX segmentation network through OpenCV's DNN module, run once on
// the whole layer (`subjectMask`), and optionally again on native-resolution windows along the edge, where
// the 1024-pixel pass blurred away hair and thin structures (`subjectMaskDetailed`; the boundary-window
// heuristics of LawDIS, PDFNet and SegRefiner, docs/background-removal-review.md item 7). The window planner
// and the frequency fusion are plain pixel maths and always available.
#include "compositor/subject.h"
#include "compositor/blur.h"
#include "compositor/morphology.h"
#include "compositor/parallel.h"
#include <algorithm>
#include <cmath>
#include <vector>

#ifdef COMPOSITOR_HAVE_OPENCV
#include <opencv2/core.hpp>
#include <opencv2/dnn.hpp>
#include <opencv2/imgproc.hpp>
#include <memory>
#include <mutex>
#endif

namespace compositor {

std::vector<DetailWindow> detailWindows(const GrayImage& uncertain, int size, int maxWindows, int minPixels) {
    const int w = uncertain.width(), h = uncertain.height();
    std::vector<DetailWindow> windows;
    if (size <= 0 || size > w || size > h || maxWindows <= 0) return windows;
    // Candidate corners on a half-window grid, the last one flush with the far edge.
    auto corners = [size](int length) {
        std::vector<int> out;
        for (int c = 0; c + size < length; c += std::max(1, size / 2)) out.push_back(c);
        out.push_back(length - size);
        return out;
    };
    const std::vector<int> xs = corners(w), ys = corners(h);
    std::vector<uint8_t> uncovered(size_t(w) * h);
    for (size_t i = 0; i < uncovered.size(); i++) uncovered[i] = uncertain.data()[i] ? 1 : 0;
    std::vector<uint32_t> integral(size_t(w + 1) * size_t(h + 1));
    auto sumIn = [&](int x0, int y0, int x1, int y1) {   // [x0, x1) x [y0, y1)
        const size_t stride = size_t(w + 1);
        return integral[size_t(y1) * stride + size_t(x1)] - integral[size_t(y0) * stride + size_t(x1)] - integral[size_t(y1) * stride + size_t(x0)] + integral[size_t(y0) * stride + size_t(x0)];
    };
    while (int(windows.size()) < maxWindows) {
        for (int y = 0; y < h; y++) {
            uint32_t rowSum = 0;
            for (int x = 0; x < w; x++) {
                rowSum += uncovered[size_t(y) * w + size_t(x)];
                integral[size_t(y + 1) * size_t(w + 1) + size_t(x + 1)] = integral[size_t(y) * size_t(w + 1) + size_t(x + 1)] + rowSum;
            }
        }
        DetailWindow best;
        uint32_t bestCount = 0;
        for (int y : ys)
            for (int x : xs) {
                const uint32_t count = sumIn(x, y, x + size, y + size);
                if (count > bestCount) { bestCount = count; best = {x, y, size}; }
            }
        if (bestCount < uint32_t(std::max(1, minPixels))) break;
        windows.push_back(best);
        for (int y = best.y; y < best.y + size; y++) std::fill(&uncovered[size_t(y) * w + size_t(best.x)], &uncovered[size_t(y) * w + size_t(best.x)] + size, uint8_t(0));
    }
    return windows;
}

std::shared_ptr<GrayImage> fuseDetail(const GrayImage& coarse, const GrayImage& local, const GrayImage& weight, double sigma) {
    const int w = coarse.width(), h = coarse.height();
    auto out = std::make_shared<GrayImage>(coarse);
    if (local.width() != w || local.height() != h || weight.width() != w || weight.height() != h) return out;
    GrayImage lowCoarse = coarse, lowLocal = local;
    if (sigma > 0) { gaussianBlur(lowCoarse, sigma); gaussianBlur(lowLocal, sigma); }
    parallelRows(0, h, [&](int y0, int y1) {
        for (int y = y0; y < y1; y++)
            for (int x = 0; x < w; x++) {
                const int k = weight.at(x, y);
                if (!k) continue;
                const int c = coarse.at(x, y);
                int fused;
                if (sigma > 0) fused = std::clamp(int(local.at(x, y)) + int(lowCoarse.at(x, y)) - int(lowLocal.at(x, y)), 0, 255);
                else {
                    // Sure means within 0.3 of 0 or 1: confidence ramps from 0 at 0.3..0.7 to 1 at the ends.
                    const float l = local.at(x, y) / 255.0f, cf = c / 255.0f;
                    const float sureL = std::clamp((std::fabs(l - 0.5f) - 0.2f) / 0.3f, 0.0f, 1.0f), sureC = std::clamp((std::fabs(cf - 0.5f) - 0.2f) / 0.3f, 0.0f, 1.0f);
                    const float value = sureL * l + (1 - sureL) * (sureC * cf + (1 - sureC) * l);
                    fused = int(std::lround(std::clamp(value, 0.0f, 1.0f) * 255));
                }
                out->at(x, y) = uint8_t(c + ((fused - c) * k + (fused >= c ? 127 : -127)) / 255);
            }
    });
    return out;
}

#ifdef COMPOSITOR_HAVE_OPENCV

bool subjectModelSupported() { return true; }

namespace {

// One loaded network per model path, kept for the session: loading takes longer than running. The cache and
// its lock are never destroyed: a model may still be running on a worker thread when the process exits, and
// tearing the network down under it corrupts the heap. Entries live behind pointers so that loading another
// model never moves one that is in use.
struct LoadedModel { std::string path; cv::dnn::Net net; };
std::mutex& modelLock = *new std::mutex;
std::vector<std::unique_ptr<LoadedModel>>& models = *new std::vector<std::unique_ptr<LoadedModel>>;

cv::dnn::Net* modelFor(const std::string& path, std::string* error) {
    std::lock_guard<std::mutex> lock(modelLock);
    for (auto& m : models) if (m->path == path) return &m->net;
    try {
        cv::dnn::Net net = cv::dnn::readNetFromONNX(path);
        if (net.empty()) { if (error) *error = "The model file could not be read."; return nullptr; }
        models.push_back(std::make_unique<LoadedModel>(LoadedModel{path, std::move(net)}));
        return &models.back()->net;
    } catch (const std::exception& e) {
        if (error) *error = std::string("The model could not be loaded: ") + e.what();
        return nullptr;
    }
}

// What the file name says about the model: IS-Net takes 1024 px and (x - 0.5); the U2Net family 320 px
// with ImageNet normalisation over the image's maximum; MODNet-style matting models 512 px, (x - 0.5) / 0.5,
// and give an alpha matte directly (no min-max stretch afterwards); PP-HumanSeg (OpenCV's model zoo) 192 px,
// (x - 0.5) / 0.5, a two-class softmax whose second plane is the person.
struct Kind { bool isnet = false, modnet = false, humanseg = false; int size = 320; };

Kind kindOf(const std::string& modelPath) {
    const std::string name = modelPath.substr(modelPath.rfind('/') + 1);
    Kind k;
    k.isnet = name.find("isnet") != std::string::npos;
    k.modnet = name.find("modnet") != std::string::npos;
    k.humanseg = name.find("pphumanseg") != std::string::npos;
    k.size = k.isnet ? 1024 : k.modnet ? 512 : k.humanseg ? 192 : 320;
    return k;
}

/// The network's input size for a source of (w, h). The saliency nets and PP-HumanSeg are exported at a fixed
/// square. MODNet keeps the aspect ratio: the short side becomes 512 and both sides round down to a multiple
/// of 32 (its own inference rule), when the export takes a free shape.
void inputSize(const Kind& kind, int w, int h, int& inW, int& inH) {
    inW = inH = kind.size;
    if (!kind.modnet || w <= 0 || h <= 0) return;
    if (w >= h) inW = int(std::lround(double(w) / h * kind.size));
    else inH = int(std::lround(double(h) / w * kind.size));
    inW = std::max(32, inW - inW % 32);
    inH = std::max(32, inH - inH % 32);
}

/// A crop of the layer's straight colour over black where transparent, as RGB float 0..1.
cv::Mat straightRgb(const Image& image, int x0, int y0, int w, int h) {
    cv::Mat rgb(h, w, CV_32FC3);
    for (int y = 0; y < h; y++) {
        const uint8_t* p = image.pixel(x0, y0 + y);
        cv::Vec3f* out = rgb.ptr<cv::Vec3f>(y);
        for (int x = 0; x < w; x++, p += 4) {
            const float a = p[3] ? p[3] / 255.0f : 1.0f;
            out[x] = {p[0] / 255.0f / a, p[1] / 255.0f / a, p[2] / 255.0f / a};
        }
    }
    return rgb;
}

/// One forward pass over `rgb` resized to (inW, inH) with the model's normalisation; `pred` is the prediction
/// plane at that size, as the network gives it. A non-square shape a fixed-shape export refuses is retried at
/// the square it was exported with.
bool predict(cv::dnn::Net* net, const Kind& kind, const cv::Mat& rgb, int inW, int inH, cv::Mat& pred, std::string* error) {
    cv::Mat resized, channels[3];
    auto prepare = [&](int width, int height) {
        cv::resize(rgb, resized, cv::Size(width, height), 0, 0, cv::INTER_AREA);
        cv::split(resized, channels);
        if (kind.isnet) { for (auto& c : channels) c = c - 0.5f; }
        else if (kind.modnet || kind.humanseg) { for (auto& c : channels) c = (c - 0.5f) / 0.5f; }
        else {
            double mx = 0;
            cv::minMaxLoc(resized.reshape(1), nullptr, &mx);
            const float mean[3] = {0.485f, 0.456f, 0.406f}, sd[3] = {0.229f, 0.224f, 0.225f};
            for (int c = 0; c < 3; c++) channels[c] = (channels[c] / float(std::max(1e-6, mx)) - mean[c]) / sd[c];
        }
        cv::merge(channels, 3, resized);
    };
    auto run = [&](cv::Mat& out, std::string* why) {
        cv::Mat blob = cv::dnn::blobFromImage(resized, 1.0, cv::Size(resized.cols, resized.rows), cv::Scalar(), false, false);
        try {
            std::lock_guard<std::mutex> lock(modelLock);
            net->setInput(blob);
            out = net->forward();
        } catch (const std::exception& e) {
            if (why) *why = std::string("The model could not be run: ") + e.what();
            return false;
        }
        if (out.dims != 4 || out.size[2] != resized.rows || out.size[3] != resized.cols) { if (why) *why = "The model gave an unexpected output."; return false; }
        if (kind.humanseg && out.size[1] < 2) { if (why) *why = "The model gave an unexpected output."; return false; }
        return true;
    };
    prepare(inW, inH);
    cv::Mat out;
    std::string why;
    if (!run(out, &why)) {
        if (inW == inH) { if (error) *error = why; return false; }
        prepare(kind.size, kind.size);
        if (!run(out, &why)) { if (error) *error = why; return false; }
    }
    pred = cv::Mat(resized.rows, resized.cols, CV_32F, out.ptr<float>(0, kind.humanseg ? 1 : 0)).clone();
    return true;
}

std::shared_ptr<GrayImage> toMask(const cv::Mat& plane, int w, int h) {
    cv::Mat full;
    cv::resize(plane, full, cv::Size(w, h), 0, 0, cv::INTER_LINEAR);
    auto mask = std::make_shared<GrayImage>(w, h);
    for (int y = 0; y < h; y++) {
        const float* row = full.ptr<float>(y);
        for (int x = 0; x < w; x++) mask->at(x, y) = uint8_t(std::min(255.0f, std::max(0.0f, row[x] * 255 + 0.5f)));
    }
    return mask;
}

} // namespace

std::shared_ptr<GrayImage> subjectMask(const Image& image, const std::string& modelPath, std::string* error) {
    if (image.isEmpty()) return nullptr;
    cv::dnn::Net* net = modelFor(modelPath, error);
    if (!net) return nullptr;
    const Kind kind = kindOf(modelPath);
    int inW, inH;
    inputSize(kind, image.width(), image.height(), inW, inH);
    cv::Mat pred;
    if (!predict(net, kind, straightRgb(image, 0, 0, image.width(), image.height()), inW, inH, pred, error)) return nullptr;
    if (!kind.modnet && !kind.humanseg) {
        double mn, mx;
        cv::minMaxLoc(pred, &mn, &mx);
        pred = (pred - mn) / std::max(1e-6, mx - mn);
    }
    return toMask(pred, image.width(), image.height());
}

std::shared_ptr<GrayImage> subjectMaskDetailed(const Image& image, const std::string& modelPath, const GrayImage* coarseIn, int maxWindows, std::string* error) {
    std::shared_ptr<GrayImage> coarse = coarseIn && coarseIn->width() == image.width() && coarseIn->height() == image.height() ? std::make_shared<GrayImage>(*coarseIn) : subjectMask(image, modelPath, error);
    if (!coarse) return nullptr;
    const Kind kind = kindOf(modelPath);
    const int w = image.width(), h = image.height();
    const double scale = double(std::max(w, h)) / kind.size;
    if (kind.humanseg || scale < 1.5 || maxWindows <= 0) return coarse;
    cv::dnn::Net* net = modelFor(modelPath, error);
    if (!net) return nullptr;
    // Windows of the model's own size at native resolution; above four times the input, of twice that size
    // reduced by half, so a huge layer still gets one pass per window.
    const int reduction = scale > 4 ? 2 : 1;
    const int crop = std::min(kind.size * reduction, std::min(w, h));
    if (crop < kind.size / 2) return coarse;
    // Where the coarse pass is unsure: its half-transparent pixels, widened by a few coarse pixels.
    GrayImage soft(w, h, 0);
    for (size_t i = 0; i < soft.byteCount(); i++) soft.data()[i] = coarse->data()[i] > 25 && coarse->data()[i] < 230 ? 255 : 0;
    const int margin = std::max(12, int(std::lround(4 * scale)));
    std::shared_ptr<GrayImage> uncertain = growSelection(soft, margin);
    for (size_t i = 0; i < uncertain->byteCount(); i++) uncertain->data()[i] = uncertain->data()[i] >= 128 ? 255 : 0;
    const std::vector<DetailWindow> windows = detailWindows(*uncertain, crop, maxWindows, crop * crop / 200);
    if (windows.empty()) return coarse;

    std::vector<float> sum(size_t(w) * h, 0), weightSum(size_t(w) * h, 0);
    std::vector<float> hann(static_cast<size_t>(crop));
    for (int i = 0; i < crop; i++) hann[size_t(i)] = float(0.5 - 0.5 * std::cos(2 * M_PI * (i + 0.5) / crop));
    for (const DetailWindow& window : windows) {
        cv::Mat pred;
        if (!predict(net, kind, straightRgb(image, window.x, window.y, crop, crop), kind.size, kind.size, pred, error)) return nullptr;
        if (reduction != 1 || pred.cols != crop || pred.rows != crop) { cv::Mat resized; cv::resize(pred, resized, cv::Size(crop, crop), 0, 0, cv::INTER_LINEAR); pred = resized; }
        // Calibrate the window's output to the coarse mask where that is confident: a least-squares line
        // through (prediction, coarse) over the sure pixels. A window without both kinds, or whose line runs
        // the wrong way, is a window the network read differently from the whole image, and is skipped.
        double sx = 0, sy = 0, sxx = 0, sxy = 0;
        int n = 0, sureF = 0, sureB = 0;
        for (int y = 0; y < crop; y++) {
            const float* row = pred.ptr<float>(y);
            for (int x = 0; x < crop; x++) {
                const int c = coarse->at(window.x + x, window.y + y);
                if (c > 25 && c < 230) continue;
                const double px = row[x], py = c / 255.0;
                sx += px; sy += py; sxx += px * px; sxy += px * py; n++;
                (c >= 230 ? sureF : sureB)++;
            }
        }
        const int few = crop * crop / 100;
        if (sureF < few || sureB < few) continue;
        const double det = n * sxx - sx * sx;
        if (std::fabs(det) < 1e-9) continue;
        const double a = (n * sxy - sx * sy) / det, b = (sy - a * sx) / n;
        if (a <= 0) continue;
        for (int y = 0; y < crop; y++) {
            const float* row = pred.ptr<float>(y);
            const size_t base = size_t(window.y + y) * w + size_t(window.x);
            for (int x = 0; x < crop; x++) {
                const float value = std::clamp(float(a * row[x] + b), 0.0f, 1.0f), wgt = hann[size_t(x)] * hann[size_t(y)];
                sum[base + size_t(x)] += wgt * value;
                weightSum[base + size_t(x)] += wgt;
            }
        }
    }
    GrayImage local = *coarse, weight(w, h, 0);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            const size_t i = size_t(y) * w + size_t(x);
            if (weightSum[i] < 0.05f || !uncertain->at(x, y)) continue;
            local.at(x, y) = uint8_t(std::lround(std::clamp(sum[i] / weightSum[i], 0.0f, 1.0f) * 255));
            weight.at(x, y) = 255;
        }
    // Inside the band the windows' mask takes over where it is sure, the coarse one where only it is sure
    // (a window's output has a soft floor around the subject at native resolution, where the whole-image pass
    // is certain of background), feathered at the band's edge. Exchanging only the high frequencies would
    // guard against a window that disagrees with the whole-image pass, but the calibration already drops
    // those, and the exchange rings wherever the window puts the edge further out, which is exactly the fur
    // the coarse pass missed.
    gaussianBlur(weight, 2);
    return fuseDetail(*coarse, local, weight, 0);
}

#else

bool subjectModelSupported() { return false; }

std::shared_ptr<GrayImage> subjectMask(const Image&, const std::string&, std::string* error) {
    if (error) *error = "This build has no segmentation model support (OpenCV was not found when building).";
    return nullptr;
}

std::shared_ptr<GrayImage> subjectMaskDetailed(const Image&, const std::string&, const GrayImage*, int, std::string* error) {
    if (error) *error = "This build has no segmentation model support (OpenCV was not found when building).";
    return nullptr;
}

#endif

} // namespace compositor
