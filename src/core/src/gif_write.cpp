// Animated GIF export (gif.h): per-frame palettes (exact colours when they fit, else a median cut), transparency
// below half alpha, and an LZW encoder following Patchy's (MIT, src/formats/gif_document_io.cpp; see
// src/third_party/patchy_psd/README.md).
#include "compositor/gif.h"
#include "compositor/depth.h"
#include "compositor/render.h"
#include <algorithm>
#include <array>
#include <climits>
#include <fstream>
#include <unordered_map>

namespace compositor {

namespace {

/// LZW code stream packed LSB first into 255-byte sub-blocks (Patchy's SubBlockBitWriter).
class SubBlockWriter {
public:
    explicit SubBlockWriter(std::vector<uint8_t>& out) : out_(out) {}
    void code(uint32_t value, int width) {
        buffer_ |= value << bits_;
        bits_ += width;
        while (bits_ >= 8) { push(uint8_t(buffer_ & 0xFF)); buffer_ >>= 8; bits_ -= 8; }
    }
    void finish() {
        if (bits_ > 0) push(uint8_t(buffer_ & 0xFF));
        buffer_ = 0;
        bits_ = 0;
        flush();
        out_.push_back(0);
    }

private:
    void push(uint8_t byte) { block_.push_back(byte); if (block_.size() == 255) flush(); }
    void flush() {
        if (block_.empty()) return;
        out_.push_back(uint8_t(block_.size()));
        out_.insert(out_.end(), block_.begin(), block_.end());
        block_.clear();
    }
    std::vector<uint8_t>& out_;
    std::vector<uint8_t> block_;
    uint32_t buffer_ = 0;
    int bits_ = 0;
};

/// Patchy's encode_lzw: the code width grows when the code just assigned reaches 2^width, which is when the
/// decoder, one entry behind, grows its own; at 4096 entries the table clears.
void encodeLzw(std::vector<uint8_t>& out, const std::vector<uint8_t>& indexes, int minCodeSize) {
    SubBlockWriter bits(out);
    const uint32_t clear = 1u << minCodeSize, end = clear + 1;
    std::unordered_map<uint32_t, uint16_t> dictionary;
    dictionary.reserve(8192);
    uint32_t next = clear + 2;
    int width = minCodeSize + 1;
    bits.code(clear, width);
    if (indexes.empty()) { bits.code(end, width); bits.finish(); return; }
    uint32_t prefix = indexes[0];
    for (size_t i = 1; i < indexes.size(); i++) {
        const uint8_t symbol = indexes[i];
        const uint32_t key = prefix << 8 | symbol;
        auto found = dictionary.find(key);
        if (found != dictionary.end()) { prefix = found->second; continue; }
        bits.code(prefix, width);
        dictionary.emplace(key, uint16_t(next));
        if (next == (1u << width) && width < 12) width++;
        next++;
        if (next >= 4096) {
            bits.code(clear, width);
            dictionary.clear();
            next = clear + 2;
            width = minCodeSize + 1;
        }
        prefix = symbol;
    }
    bits.code(prefix, width);
    bits.code(end, width);
    bits.finish();
}

using Rgb = std::array<uint8_t, 3>;

struct Indexed {
    std::vector<Rgb> palette;   // at most 256 entries, the transparent one included
    std::vector<uint8_t> indexes;
    int transparent = -1;
};

/// The straight colour of a premultiplied pixel.
Rgb straight(const uint8_t* p) {
    const int a = p[3];
    if (a == 255) return {p[0], p[1], p[2]};
    auto un = [a](int c) { return uint8_t(std::min(255, (c * 255 + a / 2) / a)); };
    return {un(p[0]), un(p[1]), un(p[2])};
}

uint32_t key(const Rgb& c) { return uint32_t(c[0]) << 16 | uint32_t(c[1]) << 8 | c[2]; }
uint16_t bin(const Rgb& c) { return uint16_t((c[0] >> 3) << 10 | (c[1] >> 3) << 5 | (c[2] >> 3)); }
int channel(uint16_t b, int c) { return int(b >> (10 - 5 * c)) & 31; }

/// Median cut over a 15-bit histogram: the box with the most pixels times its widest side splits at its weighted
/// median until there are `colors` boxes; each box's mean becomes a palette entry.
std::vector<Rgb> medianCut(const std::vector<uint32_t>& counts, const std::vector<std::array<uint64_t, 3>>& sums, int colors) {
    struct Box { std::vector<uint16_t> bins; uint64_t pixels = 0; };
    Box all;
    for (uint32_t b = 0; b < 32768; b++) if (counts[b]) { all.bins.push_back(uint16_t(b)); all.pixels += counts[b]; }
    std::vector<Box> boxes;
    boxes.push_back(std::move(all));
    auto extent = [](const Box& box, int& axis) {
        int best = -1;
        for (int c = 0; c < 3; c++) {
            int lo = 31, hi = 0;
            for (uint16_t b : box.bins) { lo = std::min(lo, channel(b, c)); hi = std::max(hi, channel(b, c)); }
            if (hi - lo > best) { best = hi - lo; axis = c; }
        }
        return best;
    };
    while (int(boxes.size()) < colors) {
        int pick = -1, axis = 0;
        double score = 0;
        for (size_t i = 0; i < boxes.size(); i++) {
            if (boxes[i].bins.size() < 2) continue;
            int a = 0;
            const int e = extent(boxes[i], a);
            const double s = double(boxes[i].pixels) * (e + 1);
            if (e > 0 && s > score) { score = s; pick = int(i); axis = a; }
        }
        if (pick < 0) break;
        Box& box = boxes[size_t(pick)];
        std::sort(box.bins.begin(), box.bins.end(), [axis](uint16_t x, uint16_t y) { return channel(x, axis) < channel(y, axis); });
        uint64_t running = 0;
        size_t cut = 1;
        for (size_t i = 0; i + 1 < box.bins.size(); i++) {
            running += counts[box.bins[i]];
            cut = i + 1;
            if (running * 2 >= box.pixels) break;
        }
        Box upper;
        upper.bins.assign(box.bins.begin() + std::ptrdiff_t(cut), box.bins.end());
        box.bins.resize(cut);
        box.pixels = 0;
        for (uint16_t b : box.bins) box.pixels += counts[b];
        for (uint16_t b : upper.bins) upper.pixels += counts[b];
        boxes.push_back(std::move(upper));
    }
    std::vector<Rgb> palette;
    for (const Box& box : boxes) {
        if (!box.pixels) continue;
        std::array<uint64_t, 3> sum{};
        for (uint16_t b : box.bins) for (size_t c = 0; c < 3; c++) sum[c] += sums[b][c];
        palette.push_back({uint8_t(sum[0] / box.pixels), uint8_t(sum[1] / box.pixels), uint8_t(sum[2] / box.pixels)});
    }
    return palette;
}

Indexed quantise(const Image& image) {
    Indexed out;
    const int w = image.width(), h = image.height();
    out.indexes.resize(size_t(w) * size_t(h));
    bool clear = false, exactFits = true;
    std::unordered_map<uint32_t, uint8_t> exact;
    for (int y = 0; y < h && exactFits; y++) {
        const uint8_t* p = image.row(y);
        for (int x = 0; x < w; x++, p += 4) {
            if (p[3] < 128) { clear = true; continue; }
            const Rgb c = straight(p);
            if (exact.count(key(c))) continue;
            if (exact.size() >= 255) { exactFits = false; break; }
            exact.emplace(key(c), uint8_t(exact.size()));
            out.palette.push_back(c);
        }
    }
    if (exactFits) {
        const uint8_t none = uint8_t(out.palette.size());
        for (int y = 0; y < h; y++) {
            const uint8_t* p = image.row(y);
            uint8_t* d = out.indexes.data() + size_t(y) * size_t(w);
            for (int x = 0; x < w; x++, p += 4) d[x] = p[3] < 128 ? none : exact[key(straight(p))];
        }
    } else {
        clear = false;
        std::vector<uint32_t> counts(32768, 0);
        std::vector<std::array<uint64_t, 3>> sums(32768, {0, 0, 0});
        for (int y = 0; y < h; y++) {
            const uint8_t* p = image.row(y);
            for (int x = 0; x < w; x++, p += 4) {
                if (p[3] < 128) { clear = true; continue; }
                const Rgb c = straight(p);
                const uint16_t b = bin(c);
                counts[b]++;
                for (size_t k = 0; k < 3; k++) sums[b][k] += c[k];
            }
        }
        out.palette = medianCut(counts, sums, 255);
        // Each occupied bin maps to the palette colour nearest its centre.
        std::vector<uint8_t> lut(32768, 0);
        for (uint32_t b = 0; b < 32768; b++) {
            if (!counts[b]) continue;
            const int r = int((b >> 10) & 31) * 8 + 4, g = int((b >> 5) & 31) * 8 + 4, bl = int(b & 31) * 8 + 4;
            int best = 0, bestDistance = INT_MAX;
            for (size_t i = 0; i < out.palette.size(); i++) {
                const int dr = r - out.palette[i][0], dg = g - out.palette[i][1], db = bl - out.palette[i][2];
                const int d = 2 * dr * dr + 4 * dg * dg + 3 * db * db;
                if (d < bestDistance) { bestDistance = d; best = int(i); }
            }
            lut[b] = uint8_t(best);
        }
        const uint8_t none = uint8_t(out.palette.size());
        for (int y = 0; y < h; y++) {
            const uint8_t* p = image.row(y);
            uint8_t* d = out.indexes.data() + size_t(y) * size_t(w);
            for (int x = 0; x < w; x++, p += 4) d[x] = p[3] < 128 ? none : lut[bin(straight(p))];
        }
    }
    if (clear) { out.transparent = int(out.palette.size()); out.palette.push_back({0, 0, 0}); }
    if (out.palette.empty()) out.palette.push_back({0, 0, 0});
    return out;
}

int tableBits(size_t entries) {
    int bits = 1;
    while ((size_t(1) << bits) < entries) bits++;
    return bits;
}

void putU16(std::vector<uint8_t>& out, int v) { out.push_back(uint8_t(v & 0xFF)); out.push_back(uint8_t((v >> 8) & 0xFF)); }

} // namespace

std::vector<uint8_t> encodeGif(const std::vector<GifEncodeFrame>& frames, int loopCount, std::string* error) {
    auto failed = [error](const char* why) { if (error) *error = why; return std::vector<uint8_t>{}; };
    if (frames.empty() || !frames[0].image) return failed("There are no frames to write.");
    const int width = frames[0].image->width(), height = frames[0].image->height();
    if (width < 1 || height < 1 || width > 0xFFFF || height > 0xFFFF) return failed("A GIF is at most 65535 pixels on a side.");
    for (const GifEncodeFrame& f : frames)
        if (!f.image || f.image->width() != width || f.image->height() != height) return failed("Every frame of a GIF must be the same size.");
    std::vector<uint8_t> out = {'G', 'I', 'F', '8', '9', 'a'};
    putU16(out, width);
    putU16(out, height);
    out.push_back(0x70);   // no global table (each frame has its own), 8 bits of colour resolution
    out.push_back(0);
    out.push_back(0);
    if (frames.size() > 1 && loopCount != 1) {
        static const char app[] = "NETSCAPE2.0";
        out.push_back(0x21); out.push_back(0xFF); out.push_back(11);
        out.insert(out.end(), app, app + 11);
        out.push_back(3); out.push_back(1);
        putU16(out, loopCount <= 0 ? 0 : std::min(65535, loopCount - 1));
        out.push_back(0);
    }
    for (const GifEncodeFrame& f : frames) {
        const Indexed indexed = quantise(*f.image);
        const bool clear = indexed.transparent >= 0;
        out.push_back(0x21); out.push_back(0xF9); out.push_back(4);
        out.push_back(uint8_t(0x08 | (clear ? 1 : 0)));   // disposal 2: every frame is a whole picture
        putU16(out, std::clamp((f.delayMs + 5) / 10, 0, 65535));
        out.push_back(uint8_t(clear ? indexed.transparent : 0));
        out.push_back(0);
        out.push_back(0x2C);
        putU16(out, 0); putU16(out, 0); putU16(out, width); putU16(out, height);
        const int bits = tableBits(indexed.palette.size());
        out.push_back(uint8_t(0x80 | (bits - 1)));
        for (size_t i = 0; i < (size_t(1) << bits); i++) {
            const Rgb c = i < indexed.palette.size() ? indexed.palette[i] : Rgb{0, 0, 0};
            out.insert(out.end(), c.begin(), c.end());
        }
        const int minCodeSize = std::max(2, bits);
        out.push_back(uint8_t(minCodeSize));
        encodeLzw(out, indexed.indexes, minCodeSize);
    }
    out.push_back(0x3B);
    return out;
}

std::vector<uint8_t> encodeDocumentGif(const Document& document, std::string* error) {
    std::vector<std::shared_ptr<Image>> images;
    std::vector<GifEncodeFrame> frames;
    const Animation& a = document.animation;
    // A 16-bit document's frames are rendered at 16 bits and dithered down to 8 before the palette is chosen, as the
    // other 8-bit formats take it (GIF holds 8 bits a channel at most).
    auto flat = [](const Document& d) { return d.sampleType == SampleType::U16 ? ditherToEightBit(*renderFlattened16(d)) : renderFlattened(d); };
    if (a.frames.empty()) {
        images.push_back(flat(document));
        frames.push_back({images.back().get(), 0});
    } else {
        for (size_t i = 0; i < a.frames.size(); i++) {
            Document copy = document;
            applyFrame(copy, a.frames[i]);
            images.push_back(flat(copy));
            frames.push_back({images.back().get(), a.frames[i].delayMs});
        }
    }
    return encodeGif(frames, a.frames.empty() ? 1 : a.loopCount, error);
}

bool writeDocumentGif(const std::string& path, const Document& document, std::string* error) {
    const std::vector<uint8_t> bytes = encodeDocumentGif(document, error);
    if (bytes.empty()) return false;
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
    if (!out) { if (error) *error = "The file could not be written."; return false; }
    return true;
}

} // namespace compositor
