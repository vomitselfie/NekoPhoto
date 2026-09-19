#include "compositor/scribble.h"
#include <algorithm>
#include <cmath>

#ifdef COMPOSITOR_HAVE_OPENCV
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#endif

namespace compositor {

#ifdef COMPOSITOR_HAVE_OPENCV

bool scribbleSelectionSupported() { return true; }

std::shared_ptr<GrayImage> scribbleSelection(const Image& image, const GrayImage& labels, int limit, int iterations, std::string* error) {
    const int w = image.width(), h = image.height();
    if (w <= 0 || h <= 0 || labels.width() != w || labels.height() != h) { if (error) *error = "The strokes do not match the image."; return nullptr; }
    // The reduced grid, and the strokes carried onto it so that a thin stroke survives: a reduced pixel takes
    // the foreground label when any of its pixels has it, else the background label when any has that.
    const double factor = limit > 0 ? std::min(1.0, double(limit) / std::max(w, h)) : 1;
    const int rw = std::max(1, int(std::lround(w * factor))), rh = std::max(1, int(std::lround(h * factor)));
    cv::Mat small(rh, rw, CV_8UC1, cv::Scalar(0));
    int minX = rw, minY = rh, maxX = -1, maxY = -1;
    for (int y = 0; y < h; y++) {
        const uint8_t* row = labels.row(y);
        const int ry = std::min(rh - 1, int(y * factor));
        for (int x = 0; x < w; x++) {
            const uint8_t v = row[x];
            if (!v) continue;
            const int rx = std::min(rw - 1, int(x * factor));
            uint8_t& cell = small.at<uint8_t>(ry, rx);
            if (v == 1) { cell = 1; minX = std::min(minX, rx); minY = std::min(minY, ry); maxX = std::max(maxX, rx); maxY = std::max(maxY, ry); }
            else if (v == 2 && cell != 1) cell = 2;
        }
    }
    if (maxX < 0) { if (error) *error = "Mark the subject with a foreground stroke first."; return nullptr; }
    // The image as straight BGR bytes over black where transparent.
    cv::Mat bgrFull(h, w, CV_8UC3);
    for (int y = 0; y < h; y++) {
        const uint8_t* p = image.row(y);
        cv::Vec3b* out = bgrFull.ptr<cv::Vec3b>(y);
        for (int x = 0; x < w; x++, p += 4) {
            const int a = p[3];
            auto straight = [a](int c) { return uint8_t(a ? std::min(255, c * 255 / a) : c); };
            out[x] = {straight(p[2]), straight(p[1]), straight(p[0])};
        }
    }
    cv::Mat bgr;
    if (rw != w || rh != h) cv::resize(bgrFull, bgr, cv::Size(rw, rh), 0, 0, cv::INTER_AREA);
    else bgr = bgrFull;
    // GrabCut's mask: probable background everywhere, probable foreground around the foreground strokes (their
    // box grown by half its size), the strokes themselves certain.
    cv::Mat mask(rh, rw, CV_8UC1, cv::Scalar(cv::GC_PR_BGD));
    const int growX = std::max(8, (maxX - minX + 1) / 2), growY = std::max(8, (maxY - minY + 1) / 2);
    const cv::Rect around(std::max(0, minX - growX), std::max(0, minY - growY), 0, 0);
    const int x1 = std::min(rw, maxX + growX + 1), y1 = std::min(rh, maxY + growY + 1);
    mask(cv::Rect(around.x, around.y, x1 - around.x, y1 - around.y)).setTo(cv::GC_PR_FGD);
    for (int y = 0; y < rh; y++)
        for (int x = 0; x < rw; x++) {
            const uint8_t v = small.at<uint8_t>(y, x);
            if (v == 1) mask.at<uint8_t>(y, x) = cv::GC_FGD;
            else if (v == 2) mask.at<uint8_t>(y, x) = cv::GC_BGD;
        }
    cv::Mat background, foreground;
    try {
        cv::grabCut(bgr, mask, cv::Rect(), background, foreground, std::max(1, iterations), cv::GC_INIT_WITH_MASK);
    } catch (const std::exception& e) {
        if (error) *error = std::string("GrabCut failed: ") + e.what();
        return nullptr;
    }
    cv::Mat coverageSmall(rh, rw, CV_8UC1);
    for (int y = 0; y < rh; y++)
        for (int x = 0; x < rw; x++) {
            const uint8_t v = mask.at<uint8_t>(y, x);
            coverageSmall.at<uint8_t>(y, x) = (v == cv::GC_FGD || v == cv::GC_PR_FGD) ? 255 : 0;
        }
    cv::Mat coverageFull;
    if (rw != w || rh != h) cv::resize(coverageSmall, coverageFull, cv::Size(w, h), 0, 0, cv::INTER_LINEAR);
    else coverageFull = coverageSmall;
    auto out = std::make_shared<GrayImage>(w, h);
    for (int y = 0; y < h; y++) std::copy(coverageFull.ptr<uint8_t>(y), coverageFull.ptr<uint8_t>(y) + w, out->row(y));
    return out;
}

#else

bool scribbleSelectionSupported() { return false; }

std::shared_ptr<GrayImage> scribbleSelection(const Image&, const GrayImage&, int, int, std::string* error) {
    if (error) *error = "This build has no OpenCV, which runs the scribble selection.";
    return nullptr;
}

#endif

} // namespace compositor
