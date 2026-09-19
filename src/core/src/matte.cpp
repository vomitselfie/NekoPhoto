// Matte refinement for Remove Background: a guided filter pulls the model's coarse mask onto the
// image's own edges (hair, fur), Shift Edge moves the whole edge, Contrast hardens it.
//
// Matting (Gastal & Oliveira 2010, shared sampling) then solves the true opacity in a band around the
// edge from foreground and background colour samples, for hair against a busy background.
//
// The filter is He, Sun & Tang's guided filter in its colour form, with the layer's R, G, B as the
// guide, so colour edges invisible in luma still steer the matte; the regularisation is edge-aware
// (Li et al.'s weighted guided filter), so flat background stays smooth while edges keep their detail;
// and, as in the fast guided filter (He & Sun 2015), the coefficients are computed on a subsampled grid
// and upsampled, then applied to the full-resolution guide.
#include "compositor/subject.h"
#include "compositor/blur.h"
#include "compositor/filters.h"
#include "compositor/parallel.h"
#include "compositor/render.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace compositor {

MatteSettings MatteSettings::normalized() const {
    auto c = [](double v, double lo, double hi, double f) { return std::isfinite(v) ? std::min(hi, std::max(lo, v)) : f; };
    return {c(refineEdges, 0, 40, 12), c(contrast, 0, 100, 25), c(shiftEdge, -10, 10, 0), c(matting, 0, 40, 0)};
}

namespace {

using Map = std::vector<float>;

/// Mean over a (2r+1)^2 square, clamp-to-edge, as two running-sum passes; the cost is independent of r.
void boxMean(const Map& src, Map& out, int width, int height, int radius) {
    const float inv = 1.0f / float(radius * 2 + 1);
    Map pass(size_t(width) * height);
    parallelRows(0, height, [&](int y0, int y1) {
        for (int y = y0; y < y1; y++) {
            const float* row = &src[size_t(y) * width];
            float* o = &pass[size_t(y) * width];
            float sum = 0;
            for (int x = -radius; x <= radius; x++) sum += row[std::clamp(x, 0, width - 1)];
            for (int x = 0; x < width; x++) {
                o[x] = sum * inv;
                sum -= row[std::clamp(x - radius, 0, width - 1)];
                sum += row[std::clamp(x + radius + 1, 0, width - 1)];
            }
        }
    });
    out.assign(size_t(width) * height, 0);
    const int band = 64, bands = (width + band - 1) / band;
    parallelRows(0, bands, [&](int b0, int b1) {
        std::vector<float> sum(static_cast<size_t>(band));
        for (int b = b0; b < b1; b++) {
            const int x0 = b * band, n = std::min(band, width - x0);
            std::fill(sum.begin(), sum.begin() + n, 0.0f);
            for (int y = -radius; y <= radius; y++) { const float* p = &pass[size_t(std::clamp(y, 0, height - 1)) * width + x0]; for (int j = 0; j < n; j++) sum[size_t(j)] += p[j]; }
            for (int y = 0; y < height; y++) {
                float* o = &out[size_t(y) * width + x0];
                for (int j = 0; j < n; j++) o[j] = sum[size_t(j)] * inv;
                const float* leave = &pass[size_t(std::clamp(y - radius, 0, height - 1)) * width + x0];
                const float* enter = &pass[size_t(std::clamp(y + radius + 1, 0, height - 1)) * width + x0];
                for (int j = 0; j < n; j++) sum[size_t(j)] += enter[j] - leave[j];
            }
        }
    }, 1);
}

/// Bilinear upsample of a float map from (sw, sh) to (dw, dh).
Map upsample(const Map& src, int sw, int sh, int dw, int dh) {
    if (sw == dw && sh == dh) return src;
    Map out(size_t(dw) * dh);
    const double fx = double(sw) / dw, fy = double(sh) / dh;
    parallelRows(0, dh, [&](int y0, int y1) {
        for (int y = y0; y < y1; y++) {
            double sy = std::clamp((y + 0.5) * fy - 0.5, 0.0, double(sh - 1));
            int iy = int(sy), iy1 = std::min(iy + 1, sh - 1);
            float wy = float(sy - iy);
            const float *r0 = &src[size_t(iy) * sw], *r1 = &src[size_t(iy1) * sw];
            float* o = &out[size_t(y) * dw];
            for (int x = 0; x < dw; x++) {
                double sx = std::clamp((x + 0.5) * fx - 0.5, 0.0, double(sw - 1));
                int ix = int(sx), ix1 = std::min(ix + 1, sw - 1);
                float wx = float(sx - ix);
                o[x] = (r0[ix] * (1 - wx) + r0[ix1] * wx) * (1 - wy) + (r1[ix] * (1 - wx) + r1[ix1] * wx) * wy;
            }
        }
    });
    return out;
}

/// The guide's straight R, G, B in 0..1 at (width, height).
std::array<Map, 3> colourLevels(const Image& image, int width, int height) {
    std::shared_ptr<const Image> source = std::make_shared<Image>(image);
    if (width != image.width() || height != image.height()) {
        LayerTransform full(Point(0, 0), Size(image.width(), image.height()));
        source = resampleLayer(image, full, full, width, height);
    }
    std::array<Map, 3> out;
    for (auto& m : out) m.resize(size_t(width) * height);
    parallelRows(0, height, [&](int y0, int y1) {
        for (int y = y0; y < y1; y++) {
            const uint8_t* p = source->row(y);
            for (int x = 0; x < width; x++, p += 4) {
                float a = p[3] ? p[3] / 255.0f : 1.0f;
                for (int c = 0; c < 3; c++) out[size_t(c)][size_t(y) * width + size_t(x)] = p[c] / 255.0f / a;
            }
        }
    });
    return out;
}

Map maskLevels(const GrayImage& mask, int width, int height) {
    std::shared_ptr<const GrayImage> source = std::make_shared<GrayImage>(mask);
    if (width != mask.width() || height != mask.height()) {
        LayerTransform full(Point(0, 0), Size(mask.width(), mask.height()));
        source = resampleMask(mask, full, full, width, height, 0);
    }
    Map out(size_t(width) * height);
    for (int y = 0; y < height; y++) for (int x = 0; x < width; x++) out[size_t(y) * width + size_t(x)] = source->at(x, y) / 255.0f;
    return out;
}

std::shared_ptr<GrayImage> fromLevels(const Map& levels, int width, int height, int fullWidth, int fullHeight) {
    auto small = std::make_shared<GrayImage>(width, height);
    for (int y = 0; y < height; y++) for (int x = 0; x < width; x++) small->at(x, y) = uint8_t(std::min(255.0f, std::max(0.0f, levels[size_t(y) * width + size_t(x)] * 255 + 0.5f)));
    if (width == fullWidth && height == fullHeight) return small;
    LayerTransform full(Point(0, 0), Size(fullWidth, fullHeight));
    return resampleMask(*small, full, full, fullWidth, fullHeight, 0);
}

/// Solves the N x N system A x = b (a covariance matrix plus regularisation) in place by Gaussian elimination.
template <int N>
void solve(double A[N][N], double b[N], float out[N]) {
    for (int c = 0; c < N; c++) {
        int pivot = c;
        for (int r = c + 1; r < N; r++) if (std::fabs(A[r][c]) > std::fabs(A[pivot][c])) pivot = r;
        if (pivot != c) { std::swap(A[c], A[pivot]); std::swap(b[c], b[pivot]); }
        double d = A[c][c];
        if (std::fabs(d) < 1e-12) { for (int k = 0; k < N; k++) out[k] = 0; return; }
        for (int r = c + 1; r < N; r++) {
            double f = A[r][c] / d;
            if (f == 0) continue;
            for (int k = c; k < N; k++) A[r][k] -= f * A[c][k];
            b[r] -= f * b[c];
        }
    }
    for (int c = N - 1; c >= 0; c--) {
        double v = b[c];
        for (int k = c + 1; k < N; k++) v -= A[c][k] * out[k];
        out[c] = float(v / A[c][c]);
    }
}

/// Running max (or min) over a window of 2r+1 along each row, then each column: van Herk / Gil-Werman, O(N).
void runningExtreme(GrayImage& image, int r, bool takeMax) {
    const int w = image.width(), h = image.height(), n = 2 * r + 1;
    auto pick = [takeMax](uint8_t a, uint8_t b) { return takeMax ? std::max(a, b) : std::min(a, b); };
    const uint8_t pad = takeMax ? 0 : 255;
    auto pass = [&](int length, auto get, auto set) {
        // Prefix extremes within blocks of n and suffix extremes within blocks; the window max is their combination.
        const size_t count = static_cast<size_t>(length);
        std::vector<uint8_t> prefix(count), suffix(count), values(count);
        for (int i = 0; i < length; i++) values[size_t(i)] = get(i);
        for (int start = 0; start < length; start += n) {
            int end = std::min(length, start + n);
            prefix[size_t(start)] = values[size_t(start)];
            for (int i = start + 1; i < end; i++) prefix[size_t(i)] = pick(prefix[size_t(i - 1)], values[size_t(i)]);
            suffix[size_t(end - 1)] = values[size_t(end - 1)];
            for (int i = end - 2; i >= start; i--) suffix[size_t(i)] = pick(suffix[size_t(i + 1)], values[size_t(i)]);
        }
        for (int i = 0; i < length; i++) {
            int lo = i - r, hi = i + r;
            uint8_t v = pad;
            if (lo >= 0 && hi < length) v = pick(suffix[size_t(lo)], prefix[size_t(hi)]);
            else for (int k = std::max(0, lo); k <= std::min(length - 1, hi); k++) v = pick(v, values[size_t(k)]);
            set(i, v);
        }
    };
    parallelRows(0, h, [&](int y0, int y1) {
        for (int y = y0; y < y1; y++) {
            uint8_t* row = image.row(y);
            pass(w, [&](int i) { return row[i]; }, [&](int i, uint8_t v) { row[i] = v; });
        }
    });
    parallelRows(0, w, [&](int x0, int x1) {
        for (int x = x0; x < x1; x++) pass(h, [&](int i) { return image.at(x, i); }, [&](int i, uint8_t v) { image.at(x, i) = v; });
    }, 16);
}

} // namespace

std::shared_ptr<GrayImage> guidedRefine(const GrayImage& mask, const Image& guide, double radius, int limit) {
    constexpr int C = 3;   // guide channels: R, G, B
    const int fullW = mask.width(), fullH = mask.height();
    const double factor = limit > 0 ? std::min(1.0, double(limit) / std::max(fullW, fullH)) : 1;
    const int width = std::max(1, int(std::lround(fullW * factor))), height = std::max(1, int(std::lround(fullH * factor)));
    // Coefficients on a coarser grid for large images (the fast guided filter); the output uses the full guide.
    const int longest = std::max(width, height);
    const int sub = longest > 1536 ? 4 : longest > 768 ? 2 : 1;
    const int lowW = std::max(1, (width + sub - 1) / sub), lowH = std::max(1, (height + sub - 1) / sub);
    const int r = std::max(1, int(std::lround(radius * factor / sub)));

    std::array<Map, C> guideFull = colourLevels(guide, width, height);
    std::array<Map, C> I = sub == 1 ? guideFull : colourLevels(guide, lowW, lowH);   // the guide at the coefficient grid
    Map p = maskLevels(mask, lowW, lowH);                                              // the mask being filtered
    const size_t count = size_t(lowW) * lowH;

    // Means and second moments over the window.
    std::array<Map, C> meanI, meanIp;
    Map meanP, meanII[C][C], product(count);
    for (int c = 0; c < C; c++) boxMean(I[size_t(c)], meanI[size_t(c)], lowW, lowH, r);
    boxMean(p, meanP, lowW, lowH, r);
    for (int u = 0; u < C; u++)
        for (int v = u; v < C; v++) {
            for (size_t i = 0; i < count; i++) product[i] = I[size_t(u)][i] * I[size_t(v)][i];
            boxMean(product, meanII[u][v], lowW, lowH, r);
        }
    for (int c = 0; c < C; c++) {
        for (size_t i = 0; i < count; i++) product[i] = I[size_t(c)][i] * p[i];
        boxMean(product, meanIp[size_t(c)], lowW, lowH, r);
    }
    // Edge-aware regularisation: windows on strong edges get a small epsilon (follow the guide closely),
    // flat ones a large one (smooth), from the guide's 3x3 luma variance (the weighted guided filter).
    Map luma(count), lumaMean, lumaSqMean, gamma(count);
    for (size_t i = 0; i < count; i++) { luma[i] = 0.299f * I[0][i] + 0.587f * I[1][i] + 0.114f * I[2][i]; product[i] = luma[i] * luma[i]; }
    boxMean(luma, lumaMean, lowW, lowH, 1);
    boxMean(product, lumaSqMean, lowW, lowH, 1);
    const float delta = 1e-4f;
    double meanInverse = 0;
    for (size_t i = 0; i < count; i++) { float v = std::max(0.0f, lumaSqMean[i] - lumaMean[i] * lumaMean[i]) + delta; gamma[i] = v; meanInverse += 1.0 / v; }
    meanInverse /= double(std::max<size_t>(1, count));
    const float lambda = 1e-4f;

    // Per window: q = a . I + b with a = (cov(I, I) + eps)^-1 cov(I, p), b = mean(p) - a . mean(I).
    std::array<Map, C> a;
    for (auto& m : a) m.resize(count);
    Map b(count);
    parallelRows(0, lowH, [&](int y0, int y1) {
        for (int y = y0; y < y1; y++)
            for (int x = 0; x < lowW; x++) {
                size_t i = size_t(y) * lowW + size_t(x);
                float epsilon = std::clamp(lambda / float(gamma[i] * meanInverse), 1e-6f, 1e-2f);
                double A[C][C], rhs[C];
                for (int u = 0; u < C; u++) {
                    for (int v = u; v < C; v++) A[u][v] = A[v][u] = meanII[u][v][i] - meanI[size_t(u)][i] * meanI[size_t(v)][i];
                    A[u][u] += epsilon;
                    rhs[u] = meanIp[size_t(u)][i] - meanI[size_t(u)][i] * meanP[i];
                }
                float coefficients[C];
                solve<C>(A, rhs, coefficients);
                float offset = meanP[i];
                for (int c = 0; c < C; c++) { a[size_t(c)][i] = coefficients[c]; offset -= coefficients[c] * meanI[size_t(c)][i]; }
                b[i] = offset;
            }
    });
    std::array<Map, C> meanA;
    Map meanB;
    for (int c = 0; c < C; c++) boxMean(a[size_t(c)], meanA[size_t(c)], lowW, lowH, r);
    boxMean(b, meanB, lowW, lowH, r);

    // Apply at the working resolution with the full guide.
    std::array<Map, C> up;
    for (int c = 0; c < C; c++) up[size_t(c)] = upsample(meanA[size_t(c)], lowW, lowH, width, height);
    Map upB = upsample(meanB, lowW, lowH, width, height);
    Map result(size_t(width) * height);
    parallelRows(0, height, [&](int y0, int y1) {
        for (int y = y0; y < y1; y++)
            for (int x = 0; x < width; x++) {
                size_t i = size_t(y) * width + size_t(x);
                float q = upB[i];
                for (int c = 0; c < C; c++) q += up[size_t(c)][i] * guideFull[size_t(c)][i];
                result[i] = std::clamp(q, 0.0f, 1.0f);
            }
    });
    return fromLevels(result, width, height, fullW, fullH);
}

namespace {

/// A foreground or background colour sample found along a ray, with its distance from the pixel.
struct Sample { float r, g, b, distance; };

/// The best (foreground, background) pair for a pixel's colour: the opacity it implies and how badly it
/// explains the colour, with a small preference for nearby samples and well-separated pairs.
struct Pair { float fr = 0, fg = 0, fb = 0, br = 0, bg = 0, bb = 0, alpha = 0, cost = 1e9f; };

inline void score(const float colour[3], float fr, float fg, float fb, float br, float bg, float bb, float spatial, Pair& best) {
    const float dr = fr - br, dg = fg - bg, db = fb - bb;
    const float separation = dr * dr + dg * dg + db * db;
    float alpha = separation > 1e-6f ? ((colour[0] - br) * dr + (colour[1] - bg) * dg + (colour[2] - bb) * db) / separation : 0.5f;
    alpha = std::clamp(alpha, 0.0f, 1.0f);
    const float er = colour[0] - (alpha * fr + (1 - alpha) * br), eg = colour[1] - (alpha * fg + (1 - alpha) * bg), eb = colour[2] - (alpha * fb + (1 - alpha) * bb);
    const float chroma = std::sqrt(er * er + eg * eg + eb * eb);
    const float cost = chroma / std::max(0.05f, std::sqrt(separation)) + spatial;
    if (cost < best.cost) best = {fr, fg, fb, br, bg, bb, alpha, cost};
}

} // namespace

std::shared_ptr<GrayImage> matteBand(const GrayImage& matte, const Image& guide, double bandFull, int limit) {
    const int fullW = matte.width(), fullH = matte.height();
    const double factor = limit > 0 ? std::min(1.0, double(limit) / std::max(fullW, fullH)) : 1;
    const int width = std::max(1, int(std::lround(fullW * factor))), height = std::max(1, int(std::lround(fullH * factor)));
    const int band = std::max(1, int(std::lround(bandFull * factor)));
    std::array<Map, 3> colour = colourLevels(guide, width, height);
    Map levels = maskLevels(matte, width, height);
    // The trimap: sure foreground is the matte eroded by the band, sure background the eroded complement
    // (running min and max, O(N)); everything else is unknown and gets solved.
    GrayImage eroded(width, height), dilated(width, height);
    for (int y = 0; y < height; y++) for (int x = 0; x < width; x++) eroded.at(x, y) = dilated.at(x, y) = levels[size_t(y) * width + size_t(x)] >= 0.5f ? 255 : 0;
    runningExtreme(eroded, band, false);
    runningExtreme(dilated, band, true);
    enum Region : uint8_t { Unknown = 0, Foreground = 1, Background = 2 };
    std::vector<uint8_t> region(size_t(width) * height, Unknown);
    std::vector<int32_t> slot(region.size(), -1);   // each unknown pixel's index among the unknowns
    int32_t unknowns = 0;
    for (size_t i = 0; i < region.size(); i++) {
        if (eroded.data()[i]) region[i] = Foreground;
        else if (!dilated.data()[i]) region[i] = Background;
        else slot[i] = unknowns++;
    }
    auto at = [&](int x, int y) { return size_t(y) * width + size_t(x); };
    auto colourAt = [&](size_t i, float out[3]) { out[0] = colour[0][i]; out[1] = colour[1][i]; out[2] = colour[2][i]; };

    // Sampling: rays from each unknown pixel into both sure regions, then the best pair.
    constexpr int rays = 6;
    const int reach = 3 * band + 4;
    std::vector<Pair> pairs(static_cast<size_t>(unknowns));
    parallelRows(0, height, [&](int y0, int y1) {
        Sample fg[rays], bg[rays];
        for (int y = y0; y < y1; y++)
            for (int x = 0; x < width; x++) {
                const size_t p = at(x, y);
                if (region[p] != Unknown) continue;
                int nf = 0, nb = 0;
                const double start = ((x * 7 + y * 13) % rays) * (2 * M_PI / rays) / rays;   // decorrelate neighbours
                for (int k = 0; k < rays; k++) {
                    const double angle = start + k * (2 * M_PI / rays), dx = std::cos(angle), dy = std::sin(angle);
                    bool haveF = false, haveB = false;
                    for (int step = 1; step <= reach && !(haveF && haveB); step++) {
                        const int sx = int(std::lround(x + dx * step)), sy = int(std::lround(y + dy * step));
                        if (sx < 0 || sy < 0 || sx >= width || sy >= height) break;
                        const size_t q = at(sx, sy);
                        if (region[q] == Foreground && !haveF) { fg[nf++] = {colour[0][q], colour[1][q], colour[2][q], float(step)}; haveF = true; }
                        else if (region[q] == Background && !haveB) { bg[nb++] = {colour[0][q], colour[1][q], colour[2][q], float(step)}; haveB = true; }
                    }
                }
                if (!nf || !nb) continue;
                float c[3];
                colourAt(p, c);
                Pair best;
                for (int i = 0; i < nf; i++)
                    for (int j = 0; j < nb; j++)
                        score(c, fg[i].r, fg[i].g, fg[i].b, bg[j].r, bg[j].g, bg[j].b, 0.02f * (fg[i].distance + bg[j].distance) / float(band), best);
                pairs[size_t(slot[p])] = best;
            }
    }, 8);
    // Sharing: a neighbour's pair that explains this pixel better is taken over (two passes).
    for (int pass = 0; pass < 2; pass++) {
        std::vector<Pair> shared = pairs;
        parallelRows(0, height, [&](int y0, int y1) {
            for (int y = y0; y < y1; y++)
                for (int x = 0; x < width; x++) {
                    const size_t p = at(x, y);
                    if (region[p] != Unknown) continue;
                    float c[3];
                    colourAt(p, c);
                    Pair best = pairs[size_t(slot[p])];
                    for (int j = -1; j <= 1; j++)
                        for (int i = -1; i <= 1; i++) {
                            if ((!i && !j) || x + i < 0 || y + j < 0 || x + i >= width || y + j >= height) continue;
                            const int32_t ns = slot[at(x + i, y + j)];
                            if (ns < 0) continue;
                            const Pair& n = pairs[size_t(ns)];
                            if (n.cost >= 1e9f) continue;
                            score(c, n.fr, n.fg, n.fb, n.br, n.bg, n.bb, 0, best);
                        }
                    shared[size_t(slot[p])] = best;
                }
        }, 8);
        pairs.swap(shared);
    }
    // Smoothing: opacities averaged over a small window, weighted by confidence and colour similarity.
    constexpr int radius = 3;
    const float chromaScale = 0.1f, colourScale = 0.1f;
    Map result = levels;
    parallelRows(0, height, [&](int y0, int y1) {
        for (int y = y0; y < y1; y++)
            for (int x = 0; x < width; x++) {
                const size_t p = at(x, y);
                if (region[p] != Unknown) { result[p] = region[p] == Foreground ? 1.0f : 0.0f; continue; }
                if (pairs[size_t(slot[p])].cost >= 1e9f) continue;   // no samples reached: the matte as it was
                float c[3];
                colourAt(p, c);
                double sum = 0, weight = 0;
                for (int j = -radius; j <= radius; j++)
                    for (int i = -radius; i <= radius; i++) {
                        const int sx = x + i, sy = y + j;
                        if (sx < 0 || sy < 0 || sx >= width || sy >= height) continue;
                        const size_t q = at(sx, sy);
                        float alpha, confidence;
                        if (region[q] == Unknown) { const Pair& n = pairs[size_t(slot[q])]; if (n.cost >= 1e9f) continue; alpha = n.alpha; confidence = n.cost; }
                        else { alpha = region[q] == Foreground ? 1.0f : 0.0f; confidence = 0; }
                        float n[3];
                        colourAt(q, n);
                        const float dc = (n[0] - c[0]) * (n[0] - c[0]) + (n[1] - c[1]) * (n[1] - c[1]) + (n[2] - c[2]) * (n[2] - c[2]);
                        const double w = std::exp(-confidence / chromaScale) * std::exp(-dc / (colourScale * colourScale));
                        sum += w * alpha; weight += w;
                    }
                if (weight > 0) result[p] = float(sum / weight);
            }
    }, 8);
    return fromLevels(result, width, height, fullW, fullH);
}

std::shared_ptr<GrayImage> refineMatte(const GrayImage& mask, const Image& guide, const MatteSettings& raw, int limit) {
    MatteSettings s = raw.normalized();
    std::shared_ptr<GrayImage> out = s.refineEdges > 0 ? guidedRefine(mask, guide, s.refineEdges, limit) : std::make_shared<GrayImage>(mask);
    if (s.matting > 0) out = matteBand(*out, guide, s.matting, limit);
    if (s.shiftEdge != 0) {
        // Grey-level dilation or erosion: every iso-contour moves by the amount and the soft ramp survives.
        int r = std::max(1, int(std::lround(std::fabs(s.shiftEdge))));
        runningExtreme(*out, r, s.shiftEdge > 0);
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
