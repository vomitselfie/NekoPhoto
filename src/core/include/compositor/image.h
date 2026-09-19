// Pixel buffers. The one canonical in-memory format, matching the Mac app:
//   RGBA, 8 bits per channel, premultiplied alpha, sRGB, rows top-down, explicit byte stride.
// Masks and coverage are 8-bit gray, white = full.
// Buffers are immutable once shared (ImagePtr / GrayPtr), so undo snapshots and
// renders can hold them without copying.
#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace compositor {

class Image {
public:
    Image() = default;
    Image(int width, int height);
    int width() const { return width_; }
    int height() const { return height_; }
    /// Bytes per row.
    int stride() const { return stride_; }
    bool isEmpty() const { return width_ <= 0 || height_ <= 0; }
    uint8_t* data() { return pixels_.data(); }
    const uint8_t* data() const { return pixels_.data(); }
    uint8_t* row(int y) { return pixels_.data() + size_t(y) * stride_; }
    const uint8_t* row(int y) const { return pixels_.data() + size_t(y) * stride_; }
    uint8_t* pixel(int x, int y) { return row(y) + x * 4; }
    const uint8_t* pixel(int x, int y) const { return row(y) + x * 4; }
    size_t byteCount() const { return pixels_.size(); }
    void fill(uint8_t r, uint8_t g, uint8_t b, uint8_t a);
    void clear() { fill(0, 0, 0, 0); }
    bool operator==(const Image& other) const { return width_ == other.width_ && height_ == other.height_ && pixels_ == other.pixels_; }

private:
    int width_ = 0, height_ = 0, stride_ = 0;
    std::vector<uint8_t> pixels_;
};

class GrayImage {
public:
    GrayImage() = default;
    GrayImage(int width, int height, uint8_t value = 0);
    int width() const { return width_; }
    int height() const { return height_; }
    int stride() const { return width_; }
    bool isEmpty() const { return width_ <= 0 || height_ <= 0; }
    uint8_t* data() { return pixels_.data(); }
    const uint8_t* data() const { return pixels_.data(); }
    uint8_t* row(int y) { return pixels_.data() + size_t(y) * width_; }
    const uint8_t* row(int y) const { return pixels_.data() + size_t(y) * width_; }
    uint8_t at(int x, int y) const { return row(y)[x]; }
    uint8_t& at(int x, int y) { return row(y)[x]; }
    size_t byteCount() const { return pixels_.size(); }
    void fill(uint8_t value);
    bool operator==(const GrayImage& other) const { return width_ == other.width_ && height_ == other.height_ && pixels_ == other.pixels_; }

private:
    int width_ = 0, height_ = 0;
    std::vector<uint8_t> pixels_;
};

using ImagePtr = std::shared_ptr<const Image>;
using GrayPtr = std::shared_ptr<const GrayImage>;

/// Half-open bounds of the pixels with nonzero alpha: {x0, y0, x1, y1}; all zero when empty.
struct PixelBounds { int x0 = 0, y0 = 0, x1 = 0, y1 = 0; bool isEmpty() const { return x1 <= x0 || y1 <= y0; } };
PixelBounds alphaBounds(const Image& image);
/// The same, scanning only `within` (pixel bounds); useful when everything outside is known to be unchanged.
PixelBounds alphaBounds(const Image& image, const PixelBounds& within);
PixelBounds nonzeroBounds(const GrayImage& image);

std::shared_ptr<Image> cropImage(const Image& image, int x, int y, int width, int height);
std::shared_ptr<GrayImage> cropGray(const GrayImage& image, int x, int y, int width, int height);

/// Box-filtered halving (each output pixel the average of 2x2 inputs), the sharp reduction the Mac uses for large scale-downs.
std::shared_ptr<Image> halveImage(const Image& image);
std::shared_ptr<GrayImage> halveGray(const GrayImage& image);
/// `level` halvings built on the spot (nothing cached): for images that have no shared owner. Level 0 returns null.
std::shared_ptr<Image> reduceImage(const Image& image, int level);
std::shared_ptr<GrayImage> reduceGray(const GrayImage& image, int level);

/// A reduction with the longest side at most `maxSide` pixels, box-filtered.
std::shared_ptr<Image> makeThumbnail(const Image& image, int maxSide = 96);
std::shared_ptr<GrayImage> makeGrayThumbnail(const GrayImage& image, int maxSide = 96);

/// Straight (non-premultiplied) <-> premultiplied conversion, as when reading and writing PNGs.
void premultiply(Image& image);
void unpremultiply(Image& image);

/// Power-of-two reductions of an image, built on demand and cached by image identity.
class MipCache {
public:
    static MipCache& shared();
    /// `level` halvings of `image` (level 0 is the image itself).
    ImagePtr level(const ImagePtr& image, int level);
    GrayPtr level(const GrayPtr& image, int level);
    /// The level for drawing an image at `factor` destination pixels per source pixel:
    /// halvings until the final resample is at most 2x reduction.
    static int levelFor(double factor);
    void clear();

private:
    struct Entry { std::weak_ptr<const Image> source; std::vector<ImagePtr> levels; };
    struct GrayEntry { std::weak_ptr<const GrayImage> source; std::vector<GrayPtr> levels; };
    std::vector<Entry> entries_;
    std::vector<GrayEntry> grayEntries_;
    std::mutex mutex_;
};

} // namespace compositor
