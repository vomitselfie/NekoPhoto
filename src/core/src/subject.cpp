#include "compositor/subject.h"
#include <algorithm>
#include <cmath>

#ifdef COMPOSITOR_HAVE_OPENCV
#include <opencv2/core.hpp>
#include <opencv2/dnn.hpp>
#include <opencv2/imgproc.hpp>
#include <memory>
#include <mutex>
#endif

namespace compositor {

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

} // namespace

std::shared_ptr<GrayImage> subjectMask(const Image& image, const std::string& modelPath, std::string* error) {
    if (image.isEmpty()) return nullptr;
    cv::dnn::Net* net = modelFor(modelPath, error);
    if (!net) return nullptr;
    // What the file name says about the model: IS-Net takes 1024 px and (x - 0.5); the U2Net family 320 px
    // with ImageNet normalisation over the image's maximum; MODNet-style matting models 512 px, (x - 0.5) / 0.5,
    // and give an alpha matte directly (no min-max stretch afterwards); PP-HumanSeg 192 px, a probability.
    std::string name = modelPath.substr(modelPath.rfind('/') + 1);
    bool isnet = name.find("isnet") != std::string::npos;
    bool modnet = name.find("modnet") != std::string::npos;
    // PP-HumanSeg (OpenCV's model zoo): 192 px, (x - 0.5) / 0.5, a two-class softmax whose second plane is the person.
    bool humanseg = name.find("pphumanseg") != std::string::npos;
    int size = isnet ? 1024 : modnet ? 512 : humanseg ? 192 : 320;
    // The saliency nets and PP-HumanSeg are exported at a fixed square. MODNet keeps the aspect ratio: the short
    // side becomes 512 and both sides round down to a multiple of 32 (its own inference rule), when the export
    // takes a free shape; a fixed-shape export gets the square after all.
    int inW = size, inH = size;
    if (modnet && image.width() > 0 && image.height() > 0) {
        if (image.width() >= image.height()) { inH = size; inW = int(std::lround(double(image.width()) / image.height() * size)); }
        else { inW = size; inH = int(std::lround(double(image.height()) / image.width() * size)); }
        inW = std::max(32, inW - inW % 32);
        inH = std::max(32, inH - inH % 32);
    }
    // The layer's straight colour over black where transparent, as RGB float 0..1.
    cv::Mat rgb(image.height(), image.width(), CV_32FC3);
    for (int y = 0; y < image.height(); y++) {
        const uint8_t* p = image.row(y);
        cv::Vec3f* out = rgb.ptr<cv::Vec3f>(y);
        for (int x = 0; x < image.width(); x++, p += 4) {
            float a = p[3] ? p[3] / 255.0f : 1.0f;
            out[x] = {p[0] / 255.0f / a, p[1] / 255.0f / a, p[2] / 255.0f / a};
        }
    }
    cv::Mat resized;
    cv::Mat channels[3];
    auto prepare = [&](int width, int height) {
        cv::resize(rgb, resized, cv::Size(width, height), 0, 0, cv::INTER_AREA);
        cv::split(resized, channels);
    };
    prepare(inW, inH);
    if (isnet) {
        for (auto& c : channels) c = c - 0.5f; // (x - 0.5) / 1
    } else if (modnet || humanseg) {
        for (auto& c : channels) c = (c - 0.5f) / 0.5f;
    } else {
        double mx = 0;
        cv::minMaxLoc(resized.reshape(1), nullptr, &mx);
        const float mean[3] = {0.485f, 0.456f, 0.406f}, sd[3] = {0.229f, 0.224f, 0.225f};
        for (int c = 0; c < 3; c++) channels[c] = (channels[c] / float(std::max(1e-6, mx)) - mean[c]) / sd[c];
    }
    auto run = [&](cv::Mat& out, std::string* why) {
        cv::merge(channels, 3, resized);
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
        return true;
    };
    cv::Mat out;
    std::string why;
    if (!run(out, &why)) {
        if (inW == inH) { if (error) *error = why; return nullptr; }
        // A fixed-shape export: once more at the square it was exported with.
        inW = inH = size;
        prepare(inW, inH);
        if (isnet) { for (auto& c : channels) c = c - 0.5f; }
        else { for (auto& c : channels) c = (c - 0.5f) / 0.5f; }
        if (!run(out, &why)) { if (error) *error = why; return nullptr; }
    }
    if (humanseg && out.size[1] < 2) { if (error) *error = "The model gave an unexpected output."; return nullptr; }
    cv::Mat pred(inH, inW, CV_32F, out.ptr<float>(0, humanseg ? 1 : 0));
    cv::Mat scaled;
    if (modnet || humanseg) scaled = pred;
    else {
        double mn, mx;
        cv::minMaxLoc(pred, &mn, &mx);
        scaled = (pred - mn) / std::max(1e-6, mx - mn);
    }
    cv::Mat full;
    cv::resize(scaled, full, cv::Size(image.width(), image.height()), 0, 0, cv::INTER_LINEAR);
    auto mask = std::make_shared<GrayImage>(image.width(), image.height());
    for (int y = 0; y < image.height(); y++) {
        const float* row = full.ptr<float>(y);
        for (int x = 0; x < image.width(); x++) mask->at(x, y) = uint8_t(std::min(255.0f, std::max(0.0f, row[x] * 255 + 0.5f)));
    }
    return mask;
}

#else

bool subjectModelSupported() { return false; }

std::shared_ptr<GrayImage> subjectMask(const Image&, const std::string&, std::string* error) {
    if (error) *error = "This build has no segmentation model support (OpenCV was not found when building).";
    return nullptr;
}

#endif

} // namespace compositor
