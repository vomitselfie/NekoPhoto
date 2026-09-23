// Matte refinement for Remove Background: a guided filter pulls the model's coarse mask onto the
// image's own edges (hair, fur), Shift Edge moves the whole edge, Contrast hardens it.
//
// Matting then solves the true opacity in a band around the edge from foreground and background colour
// samples (He et al.'s global sampling over Gastal & Oliveira's cost), for hair against a busy background.
//
// The filter is He, Sun & Tang's guided filter in its colour form, with the layer's R, G, B as the
// guide, so colour edges invisible in luma still steer the matte; the regularisation is edge-aware
// (Li et al.'s weighted guided filter), so flat background stays smooth while edges keep their detail;
// and, as in the fast guided filter (He & Sun 2015), the coefficients are computed on a subsampled grid
// and upsampled, then applied to the full-resolution guide.
#include "compositor/matte.h"
#include "compositor/blur.h"
#include "compositor/filters.h"
#include "compositor/morphology.h"
#include "compositor/parallel.h"
#include "compositor/render.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace compositor {

MatteSettings MatteSettings::normalized() const {
    auto c = [](double v, double lo, double hi, double f) { return std::isfinite(v) ? std::min(hi, std::max(lo, v)) : f; };
    return {c(refineEdges, 0, 40, 12), c(contrast, 0, 100, 25), c(shiftEdge, -10, 10, 0), c(matting, 0, 400, 0), cleanup, decontaminate, highPass, sideWindows, narrowBand};
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

namespace {

/// Three box passes: a low-pass close to a Gaussian of sigma about `radius`.
Map lowPass(const Map& src, int width, int height, int radius) {
    Map a, b;
    boxMean(src, a, width, height, radius);
    boxMean(a, b, width, height, radius);
    boxMean(b, a, width, height, radius);
    return a;
}

/// Sums over any rectangle of a map in constant time.
struct Integral {
    int w = 0, h = 0;
    std::vector<double> s;
    Integral(const Map& src, int width, int height) : w(width), h(height), s(size_t(width + 1) * size_t(height + 1), 0.0) {
        for (int y = 0; y < h; y++) {
            double row = 0;
            for (int x = 0; x < w; x++) { row += src[size_t(y) * w + size_t(x)]; s[size_t(y + 1) * size_t(w + 1) + size_t(x + 1)] = s[size_t(y) * size_t(w + 1) + size_t(x + 1)] + row; }
        }
    }
    double mean(int x0, int y0, int x1, int y1) const {   // inclusive corners, clipped
        x0 = std::max(0, x0); y0 = std::max(0, y0); x1 = std::min(w - 1, x1); y1 = std::min(h - 1, y1);
        if (x1 < x0 || y1 < y0) return 0;
        const size_t stride = size_t(w + 1);
        const double sum = s[size_t(y1 + 1) * stride + size_t(x1 + 1)] - s[size_t(y0) * stride + size_t(x1 + 1)] - s[size_t(y1 + 1) * stride + size_t(x0)] + s[size_t(y0) * stride + size_t(x0)];
        return sum / (double(x1 - x0 + 1) * double(y1 - y0 + 1));
    }
};

} // namespace

std::shared_ptr<GrayImage> guidedRefine(const GrayImage& mask, const Image& guide, double radius, int limit, bool highPass, bool sideWindows) {
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
    const Map pOriginal = p;
    // The high-pass variant filters the guide's and the mask's detail (their Gaussian residuals) with no
    // intercept, and adds the mask's low-pass back at the end: an explicit "the mask's shape plus the image's
    // edges" model.
    std::array<Map, C> lowIFull;
    Map lowPFull;
    if (highPass) {
        for (int c = 0; c < C; c++) { Map low = lowPass(I[size_t(c)], lowW, lowH, r); for (size_t i = 0; i < count; i++) I[size_t(c)][i] -= low[i]; }
        Map low = lowPass(p, lowW, lowH, r);
        for (size_t i = 0; i < count; i++) p[i] -= low[i];
        const int rFull = std::max(1, int(std::lround(radius * factor)));
        for (int c = 0; c < C; c++) lowIFull[size_t(c)] = lowPass(guideFull[size_t(c)], width, height, rFull);
        lowPFull = lowPass(maskLevels(mask, width, height), width, height, rFull);
    }
    const float centred = highPass ? 0.0f : 1.0f;   // covariances about the mean, or raw moments of residuals

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
                    for (int v = u; v < C; v++) A[u][v] = A[v][u] = meanII[u][v][i] - centred * meanI[size_t(u)][i] * meanI[size_t(v)][i];
                    A[u][u] += epsilon;
                    rhs[u] = meanIp[size_t(u)][i] - centred * meanI[size_t(u)][i] * meanP[i];
                }
                float coefficients[C];
                solve<C>(A, rhs, coefficients);
                float offset = centred * meanP[i];
                for (int c = 0; c < C; c++) { a[size_t(c)][i] = coefficients[c]; offset -= centred * coefficients[c] * meanI[size_t(c)][i]; }
                b[i] = offset;
            }
    });
    std::array<Map, C> meanA;
    Map meanB;
    for (int c = 0; c < C; c++) boxMean(a[size_t(c)], meanA[size_t(c)], lowW, lowH, r);
    boxMean(b, meanB, lowW, lowH, r);
    if (sideWindows) {
        // Side-window filtering (Yin, Gong & Qiu, CVPR 2019) where the mask is soft: the coefficients of the
        // one of eight half windows (left, right, up, down at (2r+1) x (r+1); the four corners at (r+1) x
        // (r+1)) whose output stays closest to the input, in place of the centred average. A window that
        // straddles an edge is never chosen, so the edge stays sharp and no halo forms.
        GrayImage soft(lowW, lowH, 0);
        for (size_t i = 0; i < count; i++) if (pOriginal[i] > 0.02f && pOriginal[i] < 0.98f) soft.data()[i] = 255;
        runningExtreme(soft, r, true);
        std::array<Integral, C> intI{Integral(I[0], lowW, lowH), Integral(I[1], lowW, lowH), Integral(I[2], lowW, lowH)};
        Integral intP(p, lowW, lowH);
        std::vector<Integral> intII, intIp;
        for (int u = 0; u < C; u++) {
            for (int v = u; v < C; v++) { for (size_t i = 0; i < count; i++) product[i] = I[size_t(u)][i] * I[size_t(v)][i]; intII.emplace_back(product, lowW, lowH); }
            for (size_t i = 0; i < count; i++) product[i] = I[size_t(u)][i] * p[i];
            intIp.emplace_back(product, lowW, lowH);
        }
        auto pairIndex = [](int u, int v) { if (u > v) std::swap(u, v); return u == 0 ? v : u == 1 ? 2 + v : 5; };   // (0,0)(0,1)(0,2)(1,1)(1,2)(2,2)
        parallelRows(0, lowH, [&](int y0, int y1) {
            for (int y = y0; y < y1; y++)
                for (int x = 0; x < lowW; x++) {
                    const size_t i = size_t(y) * lowW + size_t(x);
                    if (!soft.data()[i]) continue;
                    const float epsilon = std::clamp(lambda / float(gamma[i] * meanInverse), 1e-6f, 1e-2f);
                    const int windows[8][4] = {{x - r, y - r, x, y + r}, {x, y - r, x + r, y + r}, {x - r, y - r, x + r, y}, {x - r, y, x + r, y + r},
                                               {x - r, y - r, x, y}, {x, y - r, x + r, y}, {x - r, y, x, y + r}, {x, y, x + r, y + r}};
                    float bestGap = INFINITY, bestA[C] = {0, 0, 0}, bestB = 0;
                    for (const int* win : windows) {
                        double mI[C], mP = intP.mean(win[0], win[1], win[2], win[3]), A[C][C], rhs[C];
                        for (int c = 0; c < C; c++) mI[c] = intI[size_t(c)].mean(win[0], win[1], win[2], win[3]);
                        for (int u = 0; u < C; u++) {
                            for (int v = u; v < C; v++) A[u][v] = A[v][u] = intII[size_t(pairIndex(u, v))].mean(win[0], win[1], win[2], win[3]) - centred * mI[u] * mI[v];
                            A[u][u] += epsilon;
                            rhs[u] = intIp[size_t(u)].mean(win[0], win[1], win[2], win[3]) - centred * mI[u] * mP;
                        }
                        float coefficients[C];
                        solve<C>(A, rhs, coefficients);
                        float offset = centred * float(mP), q = 0;
                        for (int c = 0; c < C; c++) { offset -= centred * coefficients[c] * float(mI[c]); q += coefficients[c] * I[size_t(c)][i]; }
                        q += offset;
                        const float gap = std::fabs(q - p[i]);
                        if (gap < bestGap) { bestGap = gap; for (int c = 0; c < C; c++) bestA[c] = coefficients[c]; bestB = offset; }
                    }
                    for (int c = 0; c < C; c++) meanA[size_t(c)][i] = bestA[c];
                    meanB[i] = bestB;
                }
        });
    }

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
                if (highPass) { q += lowPFull[i]; for (int c = 0; c < C; c++) q += up[size_t(c)][i] * (guideFull[size_t(c)][i] - lowIFull[size_t(c)][i]); }
                else for (int c = 0; c < C; c++) q += up[size_t(c)][i] * guideFull[size_t(c)][i];
                result[i] = std::clamp(q, 0.0f, 1.0f);
            }
    });
    return fromLevels(result, width, height, fullW, fullH);
}

namespace {

/// A foreground or background colour sample found along a ray, with its distance from the pixel.
struct Sample { float r, g, b, distance; };

/// The best (foreground, background) pair for a pixel's colour: the opacity it implies and how badly it
/// explains the colour, with a small preference for nearby samples and well-separated pairs, and a pull
/// towards the opacity the matte already gives the pixel. The pull breaks the tie the colours alone cannot:
/// dark fur beside a dark patch of background is explained as well by "all foreground" as by "all
/// background", and the model's mask, which put the pixel inside the subject, is the evidence that decides.
struct Pair { float fr = 0, fg = 0, fb = 0, br = 0, bg = 0, bb = 0, alpha = 0, cost = 1e9f; };

constexpr float priorWeight = 0.25f;

inline void score(const float colour[3], float fr, float fg, float fb, float br, float bg, float bb, float spatial, float prior, Pair& best) {
    const float dr = fr - br, dg = fg - bg, db = fb - bb;
    const float separation = dr * dr + dg * dg + db * db;
    float alpha = separation > 1e-6f ? ((colour[0] - br) * dr + (colour[1] - bg) * dg + (colour[2] - bb) * db) / separation : 0.5f;
    alpha = std::clamp(alpha, 0.0f, 1.0f);
    const float er = colour[0] - (alpha * fr + (1 - alpha) * br), eg = colour[1] - (alpha * fg + (1 - alpha) * bg), eb = colour[2] - (alpha * fb + (1 - alpha) * bb);
    const float chroma = std::sqrt(er * er + eg * eg + eb * eb);
    const float cost = chroma / std::max(0.05f, std::sqrt(separation)) + spatial + priorWeight * std::fabs(alpha - prior);
    if (cost < best.cost) best = {fr, fg, fb, br, bg, bb, alpha, cost};
}

} // namespace

std::shared_ptr<GrayImage> matteBand(const GrayImage& matte, const Image& guide, double bandFull, int limit, const GrayImage* trimapFrom, MatteDebug* debug, bool narrow) {
    const int fullW = matte.width(), fullH = matte.height();
    const double factor = limit > 0 ? std::min(1.0, double(limit) / std::max(fullW, fullH)) : 1;
    const int width = std::max(1, int(std::lround(fullW * factor))), height = std::max(1, int(std::lround(fullH * factor)));
    const int band = std::max(1, int(std::lround(bandFull * factor)));
    std::array<Map, 3> colour = colourLevels(guide, width, height);
    Map levels = maskLevels(matte, width, height);
    // The trimap: sure foreground is the mask eroded by the band, sure background the eroded complement
    // (running min and max, O(N)); everything else is unknown and gets solved. The mask that shapes it, and
    // that the pair search leans on where colours cannot decide, is the model's own when the caller passes
    // it: the filtered matte can dip inside the subject where fur changes colour near the edge, and a dip
    // would open a hole in the sure foreground or vote the fur out. Half-transparent pixels beyond the band
    // are hardened with their region; the cleanup handles the specks that remain.
    const bool shaped = trimapFrom && trimapFrom->width() == fullW && trimapFrom->height() == fullH;
    const Map shape = shaped ? maskLevels(*trimapFrom, width, height) : levels;
    GrayImage eroded(width, height), dilated(width, height);
    for (int y = 0; y < height; y++) for (int x = 0; x < width; x++) eroded.at(x, y) = dilated.at(x, y) = shape[size_t(y) * width + size_t(x)] >= 0.5f ? 255 : 0;
    runningExtreme(eroded, band, false);
    runningExtreme(dilated, band, true);
    enum Region : uint8_t { Unknown = 0, Foreground = 1, Background = 2 };
    std::vector<uint8_t> region(size_t(width) * height, Unknown);
    for (size_t i = 0; i < region.size(); i++) {
        if (eroded.data()[i]) region[i] = Foreground;
        else if (!dilated.data()[i]) region[i] = Background;
    }
    auto at = [&](int x, int y) { return size_t(y) * width + size_t(x); };
    auto colourAt = [&](size_t i, float out[3]) { out[0] = colour[0][i]; out[1] = colour[1][i]; out[2] = colour[2][i]; };

    // Global sampling (He, Rhemann, Rother, Tang & Sun 2011): the candidates are every sure pixel within a few
    // pixels of the band, sorted by luma so that nearby indices are similar colours, and each unknown pixel
    // searches the (foreground, background) index space PatchMatch-style: good pairs propagate along the row
    // and from the previous sweep's row above or below, and random pairs are tried at halving distances. A
    // pair anywhere along the edge can explain a pixel, not only the first one a ray happens to hit, which is
    // what dark fur beside light fur needs.
    constexpr int depth = 4;
    GrayImage innerF = eroded, outerB = dilated;
    runningExtreme(innerF, depth, false);
    runningExtreme(outerB, depth, true);
    struct Candidate { float r, g, b, luma; int x, y; };
    std::vector<Candidate> fs, bs;
    for (int y = 0; y < height; y++)
        for (int x = 0; x < width; x++) {
            const size_t i = at(x, y);
            const bool nearF = region[i] == Foreground && !innerF.data()[i], nearB = region[i] == Background && outerB.data()[i];
            if (!nearF && !nearB) continue;
            Candidate c{colour[0][i], colour[1][i], colour[2][i], 0, x, y};
            c.luma = 0.299f * c.r + 0.587f * c.g + 0.114f * c.b;
            (nearF ? fs : bs).push_back(c);
        }
    auto byLuma = [](const Candidate& a, const Candidate& b) { return a.luma < b.luma; };
    std::sort(fs.begin(), fs.end(), byLuma);
    std::sort(bs.begin(), bs.end(), byLuma);
    if (narrow && !fs.empty() && !bs.empty()) {
        // Band narrowing (after Liang et al., IET IP 2023): a band pixel whose colour sits on one side's
        // samples and far from the other's is that side, decided before the pair search.
        auto distanceTo = [](const std::vector<Candidate>& list, const float c[3]) {
            const float luma = 0.299f * c[0] + 0.587f * c[1] + 0.114f * c[2];
            const int n = int(list.size());
            const int centre = int(std::lower_bound(list.begin(), list.end(), luma, [](const Candidate& a, float v) { return a.luma < v; }) - list.begin());
            float best = INFINITY;
            for (int i = std::max(0, centre - 12); i <= std::min(n - 1, centre + 12); i++) {
                const float dr = list[size_t(i)].r - c[0], dg = list[size_t(i)].g - c[1], db = list[size_t(i)].b - c[2];
                best = std::min(best, dr * dr + dg * dg + db * db);
            }
            return std::sqrt(best);
        };
        constexpr float close = 0.04f, far = 0.12f;
        parallelRows(0, height, [&](int y0, int y1) {
            for (int y = y0; y < y1; y++)
                for (int x = 0; x < width; x++) {
                    const size_t i = at(x, y);
                    if (region[i] != Unknown) continue;
                    float c[3];
                    colourAt(i, c);
                    const float dF = distanceTo(fs, c), dB = distanceTo(bs, c);
                    if (dF < close && dB > far) region[i] = Foreground;
                    else if (dB < close && dF > far) region[i] = Background;
                }
        });
    }
    std::vector<int32_t> slot(region.size(), -1);   // each unknown pixel's index among the unknowns
    int32_t unknowns = 0;
    for (size_t i = 0; i < region.size(); i++) if (region[i] == Unknown) slot[i] = unknowns++;
    const int nF = int(fs.size()), nB = int(bs.size());
    std::vector<Pair> pairs(static_cast<size_t>(unknowns));
    if (unknowns && nF && nB) {
        // Each pixel's distance to the sure regions normalises the spatial cost: the nearest candidates are free.
        const std::vector<float> toF = squaredDistanceTransform(eroded, true), toB = squaredDistanceTransform(dilated, false);
        std::vector<int32_t> chosenF(static_cast<size_t>(unknowns), 0), chosenB(static_cast<size_t>(unknowns), 0);
        auto evaluate = [&](int x, int y, size_t p, int iF, int iB) {
            const size_t s = size_t(slot[p]);
            const Candidate &f = fs[size_t(iF)], &b = bs[size_t(iB)];
            float c[3];
            colourAt(p, c);
            const float dfx = float(f.x - x), dfy = float(f.y - y), dbx = float(b.x - x), dby = float(b.y - y);
            const float spatial = 0.02f * (std::sqrt(dfx * dfx + dfy * dfy) / std::max(1.0f, std::sqrt(toF[p])) + std::sqrt(dbx * dbx + dby * dby) / std::max(1.0f, std::sqrt(toB[p])));
            Pair& best = pairs[s];
            const float before = best.cost;
            score(c, f.r, f.g, f.b, b.r, b.g, b.b, spatial, shape[p], best);
            if (best.cost < before) { chosenF[s] = iF; chosenB[s] = iB; }
        };
        auto hash = [](uint32_t a, uint32_t b, uint32_t c) {
            uint32_t h = a * 0x9E3779B1u ^ (b + 0x7F4A7C15u) * 0x85EBCA77u ^ (c + 1) * 0xC2B2AE3Du;
            h ^= h >> 15; h *= 0x2C1B3C6Du; h ^= h >> 12;
            return h;
        };
        // Every pixel starts from the two hypotheses that matter, "opaque" (the foreground candidate nearest
        // its colour, with a background as far from it in luma as the list offers) and "transparent" (the
        // reverse), plus a random pair; the search then trades them for nearer samples of the same colours.
        // Without the seeds, a pair that needs both indices in narrow ranges of the sorted lists (dark fur
        // against a mostly dark background, say) is rarely hit by random draws.
        std::vector<float> fLuma(fs.size()), bLuma(bs.size());
        for (size_t i = 0; i < fs.size(); i++) fLuma[i] = fs[i].luma;
        for (size_t i = 0; i < bs.size(); i++) bLuma[i] = bs[i].luma;
        auto nearestByColour = [](const std::vector<Candidate>& list, const std::vector<float>& lumas, const float c[3]) {
            const float luma = 0.299f * c[0] + 0.587f * c[1] + 0.114f * c[2];
            const int n = int(list.size());
            const int centre = int(std::lower_bound(lumas.begin(), lumas.end(), luma) - lumas.begin());
            int best = std::clamp(centre, 0, n - 1);
            float bestDistance = 1e9f;
            for (int i = std::max(0, centre - 8); i <= std::min(n - 1, centre + 8); i++) {
                const float dr = list[size_t(i)].r - c[0], dg = list[size_t(i)].g - c[1], db = list[size_t(i)].b - c[2];
                const float d = dr * dr + dg * dg + db * db;
                if (d < bestDistance) { bestDistance = d; best = i; }
            }
            return best;
        };
        parallelRows(0, height, [&](int y0, int y1) {
            for (int y = y0; y < y1; y++)
                for (int x = 0; x < width; x++) {
                    const size_t p = at(x, y);
                    if (region[p] != Unknown) continue;
                    const uint32_t h = hash(uint32_t(x), uint32_t(y), 0);
                    evaluate(x, y, p, int(h % uint32_t(nF)), int((h >> 7) % uint32_t(nB)));
                    float c[3];
                    colourAt(p, c);
                    const float luma = 0.299f * c[0] + 0.587f * c[1] + 0.114f * c[2];
                    const int farB = std::fabs(bLuma.front() - luma) > std::fabs(bLuma.back() - luma) ? 0 : nB - 1;
                    const int farF = std::fabs(fLuma.front() - luma) > std::fabs(fLuma.back() - luma) ? 0 : nF - 1;
                    evaluate(x, y, p, nearestByColour(fs, fLuma, c), farB);
                    evaluate(x, y, p, farF, nearestByColour(bs, bLuma, c));
                }
        }, 8);
        constexpr int iterations = 8;
        std::vector<int32_t> previousF, previousB;
        for (int it = 0; it < iterations; it++) {
            const bool forward = it % 2 == 0;
            previousF = chosenF;   // the row above or below is read from the last sweep, so rows can run in parallel
            previousB = chosenB;
            parallelRows(0, height, [&](int y0, int y1) {
                for (int y = y0; y < y1; y++)
                    for (int k = 0; k < width; k++) {
                        const int x = forward ? k : width - 1 - k;
                        const size_t p = at(x, y);
                        if (region[p] != Unknown) continue;
                        const size_t s = size_t(slot[p]);
                        const int nx = forward ? x - 1 : x + 1, ny = forward ? y - 1 : y + 1;
                        if (nx >= 0 && nx < width) { const int32_t ns = slot[at(nx, y)]; if (ns >= 0) evaluate(x, y, p, chosenF[size_t(ns)], chosenB[size_t(ns)]); }
                        if (ny >= 0 && ny < height) { const int32_t ns = slot[at(x, ny)]; if (ns >= 0) evaluate(x, y, p, previousF[size_t(ns)], previousB[size_t(ns)]); }
                        int step = 0;
                        for (float rF = float(nF), rB = float(nB); rF >= 1 || rB >= 1; rF *= 0.5f, rB *= 0.5f, step++) {
                            const uint32_t r = hash(uint32_t(x), uint32_t(y), uint32_t(it * 64 + step + 1));
                            const float u = float(r & 0xFFFF) / 32768.0f - 1, v = float((r >> 16) & 0xFFFF) / 32768.0f - 1;
                            const int iF = std::clamp(chosenF[s] + int(u * rF), 0, nF - 1), iB = std::clamp(chosenB[s] + int(v * rB), 0, nB - 1);
                            evaluate(x, y, p, iF, iB);
                        }
                    }
            }, 8);
        }
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
    if (debug) {
        debug->trimap = std::make_shared<GrayImage>(width, height, 0);
        debug->pairAlpha = std::make_shared<GrayImage>(width, height, 0);
        debug->chosenF = std::make_shared<Image>(width, height);
        debug->chosenB = std::make_shared<Image>(width, height);
        auto byte = [](float v) { return uint8_t(std::lround(std::clamp(v, 0.0f, 1.0f) * 255)); };
        for (int y = 0; y < height; y++)
            for (int x = 0; x < width; x++) {
                const size_t p = at(x, y);
                debug->trimap->at(x, y) = region[p] == Foreground ? 255 : region[p] == Background ? 0 : 128;
                if (region[p] != Unknown || pairs.empty()) continue;
                const Pair& pr = pairs[size_t(slot[p])];
                if (pr.cost >= 1e9f) continue;
                debug->pairAlpha->at(x, y) = byte(pr.alpha);
                uint8_t* f = debug->chosenF->pixel(x, y);
                f[0] = byte(pr.fr); f[1] = byte(pr.fg); f[2] = byte(pr.fb); f[3] = 255;
                uint8_t* b = debug->chosenB->pixel(x, y);
                b[0] = byte(pr.br); b[1] = byte(pr.bg); b[2] = byte(pr.bb); b[3] = 255;
            }
    }
    return fromLevels(result, width, height, fullW, fullH);
}

std::shared_ptr<GrayImage> refineMatte(const GrayImage& mask, const Image& guide, const MatteSettings& raw, int limit) {
    MatteSettings s = raw.normalized();
    std::shared_ptr<GrayImage> out = s.refineEdges > 0 ? guidedRefine(mask, guide, s.refineEdges, limit, s.highPass, s.sideWindows) : std::make_shared<GrayImage>(mask);
    if (s.matting > 0) out = matteBand(*out, guide, s.matting, limit, &mask, nullptr, s.narrowBand);
    if (s.cleanup) cleanMatte(*out);
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

// ---- Speckle cleanup ----------------------------------------------------------------------------------------
//
// Half-transparent alpha belongs at the subject's edge (the connectivity prior of Beijing Normal U. + HUST,
// 2025): a soft region that touches only foreground is interior noise and becomes opaque, one that touches
// only background is a floating speck and becomes transparent. Regions touching both are the edge itself.

void cleanMatte(GrayImage& matte) {
    const int w = matte.width(), h = matte.height();
    if (w <= 0 || h <= 0) return;
    constexpr uint8_t low = 25, high = 230;   // below: background, above: foreground, between: soft
    uint8_t* d = matte.data();
    const size_t n = size_t(w) * h;
    std::vector<uint8_t> visited(n, 0);
    std::vector<int32_t> stack, component;
    for (size_t start = 0; start < n; start++) {
        if (visited[start] || d[start] <= low || d[start] >= high) continue;
        // Flood one soft component, noting which sure regions it touches.
        bool touchesF = false, touchesB = false;
        component.clear();
        stack.assign(1, int32_t(start));
        visited[start] = 1;
        while (!stack.empty()) {
            const int32_t i = stack.back();
            stack.pop_back();
            component.push_back(i);
            const int x = i % w, y = i / w;
            const int32_t around[4] = {x > 0 ? i - 1 : -1, x < w - 1 ? i + 1 : -1, y > 0 ? i - w : -1, y < h - 1 ? i + w : -1};
            for (int32_t q : around) {
                if (q < 0) continue;
                const uint8_t v = d[q];
                if (v >= high) touchesF = true;
                else if (v <= low) touchesB = true;
                else if (!visited[q]) { visited[q] = 1; stack.push_back(q); }
            }
        }
        if (touchesF == touchesB) continue;   // the edge itself (or a soft image with no sure pixels at all)
        const uint8_t value = touchesF ? 255 : 0;
        for (int32_t i : component) d[i] = value;
    }
}

// ---- Foreground estimation ------------------------------------------------------------------------------
//
// Germer, Uelwer, Conrad & Harmeling, "Fast Multi-Level Foreground Estimation" (ICPR 2020): each pixel's colour
// is alpha * F + (1 - alpha) * B, with F and B asked to vary smoothly, more strictly where alpha does not
// change. Holding the neighbours fixed that is a 2x2 system per pixel, shared by the three channels, swept
// Jacobi-style coarse to fine from a level a couple of pixels wide, so that colour equalises across the whole
// image at the small levels and settles at the large ones.
// The base of the pyramid is at most 2048 px on its longest side; the full-size pass runs in tiles with a
// margin wider than its few sweeps can reach, so it is identical to a whole-image pass at a fraction of the
// memory, and only where the matte is soft.

namespace {

struct ForegroundLevel {
    int width = 0, height = 0;
    std::array<Map, 3> colour;   // straight R, G, B
    Map alpha;
    std::vector<uint8_t> work;   // 1 where the estimate is worth computing (near a soft pixel); empty: everywhere
};

constexpr float smoothness = 1e-5f, alphaWeight = 1.0f;   // the reference implementation's regularisation and gradient weight

/// One Jacobi sweep over `level`: F, B from their neighbours' previous values.
void relaxForeground(const ForegroundLevel& level, std::array<Map, 3>& F, std::array<Map, 3>& B, std::array<Map, 3>& nextF, std::array<Map, 3>& nextB) {
    const int w = level.width, h = level.height;
    parallelRows(0, h, [&](int y0, int y1) {
        for (int y = y0; y < y1; y++)
            for (int x = 0; x < w; x++) {
                const size_t i = size_t(y) * w + size_t(x);
                if (!level.work.empty() && !level.work[i]) { for (int c = 0; c < 3; c++) { nextF[size_t(c)][i] = F[size_t(c)][i]; nextB[size_t(c)][i] = B[size_t(c)][i]; } continue; }
                const float a = level.alpha[i];
                float weightSum = 0, sumF[3] = {0, 0, 0}, sumB[3] = {0, 0, 0};
                const size_t around[4] = {x > 0 ? i - 1 : i, x < w - 1 ? i + 1 : i, y > 0 ? i - size_t(w) : i, y < h - 1 ? i + size_t(w) : i};
                for (size_t q : around) {
                    if (q == i) continue;
                    const float wq = smoothness + alphaWeight * std::fabs(a - level.alpha[q]);
                    weightSum += wq;
                    for (int c = 0; c < 3; c++) { sumF[c] += wq * F[size_t(c)][q]; sumB[c] += wq * B[size_t(c)][q]; }
                }
                // [a^2 + W, a(1-a); a(1-a), (1-a)^2 + W] [F; B] = [a I + SF; (1-a) I + SB]
                const float m00 = a * a + weightSum, m01 = a * (1 - a), m11 = (1 - a) * (1 - a) + weightSum;
                const float det = std::max(1e-12f, m00 * m11 - m01 * m01);
                for (int c = 0; c < 3; c++) {
                    const float I = level.colour[size_t(c)][i];
                    const float r0 = a * I + sumF[c], r1 = (1 - a) * I + sumB[c];
                    nextF[size_t(c)][i] = std::clamp((m11 * r0 - m01 * r1) / det, 0.0f, 1.0f);
                    nextB[size_t(c)][i] = std::clamp((m00 * r1 - m01 * r0) / det, 0.0f, 1.0f);
                }
            }
    });
}

void sweepForeground(const ForegroundLevel& level, std::array<Map, 3>& F, std::array<Map, 3>& B, int iterations) {
    std::array<Map, 3> nextF, nextB;
    for (int c = 0; c < 3; c++) { nextF[size_t(c)].resize(F[size_t(c)].size()); nextB[size_t(c)].resize(B[size_t(c)].size()); }
    for (int k = 0; k < iterations; k++) {
        relaxForeground(level, F, B, nextF, nextB);
        F.swap(nextF);
        B.swap(nextB);
    }
}

/// 2x2 block average of a map (odd sizes take the clamped block).
Map halveMap(const Map& src, int sw, int sh, int dw, int dh) {
    Map out(size_t(dw) * dh);
    for (int y = 0; y < dh; y++)
        for (int x = 0; x < dw; x++) {
            float sum = 0;
            for (int j = 0; j < 2; j++)
                for (int i = 0; i < 2; i++) sum += src[size_t(std::min(sh - 1, 2 * y + j)) * sw + size_t(std::min(sw - 1, 2 * x + i))];
            out[size_t(y) * dw + size_t(x)] = sum * 0.25f;
        }
    return out;
}

/// 1 within `margin` pixels of a half-transparent alpha, the pixels worth solving.
std::vector<uint8_t> workNear(const Map& alpha, int w, int h, int margin) {
    GrayImage soft(w, h, 0);
    bool any = false;
    for (size_t i = 0; i < alpha.size(); i++) if (alpha[i] > 0.004f && alpha[i] < 0.996f) { soft.data()[i] = 255; any = true; }
    std::vector<uint8_t> work(alpha.size(), 0);
    if (!any) return work;
    runningExtreme(soft, margin, true);
    for (size_t i = 0; i < work.size(); i++) work[i] = soft.data()[i] ? 1 : 0;
    return work;
}

} // namespace

std::shared_ptr<Image> estimateForeground(const Image& image, const GrayImage& matte) {
    const int w = image.width(), h = image.height();
    auto out = std::make_shared<Image>(image);
    if (w <= 0 || h <= 0 || matte.width() != w || matte.height() != h) return out;
    // Which pixels get new colours: soft ones, and their transparent neighbours (a resampled mask blends them in).
    std::vector<uint8_t> replace(size_t(w) * h, 0);
    bool any = false;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            const uint8_t m = matte.at(x, y);
            if (m == 255) continue;
            bool near = false;
            for (int j = -1; j <= 1 && !near; j++)
                for (int i = -1; i <= 1 && !near; i++) {
                    const int sx = x + i, sy = y + j;
                    if (sx < 0 || sy < 0 || sx >= w || sy >= h) continue;
                    const uint8_t v = matte.at(sx, sy);
                    near = v > 0 && v < 255;
                }
            if (near) { replace[size_t(y) * w + size_t(x)] = 1; any = true; }
        }
    if (!any) return out;

    // The dense pyramid: from a base no larger than 2048 px down to a couple of pixels.
    std::vector<ForegroundLevel> levels;
    {
        ForegroundLevel base;
        base.width = w; base.height = h;
        while (std::max(base.width, base.height) > 2048) { base.width = (base.width + 1) / 2; base.height = (base.height + 1) / 2; }
        base.colour = colourLevels(image, base.width, base.height);
        base.alpha = maskLevels(matte, base.width, base.height);
        levels.push_back(std::move(base));
        while (std::max(levels.back().width, levels.back().height) > 2) {
            const ForegroundLevel& fine = levels.back();
            ForegroundLevel coarse;
            coarse.width = (fine.width + 1) / 2; coarse.height = (fine.height + 1) / 2;
            for (int c = 0; c < 3; c++) coarse.colour[size_t(c)] = halveMap(fine.colour[size_t(c)], fine.width, fine.height, coarse.width, coarse.height);
            coarse.alpha = halveMap(fine.alpha, fine.width, fine.height, coarse.width, coarse.height);
            levels.push_back(std::move(coarse));
        }
        for (ForegroundLevel& level : levels)
            if (std::max(level.width, level.height) > 64) level.work = workNear(level.alpha, level.width, level.height, 6);
    }
    // Coarse to fine: colours start as the image itself.
    std::array<Map, 3> F, B;
    for (size_t l = levels.size(); l-- > 0;) {
        const ForegroundLevel& level = levels[l];
        if (l + 1 == levels.size()) { F = level.colour; B = level.colour; }
        else {
            const ForegroundLevel& coarse = levels[l + 1];
            for (int c = 0; c < 3; c++) { F[size_t(c)] = upsample(F[size_t(c)], coarse.width, coarse.height, level.width, level.height); B[size_t(c)] = upsample(B[size_t(c)], coarse.width, coarse.height, level.width, level.height); }
        }
        sweepForeground(level, F, B, std::max(level.width, level.height) <= 32 ? 10 : 4);
    }
    const ForegroundLevel& base = levels.front();
    auto write = [&](int x, int y, const float f[3]) {
        uint8_t* p = out->pixel(x, y);
        const float a = p[3];
        for (int c = 0; c < 3; c++) p[c] = uint8_t(std::lround(std::clamp(f[c], 0.0f, 1.0f) * a));
    };
    if (base.width == w && base.height == h) {
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                const size_t i = size_t(y) * w + size_t(x);
                if (!replace[i]) continue;
                const float f[3] = {F[0][i], F[1][i], F[2][i]};
                write(x, y, f);
            }
        return out;
    }
    // Full size, in tiles: colours from the base bilinearly, then a few sweeps against the real pixels. The
    // margin exceeds what the sweeps can reach, so the tiles agree with a whole-image pass.
    constexpr int tile = 256, margin = 8, sweeps = 4;
    const int tilesX = (w + tile - 1) / tile, tilesY = (h + tile - 1) / tile;
    parallelRows(0, tilesY, [&](int ty0, int ty1) {
        ForegroundLevel local;
        std::array<Map, 3> lf, lb;
        for (int ty = ty0; ty < ty1; ty++)
            for (int tx = 0; tx < tilesX; tx++) {
                const int x0 = tx * tile, y0 = ty * tile, x1 = std::min(w, x0 + tile), y1 = std::min(h, y0 + tile);
                bool needed = false;
                for (int y = y0; y < y1 && !needed; y++) for (int x = x0; x < x1 && !needed; x++) needed = replace[size_t(y) * w + size_t(x)];
                if (!needed) continue;
                const int lx0 = std::max(0, x0 - margin), ly0 = std::max(0, y0 - margin), lx1 = std::min(w, x1 + margin), ly1 = std::min(h, y1 + margin);
                local.width = lx1 - lx0; local.height = ly1 - ly0;
                const size_t count = size_t(local.width) * local.height;
                for (int c = 0; c < 3; c++) { local.colour[size_t(c)].resize(count); lf[size_t(c)].resize(count); lb[size_t(c)].resize(count); }
                local.alpha.resize(count);
                local.work.clear();
                const double sx = double(base.width) / w, sy = double(base.height) / h;
                for (int y = ly0; y < ly1; y++) {
                    const uint8_t* p = image.pixel(lx0, y);
                    for (int x = lx0; x < lx1; x++, p += 4) {
                        const size_t i = size_t(y - ly0) * local.width + size_t(x - lx0);
                        const float a = p[3] ? p[3] / 255.0f : 1.0f;
                        for (int c = 0; c < 3; c++) local.colour[size_t(c)][i] = p[c] / 255.0f / a;
                        local.alpha[i] = matte.at(x, y) / 255.0f;
                        // Bilinear sample of the base level's F and B.
                        const double bx = std::clamp((x + 0.5) * sx - 0.5, 0.0, double(base.width - 1)), by = std::clamp((y + 0.5) * sy - 0.5, 0.0, double(base.height - 1));
                        const int ix = int(bx), iy = int(by), ix1 = std::min(ix + 1, base.width - 1), iy1 = std::min(iy + 1, base.height - 1);
                        const float wx = float(bx - ix), wy = float(by - iy);
                        for (int c = 0; c < 3; c++) {
                            const Map &f = F[size_t(c)], &b = B[size_t(c)];
                            const size_t i00 = size_t(iy) * base.width + size_t(ix), i01 = size_t(iy) * base.width + size_t(ix1), i10 = size_t(iy1) * base.width + size_t(ix), i11 = size_t(iy1) * base.width + size_t(ix1);
                            lf[size_t(c)][i] = (f[i00] * (1 - wx) + f[i01] * wx) * (1 - wy) + (f[i10] * (1 - wx) + f[i11] * wx) * wy;
                            lb[size_t(c)][i] = (b[i00] * (1 - wx) + b[i01] * wx) * (1 - wy) + (b[i10] * (1 - wx) + b[i11] * wx) * wy;
                        }
                    }
                }
                sweepForeground(local, lf, lb, sweeps);
                for (int y = y0; y < y1; y++)
                    for (int x = x0; x < x1; x++) {
                        if (!replace[size_t(y) * w + size_t(x)]) continue;
                        const size_t i = size_t(y - ly0) * local.width + size_t(x - lx0);
                        const float f[3] = {lf[0][i], lf[1][i], lf[2][i]};
                        write(x, y, f);
                    }
            }
    }, 1);
    return out;
}

} // namespace compositor
