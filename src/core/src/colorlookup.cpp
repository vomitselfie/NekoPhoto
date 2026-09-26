// Color Lookup (adjustments.h): the LUT Photoshop embeds in a 'clrL' adjustment, as a 3D table applied with trilinear
// interpolation. A .cube (Adobe/Resolve: LUT_3D_SIZE, optional DOMAIN_MIN / DOMAIN_MAX, red fastest; a 1D LUT_1D_SIZE
// too), a .3dl (an input shaper line, then integer triplets, blue fastest) or an ICC profile (an abstract profile run
// between sRGB and sRGB, or an RGB device link) through lcms2. Tables are cached by the LUT's content, never by where
// its bytes happen to live.
#include "compositor/adjustments.h"
#include "compositor/parallel.h"
#include "lcms2.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <sstream>

namespace compositor {

namespace {

struct Table {
    int size = 0;                      // points per axis (3D), or entries (1D)
    bool oneD = false;
    std::vector<float> rgb;            // 3D: index (r + size * (g + size * b)) * 3; 1D: index * 3
    double domainMin[3] = {0, 0, 0}, domainMax[3] = {1, 1, 1};
};

std::shared_ptr<Table> parseCube(const std::string& text) {
    auto t = std::make_shared<Table>();
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (auto hash = line.find('#'); hash != std::string::npos) line.resize(hash);
        std::istringstream ls(line);
        std::string first;
        if (!(ls >> first)) continue;
        if (first == "TITLE") continue;
        if (first == "LUT_3D_SIZE" || first == "LUT_1D_SIZE") {
            ls >> t->size;
            t->oneD = first == "LUT_1D_SIZE";
            if (t->size < 2 || t->size > (t->oneD ? 65536 : 256)) return nullptr;
            continue;
        }
        if (first == "DOMAIN_MIN") { ls >> t->domainMin[0] >> t->domainMin[1] >> t->domainMin[2]; continue; }
        if (first == "DOMAIN_MAX") { ls >> t->domainMax[0] >> t->domainMax[1] >> t->domainMax[2]; continue; }
        if (first == "LUT_3D_INPUT_RANGE" || first == "LUT_1D_INPUT_RANGE") {
            double lo = 0, hi = 1;
            ls >> lo >> hi;
            for (int c = 0; c < 3; c++) { t->domainMin[c] = lo; t->domainMax[c] = hi; }
            continue;
        }
        char* end = nullptr;
        const double r = std::strtod(first.c_str(), &end);
        if (end == first.c_str()) continue;   // an unknown keyword
        double g = 0, b = 0;
        if (!(ls >> g >> b)) return nullptr;
        t->rgb.insert(t->rgb.end(), {float(r), float(g), float(b)});
    }
    const size_t expected = t->oneD ? size_t(t->size) : size_t(t->size) * size_t(t->size) * size_t(t->size);
    if (t->size < 2 || t->rgb.size() != expected * 3) return nullptr;
    for (int c = 0; c < 3; c++) if (!(t->domainMax[c] > t->domainMin[c])) return nullptr;
    return t;
}

std::shared_ptr<Table> parse3dl(const std::string& text) {
    std::istringstream in(text);
    std::string line;
    std::vector<long> shaper;
    std::vector<long> values;
    while (std::getline(in, line)) {
        if (auto hash = line.find('#'); hash != std::string::npos) line.resize(hash);
        std::istringstream ls(line);
        std::vector<long> numbers;
        long v;
        while (ls >> v) numbers.push_back(v);
        if (numbers.empty()) continue;
        if (shaper.empty() && numbers.size() > 3) { shaper = numbers; continue; }
        if (numbers.size() != 3) continue;   // "Mesh" lines and the like
        values.insert(values.end(), numbers.begin(), numbers.end());
    }
    const long count = long(values.size() / 3);
    int n = shaper.empty() ? int(std::lround(std::cbrt(double(count)))) : int(shaper.size());
    if (n < 2 || long(n) * n * n != count) return nullptr;
    // The output's bit depth: the next power of two above the largest value.
    const long top = std::max(1L, *std::max_element(values.begin(), values.end()));
    double scale = 1;
    while (scale <= double(top)) scale *= 2;
    scale -= 1;
    auto t = std::make_shared<Table>();
    t->size = n;
    t->rgb.resize(size_t(n) * n * n * 3);
    // Blue changes fastest in a .3dl; the table here is red fastest.
    size_t i = 0;
    for (int r = 0; r < n; r++)
        for (int g = 0; g < n; g++)
            for (int b = 0; b < n; b++, i += 3) {
                const size_t at = (size_t(r) + size_t(n) * (size_t(g) + size_t(n) * size_t(b))) * 3;
                for (int c = 0; c < 3; c++) t->rgb[at + size_t(c)] = float(values[i + size_t(c)] / scale);
            }
    return t;
}

std::shared_ptr<Table> fromProfile(const std::vector<uint8_t>& bytes) {
    cmsContext context = cmsCreateContext(nullptr, nullptr);
    if (!context) return nullptr;
    std::shared_ptr<Table> out;
    cmsHPROFILE profile = cmsOpenProfileFromMemTHR(context, bytes.data(), cmsUInt32Number(bytes.size()));
    cmsHPROFILE srgb = cmsCreate_sRGBProfileTHR(context);
    cmsHTRANSFORM transform = nullptr;
    if (profile && srgb) {
        const cmsProfileClassSignature cls = cmsGetDeviceClass(profile);
        if (cls == cmsSigAbstractClass) {
            cmsHPROFILE chain[3] = {srgb, profile, srgb};
            transform = cmsCreateMultiprofileTransformTHR(context, chain, 3, TYPE_RGB_FLT, TYPE_RGB_FLT, INTENT_PERCEPTUAL, 0);
        } else if (cls == cmsSigLinkClass && cmsGetColorSpace(profile) == cmsSigRgbData && cmsGetPCS(profile) == cmsSigRgbData) {
            transform = cmsCreateTransformTHR(context, profile, TYPE_RGB_FLT, nullptr, TYPE_RGB_FLT, INTENT_PERCEPTUAL, 0);
        }
    }
    if (transform) {
        constexpr int n = 33;
        out = std::make_shared<Table>();
        out->size = n;
        std::vector<float> grid(size_t(n) * n * n * 3);
        size_t i = 0;
        for (int b = 0; b < n; b++)
            for (int g = 0; g < n; g++)
                for (int r = 0; r < n; r++, i += 3) { grid[i] = float(r) / (n - 1); grid[i + 1] = float(g) / (n - 1); grid[i + 2] = float(b) / (n - 1); }
        out->rgb.resize(grid.size());
        cmsDoTransform(transform, grid.data(), out->rgb.data(), cmsUInt32Number(n * n * n));
        cmsDeleteTransform(transform);
    }
    if (srgb) cmsCloseProfile(srgb);
    if (profile) cmsCloseProfile(profile);
    cmsDeleteContext(context);
    return out;
}

std::shared_ptr<Table> buildTable(const ColorLookupSettings& s) {
    if (s.format == "cube") return parseCube(s.data);
    if (s.format == "3dl") return parse3dl(s.data);
    if (s.format == "icc") { auto bytes = fromBase64(s.data); return bytes ? fromProfile(*bytes) : nullptr; }
    return nullptr;
}

/// The table for these settings, from a small cache keyed by the LUT's content (a document's layers share one).
std::shared_ptr<Table> tableFor(const ColorLookupSettings& s) {
    static std::mutex mutex;
    static std::list<std::pair<std::string, std::shared_ptr<Table>>> cache;   // most recent first
    const std::string key = s.format + '\n' + s.data;
    {
        std::lock_guard lock(mutex);
        for (auto it = cache.begin(); it != cache.end(); ++it)
            if (it->first == key) { cache.splice(cache.begin(), cache, it); return cache.front().second; }
    }
    auto table = buildTable(s);
    std::lock_guard lock(mutex);
    cache.emplace_front(key, table);
    while (cache.size() > 4) cache.pop_back();
    return table;
}

const char* kBase64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

} // namespace

std::string toBase64(const std::vector<uint8_t>& bytes) {
    std::string out;
    out.reserve((bytes.size() + 2) / 3 * 4);
    for (size_t i = 0; i < bytes.size(); i += 3) {
        const uint32_t v = uint32_t(bytes[i]) << 16 | (i + 1 < bytes.size() ? uint32_t(bytes[i + 1]) << 8 : 0) | (i + 2 < bytes.size() ? bytes[i + 2] : 0);
        out += kBase64[(v >> 18) & 63];
        out += kBase64[(v >> 12) & 63];
        out += i + 1 < bytes.size() ? kBase64[(v >> 6) & 63] : '=';
        out += i + 2 < bytes.size() ? kBase64[v & 63] : '=';
    }
    return out;
}

std::optional<std::vector<uint8_t>> fromBase64(const std::string& text) {
    std::vector<uint8_t> out;
    uint32_t buffer = 0;
    int bits = 0;
    for (char c : text) {
        if (c == '=' || c == '\n' || c == '\r' || c == ' ') continue;
        const char* at = std::strchr(kBase64, c);
        if (!at || !*at) return std::nullopt;
        buffer = buffer << 6 | uint32_t(at - kBase64);
        bits += 6;
        if (bits >= 8) { bits -= 8; out.push_back(uint8_t(buffer >> bits)); }
    }
    return out;
}

bool colorLookupReadable(const ColorLookupSettings& s) { return tableFor(s) != nullptr; }

void applyColorLookup(Image& image, const ColorLookupSettings& s) {
    const auto table = tableFor(s);
    if (!table) return;
    const Table& t = *table;
    const int n = t.size;
    auto lookup1D = [&](int c, double v) {
        const double x = std::clamp((v - t.domainMin[c]) / (t.domainMax[c] - t.domainMin[c]), 0.0, 1.0) * (n - 1);
        const int i = std::min(n - 2, int(x));
        const double f = x - i;
        return t.rgb[size_t(i) * 3 + size_t(c)] * (1 - f) + t.rgb[size_t(i + 1) * 3 + size_t(c)] * f;
    };
    parallelRows(0, image.height(), [&](int y0, int y1) {
        for (int y = y0; y < y1; y++)
            for (int x = 0; x < image.width(); x++) {
                uint8_t* p = image.pixel(x, y);
                const int a = p[3];
                if (!a) continue;
                double in[3];
                for (int c = 0; c < 3; c++) in[c] = std::min(255, p[c] * 255 / a) / 255.0;
                double out[3];
                if (t.oneD) {
                    for (int c = 0; c < 3; c++) out[c] = lookup1D(c, in[c]);
                } else {
                    double pos[3];
                    int i0[3];
                    double f[3];
                    for (int c = 0; c < 3; c++) {
                        pos[c] = std::clamp((in[c] - t.domainMin[c]) / (t.domainMax[c] - t.domainMin[c]), 0.0, 1.0) * (n - 1);
                        i0[c] = std::min(n - 2, int(pos[c]));
                        f[c] = pos[c] - i0[c];
                    }
                    auto at = [&](int r, int g, int b, int c) { return double(t.rgb[(size_t(r) + size_t(n) * (size_t(g) + size_t(n) * size_t(b))) * 3 + size_t(c)]); };
                    for (int c = 0; c < 3; c++) {
                        double acc = 0;
                        for (int k = 0; k < 8; k++) {
                            const int dr = k & 1, dg = (k >> 1) & 1, db = (k >> 2) & 1;
                            const double w = (dr ? f[0] : 1 - f[0]) * (dg ? f[1] : 1 - f[1]) * (db ? f[2] : 1 - f[2]);
                            acc += w * at(i0[0] + dr, i0[1] + dg, i0[2] + db, c);
                        }
                        out[c] = acc;
                    }
                }
                for (int c = 0; c < 3; c++) p[c] = uint8_t(std::clamp(std::lround(std::clamp(out[c], 0.0, 1.0) * a), 0L, long(a)));
            }
    });
}

} // namespace compositor
