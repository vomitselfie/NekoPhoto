#include "compositor/image.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace compositor {

Image::Image(int width, int height)
    : width_(std::max(0, width)), height_(std::max(0, height)), stride_(std::max(0, width) * 4),
      pixels_(size_t(stride_) * size_t(height_), 0) {}

void Image::fill(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    for (int y = 0; y < height_; y++) {
        uint8_t* p = row(y);
        for (int x = 0; x < width_; x++, p += 4) { p[0] = r; p[1] = g; p[2] = b; p[3] = a; }
    }
}

GrayImage::GrayImage(int width, int height, uint8_t value)
    : width_(std::max(0, width)), height_(std::max(0, height)), pixels_(size_t(width_) * size_t(height_), value) {}

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
    for (int y = wy0; y < wy1; y++) {
        const uint8_t* row = image.row(y) + size_t(wx0) * 4;
        int first = firstOpaque(row, wx1 - wx0);
        if (first == wx1 - wx0) continue;
        x0 = std::min(x0, wx0 + first);
        x1 = std::max(x1, wx0 + endOpaque(row, wx1 - wx0));
        y0 = std::min(y0, y);
        y1 = y + 1;
    }
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
    for (int j = 0; j < out->height(); j++) {
        int sy = y + j;
        if (sy < 0 || sy >= image.height()) continue;
        for (int i = 0; i < out->width(); i++) {
            int sx = x + i;
            if (sx < 0 || sx >= image.width()) continue;
            std::memcpy(out->pixel(i, j), image.pixel(sx, sy), 4);
        }
    }
    return out;
}

std::shared_ptr<GrayImage> cropGray(const GrayImage& image, int x, int y, int width, int height) {
    auto out = std::make_shared<GrayImage>(std::max(0, width), std::max(0, height));
    for (int j = 0; j < out->height(); j++) {
        int sy = y + j;
        if (sy < 0 || sy >= image.height()) continue;
        for (int i = 0; i < out->width(); i++) {
            int sx = x + i;
            if (sx < 0 || sx >= image.width()) continue;
            out->at(i, j) = image.at(sx, sy);
        }
    }
    return out;
}

std::shared_ptr<Image> halveImage(const Image& image) {
    int w = (image.width() + 1) / 2, h = (image.height() + 1) / 2;
    auto out = std::make_shared<Image>(w, h);
    for (int y = 0; y < h; y++) {
        int y0 = std::min(2 * y, image.height() - 1), y1 = std::min(2 * y + 1, image.height() - 1);
        const uint8_t* r0 = image.row(y0);
        const uint8_t* r1 = image.row(y1);
        uint8_t* o = out->row(y);
        for (int x = 0; x < w; x++, o += 4) {
            int x0 = std::min(2 * x, image.width() - 1) * 4, x1 = std::min(2 * x + 1, image.width() - 1) * 4;
            for (int c = 0; c < 4; c++)
                o[c] = uint8_t((int(r0[x0 + c]) + r0[x1 + c] + r1[x0 + c] + r1[x1 + c] + 2) / 4);
        }
    }
    return out;
}

std::shared_ptr<GrayImage> halveGray(const GrayImage& image) {
    int w = (image.width() + 1) / 2, h = (image.height() + 1) / 2;
    auto out = std::make_shared<GrayImage>(w, h);
    for (int y = 0; y < h; y++) {
        int y0 = std::min(2 * y, image.height() - 1), y1 = std::min(2 * y + 1, image.height() - 1);
        const uint8_t* r0 = image.row(y0);
        const uint8_t* r1 = image.row(y1);
        uint8_t* o = out->row(y);
        for (int x = 0; x < w; x++) {
            int x0 = std::min(2 * x, image.width() - 1), x1 = std::min(2 * x + 1, image.width() - 1);
            o[x] = uint8_t((int(r0[x0]) + r0[x1] + r1[x0] + r1[x1] + 2) / 4);
        }
    }
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

int MipCache::levelFor(double factor) {
    if (!(factor > 0) || factor >= 1) return 0;
    int level = int(std::floor(std::log2(1.0 / factor)));
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

ImagePtr MipCache::level(const ImagePtr& image, int level) {
    if (!image || level <= 0) return image;
    std::lock_guard<std::mutex> lock(mutex_);
    // Drop entries whose source is gone.
    entries_.erase(std::remove_if(entries_.begin(), entries_.end(), [](const Entry& e) { return e.source.expired(); }), entries_.end());
    Entry* entry = nullptr;
    for (auto& e : entries_) if (e.source.lock() == image) { entry = &e; break; }
    if (!entry) { entries_.push_back({image, {image}}); entry = &entries_.back(); }
    while (int(entry->levels.size()) <= level) {
        const ImagePtr& last = entry->levels.back();
        if (last->width() <= 1 && last->height() <= 1) return last;
        entry->levels.push_back(halveImage(*last));
    }
    return entry->levels[size_t(level)];
}

GrayPtr MipCache::level(const GrayPtr& image, int level) {
    if (!image || level <= 0) return image;
    std::lock_guard<std::mutex> lock(mutex_);
    grayEntries_.erase(std::remove_if(grayEntries_.begin(), grayEntries_.end(), [](const GrayEntry& e) { return e.source.expired(); }), grayEntries_.end());
    GrayEntry* entry = nullptr;
    for (auto& e : grayEntries_) if (e.source.lock() == image) { entry = &e; break; }
    if (!entry) { grayEntries_.push_back({image, {image}}); entry = &grayEntries_.back(); }
    while (int(entry->levels.size()) <= level) {
        const GrayPtr& last = entry->levels.back();
        if (last->width() <= 1 && last->height() <= 1) return last;
        entry->levels.push_back(halveGray(*last));
    }
    return entry->levels[size_t(level)];
}

void MipCache::clear() { std::lock_guard<std::mutex> lock(mutex_); entries_.clear(); grayEntries_.clear(); }

} // namespace compositor
