#include "compositor/image.h"
#include "compositor/parallel.h"
#include "compositor/simd.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <type_traits>

namespace compositor {

namespace {
/// Whether a size can be held as pixels: both sides within the per-side limit. A size that is not (a
/// negative, or one read from a malformed file) empties the image, so callers see `isEmpty()` rather than a
/// buffer whose stride claims more than it holds.
constexpr bool holdable(int width, int height) {
    return width >= 0 && height >= 0 && width <= maxImageSide && height <= maxImageSide;
}
}   // namespace

namespace {
/// A bulk copy, on every core once it is large enough to be worth it (a 4096 x 4096 layer is 64 MB).
void copyBytes(uint8_t* to, const uint8_t* from, size_t n) {
    constexpr size_t band = size_t(1) << 20;
    if (n < 4 * band) { if (n) std::memcpy(to, from, n); return; }
    const int bands = int((n + band - 1) / band);
    parallelFor(0, bands, 1, [&](int b0, int b1) {
        const size_t start = size_t(b0) * band, end = std::min(n, size_t(b1) * band);
        std::memcpy(to + start, from + start, end - start);
    });
}
}   // namespace

Image::Image(int width, int height)
    : width_(holdable(width, height) ? width : 0), height_(holdable(width, height) ? height : 0),
      stride_(width_ * 4), pixels_(size_t(stride_) * size_t(height_)) {}

Image::Image(const Image& other) : width_(other.width_), height_(other.height_), stride_(other.stride_), pixels_(other.pixels_.size()) {
    copyBytes(pixels_.data(), other.pixels_.data(), pixels_.size());
}

Image& Image::operator=(const Image& other) {
    if (this != &other) { Image copy(other); *this = std::move(copy); }
    return *this;
}

void Image::fill(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    for (int y = 0; y < height_; y++) {
        uint8_t* p = row(y);
        for (int x = 0; x < width_; x++, p += 4) { p[0] = r; p[1] = g; p[2] = b; p[3] = a; }
    }
}

GrayImage::GrayImage(int width, int height, uint8_t value)
    : width_(holdable(width, height) ? width : 0), height_(holdable(width, height) ? height : 0),
      pixels_(size_t(width_) * size_t(height_)) {
    if (value) std::memset(pixels_.data(), value, pixels_.size());
}

GrayImage::GrayImage(const GrayImage& other) : width_(other.width_), height_(other.height_), pixels_(other.pixels_.size()) {
    copyBytes(pixels_.data(), other.pixels_.data(), pixels_.size());
}

GrayImage& GrayImage::operator=(const GrayImage& other) {
    if (this != &other) { GrayImage copy(other); *this = std::move(copy); }
    return *this;
}

void GrayImage::fill(uint8_t value) { std::fill(pixels_.begin(), pixels_.end(), value); }

namespace {
/// First pixel in [0, n) of a row with nonzero alpha, or n: two pixels per 64-bit word.
int firstOpaque(const uint8_t* row, int n) {
    constexpr uint64_t alphaMask = 0xFF000000FF000000ULL;
    int x = 0;
    for (; x + 2 <= n; x += 2) {
        uint64_t word;
        std::memcpy(&word, row + size_t(x) * 4, 8);
        if (word & alphaMask) break;
    }
    for (; x < n; x++) if (row[size_t(x) * 4 + 3]) return x;
    return n;
}
int endOpaque(const uint8_t* row, int n) {
    constexpr uint64_t alphaMask = 0xFF000000FF000000ULL;
    int x = n;
    for (; x - 2 >= 0; x -= 2) {
        uint64_t word;
        std::memcpy(&word, row + size_t(x - 2) * 4, 8);
        if (word & alphaMask) break;
    }
    for (; x > 0; x--) if (row[size_t(x - 1) * 4 + 3]) return x;
    return 0;
}
} // namespace

PixelBounds alphaBounds(const Image& image, const PixelBounds& within) {
    PixelBounds b;
    const int wx0 = std::max(0, within.x0), wy0 = std::max(0, within.y0), wx1 = std::min(image.width(), within.x1), wy1 = std::min(image.height(), within.y1);
    int x0 = image.width(), y0 = image.height(), x1 = 0, y1 = 0;
    std::mutex merge;
    // Bands of rows on every core, each finding its own extent; a large layer is 16 M pixels to look through.
    parallelFor(wy0, wy1, 64, [&](int ya, int yb) {
        int bx0 = image.width(), by0 = image.height(), bx1 = 0, by1 = 0;
        for (int y = ya; y < yb; y++) {
            const uint8_t* row = image.row(y) + size_t(wx0) * 4;
            int first = firstOpaque(row, wx1 - wx0);
            if (first == wx1 - wx0) continue;
            bx0 = std::min(bx0, wx0 + first);
            bx1 = std::max(bx1, wx0 + endOpaque(row, wx1 - wx0));
            by0 = std::min(by0, y);
            by1 = y + 1;
        }
        if (bx1 <= bx0) return;
        std::lock_guard<std::mutex> lock(merge);
        x0 = std::min(x0, bx0); x1 = std::max(x1, bx1); y0 = std::min(y0, by0); y1 = std::max(y1, by1);
    });
    if (x1 > x0 && y1 > y0) { b.x0 = x0; b.y0 = y0; b.x1 = x1; b.y1 = y1; }
    return b;
}

PixelBounds alphaBounds(const Image& image) { return alphaBounds(image, PixelBounds{0, 0, image.width(), image.height()}); }

namespace {
/// First nonzero byte index in [0, n), or n: eight bytes at a time.
int firstNonzero(const uint8_t* p, int n) {
    int x = 0;
    for (; x + 8 <= n; x += 8) {
        uint64_t word;
        std::memcpy(&word, p + x, 8);
        if (word) break;
    }
    for (; x < n; x++) if (p[x]) return x;
    return n;
}
/// Last nonzero byte index in [0, n) plus one, or 0.
int endNonzero(const uint8_t* p, int n) {
    int x = n;
    for (; x - 8 >= 0; x -= 8) {
        uint64_t word;
        std::memcpy(&word, p + x - 8, 8);
        if (word) break;
    }
    for (; x > 0; x--) if (p[x - 1]) return x;
    return 0;
}
} // namespace

PixelBounds nonzeroBounds(const GrayImage& image) {
    PixelBounds b;
    int x0 = image.width(), y0 = image.height(), x1 = 0, y1 = 0;
    for (int y = 0; y < image.height(); y++) {
        const uint8_t* p = image.row(y);
        int first = firstNonzero(p, image.width());
        if (first == image.width()) continue;
        x0 = std::min(x0, first);
        x1 = std::max(x1, endNonzero(p, image.width()));
        y0 = std::min(y0, y);
        y1 = y + 1;
    }
    if (x1 > x0 && y1 > y0) { b.x0 = x0; b.y0 = y0; b.x1 = x1; b.y1 = y1; }
    return b;
}

std::shared_ptr<Image> cropImage(const Image& image, int x, int y, int width, int height) {
    auto out = std::make_shared<Image>(std::max(0, width), std::max(0, height));
    // Outside the source stays transparent (zero pages); inside, whole row spans on every core.
    const int i0 = std::max(0, -x), i1 = std::min(out->width(), image.width() - x);
    if (i1 <= i0) return out;
    parallelFor(0, out->height(), 128, [&](int ja, int jb) {
        for (int j = ja; j < jb; j++) {
            const int sy = y + j;
            if (sy < 0 || sy >= image.height()) continue;
            std::memcpy(out->pixel(i0, j), image.pixel(x + i0, sy), size_t(i1 - i0) * 4);
        }
    });
    return out;
}

std::shared_ptr<GrayImage> cropGray(const GrayImage& image, int x, int y, int width, int height) {
    auto out = std::make_shared<GrayImage>(std::max(0, width), std::max(0, height));
    const int i0 = std::max(0, -x), i1 = std::min(out->width(), image.width() - x);
    if (i1 <= i0) return out;
    parallelFor(0, out->height(), 256, [&](int ja, int jb) {
        for (int j = ja; j < jb; j++) {
            const int sy = y + j;
            if (sy < 0 || sy >= image.height()) continue;
            std::memcpy(out->row(j) + i0, image.row(sy) + x + i0, size_t(i1 - i0));
        }
    });
    return out;
}

namespace {

using namespace simd;

/// Two output pixels from four source pixels of each of two rows: (p00 + p01 + p10 + p11 + 2) / 4 per channel.
inline void halvePairRGBA(const uint8_t* r0, const uint8_t* r1, uint8_t* o) {
    u8x16 a, b;
    std::memcpy(&a, r0, 16); std::memcpy(&b, r1, 16);
    // Row sums in 16 bits, then each pair of neighbouring pixels.
    const u16x8 lo = widenLow(a) + widenLow(b), hi = widenHigh(a) + widenHigh(b);
    const u16x8 left = COMPOSITOR_SHUFFLE(u16x8, lo, hi, 0, 1, 2, 3, 8, 9, 10, 11), right = COMPOSITOR_SHUFFLE(u16x8, lo, hi, 4, 5, 6, 7, 12, 13, 14, 15);
    const u16x8 sum = (left + right + 2) >> 2;
    const u8x8 out = __builtin_convertvector(sum, u8x8);
    std::memcpy(o, &out, 8);
}

/// Sixteen gray output pixels from thirty-two source pixels of each of two rows.
inline void halveRunGray(const uint8_t* r0, const uint8_t* r1, uint8_t* o) {
    u8x16 a0, a1, b0, b1;
    std::memcpy(&a0, r0, 16); std::memcpy(&a1, r0 + 16, 16); std::memcpy(&b0, r1, 16); std::memcpy(&b1, r1 + 16, 16);
    const u8x16 evens = COMPOSITOR_SHUFFLE(u8x16, a0, a1, 0, 2, 4, 6, 8, 10, 12, 14, 16, 18, 20, 22, 24, 26, 28, 30);
    const u8x16 odds = COMPOSITOR_SHUFFLE(u8x16, a0, a1, 1, 3, 5, 7, 9, 11, 13, 15, 17, 19, 21, 23, 25, 27, 29, 31);
    const u8x16 evensBelow = COMPOSITOR_SHUFFLE(u8x16, b0, b1, 0, 2, 4, 6, 8, 10, 12, 14, 16, 18, 20, 22, 24, 26, 28, 30);
    const u8x16 oddsBelow = COMPOSITOR_SHUFFLE(u8x16, b0, b1, 1, 3, 5, 7, 9, 11, 13, 15, 17, 19, 21, 23, 25, 27, 29, 31);
    const u16x8 lo = (widenLow(evens) + widenLow(odds) + widenLow(evensBelow) + widenLow(oddsBelow) + 2) >> 2;
    const u16x8 hi = (widenHigh(evens) + widenHigh(odds) + widenHigh(evensBelow) + widenHigh(oddsBelow) + 2) >> 2;
    const u8x8 outLo = __builtin_convertvector(lo, u8x8), outHi = __builtin_convertvector(hi, u8x8);
    std::memcpy(o, &outLo, 8); std::memcpy(o + 8, &outHi, 8);
}

} // namespace

std::shared_ptr<Image> halveImage(const Image& image) {
    const int sw = image.width(), sh = image.height();
    const int w = (sw + 1) / 2, h = (sh + 1) / 2;
    auto out = std::make_shared<Image>(w, h);
    const int pairs = sw / 4;   // output pixel pairs whose four source pixels all exist
    parallelRows(0, h, [&](int y0, int y1) {
        for (int y = y0; y < y1; y++) {
            const uint8_t* r0 = image.row(std::min(2 * y, sh - 1));
            const uint8_t* r1 = image.row(std::min(2 * y + 1, sh - 1));
            uint8_t* o = out->row(y);
            for (int i = 0; i < pairs; i++) halvePairRGBA(r0 + size_t(i) * 16, r1 + size_t(i) * 16, o + size_t(i) * 8);
            for (int x = pairs * 2; x < w; x++) {
                int x0 = std::min(2 * x, sw - 1) * 4, x1 = std::min(2 * x + 1, sw - 1) * 4;
                for (int c = 0; c < 4; c++) o[x * 4 + c] = uint8_t((int(r0[x0 + c]) + r0[x1 + c] + r1[x0 + c] + r1[x1 + c] + 2) / 4);
            }
        }
    }, 64);
    return out;
}

std::shared_ptr<GrayImage> halveGray(const GrayImage& image) {
    const int sw = image.width(), sh = image.height();
    const int w = (sw + 1) / 2, h = (sh + 1) / 2;
    auto out = std::make_shared<GrayImage>(w, h);
    const int runs = sw / 32;
    parallelRows(0, h, [&](int y0, int y1) {
        for (int y = y0; y < y1; y++) {
            const uint8_t* r0 = image.row(std::min(2 * y, sh - 1));
            const uint8_t* r1 = image.row(std::min(2 * y + 1, sh - 1));
            uint8_t* o = out->row(y);
            for (int i = 0; i < runs; i++) halveRunGray(r0 + size_t(i) * 32, r1 + size_t(i) * 32, o + size_t(i) * 16);
            for (int x = runs * 16; x < w; x++) {
                int x0 = std::min(2 * x, sw - 1), x1 = std::min(2 * x + 1, sw - 1);
                o[x] = uint8_t((int(r0[x0]) + r0[x1] + r1[x0] + r1[x1] + 2) / 4);
            }
        }
    }, 128);
    return out;
}

namespace {

// Area-averaging reduction to exactly `w` x `h` (used for thumbnails; box filter over the covered source area).
// A large box is sampled on a grid of at most 8 x 8 points instead of read entirely: a thumbnail of a
// 12-megapixel layer then costs a few hundred thousand reads, not twelve million.
template <int Channels, typename Img>
std::shared_ptr<Img> boxResize(const Img& image, int w, int h) {
    auto out = std::make_shared<Img>(w, h);
    double sx = double(image.width()) / w, sy = double(image.height()) / h;
    const int stepX = std::max(1, int(sx / 8)), stepY = std::max(1, int(sy / 8));
    for (int y = 0; y < h; y++) {
        int y0 = int(std::floor(y * sy)), y1 = std::max(y0 + 1, int(std::floor((y + 1) * sy)));
        y1 = std::min(y1, image.height());
        for (int x = 0; x < w; x++) {
            int x0 = int(std::floor(x * sx)), x1 = std::max(x0 + 1, int(std::floor((x + 1) * sx)));
            x1 = std::min(x1, image.width());
            long sum[Channels] = {0};
            long count = 0;
            for (int j = y0 + stepY / 2; j < y1; j += stepY) {
                const uint8_t* p = image.row(j) + (x0 + stepX / 2) * Channels;
                for (int i = x0 + stepX / 2; i < x1; i += stepX, p += Channels * stepX) { for (int c = 0; c < Channels; c++) sum[c] += p[c]; count++; }
            }
            uint8_t* o = out->row(y) + x * Channels;
            for (int c = 0; c < Channels; c++) o[c] = count ? uint8_t((sum[c] + count / 2) / count) : 0;
        }
    }
    return out;
}

} // namespace

std::shared_ptr<Image> makeThumbnail(const Image& image, int maxSide) {
    if (image.isEmpty()) return std::make_shared<Image>(1, 1);
    double factor = std::min(1.0, double(maxSide) / std::max(image.width(), image.height()));
    int w = std::max(1, int(image.width() * factor)), h = std::max(1, int(image.height() * factor));
    if (w == image.width() && h == image.height()) return std::make_shared<Image>(image);
    return boxResize<4>(image, w, h);
}

std::shared_ptr<GrayImage> makeGrayThumbnail(const GrayImage& image, int maxSide) {
    if (image.isEmpty()) return std::make_shared<GrayImage>(1, 1);
    double factor = std::min(1.0, double(maxSide) / std::max(image.width(), image.height()));
    int w = std::max(1, int(image.width() * factor)), h = std::max(1, int(image.height() * factor));
    if (w == image.width() && h == image.height()) return std::make_shared<GrayImage>(image);
    return boxResize<1>(image, w, h);
}

void premultiply(Image& image) {
    for (int y = 0; y < image.height(); y++) {
        uint8_t* p = image.row(y);
        for (int x = 0; x < image.width(); x++, p += 4) {
            unsigned a = p[3];
            if (a == 255) continue;
            if (a == 0) { p[0] = p[1] = p[2] = 0; continue; }
            for (int c = 0; c < 3; c++) p[c] = uint8_t((p[c] * a + 127) / 255);
        }
    }
}

void unpremultiply(Image& image) {
    for (int y = 0; y < image.height(); y++) {
        uint8_t* p = image.row(y);
        for (int x = 0; x < image.width(); x++, p += 4) {
            unsigned a = p[3];
            if (a == 255 || a == 0) continue;
            for (int c = 0; c < 3; c++) p[c] = uint8_t(std::min(255u, (p[c] * 255u + a / 2) / a));
        }
    }
}

MipCache& MipCache::shared() { static MipCache cache; return cache; }

int MipCache::levelFor(double factor, bool rounded) {
    if (!(factor > 0) || factor >= 1) return 0;
    double halvings = std::log2(1.0 / factor);
    int level = int(std::floor(rounded ? halvings + 0.5 : halvings));
    return std::max(0, std::min(level, 16));
}

std::shared_ptr<Image> reduceImage(const Image& image, int level) {
    if (level <= 0) return nullptr;
    std::shared_ptr<Image> out = halveImage(image);
    for (int i = 1; i < level && (out->width() > 1 || out->height() > 1); i++) out = halveImage(*out);
    return out;
}

std::shared_ptr<GrayImage> reduceGray(const GrayImage& image, int level) {
    if (level <= 0) return nullptr;
    std::shared_ptr<GrayImage> out = halveGray(image);
    for (int i = 1; i < level && (out->width() > 1 || out->height() > 1); i++) out = halveGray(*out);
    return out;
}

template <typename Img>
std::shared_ptr<const Img> MipCache::levelOf(std::vector<Entry<Img>>& entries, const std::shared_ptr<const Img>& image, int level) {
    std::lock_guard<std::mutex> lock(mutex_);
    // Drop entries whose source is gone.
    for (auto it = entries.begin(); it != entries.end();) {
        if (it->source.expired()) { used_ -= it->bytes; it = entries.erase(it); }
        else ++it;
    }
    Entry<Img>* entry = nullptr;
    for (auto& e : entries) if (e.source.lock() == image) { entry = &e; break; }
    if (!entry) { entries.push_back({image, {}, 0, 0}); entry = &entries.back(); }
    entry->lastUse = ++clock_;
    while (int(entry->levels.size()) < level) {
        const Img& last = entry->levels.empty() ? *image : *entry->levels.back();
        if (last.width() <= 1 && last.height() <= 1) break;
        std::shared_ptr<const Img> next;
        if constexpr (std::is_same_v<Img, Image>) next = halveImage(last); else next = halveGray(last);
        entry->bytes += next->byteCount();
        used_ += next->byteCount();
        entry->levels.push_back(next);
    }
    std::shared_ptr<const Img> result = entry->levels.empty() ? image : entry->levels[size_t(std::min(level, int(entry->levels.size())) - 1)];
    enforceBudget(entry->lastUse);
    return result;
}

ImagePtr MipCache::level(const ImagePtr& image, int level) {
    if (!image || level <= 0) return image;
    return levelOf(entries_, image, level);
}

GrayPtr MipCache::level(const GrayPtr& image, int level) {
    if (!image || level <= 0) return image;
    return levelOf(grayEntries_, image, level);
}

void MipCache::enforceBudget(uint64_t keep) {
    // The least recently used entries go first; the one just used stays whatever its size.
    while (used_ > budget_) {
        uint64_t oldest = keep;
        int which = -1; size_t index = 0;
        for (size_t i = 0; i < entries_.size(); i++) if (entries_[i].lastUse < oldest) { oldest = entries_[i].lastUse; which = 0; index = i; }
        for (size_t i = 0; i < grayEntries_.size(); i++) if (grayEntries_[i].lastUse < oldest) { oldest = grayEntries_[i].lastUse; which = 1; index = i; }
        if (which < 0) return;
        if (which == 0) { used_ -= entries_[index].bytes; entries_.erase(entries_.begin() + long(index)); }
        else { used_ -= grayEntries_[index].bytes; grayEntries_.erase(grayEntries_.begin() + long(index)); }
    }
}

void MipCache::setBudget(size_t bytes) {
    std::lock_guard<std::mutex> lock(mutex_);
    budget_ = bytes;
    enforceBudget(clock_ + 1);
}

void MipCache::clear() { std::lock_guard<std::mutex> lock(mutex_); entries_.clear(); grayEntries_.clear(); used_ = 0; }

} // namespace compositor
