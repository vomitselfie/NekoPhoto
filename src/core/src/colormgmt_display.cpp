// The display's precalculated transform for 16-bit CMYK and Lab documents (colormgmt_display.h).
//
// Little CMS takes these documents to the screen through its float pipeline, which evaluates the profiles in full for
// every pixel (its 16-bit pipeline precalculates a grid, but in the monitor's encoded values, where gamut clipping and
// the steep start of the monitor's curve put kinks between the nodes: up to 20 levels off on the sweep). Here:
//
// - CMYK: a 33-point 4D grid of Little CMS's own float results at the nodes, in the monitor's linear light and not
//   clipped, so clipping happens after interpolating; cyan is interpolated linearly and magenta, yellow and black
//   tetrahedrally, as Little CMS evaluates a CMYK table. Nodes sit closer together towards no ink and full ink
//   (cmykNodeInk), where printing profiles bend most. Then a per-channel table takes linear light to the monitor's byte.
// - Lab: no grid. Lab to XYZ is a formula, and a matrix-shaper monitor's linear light is an affine function of XYZ (its
//   matrix, black point compensation, adaptation), so the transform is fitted once from Little CMS and checked.
//
// Either is built only when the monitor is a matrix-shaper (its linear version, linearProfile, then holds the same
// matrix) and the split into linear light and curves reproduces Little CMS's own float results; otherwise the caller
// gets transformBetween. Colours with a channel near black, where the monitor's curve is steep enough to turn the
// grid's error into more than half a level, go through Little CMS per pixel (DisplayGrid::shadow; about one pixel in
// seven of the CMYK fixtures).
//
// The sizes were chosen on the sweep in display_grid_tests and on Photoshop-saved CMYK files (FOGRA39 and Japan Color
// 2001 Coated): 33 nodes with the shadows taken exactly keep every one within a level of Little CMS; 33 nodes alone are
// 3 levels off near the gamut's edge, and uniformly spaced nodes 3 levels off in the lightest tints.
#include "colormgmt_display.h"
#include "nekophoto/depth.h"
#include "nekophoto/parallel.h"
#include "lcms2.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <list>
#include <mutex>
#include <random>
#include <tuple>

namespace nekophoto {

namespace {

constexpr int cmykGridSize = 33;
/// The grid's error in linear light near black (with margin), which DisplayGrid::shadow keeps from showing.
constexpr double shadowError = 4.0e-4;
constexpr int encodeEntries = 65536;

/// The ink (0..1) of the CMYK grid's node at `g` (0..1 along an axis): uniform blended with smoothstep, so nodes are
/// closer together at both ends.
double cmykNodeInk(double g) {
    constexpr double ends = 0.4;
    return (1.0 - ends) * g + ends * g * g * (3.0 - 2.0 * g);
}

uint64_t fingerprint(const std::vector<uint8_t>& bytes) {
    uint64_t h = 1469598103934665603ull;
    for (uint8_t b : bytes) { h ^= b; h *= 1099511628211ull; }
    return h ^ (uint64_t(bytes.size()) << 1);
}

/// A float as Little CMS packs it into a byte: saturated to 16 bits, then its 16-to-8 rounding.
uint8_t littleCmsByte(float v) {
    const double d = double(v) * 65535.0;
    const uint32_t w = d <= 0 ? 0u : d >= 65535 ? 65535u : uint32_t(std::floor(d + 0.5));
    return uint8_t((w * 65281u + 8388608u) >> 24);
}

/// CIE's inverse of f (Little CMS's f_1): the cube law, linear near black.
inline float cubeLaw(float t) {
    constexpr float limit = 24.0f / 116.0f;
    return t <= limit ? (108.0f / 841.0f) * (t - 16.0f / 116.0f) : t * t * t;
}

/// A stored sample straight: the sample itself when opaque, else divided by alpha (rounded, clipped), as
/// ColorTransform::applyStaged makes it straight (it keeps the quotient in float; one 15-bit step at most).
inline uint32_t straight(uint32_t v, uint32_t a) { return a >= one16 ? std::min(v, one16) : std::min(one16, (v * one16 + a / 2) / a); }

/// Whether `profile`'s output direction is a matrix and curves only, the case linearProfile reproduces.
bool matrixShaperOutput(cmsHPROFILE h) {
    if (!h || cmsGetColorSpace(h) != cmsSigRgbData || !cmsIsMatrixShaper(h)) return false;
    for (cmsTagSignature sig : {cmsSigBToA0Tag, cmsSigBToA1Tag, cmsSigBToA2Tag, cmsSigBToD0Tag, cmsSigBToD1Tag, cmsSigBToD2Tag, cmsSigBToD3Tag})
        if (cmsIsTag(h, sig)) return false;
    return true;
}

struct Profiles {
    cmsContext context = nullptr;
    cmsHPROFILE document = nullptr, monitor = nullptr, linear = nullptr;
    ~Profiles() {
        for (cmsHPROFILE h : {document, monitor, linear}) if (h) cmsCloseProfile(h);
        if (context) cmsDeleteContext(context);
    }
};

/// The monitor's curves: linear light (red, green and blue alike) to its encoded values through Little CMS, the
/// per-channel byte tables, and the shadow thresholds. False when the split does not hold.
bool buildEncoding(const Profiles& p, DisplayGrid& grid) {
    cmsHTRANSFORM curves = cmsCreateTransformTHR(p.context, p.linear, TYPE_RGB_FLT, p.monitor, TYPE_RGB_FLT, INTENT_RELATIVE_COLORIMETRIC, cmsFLAGS_NOCACHE);
    if (!curves) return false;
    std::vector<float> in(size_t(encodeEntries) * 3), out(size_t(encodeEntries) * 3);
    for (int i = 0; i < encodeEntries; i++) {
        const double u = double(i) / double(encodeEntries - 1);
        const float linear = float(u * u);
        for (int c = 0; c < 3; c++) in[size_t(i) * 3 + size_t(c)] = linear;
    }
    cmsDoTransform(curves, in.data(), out.data(), cmsUInt32Number(encodeEntries));
    cmsDeleteTransform(curves);
    for (int c = 0; c < 3; c++) {
        grid.encode[size_t(c)].resize(size_t(encodeEntries));
        for (int i = 0; i < encodeEntries; i++) grid.encode[size_t(c)][size_t(i)] = littleCmsByte(out[size_t(i) * 3 + size_t(c)]);
        // The shadow threshold: the most linear light at which `shadowError` less would still move the output by half a
        // level (none for a curve with a linear toe as steep as sRGB's or less).
        auto encoded = [&](double linear) {
            const double u = std::sqrt(std::clamp(linear, 0.0, 1.0)) * double(encodeEntries - 1);
            const size_t i = std::min(size_t(u), size_t(encodeEntries - 2));
            const double f = u - double(i);
            return double(out[i * 3 + size_t(c)]) * (1 - f) + double(out[(i + 1) * 3 + size_t(c)]) * f;
        };
        float shadow = std::numeric_limits<float>::lowest();
        for (double x = 0.05; x > shadowError; x *= 0.98)
            if ((encoded(x) - encoded(x - shadowError)) * 255.0 > 0.5) { shadow = float(x); break; }
        grid.shadow[size_t(c)] = shadow;
    }
    return true;
}

/// Little CMS's float results for `count` colours, through `t` in parallel (its transforms are made without the
/// one-pixel cache, so several threads may use one).
void transformAll(cmsHTRANSFORM t, const float* in, int inChannels, float* out, size_t count) {
    constexpr size_t chunk = 4096;
    const int chunks = int((count + chunk - 1) / chunk);
    parallelFor(0, chunks, 1, [&](int c0, int c1) {
        for (int c = c0; c < c1; c++) {
            const size_t begin = size_t(c) * chunk, n = std::min(chunk, count - begin);
            cmsDoTransform(t, in + begin * size_t(inChannels), out + begin * 3, cmsUInt32Number(n));
        }
    });
}

/// Little CMS's float input (ink 0..100, or L 0..100 and signed a and b) for a stored straight sample.
float inputOf(ColorModel model, int channel, uint32_t sample) {
    const double s = double(sample) / double(one16);
    if (model == ColorModel::CMYK) return float((1.0 - s) * 100.0);
    return channel == 0 ? float(s * 100.0) : float((double(sample) - 16384.0) / 128.0);
}

/// Checks that linear light through the monitor's curves gives what the whole transform gives, on random colours.
bool splitHolds(const Profiles& p, ColorModel model, cmsUInt32Number format, cmsUInt32Number intent, cmsUInt32Number flags, cmsHTRANSFORM toLinear) {
    cmsHTRANSFORM whole = cmsCreateTransformTHR(p.context, p.document, format, p.monitor, TYPE_RGB_FLT, intent, flags);
    cmsHTRANSFORM curves = cmsCreateTransformTHR(p.context, p.linear, TYPE_RGB_FLT, p.monitor, TYPE_RGB_FLT, INTENT_RELATIVE_COLORIMETRIC, cmsFLAGS_NOCACHE);
    bool holds = whole && curves;
    if (holds) {
        const int channels = model == ColorModel::CMYK ? 4 : 3;
        constexpr size_t count = 4096;
        std::mt19937 rng(1);
        std::uniform_int_distribution<uint32_t> sample(0, one16);
        std::vector<float> in(count * size_t(channels)), linear(count * 3), viaLinear(count * 3), direct(count * 3);
        for (size_t i = 0; i < count; i++)
            for (int c = 0; c < channels; c++) in[i * size_t(channels) + size_t(c)] = inputOf(model, c, sample(rng));
        cmsDoTransform(toLinear, in.data(), linear.data(), cmsUInt32Number(count));
        cmsDoTransform(curves, linear.data(), viaLinear.data(), cmsUInt32Number(count));
        cmsDoTransform(whole, in.data(), direct.data(), cmsUInt32Number(count));
        for (size_t i = 0; i < count * 3 && holds; i++)
            holds = std::fabs(std::clamp(viaLinear[i], 0.0f, 1.0f) - std::clamp(direct[i], 0.0f, 1.0f)) <= 2.0e-4f;
    }
    if (whole) cmsDeleteTransform(whole);
    if (curves) cmsDeleteTransform(curves);
    return holds;
}

bool buildCmyk(const Profiles& p, cmsHTRANSFORM toLinear, DisplayGrid& grid) {
    grid.kind = DisplayGrid::Kind::Cmyk;
    const int n = cmykGridSize;
    grid.size = n;
    // A stored sample's coordinate: the inverse of cmykNodeInk (increasing), by bisection; no ink and full ink exactly.
    grid.coordinate.resize(size_t(one16) + 1);
    for (uint32_t s = 0; s <= one16; s++) {
        const double ink = 1.0 - double(s) / double(one16);
        double lo = 0, hi = 1;
        for (int i = 0; i < 48; i++) {
            const double mid = (lo + hi) / 2;
            (cmykNodeInk(mid) < ink ? lo : hi) = mid;
        }
        grid.coordinate[s] = s == 0 ? float(n - 1) : s == one16 ? 0.0f : float((lo + hi) / 2 * double(n - 1));
    }
    std::vector<float> ink(static_cast<size_t>(n));
    for (int k = 0; k < n; k++) ink[size_t(k)] = float(cmykNodeInk(double(k) / double(n - 1)) * 100.0);
    const size_t count = size_t(n) * size_t(n) * size_t(n) * size_t(n);
    std::vector<float> in(count * 4);
    for (size_t i = 0; i < count; i++) {
        size_t r = i;
        for (int c = 3; c >= 0; c--) { in[i * 4 + size_t(c)] = ink[r % size_t(n)]; r /= size_t(n); }
    }
    grid.nodes.resize(count * 3);
    transformAll(toLinear, in.data(), 4, grid.nodes.data(), count);
    // A node Little CMS shows as neutral (to float precision) is stored exactly neutral.
    for (size_t i = 0; i < count; i++) {
        float* v = &grid.nodes[i * 3];
        if (std::max({v[0], v[1], v[2]}) - std::min({v[0], v[1], v[2]}) <= 2.0e-6f) v[0] = v[1] = v[2] = (v[0] + v[1] + v[2]) / 3.0f;
    }
    return true;
}

/// Fits linear light = W · (f⁻¹(fx) Xn, f⁻¹(fy) Yn, f⁻¹(fz) Zn) + o to Little CMS's Lab results by least squares and
/// checks it on random colours; false when it is not that (the fit misses by more than float noise).
bool buildLab(cmsHTRANSFORM toLinear, DisplayGrid& grid) {
    grid.kind = DisplayGrid::Kind::Lab;
    const cmsCIEXYZ* white = cmsD50_XYZ();
    auto terms = [&](const float* lab, double* q) {
        const float fy = (lab[0] + 16.0f) / 116.0f, fx = fy + 0.002f * lab[1], fz = fy - 0.005f * lab[2];
        q[0] = double(cubeLaw(fx)) * white->X; q[1] = double(cubeLaw(fy)) * white->Y; q[2] = double(cubeLaw(fz)) * white->Z; q[3] = 1.0;
    };
    std::vector<float> in, out;
    for (int l = 0; l <= 8; l++)
        for (int a = 0; a <= 8; a++)
            for (int b = 0; b <= 8; b++) in.insert(in.end(), {float(l) * 12.5f, float(a) * 32.0f - 128.0f, float(b) * 32.0f - 128.0f});
    const size_t count = in.size() / 3;
    out.resize(count * 3);
    cmsDoTransform(toLinear, in.data(), out.data(), cmsUInt32Number(count));
    // Normal equations, one 4 x 4 system shared by the three channels.
    double ata[4][4] = {}, atb[3][4] = {};
    for (size_t i = 0; i < count; i++) {
        double q[4];
        terms(&in[i * 3], q);
        for (int r = 0; r < 4; r++) {
            for (int c = 0; c < 4; c++) ata[r][c] += q[r] * q[c];
            for (int ch = 0; ch < 3; ch++) atb[ch][r] += q[r] * double(out[i * 3 + size_t(ch)]);
        }
    }
    for (int ch = 0; ch < 3; ch++) {
        double m[4][5];
        for (int r = 0; r < 4; r++) { for (int c = 0; c < 4; c++) m[r][c] = ata[r][c]; m[r][4] = atb[ch][r]; }
        for (int col = 0; col < 4; col++) {
            int pivot = col;
            for (int r = col + 1; r < 4; r++) if (std::fabs(m[r][col]) > std::fabs(m[pivot][col])) pivot = r;
            if (std::fabs(m[pivot][col]) < 1e-12) return false;
            for (int c = 0; c < 5; c++) std::swap(m[col][c], m[pivot][c]);
            for (int r = 0; r < 4; r++) {
                if (r == col) continue;
                const double f = m[r][col] / m[col][col];
                for (int c = col; c < 5; c++) m[r][c] -= f * m[col][c];
            }
        }
        for (int r = 0; r < 4; r++) grid.affine[size_t(ch * 4 + r)] = float(m[r][4] / m[r][r]);
    }
    // The check: random stored Lab samples, within float noise of Little CMS (in linear light).
    std::mt19937 rng(2);
    std::uniform_int_distribution<uint32_t> sample(0, one16);
    constexpr size_t checks = 4096;
    std::vector<float> lab(checks * 3), linear(checks * 3);
    for (size_t i = 0; i < checks; i++)
        for (int c = 0; c < 3; c++) lab[i * 3 + size_t(c)] = inputOf(ColorModel::Lab, c, sample(rng));
    cmsDoTransform(toLinear, lab.data(), linear.data(), cmsUInt32Number(checks));
    for (size_t i = 0; i < checks; i++) {
        double q[4];
        terms(&lab[i * 3], q);
        for (int ch = 0; ch < 3; ch++) {
            double v = 0;
            for (int r = 0; r < 4; r++) v += double(grid.affine[size_t(ch * 4 + r)]) * q[r];
            if (std::fabs(v - double(linear[i * 3 + size_t(ch)])) > 2.0e-5 * (1.0 + std::fabs(v))) return false;
        }
    }
    // Neutral: the rows give one value for a = b = 0 (to the profiles' fixed-point matrices, which leave Little CMS a few
    // parts in 100000 apart), so such pixels take that one value and are drawn exactly grey.
    const float* w = grid.affine.data();
    const double neutral[3] = {double(w[0]) * white->X + double(w[1]) * white->Y + double(w[2]) * white->Z,
                               double(w[4]) * white->X + double(w[5]) * white->Y + double(w[6]) * white->Z,
                               double(w[8]) * white->X + double(w[9]) * white->Y + double(w[10]) * white->Z};
    const double offsets[3] = {double(w[3]), double(w[7]), double(w[11])};
    grid.neutralLab = std::max({neutral[0], neutral[1], neutral[2]}) - std::min({neutral[0], neutral[1], neutral[2]}) <= 1.0e-4 &&
                      std::max({offsets[0], offsets[1], offsets[2]}) - std::min({offsets[0], offsets[1], offsets[2]}) <= 1.0e-6;
    grid.neutralWeight = float((neutral[0] + neutral[1] + neutral[2]) / 3.0);
    grid.neutralOffset = float((offsets[0] + offsets[1] + offsets[2]) / 3.0);
    grid.shadow.fill(std::numeric_limits<float>::lowest());   // the formula is Little CMS's own: nothing to fall back for
    return true;
}

std::shared_ptr<const DisplayGrid> buildGrid(const ColorProfile& from, const ColorProfile& to, const ConvertOptions& options, PixelFormat input,
                                             ColorTransformPtr exact) {
    const ColorModel model = pixelFormatModel(input);
    const ColorProfile& document = effectiveProfile(from, model);
    const ColorProfile& monitor = effectiveProfile(to, ColorModel::RGB);
    if (document.model != model || monitor.model != ColorModel::RGB) return nullptr;
    Profiles p;
    p.context = cmsCreateContext(nullptr, nullptr);
    p.document = cmsOpenProfileFromMemTHR(p.context, document.icc.data(), cmsUInt32Number(document.icc.size()));
    p.monitor = cmsOpenProfileFromMemTHR(p.context, monitor.icc.data(), cmsUInt32Number(monitor.icc.size()));
    if (!p.document || !matrixShaperOutput(p.monitor)) return nullptr;
    const ColorProfile linear = linearProfile(monitor);
    p.linear = cmsOpenProfileFromMemTHR(p.context, linear.icc.data(), cmsUInt32Number(linear.icc.size()));
    if (!matrixShaperOutput(p.linear)) return nullptr;
    const cmsUInt32Number format = model == ColorModel::CMYK ? TYPE_CMYK_FLT : TYPE_Lab_FLT;
    const cmsUInt32Number intent = cmsUInt32Number(options.intent);
    const cmsUInt32Number flags = cmsFLAGS_NOCACHE | (options.blackPointCompensation ? cmsFLAGS_BLACKPOINTCOMPENSATION : 0);
    cmsHTRANSFORM toLinear = cmsCreateTransformTHR(p.context, p.document, format, p.linear, TYPE_RGB_FLT, intent, flags);
    if (!toLinear) return nullptr;
    auto grid = std::make_shared<DisplayGrid>();
    grid->exact = std::move(exact);
    bool ok = splitHolds(p, model, format, intent, flags, toLinear) && buildEncoding(p, *grid);
    if (ok) ok = model == ColorModel::CMYK ? buildCmyk(p, toLinear, *grid) : buildLab(toLinear, *grid);
    cmsDeleteTransform(toLinear);
    return ok ? std::shared_ptr<const DisplayGrid>(std::move(grid)) : nullptr;
}

// ---- The cache: the last few grids, by what they were made from -------------------------------------------------------

struct GridKey {
    uint64_t from = 0, to = 0;
    int intent = 0, bpc = 0, input = 0;
    bool operator==(const GridKey& o) const { return std::tie(from, to, intent, bpc, input) == std::tie(o.from, o.to, o.intent, o.bpc, o.input); }
};
constexpr size_t gridCacheCapacity = 4;
std::mutex gridMutex;
std::list<std::pair<GridKey, ColorTransformPtr>>& gridCache() {
    static std::list<std::pair<GridKey, ColorTransformPtr>> entries;
    return entries;
}

/// One output pixel: linear light to the monitor's bytes, premultiplied by the 8-bit alpha.
inline void store(const DisplayGrid& g, const float* linear, uint32_t alpha8, uint8_t* dst) {
    for (int c = 0; c < 3; c++) {
        const float v = std::clamp(linear[c], 0.0f, 1.0f);
        const uint32_t byte = g.encode[size_t(c)][size_t(std::sqrt(v) * float(encodeEntries - 1) + 0.5f)];
        dst[c] = alpha8 == 255 ? uint8_t(byte) : uint8_t((byte * alpha8 + 127) / 255);
    }
    dst[3] = uint8_t(alpha8);
}

/// Tetrahedral interpolation in the cube at `base` over the strides `sx`, `sy` and `sz` with the fractions `rx`, `ry`
/// and `rz`, written as weights so that a point on an edge or face only reads the nodes there (and a node gives its own
/// value back): Little CMS's decomposition of the cube.
inline void tetrahedral(const float* nodes, size_t base, size_t sx, size_t sy, size_t sz, float rx, float ry, float rz, float* out) {
    size_t o1, o2;
    float f1, f2, f3;
    if (rx >= ry) {
        if (ry >= rz) { o1 = sx; o2 = sx + sy; f1 = rx; f2 = ry; f3 = rz; }
        else if (rx >= rz) { o1 = sx; o2 = sx + sz; f1 = rx; f2 = rz; f3 = ry; }
        else { o1 = sz; o2 = sx + sz; f1 = rz; f2 = rx; f3 = ry; }
    } else {
        if (rx >= rz) { o1 = sy; o2 = sx + sy; f1 = ry; f2 = rx; f3 = rz; }
        else if (ry >= rz) { o1 = sy; o2 = sy + sz; f1 = ry; f2 = rz; f3 = rx; }
        else { o1 = sz; o2 = sy + sz; f1 = rz; f2 = ry; f3 = rx; }
    }
    const float* c0 = nodes + base * 3;
    const float* c1 = nodes + (base + o1) * 3;
    const float* c2 = nodes + (base + o2) * 3;
    const float* c3 = nodes + (base + sx + sy + sz) * 3;
    const float w0 = 1.0f - f1, w1 = f1 - f2, w2 = f2 - f3;
    for (int c = 0; c < 3; c++) out[c] = c0[c] * w0 + c1[c] * w1 + c2[c] * w2 + c3[c] * f3;
}

} // namespace

void DisplayGrid::apply(const void* in, void* out, size_t count) const {
    const auto* src = static_cast<const uint16_t*>(in);
    auto* dst = static_cast<uint8_t*>(out);
    const size_t channels = kind == Kind::Cmyk ? 5 : 4;
    // Pixels the grid leaves to Little CMS, gathered and converted together after the others.
    thread_local std::vector<uint16_t> pending;
    thread_local std::vector<uint8_t> pendingOut;
    thread_local std::vector<size_t> pendingAt;
    pending.clear();
    pendingAt.clear();
    const size_t n = size_t(size), s3 = 1, s2 = n, s1 = n * n, s0 = n * n * n;
    const cmsCIEXYZ* white = cmsD50_XYZ();
    const float whiteX = float(white->X), whiteY = float(white->Y), whiteZ = float(white->Z);
    for (size_t i = 0; i < count; i++) {
        const uint16_t* p = src + i * channels;
        uint8_t* d = dst + i * 4;
        const uint32_t alpha = std::min<uint32_t>(p[channels - 1], one16);
        if (alpha == 0) { d[0] = d[1] = d[2] = d[3] = 0; continue; }
        float linear[3];
        if (kind == Kind::Cmyk) {
            size_t base = 0;
            float f[4];
            for (size_t c = 0; c < 4; c++) {
                const float t = coordinate[straight(p[c], alpha)];
                const int k = std::min(int(t), size - 2);
                f[c] = t - float(k);
                base = base * n + size_t(k);
            }
            float lo[3], hi[3];
            tetrahedral(nodes.data(), base, s1, s2, s3, f[1], f[2], f[3], lo);
            tetrahedral(nodes.data(), base + s0, s1, s2, s3, f[1], f[2], f[3], hi);
            for (int c = 0; c < 3; c++) linear[c] = lo[c] * (1.0f - f[0]) + hi[c] * f[0];
            if (linear[0] < shadow[0] || linear[1] < shadow[1] || linear[2] < shadow[2]) {
                pending.insert(pending.end(), p, p + channels);
                pendingAt.push_back(i);
                continue;
            }
        } else {
            const uint32_t sl = straight(p[0], alpha), sa = straight(p[1], alpha), sb = straight(p[2], alpha);
            const float fy = (float(sl) * (100.0f / float(one16)) + 16.0f) / 116.0f;
            if (neutralLab && sa == 16384 && sb == 16384) {
                linear[0] = linear[1] = linear[2] = neutralWeight * cubeLaw(fy) + neutralOffset;
            } else {
                const float a = (float(sa) - 16384.0f) / 128.0f, b = (float(sb) - 16384.0f) / 128.0f;
                const float q[3] = {cubeLaw(fy + 0.002f * a) * whiteX, cubeLaw(fy) * whiteY, cubeLaw(fy - 0.005f * b) * whiteZ};
                for (int c = 0; c < 3; c++) {
                    const float* w = &affine[size_t(c) * 4];
                    linear[c] = w[0] * q[0] + w[1] * q[1] + w[2] * q[2] + w[3];
                }
            }
        }
        store(*this, linear, narrow16(alpha), d);
    }
    if (pendingAt.empty()) return;
    pendingOut.resize(pendingAt.size() * 4);
    exact->apply(pending.data(), pendingOut.data(), pendingAt.size());
    for (size_t j = 0; j < pendingAt.size(); j++) std::copy_n(&pendingOut[j * 4], 4, dst + pendingAt[j] * 4);
}

ColorTransformPtr displayTransformBetween(const ColorProfile& from, const ColorProfile& to, const ConvertOptions& options, PixelFormat input) {
    ColorTransformPtr exact = transformBetween(from, to, options, input, PixelFormat::RGBA8);
    if (!exact || (input != PixelFormat::CMYKA16 && input != PixelFormat::LabA16)) return exact;
    GridKey key;
    key.from = fingerprint(effectiveProfile(from, pixelFormatModel(input)).icc);
    key.to = fingerprint(effectiveProfile(to, ColorModel::RGB).icc);
    key.intent = int(options.intent);
    key.bpc = options.blackPointCompensation ? 1 : 0;
    key.input = int(input);
    // Held while building, so that two windows asking at once build one grid.
    std::lock_guard<std::mutex> lock(gridMutex);
    auto& entries = gridCache();
    for (auto it = entries.begin(); it != entries.end(); ++it)
        if (it->first == key) {
            entries.splice(entries.begin(), entries, it);
            return entries.front().second ? entries.front().second : exact;
        }
    std::shared_ptr<const DisplayGrid> grid = buildGrid(from, to, options, input, exact);
    // A combination the grid cannot take is remembered too (as null), so it is not tried again every frame.
    ColorTransformPtr made = grid ? makeGridTransform(std::move(grid), input) : nullptr;
    entries.emplace_front(key, made);
    while (entries.size() > gridCacheCapacity) entries.pop_back();
    return made ? made : exact;
}

} // namespace nekophoto
