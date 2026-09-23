#include "compositor/psd.h"
#include "photoshop.h"
#include "compositor/adjustments.h"
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
std::shared_ptr<Image> assemble(int mode, int width, int height, const std::map<int, std::vector<uint8_t>>& planes, const std::vector<uint8_t>& palette) {
    auto image = std::make_shared<Image>(width, height);
    auto plane = [&](int id) -> const uint8_t* { auto it = planes.find(id); return it == planes.end() ? nullptr : it->second.data(); };
    const uint8_t *c0 = plane(0), *c1 = plane(1), *c2 = plane(2), *c3 = plane(3), *alpha = plane(-1);
    for (int y = 0; y < height; y++) {
        uint8_t* p = image->row(y);
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
    int width() const { return extentOf(left, right); }
    int height() const { return extentOf(top, bottom); }
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
    int width() const { return extentOf(left, right); }
    int height() const { return extentOf(top, bottom); }
};

bool psbLongKey(const std::string& key) {
    static const std::set<std::string> keys{"LMsk", "Lr16", "Lr32", "Layr", "Mt16", "Mt32", "Mtrn", "Alph", "FMsk", "lnk2", "FEid", "FXid", "PxSD", "cinf"};
    return keys.count(key) > 0;
}

/// Tagged blocks (`8BIM`/`8B64`, key, length, data) up to `end`; `pad` rounds the data length.
void readTaggedBlocks(Reader& r, size_t end, bool psb, size_t pad, std::map<std::string, std::pair<const uint8_t*, size_t>>& blocks) {
    while (r.position() + 12 <= end) {
        std::string sig = r.chars(4);
        if (sig != "8BIM" && sig != "8B64") break;
        std::string key = r.chars(4);
        uint64_t len = (psb && psbLongKey(key)) ? r.u64() : r.u32();
        if (len > r.remaining()) throw Truncated{};
        const uint8_t* data = r.bytes(size_t(len));
        blocks[key] = {data, size_t(len)};
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
    if (maskLen >= 20) {
        size_t maskEnd = r.position() + maskLen;
        rec.mask.present = true;
        rec.mask.top = r.i32(); rec.mask.left = r.i32(); rec.mask.bottom = r.i32(); rec.mask.right = r.i32();
        if (int64_t(rec.mask.right) - rec.mask.left > maxImageSide || int64_t(rec.mask.bottom) - rec.mask.top > maxImageSide) throw Truncated{};
        rec.mask.defaultColour = r.u8(); rec.mask.flags = r.u8();
        r.seek(maskEnd);
    } else r.skip(maskLen);
    uint32_t rangesLen = r.u32();
    r.skip(rangesLen);
    rec.name = r.pascal(4);
    readTaggedBlocks(r, extraEnd, psb, 1, rec.blocks);
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

std::optional<PsdImport> importPsd(const std::string& path, std::string* error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) { if (error) *error = "The file could not be opened."; return std::nullopt; }
    std::vector<uint8_t> file((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
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

        // Image resources: the resolution is the one thing kept.
        Document document{int(width), int(height)};
        uint32_t resourcesLen = r.u32();
        size_t resourcesEnd = r.position() + resourcesLen;
        while (r.position() + 12 <= resourcesEnd) {
            if (r.chars(4) != "8BIM") break;
            uint16_t id = r.u16();
            r.pascal(2);
            uint32_t len = r.u32();
            size_t dataStart = r.position();
            if (id == 0x03ED && len >= 4) { double hres = r.u32() / 65536.0; if (hres > 0 && hres < 100000) document.resolution = hres; }
            r.seek(dataStart + len + (len & 1));
        }
        r.seek(resourcesEnd);

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
            if (r.position() < layerMaskEnd) readTaggedBlocks(r, layerMaskEnd, psb, 4, globalBlocks);
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
        auto decodeRecordChannels = [&](const Record& rec, std::map<int, std::vector<uint8_t>>& planes, std::map<int, std::vector<uint8_t>>& maskPlanes, size_t& cursor) {
            for (const Channel& c : rec.channels) {
                // Subtraction, not addition: a PSB's channel length is a full 64-bit field, so `cursor +
                // c.length` wraps and a wrapped sum passes the test while the reader runs off the file.
                if (c.length < 2 || cursor > file.size() || c.length > file.size() - cursor) throw Truncated{};
                Reader ch(file.data() + cursor, size_t(c.length));
                int compression = ch.u16();
                const uint8_t* data = file.data() + cursor + 2;
                size_t size = size_t(c.length) - 2;
                int w = rec.width(), h = rec.height();
                if (c.id == -2 || c.id == -3) { w = rec.mask.width(); h = rec.mask.height(); }
                std::vector<uint8_t> plane;
                std::string why;
                if (w > 0 && h > 0 && decodePlane(data, size, compression, w, h, depth, psb, plane, &why)) (c.id == -2 || c.id == -3 ? maskPlanes : planes)[c.id] = std::move(plane);
                else if (w > 0 && h > 0) notes.push_back("Layer \"" + rec.name + "\": a channel could not be read (" + why + ").");
                cursor += size_t(c.length);
            }
        };

        // Records run bottom to top. Folders arrive as an end marker first, then their contents, then the
        // folder itself; clipped layers sit directly above what they clip to.
        struct OpenGroup { size_t firstChild; std::vector<Uuid> members; };
        std::vector<OpenGroup> open;
        std::vector<Layer>& layers = document.layers;
        std::vector<std::optional<Uuid>> parents;   // the parent each layer was added under
        size_t cursor = channelDataStart;
        int effectsCount = 0, vectorMasks = 0, smartObjects = 0;
        for (const Record& rec : records) {
            std::map<int, std::vector<uint8_t>> planes, maskPlanes;
            decodeRecordChannels(rec, planes, maskPlanes, cursor);
            const bool hidden = rec.flags & 2;
            std::optional<Uuid> parent = open.empty() ? std::nullopt : std::optional<Uuid>();
            bool lossyBlend = false;
            BlendMode blend = blendFor(rec.blend, &lossyBlend);
            if (rec.section == 3) { open.push_back({layers.size(), {}}); continue; }   // a folder's end marker: its contents follow
            if (rec.section == 1 || rec.section == 2) {
                if (open.empty()) continue;
                OpenGroup group = open.back();
                open.pop_back();
                Layer folder(rec.name.empty() ? "Folder" : rec.name, document.size());
                folder.isGroup = true;
                folder.visible = !hidden;
                folder.opacity = rec.opacity / 255.0;
                folder.blendMode = blend;
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
            if (rec.width() > 0 && rec.height() > 0 && !planes.empty()) image = assemble(mode, rec.width(), rec.height(), planes, palette);
            std::optional<AdjustmentSettings> settings;
            bool skippedAdjustment = false;
            if (auto b = block("levl")) settings = levelsFrom(b->first, b->second);
            else if (auto b = block("curv")) settings = curvesFrom(b->first, b->second);
            else if (auto b = block("hue2")) settings = hueSaturationFrom(b->first, b->second);
            else if (auto b = block("expA")) settings = exposureFrom(b->first, b->second);
            else if (auto b = block("grdm")) settings = gradientMapFrom(b->first, b->second);
            else {
                static const std::map<std::string, const char*> others{{"brit", "Brightness/Contrast"}, {"blwh", "Black & White"}, {"vibA", "Vibrance"}, {"phfl", "Photo Filter"}, {"mixr", "Channel Mixer"},
                    {"clrL", "Color Lookup"}, {"nvrt", "Invert"}, {"post", "Posterize"}, {"thrs", "Threshold"}, {"selc", "Selective Color"}, {"blnc", "Color Balance"}};
                for (auto& [key, name] : others) if (block(key.c_str())) { notes.push_back("Layer \"" + rec.name + "\": " + name + " adjustment layers have no counterpart; skipped."); skippedAdjustment = true; break; }
            }
            if (settings) adjustment = settings->toLayerAdjustment();
            if (!image && !adjustment) {
                if (auto b = block("SoCo")) {
                    if (auto colour = solidColourFrom(b->first, b->second)) {
                        image = std::make_shared<Image>(int(width), int(height));
                        image->fill(uint8_t(std::lround(colour->red * 255)), uint8_t(std::lround(colour->green * 255)), uint8_t(std::lround(colour->blue * 255)), 255);
                    }
                } else if (block("GdFl") || block("PtFl")) notes.push_back("Layer \"" + rec.name + "\": gradient and pattern fill layers are not carried; skipped.");
            }
            if (block("TySh")) { if (auto text = textFrom(block("TySh")->first, block("TySh")->second)) extraJson = "{\"psdText\":\"" + jsonEscape(*text) + "\"}"; notes.push_back("Layer \"" + rec.name + "\": text was imported as pixels (its text is kept as psdText in the project)."); }
            if (block("lfx2") || block("lrFX")) effectsCount++;
            if (block("vmsk") || block("vsms")) vectorMasks++;
            if (block("SoLd") || block("PlLd")) smartObjects++;
            // An adjustment or fill kind with no counterpart is dropped; an empty layer (a divider, a layer never
            // painted on) stays as a blank one, so the structure survives.
            if (!image && !adjustment && (block("GdFl") || block("PtFl") || skippedAdjustment)) continue;

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
            if (lossyBlend) notes.push_back("Layer \"" + layer.name + "\": blend mode " + blendDescription(rec.blend) + " has no counterpart; " + blendModeName(blend) + " was used.");
            // The mask: Photoshop keeps it in its own rectangle with a default beyond; ours covers the layer's pixels.
            auto userMask = maskPlanes.find(-2);
            if (userMask == maskPlanes.end()) userMask = maskPlanes.find(-3);
            if (rec.mask.present && userMask != maskPlanes.end()) {
                const int lw = image ? image->width() : int(width), lh = image ? image->height() : int(height);
                const int lx = image ? int(layer.transform.origin.x) : 0, ly = image ? int(layer.transform.origin.y) : 0;
                auto mask = std::make_shared<GrayImage>(lw, lh, rec.mask.defaultColour);
                const int mw = rec.mask.right - rec.mask.left, mh = rec.mask.bottom - rec.mask.top;
                for (int y = 0; y < mh; y++) {
                    const int ty = rec.mask.top + y - ly;
                    if (ty < 0 || ty >= lh) continue;
                    for (int x = 0; x < mw; x++) { const int tx = rec.mask.left + x - lx; if (tx >= 0 && tx < lw) mask->at(tx, ty) = userMask->second[size_t(y) * mw + size_t(x)]; }
                }
                LayerMask lm;
                lm.asset = MaskAsset::make(mask);
                lm.enabled = !(rec.mask.flags & 2);
                layer.mask = lm;
            }
            if (rec.clipping) {
                // Clipped to the nearest unclipped layer below it in the same folder.
                const size_t from = open.empty() ? 0 : open.back().firstChild;
                for (size_t i = layers.size(); i-- > from;) if (!layers[i].maskSourceId && !layers[i].isGroup) { layer.maskSourceId = layers[i].id; break; }
            }
            layers.push_back(layer);
            if (!open.empty()) open.back().members.push_back(layer.id);
        }
        if (effectsCount) notes.push_back(std::to_string(effectsCount) + " layer style(s) (drop shadows, strokes, glows) were not carried.");
        if (vectorMasks) notes.push_back(std::to_string(vectorMasks) + " vector mask(s) were not carried.");
        if (smartObjects) notes.push_back(std::to_string(smartObjects) + " smart object(s) were imported as pixels.");
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
                std::vector<size_t> rows(size_t(planeCount) * height);
                for (size_t i = 0; i < rows.size(); i++) rows[i] = psb ? counts.u32() : counts.u16();
                size_t offset = counts.position();
                for (int c = 0; c < planeCount && ok; c++) {
                    size_t total = 0;
                    for (uint32_t y = 0; y < height; y++) total += rows[size_t(c) * height + y];
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
                result.composite = assemble(mode == Bitmap ? Grayscale : mode, int(width), int(height), planes, palette);
            } else notes.push_back("The merged image could not be read; only the layers were imported.");
        }
        if (layers.empty()) {
            if (!result.composite) { if (error) *error = "The file has neither layers nor a readable merged image."; return std::nullopt; }
            Layer background(Asset::make(result.composite, "Background"), Point(0, 0));
            background.name = "Background";
            layers.push_back(background);
            if (records.empty()) notes.push_back("The file carries no layers (it was saved flattened); the merged image is the only layer.");
        }
        result.document = std::move(document);
        return result;
    } catch (Truncated&) {
        if (error) *error = "The file ends early or has a structure this reader does not understand.";
        return std::nullopt;
    }
}

} // namespace compositor
