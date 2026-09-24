#include "compositor/png.h"
#include "compositor/parallel.h"
#include <algorithm>
#include <atomic>
#include <csetjmp>
#include <cstdlib>
#include <functional>
#include <cstdio>
#include <cstring>
#include <png.h>
#include <zlib.h>

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

// libpng reports errors by longjmp. Every call that may jump runs in one of these functions, which create
// no C++ object after their setjmp, so a jump never skips a destructor; the images and row lists live in
// the callers.
bool pngReadInfo(png_structp png, png_infop info) {
    if (setjmp(png_jmpbuf(png))) return false;
    png_read_info(png, info);
    return true;
}

/// Asks for 8-bit RGBA whatever the file holds.
bool pngExpandToRgba(png_structp png, png_infop info) {
    if (setjmp(png_jmpbuf(png))) return false;
    const int depth = png_get_bit_depth(png, info), type = png_get_color_type(png, info);
    if (type == PNG_COLOR_TYPE_PALETTE) png_set_palette_to_rgb(png);
    if (type == PNG_COLOR_TYPE_GRAY && depth < 8) png_set_expand_gray_1_2_4_to_8(png);
    if (png_get_valid(png, info, PNG_INFO_tRNS)) png_set_tRNS_to_alpha(png);
    if (depth == 16) png_set_strip_16(png);
    if (depth < 8) png_set_packing(png);
    if (type == PNG_COLOR_TYPE_GRAY || type == PNG_COLOR_TYPE_GRAY_ALPHA) png_set_gray_to_rgb(png);
    png_set_filler(png, 0xFF, PNG_FILLER_AFTER);
    png_set_interlace_handling(png);
    png_read_update_info(png, info);
    return true;
}

bool pngExpandGray(png_structp png, png_infop info) {
    if (setjmp(png_jmpbuf(png))) return false;
    if (png_get_bit_depth(png, info) < 8) png_set_expand_gray_1_2_4_to_8(png);
    png_set_interlace_handling(png);
    png_read_update_info(png, info);
    return true;
}

bool pngReadRows(png_structp png, png_bytep* rows) {
    if (setjmp(png_jmpbuf(png))) return false;
    png_read_image(png, rows);
    png_read_end(png, nullptr);
    return true;
}

std::shared_ptr<Image> readRgba(png_structp png, png_infop info, std::string* error) {
    if (!pngReadInfo(png, info)) { if (error) *error = "PNG decoding failed"; return nullptr; }
    const png_uint_32 width = png_get_image_width(png, info), height = png_get_image_height(png, info);
    if (width == 0 || height == 0 || width > 30000 || height > 30000) { if (error) *error = "PNG too large"; return nullptr; }
    if (!pngExpandToRgba(png, info)) { if (error) *error = "PNG decoding failed"; return nullptr; }
    auto image = std::make_shared<Image>(int(width), int(height));
    std::vector<png_bytep> rows(height);
    for (png_uint_32 y = 0; y < height; y++) rows[y] = image->row(int(y));
    if (!pngReadRows(png, rows.data())) { if (error) *error = "PNG decoding failed"; return nullptr; }
    premultiply(*image);
    return image;
}

} // namespace

bool readPngInfo(const std::string& path, PngInfo& out, std::string* error) {
    Reader r;
    if (!r.open(path, error)) return false;
    if (!pngReadInfo(r.png, r.info)) { if (error) *error = "PNG header unreadable"; return false; }
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
    if (!pngReadInfo(r.png, r.info)) { if (error) *error = "PNG decoding failed"; return nullptr; }
    const png_uint_32 width = png_get_image_width(r.png, r.info), height = png_get_image_height(r.png, r.info);
    const int depth = png_get_bit_depth(r.png, r.info), type = png_get_color_type(r.png, r.info);
    if (type != PNG_COLOR_TYPE_GRAY || depth > 8 || png_get_valid(r.png, r.info, PNG_INFO_tRNS)) {
        if (error) *error = "mask is not 8-bit grayscale without alpha";
        return nullptr;
    }
    if (width == 0 || height == 0 || width > 30000 || height > 30000) { if (error) *error = "PNG too large"; return nullptr; }
    if (!pngExpandGray(r.png, r.info)) { if (error) *error = "PNG decoding failed"; return nullptr; }
    auto image = std::make_shared<GrayImage>(int(width), int(height));
    std::vector<png_bytep> rows(height);
    for (png_uint_32 y = 0; y < height; y++) rows[y] = image->row(int(y));
    if (!pngReadRows(r.png, rows.data())) { if (error) *error = "PNG decoding failed"; return nullptr; }
    return image;
}

namespace {

// PNG writing without libpng, so one image can be compressed on every core: the rows are cut into strips of
// about a megabyte, each strip is filtered and raw-deflated on its own, ends with a sync flush (the last one
// finishes the stream), and the strips join under one zlib header with their Adler-32s combined, as pigz does.
// Each strip is primed with the 32 KB of filtered data before it, so the split costs almost no compression.

void putU32(std::vector<uint8_t>& out, uint32_t v) {
    uint8_t b[4] = {uint8_t(v >> 24), uint8_t(v >> 16), uint8_t(v >> 8), uint8_t(v)};
    out.insert(out.end(), b, b + 4);
}

void putChunk(std::vector<uint8_t>& out, const char* type, const uint8_t* data, size_t size) {
    putU32(out, uint32_t(size));
    size_t at = out.size();
    out.insert(out.end(), type, type + 4);
    if (size) out.insert(out.end(), data, data + size);
    putU32(out, uint32_t(crc32(0, out.data() + at, uInt(size + 4))));
}

uint8_t paeth(int a, int b, int c) {
    int p = a + b - c, pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
    return uint8_t(pa <= pb && pa <= pc ? a : pb <= pc ? b : c);
}

/// libpng's default choice for 8-bit images: every filter is tried and the row keeps the one whose bytes,
/// read as signed, have the smallest absolute sum. `out` gets the filter type byte and the filtered row.
void filterRow(const uint8_t* row, const uint8_t* prior, int bytes, int bpp, uint8_t* out, uint8_t* trial) {
    unsigned best = ~0u;
    for (int type = 0; type < 5; type++) {
        unsigned sum = 0;
        for (int i = 0; i < bytes; i++) {
            int left = i >= bpp ? row[i - bpp] : 0, up = prior[i], corner = i >= bpp ? prior[i - bpp] : 0;
            uint8_t v = row[i];
            switch (type) {
            case 1: v = uint8_t(v - left); break;
            case 2: v = uint8_t(v - up); break;
            case 3: v = uint8_t(v - ((left + up) >> 1)); break;
            case 4: v = uint8_t(v - paeth(left, up, corner)); break;
            default: break;
            }
            trial[i] = v;
            sum += unsigned(std::abs(int(int8_t(v))));
        }
        if (sum < best) { best = sum; out[0] = uint8_t(type); std::memcpy(out + 1, trial, size_t(bytes)); }
    }
}

using RowSource = std::function<void(int y, uint8_t* out)>;

bool encodePng(int width, int height, int colorType, int bpp, const RowSource& source, double dpi, int level,
               std::vector<uint8_t>& out, std::string* error) {
    if (width <= 0 || height <= 0) { if (error) *error = "PNG encoding failed: empty image"; return false; }
    const int rowBytes = width * bpp;
    const size_t lineBytes = size_t(rowBytes) + 1;
    const int stripRows = std::max(8, int((size_t(1) << 20) / lineBytes));
    const int strips = (height + stripRows - 1) / stripRows;
    const size_t window = 32768;
    const int primeRows = int((window + lineBytes - 1) / lineBytes);
    std::vector<std::vector<uint8_t>> packed(static_cast<size_t>(strips));
    std::vector<uLong> checks(static_cast<size_t>(strips));
    std::vector<size_t> lengths(static_cast<size_t>(strips));
    std::atomic<bool> failed{false};

    parallelFor(0, strips, 1, [&](int s0, int s1) {
        std::vector<uint8_t> prior(static_cast<size_t>(rowBytes)), row(static_cast<size_t>(rowBytes)), trial(static_cast<size_t>(rowBytes)), lines;
        for (int s = s0; s < s1; s++) {
            const int y0 = s * stripRows, y1 = std::min(height, y0 + stripRows);
            const int first = std::max(0, y0 - primeRows);
            lines.resize(size_t(y1 - first) * lineBytes);
            std::fill(prior.begin(), prior.end(), 0);
            if (first > 0) source(first - 1, prior.data());
            for (int y = first; y < y1; y++) {
                source(y, row.data());
                filterRow(row.data(), prior.data(), rowBytes, bpp, lines.data() + size_t(y - first) * lineBytes, trial.data());
                std::swap(row, prior);
            }
            const size_t primed = size_t(y0 - first) * lineBytes, length = lines.size() - primed;
            const uint8_t* data = lines.data() + primed;
            z_stream z{};
            if (deflateInit2(&z, level, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY) != Z_OK) { failed = true; return; }
            if (primed) {
                size_t dictionary = std::min(window, primed);
                deflateSetDictionary(&z, data - dictionary, uInt(dictionary));
            }
            std::vector<uint8_t>& chunk = packed[size_t(s)];
            chunk.resize(deflateBound(&z, uLong(length)) + 16);
            z.next_in = const_cast<Bytef*>(data);
            z.avail_in = uInt(length);
            z.next_out = chunk.data();
            z.avail_out = uInt(chunk.size());
            const bool last = s == strips - 1;
            int status = deflate(&z, last ? Z_FINISH : Z_SYNC_FLUSH);
            if ((last ? status != Z_STREAM_END : status != Z_OK) || z.avail_in != 0) failed = true;
            chunk.resize(chunk.size() - z.avail_out);
            deflateEnd(&z);
            checks[size_t(s)] = adler32(1, data, uInt(length));
            lengths[size_t(s)] = length;
        }
    });
    if (failed) { if (error) *error = "PNG encoding failed"; return false; }

    out.clear();
    static const uint8_t signature[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    out.insert(out.end(), signature, signature + 8);
    std::vector<uint8_t> header;
    putU32(header, uint32_t(width));
    putU32(header, uint32_t(height));
    header.insert(header.end(), {8, uint8_t(colorType), 0, 0, 0});
    putChunk(out, "IHDR", header.data(), header.size());
    if (colorType == 6) {
        // The chunks png_set_sRGB_gAMA_and_cHRM writes: sRGB, perceptual intent, with the matching fallbacks.
        std::vector<uint8_t> gamma, chroma;
        putU32(gamma, 45455);
        for (uint32_t v : {31270u, 32900u, 64000u, 33000u, 30000u, 60000u, 15000u, 6000u}) putU32(chroma, v);
        const uint8_t intent = 0;
        putChunk(out, "gAMA", gamma.data(), gamma.size());
        putChunk(out, "cHRM", chroma.data(), chroma.size());
        putChunk(out, "sRGB", &intent, 1);
    }
    if (dpi > 0) {
        std::vector<uint8_t> physical;
        uint32_t ppm = uint32_t(dpi / 0.0254 + 0.5);
        putU32(physical, ppm);
        putU32(physical, ppm);
        physical.push_back(1);
        putChunk(out, "pHYs", physical.data(), physical.size());
    }
    uLong check = checks[0];
    for (size_t s = 1; s < checks.size(); s++) check = adler32_combine(check, checks[s], z_off_t(lengths[s]));
    const int levelFlag = level < 2 ? 0 : level < 6 ? 1 : level == 6 ? 2 : 3;
    uint8_t flags = uint8_t(levelFlag << 6);
    flags = uint8_t(flags + 31 - ((0x78 * 256 + flags) % 31));
    for (size_t s = 0; s < packed.size(); s++) {
        std::vector<uint8_t>& data = packed[s];
        if (s == 0) data.insert(data.begin(), {0x78, flags});
        if (s + 1 == packed.size()) putU32(data, uint32_t(check));
        putChunk(out, "IDAT", data.data(), data.size());
        std::vector<uint8_t>().swap(data);
    }
    putChunk(out, "IEND", nullptr, 0);
    return true;
}

bool writeFile(const std::string& path, const std::vector<uint8_t>& bytes, std::string* error) {
    FILE* file = std::fopen(path.c_str(), "wb");
    if (!file) { if (error) *error = "cannot write " + path; return false; }
    bool ok = std::fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size();
    ok = std::fclose(file) == 0 && ok;
    if (!ok && error) *error = "write failed for " + path;
    return ok;
}

} // namespace

bool encodePngImage(const Image& image, std::vector<uint8_t>& out, double dpi, std::string* error) {
    // Straight alpha, one row at a time, with unpremultiply()'s rounding.
    auto straight = [&](int y, uint8_t* row) {
        std::memcpy(row, image.row(y), size_t(image.width()) * 4);
        for (int x = 0; x < image.width(); x++, row += 4) {
            unsigned a = row[3];
            if (a == 255 || a == 0) continue;
            for (int c = 0; c < 3; c++) row[c] = uint8_t(std::min(255u, (row[c] * 255u + a / 2) / a));
        }
    };
    return encodePng(image.width(), image.height(), 6, 4, straight, dpi, pngCompressionLevel, out, error);
}

bool writePngImage(const std::string& path, const Image& image, double dpi, std::string* error) {
    std::vector<uint8_t> bytes;
    return encodePngImage(image, bytes, dpi, error) && writeFile(path, bytes, error);
}

bool writePngGray(const std::string& path, const GrayImage& image, std::string* error) {
    std::vector<uint8_t> bytes;
    auto copy = [&](int y, uint8_t* row) { std::memcpy(row, image.row(y), size_t(image.width())); };
    return encodePng(image.width(), image.height(), 0, 1, copy, 0, pngCompressionLevel, bytes, error) && writeFile(path, bytes, error);
}

} // namespace compositor
