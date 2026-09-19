#include "compositor/png.h"
#include <csetjmp>
#include <cstdio>
#include <cstring>
#include <png.h>

namespace compositor {

namespace {

struct Reader {
    FILE* file = nullptr;
    png_structp png = nullptr;
    png_infop info = nullptr;
    ~Reader() {
        if (png) png_destroy_read_struct(&png, info ? &info : nullptr, nullptr);
        if (file) std::fclose(file);
    }
    bool open(const std::string& path, std::string* error) {
        file = std::fopen(path.c_str(), "rb");
        if (!file) { if (error) *error = "cannot open " + path; return false; }
        unsigned char header[8];
        if (std::fread(header, 1, 8, file) != 8 || png_sig_cmp(header, 0, 8)) { if (error) *error = "not a PNG file"; return false; }
        png = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
        info = png ? png_create_info_struct(png) : nullptr;
        if (!png || !info) { if (error) *error = "libpng initialisation failed"; return false; }
        png_init_io(png, file);
        png_set_sig_bytes(png, 8);
        return true;
    }
};

struct MemorySource { const uint8_t* data; size_t size; size_t offset; };
void readFromMemory(png_structp png, png_bytep out, png_size_t length) {
    auto* src = static_cast<MemorySource*>(png_get_io_ptr(png));
    if (src->offset + length > src->size) { png_error(png, "truncated PNG"); return; }
    std::memcpy(out, src->data + src->offset, length);
    src->offset += length;
}

void writeToVector(png_structp png, png_bytep data, png_size_t length) {
    auto* out = static_cast<std::vector<uint8_t>*>(png_get_io_ptr(png));
    out->insert(out->end(), data, data + length);
}
void flushNothing(png_structp) {}

std::shared_ptr<Image> readRgba(png_structp png, png_infop info, std::string* error) {
    if (setjmp(png_jmpbuf(png))) { if (error) *error = "PNG decoding failed"; return nullptr; }
    png_read_info(png, info);
    png_uint_32 width = png_get_image_width(png, info), height = png_get_image_height(png, info);
    int depth = png_get_bit_depth(png, info), type = png_get_color_type(png, info);
    if (width == 0 || height == 0 || width > 30000 || height > 30000) { if (error) *error = "PNG too large"; return nullptr; }
    if (type == PNG_COLOR_TYPE_PALETTE) png_set_palette_to_rgb(png);
    if (type == PNG_COLOR_TYPE_GRAY && depth < 8) png_set_expand_gray_1_2_4_to_8(png);
    if (png_get_valid(png, info, PNG_INFO_tRNS)) png_set_tRNS_to_alpha(png);
    if (depth == 16) png_set_strip_16(png);
    if (depth < 8) png_set_packing(png);
    if (type == PNG_COLOR_TYPE_GRAY || type == PNG_COLOR_TYPE_GRAY_ALPHA) png_set_gray_to_rgb(png);
    png_set_filler(png, 0xFF, PNG_FILLER_AFTER);
    png_set_interlace_handling(png);
    png_read_update_info(png, info);
    auto image = std::make_shared<Image>(int(width), int(height));
    std::vector<png_bytep> rows(height);
    for (png_uint_32 y = 0; y < height; y++) rows[y] = image->row(int(y));
    png_read_image(png, rows.data());
    png_read_end(png, nullptr);
    premultiply(*image);
    return image;
}

} // namespace

bool readPngInfo(const std::string& path, PngInfo& out, std::string* error) {
    Reader r;
    if (!r.open(path, error)) return false;
    if (setjmp(png_jmpbuf(r.png))) { if (error) *error = "PNG header unreadable"; return false; }
    png_read_info(r.png, r.info);
    out.width = int(png_get_image_width(r.png, r.info));
    out.height = int(png_get_image_height(r.png, r.info));
    out.bitDepth = png_get_bit_depth(r.png, r.info);
    int type = png_get_color_type(r.png, r.info);
    out.gray = type == PNG_COLOR_TYPE_GRAY;
    return true;
}

std::shared_ptr<Image> readPngImage(const std::string& path, std::string* error) {
    Reader r;
    if (!r.open(path, error)) return nullptr;
    return readRgba(r.png, r.info, error);
}

std::shared_ptr<Image> decodePngImage(const uint8_t* data, size_t size, std::string* error) {
    if (size < 8 || png_sig_cmp(data, 0, 8)) { if (error) *error = "not a PNG"; return nullptr; }
    png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    png_infop info = png ? png_create_info_struct(png) : nullptr;
    if (!png || !info) { if (png) png_destroy_read_struct(&png, nullptr, nullptr); if (error) *error = "libpng initialisation failed"; return nullptr; }
    MemorySource source{data, size, 0};
    png_set_read_fn(png, &source, readFromMemory);
    auto image = readRgba(png, info, error);
    png_destroy_read_struct(&png, &info, nullptr);
    return image;
}

std::shared_ptr<GrayImage> readPngGray(const std::string& path, std::string* error) {
    Reader r;
    if (!r.open(path, error)) return nullptr;
    if (setjmp(png_jmpbuf(r.png))) { if (error) *error = "PNG decoding failed"; return nullptr; }
    png_read_info(r.png, r.info);
    png_uint_32 width = png_get_image_width(r.png, r.info), height = png_get_image_height(r.png, r.info);
    int depth = png_get_bit_depth(r.png, r.info), type = png_get_color_type(r.png, r.info);
    if (type != PNG_COLOR_TYPE_GRAY || depth > 8 || png_get_valid(r.png, r.info, PNG_INFO_tRNS)) {
        if (error) *error = "mask is not 8-bit grayscale without alpha";
        return nullptr;
    }
    if (width == 0 || height == 0 || width > 30000 || height > 30000) { if (error) *error = "PNG too large"; return nullptr; }
    if (depth < 8) png_set_expand_gray_1_2_4_to_8(r.png);
    png_set_interlace_handling(r.png);
    png_read_update_info(r.png, r.info);
    auto image = std::make_shared<GrayImage>(int(width), int(height));
    std::vector<png_bytep> rows(height);
    for (png_uint_32 y = 0; y < height; y++) rows[y] = image->row(int(y));
    png_read_image(r.png, rows.data());
    png_read_end(r.png, nullptr);
    return image;
}

namespace {

bool writePng(png_structp png, png_infop info, int width, int height, int colorType, const std::vector<png_bytep>& rows, double dpi, std::string* error) {
    if (setjmp(png_jmpbuf(png))) { if (error) *error = "PNG encoding failed"; return false; }
    png_set_IHDR(png, info, png_uint_32(width), png_uint_32(height), 8, colorType, PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    if (colorType == PNG_COLOR_TYPE_RGBA) png_set_sRGB_gAMA_and_cHRM(png, info, PNG_sRGB_INTENT_PERCEPTUAL);
    if (dpi > 0) {
        png_uint_32 ppm = png_uint_32(dpi / 0.0254 + 0.5);
        png_set_pHYs(png, info, ppm, ppm, PNG_RESOLUTION_METER);
    }
    png_write_info(png, info);
    png_write_image(png, const_cast<png_bytep*>(rows.data()));
    png_write_end(png, nullptr);
    return true;
}

} // namespace

bool encodePngImage(const Image& image, std::vector<uint8_t>& out, double dpi, std::string* error) {
    Image straight = image;
    unpremultiply(straight);
    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    png_infop info = png ? png_create_info_struct(png) : nullptr;
    if (!png || !info) { if (png) png_destroy_write_struct(&png, nullptr); if (error) *error = "libpng initialisation failed"; return false; }
    out.clear();
    png_set_write_fn(png, &out, writeToVector, flushNothing);
    std::vector<png_bytep> rows(size_t(straight.height()));
    for (int y = 0; y < straight.height(); y++) rows[size_t(y)] = straight.row(y);
    bool ok = writePng(png, info, straight.width(), straight.height(), PNG_COLOR_TYPE_RGBA, rows, dpi, error);
    png_destroy_write_struct(&png, &info);
    return ok;
}

bool writePngImage(const std::string& path, const Image& image, double dpi, std::string* error) {
    std::vector<uint8_t> bytes;
    if (!encodePngImage(image, bytes, dpi, error)) return false;
    FILE* file = std::fopen(path.c_str(), "wb");
    if (!file) { if (error) *error = "cannot write " + path; return false; }
    bool ok = std::fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size();
    ok = std::fclose(file) == 0 && ok;
    if (!ok && error) *error = "write failed for " + path;
    return ok;
}

bool writePngGray(const std::string& path, const GrayImage& image, std::string* error) {
    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    png_infop info = png ? png_create_info_struct(png) : nullptr;
    if (!png || !info) { if (png) png_destroy_write_struct(&png, nullptr); if (error) *error = "libpng initialisation failed"; return false; }
    std::vector<uint8_t> out;
    png_set_write_fn(png, &out, writeToVector, flushNothing);
    std::vector<png_bytep> rows(size_t(image.height()));
    for (int y = 0; y < image.height(); y++) rows[size_t(y)] = const_cast<png_bytep>(image.row(y));
    bool ok = writePng(png, info, image.width(), image.height(), PNG_COLOR_TYPE_GRAY, rows, 0, error);
    png_destroy_write_struct(&png, &info);
    if (!ok) return false;
    FILE* file = std::fopen(path.c_str(), "wb");
    if (!file) { if (error) *error = "cannot write " + path; return false; }
    ok = std::fwrite(out.data(), 1, out.size(), file) == out.size();
    ok = std::fclose(file) == 0 && ok;
    return ok;
}

} // namespace compositor
