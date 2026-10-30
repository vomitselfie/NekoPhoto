// The PSD reader's parsing (psd_read.h): one channel's compressed plane decoded to samples, a layer's or the merged
// image's planes assembled into pixels, and the layer records with their tagged blocks.
#include "psd_read.h"
#include "nekophoto/adjustments.h"
#include "nekophoto/colour.h"
#include "nekophoto/colormgmt.h"
#include "nekophoto/depth.h"
#include "nekophoto/parallel.h"
#include "psd/psd_descriptor.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <span>
#include <string>
#include <vector>
#include <zlib.h>

namespace nekophoto::psd_read {

using namespace photoshop;

// ---- Channel data ------------------------------------------------------------------------------------

namespace {

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

} // namespace

/// Decodes one plane of `width` x `height` samples to 8 bits from `compression` (0 raw, 1 RLE, 2 zip,
/// 3 zip with prediction) and `depth` bits. False when the data does not add up.
bool decodePlane(const uint8_t* data, size_t size, int compression, int width, int height, int depth, bool psb, std::vector<uint8_t>& out, std::string* why,
                 std::vector<uint16_t>* wide, std::vector<uint8_t>* raw16, std::vector<float>* floats) {
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
    // A 16-bit plane also at Photoshop's internal 0..32768, for a 16-bit document.
    if (raw16 && depth == 16) *raw16 = raw;   // the file's own samples (an alpha channel's carry)
    if (wide && depth == 16) {
        wide->resize(out.size());
        for (size_t i = 0; i < out.size(); i++) (*wide)[i] = from65535(unsigned(raw[i * 2]) * 256 + raw[i * 2 + 1]);
    }
    // A 32-bit plane as its floats, for a 32-bit document (cleaned where the pixels are assembled).
    if (floats && depth == 32) {
        floats->resize(out.size());
        for (size_t i = 0; i < out.size(); i++) {
            const uint32_t bits = (uint32_t(raw[i * 4]) << 24) | (uint32_t(raw[i * 4 + 1]) << 16) | (uint32_t(raw[i * 4 + 2]) << 8) | raw[i * 4 + 3];
            std::memcpy(&(*floats)[i], &bits, 4);
        }
    }
    if (depth == 8) out = std::move(raw);
    else if (depth == 16) for (size_t i = 0; i < out.size(); i++) out[i] = uint8_t((unsigned(raw[i * 2]) * 256 + raw[i * 2 + 1] + 128) / 257);
    else for (size_t i = 0; i < out.size(); i++) { uint32_t bits = (uint32_t(raw[i * 4]) << 24) | (uint32_t(raw[i * 4 + 1]) << 16) | (uint32_t(raw[i * 4 + 2]) << 8) | raw[i * 4 + 3]; float f; std::memcpy(&f, &bits, 4); out[i] = encodeLinear(f); }
    return true;
}

// ---- Colour ---------------------------------------------------------------------------------------

namespace {

void labToRgb(double L, double a, double b, uint8_t out[3]) {
    double fy = (L + 16) / 116, fx = fy + a / 500, fz = fy - b / 200;
    auto inv = [](double t) { return t > 6.0 / 29 ? t * t * t : 3.0 * (6.0 / 29) * (6.0 / 29) * (t - 4.0 / 29); };
    double X = 0.95047 * inv(fx), Y = 1.0 * inv(fy), Z = 1.08883 * inv(fz);
    double r = 3.2406 * X - 1.5372 * Y - 0.4986 * Z, g = -0.9689 * X + 1.8758 * Y + 0.0415 * Z, bl = 0.0557 * X - 0.2040 * Y + 1.0570 * Z;
    out[0] = encodeLinear(float(r)); out[1] = encodeLinear(float(g)); out[2] = encodeLinear(float(bl));
}

} // namespace

/// Premultiplied RGBA from the decoded planes of a layer or the composite.
std::shared_ptr<Image> assemble(int mode, int width, int height, const std::map<int, std::vector<uint8_t>>& planes, const std::vector<uint8_t>& palette,
                                const CmykToSrgb* cmykProfile) {
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

/// An RGB or grayscale 16-bit document's planes (0..32768, straight colour) as a premultiplied 16-bit image.
std::shared_ptr<Image16> assemble16(int mode, int width, int height, const std::map<int, std::vector<uint16_t>>& planes) {
    auto image = std::make_shared<Image16>(width, height);
    auto plane = [&](int id) -> const uint16_t* { auto it = planes.find(id); return it == planes.end() ? nullptr : it->second.data(); };
    const uint16_t *c0 = plane(0), *c1 = mode == RGB ? plane(1) : c0, *c2 = mode == RGB ? plane(2) : c0, *alpha = plane(-1);
    for (int y = 0; y < height; y++) {
        uint16_t* p = image->row(y);
        for (int x = 0; x < width; x++, p += 4) {
            const size_t i = size_t(y) * width + size_t(x);
            const uint32_t a = alpha ? alpha[i] : one16;
            p[0] = uint16_t(mul15(c0 ? c0[i] : 0, a)); p[1] = uint16_t(mul15(c1 ? c1[i] : 0, a)); p[2] = uint16_t(mul15(c2 ? c2[i] : 0, a));
            p[3] = uint16_t(a);
        }
    }
    return image;
}

/// A 32-bit file's planes (straight linear colour, alpha) as premultiplied linear float, NaN and infinities cleaned.
std::shared_ptr<ImageF> assembleF(int mode, int width, int height, const std::map<int, std::vector<float>>& planes) {
    auto image = std::make_shared<ImageF>(width, height);
    auto plane = [&](int id) -> const float* { auto it = planes.find(id); return it == planes.end() ? nullptr : it->second.data(); };
    const float *c0 = plane(0), *c1 = mode == RGB ? plane(1) : c0, *c2 = mode == RGB ? plane(2) : c0, *alpha = plane(-1);
    for (int y = 0; y < height; y++) {
        float* p = image->row(y);
        for (int x = 0; x < width; x++, p += 4) {
            const size_t i = size_t(y) * width + size_t(x);
            const float a = alpha ? cleanCoverage(alpha[i]) : 1.0f;
            p[0] = c0 ? cleanColour(c0[i]) * a : 0; p[1] = c1 ? cleanColour(c1[i]) * a : 0; p[2] = c2 ? cleanColour(c2[i]) * a : 0;
            p[3] = a;
        }
    }
    return image;
}

namespace {

/// A CMYK or Lab file's planes (straight, as stored: CMYK as inverted ink, Lab's a and b offset) premultiplied at the
/// document's layout: an ImageC8 or 5-channel Image16 for CMYK, an Image or Image16 for Lab (P7 step D). A missing
/// plane is no ink (CMYK), black (L) or neutral (a, b).
template <class Img, class T>
AnyImage assembleMode(int mode, int width, int height, const std::map<int, std::vector<T>>& planes) {
    constexpr bool deep = sizeof(T) == 2;
    const uint32_t one = deep ? one16 : 255u, neutral = deep ? 16384u : 128u;
    const int colours = mode == CMYK ? 4 : 3, n = colours + 1;
    std::shared_ptr<Img> image;
    if constexpr (requires { Img(width, height, n); }) image = std::make_shared<Img>(width, height, n);
    else image = std::make_shared<Img>(width, height);
    auto plane = [&](int id) -> const T* { auto it = planes.find(id); return it == planes.end() || it->second.size() < size_t(width) * height ? nullptr : it->second.data(); };
    const T* c[4] = {plane(0), plane(1), plane(2), plane(3)};
    const T* alpha = plane(-1);
    uint32_t missing[4];
    for (int k = 0; k < 4; k++) missing[k] = mode == CMYK ? one : k == 0 ? 0 : neutral;
    parallelRows(0, height, [&](int ya, int yb) {
        for (int y = ya; y < yb; y++) {
            auto* p = image->row(y);
            for (int x = 0; x < width; x++, p += n) {
                const size_t i = size_t(y) * width + size_t(x);
                const uint32_t a = alpha ? std::min<uint32_t>(alpha[i], one) : one;
                for (int k = 0; k < colours; k++) {
                    const uint32_t v = std::min<uint32_t>(c[k] ? c[k][i] : missing[k], one);
                    p[k] = T(deep ? mul15(v, a) : (v * a + 127) / 255);
                }
                p[colours] = T(a);
            }
        }
    }, 64);
    return std::shared_ptr<const Img>(image);
}

} // namespace

AnyImage assembleMode8(int mode, int width, int height, const std::map<int, std::vector<uint8_t>>& planes) {
    return mode == CMYK ? assembleMode<ImageC8>(mode, width, height, planes) : assembleMode<Image>(mode, width, height, planes);
}
AnyImage assembleMode16(int mode, int width, int height, const std::map<int, std::vector<uint16_t>>& planes) {
    return assembleMode<Image16>(mode, width, height, planes);
}

// ---- Layer records --------------------------------------------------------------------------------

bool psbLongKey(const std::string& key) {
    static const std::set<std::string> keys{"LMsk", "Lr16", "Lr32", "Layr", "Mt16", "Mt32", "Mtrn", "Alph", "FMsk", "lnk2", "FEid", "FXid", "PxSD", "cinf"};
    return keys.count(key) > 0;
}

/// Tagged blocks (`8BIM`/`8B64`, key, length, data) up to `end`; `pad` rounds the data length.
void readTaggedBlocks(Reader& r, size_t end, bool psb, size_t pad, std::map<std::string, std::pair<const uint8_t*, size_t>>& blocks,
                      std::vector<std::pair<std::string, std::pair<const uint8_t*, size_t>>>* ordered) {
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
    for (int i = 0; i < channels; i++) { RecordChannel c; c.id = r.i16(); c.length = r.length(psb); rec.channels.push_back(c); }
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

} // namespace nekophoto::psd_read
