#include "compositor/subject.h"
#include <algorithm>
#include <cmath>

#ifdef COMPOSITOR_HAVE_OPENCV
#include <opencv2/core.hpp>
#include <opencv2/dnn.hpp>
#include <opencv2/imgproc.hpp>
#include <mutex>
#endif

namespace compositor {

#ifdef COMPOSITOR_HAVE_OPENCV

bool subjectModelSupported() { return true; }

namespace {

// One loaded network per model path, kept for the session: loading takes longer than running.
struct LoadedModel { std::string path; cv::dnn::Net net; };
std::mutex modelLock;
std::vector<LoadedModel> models;

cv::dnn::Net* modelFor(const std::string& path, std::string* error) {
    std::lock_guard<std::mutex> lock(modelLock);
    for (auto& m : models) if (m.path == path) return &m.net;
    try {
        cv::dnn::Net net = cv::dnn::readNetFromONNX(path);
        if (net.empty()) { if (error) *error = "The model file could not be read."; return nullptr; }
        models.push_back({path, std::move(net)});
        return &models.back().net;
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
    // and give an alpha matte directly (no min-max stretch afterwards).
    std::string name = modelPath.substr(modelPath.rfind('/') + 1);
    bool isnet = name.find("isnet") != std::string::npos;
    bool modnet = name.find("modnet") != std::string::npos;
    int size = isnet ? 1024 : modnet ? 512 : 320;
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
    cv::resize(rgb, resized, cv::Size(size, size), 0, 0, cv::INTER_AREA);
    cv::Mat channels[3];
    cv::split(resized, channels);
    if (isnet) {
        for (auto& c : channels) c = c - 0.5f; // (x - 0.5) / 1
    } else if (modnet) {
        for (auto& c : channels) c = (c - 0.5f) / 0.5f;
    } else {
        double mx = 0;
        cv::minMaxLoc(resized.reshape(1), nullptr, &mx);
        const float mean[3] = {0.485f, 0.456f, 0.406f}, sd[3] = {0.229f, 0.224f, 0.225f};
        for (int c = 0; c < 3; c++) channels[c] = (channels[c] / float(std::max(1e-6, mx)) - mean[c]) / sd[c];
    }
    cv::merge(channels, 3, resized);
    cv::Mat blob = cv::dnn::blobFromImage(resized, 1.0, cv::Size(size, size), cv::Scalar(), false, false);
    cv::Mat out;
    try {
        std::lock_guard<std::mutex> lock(modelLock);
        net->setInput(blob);
        out = net->forward();
    } catch (const std::exception& e) {
        if (error) *error = std::string("The model could not be run: ") + e.what();
        return nullptr;
    }
    if (out.dims != 4 || out.size[2] != size || out.size[3] != size) { if (error) *error = "The model gave an unexpected output."; return nullptr; }
    cv::Mat pred(size, size, CV_32F, out.ptr<float>(0, 0));
    cv::Mat scaled;
    if (modnet) scaled = pred;
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
