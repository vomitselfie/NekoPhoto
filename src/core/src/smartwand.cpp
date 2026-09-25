#include "compositor/smartwand.h"
#include "compositor/parallel.h"
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
                for (int c = 0; c < 3; c++) s[c] = a == 0 ? 0 : uint8_t(std::min(255u, (p[c] * 255u + a / 2) / a));
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

SmartWandImage::Field SmartWandImage::propagate(int seedX, int seedY, int radius, int limit, const SmartWandOptions& options) const {
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

    const int maxLevel = std::min(65534, limit * quarter);
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
        for (const Coarse& k : coarse) {
            // The pixel's colour must be one of the two the click's neighbourhood is made of, and its own
            // neighbourhood must match the click's: mean and spread, a Gaussian stand-in for comparing the two
            // colour distributions.
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
            const float region = float(options.regionWeight) * (std::sqrt(dm) * unitsPerLab + ds) + alpha;
            c = std::min(c, region * quarter);
        }
        return int(std::min(65534.0f, c));
    };
    const bool additive = options.accumulation == WandAccumulation::Additive;
    // Additive: a step costs its pixel's cost over four (diagonals by the square root of two), plus the toll.
    const int toll = int(std::lround(options.stepToll * quarter));
    // A bucket queue: costs are small integers, so each pixel is settled in order without a heap.
    std::vector<std::vector<uint32_t>> buckets(size_t(maxLevel) + 1);
    const size_t seed = size_t(seedY) * width_ + seedX;
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

long thresholdWandField(const SmartWandImage::Field& field, int tolerance, bool soft, GrayImage& mask) {
    const int inside = std::max(0, tolerance) * quarter;
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
    const int inside = std::max(0, tolerance) * quarter;
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

} // namespace compositor
