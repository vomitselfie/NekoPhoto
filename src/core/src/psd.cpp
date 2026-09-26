#include "compositor/psd.h"
#include "photoshop.h"
#include "compositor/adjustments.h"
#include "compositor/colour.h"
#include "compositor/png.h"
#include "compositor/render.h"
#include "compositor/smartfilter.h"
#include "psd/psd_descriptor.hpp"
#include <cstdio>
#include <new>
#include <stdexcept>
#include <zlib.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <sstream>

namespace compositor {

using namespace photoshop;

namespace {

// ---- Channel data ------------------------------------------------------------------------------------

struct Channel { int id = 0; uint64_t length = 0; const uint8_t* data = nullptr; };

/// PackBits: literal runs and repeats.
void unpackBits(const uint8_t* in, size_t n, uint8_t* out, size_t outSize) {
    size_t i = 0, o = 0;
    while (i < n && o < outSize) {
        int8_t h = int8_t(in[i++]);
        if (h >= 0) { size_t len = size_t(h) + 1; if (i + len > n) len = n - i; len = std::min(len, outSize - o); std::memcpy(out + o, in + i, len); i += len; o += len; }
        else if (h != -128) { size_t len = size_t(-h) + 1; if (i >= n) break; std::memset(out + o, in[i++], std::min(len, outSize - o)); o += std::min(len, outSize - o); }
    }
}

/// sRGB encoding of a linear value, for 32-bit files.
uint8_t encodeLinear(float v) {
    v = std::clamp(v, 0.0f, 1.0f);
    float s = v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1 / 2.4f) - 0.055f;
    return uint8_t(std::lround(s * 255));
}

/// Decodes one plane of `width` x `height` samples to 8 bits from `compression` (0 raw, 1 RLE, 2 zip,
/// 3 zip with prediction) and `depth` bits. False when the data does not add up.
bool decodePlane(const uint8_t* data, size_t size, int compression, int width, int height, int depth, bool psb, std::vector<uint8_t>& out, std::string* why) {
    const size_t bytesPer = size_t(depth) / 8, rowBytes = size_t(width) * bytesPer, total = rowBytes * size_t(height);
    if (depth != 8 && depth != 16 && depth != 32) { if (why) *why = "unsupported bit depth"; return false; }
    std::vector<uint8_t> raw(total);
    try {
        Reader r(data, size);
        if (compression == 0) {
            std::memcpy(raw.data(), r.bytes(total), total);
        } else if (compression == 1) {
            const size_t rowCount = static_cast<size_t>(height);
            std::vector<size_t> counts(rowCount);
            for (int y = 0; y < height; y++) counts[size_t(y)] = psb ? r.u32() : r.u16();
            for (int y = 0; y < height; y++) unpackBits(r.bytes(counts[size_t(y)]), counts[size_t(y)], &raw[size_t(y) * rowBytes], rowBytes);
        } else if (compression == 2 || compression == 3) {
            uLongf destLen = uLongf(total);
            if (uncompress(raw.data(), &destLen, data, uLong(size)) != Z_OK || destLen != total) { if (why) *why = "zip data did not inflate"; return false; }
            if (compression == 3) {
                // Rows hold differences from the sample to the left; 32-bit rows keep their bytes planar.
                for (int y = 0; y < height; y++) {
                    uint8_t* row = &raw[size_t(y) * rowBytes];
                    if (depth == 8) for (size_t x = 1; x < rowBytes; x++) row[x] = uint8_t(row[x] + row[x - 1]);
                    else if (depth == 16) for (int x = 1; x < width; x++) { uint16_t prev = uint16_t((row[(x - 1) * 2] << 8) | row[(x - 1) * 2 + 1]), cur = uint16_t((row[x * 2] << 8) | row[x * 2 + 1]); cur = uint16_t(cur + prev); row[x * 2] = uint8_t(cur >> 8); row[x * 2 + 1] = uint8_t(cur); }
                    else {
                        for (size_t x = 1; x < rowBytes; x++) row[x] = uint8_t(row[x] + row[x - 1]);
                        std::vector<uint8_t> interleaved(rowBytes);
                        for (int x = 0; x < width; x++) for (int b = 0; b < 4; b++) interleaved[size_t(x) * 4 + size_t(b)] = row[size_t(b) * size_t(width) + size_t(x)];
                        std::memcpy(row, interleaved.data(), rowBytes);
                    }
                }
            }
        } else { if (why) *why = "unknown compression"; return false; }
    } catch (Truncated&) { if (why) *why = "channel data ends early"; return false; }
    out.resize(size_t(width) * size_t(height));
    if (depth == 8) out = std::move(raw);
    else if (depth == 16) for (size_t i = 0; i < out.size(); i++) out[i] = uint8_t((unsigned(raw[i * 2]) * 256 + raw[i * 2 + 1] + 128) / 257);
    else for (size_t i = 0; i < out.size(); i++) { uint32_t bits = (uint32_t(raw[i * 4]) << 24) | (uint32_t(raw[i * 4 + 1]) << 16) | (uint32_t(raw[i * 4 + 2]) << 8) | raw[i * 4 + 3]; float f; std::memcpy(&f, &bits, 4); out[i] = encodeLinear(f); }
    return true;
}

// ---- Colour ---------------------------------------------------------------------------------------

enum ColourMode { Bitmap = 0, Grayscale = 1, Indexed = 2, RGB = 3, CMYK = 4, Multichannel = 7, Duotone = 8, Lab = 9 };

void labToRgb(double L, double a, double b, uint8_t out[3]) {
    double fy = (L + 16) / 116, fx = fy + a / 500, fz = fy - b / 200;
    auto inv = [](double t) { return t > 6.0 / 29 ? t * t * t : 3.0 * (6.0 / 29) * (6.0 / 29) * (t - 4.0 / 29); };
    double X = 0.95047 * inv(fx), Y = 1.0 * inv(fy), Z = 1.08883 * inv(fz);
    double r = 3.2406 * X - 1.5372 * Y - 0.4986 * Z, g = -0.9689 * X + 1.8758 * Y + 0.0415 * Z, bl = 0.0557 * X - 0.2040 * Y + 1.0570 * Z;
    out[0] = encodeLinear(float(r)); out[1] = encodeLinear(float(g)); out[2] = encodeLinear(float(bl));
}

/// Premultiplied RGBA from the decoded planes of a layer or the composite.
std::shared_ptr<Image> assemble(int mode, int width, int height, const std::map<int, std::vector<uint8_t>>& planes, const std::vector<uint8_t>& palette,
                                const CmykToSrgb* cmykProfile = nullptr) {
    auto image = std::make_shared<Image>(width, height);
    auto plane = [&](int id) -> const uint8_t* { auto it = planes.find(id); return it == planes.end() ? nullptr : it->second.data(); };
    const uint8_t *c0 = plane(0), *c1 = plane(1), *c2 = plane(2), *c3 = plane(3), *alpha = plane(-1);
    std::vector<uint8_t> inks, converted;
    for (int y = 0; y < height; y++) {
        uint8_t* p = image->row(y);
        // CMYK through the file's own profile, a row at a time.
        if (mode == CMYK && cmykProfile) {
            inks.resize(size_t(width) * 4);
            converted.resize(size_t(width) * 3);
            for (int x = 0; x < width; x++) {
                const size_t i = size_t(y) * width + size_t(x);
                inks[size_t(x) * 4] = c0 ? c0[i] : 255; inks[size_t(x) * 4 + 1] = c1 ? c1[i] : 255;
                inks[size_t(x) * 4 + 2] = c2 ? c2[i] : 255; inks[size_t(x) * 4 + 3] = c3 ? c3[i] : 255;
            }
            cmykProfile->convert(inks.data(), converted.data(), size_t(width));
            for (int x = 0; x < width; x++, p += 4) {
                const unsigned a = alpha ? alpha[size_t(y) * width + size_t(x)] : 255;
                for (int c = 0; c < 3; c++) p[c] = uint8_t((converted[size_t(x) * 3 + size_t(c)] * a + 127) / 255);
                p[3] = uint8_t(a);
            }
            continue;
        }
        for (int x = 0; x < width; x++, p += 4) {
            const size_t i = size_t(y) * width + size_t(x);
            uint8_t rgb[3] = {0, 0, 0};
            switch (mode) {
            case RGB: rgb[0] = c0 ? c0[i] : 0; rgb[1] = c1 ? c1[i] : 0; rgb[2] = c2 ? c2[i] : 0; break;
            case Grayscale: case Duotone: case Multichannel: rgb[0] = rgb[1] = rgb[2] = c0 ? c0[i] : 0; break;
            case CMYK: {
                // Stored inverted (255 = no ink): the multiply is the naive conversion.
                unsigned c = c0 ? c0[i] : 255, m = c1 ? c1[i] : 255, ye = c2 ? c2[i] : 255, k = c3 ? c3[i] : 255;
                rgb[0] = uint8_t(c * k / 255); rgb[1] = uint8_t(m * k / 255); rgb[2] = uint8_t(ye * k / 255);
                break;
            }
            case Indexed: { unsigned idx = c0 ? c0[i] : 0; if (palette.size() >= 768) { rgb[0] = palette[idx]; rgb[1] = palette[256 + idx]; rgb[2] = palette[512 + idx]; } else rgb[0] = rgb[1] = rgb[2] = uint8_t(idx); break; }
            case Lab: labToRgb((c0 ? c0[i] : 0) * 100.0 / 255, (c1 ? c1[i] : 128) - 128.0, (c2 ? c2[i] : 128) - 128.0, rgb); break;
            default: rgb[0] = rgb[1] = rgb[2] = c0 ? c0[i] : 0; break;
            }
            const unsigned a = alpha ? alpha[i] : 255;
            for (int c = 0; c < 3; c++) p[c] = uint8_t((rgb[c] * a + 127) / 255);
            p[3] = uint8_t(a);
        }
    }
    return image;
}

// ---- Layer records --------------------------------------------------------------------------------

/// One side of a rectangle whose corners came from the file. The subtraction is 64-bit because `right -
/// left` on two raw i32 overflows, and the result is clamped to what a buffer can hold, so a rectangle can
/// never size an allocation or a loop beyond the pixel format's limit.
int extentOf(int low, int high) { return int(std::clamp<int64_t>(int64_t(high) - int64_t(low), 0, maxImageSide)); }

struct MaskRecord {
    bool present = false; int top = 0, left = 0, bottom = 0, right = 0; uint8_t defaultColour = 0; uint8_t flags = 0;
    // With a vector mask too, the painted mask is the "real" one (channel -3) with its own rectangle, and
    // the -2 plane is the vector mask rendered (flag bit 3).
    bool real = false; int realTop = 0, realLeft = 0, realBottom = 0, realRight = 0; uint8_t realDefault = 0, realFlags = 0;
    std::vector<uint8_t> section;   // the whole mask section, as stored
    int width() const { return extentOf(left, right); }
    int height() const { return extentOf(top, bottom); }
    int realWidth() const { return extentOf(realLeft, realRight); }
    int realHeight() const { return extentOf(realTop, realBottom); }
};

struct Record {
    int top = 0, left = 0, bottom = 0, right = 0;
    std::vector<Channel> channels;
    std::string blend = "norm";
    uint8_t opacity = 255, clipping = 0, flags = 0, fillOpacity = 255;
    MaskRecord mask;
    std::string name;
    int section = 0;                                   // lsct: 1 open folder, 2 closed folder, 3 the folder's end marker
    std::map<std::string, std::pair<const uint8_t*, size_t>> blocks;   // tagged blocks by key
    std::vector<std::pair<std::string, std::pair<const uint8_t*, size_t>>> ordered;   // and in file order
    std::vector<uint8_t> blendingRanges;
    int width() const { return extentOf(left, right); }
    int height() const { return extentOf(top, bottom); }
};

bool psbLongKey(const std::string& key) {
    static const std::set<std::string> keys{"LMsk", "Lr16", "Lr32", "Layr", "Mt16", "Mt32", "Mtrn", "Alph", "FMsk", "lnk2", "FEid", "FXid", "PxSD", "cinf"};
    return keys.count(key) > 0;
}

/// Tagged blocks (`8BIM`/`8B64`, key, length, data) up to `end`; `pad` rounds the data length.
void readTaggedBlocks(Reader& r, size_t end, bool psb, size_t pad, std::map<std::string, std::pair<const uint8_t*, size_t>>& blocks,
                      std::vector<std::pair<std::string, std::pair<const uint8_t*, size_t>>>* ordered = nullptr) {
    while (r.position() + 12 <= end) {
        std::string sig = r.chars(4);
        if (sig != "8BIM" && sig != "8B64") break;
        std::string key = r.chars(4);
        uint64_t len = (psb && psbLongKey(key)) ? r.u64() : r.u32();
        if (len > r.remaining()) throw Truncated{};
        const uint8_t* data = r.bytes(size_t(len));
        blocks[key] = {data, size_t(len)};
        if (ordered) ordered->push_back({key, {data, size_t(len)}});
        if (pad > 1 && len % pad) r.skip(size_t(pad - len % pad));
    }
    r.seek(end);
}

Record readRecord(Reader& r, bool psb) {
    Record rec;
    rec.top = r.i32(); rec.left = r.i32(); rec.bottom = r.i32(); rec.right = r.i32();
    // The rectangle bounds every plane allocated for this layer, so it gets the canvas's own limit instead
    // of being trusted. Clamping instead would import a plausible-looking layer from a nonsense rectangle.
    if (int64_t(rec.right) - rec.left > maxImageSide || int64_t(rec.bottom) - rec.top > maxImageSide) throw Truncated{};
    uint16_t channels = r.u16();
    if (channels > 64) throw Truncated{};
    for (int i = 0; i < channels; i++) { Channel c; c.id = r.i16(); c.length = r.length(psb); rec.channels.push_back(c); }
    if (r.chars(4) != "8BIM") throw Truncated{};
    rec.blend = r.chars(4);
    rec.opacity = r.u8(); rec.clipping = r.u8(); rec.flags = r.u8(); r.u8();
    uint32_t extra = r.u32();
    size_t extraEnd = r.position() + extra;
    if (extraEnd > r.position() + r.remaining()) throw Truncated{};
    uint32_t maskLen = r.u32();
    if (maskLen > r.remaining()) throw Truncated{};
    if (maskLen >= 18) {
        size_t maskEnd = r.position() + maskLen;
        { const uint8_t* all = r.bytes(maskLen); rec.mask.section.assign(all, all + maskLen); r.seek(maskEnd - maskLen); }
        rec.mask.present = true;
        rec.mask.top = r.i32(); rec.mask.left = r.i32(); rec.mask.bottom = r.i32(); rec.mask.right = r.i32();
        if (int64_t(rec.mask.right) - rec.mask.left > maxImageSide || int64_t(rec.mask.bottom) - rec.mask.top > maxImageSide) throw Truncated{};
        rec.mask.defaultColour = r.u8(); rec.mask.flags = r.u8();
        // The real mask's fields come right after the flags (Photoshop's order, which Patchy pinned; the
        // format document lists the parameters first). A parameters-only section (bit 4 without bit 3) has none.
        const bool parametersOnly = (rec.mask.flags & 0x18) == 0x10;
        if (maskLen >= 36 && !parametersOnly) {
            rec.mask.real = true;
            rec.mask.realFlags = r.u8(); rec.mask.realDefault = r.u8();
            rec.mask.realTop = r.i32(); rec.mask.realLeft = r.i32(); rec.mask.realBottom = r.i32(); rec.mask.realRight = r.i32();
            if (int64_t(rec.mask.realRight) - rec.mask.realLeft > maxImageSide || int64_t(rec.mask.realBottom) - rec.mask.realTop > maxImageSide) throw Truncated{};
        }
        r.seek(maskEnd);
    } else r.skip(maskLen);
    uint32_t rangesLen = r.u32();
    if (rangesLen > r.remaining()) throw Truncated{};
    const uint8_t* ranges = r.bytes(rangesLen);
    rec.blendingRanges.assign(ranges, ranges + rangesLen);
    rec.name = r.pascal(4);
    readTaggedBlocks(r, extraEnd, psb, 1, rec.blocks, &rec.ordered);
    auto luni = rec.blocks.find("luni");
    if (luni != rec.blocks.end()) { try { Reader u(luni->second.first, luni->second.second); rec.name = u.unicode(); } catch (Truncated&) {} }
    auto lsct = rec.blocks.find("lsct");
    if (lsct != rec.blocks.end() && lsct->second.second >= 4) { Reader s(lsct->second.first, lsct->second.second); rec.section = int(s.u32()); if (lsct->second.second >= 12) { s.chars(4); rec.blend = s.chars(4); } }
    auto iopa = rec.blocks.find("iOpa");
    if (iopa != rec.blocks.end() && iopa->second.second >= 1) rec.fillOpacity = iopa->second.first[0];
    return rec;
}

// ---- Photoshop settings as ours ---------------------------------------------------------------------

BlendMode blendFor(const std::string& key, bool* lossy) {
    static const std::map<std::string, BlendMode> exact{
        {"norm", BlendMode::Normal}, {"pass", BlendMode::Normal}, {"mul ", BlendMode::Multiply}, {"scrn", BlendMode::Screen}, {"over", BlendMode::Overlay},
        {"dark", BlendMode::Darken}, {"lite", BlendMode::Lighten}, {"diff", BlendMode::Difference}, {"div ", BlendMode::ColorDodge}, {"idiv", BlendMode::ColorBurn},
        {"hue ", BlendMode::Hue}, {"sat ", BlendMode::Saturation}, {"colr", BlendMode::Color}, {"lum ", BlendMode::Luminosity}};
    static const std::map<std::string, BlendMode> nearest{
        {"diss", BlendMode::Normal}, {"lbrn", BlendMode::ColorBurn}, {"dkCl", BlendMode::Darken}, {"lddg", BlendMode::Screen}, {"lgCl", BlendMode::Lighten},
        {"sLit", BlendMode::Overlay}, {"hLit", BlendMode::Overlay}, {"vLit", BlendMode::Overlay}, {"lLit", BlendMode::Overlay}, {"pLit", BlendMode::Overlay},
        {"hMix", BlendMode::Overlay}, {"smud", BlendMode::Difference}, {"fsub", BlendMode::Difference}, {"fdiv", BlendMode::Normal}};
    if (auto it = exact.find(key); it != exact.end()) { *lossy = false; return it->second; }
    *lossy = true;
    if (auto it = nearest.find(key); it != nearest.end()) return it->second;
    return BlendMode::Normal;
}

// ---- What is carried for PSD export ----------------------------------------------------------------

/// Image resources written back on PSD export. Left out: the resolution (ours), thumbnails, what indexes
/// layers or alpha channels by position (both are rewritten), the ID seed (layer ids are reassigned), and
/// a non-RGB file's colour profile and colour settings (the export is RGB).
bool carriedResource(uint16_t id, int mode) {
    // Resolution; thumbnails; layer state, groups, selection, group-enabled ids; ID seed; alpha channel names,
    // unicode names, ids, display info and old display info; transparency index.
    static const std::set<uint16_t> never{1005, 1033, 1036, 1024, 1026, 1069, 1072, 1044, 1006, 1045, 1053, 1077, 1007, 1047};
    // Colour profile and untagged flag; colour and duotone halftoning and transfer; duotone image info.
    static const std::set<uint16_t> colourBound{1039, 1041, 1013, 1014, 1016, 1017, 1018};
    if (never.count(id)) return false;
    if (mode != RGB && colourBound.count(id)) return false;
    return true;
}

/// Global tagged blocks written back: everything but the layer information itself (written anew) and
/// the merged image's 16/32-bit transparency.
bool carriedGlobalBlock(const std::string& key) {
    static const std::set<std::string> never{"Lr16", "Lr32", "Layr", "LMsk", "Mt16", "Mt32", "Mtrn", "Alph"};
    return !never.count(key);
}

/// Per-layer blocks written back: all but what NekoPhoto reads into its own model and writes itself.
bool smartObjectBlock(const std::string& key) { return key == "SoLd" || key == "SoLE" || key == "PlLd" || key == "plLd"; }

bool carriedLayerBlock(const std::string& key, bool modelledAdjustment, bool smartObject = false) {
    if (smartObject && smartObjectBlock(key)) return false;   // the instance keeps them (smartobject.h)
    static const std::set<std::string> never{"luni", "lsct", "lsdk", "iOpa", "lyid"};
    static const std::set<std::string> adjustments{"levl", "curv", "hue2", "expA", "grdm"};
    if (never.count(key)) return false;
    if (modelledAdjustment && adjustments.count(key)) return false;
    return true;
}

const char* blendDescription(const std::string& key) {
    static const std::map<std::string, const char*> names{{"diss", "Dissolve"}, {"lbrn", "Linear Burn"}, {"dkCl", "Darker Color"}, {"lddg", "Linear Dodge (Add)"}, {"lgCl", "Lighter Color"},
        {"sLit", "Soft Light"}, {"hLit", "Hard Light"}, {"vLit", "Vivid Light"}, {"lLit", "Linear Light"}, {"pLit", "Pin Light"}, {"hMix", "Hard Mix"}, {"smud", "Exclusion"}, {"fsub", "Subtract"}, {"fdiv", "Divide"}};
    auto it = names.find(key);
    return it == names.end() ? key.c_str() : it->second;
}

std::optional<AdjustmentSettings> levelsFrom(const uint8_t* data, size_t size) {
    Reader r(data, size);
    if (r.u16() != 2) return std::nullopt;
    AdjustmentSettings s = AdjustmentSettings::defaults(AdjustmentKind::Levels);
    for (int i = 0; i < 4 && r.remaining() >= 10; i++) {
        double black = r.u16(), white = r.u16(), outBlack = r.u16(), outWhite = r.u16(), gamma = r.u16() / 100.0;
        s.levels.ranges[size_t(i)] = LevelsRange{black, gamma > 0 ? gamma : 1, white, outBlack, outWhite}.normalized();
    }
    return s;
}

std::optional<AdjustmentSettings> curvesFrom(const uint8_t* data, size_t size) {
    Reader r(data, size);
    r.u8();
    if (r.u16() != 1) return std::nullopt;
    uint32_t mask = r.u32();
    AdjustmentSettings s = AdjustmentSettings::defaults(AdjustmentKind::Curves);
    // Bit 0 is the composite curve, bits 1..3 red, green, blue; the rest (alpha, spot) have no home here.
    for (int channel = 0; channel < 32 && r.remaining() >= 2; channel++) {
        if (!(mask & (1u << channel))) continue;
        uint16_t count = r.u16();
        std::vector<CurvePoint> points;
        for (int i = 0; i < count && r.remaining() >= 4; i++) { double out = r.u16(), in = r.u16(); points.push_back({in, out}); }
        if (channel < 4 && points.size() >= 2) s.curves.channels[size_t(channel)] = points;
    }
    return s.curves.isValid() ? std::optional<AdjustmentSettings>(s) : std::nullopt;
}

std::optional<AdjustmentSettings> hueSaturationFrom(const uint8_t* data, size_t size) {
    Reader r(data, size);
    if (r.u16() != 2) return std::nullopt;
    AdjustmentSettings s = AdjustmentSettings::defaults(AdjustmentKind::HueSaturation);
    s.hsv.colorize = r.u8() != 0;
    r.u8();
    double colorizeHue = r.i16(), colorizeSat = r.i16(), colorizeLight = r.i16();
    double hue = r.i16(), sat = r.i16(), light = r.i16();
    s.hsv.photoshopSaturation = true;
    if (s.hsv.colorize) { s.hsv.adjustments[0] = {colorizeHue, colorizeSat, colorizeLight}; return s; }
    if (hue != 0 || sat != 0 || light != 0) s.hsv.adjustments[0] = {hue, sat, light};
    for (int range = 1; range <= 6 && r.remaining() >= 14; range++) {
        double a = r.i16(), b = r.i16(), c = r.i16(), d = r.i16();
        double h = r.i16(), sa = r.i16(), l = r.i16();
        s.hsv.bands[range] = HueBand{a, b, c, d};
        if (h != 0 || sa != 0 || l != 0) s.hsv.adjustments[range] = {h, sa, l};
    }
    return s;
}

std::optional<AdjustmentSettings> exposureFrom(const uint8_t* data, size_t size) {
    Reader r(data, size);
    if (r.u16() != 1) return std::nullopt;
    AdjustmentSettings s = AdjustmentSettings::defaults(AdjustmentKind::Exposure);
    s.exposure = ExposureSettings{r.f32(), r.f32(), r.f32()}.normalized();
    return s;
}

// ---- Photoshop's other adjustment layers (adjustments_more.cpp); a block that does not read stays carried ----

std::optional<patchy::psd::DescriptorObject> versionedDescriptor(const uint8_t* data, size_t size) {
    try {
        patchy::psd::BigEndianReader r(std::span<const uint8_t>(data, size));
        if (r.read_u32() != 16) return std::nullopt;
        return patchy::psd::read_descriptor(r);
    } catch (std::exception&) { return std::nullopt; }
}

/// Brightness/Contrast: Photoshop 2026's descriptor ('CgEd') when it reads, else the legacy 'brit' (Patchy's rule).
std::optional<AdjustmentSettings> brightnessContrastFrom(const std::pair<const uint8_t*, size_t>* cged, const std::pair<const uint8_t*, size_t>* brit) {
    AdjustmentSettings s = AdjustmentSettings::defaults(AdjustmentKind::BrightnessContrast);
    if (cged)
        if (auto d = versionedDescriptor(cged->first, cged->second)) {
            const auto* b = patchy::psd::descriptor_value(*d, "Brgh");
            const auto* c = patchy::psd::descriptor_value(*d, "Cntr");
            if (b && c && b->type == patchy::psd::DescriptorValue::Type::Integer && c->type == patchy::psd::DescriptorValue::Type::Integer) {
                s.brightnessContrast.legacy = patchy::psd::descriptor_bool(*d, "useLegacy", false);
                s.brightnessContrast.brightness = b->integer_value;
                s.brightnessContrast.contrast = c->integer_value;
                s.brightnessContrast = s.brightnessContrast.normalized();
                return s;
            }
        }
    if (brit && brit->second >= 4) {
        Reader r(brit->first, brit->second);
        s.brightnessContrast.legacy = true;
        s.brightnessContrast.brightness = r.i16();
        s.brightnessContrast.contrast = r.i16();
        s.brightnessContrast = s.brightnessContrast.normalized();
        return s;
    }
    return std::nullopt;
}

std::optional<AdjustmentSettings> simpleAdjustmentFrom(const std::string& key, const uint8_t* data, size_t size) {
    try {
        Reader r(data, size);
        if (key == "nvrt") return AdjustmentSettings::defaults(AdjustmentKind::Invert);
        if (key == "post") {
            AdjustmentSettings s = AdjustmentSettings::defaults(AdjustmentKind::Posterize);
            s.posterize.levels = std::clamp(int(r.u16()), 2, 255);
            return s;
        }
        if (key == "thrs") {
            AdjustmentSettings s = AdjustmentSettings::defaults(AdjustmentKind::Threshold);
            s.threshold.level = std::clamp(int(r.u16()), 1, 255);
            return s;
        }
        if (key == "blnc") {
            // Shadows, midtones, highlights: cyan-red, magenta-green, yellow-blue each; then Preserve Luminosity.
            AdjustmentSettings s = AdjustmentSettings::defaults(AdjustmentKind::ColorBalance);
            for (auto& range : s.colorBalance.ranges) for (double& v : range) v = std::clamp(int(r.i16()), -100, 100);
            s.colorBalance.preserveLuminosity = r.remaining() > 0 && r.u8() != 0;
            return s;
        }
        if (key == "mixr") {
            // Version, monochrome, then per output channel: red, green, blue, (CMYK's fourth), constant, in percent.
            if (r.u16() != 1) return std::nullopt;
            AdjustmentSettings s = AdjustmentSettings::defaults(AdjustmentKind::ChannelMixer);
            s.channelMixer.monochrome = r.u16() != 0;
            for (int out = 0; out < 3 && r.remaining() >= 10; out++) {
                auto& row = s.channelMixer.rows[size_t(out)];
                row[0] = r.i16(); row[1] = r.i16(); row[2] = r.i16(); r.i16(); row[3] = r.i16();
                for (double& v : row) v = std::clamp(v, -200.0, 200.0);
            }
            // Monochrome keeps its grey in the first record.
            if (s.channelMixer.monochrome) s.channelMixer.rows[3] = s.channelMixer.rows[0];
            return s;
        }
        if (key == "selc") {
            // Version, method (0 relative, 1 absolute), then ten CMYK records: a reserved one, then reds .. blacks.
            if (r.u16() != 1) return std::nullopt;
            AdjustmentSettings s = AdjustmentSettings::defaults(AdjustmentKind::SelectiveColor);
            s.selectiveColor.absolute = r.u16() == 1;
            for (int i = 0; i < 4; i++) r.i16();
            for (auto& range : s.selectiveColor.ranges) for (double& v : range) v = std::clamp(int(r.i16()), -100, 100);
            return s;
        }
        if (key == "phfl") {
            // Version 2 (a colour space and four components) or 3 (XYZ); density in percent; Preserve Luminosity.
            const uint16_t version = r.u16();
            AdjustmentSettings s = AdjustmentSettings::defaults(AdjustmentKind::PhotoFilter);
            if (version == 3) {
                // XYZ as three 32-bit numbers. Their scale is not documented: taken as fixed 16.16 when that gives a Y
                // in range, else as hundredths (unchecked against Photoshop; docs/adjustment-layers.md).
                double xyz[3];
                for (double& v : xyz) v = double(int32_t(r.u32()));
                double k = 65536;
                if (!(xyz[1] / k > 0.001 && xyz[1] / k <= 1.2)) k = xyz[1] > 0 && xyz[1] <= 120 ? 100 : 10000;
                const double X = xyz[0] / k, Y = xyz[1] / k, Z = xyz[2] / k;
                // D50 XYZ (Photoshop's connection space) to linear sRGB, then encoded.
                double lin[3] = {3.1338561 * X - 1.6168667 * Y - 0.4906146 * Z, -0.9787684 * X + 1.9161415 * Y + 0.0334540 * Z,
                                 0.0719453 * X - 0.2289914 * Y + 1.4052427 * Z};
                for (double& v : lin) { v = std::clamp(v, 0.0, 1.0); v = v <= 0.0031308 ? 12.92 * v : 1.055 * std::pow(v, 1 / 2.4) - 0.055; }
                s.photoFilter.color = {lin[0], lin[1], lin[2]};
            } else if (version == 2) {
                const uint16_t space = r.u16();
                const double a = r.u16() / 65535.0, b = r.u16() / 65535.0, c = r.u16() / 65535.0;
                r.u16();
                if (space != 0) return std::nullopt;   // RGB only here
                s.photoFilter.color = {a, b, c};
            } else return std::nullopt;
            s.photoFilter.density = std::clamp(double(r.u32()), 0.0, 100.0);
            s.photoFilter.preserveLuminosity = r.remaining() > 0 && r.u8() != 0;
            return s;
        }
        if (key == "clrL") {
            // A version, then Photoshop's descriptor holding the LUT file itself (LUT3DFileData, its LUTFormat) or an
            // ICC profile ('profile'), its name and Dither.
            if (r.u16() != 1) return std::nullopt;
            auto d = versionedDescriptor(data + 2, size - 2);
            if (!d) return std::nullopt;
            AdjustmentSettings s = AdjustmentSettings::defaults(AdjustmentKind::ColorLookup);
            auto raw = [&](const char* k) -> std::vector<uint8_t> {
                const auto* v = patchy::psd::descriptor_value(*d, k);
                return v && v->type == patchy::psd::DescriptorValue::Type::Raw ? v->raw_value : std::vector<uint8_t>{};
            };
            auto text = [&](const char* k) {
                const auto* v = patchy::psd::descriptor_value(*d, k);
                return v && v->type == patchy::psd::DescriptorValue::Type::String ? v->string_value : std::string();
            };
            const std::vector<uint8_t> lut = raw("LUT3DFileData");
            const std::vector<uint8_t> profile = raw("profile");
            s.colorLookup.dither = patchy::psd::descriptor_bool(*d, "Dthr", false);
            s.colorLookup.name = text("LUT3DFileName");
            if (s.colorLookup.name.empty()) s.colorLookup.name = text("Nm  ");
            if (!lut.empty()) {
                std::string body(lut.begin(), lut.end());
                const auto* fmt = patchy::psd::descriptor_value(*d, "LUTFormat");
                const std::string f = fmt && fmt->type == patchy::psd::DescriptorValue::Type::Enum ? fmt->enum_value : std::string();
                s.colorLookup.format = f.find("3DL") != std::string::npos ? "3dl"
                    : f.find("CUBE") != std::string::npos || body.find("LUT_3D_SIZE") != std::string::npos || body.find("LUT_1D_SIZE") != std::string::npos ? "cube" : "3dl";
                s.colorLookup.data = std::move(body);
            } else if (!profile.empty()) {
                s.colorLookup.format = "icc";
                s.colorLookup.data = toBase64(profile);
            }
            return s;
        }
        if (key == "blwh" || key == "vibA") {
            auto d = versionedDescriptor(data, size);
            if (!d) return std::nullopt;
            auto integer = [&](const char* k, double fallback) {
                const auto* v = patchy::psd::descriptor_value(*d, k);
                return v && v->type == patchy::psd::DescriptorValue::Type::Integer ? double(v->integer_value) : fallback;
            };
            if (key == "vibA") {
                AdjustmentSettings s = AdjustmentSettings::defaults(AdjustmentKind::Vibrance);
                s.vibrance = {std::clamp(integer("vibrance", 0), -100.0, 100.0), std::clamp(integer("Strt", 0), -100.0, 100.0)};
                return s;
            }
            AdjustmentSettings s = AdjustmentSettings::defaults(AdjustmentKind::BlackWhite);
            const char* keys[6] = {"Rd  ", "Yllw", "Grn ", "Cyn ", "Bl  ", "Mgnt"};
            for (size_t i = 0; i < 6; i++) s.blackWhite.weights[i] = std::clamp(integer(keys[i], s.blackWhite.weights[i]), -200.0, 300.0);
            s.blackWhite.tint = patchy::psd::descriptor_bool(*d, "useTint", false);
            if (const auto* tint = patchy::psd::descriptor_object(*d, "tintColor"))
                s.blackWhite.tintColor = AdjustmentColor{patchy::psd::descriptor_number(*tint, "Rd  ", 225) / 255, patchy::psd::descriptor_number(*tint, "Grn ", 211) / 255,
                                                         patchy::psd::descriptor_number(*tint, "Bl  ", 179) / 255}.clamped();
            return s;
        }
    } catch (...) {}
    return std::nullopt;
}

std::optional<AdjustmentSettings> gradientMapFrom(const uint8_t* data, size_t size) {
    Reader r(data, size);
    if (r.u16() != 1) return std::nullopt;
    AdjustmentSettings s = AdjustmentSettings::defaults(AdjustmentKind::GradientMap);
    s.gradientMap.reversed = r.u8() != 0;
    r.u8();
    r.unicode();
    uint16_t stops = r.u16();
    // The first and last colour stops give the ends of the ramp; the ramp in between is ours.
    std::optional<AdjustmentColor> first, last;
    for (int i = 0; i < stops && r.remaining() >= 18; i++) {
        r.i32(); r.i32(); r.i16();
        double cr = r.u16() / 65535.0, cg = r.u16() / 65535.0, cb = r.u16() / 65535.0; r.u16();
        AdjustmentColor colour{cr, cg, cb};
        if (!first) first = colour;
        last = colour;
    }
    if (first) s.gradientMap.shadows = *first;
    if (last) s.gradientMap.highlights = *last;
    return s;
}

/// A solid colour fill layer's colour, from its descriptor.
std::optional<AdjustmentColor> solidColourFrom(const uint8_t* data, size_t size) {
    try {
        Reader r(data, size);
        Descriptor d = readDescriptor(r);
        const Descriptor* colour = d.item("Clr ");
        if (!colour) return std::nullopt;
        auto channel = [&](const char* key) { const Descriptor* v = colour->item(key); return v ? std::clamp(v->number / 255.0, 0.0, 1.0) : 0.0; };
        return AdjustmentColor{channel("Rd  "), channel("Grn "), channel("Bl  ")};
    } catch (Truncated&) { return std::nullopt; }
}

/// The text of a type layer, from its descriptor.
std::optional<std::string> textFrom(const uint8_t* data, size_t size) {
    try {
        Reader r(data, size);
        if (r.u16() != 1) return std::nullopt;
        for (int i = 0; i < 6; i++) r.f64();
        if (r.u16() != 50) return std::nullopt;
        Descriptor d = readDescriptor(r);
        const Descriptor* text = d.item("Txt ");
        return text ? std::optional<std::string>(text->text) : std::nullopt;
    } catch (Truncated&) { return std::nullopt; }
}

std::string jsonEscape(const std::string& s) {
    std::string out;
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { out += '\\'; out += char(c); }
        else if (c < 0x20) { char buf[8]; std::snprintf(buf, sizeof buf, "\\u%04x", c); out += buf; }
        else out += char(c);
    }
    return out;
}

} // namespace

// ---- The import ---------------------------------------------------------------------------------------

std::optional<PsdImport> importPsd(const std::string& path, std::string* error, const PsdImportOptions& options) {
    std::ifstream in(path, std::ios::binary);
    if (!in) { if (error) *error = "The file could not be opened."; return std::nullopt; }
    std::vector<uint8_t> file((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return importPsdBytes(file, error, options);
}

namespace {

/// A smart object source's contents as an image: an embedded PSD/PSB through this importer (Photoshop's merged
/// image when it is real, else our render of its layers), PNG directly, anything else through the app's hook.
ImagePtr decodeSource(const SmartObjectSource& source, const PsdImportOptions& options) {
    if (source.kind != SmartObjectSource::Kind::Embedded || !source.bytes || source.bytes->empty()) return nullptr;
    const std::vector<uint8_t>& bytes = *source.bytes;
    const bool psdFile = bytes.size() >= 4 && std::memcmp(bytes.data(), "8BPS", 4) == 0;
    if (psdFile) {
        if (options.depth + 1 >= psdSmartObjectDepthLimit) return nullptr;
        PsdImportOptions inner = options;
        inner.depth++;
        std::string error;
        auto nested = importPsdBytes(bytes, &error, inner);
        if (!nested) return nullptr;
        if (nested->realComposite && nested->composite) return nested->composite;
        return renderFlattened(nested->document);
    }
    if (bytes.size() >= 8 && std::memcmp(bytes.data(), "\x89PNG", 4) == 0) return decodePngImage(bytes.data(), bytes.size());
    if (options.decodeImage) return options.decodeImage(bytes, source.fileType, source.fileName);
    return nullptr;
}

} // namespace

std::optional<PsdImport> importPsdBytes(const std::vector<uint8_t>& file, std::string* error, const PsdImportOptions& options) {
    try {
        Reader r(file.data(), file.size());
        if (r.chars(4) != "8BPS") { if (error) *error = "Not a Photoshop file (no 8BPS signature)."; return std::nullopt; }
        const uint16_t version = r.u16();
        if (version != 1 && version != 2) { if (error) *error = "Unknown Photoshop file version."; return std::nullopt; }
        const bool psb = version == 2;
        r.skip(6);
        const int channels = r.u16();
        const uint32_t height = r.u32(), width = r.u32();
        const int depth = r.u16(), mode = r.u16();
        if (width < 1 || height < 1 || width > 30000 || height > 30000) { if (error) *error = "The canvas size is outside the supported range (up to 30,000 pixels per side)."; return std::nullopt; }
        if ((long long)width * height > Document::pixelBudget) { if (error) *error = "The canvas exceeds the 100-megapixel budget."; return std::nullopt; }
        PsdImport result;
        std::vector<std::string>& notes = result.notes;
        if (depth != 8) notes.push_back(std::to_string(depth) + "-bit channels were reduced to 8 bits.");
        if (mode == CMYK) notes.push_back("CMYK colour was converted with a plain formula, not a colour profile.");
        else if (mode == Lab) notes.push_back("Lab colour was converted to sRGB.");
        else if (mode == Indexed || mode == Duotone || mode == Multichannel || mode == Bitmap) notes.push_back("The file's colour mode (" + std::string(mode == Indexed ? "indexed" : mode == Duotone ? "duotone" : mode == Multichannel ? "multichannel" : "bitmap") + ") was converted to RGB.");
        else if (mode != RGB && mode != Grayscale) { if (error) *error = "Unsupported colour mode " + std::to_string(mode) + "."; return std::nullopt; }

        // Colour mode data (a palette for indexed files).
        uint32_t colourDataLen = r.u32();
        std::vector<uint8_t> palette;
        if (colourDataLen) { const uint8_t* p = r.bytes(colourDataLen); if (mode == Indexed && colourDataLen >= 768) palette.assign(p, p + 768); }

        // Image resources: the resolution is ours; the rest is carried for PSD export (psd_carry.h).
        Document document{int(width), int(height)};
        auto docCarry = std::make_shared<PsdDocumentCarry>();
        std::shared_ptr<const CmykToSrgb> cmykProfile;
        docCarry->width = int(width); docCarry->height = int(height);
        uint32_t resourcesLen = r.u32();
        size_t resourcesEnd = r.position() + resourcesLen;
        while (r.position() + 12 <= resourcesEnd) {
            if (r.chars(4) != "8BIM") break;
            uint16_t id = r.u16();
            std::string resourceName = r.pascal(2);
            uint32_t len = r.u32();
            size_t dataStart = r.position();
            if (len <= r.remaining() && carriedResource(id, mode)) {
                const uint8_t* p = r.bytes(len);
                docCarry->resources.push_back({id, resourceName, std::vector<uint8_t>(p, p + len)});
                r.seek(dataStart);
            }
            if (id == 0x03ED && len >= 4) { double hres = r.u32() / 65536.0; if (hres > 0 && hres < 100000) document.resolution = hres; }
            if (id == 1057 && len >= 5) { r.u32(); result.realComposite = r.u8() != 0; }   // version info: hasRealMergedData
            if (id == 1039 && mode == CMYK && len <= r.remaining()) {
                // The ICC profile: a CMYK file's colours go to sRGB through it.
                const uint8_t* icc = r.bytes(len);
                cmykProfile = CmykToSrgb::fromProfile(std::vector<uint8_t>(icc, icc + len));
                r.seek(dataStart);
            }
            r.seek(dataStart + len + (len & 1));
        }
        r.seek(resourcesEnd);
        for (auto& n : notes)
            if (cmykProfile && n.rfind("CMYK colour was converted", 0) == 0) n = "CMYK colour was converted to sRGB through the file's own colour profile.";

        // Layer and mask information.
        const uint64_t layerMaskLen = r.length(psb);
        const size_t layerMaskEnd = r.position() + size_t(layerMaskLen);
        std::vector<Record> records;
        bool transparencyFirst = false;
        size_t channelDataStart = 0;
        auto readLayerInfo = [&](Reader& li) {
            int16_t count = li.i16();
            transparencyFirst = count < 0;
            count = int16_t(std::abs(int(count)));
            if (count > Document::maxLayers) throw Truncated{};
            for (int i = 0; i < count; i++) records.push_back(readRecord(li, psb));
            channelDataStart = li.position();
        };
        if (layerMaskLen > 0) {
            const uint64_t layerInfoLen = r.length(psb);
            const size_t layerInfoStart = r.position();
            if (layerInfoLen > 0) { Reader li(file.data(), file.size()); li.seek(layerInfoStart); readLayerInfo(li); }
            r.seek(layerInfoStart + size_t(layerInfoLen));
            // Global layer mask info, then additional blocks: 16- and 32-bit files keep their layers there.
            if (r.position() + 4 <= layerMaskEnd) { uint32_t globalLen = r.u32(); r.skip(globalLen); }
            std::map<std::string, std::pair<const uint8_t*, size_t>> globalBlocks;
            std::vector<std::pair<std::string, std::pair<const uint8_t*, size_t>>> globalOrder;
            if (r.position() < layerMaskEnd) readTaggedBlocks(r, layerMaskEnd, psb, 4, globalBlocks, &globalOrder);
            for (auto& [key, data] : globalOrder) {
                if (carriedGlobalBlock(key) && !((psb || depth != 8) && (key == "FEid" || key == "FXid"))) docCarry->globals.push_back({key, std::vector<uint8_t>(data.first, data.first + data.second)});
                // Smart object sources: the linked-file blocks' embedded (and linked) files.
                if (key == "lnk2" || key == "lnkD" || key == "lnk3" || key == "lnkE")
                    for (SmartObjectSource& s : parsePsdLinkBlock(std::vector<uint8_t>(data.first, data.first + data.second))) {
                        if (s.id.empty() || document.smartObjects.count(s.id)) continue;
                        s.psdBlock = key;
                        s.image = decodeSource(s, options);
                        if (s.image) { s.width = s.image->width(); s.height = s.image->height(); }
                        const std::string id = s.id;
                        document.smartObjects[id] = std::make_shared<const SmartObjectSource>(std::move(s));
                    }
            }
            if (records.empty()) {
                for (const char* key : {"Lr16", "Lr32"}) {
                    auto it = globalBlocks.find(key);
                    if (it == globalBlocks.end()) continue;
                    Reader li(file.data(), file.size());
                    li.seek(size_t(it->second.first - file.data()));
                    readLayerInfo(li);
                    break;
                }
            }
        }
        r.seek(layerMaskEnd);
        // A project holds a gigapixel of layers (and as much of masks); refuse more before decoding any of it.
        long long layerTotal = 0, maskTotal = 0;
        for (const Record& rec : records) {
            layerTotal += (long long)rec.width() * rec.height();
            maskTotal += (long long)rec.mask.width() * rec.mask.height();
        }
        if (layerTotal > Document::projectPixelBudget || maskTotal > Document::projectPixelBudget) {
            if (error) *error = "The layers total " + std::to_string(std::max(layerTotal, maskTotal) / 1000000) + " megapixels; a project holds up to 1,000.";
            return std::nullopt;
        }

        // Channel image data follows the records, one channel after another in record order.
        auto decodeRecordChannels = [&](const Record& rec, std::map<int, std::vector<uint8_t>>& planes, std::map<int, std::vector<uint8_t>>& maskPlanes, size_t& cursor,
                                        std::vector<std::pair<int, std::vector<uint8_t>>>& maskRaw) {
            for (const Channel& c : rec.channels) {
                // Subtraction, not addition: a PSB's channel length is a full 64-bit field, so `cursor +
                // c.length` wraps and a wrapped sum passes the test while the reader runs off the file.
                if (c.length < 2 || cursor > file.size() || c.length > file.size() - cursor) throw Truncated{};
                Reader ch(file.data() + cursor, size_t(c.length));
                int compression = ch.u16();
                const uint8_t* data = file.data() + cursor + 2;
                size_t size = size_t(c.length) - 2;
                int w = rec.width(), h = rec.height();
                if (c.id == -2) { w = rec.mask.width(); h = rec.mask.height(); }
                if (c.id == -3) { w = rec.mask.realWidth(); h = rec.mask.realHeight(); }
                if (c.id == -2 || c.id == -3) maskRaw.push_back({c.id, std::vector<uint8_t>(file.data() + cursor, file.data() + cursor + size_t(c.length))});
                std::vector<uint8_t> plane;
                std::string why;
                if (w > 0 && h > 0 && decodePlane(data, size, compression, w, h, depth, psb, plane, &why)) (c.id == -2 || c.id == -3 ? maskPlanes : planes)[c.id] = std::move(plane);
                else if (w > 0 && h > 0) notes.push_back("Layer \"" + rec.name + "\": a channel could not be read (" + why + ").");
                cursor += size_t(c.length);
            }
        };

        // Records run bottom to top. Folders arrive as an end marker first, then their contents, then the
        // folder itself; clipped layers sit directly above what they clip to.
        struct OpenGroup { size_t firstChild; std::vector<Uuid> members; const Record* end = nullptr; };
        std::vector<OpenGroup> open;
        std::vector<Layer>& layers = document.layers;
        std::vector<std::optional<Uuid>> parents;   // the parent each layer was added under
        size_t cursor = channelDataStart;
        int effectsCount = 0, vectorMasks = 0, smartObjects = 0, editableSmartObjects = 0;
        std::map<std::string, int> lockedSmartObjects;
        // What the record holds that NekoPhoto does not model, bound to the layer's content as imported.
        auto carryFor = [&](const Record& rec, const Layer& layer, bool modelledAdjustment,
                            const std::vector<std::pair<int, std::vector<uint8_t>>>& maskRaw) -> std::shared_ptr<const PsdLayerCarry> {
            auto carry = std::make_shared<PsdLayerCarry>();
            for (auto& [key, data] : rec.ordered)
                if (carriedLayerBlock(key, modelledAdjustment, layer.smartObject.has_value())) carry->blocks.push_back({key, std::vector<uint8_t>(data.first, data.first + data.second)});
            carry->blendingRanges = rec.blendingRanges;
            carry->blendKey = rec.blend;
            carry->blendAs = int(layer.blendMode);
            carry->flags = rec.flags;
            carry->closedFolder = rec.section == 2;
            carry->opacity = rec.opacity;
            carry->fill = rec.fillOpacity;
            auto lyid = rec.blocks.find("lyid");
            if (lyid != rec.blocks.end() && lyid->second.second >= 4) { Reader id(lyid->second.first, 4); carry->layerId = id.u32(); }
            carry->contentHash = psdContentHash(layer.asset ? layer.asset->image.get() : nullptr);
            if (layer.adjustment) {
                // Settings as read, so an unchanged adjustment's own block goes back byte for byte.
                AdjustmentSettings read;
                if (AdjustmentSettings::parse(layer.adjustment->json, read)) carry->adjustmentJson = read.toJson();
            }
            carry->placement = layer.transform;
            if (!psb && depth == 8 && !rec.mask.section.empty()) {
                carry->maskData = rec.mask.section;
                carry->maskChannels = maskRaw;
                carry->maskHash = layer.mask ? psdMaskHash(layer.mask->asset.image.get(), layer.mask->enabled) : 0;
            }
            const bool plainBlend = rec.blend == "norm" || (rec.blend == "pass" && layer.isGroup);
            if (carry->blocks.empty() && carry->blendingRanges.empty() && carry->fill == 255 && carry->layerId == 0 && carry->maskData.empty()
                && plainBlend && (rec.flags & ~0x0A) == 0 && !carry->closedFolder) return nullptr;
            return carry;
        };
        for (const Record& rec : records) {
            std::map<int, std::vector<uint8_t>> planes, maskPlanes;
            std::vector<std::pair<int, std::vector<uint8_t>>> maskRaw;
            decodeRecordChannels(rec, planes, maskPlanes, cursor, maskRaw);
            const bool hidden = rec.flags & 2;
            std::optional<Uuid> parent = open.empty() ? std::nullopt : std::optional<Uuid>();
            bool lossyBlend = false;
            BlendMode blend = blendFor(rec.blend, &lossyBlend);
            // The user mask over a lw x lh grid at (lx, ly): Photoshop keeps it in its own rectangle with a default beyond.
            auto userMaskFor = [&](int lw, int lh, int lx, int ly) -> std::optional<LayerMask> {
                // The painted mask: the real one when there is one, else -2 (which may be the vector mask
                // rendered: then it stands in for it here, and the vector mask itself is carried).
                const bool real = rec.mask.real && maskPlanes.count(-3);
                // Without a painted mask, a plane "rendered from other data" (flag bit 3) is the vector mask baked,
                // unfeathered: the vector mask itself draws it here, and the section goes back as stored.
                if (!real && (rec.mask.flags & 0x08)) return std::nullopt;
                auto userMask = maskPlanes.find(real ? -3 : -2);
                if (!rec.mask.present || userMask == maskPlanes.end() || lw <= 0 || lh <= 0) return std::nullopt;
                const int top = real ? rec.mask.realTop : rec.mask.top, left = real ? rec.mask.realLeft : rec.mask.left;
                const int mw = real ? rec.mask.realWidth() : rec.mask.width(), mh = real ? rec.mask.realHeight() : rec.mask.height();
                auto mask = std::make_shared<GrayImage>(lw, lh, real ? rec.mask.realDefault : rec.mask.defaultColour);
                if (size_t(std::max(0, mw)) * size_t(std::max(0, mh)) > userMask->second.size()) return std::nullopt;
                for (int y = 0; y < mh; y++) {
                    const int ty = top + y - ly;
                    if (ty < 0 || ty >= lh) continue;
                    for (int x = 0; x < mw; x++) { const int tx = left + x - lx; if (tx >= 0 && tx < lw) mask->at(tx, ty) = userMask->second[size_t(y) * mw + size_t(x)]; }
                }
                LayerMask lm;
                lm.asset = MaskAsset::make(mask);
                lm.enabled = !((real ? rec.mask.realFlags : rec.mask.flags) & 2);
                return lm;
            };
            if (rec.section == 3) { open.push_back({layers.size(), {}, &rec}); continue; }   // a folder's end marker: its contents follow
            if (rec.section == 1 || rec.section == 2) {
                if (open.empty()) continue;
                OpenGroup group = open.back();
                open.pop_back();
                Layer folder(rec.name.empty() ? "Folder" : rec.name, document.size());
                folder.isGroup = true;
                folder.visible = !hidden;
                folder.opacity = rec.opacity / 255.0;
                folder.blendMode = blend;
                folder.passThrough = rec.blend == "pass";
                if (auto lm = userMaskFor(int(width), int(height), 0, 0)) folder.mask = lm;   // over the canvas, as our folders are
                folder.psdCarry = carryFor(rec, folder, false, maskRaw);
                if (group.end) {
                    auto carry = std::make_shared<PsdLayerCarry>(folder.psdCarry ? *folder.psdCarry : PsdLayerCarry{});
                    for (auto& [key, data] : group.end->ordered)
                        if (carriedLayerBlock(key, false)) carry->endBlocks.push_back({key, std::vector<uint8_t>(data.first, data.first + data.second)});
                    carry->endRanges = group.end->blendingRanges;
                    if (!folder.psdCarry) { carry->placement = folder.transform; carry->opacity = rec.opacity; carry->fill = rec.fillOpacity; carry->blendKey = rec.blend; carry->blendAs = int(folder.blendMode); carry->flags = rec.flags; carry->closedFolder = rec.section == 2; }
                    if (!carry->endBlocks.empty() || !carry->endRanges.empty()) folder.psdCarry = carry;
                }
                if (lossyBlend && rec.blend != "pass") notes.push_back("Folder \"" + folder.name + "\": blend mode " + blendDescription(rec.blend) + " has no counterpart; " + blendModeName(blend) + " was used.");
                for (const Uuid& id : group.members) if (Layer* l = document.find(id)) l->parentId = folder.id;
                layers.insert(layers.begin() + long(group.firstChild), folder);
                if (!open.empty()) open.back().members.push_back(folder.id);
                continue;
            }

            // Pixels, or what stands in for them.
            std::shared_ptr<Image> image;
            std::optional<LayerAdjustment> adjustment;
            std::string extraJson;
            auto block = [&](const char* key) -> const std::pair<const uint8_t*, size_t>* { auto it = rec.blocks.find(key); return it == rec.blocks.end() ? nullptr : &it->second; };
            if (rec.width() > 0 && rec.height() > 0 && !planes.empty()) image = assemble(mode, rec.width(), rec.height(), planes, palette, cmykProfile.get());
            std::optional<AdjustmentSettings> settings;
            if (auto b = block("levl")) settings = levelsFrom(b->first, b->second);
            else if (auto b = block("curv")) settings = curvesFrom(b->first, b->second);
            else if (auto b = block("hue2")) settings = hueSaturationFrom(b->first, b->second);
            else if (auto b = block("expA")) settings = exposureFrom(b->first, b->second);
            else if (auto b = block("grdm")) settings = gradientMapFrom(b->first, b->second);
            else if (block("CgEd") || block("brit")) settings = brightnessContrastFrom(block("CgEd"), block("brit"));
            if (!settings)
                for (const char* key : {"nvrt", "post", "thrs", "blnc", "mixr", "selc", "phfl", "blwh", "vibA", "clrL"})
                    if (auto b = block(key)) { settings = simpleAdjustmentFrom(key, b->first, b->second); break; }
            if (!settings) {
                static const std::map<std::string, const char*> others{{"brit", "Brightness/Contrast"}, {"blwh", "Black & White"}, {"vibA", "Vibrance"}, {"phfl", "Photo Filter"}, {"mixr", "Channel Mixer"},
                    {"clrL", "Color Lookup"}, {"nvrt", "Invert"}, {"post", "Posterize"}, {"thrs", "Threshold"}, {"selc", "Selective Color"}, {"blnc", "Color Balance"}};
                for (auto& [key, name] : others) if (block(key.c_str())) { notes.push_back("Layer \"" + rec.name + "\": " + name + " adjustment layers have no counterpart; it shows as an empty layer here and is written back to PSD as it was."); break; }
            }
            if (settings) adjustment = settings->toLayerAdjustment();
            if (!image && !adjustment) {
                if (auto b = block("SoCo")) {
                    if (auto colour = solidColourFrom(b->first, b->second)) {
                        image = std::make_shared<Image>(int(width), int(height));
                        image->fill(uint8_t(std::lround(colour->red * 255)), uint8_t(std::lround(colour->green * 255)), uint8_t(std::lround(colour->blue * 255)), 255);
                    }
                } else if (block("GdFl") || block("PtFl")) { notes.push_back("Layer \"" + rec.name + "\": a gradient or pattern fill; it shows as an empty layer here and is written back to PSD as it was."); }
            }
            std::optional<PsdTypeLayer> type;
            if (block("TySh")) {
                std::string why;
                type = readPhotoshopType(block("TySh")->first, block("TySh")->second, &why);
                // Without pixels only an empty type layer (a click with the Type tool, nothing typed) can be text.
                if (type && !image && !type->text.text.empty()) { type.reset(); why.clear(); }
                if (auto text = textFrom(block("TySh")->first, block("TySh")->second)) {
                    extraJson = "{\"psdText\":\"" + jsonEscape(*text) + "\"";
                    // Where Photoshop anchored the first baseline, so the first redraw here lands on it.
                    if (type) {
                        char anchor[128];
                        std::snprintf(anchor, sizeof anchor, ",\"psdTextAnchor\":[%.4f,%.4f]", type->anchorX, type->anchorY);
                        extraJson += anchor;
                        if (type->rotation != 0) { std::snprintf(anchor, sizeof anchor, ",\"psdTextRotation\":%.6f", type->rotation); extraJson += anchor; }
                    }
                    extraJson += "}";
                }
                if (!type) notes.push_back("Layer \"" + rec.name + "\": its text is " + (why.empty() ? std::string("without pixels") : why) + ", which NekoPhoto text cannot be; it shows as Photoshop drew it, and is Photoshop text again on PSD export while its pixels are unchanged.");
            }
            if (block("lfx2") || block("lrFX")) effectsCount++;
            if (block("vmsk") || block("vsms")) vectorMasks++;
            if (block("SoLd") || block("PlLd")) smartObjects++;
            // An adjustment or fill kind with no counterpart stays as an empty layer that carries it, as does an
            // empty layer (a divider, a layer never painted on), so the structure survives.

            Layer layer;
            if (image) {
                const bool solidFill = rec.width() == 0;
                layer = Layer(Asset::make(image, rec.name), Point(solidFill ? 0 : rec.left, solidFill ? 0 : rec.top));
            } else {
                layer = Layer(rec.name, document.size());
            }
            layer.name = rec.name.empty() ? "Layer" : rec.name;
            layer.visible = !hidden;
            layer.opacity = (rec.opacity / 255.0) * (rec.fillOpacity / 255.0);
            layer.blendMode = blend;
            layer.adjustment = adjustment;
            layer.extraJson = extraJson;
            if (type && !image) {
                // Empty text: a transparent pixel where Photoshop anchored it, so it is a text layer to type into.
                layer = Layer(Asset::make(std::make_shared<Image>(1, 1), rec.name), Point(std::floor(type->anchorX), std::floor(type->anchorY)));
                layer.name = rec.name.empty() ? "Layer" : rec.name;
                layer.visible = !hidden;
                layer.opacity = (rec.opacity / 255.0) * (rec.fillOpacity / 255.0);
                layer.blendMode = blend;
                layer.extraJson = extraJson;
            }
            if (type) {
                layer.text = type->text;
                layer.textImage = layer.asset->image;
                result.texts.push_back({layer.id, type->postScriptName, type->runPostScriptNames, type->leading, type->autoLeading});
            }
            if (lossyBlend) notes.push_back("Layer \"" + layer.name + "\": blend mode " + blendDescription(rec.blend) + " has no counterpart; " + blendModeName(blend) + " was used.");
            // The mask covers the layer's pixels (the canvas for a layer without any).
            if (auto lm = userMaskFor(image ? image->width() : int(width), image ? image->height() : int(height),
                                      image ? int(layer.transform.origin.x) : 0, image ? int(layer.transform.origin.y) : 0)) layer.mask = lm;
            // A placed layer: an instance of its source. Editable, its pixels become the source's image placed by
            // the quad (the mask keeps the place it had); otherwise it shows Photoshop's preview, locked.
            {
                std::optional<PsdPlacement> placement;
                SmartObjectInstance instance;
                for (const char* key : {"SoLd", "SoLE", "PlLd", "plLd"})
                    if (auto b = block(key)) {
                        instance.psdBlocks.push_back({key, std::vector<uint8_t>(b->first, b->first + b->second)});
                        if (!placement) placement = parsePsdPlacement(key, instance.psdBlocks.back().data);
                    }
                if (placement && image) {
                    instance.sourceId = placement->sourceId;
                    instance.placedId = placement->placedId;
                    instance.quad = placement->quad;
                    auto source = document.smartObjects.find(placement->sourceId);
                    const bool legacy = !block("SoLd") && !block("SoLE");
                    std::optional<LayerTransform> placed;
                    if (source != document.smartObjects.end() && source->second->image)
                        placed = transformForQuad(placement->quad, source->second->image->width(), source->second->image->height());
                    using Lock = SmartObjectInstance::Lock;
                    instance.lock = legacy ? Lock::Legacy : placement->warped ? Lock::Warp : placement->filtered ? Lock::Filters
                        : source == document.smartObjects.end() || source->second->kind == SmartObjectSource::Kind::Linked ? Lock::Linked
                        : !source->second->image ? Lock::Unreadable : placement->nonAffine || !placed ? Lock::Perspective : Lock::None;
                    std::optional<WarpedRaster> warped;
                    if (instance.lock == Lock::Filters && source != document.smartObjects.end() && source->second->kind == SmartObjectSource::Kind::Embedded
                        && source->second->image) {
                        // Smart Filters NekoPhoto draws: the contents placed (and warped), then the stack over them.
                        if (auto filtered = filteredSmartObjectRaster(docCarry->globals, instance, *source->second->image, placement->quad)) {
                            warped = WarpedRaster{filtered->image, LayerTransform(Point(filtered->x, filtered->y), Size(filtered->image->width(), filtered->image->height()))};
                            instance.lock = Lock::None;
                        }
                    }
                    if (!warped && placement->warp && (instance.lock == Lock::None || instance.lock == Lock::Perspective)) {
                        // A warp NekoPhoto draws: from the contents, through the mesh, onto the quad (any quad).
                        warped = renderWarpedImage(*source->second->image, *placement->warp, placement->quad);
                        instance.lock = warped ? Lock::None : Lock::Warp;
                    }
                    if (warped) {
                        const LayerTransform raster = layer.transform;
                        layer.asset = Asset::make(warped->image, layer.name);
                        layer.transform = warped->transform;
                        layer.transform.sampling = raster.sampling;
                        if (layer.mask && !layer.mask->placement) layer.mask->placement = raster;
                    } else if (!instance.locked()) {
                        const LayerTransform raster = layer.transform;
                        layer.asset = Asset::make(source->second->image, layer.name);
                        layer.transform = *placed;
                        layer.transform.sampling = raster.sampling;
                        if (layer.mask && !layer.mask->placement) layer.mask->placement = raster;
                    }
                    instance.placedTransform = layer.transform;
                    instance.placedWidth = layer.asset->image->width();
                    instance.placedHeight = layer.asset->image->height();
                    layer.smartImage = layer.asset->image;
                    layer.smartObject = std::move(instance);
                    if (layer.smartObject->locked()) lockedSmartObjects[smartObjectLockDescription(layer.smartObject->lock)]++;
                    else editableSmartObjects++;
                }
            }
            layer.psdCarry = carryFor(rec, layer, settings.has_value(), maskRaw);
            if (rec.clipping) {
                // Clipped to the nearest unclipped layer below it in the same folder.
                const size_t from = open.empty() ? 0 : open.back().firstChild;
                for (size_t i = layers.size(); i-- > from;) if (!layers[i].maskSourceId && !layers[i].isGroup) { layer.maskSourceId = layers[i].id; break; }
            }
            layers.push_back(layer);
            if (!open.empty()) open.back().members.push_back(layer.id);
        }
        if (effectsCount) notes.push_back(std::to_string(effectsCount) + " layer style(s) (shadows, glows, strokes, bevels, overlays) show as Photoshop draws them and are written back on PSD export; they cannot be edited here yet.");
        if (vectorMasks) notes.push_back(std::to_string(vectorMasks) + " vector mask(s) and shape(s) show as Photoshop draws them and follow their layers; their paths cannot be edited here yet.");
        if (editableSmartObjects) notes.push_back(std::to_string(editableSmartObjects) + " smart object(s) place their contents here: moving or scaling one resamples the original, and each stays a smart object on PSD export.");
        for (auto& [why, count] : lockedSmartObjects)
            notes.push_back(std::to_string(count) + " smart object(s) " + why + " show Photoshop's preview: they can be moved and scaled, and stay smart objects on PSD export.");
        (void)smartObjects;
        while (!open.empty()) open.pop_back();   // an unterminated folder: its members stay at the top level

        // The merged image: every channel one after another, one compression for all.
        {
            const int compression = r.u16();
            const int planeCount = channels;
            std::map<int, std::vector<uint8_t>> planes;
            const size_t bytesPer = size_t(depth) / 8, planeBytes = size_t(width) * height * bytesPer;
            const uint8_t* data = file.data() + r.position();
            size_t available = file.size() - r.position();
            const size_t planeTotal = static_cast<size_t>(planeCount);
            std::vector<std::vector<uint8_t>> decoded(planeTotal);
            bool ok = true;
            if (compression == 0) {
                for (int c = 0; c < planeCount && ok; c++) {
                    if (size_t(c + 1) * planeBytes > available) { ok = false; break; }
                    std::string why;
                    ok = decodePlane(data + size_t(c) * planeBytes, planeBytes, 0, int(width), int(height), depth, psb, decoded[size_t(c)], &why);
                }
            } else if (compression == 1) {
                // Row byte counts for every channel first, then the rows.
                Reader counts(data, available);
                // The counts must be in the file before anything is sized by them (a hostile file claimed gigabytes).
                if (size_t(planeCount) * height * (psb ? 4 : 2) > available) { ok = false; }
                std::vector<size_t> rows(ok ? size_t(planeCount) * height : 0);
                for (size_t i = 0; i < rows.size(); i++) rows[i] = psb ? counts.u32() : counts.u16();
                size_t offset = counts.position();
                for (int c = 0; c < planeCount && ok; c++) {
                    size_t total = 0;
                    for (uint32_t y = 0; y < height; y++) total += rows[size_t(c) * height + y];
                    if (offset + total > available) { ok = false; break; }
                    // A channel's rows as their own RLE stream: its own counts, then its data.
                    std::vector<uint8_t> stream;
                    stream.reserve(total + height * (psb ? 4 : 2));
                    for (uint32_t y = 0; y < height; y++) { size_t n = rows[size_t(c) * height + y]; if (psb) { stream.push_back(uint8_t(n >> 24)); stream.push_back(uint8_t(n >> 16)); } stream.push_back(uint8_t(n >> 8)); stream.push_back(uint8_t(n)); }
                    if (offset + total > available) { ok = false; break; }
                    stream.insert(stream.end(), data + offset, data + offset + total);
                    offset += total;
                    std::string why;
                    ok = decodePlane(stream.data(), stream.size(), 1, int(width), int(height), depth, psb, decoded[size_t(c)], &why);
                }
            } else ok = false;
            if (ok) {
                const int colourChannels = mode == RGB || mode == Lab ? 3 : mode == CMYK ? 4 : 1;
                for (int c = 0; c < planeCount; c++) {
                    if (c < colourChannels) planes[c] = std::move(decoded[size_t(c)]);
                    else if (c == colourChannels && transparencyFirst) planes[-1] = std::move(decoded[size_t(c)]);
                }
                if (mode == Bitmap) {
                    // One bit per pixel, 1 = black, rows padded to bytes.
                    std::vector<uint8_t> gray(size_t(width) * height);
                    const size_t rowBytes = (width + 7) / 8;
                    if (available >= rowBytes * height) for (uint32_t y = 0; y < height; y++) for (uint32_t x = 0; x < width; x++) gray[size_t(y) * width + x] = (data[y * rowBytes + x / 8] >> (7 - x % 8)) & 1 ? 0 : 255;
                    planes.clear(); planes[0] = std::move(gray);
                }
                // With transparency, Photoshop stores the merged colour matted against white: take the white out.
                if (auto alpha = planes.find(-1); alpha != planes.end() && mode != Bitmap && mode != Indexed) {
                    const std::vector<uint8_t>& a = alpha->second;
                    for (auto& [id, plane] : planes) {
                        if (id < 0 || plane.size() != a.size()) continue;
                        for (size_t i = 0; i < plane.size(); i++) {
                            const int A = a[i];
                            plane[i] = A == 0 ? 0 : uint8_t(std::clamp((int(plane[i]) - (255 - A)) * 255 / A, 0, 255));
                        }
                    }
                }
                result.composite = assemble(mode == Bitmap ? Grayscale : mode, int(width), int(height), planes, palette, cmykProfile.get());
            } else notes.push_back("The merged image could not be read; only the layers were imported.");
        }
        if (layers.empty()) {
            if (!result.composite) { if (error) *error = "The file has neither layers nor a readable merged image."; return std::nullopt; }
            Layer background(Asset::make(result.composite, "Background"), Point(0, 0));
            background.name = "Background";
            layers.push_back(background);
            if (records.empty()) notes.push_back("The file carries no layers (it was saved flattened); the merged image is the only layer.");
        }
        if (!docCarry->resources.empty() || !docCarry->globals.empty()) document.psdCarry = docCarry;
        result.document = std::move(document);
        return result;
    } catch (Truncated&) {
        if (error) *error = "The file ends early or has a structure this reader does not understand.";
        return std::nullopt;
    } catch (const std::bad_alloc&) {
        // A size in the file (a layer, a channel, a compressed stream) larger than memory allows.
        if (error) *error = "The file asks for more memory than is available.";
        return std::nullopt;
    } catch (const std::length_error&) {
        if (error) *error = "The file asks for more memory than is available.";
        return std::nullopt;
    }
}

} // namespace compositor
