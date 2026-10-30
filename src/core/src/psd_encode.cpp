// The PSD writer's encoding (psd_write.h): channels compressed as Photoshop writes them at 8, 16 and 32 bits, straight
// planes from premultiplied pixels, a layer record's bytes, and the merged image of a CMYK or Lab document.
#include "psd_write.h"
#include "psd_channels.h"
#include "nekophoto/colormodes.h"
#include "nekophoto/depth.h"
#include "nekophoto/parallel.h"
#include "nekophoto/render.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <set>
#include <zlib.h>

namespace nekophoto::psd_write {

/// PackBits: runs of three or more equal bytes as repeats, the rest as literals of up to 128.
void packBits(const uint8_t* row, int n, std::vector<uint8_t>& out) {
    int i = 0;
    while (i < n) {
        int run = 1;
        while (i + run < n && run < 128 && row[i + run] == row[i]) run++;
        if (run >= 3) { out.push_back(uint8_t(257 - run)); out.push_back(row[i]); i += run; continue; }
        const int start = i;
        while (i < n && i - start < 128) {
            if (i + 2 < n && row[i] == row[i + 1] && row[i] == row[i + 2]) break;
            i++;
        }
        out.push_back(uint8_t(i - start - 1));
        out.insert(out.end(), row + start, row + i);
    }
}

/// The tagged blocks whose length is 64 bits in a PSB.
bool longKey(const std::string& key) {
    static const std::set<std::string> keys{"LMsk", "Lr16", "Lr32", "Layr", "Mt16", "Mt32", "Mtrn", "Alph", "FMsk", "lnk2", "FEid", "FXid", "PxSD", "cinf"};   // cinf: Photoshop writes it wide in a PSB too (Patchy)
    return keys.count(key) > 0;
}

/// A PackBits row made even in length, decoding the same: one literal run split in two, else the row written
/// as literal runs sized to come out even. Photoshop rejects a smart object's embedded file whose merged image
/// has an odd row when the document keeps Smart Filter caches (Patchy's pinned rule); even rows everywhere.
void makeRowEven(std::vector<uint8_t>& row, const uint8_t* raw, int w) {
    if (row.size() % 2 == 0) return;
    for (size_t i = 0; i < row.size();) {
        const int8_t header = int8_t(row[i]);
        if (header >= 1) {   // a literal run of two or more: split its first byte off
            const size_t count = size_t(header) + 1;
            std::vector<uint8_t> out(row.begin(), row.begin() + long(i));
            out.push_back(0); out.push_back(row[i + 1]);
            out.push_back(uint8_t(count - 2)); out.insert(out.end(), row.begin() + long(i + 2), row.end());
            row.swap(out);
            return;
        }
        i += header >= 0 ? size_t(header) + 2 : header == -128 ? 1 : 2;
    }
    // No literal run to split: the row as literals, one chunk split short to fix the parity.
    std::vector<uint8_t> out;
    int x = 0;
    const int chunks = (w + 127) / 128;
    bool evenOut = (size_t(w) + size_t(chunks)) % 2 == 0;
    while (x < w) {
        int n = std::min(128, w - x);
        if (!evenOut && n >= 2) { n = 1; evenOut = true; }
        out.push_back(uint8_t(n - 1));
        out.insert(out.end(), raw + x, raw + x + n);
        x += n;
    }
    row.swap(out);
}

/// One channel's data as a layer record stores it: the compression word, then the rows.
std::vector<uint8_t> encodeChannel(const std::vector<uint8_t>& plane, int w, int h, bool compress, bool large) {
    Out o;
    if (w <= 0 || h <= 0) { o.u16(0); return o.b; }
    if (compress) {
        std::vector<std::vector<uint8_t>> rows(static_cast<size_t>(h));
        size_t total = 0;
        parallelRows(0, h, [&](int ya, int yb) { for (int y = ya; y < yb; y++) packBits(plane.data() + size_t(y) * w, w, rows[size_t(y)]); }, 64);
        for (auto& r : rows) total += r.size();
        if (total + size_t(h) * (large ? 4 : 2) < plane.size()) {
            o.b.reserve(2 + size_t(h) * 4 + total);
            o.u16(1);
            for (auto& r : rows) { if (large) o.u32(uint32_t(r.size())); else o.u16(unsigned(r.size())); }
            for (auto& r : rows) o.bytes(r);
            return o.b;
        }
    }
    o.b.reserve(2 + plane.size());
    o.u16(0);
    o.bytes(plane);
    return o.b;
}

/// A 16-bit channel (0..65535 samples) as a layer record stores it: zip with prediction, as Photoshop writes 16-bit
/// layers (each sample the difference from the one to its left, big-endian, deflated), or raw.
std::vector<uint8_t> encodeChannel16(const std::vector<uint16_t>& plane, int w, int h, bool compress) {
    Out o;
    if (w <= 0 || h <= 0) { o.u16(0); return o.b; }
    std::vector<uint8_t> bytes(plane.size() * 2);
    for (int y = 0; y < h; y++) {
        const uint16_t* row = plane.data() + size_t(y) * size_t(w);
        uint8_t* out = bytes.data() + size_t(y) * size_t(w) * 2;
        for (int x = 0; x < w; x++) {
            const uint16_t v = compress ? uint16_t(row[x] - (x ? row[x - 1] : 0)) : row[x];
            out[x * 2] = uint8_t(v >> 8); out[x * 2 + 1] = uint8_t(v);
        }
    }
    if (compress) {
        uLongf size = compressBound(uLong(bytes.size()));
        std::vector<uint8_t> packed(size);
        if (compress2(packed.data(), &size, bytes.data(), uLong(bytes.size()), 6) == Z_OK) {
            packed.resize(size);
            o.b.reserve(2 + packed.size());
            o.u16(3);
            o.bytes(packed);
            return o.b;
        }
        for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {   // deflate failed: raw after all
            const uint16_t v = plane[size_t(y) * size_t(w) + size_t(x)];
            bytes[(size_t(y) * size_t(w) + size_t(x)) * 2] = uint8_t(v >> 8); bytes[(size_t(y) * size_t(w) + size_t(x)) * 2 + 1] = uint8_t(v);
        }
    }
    o.b.reserve(2 + bytes.size());
    o.u16(0);
    o.bytes(bytes);
    return o.b;
}

/// A 32-bit channel (linear floats) as a record stores it: zip with prediction, as Photoshop writes 32-bit layers (each
/// row's floats as four byte planes, big-endian, each byte the difference from the one before: predictFloatRow), or raw
/// big-endian floats.
std::vector<uint8_t> encodeChannel32(const std::vector<float>& plane, int w, int h, bool compress) {
    Out o;
    if (w <= 0 || h <= 0) { o.u16(0); return o.b; }
    const size_t rowBytes = size_t(w) * 4;
    std::vector<uint8_t> bytes(rowBytes * size_t(h));
    if (compress) {
        for (int y = 0; y < h; y++) predictFloatRow(plane.data() + size_t(y) * size_t(w), w, bytes.data() + size_t(y) * rowBytes);
        uLongf size = compressBound(uLong(bytes.size()));
        std::vector<uint8_t> packed(size);
        if (compress2(packed.data(), &size, bytes.data(), uLong(bytes.size()), 6) == Z_OK) {
            packed.resize(size);
            o.b.reserve(2 + packed.size());
            o.u16(3);
            o.bytes(packed);
            return o.b;
        }
    }
    for (size_t i = 0; i < plane.size(); i++) {
        uint32_t bits;
        std::memcpy(&bits, &plane[i], 4);
        for (int b = 0; b < 4; b++) bytes[i * 4 + size_t(b)] = uint8_t(bits >> (24 - 8 * b));
    }
    o.b.reserve(2 + bytes.size());
    o.u16(0);
    o.bytes(bytes);
    return o.b;
}

/// Straight linear float planes from a premultiplied 32-bit image (colour above 1 kept).
std::array<std::vector<float>, 4> straightPlanesF(const ImageF& image) {
    const int w = image.width(), h = image.height();
    std::array<std::vector<float>, 4> planes;
    for (auto& p : planes) p.resize(size_t(w) * h);
    parallelRows(0, h, [&](int ya, int yb) {
        for (int y = ya; y < yb; y++) {
            const float* s = image.row(y);
            for (int x = 0; x < w; x++, s += 4) {
                const size_t i = size_t(y) * w + x;
                const float a = cleanCoverage(s[3]);
                planes[3][i] = a;
                for (int c = 0; c < 3; c++) planes[size_t(c)][i] = a > 0 ? cleanColour(s[c]) / a : 0.0f;
            }
        }
    }, 64);
    return planes;
}

/// Straight 0..65535 planes from a premultiplied 16-bit image, as straightPlanes does at 8 bits.
std::array<std::vector<uint16_t>, 4> straightPlanes16(const Image16& image) {
    const int w = image.width(), h = image.height();
    std::array<std::vector<uint16_t>, 4> planes;
    for (auto& p : planes) p.resize(size_t(w) * h);
    parallelRows(0, h, [&](int ya, int yb) {
        for (int y = ya; y < yb; y++) {
            const uint16_t* s = image.row(y);
            for (int x = 0; x < w; x++, s += 4) {
                const size_t i = size_t(y) * w + x;
                const uint32_t a = s[3];
                planes[3][i] = to65535(a);
                for (int c = 0; c < 3; c++) planes[size_t(c)][i] = a == 0 ? 0 : to65535(std::min<uint32_t>(one16, (s[c] * one16 + a / 2) / a));
            }
        }
    }, 64);
    return planes;
}

/// Straight (not premultiplied) planes from premultiplied RGBA: PSD channels hold straight colour, and
/// writing premultiplied values would darken every soft edge.
std::array<std::vector<uint8_t>, 4> straightPlanes(const Image& image) {
    const int w = image.width(), h = image.height();
    std::array<std::vector<uint8_t>, 4> planes;
    for (auto& p : planes) p.resize(size_t(w) * h);
    parallelRows(0, h, [&](int ya, int yb) {
        for (int y = ya; y < yb; y++) {
            const uint8_t* s = image.row(y);
            for (int x = 0; x < w; x++, s += 4) {
                const size_t i = size_t(y) * w + x;
                const unsigned a = s[3];
                planes[3][i] = uint8_t(a);
                for (int c = 0; c < 3; c++) planes[size_t(c)][i] = a == 0 ? 0 : uint8_t(std::min(255u, (s[c] * 255u + a / 2) / a));
            }
        }
    }, 64);
    return planes;
}

/// Straight planes of a CMYK or Lab buffer, `n` samples a pixel (alpha last), as straightPlanes does for RGB: 8-bit
/// samples as bytes, 16-bit ones as 0..65535. A transparent pixel stores no ink (CMYK) or neutral a and b (Lab), as
/// Photoshop's own layers do.
template <class Img>
std::vector<std::vector<uint16_t>> straightPlanesMode(const Img& image, int n, bool lab) {
    const int w = image.width(), h = image.height();
    constexpr bool deep = sizeof(*image.data()) == 2;
    const uint32_t one = deep ? one16 : 255u;
    std::vector<std::vector<uint16_t>> planes(static_cast<size_t>(n));
    for (auto& p : planes) p.resize(size_t(w) * h);
    parallelRows(0, h, [&](int ya, int yb) {
        for (int y = ya; y < yb; y++) {
            const auto* s = image.row(y);
            for (int x = 0; x < w; x++, s += n) {
                const size_t i = size_t(y) * w + x;
                const uint32_t a = s[n - 1];
                for (int c = 0; c < n; c++) {
                    uint32_t v;
                    if (c == n - 1) v = a;
                    else if (a == 0) v = lab ? (c == 0 ? 0 : (deep ? 16384u : 128u)) : one;
                    else v = std::min<uint32_t>(one, (uint32_t(s[c]) * one + a / 2) / a);
                    planes[size_t(c)][i] = uint16_t(deep ? to65535(v) : v);
                }
            }
        }
    }, 64);
    return planes;
}

template std::vector<std::vector<uint16_t>> straightPlanesMode<Image>(const Image&, int, bool);
template std::vector<std::vector<uint16_t>> straightPlanesMode<ImageC8>(const ImageC8&, int, bool);
template std::vector<std::vector<uint16_t>> straightPlanesMode<Image16>(const Image16&, int, bool);

const char* blendKey(BlendMode mode) {
    switch (mode) {
    case BlendMode::Normal: return "norm";
    case BlendMode::Multiply: return "mul ";
    case BlendMode::Screen: return "scrn";
    case BlendMode::Overlay: return "over";
    case BlendMode::Darken: return "dark";
    case BlendMode::Lighten: return "lite";
    case BlendMode::Difference: return "diff";
    case BlendMode::ColorDodge: return "div ";
    case BlendMode::ColorBurn: return "idiv";
    case BlendMode::Hue: return "hue ";
    case BlendMode::Saturation: return "sat ";
    case BlendMode::Color: return "colr";
    case BlendMode::Luminosity: return "lum ";
    case BlendMode::Dissolve: return "diss";
    case BlendMode::LinearBurn: return "lbrn";
    case BlendMode::DarkerColor: return "dkCl";
    case BlendMode::LinearDodge: return "lddg";
    case BlendMode::LighterColor: return "lgCl";
    case BlendMode::SoftLight: return "sLit";
    case BlendMode::HardLight: return "hLit";
    case BlendMode::VividLight: return "vLit";
    case BlendMode::LinearLight: return "lLit";
    case BlendMode::PinLight: return "pLit";
    case BlendMode::HardMix: return "hMix";
    case BlendMode::Exclusion: return "smud";
    case BlendMode::Subtract: return "fsub";
    case BlendMode::Divide: return "fdiv";
    }
    return "norm";
}

/// UTF-8 to UTF-16 code units (invalid bytes become U+FFFD).
std::vector<uint16_t> utf16(const std::string& s) {
    std::vector<uint16_t> out;
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = s[i];
        uint32_t cp = 0xFFFD;
        int len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
        if (i + size_t(len) > s.size()) len = 1;
        if (len == 1) cp = c < 0x80 ? c : 0xFFFD;
        else {
            cp = c & (0xFF >> (len + 1));
            bool ok = true;
            for (int k = 1; k < len; k++) { const unsigned char d = s[i + size_t(k)]; if ((d >> 6) != 2) { ok = false; break; } cp = (cp << 6) | (d & 0x3F); }
            if (!ok) { cp = 0xFFFD; len = 1; }
        }
        if (cp >= 0x10000) { cp -= 0x10000; out.push_back(uint16_t(0xD800 + (cp >> 10))); out.push_back(uint16_t(0xDC00 + (cp & 0x3FF))); }
        else out.push_back(uint16_t(cp));
        i += size_t(len);
    }
    return out;
}

/// The legacy name field: ASCII only (anything else becomes '_'), at most 255 bytes; luni carries the real one.
std::string asciiName(const std::string& s) {
    std::string out;
    for (unsigned char c : s) { if (c >= 0x80 && (c & 0xC0) == 0x80) continue; out += c >= 0x20 && c < 0x7F ? char(c) : '_'; if (out.size() == 255) break; }
    return out;
}

void writeRecord(Out& o, const Record& r, bool large) {
    o.i32(r.top); o.i32(r.left); o.i32(r.bottom); o.i32(r.right);
    o.u16(unsigned(r.channels.size()));
    for (auto& [id, data] : r.channels) { o.i16(id); o.length(data.size(), large); }
    // A pass-through folder says so in 'lsct' only; its record says Normal, as Photoshop writes it.
    o.str("8BIM"); o.str((r.section && r.blend == "pass") ? "norm" : r.blend.c_str());
    o.u8(r.opacity); o.u8(r.clipping ? 1 : 0); o.u8(0x08 | r.flags | (r.hidden ? 2 : 0)); o.u8(0);   // bit 3: bit 4 is meaningful; Photoshop applies legacy semantics without it
    Out extra;
    if (!r.rawMask.empty()) {
        extra.u32(uint32_t(r.rawMask.size()));
        extra.bytes(r.rawMask);
    } else if (r.mask) {
        extra.u32(20);
        extra.i32(r.maskTop); extra.i32(r.maskLeft); extra.i32(r.maskBottom); extra.i32(r.maskRight);
        extra.u8(r.maskDefault); extra.u8(r.maskFlags); extra.u16(0);
    } else extra.u32(0);
    extra.u32(uint32_t(r.blendingRanges.size()));   // blending ranges (Blend If)
    extra.bytes(r.blendingRanges);
    const std::string ascii = asciiName(r.name);
    extra.u8(unsigned(ascii.size())); extra.str(ascii.c_str());
    for (size_t used = 1 + ascii.size(); used % 4; used++) extra.u8(0);
    auto block = [&](const char* key, const std::vector<uint8_t>& data) {
        extra.str("8BIM"); extra.str(key);
        extra.length(data.size() + (data.size() & 1), large && longKey(key));
        extra.bytes(data);
        if (data.size() & 1) extra.u8(0);
    };
    {
        Out u;
        const auto units = utf16(r.name);
        u.u32(uint32_t(units.size()));
        for (uint16_t c : units) u.u16(c);
        block("luni", u.b);
    }
    {
        Out id;
        id.u32(r.layerId);
        block("lyid", id.b);
    }
    if (r.fill < 255) {
        Out f;
        f.u8(r.fill); f.u8(0); f.u8(0); f.u8(0);
        block("iOpa", f.b);
    }
    if (r.section) {
        Out s;
        s.u32(uint32_t(r.section));
        if (r.section == 1 || r.section == 2) { s.str("8BIM"); s.str(r.blend.c_str()); }
        block("lsct", s.b);
    }
    if (r.adjustmentKey) block(r.adjustmentKey, r.adjustmentData);
    for (const PsdBlock& carried : r.carried) block(carried.key.c_str(), carried.data);
    o.u32(uint32_t(extra.b.size()));
    o.bytes(extra.b);
}

namespace {

/// The merged image of a CMYK or Lab document: the native render matted against white paper (no ink; L 100, a and b
/// neutral), the colour planes, its transparency, then the alpha and spot channels, at the document's depth.
void writeMergedMode(Out& f, const Document& document, const PsdExportOptions& options) {
    const bool deep = document.sampleType == SampleType::U16, lab = document.colorMode == ColorMode::Lab;
    const int w = document.width, h = document.height, colours = colorModeColorChannels(document.colorMode), n = colours + 1;
    const AnyImage flat = renderNative(document);
    const size_t bytesPer = deep ? 2 : 1;
    std::vector<std::vector<uint8_t>> raw(static_cast<size_t>(n));
    for (auto& p : raw) p.resize(size_t(w) * h * bytesPer);
    for (const Channel& channel : document.channels) raw.push_back(psdChannelPlane(channel, deep, w, h));
    auto matte = [&](const auto& image) {
        const uint32_t one = deep ? one16 : 255u, neutral = deep ? 16384u : 128u;
        parallelRows(0, h, [&](int ya, int yb) {
            for (int y = ya; y < yb; y++) {
                const auto* s = image.row(y);
                for (int x = 0; x < w; x++, s += n) {
                    const uint32_t a = s[n - 1];
                    for (int c = 0; c < n; c++) {
                        const uint32_t paper = lab && c > 0 ? neutral : one;
                        const uint32_t v = c == n - 1 ? a : std::min(one, uint32_t(s[c]) + (paper * (one - std::min(a, one)) + one / 2) / one);
                        const size_t i = (size_t(y) * w + x) * bytesPer;
                        if (deep) { const uint16_t wide = to65535(v); raw[size_t(c)][i] = uint8_t(wide >> 8); raw[size_t(c)][i + 1] = uint8_t(wide); }
                        else raw[size_t(c)][i] = uint8_t(v);
                    }
                }
            }
        }, 64);
    };
    if (flat.u16()) matte(*flat.u16());
    else if (flat.c8()) matte(*flat.c8());
    else if (flat.u8()) matte(*flat.u8());
    if (options.compress) {
        std::vector<std::vector<uint8_t>> rows(raw.size() * size_t(h));
        const int rowBytes = w * int(bytesPer);
        parallelRows(0, h, [&](int ya, int yb) {
            for (size_t c = 0; c < raw.size(); c++) for (int y = ya; y < yb; y++) {
                const uint8_t* row = raw[c].data() + size_t(y) * size_t(rowBytes);
                packBits(row, rowBytes, rows[c * size_t(h) + size_t(y)]);
                makeRowEven(rows[c * size_t(h) + size_t(y)], row, rowBytes);
            }
        }, 64);
        f.u16(1);
        for (auto& r : rows) { if (options.large) f.u32(uint32_t(r.size())); else f.u16(unsigned(r.size())); }
        for (auto& r : rows) f.bytes(r);
    } else {
        f.u16(0);
        for (auto& p : raw) f.bytes(p);
    }
}


/// A 32-bit document's merged image: in float, raw (big-endian), colour matted against white where it is transparent
/// as at 8 and 16 bits; the alpha and spot channels as float coverage.
void writeMergedFloat(Out& f, const Document& document) {
    const size_t extra = document.channels.size();
    auto flat = renderFlattenedF(document);
    const int w = document.width, h = document.height;
    const size_t n = size_t(w) * size_t(h);
    std::vector<std::vector<uint8_t>> raw(4 + extra, std::vector<uint8_t>(n * 4));
    auto put = [](std::vector<uint8_t>& plane, size_t i, float v) {
        uint32_t bits;
        std::memcpy(&bits, &v, 4);
        for (int b = 0; b < 4; b++) plane[i * 4 + size_t(b)] = uint8_t(bits >> (24 - 8 * b));
    };
    parallelRows(0, h, [&](int ya, int yb) {
        for (int y = ya; y < yb; y++) {
            const float* s = flat->row(y);
            for (int x = 0; x < w; x++, s += 4) {
                const size_t i = size_t(y) * size_t(w) + size_t(x);
                const float a = cleanCoverage(s[3]);
                for (int c = 0; c < 3; c++) put(raw[size_t(c)], i, cleanColour(s[c]) + (1 - a));
                put(raw[3], i, a);
            }
        }
    }, 64);
    for (size_t k = 0; k < extra; k++) {
        const GrayFPtr gray = grayAtDepth(document.channels[k].image, SampleType::F32).f32();
        if (gray && gray->width() == w && gray->height() == h)
            for (size_t i = 0; i < n; i++) put(raw[4 + k], i, cleanCoverage(gray->data()[i]));
    }
    f.u16(0);
    for (auto& p : raw) f.bytes(p);
}

/// A 16-bit RGB document's merged image: 0..65535 big-endian, then the alpha and spot channels.
void writeMerged16(Out& f, const Document& document, const PsdExportOptions& options) {
    const bool large = options.large;
    const size_t extra = document.channels.size();
    auto flat = renderFlattened16(document);
    const int w = document.width, h = document.height;
    std::vector<std::vector<uint8_t>> rows(size_t(h) * (4 + extra));
    std::vector<std::vector<uint8_t>> raw(4);
    for (auto& p : raw) p.resize(size_t(w) * h * 2);
    for (const Channel& channel : document.channels) raw.push_back(psdChannelPlane(channel, true, w, h));
    parallelRows(0, h, [&](int ya, int yb) {
        for (int y = ya; y < yb; y++) {
            const uint16_t* s = flat->row(y);
            for (int x = 0; x < w; x++, s += 4) {
                const size_t i = (size_t(y) * w + x) * 2;
                for (int c = 0; c < 4; c++) {
                    const uint16_t v = to65535(c == 3 ? s[3] : std::min<uint32_t>(one16, uint32_t(s[c]) + one16 - s[3]));
                    raw[size_t(c)][i] = uint8_t(v >> 8); raw[size_t(c)][i + 1] = uint8_t(v);
                }
            }
        }
    }, 64);
    if (options.compress) {
        parallelRows(0, h, [&](int ya, int yb) {
            for (size_t c = 0; c < raw.size(); c++) for (int y = ya; y < yb; y++) {
                const uint8_t* row = raw[size_t(c)].data() + size_t(y) * w * 2;
                packBits(row, w * 2, rows[size_t(c) * h + size_t(y)]);
                makeRowEven(rows[size_t(c) * h + size_t(y)], row, w * 2);
            }
        }, 64);
        f.u16(1);
        for (auto& r : rows) { if (large) f.u32(uint32_t(r.size())); else f.u16(unsigned(r.size())); }
        for (auto& r : rows) f.bytes(r);
    } else {
        f.u16(0);
        for (auto& p : raw) f.bytes(p);
    }
}

/// An 8-bit RGB document's merged image, then the alpha and spot channels.
void writeMerged8(Out& f, const Document& document, const PsdExportOptions& options) {
    const bool large = options.large;
    const size_t extra = document.channels.size();
    auto flat = renderFlattened(document);
    std::array<std::vector<uint8_t>, 4> planes;
    for (auto& p : planes) p.resize(size_t(document.width) * document.height);
    parallelRows(0, document.height, [&](int ya, int yb) {
        for (int y = ya; y < yb; y++) {
            const uint8_t* s = flat->row(y);
            for (int x = 0; x < document.width; x++, s += 4) {
                const size_t i = size_t(y) * document.width + x;
                planes[3][i] = s[3];
                for (int c = 0; c < 3; c++) planes[size_t(c)][i] = uint8_t(std::min(255, int(s[c]) + 255 - int(s[3])));
            }
        }
    }, 64);
    flat.reset();   // the planes hold it now
    const int w = document.width, h = document.height;
    std::vector<std::vector<uint8_t>> rows;
    rows.reserve(size_t(h) * 4);
    const int order[4] = {0, 1, 2, 3};
    bool rle = options.compress;
    std::vector<std::vector<uint8_t>> channelPlanes;
    for (const Channel& channel : document.channels) channelPlanes.push_back(psdChannelPlane(channel, false, w, h));
    if (rle) {
        rows.resize(size_t(h) * (4 + extra));
        parallelRows(0, h, [&](int ya, int yb) {
            for (int c : order) for (int y = ya; y < yb; y++) {
                const uint8_t* raw = planes[size_t(c)].data() + size_t(y) * w;
                packBits(raw, w, rows[size_t(c) * h + size_t(y)]);
                makeRowEven(rows[size_t(c) * h + size_t(y)], raw, w);
            }
            for (size_t k = 0; k < extra; k++) for (int y = ya; y < yb; y++) {
                const uint8_t* raw = channelPlanes[k].data() + size_t(y) * w;
                std::vector<uint8_t>& row = rows[(4 + k) * size_t(h) + size_t(y)];
                packBits(raw, w, row);
                makeRowEven(row, raw, w);
            }
        }, 64);
    }
    if (rle) {
        for (auto& p : planes) std::vector<uint8_t>().swap(p);   // the rows hold them now
        channelPlanes.clear();
        f.u16(1);
        for (auto& r : rows) { if (large) f.u32(uint32_t(r.size())); else f.u16(unsigned(r.size())); }
        for (auto& r : rows) { f.bytes(r); std::vector<uint8_t>().swap(r); }
    } else {
        f.u16(0);
        for (int c : order) f.bytes(planes[size_t(c)]);
        for (const auto& p : channelPlanes) f.bytes(p);
    }
}

} // namespace

void writeMergedImage(Out& f, const Document& document, const PsdExportOptions& options) {
    // The merged image from our own renderer, the look the layers should have. Its colour is matted
    // against white where it is transparent, as Photoshop stores it: premultiplied plus the white behind.
    // The alpha and spot channels follow, one plane each at the document's depth.
    if (document.sampleType == SampleType::F32) writeMergedFloat(f, document);
    else if (document.colorMode != ColorMode::RGB) writeMergedMode(f, document, options);
    else if (document.sampleType == SampleType::U16) writeMerged16(f, document, options);
    else writeMerged8(f, document, options);
}

} // namespace nekophoto::psd_write
