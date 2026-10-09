// Photoshop as the oracle: every PSD and PSB of Patchy's fixtures is rendered by NekoPhoto from its layers and compared
// with what Photoshop shows for it. The reference is Photoshop's own flatten where Patchy keeps one beside the file (a
// 24-bit .bmp of the same name, which Patchy's docs name as the render reference: the merged image stored inside a
// file can be stale when Photoshop saved it headless), else the merged image Photoshop stored in the file ("Maximize
// Compatibility"). Each line says which: [bmp] or [merged].
//
//   psd_composite_oracle [DIR]     DIR, else $PATCHY_FIXTURES, else ../Patchy/test-fixtures/psd beside the checkout
//
// For each file: the largest difference of any channel (0..255), the share of pixels with a channel more than 2
// levels off, and the mean difference per channel. Against a merged image the channels are premultiplied RGBA; against
// a flatten they are RGB, NekoPhoto's render matted on white as Photoshop flattens. A file passes when no more than 1%
// of its pixels are more than 2 levels off and the mean is under 1 level. Files with neither reference are listed,
// not compared: no flatten and saved without Maximize Compatibility, or merged onto a matte by another program (see
// compare()). tests/psd_oracle.txt holds the floor: how many files passed
// when it was last raised, and which. Fewer passing files than the floor fails; more prints a reminder to raise it:
//
//   COMPOSITOR_UPDATE_PSD_ORACLE=1 build/tests/psd_composite_oracle
//
// rewrites tests/psd_oracle.txt and tests/patchy-manifest.txt (the Patchy commit and the SHA-256 of every fixture
// and flatten used). A checkout that differs from the manifest is reported, not failed: the numbers are then for other files.
// Without the fixtures the test is skipped.
//
// PSD_ORACLE_DUMP=<dir> writes, for every compared file, NekoPhoto's render, the reference and a difference map
// (each pixel's largest channel difference, times 8) as PNGs: <name>-ours.png, -ps.png and -diff.png.
#include "compositor/png.h"
#include "compositor/psd.h"
#include "compositor/psd_carry.h"
#include "compositor/render.h"
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
#ifndef ORACLE_FILE
#define ORACLE_FILE "tests/psd_oracle.txt"
#endif
#ifndef MANIFEST_FILE
#define MANIFEST_FILE "tests/patchy-manifest.txt"
#endif

namespace fs = std::filesystem;
using namespace compositor;

namespace {

constexpr double kMaxShareOver2 = 1.0;   // percent of pixels more than 2 levels off
constexpr double kMaxMean = 1.0;         // levels, per channel

// ---- SHA-256 (FIPS 180-4), for the fixture manifest -------------------------------------------------------

std::string sha256(const std::vector<uint8_t>& data) {
    static const uint32_t k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be,
        0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa,
        0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85,
        0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
        0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f,
        0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::vector<uint8_t> m(data);
    const uint64_t bits = uint64_t(data.size()) * 8;
    m.push_back(0x80);
    while (m.size() % 64 != 56) m.push_back(0);
    for (int i = 7; i >= 0; i--) m.push_back(uint8_t(bits >> (i * 8)));
    auto rotr = [](uint32_t x, int n) { return (x >> n) | (x << (32 - n)); };
    for (size_t block = 0; block < m.size(); block += 64) {
        uint32_t w[64];
        for (int i = 0; i < 16; i++)
            w[i] = uint32_t(m[block + size_t(i) * 4]) << 24 | uint32_t(m[block + size_t(i) * 4 + 1]) << 16 | uint32_t(m[block + size_t(i) * 4 + 2]) << 8 |
                   uint32_t(m[block + size_t(i) * 4 + 3]);
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
    }
    char out[65];
    for (int i = 0; i < 8; i++) std::snprintf(out + i * 8, 9, "%08x", h[i]);
    return std::string(out, 64);
}

std::vector<uint8_t> readFile(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), {});
}

/// Lines of a text file without their line endings (a Windows checkout may have made them CRLF).
std::vector<std::string> readLines(const std::string& path) {
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

/// The commit a git checkout of Patchy is at, read from its .git folder (no git needed); "unknown" otherwise.
std::string patchyCommit(const fs::path& fixtures) {
    std::error_code ec;
    fs::path root = fs::weakly_canonical(fixtures, ec);
    for (int up = 0; up < 4 && !root.empty(); up++, root = root.parent_path()) {
        const fs::path git = root / ".git";
        if (!fs::is_directory(git, ec)) continue;
        const auto head = readLines((git / "HEAD").string());
        if (head.empty()) return "unknown";
        if (head[0].rfind("ref: ", 0) != 0) return trim(head[0]);
        const std::string ref = trim(head[0].substr(5));
        const auto loose = readLines((git / ref).string());
        if (!loose.empty()) return trim(loose[0]);
        for (const std::string& line : readLines((git / "packed-refs").string()))
            if (line.size() > 41 && line.compare(41, std::string::npos, ref) == 0) return line.substr(0, 40);
        return "unknown";
    }
    return "unknown";
}

struct Result {
    std::string name;
    std::string reference;   // "bmp" (Photoshop's flatten beside the file) or "merged" (the file's merged image)
    bool compared = false;   // false: no reference to compare with (the reason in `why`)
    std::string why;
    int maxError = 0;
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
    std::string ext = psd.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
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

/// The differences of `ours` (premultiplied RGBA) against `ps`, `channels` per pixel (`ps` RGBA, or RGB with `ours`
/// matted on white).
void measure(Result& r, const Image& ours, const uint8_t* ps, int channels) {
    uint64_t over = 0, sum = 0;
    int worst = 0;
    for (int y = 0; y < ours.height(); y++) {
        const uint8_t* b = ours.row(y);
        const uint8_t* a = ps + size_t(y) * size_t(ours.width()) * size_t(channels);
        for (int x = 0; x < ours.width(); x++, a += channels, b += 4) {
            int pixelWorst = 0;
            for (int c = 0; c < channels; c++) {
                const int mine = channels == 3 ? int(b[c]) + 255 - int(b[3]) : int(b[c]);
                const int d = std::abs(int(a[c]) - mine);
                sum += uint64_t(d);
                pixelWorst = std::max(pixelWorst, d);
            }
            worst = std::max(worst, pixelWorst);
            if (pixelWorst > 2) over++;
        }
    }
    const double pixels = double(ours.width()) * ours.height();
    r.maxError = worst;
    r.shareOver2 = pixels > 0 ? 100.0 * double(over) / pixels : 0;
    r.mean = pixels > 0 ? double(sum) / (pixels * channels) : 0;
    r.pass = r.shareOver2 <= kMaxShareOver2 && r.mean < kMaxMean;
}

/// PSD_ORACLE_DUMP: the render, the reference (`channels` 3: RGB, matted on white; 4: premultiplied RGBA) and the map.
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
    const std::string stem = std::string(dir) + "/" + fs::path(name).stem().string();
    writePngImage(stem + "-ours.png", mine);
    writePngImage(stem + "-ps.png", theirs);
    writePngImage(stem + "-diff.png", diff);
}

Result compare(const fs::path& path) {
    Result r;
    r.name = path.filename().string();
    std::string error;
    auto imported = importPsd(path.string(), &error);
    if (!imported) { r.why = "does not open: " + error; return r; }
    if (const fs::path bmp = flattenFor(path); !bmp.empty()) {
        // Photoshop's own flatten: what Photoshop shows.
        const Flatten ps = readBmp(bmp);
        if (ps.width > 0) {
            r.reference = "bmp";
            r.compared = true;
            auto ours = renderFlattened(imported->document);
            if (!ours || ours->width() != ps.width || ours->height() != ps.height) {
                r.why = "size differs from " + bmp.filename().string();
                r.maxError = 255; r.shareOver2 = 100; r.mean = 255;
                return r;
            }
            measure(r, *ours, ps.rgb.data(), 3);
            dump(r.name, *ours, ps.rgb.data(), 3);
            return r;
        }
    }
    if (!imported->composite) { r.why = "no merged image"; return r; }
    if (!imported->realComposite) { r.why = "saved without Maximize Compatibility"; return r; }
    const Image& ps = *imported->composite;
    auto ours = renderFlattened(imported->document);
    if (!writtenByPhotoshop(imported->document) && ours && ours->width() == ps.width() && ours->height() == ps.height() && opaque(ps) && !opaque(*ours)) {
        // Photoshop stores a merged image with transparency together with its transparency plane (the layer count is
        // negative), and puts its version info (resource 1057) in every file it writes. A merged image without
        // transparency over layers that leave the canvas clear, in a file without that resource, was flattened onto a
        // matte by another program (patchy-legacy-black-composite.psb: an old Patchy writer's black-matted merged
        // image), so the file holds no Photoshop composite of its layers to compare with.
        r.why = "merged image matted by another program (no Photoshop version info, no transparency over transparent layers)";
        return r;
    }
    r.reference = "merged";
    r.compared = true;
    if (!ours || ours->width() != ps.width() || ours->height() != ps.height()) {
        r.why = "size differs";
        r.maxError = 255; r.shareOver2 = 100; r.mean = 255;
        return r;
    }
    // One contiguous RGBA buffer of Photoshop's merged image, for measure().
    std::vector<uint8_t> rows(size_t(ps.width()) * size_t(ps.height()) * 4);
    for (int y = 0; y < ps.height(); y++) std::memcpy(rows.data() + size_t(y) * size_t(ps.width()) * 4, ps.row(y), size_t(ps.width()) * 4);
    measure(r, *ours, rows.data(), 4);
    dump(r.name, *ours, rows.data(), 4);
    return r;
}

struct Oracle {
    int floor = 0;
    std::set<std::string> passing;
};

Oracle readOracle(const std::string& path) {
    Oracle o;
    for (const std::string& raw : readLines(path)) {
        const std::string line = trim(raw);
        if (line.empty() || line[0] == '#') continue;
        if (line.rfind("floor ", 0) == 0) o.floor = std::atoi(line.c_str() + 6);
        else if (line.rfind("pass ", 0) == 0) {
            std::string name = trim(line.substr(5));
            if (const size_t bracket = name.find(" ["); bracket != std::string::npos) name = trim(name.substr(0, bracket));
            o.passing.insert(name);
        }
    }
    return o;
}

struct Manifest {
    std::string commit;
    std::map<std::string, std::string> files;   // name -> sha256
};

Manifest readManifest(const std::string& path) {
    Manifest m;
    for (const std::string& raw : readLines(path)) {
        const std::string line = trim(raw);
        if (line.empty() || line[0] == '#') continue;
        if (line.rfind("commit ", 0) == 0) { m.commit = trim(line.substr(7)); continue; }
        const size_t space = line.find(' ');
        if (space == 64) m.files[trim(line.substr(space))] = line.substr(0, 64);
    }
    return m;
}

} // namespace

int main(int argc, char** argv) {
    const char* env = std::getenv("PATCHY_FIXTURES");
    const fs::path dir = argc > 1 ? fs::path(argv[1]) : fs::path(env && *env ? env : PATCHY_FIXTURES);
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) {
        std::printf("skipped: no Patchy fixtures at %s (set PATCHY_FIXTURES or check Patchy out beside this repository)\n", dir.string().c_str());
        return 0;
    }
    std::vector<fs::path> files;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        std::string ext = entry.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        if (entry.is_regular_file() && (ext == ".psd" || ext == ".psb")) files.push_back(entry.path());
    }
    std::sort(files.begin(), files.end());
    if (files.empty()) { std::printf("skipped: no PSD or PSB files in %s\n", dir.string().c_str()); return 0; }

    // The pin: the commit and every file's SHA-256, against the manifest.
    const std::string commit = patchyCommit(dir);
    std::map<std::string, std::string> hashes;
    for (const fs::path& f : files) {
        hashes[f.filename().string()] = sha256(readFile(f));
        if (const fs::path bmp = flattenFor(f); !bmp.empty()) hashes[bmp.filename().string()] = sha256(readFile(bmp));   // the reference too
    }
    const Manifest manifest = readManifest(MANIFEST_FILE);
    std::vector<std::string> drift;
    if (!manifest.commit.empty() && commit != "unknown" && commit != manifest.commit)
        drift.push_back("Patchy is at " + commit + ", the manifest pins " + manifest.commit);
    for (const auto& [name, hash] : hashes) {
        auto it = manifest.files.find(name);
        if (it == manifest.files.end()) drift.push_back(name + ": not in the manifest");
        else if (it->second != hash) drift.push_back(name + ": differs from the manifest");
    }
    for (const auto& [name, hash] : manifest.files)
        if (!hashes.count(name)) drift.push_back(name + ": in the manifest, missing here");

    std::vector<Result> results;
    for (const fs::path& f : files) {
        results.push_back(compare(f));
        const Result& r = results.back();
        const std::string ref = r.reference.empty() ? "" : "[" + r.reference + "]";
        if (r.compared && r.why.empty())
            std::printf("%s %-8s %-60s max %3d  >2: %6.2f%%  mean %6.3f\n", r.pass ? "pass" : "FAIL", ref.c_str(), r.name.c_str(), r.maxError, r.shareOver2, r.mean);
        else if (r.compared) std::printf("FAIL %-8s %-60s %s\n", ref.c_str(), r.name.c_str(), r.why.c_str());
        else std::printf("--   %-8s %-60s %s\n", "", r.name.c_str(), r.why.c_str());
        std::fflush(stdout);
    }
    int compared = 0, passed = 0, againstFlatten = 0;
    for (const Result& r : results) { compared += r.compared; passed += r.pass; againstFlatten += r.compared && r.reference == "bmp"; }

    std::vector<const Result*> worst;
    for (const Result& r : results) if (r.compared && !r.pass) worst.push_back(&r);
    std::sort(worst.begin(), worst.end(), [](const Result* a, const Result* b) { return a->mean > b->mean; });
    if (!worst.empty()) {
        std::printf("\nWorst by mean difference:\n");
        for (size_t i = 0; i < worst.size() && i < 15; i++)
            std::printf("  %-60s %-8s mean %7.3f  >2: %6.2f%%  max %3d\n", worst[i]->name.c_str(), ("[" + worst[i]->reference + "]").c_str(), worst[i]->mean,
                        worst[i]->shareOver2, worst[i]->maxError);
    }
    std::printf("\n%d files, %d with a Photoshop reference (%d its flatten, %d the merged image), %d pass (within 2 levels on %.0f%% of pixels or more, "
                "mean under %.0f level)\n",
                int(results.size()), compared, againstFlatten, compared - againstFlatten, passed, 100 - kMaxShareOver2, kMaxMean);

    if (const char* update = std::getenv("COMPOSITOR_UPDATE_PSD_ORACLE"); update && *update && std::strcmp(update, "0") != 0) {
        std::ofstream o(ORACLE_FILE, std::ios::binary);
        o << "# Photoshop oracle: how many of Patchy's fixtures NekoPhoto renders like Photoshop's own flatten beside the file\n"
          << "# (the .bmp), else like the merged image stored in it (tests/psd_composite_oracle.cpp). The floor may only rise;\n"
          << "# the test fails below it. Each pass names its reference.\n"
          << "# Raise it after an improvement: COMPOSITOR_UPDATE_PSD_ORACLE=1 build/tests/psd_composite_oracle\n"
          << "compared " << compared << "\n"
          << "flatten " << againstFlatten << "\n"
          << "floor " << passed << "\n";
        for (const Result& r : results) if (r.pass) o << "pass " << r.name << " [" << r.reference << "]\n";
        std::ofstream m(MANIFEST_FILE, std::ios::binary);
        m << "# Patchy's fixtures (MIT, https://github.com/SethRobinson/Patchy, test-fixtures/psd) that tests/psd_composite_oracle.cpp\n"
          << "# measures: the commit and each file's SHA-256. Rewritten with COMPOSITOR_UPDATE_PSD_ORACLE=1.\n"
          << "commit " << commit << "\n";
        for (const auto& [name, hash] : hashes) m << hash << "  " << name << "\n";
        std::printf("wrote %s (floor %d) and %s\n", ORACLE_FILE, passed, MANIFEST_FILE);
        return 0;
    }

    if (!drift.empty()) {
        std::printf("\nwarning: the Patchy checkout differs from %s; the numbers are for other files:\n", MANIFEST_FILE);
        for (size_t i = 0; i < drift.size() && i < 20; i++) std::printf("  %s\n", drift[i].c_str());
        if (drift.size() > 20) std::printf("  ... and %d more\n", int(drift.size() - 20));
    }
    const Oracle oracle = readOracle(ORACLE_FILE);
    std::vector<std::string> regressed;
    for (const Result& r : results) if (oracle.passing.count(r.name) && !r.pass) regressed.push_back(r.name);
    if (!regressed.empty()) {
        std::printf("\nPassed at the last floor, failing now:\n");
        for (const std::string& name : regressed) std::printf("  %s\n", name.c_str());
    }
    if (passed < oracle.floor) {
        std::printf("FAIL: %d files pass, below the floor of %d in %s\n", passed, oracle.floor, ORACLE_FILE);
        return 1;
    }
    if (passed > oracle.floor)
        std::printf("%d files pass, above the floor of %d: raise it with COMPOSITOR_UPDATE_PSD_ORACLE=1 build/tests/psd_composite_oracle\n", passed,
                    oracle.floor);
    return 0;
}
