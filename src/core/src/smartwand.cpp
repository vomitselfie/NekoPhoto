#include "compositor/smartwand.h"
#include "compositor/parallel.h"
#include "compositor/matte.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace compositor {

namespace {

/// Tolerance units per OKLab unit: a grey step of t levels near mid-grey is about t units, so tolerances
/// keep roughly the meaning they had for the plain wand.
constexpr float unitsPerLab = 255.0f * 1.25f;
constexpr int quarter = 4;   // the field stores quarter units

const std::array<float, 256>& linearTable() {
    static const std::array<float, 256> table = [] {
        std::array<float, 256> t{};
        for (int i = 0; i < 256; i++) { const double c = i / 255.0; t[size_t(i)] = float(c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4)); }
        return t;
    }();
    return table;
}

void toOklab(uint8_t r, uint8_t g, uint8_t b, float out[3]) {
    const auto& lin = linearTable();
    const float R = lin[r], G = lin[g], B = lin[b];
    const float l = std::cbrt(0.4122214708f * R + 0.5363325363f * G + 0.0514459929f * B);
    const float m = std::cbrt(0.2119034982f * R + 0.6806995451f * G + 0.1073969566f * B);
    const float s = std::cbrt(0.0883024619f * R + 0.2817188376f * G + 0.6299787005f * B);
    out[0] = 0.2104542553f * l + 0.7936177850f * m - 0.0040720468f * s;
    out[1] = 1.9779984951f * l - 2.4285922050f * m + 0.4505937099f * s;
    out[2] = 0.0259040371f * l + 0.7827717662f * m - 0.8086757660f * s;
}

/// A separable Gaussian blur of a float plane, clamped at the edges.
std::vector<float> blur(const std::vector<float>& src, int w, int h, float sigma) {
    const int r = std::max(1, int(std::ceil(sigma * 3)));
    std::vector<float> kernel(size_t(2 * r + 1));
    float sum = 0;
    for (int i = -r; i <= r; i++) { kernel[size_t(i + r)] = std::exp(-0.5f * i * i / (sigma * sigma)); sum += kernel[size_t(i + r)]; }
    for (float& k : kernel) k /= sum;
    std::vector<float> tmp(src.size()), out(src.size());
    parallelRows(0, h, [&](int ya, int yb) {
        for (int y = ya; y < yb; y++)
            for (int x = 0; x < w; x++) {
                float acc = 0;
                for (int i = -r; i <= r; i++) acc += kernel[size_t(i + r)] * src[size_t(y) * w + size_t(std::clamp(x + i, 0, w - 1))];
                tmp[size_t(y) * w + x] = acc;
            }
    }, 32);
    parallelRows(0, h, [&](int ya, int yb) {
        for (int y = ya; y < yb; y++)
            for (int x = 0; x < w; x++) {
                float acc = 0;
                for (int i = -r; i <= r; i++) acc += kernel[size_t(i + r)] * tmp[size_t(std::clamp(y + i, 0, h - 1)) * w + x];
                out[size_t(y) * w + x] = acc;
            }
    }, 32);
    return out;
}

} // namespace

SmartWandImage::SmartWandImage(const Image& pixels, bool edges) : width_(pixels.width()), height_(pixels.height()) {
    const size_t n = size_t(width_) * height_;
    lab_.resize(n * 3);
    alpha_.resize(n);
    rgb_.resize(n * 3);
    edge_.assign(n, 0);
    std::vector<float> lightness(n);
    parallelRows(0, height_, [&](int ya, int yb) {
        for (int y = ya; y < yb; y++) {
            const uint8_t* p = pixels.row(y);
            for (int x = 0; x < width_; x++, p += 4) {
                const size_t i = size_t(y) * width_ + x;
                const unsigned a = p[3];
                alpha_[i] = uint8_t(a);
                uint8_t s[3];
                for (int c = 0; c < 3; c++) { s[c] = a == 0 ? 0 : uint8_t(std::min(255u, (p[c] * 255u + a / 2) / a)); rgb_[i * 3 + size_t(c)] = s[c]; }
                float lab[3];
                toOklab(s[0], s[1], s[2], lab);
                // Transparent pixels read as their own colour but far from any opaque one through alpha.
                for (int c = 0; c < 3; c++) lab_[i * 3 + size_t(c)] = int16_t(std::lround(lab[c] * 4096));
                lightness[i] = lab[0] * (a / 255.0f);
            }
        }
    }, 32);
    // Edges at three scales: thin line art, ordinary boundaries, soft photographic ones.
    if (edges) {
    const float sigmas[3] = {0.6f, 1.5f, 3.0f}, weights[3] = {0.45f, 0.35f, 0.20f};
    std::vector<float> edge(n, 0.0f);
    for (int k = 0; k < 3; k++) {
        const std::vector<float> L = blur(lightness, width_, height_, sigmas[k]);
        parallelRows(0, height_, [&](int ya, int yb) {
            for (int y = ya; y < yb; y++)
                for (int x = 0; x < width_; x++) {
                    auto at = [&](int dx, int dy) { return L[size_t(std::clamp(y + dy, 0, height_ - 1)) * width_ + size_t(std::clamp(x + dx, 0, width_ - 1))]; };
                    const float gx = (at(1, -1) + 2 * at(1, 0) + at(1, 1) - at(-1, -1) - 2 * at(-1, 0) - at(-1, 1)) / 8;
                    const float gy = (at(-1, 1) + 2 * at(0, 1) + at(1, 1) - at(-1, -1) - 2 * at(0, -1) - at(1, -1)) / 8;
                    edge[size_t(y) * width_ + x] += weights[k] * std::sqrt(gx * gx + gy * gy);
                }
        }, 32);
    }
    for (size_t i = 0; i < n; i++) edge_[i] = uint16_t(std::min(65535.0f, edge[i] * unitsPerLab * quarter));
    }
    // Local spread: the standard deviation of OKLab over a 5 x 5 window (texture, not shading), from box sums
    // of the values and their squares, horizontal then vertical.
    spread_.assign(n, 0);
    localMean_.assign(n * 3, 0);
    std::vector<float> rowSum(n * 6);
    parallelRows(0, height_, [&](int ya, int yb) {
        for (int y = ya; y < yb; y++)
            for (int x = 0; x < width_; x++) {
                float acc[6] = {0, 0, 0, 0, 0, 0};
                for (int i = -2; i <= 2; i++) {
                    const size_t k = size_t(y) * width_ + size_t(std::clamp(x + i, 0, width_ - 1));
                    for (int c = 0; c < 3; c++) { const float v = lab_[k * 3 + size_t(c)] / 4096.0f; acc[c] += v; acc[3 + c] += v * v; }
                }
                std::memcpy(&rowSum[(size_t(y) * width_ + x) * 6], acc, sizeof acc);
            }
    }, 32);
    parallelRows(0, height_, [&](int ya, int yb) {
        for (int y = ya; y < yb; y++)
            for (int x = 0; x < width_; x++) {
                float acc[6] = {0, 0, 0, 0, 0, 0};
                for (int j = -2; j <= 2; j++) {
                    const float* r = &rowSum[(size_t(std::clamp(y + j, 0, height_ - 1)) * width_ + x) * 6];
                    for (int c = 0; c < 6; c++) acc[c] += r[c];
                }
                float var = 0;
                const size_t i = size_t(y) * width_ + x;
                for (int c = 0; c < 3; c++) {
                    const float mean = acc[c] / 25;
                    var += std::max(0.0f, acc[3 + c] / 25 - mean * mean);
                    localMean_[i * 3 + size_t(c)] = int16_t(std::lround(mean * 4096));
                }
                spread_[i] = uint16_t(std::min(65535.0f, std::sqrt(var) * unitsPerLab * quarter));
            }
    }, 32);
    // Coarser neighbourhoods: sums of the values and their squares per 4 x 4 cell, a summed-area table over
    // the cells, and each cell's window of 5 x 5 and 9 x 9 cells.
    {
        const int W = width_, H = height_;
        cellsWide_ = (W + cell - 1) / cell; cellsHigh_ = (H + cell - 1) / cell;
        const int CW = cellsWide_, CH = cellsHigh_;
        std::vector<double> sat(size_t(CW + 1) * (CH + 1) * 7, 0.0);   // six sums and the pixel count
        auto at = [&](int x, int y) -> double* { return &sat[(size_t(y) * (CW + 1) + x) * 7]; };
        {
            std::vector<double> cells(size_t(CW) * CH * 7, 0.0);
            for (int y = 0; y < H; y++)
                for (int x = 0; x < W; x++) {
                    const size_t k = size_t(y) * W + x;
                    double* c = &cells[(size_t(y / cell) * CW + size_t(x / cell)) * 7];
                    for (int ch = 0; ch < 3; ch++) { const double v = lab_[k * 3 + size_t(ch)] / 4096.0; c[ch] += v; c[3 + ch] += v * v; }
                    c[6] += 1;
                }
            for (int y = 0; y < CH; y++) {
                double row[7] = {0, 0, 0, 0, 0, 0, 0};
                for (int x = 0; x < CW; x++) {
                    const double* c = &cells[(size_t(y) * CW + x) * 7];
                    const double* up = at(x + 1, y);
                    double* here = at(x + 1, y + 1);
                    for (int k = 0; k < 7; k++) { row[k] += c[k]; here[k] = up[k] + row[k]; }
                }
            }
        }
        for (int radius : {2, 4}) {
            Scale scale;
            scale.radius = radius * cell;
            scale.mean.assign(size_t(CW) * CH * 3, 0);
            scale.spread.assign(size_t(CW) * CH, 0);
            for (int y = 0; y < CH; y++) {
                const int y0 = std::max(0, y - radius), y1 = std::min(CH, y + radius + 1);
                for (int x = 0; x < CW; x++) {
                    const int x0 = std::max(0, x - radius), x1 = std::min(CW, x + radius + 1);
                    const double *a = at(x1, y1), *b = at(x0, y1), *c = at(x1, y0), *d = at(x0, y0);
                    const double count = std::max(1.0, a[6] - b[6] - c[6] + d[6]);
                    double var = 0;
                    const size_t i = size_t(y) * CW + x;
                    for (int k = 0; k < 3; k++) {
                        const double mean = (a[k] - b[k] - c[k] + d[k]) / count;
                        var += std::max(0.0, (a[3 + k] - b[3 + k] - c[3 + k] + d[3 + k]) / count - mean * mean);
                        scale.mean[i * 3 + size_t(k)] = int16_t(std::lround(mean * 4096));
                    }
                    scale.spread[i] = uint16_t(std::min(65535.0, std::sqrt(var) * unitsPerLab * quarter));
                }
            }
            scales_.push_back(std::move(scale));
        }
    }
}

SmartWandImage::Field SmartWandImage::propagate(int seedX, int seedY, int radius, int limit, const SmartWandOptions& options, bool anywhere) const {
    Field field;
    field.width = width_; field.height = height_; field.limit = limit;
    const size_t n = size_t(width_) * height_;
    field.cost.assign(n, 65535);
    if (seedX < 0 || seedY < 0 || seedX >= width_ || seedY >= height_) return field;

    // The patch's colour model: mean and spread in OKLab, mean alpha.
    double mean[3] = {0, 0, 0}, meanSq[3] = {0, 0, 0}, alphaMean = 0;
    int samples = 0;
    for (int y = std::max(0, seedY - radius); y <= std::min(height_ - 1, seedY + radius); y++)
        for (int x = std::max(0, seedX - radius); x <= std::min(width_ - 1, seedX + radius); x++) {
            const size_t i = size_t(y) * width_ + x;
            for (int c = 0; c < 3; c++) { const double v = lab_[i * 3 + size_t(c)] / 4096.0; mean[c] += v; meanSq[c] += v * v; }
            alphaMean += alpha_[i];
            samples++;
        }
    double spread = 0;
    for (int c = 0; c < 3; c++) { mean[c] /= samples; spread += std::max(0.0, meanSq[c] / samples - mean[c] * mean[c]); }
    alphaMean /= samples;
    // Colour within one and a half of the patch's own spread is as good as the patch.
    const float slack = float(1.5 * std::sqrt(spread) * unitsPerLab);

    const int maxLevel = std::min(65534, std::max(0, limit) * quarter);
    const float seedSpread = float(std::sqrt(spread) * unitsPerLab * quarter);
    const float seedWeight = options.neighbourWeight > 0 ? float(options.seedWeight) : 1.0f;
    // The click at the coarser scales: where it is textured there (much more spread than in its patch), a
    // pixel may also be reached by matching the click's neighbourhood rather than its colour.
    // The colours such a neighbourhood is made of: two clusters (a cloth and its grid, a weave's two threads)
    // found by a few rounds of 2-means over the window; a pixel belongs to the texture only if its colour is
    // near one of them, so a ground of another colour is not mistaken for part of the pattern.
    struct Coarse { const Scale* scale; float mean[3]; float spread; float centre[2][3]; float reach[2]; };
    std::vector<Coarse> coarse;
    if (options.regionWeight > 0)
        for (const Scale& sc : scales_) {
            const size_t i = cellOf(size_t(seedY) * width_ + seedX);
            const float spreadUnits = sc.spread[i] / float(quarter);
            if (spreadUnits < 12 || spreadUnits < 3 * float(std::sqrt(spread) * unitsPerLab)) continue;
            Coarse c{&sc, {}, spreadUnits, {}, {}};
            for (int k = 0; k < 3; k++) c.mean[k] = sc.mean[i * 3 + size_t(k)] / 4096.0f;
            std::vector<std::array<float, 3>> window;
            for (int y = std::max(0, seedY - sc.radius); y <= std::min(height_ - 1, seedY + sc.radius); y++)
                for (int x = std::max(0, seedX - sc.radius); x <= std::min(width_ - 1, seedX + sc.radius); x++) {
                    const size_t k = size_t(y) * width_ + x;
                    window.push_back({lab_[k * 3] / 4096.0f, lab_[k * 3 + 1] / 4096.0f, lab_[k * 3 + 2] / 4096.0f});
                }
            // Start from the click's colour and the window colour farthest from it.
            const size_t seedIndex = size_t(seedY) * width_ + seedX;
            std::array<float, 3> centre[2] = {{lab_[seedIndex * 3] / 4096.0f, lab_[seedIndex * 3 + 1] / 4096.0f, lab_[seedIndex * 3 + 2] / 4096.0f}, {}};
            auto dist2 = [](const std::array<float, 3>& a, const std::array<float, 3>& b) { float d = 0; for (int k = 0; k < 3; k++) d += (a[k] - b[k]) * (a[k] - b[k]); return d; };
            float far = -1;
            for (auto& v : window) if (dist2(v, centre[0]) > far) { far = dist2(v, centre[0]); centre[1] = v; }
            std::vector<int> label(window.size());
            for (int round = 0; round < 6; round++) {
                double sum[2][3] = {}; int count[2] = {0, 0};
                for (size_t k = 0; k < window.size(); k++) {
                    label[k] = dist2(window[k], centre[1]) < dist2(window[k], centre[0]) ? 1 : 0;
                    for (int ch = 0; ch < 3; ch++) sum[label[k]][ch] += window[k][size_t(ch)];
                    count[label[k]]++;
                }
                for (int m = 0; m < 2; m++) if (count[m]) for (int ch = 0; ch < 3; ch++) centre[m][size_t(ch)] = float(sum[m][ch] / count[m]);
            }
            // Each cluster's reach: two and a half of its own spread, and at least 12 units.
            double var[2] = {0, 0}; int count[2] = {0, 0};
            for (size_t k = 0; k < window.size(); k++) { var[label[k]] += dist2(window[k], centre[label[k]]); count[label[k]]++; }
            for (int m = 0; m < 2; m++) {
                for (int ch = 0; ch < 3; ch++) c.centre[m][ch] = centre[m][size_t(ch)];
                c.reach[m] = std::max(12.0f, 2.5f * float(std::sqrt(count[m] ? var[m] / count[m] : 0.0)) * unitsPerLab);
            }
            coarse.push_back(c);
        }
    // The texture's way in (see Coarse): the pixel's colour on the way between the pattern's two colours, and its
    // neighbourhood textured too. `strict` refuses flat neighbourhoods outright (no path to hold a flat pixel
    // back) instead of charging for them. In quarter units; 65534 when it does not apply.
    auto textureCost = [&](size_t i) -> float {
        float best = 65534.0f;
        for (const Coarse& k : coarse) {
            // The pixel's colour must be one of the two the click's neighbourhood is made of, and its own
            // neighbourhood must match the click's: mean and spread, a Gaussian stand-in for comparing the two
            // colour distributions (a lone line beside the click differs there, a repeating pattern does not).
            float near0 = 0, near1 = 0, dm = 0;
            const size_t ci = cellOf(i);
            for (int ch = 0; ch < 3; ch++) {
                const float v = lab_[i * 3 + size_t(ch)] / 4096.0f;
                near0 += (v - k.centre[0][ch]) * (v - k.centre[0][ch]);
                near1 += (v - k.centre[1][ch]) * (v - k.centre[1][ch]);
                const float m = k.scale->mean[ci * 3 + size_t(ch)] / 4096.0f - k.mean[ch]; dm += m * m;
            }
            if (std::sqrt(near0) * unitsPerLab > k.reach[0] && std::sqrt(near1) * unitsPerLab > k.reach[1]) continue;
            const float ds = std::fabs(k.scale->spread[ci] / float(quarter) - k.spread);
            best = std::min(best, float(options.regionWeight) * (std::sqrt(dm) * unitsPerLab + ds) * quarter);
        }
        return best;
    };
    // With Contiguous off the seeds are already known to lie in look-alike regions, so filling them may cross
    // the pattern's own boundaries more freely: any colour on the way between the pattern's two colours (the
    // blends at soft cell edges), in a neighbourhood that is textured at all, whatever its proportions.
    auto patternCost = [&](size_t i) -> float {
        float best = 65534.0f;
        for (const Coarse& k : coarse) {
            float along = 0, span = 0;
            for (int ch = 0; ch < 3; ch++) {
                const float v = lab_[i * 3 + size_t(ch)] / 4096.0f;
                along += (v - k.centre[0][ch]) * (k.centre[1][ch] - k.centre[0][ch]);
                span += (k.centre[1][ch] - k.centre[0][ch]) * (k.centre[1][ch] - k.centre[0][ch]);
            }
            const float t = span > 0 ? std::clamp(along / span, 0.0f, 1.0f) : 0.0f;
            float off = 0;
            for (int ch = 0; ch < 3; ch++) {
                const float v = lab_[i * 3 + size_t(ch)] / 4096.0f, onSegment = k.centre[0][ch] + t * (k.centre[1][ch] - k.centre[0][ch]);
                off += (v - onSegment) * (v - onSegment);
            }
            const float distance = std::sqrt(off) * unitsPerLab, reach = k.reach[0] + t * (k.reach[1] - k.reach[0]);
            if (distance > reach) continue;
            const float flatness = std::max(0.0f, 0.4f * k.spread - k.scale->spread[cellOf(i)] / float(quarter));
            best = std::min(best, float(options.regionWeight) * (distance + flatness * 4) * quarter);
        }
        return best;
    };
    auto stepCost = [&](size_t from, size_t i) -> int {
        float d2 = 0, n2 = 0;
        for (int c = 0; c < 3; c++) {
            const float d = lab_[i * 3 + size_t(c)] / 4096.0f - float(mean[c]); d2 += d * d;
            const float e = (lab_[i * 3 + size_t(c)] - lab_[from * 3 + size_t(c)]) / 4096.0f; n2 += e * e;
        }
        const float colour = std::max(0.0f, std::sqrt(d2) * unitsPerLab - slack) * seedWeight;
        // A step to the next pixel, less what the click's own texture varies by (a flat click forgives nothing).
        const float neighbour = float(options.neighbourWeight) * std::max(0.0f, std::sqrt(n2) * unitsPerLab - 2 * seedSpread / quarter);
        const float alpha = float(options.alphaWeight * std::fabs(alpha_[i] - alphaMean));
        // A textured click does not flow into smoother ground: the spread it lacks against the click's.
        const float texture = float(options.textureWeight) * std::max(0.0f, seedSpread * 0.5f - spread_[i]);
        float c = (colour + neighbour + alpha) * quarter + float(options.edgeWeight) * edge_[i] + texture;
        c = std::min(c, (anywhere ? std::min(textureCost(i), patternCost(i)) : textureCost(i)) + alpha * quarter);
        return int(std::min(65534.0f, c));
    };
    const bool additive = options.accumulation == WandAccumulation::Additive;
    // Additive: a step costs its pixel's cost over four (diagonals by the square root of two), plus the toll.
    const int toll = int(std::lround(options.stepToll * quarter));
    // A bucket queue: costs are small integers, so each pixel is settled in order without a heap.
    std::vector<std::vector<uint32_t>> buckets(size_t(maxLevel) + 1);
    const size_t seed = size_t(seedY) * width_ + seedX;
    if (anywhere) {
        // Each pixel on its own: the click's colour and texture model, no step from a neighbour. With no edge to
        // stop it, the distance from the click counts in full and colour (a, b) twice: warm skin is closer to
        // white than any outline would let through.
        // The patch's mean straight colour, for the classic per-channel difference.
        float rgbMean[3] = {0, 0, 0};
        int rgbCount = 0;
        for (int y = std::max(0, seedY - radius); y <= std::min(height_ - 1, seedY + radius); y++)
            for (int x = std::max(0, seedX - radius); x <= std::min(width_ - 1, seedX + radius); x++) {
                for (int c = 0; c < 3; c++) rgbMean[c] += rgb_[(size_t(y) * width_ + x) * 3 + size_t(c)];
                rgbCount++;
            }
        for (float& v : rgbMean) v /= float(std::max(1, rgbCount));
        // With a textured click, the pattern's colours too: two clusters of the widest textured window, in sRGB.
        std::vector<std::array<float, 3>> references{{rgbMean[0], rgbMean[1], rgbMean[2]}};
        if (!coarse.empty()) {
            const int r = coarse.back().scale->radius;
            std::vector<std::array<float, 3>> window;
            for (int y = std::max(0, seedY - r); y <= std::min(height_ - 1, seedY + r); y++)
                for (int x = std::max(0, seedX - r); x <= std::min(width_ - 1, seedX + r); x++) {
                    const size_t k = (size_t(y) * width_ + x) * 3;
                    window.push_back({float(rgb_[k]), float(rgb_[k + 1]), float(rgb_[k + 2])});
                }
            std::array<float, 3> centre[2] = {references[0], references[0]};
            auto d2 = [](const std::array<float, 3>& a, const std::array<float, 3>& b) { float d = 0; for (int c = 0; c < 3; c++) d += (a[size_t(c)] - b[size_t(c)]) * (a[size_t(c)] - b[size_t(c)]); return d; };
            float far = -1;
            for (auto& v : window) if (d2(v, centre[0]) > far) { far = d2(v, centre[0]); centre[1] = v; }
            for (int round = 0; round < 6; round++) {
                double sum[2][3] = {}; int count[2] = {0, 0};
                for (auto& v : window) { const int m = d2(v, centre[1]) < d2(v, centre[0]) ? 1 : 0; for (int c = 0; c < 3; c++) sum[m][c] += v[size_t(c)]; count[m]++; }
                for (int m = 0; m < 2; m++) if (count[m]) for (int c = 0; c < 3; c++) centre[m][size_t(c)] = float(sum[m][c] / count[m]);
            }
            references.push_back(centre[0]);
            references.push_back(centre[1]);
        }
        auto ownCost = [&](size_t i) -> int {
            // The classic wand's measure, the largest channel difference: with no edge to hold it back, perceptual
            // distance is too lenient about tints (pale skin is close to white in OKLab, 40 or more levels off in
            // a channel).
            float best = 65535;
            for (auto& ref : references) {
                float worst = 0;
                for (int c = 0; c < 3; c++) worst = std::max(worst, std::fabs(rgb_[i * 3 + size_t(c)] - ref[size_t(c)]));
                best = std::min(best, worst);
            }
            const float colour = best * quarter;
            const float alpha = float(options.alphaWeight * std::fabs(alpha_[i] - alphaMean)) * quarter;
            return int(std::min(65534.0f, colour + alpha));
        };
        // Seeds only where the neighbourhood looks like the click's at the coarse scale (a checker pocket shows
        // both of its colours; an eye white beside a line does not), or, for a flat click, is flat; the path search
        // then fills each seeded region out to its edges as a click inside it would.
        const Scale* widest = scales_.empty() ? nullptr : &scales_.back();
        const size_t seedCell = cellOf(size_t(seedY) * width_ + seedX);
        const float seedWindowSpread = widest ? widest->spread[seedCell] / float(quarter) : 0;
        float seedWindowMean[3] = {0, 0, 0};
        if (widest) for (int c = 0; c < 3; c++) seedWindowMean[c] = widest->mean[seedCell * 3 + size_t(c)] / 4096.0f;
        const bool textured = !coarse.empty();
        for (size_t i = 0; i < n; i++) {
            const int own = ownCost(i);
            if (own > maxLevel) continue;
            if (widest) {
                const size_t ci = cellOf(i);
                const float spreadHere = widest->spread[ci] / float(quarter);
                if (textured) {
                    float dm = 0;
                    for (int c = 0; c < 3; c++) { const float d = widest->mean[ci * 3 + size_t(c)] / 4096.0f - seedWindowMean[c]; dm += d * d; }
                    if (std::sqrt(dm) * unitsPerLab > 0.5f * seedWindowSpread || spreadHere < 0.5f * seedWindowSpread || spreadHere > 1.6f * seedWindowSpread) continue;
                } else if (spreadHere > std::max(6.0f, 2 * seedWindowSpread)) continue;
            }
            field.cost[i] = uint16_t(own);
            buckets[size_t(own)].push_back(uint32_t(i));
        }
    }
    field.cost[seed] = 0;
    buckets[0].push_back(uint32_t(seed));
    static const int dx[8] = {1, -1, 0, 0, 1, 1, -1, -1}, dy[8] = {0, 0, 1, -1, 1, -1, 1, -1};
    for (int level = 0; level <= maxLevel; level++) {
        auto& bucket = buckets[size_t(level)];
        for (size_t k = 0; k < bucket.size(); k++) {
            const uint32_t i = bucket[k];
            if (field.cost[i] != level) continue;   // settled cheaper already
            const int x = int(i % uint32_t(width_)), y = int(i / uint32_t(width_));
            for (int d = 0; d < 8; d++) {
                const int nx = x + dx[d], ny = y + dy[d];
                if (nx < 0 || ny < 0 || nx >= width_ || ny >= height_) continue;
                const size_t j = size_t(ny) * width_ + nx;
                if (field.cost[j] <= level) continue;
                const int step = stepCost(i, j);
                int next;
                if (additive) next = level + (d < 4 ? step : step * 181 / 128) / 4 + toll;
                else next = std::max(level, step);
                if (next > maxLevel || next >= field.cost[j]) continue;
                field.cost[j] = uint16_t(next);
                buckets[size_t(next)].push_back(uint32_t(j));
            }
        }
        std::vector<uint32_t>().swap(bucket);
    }
    return field;
}

int wandCost(int tolerance) {
    tolerance = std::clamp(tolerance, 0, 255);
    if (tolerance <= 32) return tolerance;
    const double stretch = 1.0 + (tolerance - 32) / 32.0;
    return int(std::lround(32 * stretch * stretch));
}

int wandNextTolerance(const std::vector<const SmartWandImage::Field*>& positive, const std::vector<const SmartWandImage::Field*>& negative, int tolerance) {
    if (positive.empty()) return -1;
    const int inside = wandCost(tolerance) * quarter;
    int next = 65535;
    const size_t n = positive[0]->cost.size();
    for (size_t i = 0; i < n; i++) {
        int p = 65535, q = 65535;
        for (auto* f : positive) p = std::min<int>(p, f->cost[i]);
        for (auto* f : negative) q = std::min<int>(q, f->cost[i]);
        if (p > inside && p < q && p < next) next = p;
    }
    if (next == 65535) return -1;
    for (int t = tolerance + 1; t <= 255; t++) if (wandCost(t) * quarter >= next) return t;
    return -1;
}

long thresholdWandField(const SmartWandImage::Field& field, int tolerance, bool soft, GrayImage& mask) {
    const int inside = wandCost(tolerance) * quarter;
    const int band = soft ? 2 * quarter : 0;   // two tolerance levels of antialiasing just past the threshold
    long count = 0;
    for (int y = 0; y < field.height; y++) {
        uint8_t* out = mask.row(y);
        const uint16_t* c = field.cost.data() + size_t(y) * field.width;
        for (int x = 0; x < field.width; x++) {
            const int v = c[x];
            uint8_t m = 0;
            if (v <= inside) m = 255;
            else if (band && v < inside + band && v != 65535) m = uint8_t(255 - (v - inside) * 255 / band);
            out[x] = m;
            count += m >= 128;
        }
    }
    return count;
}

long thresholdWandFields(const std::vector<const SmartWandImage::Field*>& positive, const std::vector<const SmartWandImage::Field*>& negative,
                         int tolerance, bool soft, GrayImage& mask) {
    if (positive.empty()) { std::memset(mask.data(), 0, mask.byteCount()); return 0; }
    const int width = positive[0]->width, height = positive[0]->height;
    const int inside = wandCost(tolerance) * quarter;
    const int band = soft ? 2 * quarter : 0;
    long count = 0;
    for (int y = 0; y < height; y++) {
        uint8_t* out = mask.row(y);
        for (int x = 0; x < width; x++) {
            const size_t i = size_t(y) * width + x;
            int p = 65535, n = 65535;
            for (auto* f : positive) p = std::min<int>(p, f->cost[i]);
            for (auto* f : negative) n = std::min<int>(n, f->cost[i]);
            uint8_t m = 0;
            if (p != 65535 && p < n) {
                if (p <= inside) m = 255;
                else if (band && p < inside + band) m = uint8_t(255 - (p - inside) * 255 / band);
            }
            out[x] = m;
            count += m >= 128;
        }
    }
    return count;
}

void refineWandEdge(const Image& pixels, GrayImage& mask, int band, std::vector<uint32_t>* lineColours) {
    const int W = pixels.width(), H = pixels.height();
    if (W != mask.width() || H != mask.height() || band <= 0) return;
    // Which side of the edge each pixel is on, and whether it is within `band` of it (a square neighbourhood
    // holding both sides).
    std::vector<uint8_t> inside(size_t(W) * H), near(size_t(W) * H, 0);
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) inside[size_t(y) * W + x] = mask.at(x, y) >= 128;
    {
        // Row then column: the distance, in squares, to the nearest pixel on the other side.
        std::vector<int> rowNear(size_t(W) * H, band + 1);
        parallelRows(0, H, [&](int ya, int yb) {
            for (int y = ya; y < yb; y++)
                for (int x = 0; x < W; x++) {
                    const uint8_t me = inside[size_t(y) * W + x];
                    int d = band + 1;
                    for (int k = 0; k <= band && d > band; k++) {
                        if (x - k >= 0 && inside[size_t(y) * W + x - k] != me) d = k;
                        else if (x + k < W && inside[size_t(y) * W + x + k] != me) d = k;
                    }
                    rowNear[size_t(y) * W + x] = d;
                }
        }, 32);
        // A pixel is near the edge when some pixel within `band` rows has an opposite-side pixel within `band` columns.
        parallelRows(0, H, [&](int ya, int yb) {
            for (int y = ya; y < yb; y++)
                for (int x = 0; x < W; x++) {
                    const uint8_t me = inside[size_t(y) * W + x];
                    bool found = false;
                    for (int j = -band; j <= band && !found; j++) {
                        const int Y = y + j;
                        if (Y < 0 || Y >= H) continue;
                        // Same-side rows carry the distance to their own other side; an opposite-side pixel counts at 0.
                        if (inside[size_t(Y) * W + x] != me || rowNear[size_t(Y) * W + x] <= band) found = true;
                    }
                    near[size_t(y) * W + x] = found;
                }
        }, 32);
    }
    auto straight = [&](int x, int y, float out[3]) {
        const uint8_t* p = pixels.pixel(x, y);
        const float a = p[3];
        for (int c = 0; c < 3; c++) out[c] = a > 0 ? p[c] * 255.0f / a : 0.0f;
    };
    const int R = band + 3;
    GrayImage refined = mask;
    if (lineColours) lineColours->assign(size_t(W) * H, 0);
    // First, per edge pixel: the selected colour near it (B, from sure selected pixels) and the other colour
    // (F, the fifth of unselected pixels farthest from B: the line's core, which a thin line has no sure pixels
    // of).
    struct Local { float B[3], F[3]; bool ok = false; };
    std::vector<Local> local(size_t(W) * H);
    parallelRows(0, H, [&](int ya, int yb) {
        for (int y = ya; y < yb; y++)
            for (int x = 0; x < W; x++) {
                if (!near[size_t(y) * W + x]) continue;
                Local& L = local[size_t(y) * W + x];
                float B[3] = {0, 0, 0};
                int nb = 0;
                for (int j = -R; j <= R; j++) for (int i = -R; i <= R; i++) {
                    const int X = x + i, Y = y + j;
                    if (X < 0 || Y < 0 || X >= W || Y >= H) continue;
                    const size_t k = size_t(Y) * W + X;
                    if (!inside[k] || near[k]) continue;
                    float c[3]; straight(X, Y, c);
                    for (int ch = 0; ch < 3; ch++) B[ch] += c[ch];
                    nb++;
                }
                if (nb == 0) continue;
                for (float& v : B) v /= float(nb);
                std::vector<std::pair<float, std::array<float, 3>>> others;
                for (int j = -R; j <= R; j++) for (int i = -R; i <= R; i++) {
                    const int X = x + i, Y = y + j;
                    if (X < 0 || Y < 0 || X >= W || Y >= H || inside[size_t(Y) * W + X]) continue;
                    std::array<float, 3> c; straight(X, Y, c.data());
                    float d = 0; for (int ch = 0; ch < 3; ch++) d += (c[size_t(ch)] - B[ch]) * (c[size_t(ch)] - B[ch]);
                    others.push_back({d, c});
                }
                if (others.empty()) continue;
                const size_t keep = std::max<size_t>(1, others.size() / 5);
                std::partial_sort(others.begin(), others.begin() + long(keep), others.end(), [](const auto& p, const auto& q) { return p.first > q.first; });
                for (int ch = 0; ch < 3; ch++) { L.B[ch] = B[ch]; L.F[ch] = 0; }
                for (size_t k = 0; k < keep; k++) for (int ch = 0; ch < 3; ch++) L.F[ch] += others[k].second[size_t(ch)] / float(keep);
                L.ok = true;
            }
    }, 16);
    // The line's own colour over the whole edge: the local colours farthest from their background (the cores of
    // the thicker stretches), the most distinct twentieth, averaged.
    float G[3] = {0, 0, 0};
    bool haveGlobal = false;
    {
        std::vector<std::pair<float, std::array<float, 3>>> all;
        for (const Local& L : local) {
            if (!L.ok) continue;
            float d = 0; for (int ch = 0; ch < 3; ch++) d += (L.F[ch] - L.B[ch]) * (L.F[ch] - L.B[ch]);
            all.push_back({d, {L.F[0], L.F[1], L.F[2]}});
        }
        if (!all.empty()) {
            const size_t keep = std::max<size_t>(1, all.size() / 20);
            std::partial_sort(all.begin(), all.begin() + long(keep), all.end(), [](const auto& p, const auto& q) { return p.first > q.first; });
            for (size_t k = 0; k < keep; k++) for (int ch = 0; ch < 3; ch++) G[ch] += all[k].second[size_t(ch)] / float(keep);
            haveGlobal = true;
        }
    }
    parallelRows(0, H, [&](int ya, int yb) {
        for (int y = ya; y < yb; y++)
            for (int x = 0; x < W; x++) {
                const Local& L = local[size_t(y) * W + x];
                if (!L.ok) continue;
                float F[3] = {L.F[0], L.F[1], L.F[2]};
                // A faint stretch of the same line: its local colour lies on the way from the background to the
                // line's own colour. Unmix against the line's own colour then (a lower alpha, a truer colour).
                if (haveGlobal) {
                    float bg2 = 0, t = 0;
                    for (int ch = 0; ch < 3; ch++) { bg2 += (G[ch] - L.B[ch]) * (G[ch] - L.B[ch]); t += (L.F[ch] - L.B[ch]) * (G[ch] - L.B[ch]); }
                    if (bg2 > 0) {
                        t /= bg2;
                        float off = 0;
                        for (int ch = 0; ch < 3; ch++) { const float onLine = L.B[ch] + t * (G[ch] - L.B[ch]); off += (L.F[ch] - onLine) * (L.F[ch] - onLine); }
                        if (t > 0.2f && t < 1.05f && std::sqrt(off) < 25.0f) for (int ch = 0; ch < 3; ch++) F[ch] = G[ch];
                    }
                }
                float BF2 = 0; for (int ch = 0; ch < 3; ch++) BF2 += (L.B[ch] - F[ch]) * (L.B[ch] - F[ch]);
                if (BF2 < 20.0f * 20.0f) continue;   // too alike to unmix
                float P[3]; straight(x, y, P);
                const float alpha = pixels.pixel(x, y)[3] / 255.0f;
                float t = 0; for (int ch = 0; ch < 3; ch++) t += (P[ch] - F[ch]) * (L.B[ch] - F[ch]);
                const float a = std::clamp(t / BF2, 0.0f, 1.0f) * alpha + (1 - alpha) * (inside[size_t(y) * W + x] ? 1.0f : 0.0f);
                refined.at(x, y) = uint8_t(std::lround(a * 255));
                if (lineColours) {
                    const auto q = [](float v) { return uint32_t(std::clamp(std::lround(v), 0L, 255L)); };
                    (*lineColours)[size_t(y) * W + x] = 0x1000000u | (q(F[0]) << 16) | (q(F[1]) << 8) | q(F[2]);
                }
            }
    }, 16);
    mask = std::move(refined);
}

void clearDecontaminated(Image& pixels, const GrayImage& coverage, const std::vector<uint32_t>* lineColours) {
    const int W = pixels.width(), H = pixels.height();
    if (coverage.width() != W || coverage.height() != H) return;
    // What stays is 1 - coverage of each pixel; its colour, where only part stays, is the estimated colour of
    // that part alone.
    GrayImage kept(W, H, 0);
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) kept.at(x, y) = uint8_t(std::lround((255 - coverage.at(x, y)) * pixels.pixel(x, y)[3] / 255.0));
    // The general estimate only where a half-cleared pixel has no colour from the unmixing.
    const bool haveColours = lineColours && lineColours->size() == size_t(W) * H;
    bool needEstimate = false;
    for (int y = 0; y < H && !needEstimate; y++) for (int x = 0; x < W; x++) {
        const unsigned c = coverage.at(x, y);
        if (c > 0 && c < 255 && !(haveColours && (*lineColours)[size_t(y) * W + x])) { needEstimate = true; break; }
    }
    auto foreground = needEstimate ? estimateForeground(pixels, kept) : nullptr;
    parallelRows(0, H, [&](int ya, int yb) {
        for (int y = ya; y < yb; y++)
            for (int x = 0; x < W; x++) {
                const unsigned c = coverage.at(x, y);
                if (c == 0) continue;
                uint8_t* p = pixels.pixel(x, y);
                if (c == 255) { p[0] = p[1] = p[2] = p[3] = 0; continue; }
                const unsigned a = kept.at(x, y);
                const uint32_t known = lineColours && lineColours->size() == size_t(W) * H ? (*lineColours)[size_t(y) * W + x] : 0;
                if (known) {
                    // The line's own colour, from the unmixing, at the alpha that is left.
                    p[0] = uint8_t(((known >> 16) & 0xff) * a / 255); p[1] = uint8_t(((known >> 8) & 0xff) * a / 255); p[2] = uint8_t((known & 0xff) * a / 255);
                    p[3] = uint8_t(a);
                    continue;
                }
                const uint8_t* f = foreground ? foreground->pixel(x, y) : p;
                // The estimate's straight colour at the kept alpha, premultiplied.
                const unsigned fa = f[3];
                for (int k = 0; k < 3; k++) p[k] = uint8_t(fa ? std::min(255u, (f[k] * 255u / fa) * a / 255u) : 0);
                p[3] = uint8_t(a);
            }
    }, 32);
}

} // namespace compositor
