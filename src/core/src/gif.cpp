// Animated GIF import: a GIF87a/89a parser, an LZW decoder and the frame compositing (disposal methods 0-3,
// transparency, interlacing, local colour tables), written for NekoPhoto from the GIF89a specification.
#include "compositor/gif.h"
#include "format_io.h"
#include <algorithm>
#include <array>
#include <cstring>

namespace compositor {

namespace {

using Palette = std::vector<std::array<uint8_t, 3>>;

struct Frame {
    int x = 0, y = 0, width = 0, height = 0;
    bool interlaced = false;
    Palette local;
    int transparent = -1, disposal = 0, delay = 0;   // delay in hundredths of a second
    int minCodeSize = 0;
    std::vector<uint8_t> data;
};

struct Gif {
    int width = 0, height = 0;
    Palette global;
    std::vector<Frame> frames;
    bool truncated = false;   // the stream ended or broke before its trailer
};

Palette readPalette(format_io::Reader& r, int bits) {
    Palette p(size_t(1) << bits);
    for (auto& c : p) { c[0] = r.u8(); c[1] = r.u8(); c[2] = r.u8(); }
    return p;
}

/// Sub-blocks up to the zero terminator, appended to `out` when given.
void readSubBlocks(format_io::Reader& r, std::vector<uint8_t>* out) {
    for (;;) {
        const uint8_t n = r.u8();
        if (!r.ok || n == 0) return;
        if (!r.has(n)) { r.ok = false; return; }
        if (out) out->insert(out->end(), r.data + r.pos, r.data + r.pos + n);
        r.pos += n;
    }
}

bool parse(const std::vector<uint8_t>& bytes, Gif& gif, bool withData) {
    if (bytes.size() < 13 || (std::memcmp(bytes.data(), "GIF87a", 6) != 0 && std::memcmp(bytes.data(), "GIF89a", 6) != 0)) return false;
    format_io::Reader r(bytes.data(), bytes.size());
    r.skip(6);
    gif.width = r.u16();
    gif.height = r.u16();
    const uint8_t packed = r.u8();
    r.skip(2);
    if (packed & 0x80) gif.global = readPalette(r, (packed & 7) + 1);
    if (!r.ok) return false;
    int transparent = -1, disposal = 0, delay = 0;
    for (;;) {
        const uint8_t block = r.u8();
        if (!r.ok) { gif.truncated = true; break; }
        if (block == 0x3B) break;
        if (block == 0x21) {
            const uint8_t label = r.u8();
            std::vector<uint8_t> body;
            readSubBlocks(r, &body);
            if (label == 0xF9 && body.size() >= 4) {
                disposal = (body[0] >> 2) & 7;
                delay = body[1] | body[2] << 8;
                transparent = (body[0] & 1) ? body[3] : -1;
            }
        } else if (block == 0x2C) {
            Frame f;
            f.x = r.u16(); f.y = r.u16(); f.width = r.u16(); f.height = r.u16();
            const uint8_t fp = r.u8();
            f.interlaced = fp & 0x40;
            if (fp & 0x80) f.local = readPalette(r, (fp & 7) + 1);
            f.minCodeSize = r.u8();
            readSubBlocks(r, withData ? &f.data : nullptr);
            if (!r.ok) { gif.truncated = true; if (!withData) break; }
            f.transparent = transparent; f.disposal = disposal; f.delay = delay;
            transparent = -1; disposal = 0; delay = 0;
            gif.frames.push_back(std::move(f));
            if (!r.ok) break;
        } else {
            gif.truncated = true;   // not a block GIF defines: stop at what was read
            break;
        }
    }
    return true;
}

/// Decodes the LZW stream into palette indices; stops at `count` (a short stream leaves the rest unset).
size_t decodeLzw(const std::vector<uint8_t>& data, int minCodeSize, size_t count, std::vector<uint8_t>& out) {
    out.assign(count, 0);
    if (minCodeSize < 2 || minCodeSize > 11) return 0;
    const int clear = 1 << minCodeSize, end = clear + 1;
    std::vector<uint16_t> prefix(4096);
    std::vector<uint8_t> suffix(4096), stack;
    stack.reserve(4096);
    for (int i = 0; i < clear; i++) suffix[size_t(i)] = uint8_t(i);
    int codeSize = minCodeSize + 1, next = clear + 2, old = -1;
    uint8_t first = 0;
    size_t written = 0, bitPos = 0;
    const size_t totalBits = data.size() * 8;
    while (written < count) {
        if (bitPos + size_t(codeSize) > totalBits) break;
        int code = 0;
        for (int i = 0; i < codeSize; i++, bitPos++) code |= ((data[bitPos >> 3] >> (bitPos & 7)) & 1) << i;
        if (code == clear) { codeSize = minCodeSize + 1; next = clear + 2; old = -1; continue; }
        if (code == end) break;
        if (old < 0) {
            if (code >= clear) break;
            out[written++] = uint8_t(code);
            first = uint8_t(code);
            old = code;
            continue;
        }
        const int in = code;
        stack.clear();
        if (code >= next) {
            if (code > next) break;   // corrupt
            stack.push_back(first);
            code = old;
        }
        while (code > end) { stack.push_back(suffix[size_t(code)]); code = prefix[size_t(code)]; }
        if (code >= clear) break;
        first = uint8_t(code);
        stack.push_back(first);
        for (size_t i = stack.size(); i-- > 0 && written < count;) out[written++] = stack[i];
        if (next < 4096) {
            prefix[size_t(next)] = uint16_t(old);
            suffix[size_t(next)] = first;
            next++;
            if (next == (1 << codeSize) && codeSize < 12) codeSize++;
        }
        old = in;
    }
    return written;
}

} // namespace

int gifFrameCount(const std::vector<uint8_t>& bytes) {
    Gif gif;
    return parse(bytes, gif, false) ? int(gif.frames.size()) : 0;
}

int gifFrameCount(const std::string& path) {
    std::vector<uint8_t> bytes;
    return format_io::readFile(path, bytes, nullptr) ? gifFrameCount(bytes) : 0;
}

std::optional<PsdImport> importGifBytes(const std::vector<uint8_t>& bytes, std::string* error) {
    Gif gif;
    if (!parse(bytes, gif, true)) { if (error) *error = "File is not a GIF image"; return std::nullopt; }
    if (gif.frames.empty()) { if (error) *error = "The GIF holds no frames."; return std::nullopt; }
    int width = gif.width, height = gif.height;
    if (width <= 0 || height <= 0) { width = gif.frames[0].x + gif.frames[0].width; height = gif.frames[0].y + gif.frames[0].height; }
    if (!Document::validDimension(width) || !Document::validDimension(height) || (long long)width * height > Document::pixelBudget) {
        if (error) *error = "The GIF is larger than an image may be.";
        return std::nullopt;
    }
    PsdImport result;
    result.document = Document(width, height);
    const long long framePixels = (long long)width * height;
    const size_t limit = size_t(std::min<long long>(Document::maxLayers, Document::projectPixelBudget / framePixels));
    if (gif.frames.size() > limit) {
        result.notes.push_back("Only the first " + std::to_string(limit) + " of " + std::to_string(gif.frames.size()) + " frames fit in a document.");
        gif.frames.resize(limit);
    }
    Image canvas(width, height);   // premultiplied; a GIF pixel is opaque or clear, so it is also straight
    std::vector<uint8_t> indices;
    int damaged = 0;
    for (size_t n = 0; n < gif.frames.size(); n++) {
        const Frame& f = gif.frames[n];
        const Palette& palette = f.local.empty() ? gif.global : f.local;
        std::optional<Image> saved;
        if (f.disposal == 3) saved = canvas;
        const size_t count = size_t(f.width) * size_t(f.height);
        const size_t decoded = decodeLzw(f.data, f.minCodeSize, count, indices);
        if (decoded < count) damaged++;
        // Row order within the frame: interlaced frames arrive in four passes.
        std::vector<int> rows;
        rows.reserve(size_t(f.height));
        if (f.interlaced) {
            static const int start[4] = {0, 4, 2, 1}, step[4] = {8, 8, 4, 2};
            for (int pass = 0; pass < 4; pass++) for (int y = start[pass]; y < f.height; y += step[pass]) rows.push_back(y);
        } else {
            for (int y = 0; y < f.height; y++) rows.push_back(y);
        }
        for (size_t i = 0; i < rows.size(); i++) {
            const int cy = f.y + rows[i];
            if (cy >= height) continue;
            for (int x = 0; x < f.width; x++) {
                const size_t k = i * size_t(f.width) + size_t(x);
                const int cx = f.x + x;
                if (k >= decoded || cx >= width) continue;
                const int index = indices[k];
                if (index == f.transparent || size_t(index) >= palette.size()) continue;
                uint8_t* d = canvas.pixel(cx, cy);
                d[0] = palette[size_t(index)][0]; d[1] = palette[size_t(index)][1]; d[2] = palette[size_t(index)][2]; d[3] = 255;
            }
        }
        const int ms = f.delay * 10;
        const std::string name = "Frame " + std::to_string(n + 1) + " (" + std::to_string(ms) + " ms)";
        Layer layer(Asset::make(std::make_shared<Image>(canvas), name), Point(0, 0));
        layer.visible = n == 0;
        result.document.layers.push_back(std::move(layer));
        if (f.disposal == 2) {
            for (int y = std::max(0, f.y); y < std::min(height, f.y + f.height); y++)
                for (int x = std::max(0, f.x); x < std::min(width, f.x + f.width); x++) std::memset(canvas.pixel(x, y), 0, 4);
        } else if (f.disposal == 3 && saved) {
            canvas = std::move(*saved);
        }
    }
    if (damaged) result.notes.push_back(std::to_string(damaged) + " frame(s) ended early; their missing pixels show the frame before.");
    else if (gif.truncated) result.notes.push_back("The file ends before its trailer; the frames read up to there were kept.");
    return result;
}

std::optional<PsdImport> importGif(const std::string& path, std::string* error) {
    std::vector<uint8_t> bytes;
    if (!format_io::readFile(path, bytes, error)) return std::nullopt;
    return importGifBytes(bytes, error);
}

} // namespace compositor
