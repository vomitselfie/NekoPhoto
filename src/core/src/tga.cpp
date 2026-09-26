// TGA reading and writing, ported from Patchy (MIT, src/third_party/patchy_psd/README.md):
// src/formats/tga_document_io.cpp, adapted to premultiplied compositor::Image. The palette-mode writer is left
// out (NekoPhoto has no indexed documents).
#include "compositor/tga.h"
#include "compositor/document.h"
#include "format_io.h"
#include <array>

namespace compositor {

namespace {

constexpr size_t headerSize = 18;
enum : uint8_t { typeIndexed = 1, typeTruecolor = 2, typeGrayscale = 3, typeIndexedRle = 9, typeTruecolorRle = 10, typeGrayscaleRle = 11 };

struct Header {
    uint8_t idLength = 0, colorMapType = 0, imageType = 0;
    uint16_t colorMapFirst = 0, colorMapLength = 0;
    uint8_t colorMapEntryBits = 0;
    uint16_t width = 0, height = 0;
    uint8_t pixelDepth = 0, descriptor = 0;
};

Header readHeader(format_io::Reader& r) {
    Header h;
    h.idLength = r.u8();
    h.colorMapType = r.u8();
    h.imageType = r.u8();
    h.colorMapFirst = r.u16();
    h.colorMapLength = r.u16();
    h.colorMapEntryBits = r.u8();
    r.skip(4);   // x/y origin
    h.width = r.u16();
    h.height = r.u16();
    h.pixelDepth = r.u8();
    h.descriptor = r.u8();
    return h;
}

bool plausible(const Header& h) {
    switch (h.imageType) {
    case typeIndexed: case typeIndexedRle: case typeTruecolor: case typeTruecolorRle: case typeGrayscale: case typeGrayscaleRle: break;
    default: return false;
    }
    if (h.colorMapType > 1 || h.width == 0 || h.height == 0) return false;
    return h.pixelDepth == 8 || h.pixelDepth == 15 || h.pixelDepth == 16 || h.pixelDepth == 24 || h.pixelDepth == 32;
}

std::shared_ptr<Image> failed(std::string* error, const char* why) { if (error) *error = why; return nullptr; }

} // namespace

bool isTgaData(const uint8_t* data, size_t size) {
    if (size < headerSize) return false;
    format_io::Reader r(data, size);
    return plausible(readHeader(r));
}

std::shared_ptr<Image> decodeTgaImage(const uint8_t* data, size_t size, std::string* error) {
    format_io::Reader r(data, size);
    const Header h = readHeader(r);
    if (!r.ok || !plausible(h)) return failed(error, "File is not a supported TGA image");
    if (h.pixelDepth == 15 || h.pixelDepth == 16) return failed(error, "15/16-bit TGA images are not supported; convert to 24-bit or 32-bit");
    if (h.width > maxImageSide || h.height > maxImageSide || (long long)h.width * h.height > Document::pixelBudget)
        return failed(error, "The TGA image is larger than an image may be.");
    r.skip(h.idLength);
    const bool indexed = h.imageType == typeIndexed || h.imageType == typeIndexedRle;
    const bool grayscale = h.imageType == typeGrayscale || h.imageType == typeGrayscaleRle;
    const bool rle = h.imageType >= typeIndexedRle;

    std::vector<std::array<uint8_t, 4>> colorMap;
    if (h.colorMapType == 1) {
        if (h.colorMapEntryBits != 24 && h.colorMapEntryBits != 32) return failed(error, "TGA color maps must be 24-bit or 32-bit");
        colorMap.resize(h.colorMapLength);
        for (auto& e : colorMap) {
            const uint8_t b = r.u8(), g = r.u8(), rd = r.u8();
            e = {rd, g, b, h.colorMapEntryBits == 32 ? r.u8() : uint8_t(255)};
        }
    }
    if (indexed && colorMap.empty()) return failed(error, "Indexed TGA image is missing its color map");
    if (indexed && h.pixelDepth != 8) return failed(error, "Indexed TGA images must be 8-bit");
    if (grayscale && h.pixelDepth != 8) return failed(error, "Grayscale TGA images must be 8-bit");
    if (!indexed && !grayscale && h.pixelDepth != 24 && h.pixelDepth != 32) return failed(error, "Truecolor TGA images must be 24-bit or 32-bit");
    if (!r.ok) return failed(error, "TGA data ended unexpectedly");

    const int width = h.width, height = h.height;
    const size_t bpp = h.pixelDepth / 8u, count = size_t(width) * size_t(height), total = count * bpp;
    std::vector<uint8_t> pixels;
    if (!rle) {
        if (r.remaining() < total) return failed(error, "TGA data ended unexpectedly");
        pixels.assign(data + r.pos, data + r.pos + total);
    } else {
        pixels.reserve(total);
        while (pixels.size() < total) {
            const uint8_t packet = r.u8();
            const size_t run = size_t(packet & 0x7f) + 1;
            if (packet & 0x80) {
                uint8_t value[4] = {};
                for (size_t i = 0; i < bpp; i++) value[i] = r.u8();
                for (size_t i = 0; i < run; i++) pixels.insert(pixels.end(), value, value + bpp);
            } else {
                if (!r.has(run * bpp)) { r.ok = false; break; }
                pixels.insert(pixels.end(), data + r.pos, data + r.pos + run * bpp);
                r.pos += run * bpp;
            }
            if (!r.ok) break;
        }
        if (!r.ok) return failed(error, "TGA data ended unexpectedly");
        pixels.resize(total);
    }

    const bool topDown = h.descriptor & 0x20, rightToLeft = h.descriptor & 0x10;
    const bool hasAlpha = h.pixelDepth == 32 || (indexed && h.colorMapEntryBits == 32);
    auto image = std::make_shared<Image>(width, height);
    bool anyAlpha = false;
    for (int y = 0; y < height; y++) {
        const int fy = topDown ? y : height - 1 - y;
        uint8_t* d = image->row(y);
        for (int x = 0; x < width; x++, d += 4) {
            const int fx = rightToLeft ? width - 1 - x : x;
            const uint8_t* s = pixels.data() + (size_t(fy) * size_t(width) + size_t(fx)) * bpp;
            if (indexed) {
                const size_t index = s[0], mapIndex = index >= h.colorMapFirst ? index - h.colorMapFirst : index;
                if (mapIndex >= colorMap.size()) return failed(error, "TGA pixel references a missing color map entry");
                const auto& e = colorMap[mapIndex];
                d[0] = e[0]; d[1] = e[1]; d[2] = e[2]; d[3] = hasAlpha ? e[3] : 255;
            } else if (grayscale) {
                d[0] = d[1] = d[2] = s[0]; d[3] = 255;
            } else {
                d[0] = s[2]; d[1] = s[1]; d[2] = s[0]; d[3] = bpp == 4 ? s[3] : 255;
            }
            anyAlpha = anyAlpha || (hasAlpha && d[3] != 0);
        }
    }
    if (hasAlpha && !anyAlpha)
        for (int y = 0; y < height; y++) { uint8_t* d = image->row(y); for (int x = 0; x < width; x++) d[x * 4 + 3] = 255; }
    premultiply(*image);
    return image;
}

std::shared_ptr<Image> readTgaImage(const std::string& path, std::string* error) {
    std::vector<uint8_t> bytes;
    if (!format_io::readFile(path, bytes, error)) return nullptr;
    return decodeTgaImage(bytes.data(), bytes.size(), error);
}

bool encodeTgaImage(const Image& source, std::vector<uint8_t>& out, std::string* error) {
    if (source.isEmpty()) { if (error) *error = "Cannot write an empty TGA image"; return false; }
    if (source.width() > 0xffff || source.height() > 0xffff) { if (error) *error = "TGA images cannot exceed 65535 pixels per side"; return false; }
    Image image = source;
    unpremultiply(image);
    format_io::Writer w;
    w.u8(0); w.u8(0); w.u8(typeTruecolorRle);
    w.u16(0); w.u16(0); w.u8(0);
    w.u16(0); w.u16(0);
    w.u16(uint32_t(image.width())); w.u16(uint32_t(image.height()));
    w.u8(32);
    w.u8(0x28);   // 8 alpha bits, top-left origin
    const int width = image.width();
    auto same = [&](const uint8_t* a, const uint8_t* b) { return a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3]; };
    auto put = [&](const uint8_t* p) { w.u8(p[2]); w.u8(p[1]); w.u8(p[0]); w.u8(p[3]); };
    // Runs never cross a row, as the specification recommends.
    for (int y = 0; y < image.height(); y++) {
        int x = 0;
        while (x < width) {
            const uint8_t* first = image.pixel(x, y);
            int run = 1;
            while (x + run < width && run < 128 && same(image.pixel(x + run, y), first)) run++;
            if (run >= 2) { w.u8(0x80u | unsigned(run - 1)); put(first); x += run; continue; }
            int literal = 1;
            while (x + literal < width && literal < 128) {
                if (x + literal + 1 < width && same(image.pixel(x + literal, y), image.pixel(x + literal + 1, y))) break;
                literal++;
            }
            w.u8(unsigned(literal - 1));
            for (int i = 0; i < literal; i++) put(image.pixel(x + i, y));
            x += literal;
        }
    }
    out = std::move(w.bytes);
    return true;
}

bool writeTgaImage(const std::string& path, const Image& image, std::string* error) {
    std::vector<uint8_t> bytes;
    return encodeTgaImage(image, bytes, error) && format_io::writeFile(path, bytes, error);
}

} // namespace compositor
