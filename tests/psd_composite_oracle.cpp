// Photoshop as the oracle: every PSD and PSB of a corpus of Photoshop's files is rendered by NekoPhoto from its
// layers and compared with what Photoshop shows for it. Three corpora, each found through its environment variable
// (none is part of this repository; never commit their files):
//
//   patchy       $PATCHY_FIXTURES, else Patchy's test-fixtures/psd beside the main checkout (a worktree finds it too)
//   photocraft   $PHOTOSHOP_CORPUS: a checkout of https://github.com/storytold/photocraft-corpus (258 files written by
//                Photoshop 27.10, every mode and depth), verified file by file against its own SHA256SUMS
//   psd-tools    $PSD_TOOLS_CORPUS: a checkout of https://github.com/psd-tools/psd-tools (tests/psd_files)
//
//   psd_composite_oracle [--corpus patchy|photocraft|psd-tools]   one corpus against its expectations (default patchy)
//   psd_composite_oracle DIR...                                    any folders, reported only
//
// A corpus that is not there is SKIPPED, loudly, with exit code 77 (ctest's SKIP_RETURN_CODE), never passed.
// Folders are searched recursively; names are relative to the corpus root.
//
// The reference is Photoshop's own flatten where Patchy keeps one beside the file (a 24-bit .bmp of the same name,
// which Patchy's docs name as the render reference: the merged image stored inside a file can be stale when
// Photoshop saved it headless), else the merged image Photoshop stored in the file ("Maximize Compatibility"). The
// merged image is compared in the document's own space, so no colour conversion sits between the two: CMYK and Lab
// files natively (renderNative against the merged image as stored, every ink or L/a/b channel and alpha), 16-bit files
// at 16 bits, 32-bit files in linear float (times 255, so a "level" there is a linear one), 8-bit RGB and grey in
// premultiplied RGBA. Each line says which: [bmp], [rgb8], [rgb16], [f32], [cmyk8], [cmyk16], [lab8] or [lab16].
//
// For each file: the largest difference of any channel in 8-bit levels, the share of pixels with a channel more than
// 2 levels off, and the mean difference per channel. A file passes when no more than 1% of its pixels are more than 2
// levels off and the mean is under 1 level. A file with a visible Dissolve layer is judged by PhotoCraft's dissolve
// rule instead (Photoshop's dissolve pattern is not public): outside the Dissolve layers the usual rule, inside them
// each 16 x 16 block's mean within 0.1 (25.5 levels) per channel; its line says "dissolve". Files with neither
// reference are listed, not compared.
//
// Each corpus has an expectations file in tests/ listing every file's status and numbers when it was last written.
// A file that passed and fails now, a file no longer compared, or a failing file whose mean grew by more than a level
// fails the test; a file that passes now and did not is reported as an improvement. After a change, rewrite it:
//
//   NEKOPHOTO_UPDATE_PSD_ORACLE=1 build/tests/psd_composite_oracle --corpus photocraft
//
// The patchy corpus also rewrites tests/patchy-manifest.txt (the commit and each fixture's SHA-256) and psd-tools
// tests/psd-tools-manifest.txt; a checkout that differs is reported, not failed. photocraft-corpus pins itself: a
// file that differs from its SHA256SUMS, or is missing, fails the run.
//
// PSD_ORACLE_DUMP=<dir> writes, for every compared file, NekoPhoto's render, the reference and a difference map
// (each pixel's largest channel difference, times 8) in 8-bit sRGB as PNGs: <name>-ours.png, -ps.png and -diff.png.
#include "nekophoto/png.h"
#include "nekophoto/psd.h"
#include "nekophoto/psd_carry.h"
#include "nekophoto/render.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#ifndef PATCHY_FIXTURES
#define PATCHY_FIXTURES "../Patchy/test-fixtures/psd"
#endif
#ifndef TESTS_DIR
#define TESTS_DIR "tests"
#endif

namespace fs = std::filesystem;
using namespace nekophoto;

namespace {

constexpr int kSkipped = 77;             // ctest SKIP_RETURN_CODE
constexpr double kMaxShareOver2 = 1.0;   // percent of pixels more than 2 levels off
constexpr double kMaxMean = 1.0;         // levels, per channel
constexpr double kWorseMean = 1.0;       // a failing file's mean may grow by this much before it counts as a regression
constexpr int kDissolveBlock = 16;       // PhotoCraft's dissolve rule (crates/io/tests/corpus.rs, 88ffd89)
constexpr double kDissolveMean = 0.1 * 255;

// ---- SHA-256 (FIPS 180-4), for the fixture manifests -------------------------------------------------------

std::string sha256(const std::vector<uint8_t>& data) {
    static const uint32_t k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be,
        0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa,
        0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85,
        0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
        0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f,
        0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    auto rotr = [](uint32_t x, int n) { return (x >> n) | (x << (32 - n)); };
    auto compress = [&](const uint8_t* block) {
        uint32_t w[64];
        for (int i = 0; i < 16; i++)
            w[i] = uint32_t(block[i * 4]) << 24 | uint32_t(block[i * 4 + 1]) << 16 | uint32_t(block[i * 4 + 2]) << 8 | uint32_t(block[i * 4 + 3]);
        for (int i = 16; i < 64; i++) {
            const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; i++) {
            const uint32_t t1 = hh + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
            const uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    };
    // The whole blocks in place; only the tail is copied to be padded.
    const size_t whole = data.size() / 64 * 64;
    for (size_t at = 0; at < whole; at += 64) compress(data.data() + at);
    std::vector<uint8_t> tail(data.begin() + std::ptrdiff_t(whole), data.end());
    const uint64_t bits = uint64_t(data.size()) * 8;
    tail.push_back(0x80);
    while (tail.size() % 64 != 56) tail.push_back(0);
    for (int i = 7; i >= 0; i--) tail.push_back(uint8_t(bits >> (i * 8)));
    for (size_t at = 0; at < tail.size(); at += 64) compress(tail.data() + at);
    char out[65];
    for (int i = 0; i < 8; i++) std::snprintf(out + i * 8, 9, "%08x", h[i]);
    return std::string(out, 64);
}

std::vector<uint8_t> readFile(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), {});
}

/// Lines of a text file without their line endings (a Windows checkout may have made them CRLF).
std::vector<std::string> readLines(const fs::path& path) {
    std::vector<std::string> lines;
    std::ifstream in(path, std::ios::binary);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(line);
    }
    return lines;
}

std::string trim(std::string s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    size_t i = 0;
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) i++;
    return s.substr(i);
}

std::vector<std::string> words(const std::string& line) {
    std::istringstream in(line);
    std::vector<std::string> out;
    for (std::string w; in >> w;) out.push_back(w);
    return out;
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

/// The commit a git checkout holding `folder` is at, read from its .git folder (no git needed); "unknown" otherwise.
std::string gitCommit(const fs::path& folder) {
    std::error_code ec;
    fs::path root = fs::weakly_canonical(folder, ec);
    for (int up = 0; up < 4 && !root.empty(); up++, root = root.parent_path()) {
        const fs::path git = root / ".git";
        if (!fs::is_directory(git, ec)) continue;
        const auto head = readLines(git / "HEAD");
        if (head.empty()) return "unknown";
        if (head[0].rfind("ref: ", 0) != 0) return trim(head[0]);
        const std::string ref = trim(head[0].substr(5));
        const auto loose = readLines(git / ref);
        if (!loose.empty()) return trim(loose[0]);
        for (const std::string& line : readLines(git / "packed-refs"))
            if (line.size() > 41 && line.compare(41, std::string::npos, ref) == 0) return line.substr(0, 40);
        return "unknown";
    }
    return "unknown";
}

struct Result {
    std::string name;        // relative to the corpus root, with forward slashes
    std::string reference;   // "bmp", or the merged image's space: "rgb8", "rgb16", "f32", "cmyk8", "cmyk16", "lab8", "lab16"
    bool compared = false;   // false: no reference to compare with (the reason in `why`)
    bool dissolve = false;   // judged by the dissolve rule
    std::string why;
    double maxError = 0;     // levels
    double shareOver2 = 0;   // percent of pixels
    double mean = 0;         // levels per channel
    bool pass = false;
};

/// Whether the file carries Photoshop's version info resource (1057) naming Photoshop as its writer.
bool writtenByPhotoshop(const Document& document) {
    if (!document.psdCarry) return false;
    for (const auto& resource : document.psdCarry->resources) {
        if (resource.id != 1057 || resource.data.size() < 9) continue;
        // u32 version, u8 hasRealMergedData, then the writer's name as a Unicode string (u32 length, UTF-16BE).
        const auto& d = resource.data;
        const size_t length = size_t(d[5]) << 24 | size_t(d[6]) << 16 | size_t(d[7]) << 8 | d[8];
        std::string writer;
        for (size_t i = 0; i < length && 9 + i * 2 + 1 < d.size(); i++) writer.push_back(char(d[9 + i * 2 + 1]));
        return writer.find("Photoshop") != std::string::npos;
    }
    return false;
}

bool opaque(const Image& image) {
    for (int y = 0; y < image.height(); y++)
        for (int x = 0; x < image.width(); x++)
            if (image.row(y)[x * 4 + 3] != 255) return false;
    return true;
}

/// A 24- or 32-bit uncompressed Windows bitmap as opaque RGB rows (top-down, 3 bytes a pixel); empty when it is not one.
struct Flatten {
    int width = 0, height = 0;
    std::vector<uint8_t> rgb;
};

Flatten readBmp(const fs::path& path) {
    Flatten out;
    const std::vector<uint8_t> d = readFile(path);
    auto u16 = [&](size_t at) { return unsigned(d[at]) | unsigned(d[at + 1]) << 8; };
    auto u32 = [&](size_t at) { return uint32_t(d[at]) | uint32_t(d[at + 1]) << 8 | uint32_t(d[at + 2]) << 16 | uint32_t(d[at + 3]) << 24; };
    if (d.size() < 54 || d[0] != 'B' || d[1] != 'M') return out;
    const uint32_t offset = u32(10);
    const int32_t width = int32_t(u32(18)), height = int32_t(u32(22));
    const unsigned bits = u16(28);
    const uint32_t compression = u32(30);
    if (width <= 0 || height == 0 || width > 65535 || std::abs(height) > 65535 || (bits != 24 && bits != 32)) return out;
    if (compression != 0 && !(compression == 3 && bits == 32)) return out;
    const int h = std::abs(height);
    const size_t bytesPerPixel = bits / 8, stride = (size_t(width) * bytesPerPixel + 3) / 4 * 4;
    if (offset > d.size() || d.size() - offset < stride * size_t(h)) return out;
    out.width = width;
    out.height = h;
    out.rgb.resize(size_t(width) * size_t(h) * 3);
    for (int y = 0; y < h; y++) {
        const uint8_t* row = d.data() + offset + stride * size_t(height > 0 ? h - 1 - y : y);   // positive: bottom-up
        for (int x = 0; x < width; x++) {
            uint8_t* q = out.rgb.data() + (size_t(y) * size_t(width) + size_t(x)) * 3;
            q[0] = row[size_t(x) * bytesPerPixel + 2];
            q[1] = row[size_t(x) * bytesPerPixel + 1];
            q[2] = row[size_t(x) * bytesPerPixel];
        }
    }
    return out;
}

/// Photoshop's flatten beside `psd`: the same name as a .bmp; for Patchy's two that are named otherwise, the
/// "-roundtrip" file's "-render" flatten and a PSB's "<name>-psb" one.
fs::path flattenFor(const fs::path& psd) {
    std::string stem = psd.stem().string();
    const std::string ext = lower(psd.extension().string());
    std::vector<std::string> names{stem};
    const std::string roundtrip = "-roundtrip";
    if (stem.size() > roundtrip.size() && stem.compare(stem.size() - roundtrip.size(), roundtrip.size(), roundtrip) == 0)
        names.push_back(stem.substr(0, stem.size() - roundtrip.size()) + "-render");
    if (ext.size() > 1) names.push_back(stem + "-" + ext.substr(1));
    std::error_code ec;
    for (const std::string& name : names) {
        const fs::path candidate = psd.parent_path() / (name + ".bmp");
        if (fs::is_regular_file(candidate, ec)) return candidate;
    }
    return {};
}

/// Document rectangles of the visible Dissolve layers (the canvas for one turned or flipped).
struct Box { int x0, y0, x1, y1; };
std::vector<Box> dissolveRegions(const Document& document) {
    std::vector<Box> out;
    for (const Layer& layer : document.layers) {
        if (!layer.visible || layer.blendMode != BlendMode::Dissolve) continue;
        const LayerTransform& t = layer.transform;
        if (t.rotation != 0 || t.flipX || t.flipY) { out.push_back({0, 0, document.width, document.height}); continue; }
        const int x0 = int(std::floor(t.origin.x)), y0 = int(std::floor(t.origin.y));
        out.push_back({x0, y0, int(std::ceil(t.origin.x + t.size.width)), int(std::ceil(t.origin.y + t.size.height))});
    }
    return out;
}

/// The differences of `ours` against `ps` over a `w` x `h` image of `channels` samples, both read as 8-bit levels
/// (`ours(x, y, c)`, `ps(x, y, c)` return doubles). With dissolve regions, the dissolve rule (see the top).
template <class Ours, class Ps>
void measure(Result& r, int w, int h, int channels, const Ours& ours, const Ps& ps, const std::vector<Box>& dissolve) {
    auto inside = [&](int x, int y) {
        for (const Box& b : dissolve) if (x >= b.x0 && x < b.x1 && y >= b.y0 && y < b.y1) return true;
        return false;
    };
    uint64_t over = 0, overOutside = 0, outside = 0;
    double sum = 0, sumOutside = 0, worst = 0;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            double pixelWorst = 0, pixelSum = 0;
            for (int c = 0; c < channels; c++) {
                const double d = std::abs(ours(x, y, c) - ps(x, y, c));
                pixelSum += d;
                pixelWorst = std::max(pixelWorst, d);
            }
            sum += pixelSum;
            worst = std::max(worst, pixelWorst);
            // Above 2 levels: a 16-bit or float difference is compared after rounding to a hundredth of a level.
            const bool off = std::round(pixelWorst * 100) > 200;
            over += off;
            if (!dissolve.empty() && !inside(x, y)) { outside++; overOutside += off; sumOutside += pixelSum; }
        }
    const double pixels = double(w) * h;
    r.maxError = worst;
    r.shareOver2 = pixels > 0 ? 100.0 * double(over) / pixels : 0;
    r.mean = pixels > 0 ? sum / (pixels * channels) : 0;
    r.pass = r.shareOver2 <= kMaxShareOver2 && r.mean < kMaxMean;
    if (dissolve.empty()) return;
    r.dissolve = true;
    bool good = outside == 0 || (100.0 * double(overOutside) / double(outside) <= kMaxShareOver2 && sumOutside / (double(outside) * channels) < kMaxMean);
    for (int by = 0; by < h && good; by += kDissolveBlock)
        for (int bx = 0; bx < w && good; bx += kDissolveBlock) {
            const int x1 = std::min(w, bx + kDissolveBlock), y1 = std::min(h, by + kDissolveBlock);
            for (int c = 0; c < channels && good; c++) {
                double a = 0, b = 0;
                for (int y = by; y < y1; y++)
                    for (int x = bx; x < x1; x++) { a += ours(x, y, c); b += ps(x, y, c); }
                good = std::abs(a - b) / double((x1 - bx) * (y1 - by)) <= kDissolveMean;
            }
        }
    r.pass = good;
}

/// PSD_ORACLE_DUMP: NekoPhoto's render and the reference in 8-bit sRGB (`channels` 3: RGB, ours matted on white;
/// 4: premultiplied RGBA), and the difference map.
void dump(const std::string& name, const Image& ours, const uint8_t* ps, int channels) {
    const char* dir = std::getenv("PSD_ORACLE_DUMP");
    if (!dir || !*dir) return;
    const int w = ours.width(), h = ours.height();
    Image mine(w, h), theirs(w, h), diff(w, h);
    for (int y = 0; y < h; y++) {
        const uint8_t* b = ours.row(y);
        const uint8_t* a = ps + size_t(y) * size_t(w) * size_t(channels);
        for (int x = 0; x < w; x++, a += channels, b += 4) {
            int worst = 0;
            for (int c = 0; c < 4; c++) {
                const int m = channels == 3 ? (c < 3 ? int(b[c]) + 255 - int(b[3]) : 255) : int(b[c]);
                const int t = c < channels ? int(a[c]) : 255;
                mine.row(y)[x * 4 + c] = uint8_t(m);
                theirs.row(y)[x * 4 + c] = uint8_t(t);
                if (c < channels) worst = std::max(worst, std::abs(m - t));
            }
            const uint8_t v = uint8_t(std::min(255, worst * 8));
            uint8_t* d = diff.row(y) + x * 4;
            d[0] = d[1] = d[2] = v;
            d[3] = 255;
        }
    }
    std::string flat = name;
    std::replace(flat.begin(), flat.end(), '/', '_');
    const std::string stem = std::string(dir) + "/" + fs::path(flat).stem().string();
    writePngImage(stem + "-ours.png", mine);
    writePngImage(stem + "-ps.png", theirs);
    writePngImage(stem + "-diff.png", diff);
}

void dumpMerged(const std::string& name, const Image* ours, const Image& ps) {
    if (!ours || ours->width() != ps.width() || ours->height() != ps.height()) return;
    std::vector<uint8_t> rows(size_t(ps.width()) * size_t(ps.height()) * 4);
    for (int y = 0; y < ps.height(); y++) std::memcpy(rows.data() + size_t(y) * size_t(ps.width()) * 4, ps.row(y), size_t(ps.width()) * 4);
    dump(name, *ours, rows.data(), 4);
}

template <class A>
bool sameShape(const A& a, const A& b) {
    return a.width() == b.width() && a.height() == b.height() && a.channels() == b.channels();
}

/// Two buffers of one sample type compared with `scale` levels per sample unit.
template <class A>
void measureImages(Result& r, const A& ours, const A& ps, double scale, const std::vector<Box>& dissolve) {
    auto at = [scale](const A& image) { return [&image, scale](int x, int y, int c) { return double(image.pixel(x, y)[c]) * scale; }; };
    measure(r, ps.width(), ps.height(), ps.channels(), at(ours), at(ps), dissolve);
}

void sizeDiffers(Result& r, const std::string& why) {
    r.why = why;
    r.maxError = 255; r.shareOver2 = 100; r.mean = 255;
}

Result compare(const fs::path& path, const std::string& name) {
    Result r;
    r.name = name;
    std::string error;
    auto imported = importPsd(path.string(), &error);
    if (!imported) { r.why = "does not open: " + error; return r; }
    const Document& document = imported->document;
    if (const fs::path bmp = flattenFor(path); !bmp.empty()) {
        // Photoshop's own flatten: what Photoshop shows, in sRGB.
        const Flatten ps = readBmp(bmp);
        if (ps.width > 0) {
            r.reference = "bmp";
            r.compared = true;
            auto ours = renderFlattened(document);
            if (!ours || ours->width() != ps.width || ours->height() != ps.height) { sizeDiffers(r, "size differs from " + bmp.filename().string()); return r; }
            const Image& o = *ours;
            const int w = ps.width;
            measure(r, ps.width, ps.height, 3, [&](int x, int y, int c) { const uint8_t* p = o.pixel(x, y); return double(int(p[c]) + 255 - int(p[3])); },
                    [&](int x, int y, int c) { return double(ps.rgb[(size_t(y) * size_t(w) + size_t(x)) * 3 + size_t(c)]); }, {});
            dump(r.name, o, ps.rgb.data(), 3);
            return r;
        }
    }
    if (!imported->composite) { r.why = "no merged image"; return r; }
    if (!imported->realComposite) { r.why = "saved without Maximize Compatibility"; return r; }
    const Image& merged = *imported->composite;
    auto ours8 = renderFlattened(document);
    if (!writtenByPhotoshop(document) && ours8 && ours8->width() == merged.width() && ours8->height() == merged.height() && opaque(merged) && !opaque(*ours8)) {
        // Photoshop stores a merged image with transparency together with its transparency plane (the layer count is
        // negative), and puts its version info (resource 1057) in every file it writes. A merged image without
        // transparency over layers that leave the canvas clear, in a file without that resource, was flattened onto a
        // matte by another program (patchy-legacy-black-composite.psb: an old Patchy writer's black-matted merged
        // image), so the file holds no Photoshop composite of its layers to compare with.
        r.why = "merged image matted by another program (no Photoshop version info, no transparency over transparent layers)";
        return r;
    }
    r.compared = true;
    const std::vector<Box> dissolve = dissolveRegions(document);
    dumpMerged(r.name, ours8.get(), merged);
    if (imported->compositeNative) {
        // CMYK and Lab in the document's own space: no profile between the two.
        const AnyImage& ps = imported->compositeNative;
        const AnyImage ours = renderNative(document);
        const std::string mode = document.colorMode == ColorMode::CMYK ? "cmyk" : document.colorMode == ColorMode::Lab ? "lab" : "native";
        r.reference = mode + (ps.u16() ? "16" : "8");
        if (ps.c8() && ours.c8() && sameShape(*ps.c8(), *ours.c8())) measureImages(r, *ours.c8(), *ps.c8(), 1.0, dissolve);
        else if (ps.u16() && ours.u16() && sameShape(*ps.u16(), *ours.u16())) measureImages(r, *ours.u16(), *ps.u16(), 255.0 / 32768.0, dissolve);
        else if (ps.u8() && ours.u8() && ps.u8()->width() == ours.u8()->width() && ps.u8()->height() == ours.u8()->height()) {
            const Image& a = *ours.u8();
            const Image& b = *ps.u8();
            measure(r, b.width(), b.height(), 4, [&](int x, int y, int c) { return double(a.pixel(x, y)[c]); },
                    [&](int x, int y, int c) { return double(b.pixel(x, y)[c]); }, dissolve);
        } else sizeDiffers(r, "the render's layout or size differs from the merged image's");
        return r;
    }
    if (imported->compositeF) {
        r.reference = "f32";
        auto ours = renderFlattenedF(document);
        if (!ours || !sameShape(*ours, *imported->compositeF)) { sizeDiffers(r, "size differs"); return r; }
        measureImages(r, *ours, *imported->compositeF, 255.0, dissolve);
        return r;
    }
    if (imported->composite16) {
        r.reference = "rgb16";
        auto ours = renderFlattened16(document);
        if (!ours || !sameShape(*ours, *imported->composite16)) { sizeDiffers(r, "size differs"); return r; }
        measureImages(r, *ours, *imported->composite16, 255.0 / 32768.0, dissolve);
        return r;
    }
    r.reference = "rgb8";
    if (!ours8 || ours8->width() != merged.width() || ours8->height() != merged.height()) { sizeDiffers(r, "size differs"); return r; }
    const Image& a = *ours8;
    measure(r, merged.width(), merged.height(), 4, [&](int x, int y, int c) { return double(a.pixel(x, y)[c]); },
            [&](int x, int y, int c) { return double(merged.pixel(x, y)[c]); }, dissolve);
    return r;
}

// ---- corpora -------------------------------------------------------------------------------------------------

struct Corpus {
    std::string id;            // "patchy", "photocraft", "psd-tools"; empty for folders named on the command line
    std::string variable;      // the environment variable that names its root
    fs::path root;
    fs::path expectations;     // tests/psd_oracle*.txt
    fs::path manifest;         // tests/*-manifest.txt (patchy, psd-tools); none for photocraft (SHA256SUMS)
    std::string description;
};

/// The PSDs and PSBs under `root`, recursively (hidden folders such as .git skipped), relative to it.
std::vector<std::string> collect(const fs::path& root) {
    std::vector<std::string> out;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec); it != fs::recursive_directory_iterator();
         it.increment(ec)) {
        if (ec) break;
        const std::string leaf = it->path().filename().string();
        if (it->is_directory(ec)) {
            if (!leaf.empty() && leaf[0] == '.') it.disable_recursion_pending();
            continue;
        }
        const std::string ext = lower(it->path().extension().string());
        if (it->is_regular_file(ec) && (ext == ".psd" || ext == ".psb")) out.push_back(fs::relative(it->path(), root, ec).generic_string());
    }
    std::sort(out.begin(), out.end());
    return out;
}

struct Manifest {
    std::string commit;
    std::map<std::string, std::string> files;   // name -> sha256
};

/// "<sha256>  <name>" lines (sha256sum's format, which SHA256SUMS and our manifests share), and "commit <sha>".
Manifest readManifest(const fs::path& path) {
    Manifest m;
    for (const std::string& raw : readLines(path)) {
        const std::string line = trim(raw);
        if (line.empty() || line[0] == '#') continue;
        if (line.rfind("commit ", 0) == 0) { m.commit = trim(line.substr(7)); continue; }
        const size_t space = line.find(' ');
        if (space == 64) {
            std::string name = trim(line.substr(space));
            if (!name.empty() && name[0] == '*') name = name.substr(1);   // sha256sum's binary marker
            m.files[name] = line.substr(0, 64);
        }
    }
    return m;
}

struct Expected {
    std::string status;   // "pass", "FAIL" or "--"
    double mean = 0;
};

std::map<std::string, Expected> readExpectations(const fs::path& path) {
    std::map<std::string, Expected> out;
    for (const std::string& raw : readLines(path)) {
        const auto w = words(raw);
        if (w.size() < 2 || (w[0] != "pass" && w[0] != "FAIL" && w[0] != "--")) continue;
        Expected e;
        e.status = w[0];
        for (size_t i = 2; i + 1 < w.size(); i++) if (w[i] == "mean") e.mean = std::atof(w[i + 1].c_str());
        out[w[1]] = e;
    }
    return out;
}

std::string statusOf(const Result& r) { return !r.compared ? "--" : r.pass ? "pass" : "FAIL"; }

std::string line(const Result& r) {
    char buffer[512];
    const std::string ref = r.reference.empty() ? "" : "[" + r.reference + (r.dissolve ? " dissolve" : "") + "]";
    if (r.compared && r.why.empty())
        std::snprintf(buffer, sizeof buffer, "%-4s %-64s %-18s max %7.2f  >2: %6.2f%%  mean %7.3f", statusOf(r).c_str(), r.name.c_str(), ref.c_str(), r.maxError,
                      r.shareOver2, r.mean);
    else std::snprintf(buffer, sizeof buffer, "%-4s %-64s %-18s %s", statusOf(r).c_str(), r.name.c_str(), ref.c_str(), r.why.c_str());
    return trim(buffer);
}

void skipLoudly(const std::string& message) {
    std::printf("\n*** SKIPPED: %s ***\n\n", message.c_str());
}

int run(const Corpus& corpus, const std::vector<std::string>& names, const std::map<std::string, std::string>& hashes, const std::vector<std::string>& drift,
        const std::string& commit) {
    std::vector<Result> results;
    for (const std::string& name : names) {
        results.push_back(compare(corpus.root / name, name));
        std::printf("%s\n", line(results.back()).c_str());
        std::fflush(stdout);
    }
    int compared = 0, passed = 0, againstFlatten = 0;
    for (const Result& r : results) { compared += r.compared; passed += r.pass; againstFlatten += r.compared && r.reference == "bmp"; }
    std::map<std::string, int> byRef, passByRef;
    for (const Result& r : results) if (r.compared) { byRef[r.reference]++; passByRef[r.reference] += r.pass; }

    std::vector<const Result*> worst;
    for (const Result& r : results) if (r.compared && !r.pass) worst.push_back(&r);
    std::sort(worst.begin(), worst.end(), [](const Result* a, const Result* b) { return a->mean > b->mean; });
    if (!worst.empty()) {
        std::printf("\nWorst by mean difference:\n");
        for (size_t i = 0; i < worst.size() && i < 15; i++) std::printf("  %s\n", line(*worst[i]).c_str());
    }
    std::printf("\n%s: %d files, %d with a Photoshop reference, %d pass (within 2 levels on %.0f%% of pixels or more, mean under %.0f level)\n",
                corpus.id.empty() ? corpus.root.string().c_str() : corpus.id.c_str(), int(results.size()), compared, passed, 100 - kMaxShareOver2, kMaxMean);
    for (const auto& [ref, n] : byRef) std::printf("  [%s] %d of %d\n", ref.c_str(), passByRef[ref], n);
    if (corpus.id.empty()) return 0;   // folders named on the command line: reported only

    if (const char* update = std::getenv("NEKOPHOTO_UPDATE_PSD_ORACLE"); update && *update && std::strcmp(update, "0") != 0) {
        std::ofstream o(corpus.expectations, std::ios::binary);
        o << "# Photoshop oracle over " << corpus.description << " (tests/psd_composite_oracle.cpp): each file's status and\n"
          << "# numbers (max and mean in 8-bit levels, the share of pixels more than 2 levels off) when this was written.\n"
          << "# A pass that fails, a file no longer compared, or a failure whose mean grows by more than a level fails the test.\n"
          << "# Rewrite it after a change: NEKOPHOTO_UPDATE_PSD_ORACLE=1 build/tests/psd_composite_oracle --corpus " << corpus.id << "\n"
          << "corpus " << corpus.id << "\n"
          << "commit " << commit << "\n"
          << "compared " << compared << "\n"
          << "flatten " << againstFlatten << "\n"
          << "floor " << passed << "\n";
        for (const Result& r : results) o << line(r) << "\n";
        std::printf("wrote %s (%d of %d pass)\n", corpus.expectations.string().c_str(), passed, compared);
        if (!corpus.manifest.empty()) {
            std::ofstream m(corpus.manifest, std::ios::binary);
            m << "# " << corpus.description << "\n"
              << "# that tests/psd_composite_oracle.cpp measures: the commit and each file's SHA-256. Rewritten with NEKOPHOTO_UPDATE_PSD_ORACLE=1.\n"
              << "commit " << commit << "\n";
            for (const auto& [name, hash] : hashes) m << hash << "  " << name << "\n";
            std::printf("wrote %s\n", corpus.manifest.string().c_str());
        }
        return 0;
    }

    if (!drift.empty()) {
        std::printf("\nwarning: the checkout differs from %s; the numbers are for other files:\n", corpus.manifest.string().c_str());
        for (size_t i = 0; i < drift.size() && i < 20; i++) std::printf("  %s\n", drift[i].c_str());
        if (drift.size() > 20) std::printf("  ... and %d more\n", int(drift.size() - 20));
    }
    const auto expected = readExpectations(corpus.expectations);
    if (expected.empty()) {
        std::printf("FAIL: no expectations in %s; write them with NEKOPHOTO_UPDATE_PSD_ORACLE=1\n", corpus.expectations.string().c_str());
        return 1;
    }
    std::vector<std::string> regressed, improved, fresh;
    for (const Result& r : results) {
        auto it = expected.find(r.name);
        if (it == expected.end()) { fresh.push_back(line(r)); continue; }
        const Expected& e = it->second;
        const std::string now = statusOf(r);
        if (e.status == "pass" && now != "pass") regressed.push_back(line(r) + "   (passed)");
        else if (e.status == "FAIL" && now == "--") regressed.push_back(line(r) + "   (was compared)");
        else if (e.status == "FAIL" && now == "FAIL" && r.mean > e.mean + kWorseMean) {
            char was[64];
            std::snprintf(was, sizeof was, "   (mean was %.3f)", e.mean);
            regressed.push_back(line(r) + was);
        } else if (e.status != "pass" && now == "pass") improved.push_back(line(r));
    }
    std::vector<std::string> gone;
    for (const auto& [name, e] : expected)
        if (std::none_of(results.begin(), results.end(), [&](const Result& r) { return r.name == name; })) gone.push_back(name);
    if (!fresh.empty()) {
        std::printf("\nNot in %s:\n", corpus.expectations.string().c_str());
        for (const std::string& s : fresh) std::printf("  %s\n", s.c_str());
    }
    if (!gone.empty()) {
        std::printf("\nIn %s, missing here:\n", corpus.expectations.string().c_str());
        for (const std::string& s : gone) std::printf("  %s\n", s.c_str());
    }
    if (!improved.empty()) {
        std::printf("\nIMPROVED (pass now; record it with NEKOPHOTO_UPDATE_PSD_ORACLE=1 build/tests/psd_composite_oracle --corpus %s):\n", corpus.id.c_str());
        for (const std::string& s : improved) std::printf("  %s\n", s.c_str());
    }
    if (!regressed.empty()) {
        std::printf("\nREGRESSED against %s:\n", corpus.expectations.string().c_str());
        for (const std::string& s : regressed) std::printf("  %s\n", s.c_str());
        return 1;
    }
    return 0;
}

int runCorpus(const Corpus& corpus) {
    std::error_code ec;
    if (corpus.root.empty() || !fs::is_directory(corpus.root, ec)) {
        if (corpus.root.empty()) skipLoudly("no " + corpus.description + ": set " + corpus.variable + " to its checkout");
        else skipLoudly("no " + corpus.description + " at " + corpus.root.string() + " (set " + corpus.variable + ")");
        return kSkipped;
    }
    std::vector<std::string> names;
    std::map<std::string, std::string> hashes;
    std::vector<std::string> drift;
    const std::string commit = gitCommit(corpus.root);
    if (corpus.id == "photocraft") {
        // Pinned by its own SHA256SUMS: every file listed, as listed.
        const fs::path sums = corpus.root / "SHA256SUMS";
        const Manifest m = readManifest(sums);
        if (m.files.empty()) { std::printf("FAIL: %s has no SHA256SUMS (is it a photocraft-corpus checkout?)\n", corpus.root.string().c_str()); return 1; }
        std::vector<std::string> bad;
        for (const auto& [name, hash] : m.files) {
            if (!fs::is_regular_file(corpus.root / name, ec)) { bad.push_back(name + ": missing"); continue; }
            if (sha256(readFile(corpus.root / name)) != hash) { bad.push_back(name + ": differs from SHA256SUMS"); continue; }
            const std::string ext = lower(fs::path(name).extension().string());
            if (ext == ".psd" || ext == ".psb") names.push_back(name);
        }
        for (const std::string& name : collect(corpus.root))
            if (!m.files.count(name)) std::printf("note: %s is not in SHA256SUMS and is not compared\n", name.c_str());
        if (!bad.empty()) {
            std::printf("FAIL: the corpus at %s does not match its SHA256SUMS:\n", corpus.root.string().c_str());
            for (const std::string& s : bad) std::printf("  %s\n", s.c_str());
            return 1;
        }
        std::printf("%s at %s: %d files verified against SHA256SUMS\n", corpus.description.c_str(), commit.c_str(), int(m.files.size()));
    } else {
        names = collect(corpus.root);
        for (const std::string& name : names) {
            const fs::path file = corpus.root / name;
            hashes[name] = sha256(readFile(file));
            if (const fs::path bmp = flattenFor(file); !bmp.empty())   // the reference too
                hashes[fs::relative(bmp, corpus.root, ec).generic_string()] = sha256(readFile(bmp));
        }
        const Manifest manifest = readManifest(corpus.manifest);
        if (!manifest.commit.empty() && commit != "unknown" && commit != manifest.commit)
            drift.push_back("the checkout is at " + commit + ", the manifest pins " + manifest.commit);
        for (const auto& [name, hash] : hashes) {
            auto it = manifest.files.find(name);
            if (it == manifest.files.end()) drift.push_back(name + ": not in the manifest");
            else if (it->second != hash) drift.push_back(name + ": differs from the manifest");
        }
        for (const auto& [name, hash] : manifest.files)
            if (!hashes.count(name)) drift.push_back(name + ": in the manifest, missing here");
    }
    if (names.empty()) { skipLoudly("no PSD or PSB files in " + corpus.root.string()); return kSkipped; }
    std::printf("%s: %d files under %s\n\n", corpus.id.c_str(), int(names.size()), corpus.root.string().c_str());
    return run(corpus, names, hashes, drift, commit);
}

fs::path fromEnvironment(const char* variable, const char* fallback = nullptr) {
    const char* env = std::getenv(variable);
    if (env && *env) return fs::path(env);
    return fallback ? fs::path(fallback) : fs::path();
}

} // namespace

int main(int argc, char** argv) {
    std::string which = "patchy";
    std::vector<fs::path> folders;
    for (int i = 1; i < argc; i++) {
        const std::string arg = argv[i];
        if (arg == "--corpus" && i + 1 < argc) which = argv[++i];
        else if (arg == "--help" || arg == "-h") {
            std::printf("usage: psd_composite_oracle [--corpus patchy|photocraft|psd-tools] | DIR...\n");
            return 0;
        } else folders.push_back(arg);
    }
    if (!folders.empty()) {
        // Ad hoc: every PSD and PSB under each folder, reported without expectations.
        for (const fs::path& folder : folders) {
            Corpus c;
            c.root = folder;
            std::error_code ec;
            if (!fs::is_directory(folder, ec)) { std::printf("%s: not a folder\n", folder.string().c_str()); return 1; }
            run(c, collect(folder), {}, {}, gitCommit(folder));
        }
        return 0;
    }
    const fs::path tests(TESTS_DIR);
    Corpus c;
    c.id = which;
    if (which == "patchy") {
        c.variable = "PATCHY_FIXTURES";
        c.root = fromEnvironment("PATCHY_FIXTURES", PATCHY_FIXTURES);
        c.expectations = tests / "psd_oracle.txt";
        c.manifest = tests / "patchy-manifest.txt";
        c.description = "Patchy's fixtures (MIT, https://github.com/SethRobinson/Patchy, test-fixtures/psd)";
    } else if (which == "photocraft") {
        c.variable = "PHOTOSHOP_CORPUS";
        c.root = fromEnvironment("PHOTOSHOP_CORPUS");
        c.expectations = tests / "psd_oracle_photocraft.txt";
        c.description = "photocraft-corpus (MIT OR Apache-2.0, https://github.com/storytold/photocraft-corpus)";
    } else if (which == "psd-tools") {
        c.variable = "PSD_TOOLS_CORPUS";
        c.root = fromEnvironment("PSD_TOOLS_CORPUS");
        c.expectations = tests / "psd_oracle_psdtools.txt";
        c.manifest = tests / "psd-tools-manifest.txt";
        c.description = "psd-tools' test files (MIT, https://github.com/psd-tools/psd-tools, tests/psd_files)";
    } else {
        std::printf("unknown corpus %s (patchy, photocraft or psd-tools)\n", which.c_str());
        return 2;
    }
    return runCorpus(c);
}
