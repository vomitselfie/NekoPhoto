#include "compositor/depth.h"
#include "compositor/parallel.h"
#include <cmath>
#include <cstring>

namespace compositor {

std::shared_ptr<Image16> widenImage(const Image& image) {
    auto out = std::make_shared<Image16>(image.width(), image.height());
    parallelRows(0, image.height(), [&](int ya, int yb) {
        for (int y = ya; y < yb; y++) {
            const uint8_t* s = image.row(y);
            uint16_t* d = out->row(y);
            for (int i = 0; i < image.width() * 4; i++) d[i] = widen8(s[i]);
        }
    }, 64);
    return out;
}

std::shared_ptr<Gray16> widenGray(const GrayImage& image) {
    auto out = std::make_shared<Gray16>(image.width(), image.height());
    const size_t n = size_t(image.width()) * size_t(image.height());
    for (size_t i = 0; i < n; i++) out->data()[i] = widen8(image.data()[i]);
    return out;
}

void narrowInto(const Image16& image, Image& out) {
    if (out.width() != image.width() || out.height() != image.height()) out = Image(image.width(), image.height());
    parallelRows(0, image.height(), [&](int ya, int yb) {
        for (int y = ya; y < yb; y++) {
            const uint16_t* s = image.row(y);
            uint8_t* d = out.row(y);
            for (int i = 0; i < image.width() * 4; i++) d[i] = narrow16(s[i]);
        }
    }, 64);
}

std::shared_ptr<Image> narrowImage(const Image16& image) {
    auto out = std::make_shared<Image>(image.width(), image.height());
    narrowInto(image, *out);
    return out;
}

std::shared_ptr<GrayImage> narrowGray(const Gray16& image) {
    auto out = std::make_shared<GrayImage>(image.width(), image.height());
    const size_t n = size_t(image.width()) * size_t(image.height());
    for (size_t i = 0; i < n; i++) out->data()[i] = narrow16(image.data()[i]);
    return out;
}

std::shared_ptr<Image> ditherToEightBit(const Image16& image) {
    // Thresholds in 1/16ths of an 8-bit level, centred on zero.
    static constexpr int bayer[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};
    auto out = std::make_shared<Image>(image.width(), image.height());
    parallelRows(0, image.height(), [&](int ya, int yb) {
        for (int y = ya; y < yb; y++) {
            const uint16_t* s = image.row(y);
            uint8_t* d = out->row(y);
            for (int x = 0; x < image.width(); x++, s += 4, d += 4) {
                // value * 255 / 32768 in 1/16ths, plus the threshold, floored: rounds up with the chance the
                // fraction gives it. Colour stays within the alpha it was dithered with.
                const uint32_t t = uint32_t(bayer[y & 3][x & 3]) * 2 + 1;   // odd 32ths: never a tie
                auto q = [&](uint32_t v) { return uint8_t(std::min<uint32_t>(255, (std::min(v, one16) * 255 * 32 + t * one16) / (one16 * 32))); };
                const uint8_t a = q(s[3]);
                for (int c = 0; c < 3; c++) d[c] = std::min(q(s[c]), a);
                d[3] = a;
            }
        }
    }, 64);
    return out;
}

void premultiply(Image16& image) {
    for (int y = 0; y < image.height(); y++) {
        uint16_t* p = image.row(y);
        for (int x = 0; x < image.width(); x++, p += 4) {
            const uint32_t a = p[3];
            if (a >= one16) continue;
            for (int c = 0; c < 3; c++) p[c] = uint16_t(mul15(std::min<uint32_t>(p[c], one16), a));
        }
    }
}

void unpremultiply(Image16& image) {
    for (int y = 0; y < image.height(); y++) {
        uint16_t* p = image.row(y);
        for (int x = 0; x < image.width(); x++, p += 4) {
            const uint32_t a = p[3];
            if (a >= one16 || a == 0) continue;
            for (int c = 0; c < 3; c++) p[c] = uint16_t(std::min<uint32_t>(one16, (p[c] * one16 + a / 2) / a));
        }
    }
}

namespace {

template <int Channels, typename Img>
std::shared_ptr<Img> halve16(const Img& image) {
    const int sw = image.width(), sh = image.height();
    const int w = std::max(1, (sw + 1) / 2), h = std::max(1, (sh + 1) / 2);
    auto out = std::make_shared<Img>(w, h);
    parallelRows(0, h, [&](int ya, int yb) {
        for (int y = ya; y < yb; y++) {
            const uint16_t* r0 = image.row(std::min(2 * y, sh - 1));
            const uint16_t* r1 = image.row(std::min(2 * y + 1, sh - 1));
            uint16_t* o = out->row(y);
            for (int x = 0; x < w; x++) {
                const int a = std::min(2 * x, sw - 1) * Channels, b = std::min(2 * x + 1, sw - 1) * Channels;
                for (int c = 0; c < Channels; c++) o[x * Channels + c] = uint16_t((uint32_t(r0[a + c]) + r0[b + c] + r1[a + c] + r1[b + c] + 2) / 4);
            }
        }
    }, 64);
    return out;
}

template <int Channels, typename Img>
std::shared_ptr<Img> boxResize16(const Img& image, int w, int h) {
    auto out = std::make_shared<Img>(w, h);
    const double sx = double(image.width()) / w, sy = double(image.height()) / h;
    const int stepX = std::max(1, int(sx / 8)), stepY = std::max(1, int(sy / 8));
    for (int y = 0; y < h; y++) {
        const int y0 = int(std::floor(y * sy)), y1 = std::min(std::max(y0 + 1, int(std::floor((y + 1) * sy))), image.height());
        for (int x = 0; x < w; x++) {
            const int x0 = int(std::floor(x * sx)), x1 = std::min(std::max(x0 + 1, int(std::floor((x + 1) * sx))), image.width());
            uint64_t sum[Channels] = {};
            uint64_t count = 0;
            for (int j = y0 + stepY / 2; j < y1; j += stepY)
                for (int i = x0 + stepX / 2; i < x1; i += stepX) {
                    const uint16_t* p = image.row(j) + size_t(i) * Channels;
                    for (int c = 0; c < Channels; c++) sum[c] += p[c];
                    count++;
                }
            uint16_t* o = out->row(y) + size_t(x) * Channels;
            for (int c = 0; c < Channels; c++) o[c] = count ? uint16_t((sum[c] + count / 2) / count) : 0;
        }
    }
    return out;
}

template <typename Img>
uint64_t hashSamples(const Img* image, int channels) {
    if (!image || image->isEmpty()) return 0;
    uint64_t h = 0x243f6a8885a308d3ull ^ (uint64_t(image->width()) << 32 | uint32_t(image->height()));
    auto mix = [&](uint64_t v) { h ^= v; h *= 0xff51afd7ed558ccdull; h ^= h >> 29; };
    const size_t rowBytes = size_t(image->width()) * size_t(channels) * 2;
    for (int y = 0; y < image->height(); y++) {
        const uint8_t* p = reinterpret_cast<const uint8_t*>(image->row(y));
        size_t i = 0;
        for (; i + 8 <= rowBytes; i += 8) { uint64_t v; std::memcpy(&v, p + i, 8); mix(v); }
        uint64_t tail = 0;
        std::memcpy(&tail, p + i, rowBytes - i);
        mix(tail ^ (uint64_t(y) << 48));
    }
    return h ? h : 1;
}

} // namespace

std::shared_ptr<Image16> halveImage(const Image16& image) { return halve16<4>(image); }
std::shared_ptr<Gray16> halveGray(const Gray16& image) { return halve16<1>(image); }

std::shared_ptr<Image16> reduceImage(const Image16& image, int level) {
    if (level <= 0) return nullptr;
    std::shared_ptr<Image16> out = halveImage(image);
    for (int i = 1; i < level && (out->width() > 1 || out->height() > 1); i++) out = halveImage(*out);
    return out;
}

std::shared_ptr<Gray16> reduceGray(const Gray16& image, int level) {
    if (level <= 0) return nullptr;
    std::shared_ptr<Gray16> out = halveGray(image);
    for (int i = 1; i < level && (out->width() > 1 || out->height() > 1); i++) out = halveGray(*out);
    return out;
}

std::shared_ptr<Image> makeThumbnail(const Image16& image, int maxSide) {
    if (image.isEmpty()) return std::make_shared<Image>(1, 1);
    const double factor = std::min(1.0, double(maxSide) / std::max(image.width(), image.height()));
    const int w = std::max(1, int(image.width() * factor)), h = std::max(1, int(image.height() * factor));
    if (w == image.width() && h == image.height()) return narrowImage(image);
    return narrowImage(*boxResize16<4>(image, w, h));
}

std::shared_ptr<GrayImage> makeGrayThumbnail(const Gray16& image, int maxSide) {
    if (image.isEmpty()) return std::make_shared<GrayImage>(1, 1);
    const double factor = std::min(1.0, double(maxSide) / std::max(image.width(), image.height()));
    const int w = std::max(1, int(image.width() * factor)), h = std::max(1, int(image.height() * factor));
    if (w == image.width() && h == image.height()) return narrowGray(image);
    return narrowGray(*boxResize16<1>(image, w, h));
}

std::shared_ptr<Image16> cropImage(const Image16& image, int x, int y, int width, int height) {
    auto out = std::make_shared<Image16>(std::max(0, width), std::max(0, height));
    const int i0 = std::max(0, -x), i1 = std::min(out->width(), image.width() - x);
    if (i1 <= i0) return out;
    for (int j = 0; j < out->height(); j++) {
        const int sy = y + j;
        if (sy < 0 || sy >= image.height()) continue;
        std::memcpy(out->pixel(i0, j), image.pixel(x + i0, sy), size_t(i1 - i0) * 4 * sizeof(uint16_t));
    }
    return out;
}

std::shared_ptr<Gray16> cropGray(const Gray16& image, int x, int y, int width, int height) {
    auto out = std::make_shared<Gray16>(std::max(0, width), std::max(0, height));
    const int i0 = std::max(0, -x), i1 = std::min(out->width(), image.width() - x);
    if (i1 <= i0) return out;
    for (int j = 0; j < out->height(); j++) {
        const int sy = y + j;
        if (sy < 0 || sy >= image.height()) continue;
        std::memcpy(out->row(j) + i0, image.row(sy) + x + i0, size_t(i1 - i0) * sizeof(uint16_t));
    }
    return out;
}

PixelBounds nonzeroBounds(const Gray16& image) {
    PixelBounds b;
    int x0 = image.width(), y0 = image.height(), x1 = 0, y1 = 0;
    for (int y = 0; y < image.height(); y++) {
        const uint16_t* p = image.row(y);
        int first = 0, end = image.width();
        while (first < image.width() && !p[first]) first++;
        if (first == image.width()) continue;
        while (end > first && !p[end - 1]) end--;
        x0 = std::min(x0, first); x1 = std::max(x1, end);
        y0 = std::min(y0, y); y1 = y + 1;
    }
    if (x1 > x0 && y1 > y0) { b.x0 = x0; b.y0 = y0; b.x1 = x1; b.y1 = y1; }
    return b;
}

uint64_t contentHash(const Image16* image) { return hashSamples(image, 4); }
uint64_t contentHash(const Gray16* image) { return hashSamples(image, 1); }

AnyImage imageAtDepth(const AnyImage& image, SampleType type) {
    if (!image || image.sampleType() == type) return image;
    // 32 bits: through sRGB's curve (the callers that know the document's pass it, depth_f32.cpp).
    if (type == SampleType::F32 || image.sampleType() == SampleType::F32) return imageAtDepth(image, type, nullptr);
    if (type == SampleType::U16 && image.u8()) return Image16Ptr(widenImage(*image.u8()));
    if (type == SampleType::U8 && image.u16()) return ImagePtr(narrowImage(*image.u16()));
    return nullptr;
}

AnyGray grayAtDepth(const AnyGray& image, SampleType type) {
    if (!image || image.sampleType() == type) return image;
    // Coverage in float and back: a change of scale.
    if (type == SampleType::F32) return image.u8() ? AnyGray(GrayFPtr(widenGrayF(*image.u8()))) : image.u16() ? AnyGray(GrayFPtr(widenGrayF(*image.u16()))) : nullptr;
    if (image.f32()) return type == SampleType::U8 ? AnyGray(GrayPtr(narrowGrayF(*image.f32()))) : AnyGray(Gray16Ptr(narrowGrayF16(*image.f32())));
    if (type == SampleType::U16 && image.u8()) return Gray16Ptr(widenGray(*image.u8()));
    if (type == SampleType::U8 && image.u16()) return GrayPtr(narrowGray(*image.u16()));
    return nullptr;
}

std::shared_ptr<Image16> widenImageC8(const ImageC8& image) {
    auto out = std::make_shared<Image16>(image.width(), image.height(), 5);
    parallelRows(0, image.height(), [&](int ya, int yb) {
        for (int y = ya; y < yb; y++) {
            const uint8_t* s = image.row(y);
            uint16_t* d = out->row(y);
            for (int i = 0; i < image.width() * 5; i++) d[i] = widen8(s[i]);
        }
    }, 64);
    return out;
}

std::shared_ptr<ImageC8> narrowImageC8(const Image16& image) {
    auto out = std::make_shared<ImageC8>(image.width(), image.height(), 5);
    if (image.channels() != 5) return out;
    parallelRows(0, image.height(), [&](int ya, int yb) {
        for (int y = ya; y < yb; y++) {
            const uint16_t* s = image.row(y);
            uint8_t* d = out->row(y);
            for (int i = 0; i < image.width() * 5; i++) d[i] = narrow16(s[i]);
        }
    }, 64);
    return out;
}

namespace {
// Lab across depths: L and alpha scale as any sample; a and b keep their neutral point, so they are made straight,
// moved by the offset and scale, and premultiplied again.
std::shared_ptr<Image16> widenLab(const Image& image) {
    auto out = std::make_shared<Image16>(image.width(), image.height());
    parallelRows(0, image.height(), [&](int ya, int yb) {
        for (int y = ya; y < yb; y++) {
            const uint8_t* s = image.row(y);
            uint16_t* d = out->row(y);
            for (int x = 0; x < image.width(); x++, s += 4, d += 4) {
                const uint8_t a8 = s[3];
                const uint16_t a16 = widen8(a8);
                d[0] = widen8(s[0]);
                d[3] = a16;
                for (int c = 1; c < 3; c++) {
                    const double v = labA<SampleType::U8>(s[c], a8);
                    d[c] = a8 == 0 ? 0 : storedLabAB<SampleType::U16>(v, a16);
                }
            }
        }
    }, 64);
    return out;
}

std::shared_ptr<Image> narrowLab(const Image16& image) {
    auto out = std::make_shared<Image>(image.width(), image.height());
    parallelRows(0, image.height(), [&](int ya, int yb) {
        for (int y = ya; y < yb; y++) {
            const uint16_t* s = image.row(y);
            uint8_t* d = out->row(y);
            for (int x = 0; x < image.width(); x++, s += 4, d += 4) {
                const uint16_t a16 = s[3];
                const uint8_t a8 = narrow16(a16);
                d[0] = narrow16(s[0]);
                d[3] = a8;
                for (int c = 1; c < 3; c++) {
                    // Rounded to the nearest 8-bit step of a or b.
                    const double v = std::round(labA<SampleType::U16>(s[c], a16));
                    d[c] = a8 == 0 ? 0 : storedLabAB<SampleType::U8>(v, a8);
                }
            }
        }
    }, 64);
    return out;
}
} // namespace

AnyImage imageAtFormat(const AnyImage& image, SampleType type, ColorMode mode) {
    if (!image || image.channels() != colorModeChannels(mode) || type == SampleType::F32) return nullptr;
    if (mode == ColorMode::RGB) return imageAtDepth(image, type);
    if (mode == ColorMode::CMYK) {
        if (type == SampleType::U8) return image.c8() ? image : image.u16() ? AnyImage(ImageC8Ptr(narrowImageC8(*image.u16()))) : nullptr;
        return image.u16() ? image : image.c8() ? AnyImage(Image16Ptr(widenImageC8(*image.c8()))) : nullptr;
    }
    if (type == SampleType::U8) return image.u8() ? image : image.u16() ? AnyImage(ImagePtr(narrowLab(*image.u16()))) : nullptr;
    return image.u16() ? image : image.u8() ? AnyImage(Image16Ptr(widenLab(*image.u8()))) : nullptr;
}

} // namespace compositor
