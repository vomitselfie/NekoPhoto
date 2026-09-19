#include "compositor/subject.h"
#include "compositor/filters.h"
#include "compositor/parallel.h"
#include "compositor/render.h"
#include <algorithm>
#include <cmath>
#include <vector>

namespace compositor {

MatteSettings MatteSettings::normalized() const {
    auto c = [](double v, double lo, double hi, double f) { return std::isfinite(v) ? std::min(hi, std::max(lo, v)) : f; };
    return {c(refineEdges, 0, 40, 12), c(contrast, 0, 100, 25), c(shiftEdge, -10, 10, 0)};
}

namespace {

// Mean over a (2r+1)^2 square as two running-sum passes; the cost doesn't grow with the radius.
void boxMean(const std::vector<float>& src, std::vector<float>& out, int width, int height, int radius) {
    float span = float(radius * 2 + 1);
    std::vector<float> pass(size_t(width) * height);
    parallelRows(0, height, [&](int y0, int y1) {
        for (int y = y0; y < y1; y++) {
            size_t row = size_t(y) * width;
            float sum = 0;
            for (int x = -radius; x <= radius; x++) sum += src[row + size_t(std::min(width - 1, std::max(0, x)))];
            for (int x = 0; x < width; x++) {
                pass[row + size_t(x)] = sum / span;
                sum -= src[row + size_t(std::min(width - 1, std::max(0, x - radius)))];
                sum += src[row + size_t(std::min(width - 1, std::max(0, x + radius + 1)))];
            }
        }
    });
    out.assign(size_t(width) * height, 0);
    // Columns, in bands of columns.
    int bands = std::max(1, std::min(workerCount(), width / 32));
    parallelRows(0, bands, [&](int b0, int b1) {
        for (int b = b0; b < b1; b++) {
            int x0 = width * b / bands, x1 = width * (b + 1) / bands;
            for (int x = x0; x < x1; x++) {
                float sum = 0;
                for (int y = -radius; y <= radius; y++) sum += pass[size_t(std::min(height - 1, std::max(0, y))) * width + size_t(x)];
                for (int y = 0; y < height; y++) {
                    out[size_t(y) * width + size_t(x)] = sum / span;
                    sum -= pass[size_t(std::min(height - 1, std::max(0, y - radius))) * width + size_t(x)];
                    sum += pass[size_t(std::min(height - 1, std::max(0, y + radius + 1))) * width + size_t(x)];
                }
            }
        }
    }, 1);
}

std::vector<float> grayLevels(const Image& image, int width, int height) {
    // Luminance of the (unpremultiplied) colour, resampled to width x height.
    LayerTransform full(Point(0, 0), Size(image.width(), image.height()));
    std::shared_ptr<const Image> source = std::make_shared<Image>(image);
    if (width != image.width() || height != image.height()) source = resampleLayer(image, full, full, width, height);
    std::vector<float> out(size_t(width) * height);
    for (int y = 0; y < height; y++) for (int x = 0; x < width; x++) {
        const uint8_t* p = source->pixel(x, y);
        float a = p[3] ? p[3] / 255.0f : 1.0f;
        out[size_t(y) * width + size_t(x)] = (0.299f * p[0] + 0.587f * p[1] + 0.114f * p[2]) / 255.0f / a;
    }
    return out;
}

std::vector<float> maskLevels(const GrayImage& mask, int width, int height) {
    std::shared_ptr<const GrayImage> source = std::make_shared<GrayImage>(mask);
    if (width != mask.width() || height != mask.height()) {
        LayerTransform full(Point(0, 0), Size(mask.width(), mask.height()));
        source = resampleMask(mask, full, full, width, height, 0);
    }
    std::vector<float> out(size_t(width) * height);
    for (size_t i = 0; i < out.size(); i++) out[i] = source->data()[i] / 255.0f;
    return out;
}

std::shared_ptr<GrayImage> fromLevels(const std::vector<float>& levels, int width, int height, int fullWidth, int fullHeight) {
    auto small = std::make_shared<GrayImage>(width, height);
    for (size_t i = 0; i < levels.size(); i++) small->data()[i] = uint8_t(std::min(255.0f, std::max(0.0f, levels[i] * 255 + 0.5f)));
    if (width == fullWidth && height == fullHeight) return small;
    LayerTransform full(Point(0, 0), Size(fullWidth, fullHeight));
    LayerTransform placed(Point(0, 0), Size(fullWidth, fullHeight));
    return resampleMask(*small, placed, full, fullWidth, fullHeight, 0);
}

} // namespace

std::shared_ptr<GrayImage> guidedRefine(const GrayImage& mask, const Image& guide, double radius, int limit) {
    int fullW = mask.width(), fullH = mask.height();
    double factor = limit > 0 ? std::min(1.0, double(limit) / std::max(fullW, fullH)) : 1;
    int width = std::max(1, int(std::lround(fullW * factor))), height = std::max(1, int(std::lround(fullH * factor)));
    int steps = std::max(1, int(std::lround(radius * factor)));
    const float epsilon = 1e-4f;
    std::vector<float> m = maskLevels(mask, width, height), g = grayLevels(guide, width, height);
    size_t count = m.size();
    std::vector<float> meanG, meanM, sq(count), pr(count), meanSq, meanPr, slope(count), offset(count), meanSlope, meanOffset;
    boxMean(g, meanG, width, height, steps);
    boxMean(m, meanM, width, height, steps);
    for (size_t i = 0; i < count; i++) { sq[i] = g[i] * g[i]; pr[i] = g[i] * m[i]; }
    boxMean(sq, meanSq, width, height, steps);
    boxMean(pr, meanPr, width, height, steps);
    for (size_t i = 0; i < count; i++) {
        float variance = meanSq[i] - meanG[i] * meanG[i], covariance = meanPr[i] - meanG[i] * meanM[i];
        slope[i] = covariance / (variance + epsilon);
        offset[i] = meanM[i] - slope[i] * meanG[i];
    }
    boxMean(slope, meanSlope, width, height, steps);
    boxMean(offset, meanOffset, width, height, steps);
    std::vector<float> result(count);
    for (size_t i = 0; i < count; i++) result[i] = std::min(1.0f, std::max(0.0f, meanSlope[i] * g[i] + meanOffset[i]));
    return fromLevels(result, width, height, fullW, fullH);
}

std::shared_ptr<GrayImage> refineMatte(const GrayImage& mask, const Image& guide, const MatteSettings& raw, int limit) {
    MatteSettings s = raw.normalized();
    std::shared_ptr<GrayImage> out = s.refineEdges > 0 ? guidedRefine(mask, guide, s.refineEdges, limit) : std::make_shared<GrayImage>(mask);
    if (s.shiftEdge != 0) {
        // A blur then a hard threshold at the matching level moves the edge by the blur's reach.
        double reach = std::fabs(s.shiftEdge);
        GrayImage temp(*out);
        gaussianBlur(temp, reach / 2);
        float level = s.shiftEdge < 0 ? 0.75f : 0.25f;
        for (int y = 0; y < out->height(); y++) for (int x = 0; x < out->width(); x++) {
            float v = temp.at(x, y) / 255.0f;
            float t = std::min(1.0f, std::max(0.0f, (v - level) / 0.001f));
            out->at(x, y) = uint8_t(t * 255 + 0.5f);
        }
    }
    if (s.contrast > 0) {
        // 0 leaves the mask as it is; 100 is a hard cut at the middle.
        float strength = float(s.contrast / 100);
        float slope = 1 / std::max(0.02f, 1 - strength * 0.98f);
        for (size_t i = 0; i < out->byteCount(); i++) {
            float v = out->data()[i] / 255.0f;
            v = slope * v + (1 - slope) / 2;
            out->data()[i] = uint8_t(std::min(255.0f, std::max(0.0f, v * 255 + 0.5f)));
        }
    }
    return out;
}

} // namespace compositor
