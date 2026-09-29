// The float primitives of a 32-bit document (depth.h, "32 bits"): linearising and encoding colour through the
// document's transfer curve, coverage in float, halvings, point samplers, crops, bounds, fingerprints, thumbnails and
// the cleaning of NaN and infinities.
#include "compositor/colormgmt.h"
#include "compositor/depth.h"
#include "compositor/parallel.h"
#include "compositor/view32.h"
#include <cmath>
#include <cstring>
#include <type_traits>
#include <utility>
#include <vector>

namespace compositor {

namespace {

/// The curve's value at every level of a depth (straight colour at full alpha), made once per conversion.
std::vector<float> levelTable(const TransferCurve& curve, uint32_t one) {
    std::vector<float> table(size_t(one) + 1);
    for (uint32_t i = 0; i <= one; i++) table[i] = curve.toLinear(float(double(i) / double(one)));
    return table;
}

template <class Narrow>
std::shared_ptr<ImageF> linearise(const Narrow& image, const TransferCurve& curve, uint32_t one) {
    using T = std::remove_pointer_t<decltype(std::declval<Narrow&>().data())>;
    auto out = std::make_shared<ImageF>(image.width(), image.height());
    const std::vector<float> table = levelTable(curve, one);
    const float unit = 1.0f / float(one);
    parallelRows(0, image.height(), [&](int ya, int yb) {
        for (int y = ya; y < yb; y++) {
            const T* s = image.row(y);
            float* d = out->row(y);
            for (int x = 0; x < image.width(); x++, s += 4, d += 4) {
                const uint32_t a = std::min<uint32_t>(s[3], one);
                d[3] = float(a) * unit;
                if (a == 0) { d[0] = d[1] = d[2] = 0; continue; }
                for (int c = 0; c < 3; c++) {
                    const uint32_t v = std::min<uint32_t>(s[c], a);
                    // Opaque: the level's own value; else the straight colour (a fraction of a level) through the curve.
                    d[c] = a == one ? table[v] : curve.toLinear(float(v) / float(a)) * d[3];
                }
            }
        }
    }, 32);
    return out;
}

template <class Narrow>
std::shared_ptr<Narrow> encode(const ImageF& image, const TransferCurve& curve, const ToneMap* tone, uint32_t one) {
    using T = std::remove_pointer_t<decltype(std::declval<Narrow&>().data())>;
    auto out = std::make_shared<Narrow>(image.width(), image.height());
    const bool toning = tone && !tone->identity;
    parallelRows(0, image.height(), [&](int ya, int yb) {
        for (int y = ya; y < yb; y++) {
            const float* s = image.row(y);
            T* d = out->row(y);
            for (int x = 0; x < image.width(); x++, s += 4, d += 4) {
                const float a = cleanCoverage(s[3]);
                const uint32_t A = uint32_t(std::lround(a * float(one)));
                d[3] = T(A);
                if (A == 0) { d[0] = d[1] = d[2] = 0; continue; }
                float c[3];
                for (int k = 0; k < 3; k++) c[k] = cleanColour(s[k]) / a;
                if (toning) tone->apply(c);
                for (int k = 0; k < 3; k++) {
                    const float e = curve.fromLinearExact(std::clamp(c[k], 0.0f, 1.0f));
                    d[k] = T(std::min<uint32_t>(A, uint32_t(std::lround(double(e) * double(A)))));
                }
            }
        }
    }, 32);
    return out;
}

template <class Gray>
std::shared_ptr<GrayF> widenCoverage(const Gray& image, float one) {
    auto out = std::make_shared<GrayF>(image.width(), image.height());
    const size_t n = size_t(image.width()) * size_t(image.height());
    const float unit = 1.0f / one;
    for (size_t i = 0; i < n; i++) out->data()[i] = std::min(1.0f, float(image.data()[i]) * unit);
    return out;
}

template <class Gray>
std::shared_ptr<Gray> narrowCoverage(const GrayF& image, float one) {
    using T = std::remove_pointer_t<decltype(std::declval<Gray&>().data())>;
    auto out = std::make_shared<Gray>(image.width(), image.height());
    const size_t n = size_t(image.width()) * size_t(image.height());
    for (size_t i = 0; i < n; i++) out->data()[i] = T(std::lround(cleanCoverage(image.data()[i]) * one));
    return out;
}

template <int Channels, class Img>
std::shared_ptr<Img> halveF(const Img& image) {
    const int sw = image.width(), sh = image.height();
    const int w = std::max(1, (sw + 1) / 2), h = std::max(1, (sh + 1) / 2);
    auto out = std::make_shared<Img>(w, h);
    parallelRows(0, h, [&](int ya, int yb) {
        for (int y = ya; y < yb; y++) {
            const float* r0 = image.row(std::min(2 * y, sh - 1));
            const float* r1 = image.row(std::min(2 * y + 1, sh - 1));
            float* o = out->row(y);
            for (int x = 0; x < w; x++) {
                const int a = std::min(2 * x, sw - 1) * Channels, b = std::min(2 * x + 1, sw - 1) * Channels;
                for (int c = 0; c < Channels; c++) o[x * Channels + c] = (r0[a + c] + r0[b + c] + r1[a + c] + r1[b + c]) * 0.25f;
            }
        }
    }, 64);
    return out;
}

template <int Channels, class Img>
std::shared_ptr<Img> boxResizeF(const Img& image, int w, int h) {
    auto out = std::make_shared<Img>(w, h);
    const double sx = double(image.width()) / w, sy = double(image.height()) / h;
    const int stepX = std::max(1, int(sx / 8)), stepY = std::max(1, int(sy / 8));
    for (int y = 0; y < h; y++) {
        const int y0 = int(std::floor(y * sy)), y1 = std::min(std::max(y0 + 1, int(std::floor((y + 1) * sy))), image.height());
        for (int x = 0; x < w; x++) {
            const int x0 = int(std::floor(x * sx)), x1 = std::min(std::max(x0 + 1, int(std::floor((x + 1) * sx))), image.width());
            double sum[Channels] = {};
            double count = 0;
            for (int j = y0 + stepY / 2; j < y1; j += stepY)
                for (int i = x0 + stepX / 2; i < x1; i += stepX) {
                    const float* p = image.row(j) + size_t(i) * Channels;
                    for (int c = 0; c < Channels; c++) sum[c] += p[c];
                    count++;
                }
            float* o = out->row(y) + size_t(x) * Channels;
            for (int c = 0; c < Channels; c++) o[c] = count > 0 ? float(sum[c] / count) : 0.0f;
        }
    }
    return out;
}

template <typename Img>
uint64_t hashFloats(const Img* image, int channels) {
    if (!image || image->isEmpty()) return 0;
    // A different seed from the 16-bit fingerprint's, so equal bytes at another depth never match.
    uint64_t h = 0x13198a2e03707344ull ^ (uint64_t(image->width()) << 32 | uint32_t(image->height()));
    auto mix = [&](uint64_t v) { h ^= v; h *= 0xff51afd7ed558ccdull; h ^= h >> 29; };
    const size_t rowBytes = size_t(image->width()) * size_t(channels) * 4;
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

struct TapsF { int i0, i1; float f; };
inline TapsF linearTaps(double v, int n) {
    v -= 0.5;
    if (v < 0) v = 0; else if (v > n - 1) v = n - 1;
    const int i0 = int(v);
    return {i0, std::min(i0 + 1, n - 1), float(v - i0)};
}

struct Taps4F { int i[4]; float w[4]; };
inline Taps4F cubicTaps(double v, int n) {
    v -= 0.5;
    if (v < 0) v = 0; else if (v > n - 1) v = n - 1;
    const int i1 = int(v);
    const float t = float(v - i1), t2 = t * t, t3 = t2 * t;
    Taps4F taps;
    taps.i[0] = std::max(i1 - 1, 0); taps.i[1] = i1; taps.i[2] = std::min(i1 + 1, n - 1); taps.i[3] = std::min(i1 + 2, n - 1);
    // Catmull-Rom (a = -0.5), the 8-bit table's kernel, unquantised.
    taps.w[0] = -0.5f * t3 + t2 - 0.5f * t;
    taps.w[1] = 1.5f * t3 - 2.5f * t2 + 1;
    taps.w[2] = -1.5f * t3 + 2 * t2 + 0.5f * t;
    taps.w[3] = 0.5f * t3 - 0.5f * t2;
    return taps;
}

} // namespace

std::shared_ptr<ImageF> lineariseImage(const Image& image, const TransferCurve& curve) { return linearise(image, curve, 255); }
std::shared_ptr<ImageF> lineariseImage(const Image16& image, const TransferCurve& curve) { return linearise(image, curve, one16); }
std::shared_ptr<GrayF> widenGrayF(const GrayImage& image) { return widenCoverage(image, 255.0f); }
std::shared_ptr<GrayF> widenGrayF(const Gray16& image) { return widenCoverage(image, float(one16)); }
std::shared_ptr<Image> encodeImage8(const ImageF& image, const TransferCurve& curve, const ToneMap* tone) { return encode<Image>(image, curve, tone, 255); }
std::shared_ptr<Image16> encodeImage16(const ImageF& image, const TransferCurve& curve, const ToneMap* tone) { return encode<Image16>(image, curve, tone, one16); }
std::shared_ptr<GrayImage> narrowGrayF(const GrayF& image) { return narrowCoverage<GrayImage>(image, 255.0f); }
std::shared_ptr<Gray16> narrowGrayF16(const GrayF& image) { return narrowCoverage<Gray16>(image, float(one16)); }

size_t cleanFloat(ImageF& image) {
    size_t changed = 0;
    const size_t n = size_t(image.width()) * size_t(image.height()) * size_t(image.channels());
    float* p = image.data();
    const size_t alphaAt = size_t(image.channels()) - 1;
    for (size_t i = 0; i < n; i++) {
        const bool alpha = i % size_t(image.channels()) == alphaAt;
        const float v = alpha ? cleanCoverage(p[i]) : cleanColour(p[i]);
        if (!(v == p[i])) { p[i] = v; changed++; }
    }
    return changed;
}

size_t cleanFloat(GrayF& image) {
    size_t changed = 0;
    const size_t n = size_t(image.width()) * size_t(image.height());
    float* p = image.data();
    for (size_t i = 0; i < n; i++) {
        const float v = cleanCoverage(p[i]);
        if (!(v == p[i])) { p[i] = v; changed++; }
    }
    return changed;
}

void premultiply(ImageF& image) {
    for (int y = 0; y < image.height(); y++) {
        float* p = image.row(y);
        for (int x = 0; x < image.width(); x++, p += 4)
            if (p[3] < 1) for (int c = 0; c < 3; c++) p[c] *= std::max(0.0f, p[3]);
    }
}

void unpremultiply(ImageF& image) {
    for (int y = 0; y < image.height(); y++) {
        float* p = image.row(y);
        for (int x = 0; x < image.width(); x++, p += 4) {
            if (p[3] >= 1) continue;
            if (p[3] <= 0) { p[0] = p[1] = p[2] = 0; continue; }
            for (int c = 0; c < 3; c++) p[c] /= p[3];
        }
    }
}

std::shared_ptr<ImageF> halveImage(const ImageF& image) { return halveF<4>(image); }
std::shared_ptr<GrayF> halveGray(const GrayF& image) { return halveF<1>(image); }

std::shared_ptr<ImageF> reduceImage(const ImageF& image, int level) {
    if (level <= 0) return nullptr;
    std::shared_ptr<ImageF> out = halveImage(image);
    for (int i = 1; i < level && (out->width() > 1 || out->height() > 1); i++) out = halveImage(*out);
    return out;
}

std::shared_ptr<GrayF> reduceGray(const GrayF& image, int level) {
    if (level <= 0) return nullptr;
    std::shared_ptr<GrayF> out = halveGray(image);
    for (int i = 1; i < level && (out->width() > 1 || out->height() > 1); i++) out = halveGray(*out);
    return out;
}

void sampleBilinear(const ImageF& image, double x, double y, float out[4]) {
    const TapsF tx = linearTaps(x, image.width()), ty = linearTaps(y, image.height());
    const float *a = image.pixel(tx.i0, ty.i0), *b = image.pixel(tx.i1, ty.i0), *c = image.pixel(tx.i0, ty.i1), *d = image.pixel(tx.i1, ty.i1);
    for (int k = 0; k < 4; k++) {
        const float top = a[k] + (b[k] - a[k]) * tx.f, bottom = c[k] + (d[k] - c[k]) * tx.f;
        out[k] = top + (bottom - top) * ty.f;
    }
}

void sampleBicubic(const ImageF& image, double x, double y, float out[4]) {
    const Taps4F tx = cubicTaps(x, image.width()), ty = cubicTaps(y, image.height());
    float acc[4] = {0, 0, 0, 0};
    for (int j = 0; j < 4; j++) {
        const float* row = image.row(ty.i[j]);
        for (int k = 0; k < 4; k++) {
            float h = 0;
            for (int i = 0; i < 4; i++) h += row[size_t(tx.i[i]) * 4 + size_t(k)] * tx.w[i];
            acc[k] += h * ty.w[j];
        }
    }
    for (int k = 0; k < 3; k++) out[k] = std::max(0.0f, acc[k]);
    out[3] = std::clamp(acc[3], 0.0f, 1.0f);
}

float sampleGrayBilinear(const GrayF& image, double x, double y) {
    const TapsF tx = linearTaps(x, image.width()), ty = linearTaps(y, image.height());
    const float top = image.at(tx.i0, ty.i0) + (image.at(tx.i1, ty.i0) - image.at(tx.i0, ty.i0)) * tx.f;
    const float bottom = image.at(tx.i0, ty.i1) + (image.at(tx.i1, ty.i1) - image.at(tx.i0, ty.i1)) * tx.f;
    return top + (bottom - top) * ty.f;
}

std::shared_ptr<ImageF> cropImage(const ImageF& image, int x, int y, int width, int height) {
    auto out = std::make_shared<ImageF>(std::max(0, width), std::max(0, height));
    const int i0 = std::max(0, -x), i1 = std::min(out->width(), image.width() - x);
    if (i1 <= i0) return out;
    for (int j = 0; j < out->height(); j++) {
        const int sy = y + j;
        if (sy < 0 || sy >= image.height()) continue;
        std::memcpy(out->pixel(i0, j), image.pixel(x + i0, sy), size_t(i1 - i0) * 4 * sizeof(float));
    }
    return out;
}

std::shared_ptr<GrayF> cropGray(const GrayF& image, int x, int y, int width, int height) {
    auto out = std::make_shared<GrayF>(std::max(0, width), std::max(0, height));
    const int i0 = std::max(0, -x), i1 = std::min(out->width(), image.width() - x);
    if (i1 <= i0) return out;
    for (int j = 0; j < out->height(); j++) {
        const int sy = y + j;
        if (sy < 0 || sy >= image.height()) continue;
        std::memcpy(out->row(j) + i0, image.row(sy) + x + i0, size_t(i1 - i0) * sizeof(float));
    }
    return out;
}

PixelBounds nonzeroBounds(const GrayF& image) {
    PixelBounds b;
    int x0 = image.width(), y0 = image.height(), x1 = 0, y1 = 0;
    for (int y = 0; y < image.height(); y++) {
        const float* p = image.row(y);
        int first = 0, end = image.width();
        while (first < image.width() && !(p[first] > 0)) first++;
        if (first == image.width()) continue;
        while (end > first && !(p[end - 1] > 0)) end--;
        x0 = std::min(x0, first); x1 = std::max(x1, end);
        y0 = std::min(y0, y); y1 = y + 1;
    }
    if (x1 > x0 && y1 > y0) { b.x0 = x0; b.y0 = y0; b.x1 = x1; b.y1 = y1; }
    return b;
}

PixelBounds alphaBounds(const ImageF& image) {
    PixelBounds b;
    int x0 = image.width(), y0 = image.height(), x1 = 0, y1 = 0;
    for (int y = 0; y < image.height(); y++) {
        const float* p = image.row(y);
        int first = 0, end = image.width();
        while (first < image.width() && !(p[first * 4 + 3] > 0)) first++;
        if (first == image.width()) continue;
        while (end > first && !(p[(end - 1) * 4 + 3] > 0)) end--;
        x0 = std::min(x0, first); x1 = std::max(x1, end);
        y0 = std::min(y0, y); y1 = y + 1;
    }
    if (x1 > x0 && y1 > y0) { b.x0 = x0; b.y0 = y0; b.x1 = x1; b.y1 = y1; }
    return b;
}

uint64_t contentHash(const ImageF* image) { return hashFloats(image, 4); }
uint64_t contentHash(const GrayF* image) { return hashFloats(image, 1); }

std::shared_ptr<Image> makeThumbnail(const ImageF& image, int maxSide) {
    if (image.isEmpty()) return std::make_shared<Image>(1, 1);
    const double factor = std::min(1.0, double(maxSide) / std::max(image.width(), image.height()));
    const int w = std::max(1, int(image.width() * factor)), h = std::max(1, int(image.height() * factor));
    const TransferCurve srgb = TransferCurve::srgb();
    if (w == image.width() && h == image.height()) return encodeImage8(image, srgb);
    return encodeImage8(*boxResizeF<4>(image, w, h), srgb);
}

std::shared_ptr<GrayImage> makeGrayThumbnail(const GrayF& image, int maxSide) {
    if (image.isEmpty()) return std::make_shared<GrayImage>(1, 1);
    const double factor = std::min(1.0, double(maxSide) / std::max(image.width(), image.height()));
    const int w = std::max(1, int(image.width() * factor)), h = std::max(1, int(image.height() * factor));
    if (w == image.width() && h == image.height()) return narrowGrayF(image);
    return narrowGrayF(*boxResizeF<1>(image, w, h));
}

AnyImage imageAtDepth(const AnyImage& image, SampleType type, const TransferCurve* curve) {
    if (!image || image.sampleType() == type) return image;
    if (type != SampleType::F32 && image.sampleType() != SampleType::F32) return imageAtDepth(image, type);
    if (image.channels() != 4) return nullptr;
    const TransferCurve srgb = TransferCurve::srgb();
    const TransferCurve& c = curve ? *curve : srgb;
    if (type == SampleType::F32) {
        if (image.u8()) return ImageFPtr(lineariseImage(*image.u8(), c));
        if (image.u16()) return ImageFPtr(lineariseImage(*image.u16(), c));
        return nullptr;
    }
    if (type == SampleType::U8) return ImagePtr(encodeImage8(*image.f32(), c));
    return Image16Ptr(encodeImage16(*image.f32(), c));
}

void predictFloatRow(const float* samples, int width, uint8_t* row) {
    const size_t w = size_t(width);
    for (size_t x = 0; x < w; x++) {
        uint32_t bits;
        std::memcpy(&bits, samples + x, 4);
        for (size_t b = 0; b < 4; b++) row[b * w + x] = uint8_t(bits >> (24 - 8 * b));
    }
    for (size_t i = w * 4; i-- > 1;) row[i] = uint8_t(row[i] - row[i - 1]);
}

void unpredictFloatRow(uint8_t* row, int width, float* samples) {
    const size_t w = size_t(width);
    for (size_t i = 1; i < w * 4; i++) row[i] = uint8_t(row[i] + row[i - 1]);
    for (size_t x = 0; x < w; x++) {
        const uint32_t bits = uint32_t(row[x]) << 24 | uint32_t(row[w + x]) << 16 | uint32_t(row[2 * w + x]) << 8 | row[3 * w + x];
        std::memcpy(samples + x, &bits, 4);
    }
}

} // namespace compositor
