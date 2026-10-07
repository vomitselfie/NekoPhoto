// Hue/Saturation and Color Balance against Photoshop's own output for Patchy's fixtures, when Patchy is beside this
// checkout: photoshop-hue-saturation-{master,bands}.psd against Photoshop's flatten of them (.bmp; the merged image
// stored in those two files predates the settings they carry), photoshop-color-balance{,-full}.psd against the merged
// image. Without the fixtures those cases are skipped; the rules they pin are also checked on synthetic colours.
#include "check.h"
#include "compositor/adjustments.h"
#include "compositor/psd.h"
#include "compositor/render.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace compositor;

namespace {

std::string fixture(const char* name) {
    const char* dir = std::getenv("PATCHY_FIXTURES");
    return std::string(dir ? dir : PATCHY_FIXTURES) + "/" + name;
}

/// A 24-bit BMP as straight RGB rows, top first; empty when it cannot be read.
std::vector<uint8_t> readBmp24(const std::string& path, int& w, int& h) {
    std::ifstream in(path, std::ios::binary);
    std::vector<uint8_t> f((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (f.size() < 54 || f[0] != 'B' || f[1] != 'M') return {};
    auto u32 = [&](size_t at) { return uint32_t(f[at]) | uint32_t(f[at + 1]) << 8 | uint32_t(f[at + 2]) << 16 | uint32_t(f[at + 3]) << 24; };
    const uint32_t offset = u32(10);
    w = int(int32_t(u32(18)));
    const int rawH = int(int32_t(u32(22)));
    h = std::abs(rawH);
    if (f[28] != 24 || w <= 0 || h <= 0) return {};
    const size_t stride = (size_t(w) * 3 + 3) / 4 * 4;
    if (f.size() < offset + stride * size_t(h)) return {};
    std::vector<uint8_t> rgb(size_t(w) * size_t(h) * 3);
    for (int y = 0; y < h; y++) {
        const uint8_t* row = f.data() + offset + stride * size_t(rawH > 0 ? h - 1 - y : y);
        for (int x = 0; x < w; x++) for (int k = 0; k < 3; k++) rgb[(size_t(y) * size_t(w) + size_t(x)) * 3 + size_t(k)] = row[x * 3 + 2 - k];
    }
    return rgb;
}

struct Apart { int worst = -1; double mean = 0, overTwo = 0; };

/// NekoPhoto's render of a fixture against Photoshop's: its flatten (.bmp) when named, else the stored merged image.
Apart againstPhotoshop(const char* psd, const char* bmp) {
    std::string error;
    auto imported = importPsd(fixture(psd), &error);
    if (!imported) return {};
    std::vector<uint8_t> ps;
    int w = 0, h = 0;
    if (bmp) ps = readBmp24(fixture(bmp), w, h);
    else if (imported->composite && imported->realComposite) {
        w = imported->composite->width();
        h = imported->composite->height();
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) for (int k = 0; k < 3; k++) ps.push_back(imported->composite->pixel(x, y)[k]);
    }
    if (ps.empty()) return {};
    const auto ours = renderFlattened(imported->document);
    Apart a;
    if (ours->width() != w || ours->height() != h) { a.worst = 255; return a; }
    a.worst = 0;
    double sum = 0;
    size_t over = 0;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int pixel = 0;
            for (int k = 0; k < 3; k++) {
                const int d = std::abs(int(ours->pixel(x, y)[k]) - int(ps[(size_t(y) * size_t(w) + size_t(x)) * 3 + size_t(k)]));
                sum += d;
                pixel = std::max(pixel, d);
            }
            a.worst = std::max(a.worst, pixel);
            over += pixel > 2;
        }
    a.mean = sum / (double(w) * h * 3);
    a.overTwo = double(over) / (double(w) * h);
    std::printf("  %s: max %d, mean %.3f, %.2f%% beyond two levels\n", psd, a.worst, a.mean, a.overTwo * 100);
    return a;
}

} // namespace

TEST_CASE(hue_saturation_master_matches_photoshop) {
    // Six layers, each on its own strip of the probe: hue, saturation (-100, +60) and lightness alone and together.
    const Apart a = againstPhotoshop("photoshop-hue-saturation-master.psd", "photoshop-hue-saturation-master.bmp");
    if (a.worst < 0) { std::printf("  skipped: Patchy's fixtures are not beside this checkout\n"); return; }
    CHECK(a.worst <= 1);
    CHECK(a.mean < 0.1);
}

TEST_CASE(hue_saturation_ranges_match_photoshop) {
    // Per-range hue, saturation and lightness, two ranges at once, and a range under the master. Within two levels but
    // for the feathered edges of a range: there Photoshop weighs a colour as if its hue were up to three-quarters of a
    // degree lower than it is (Patchy measured the same and found no rule).
    const Apart a = againstPhotoshop("photoshop-hue-saturation-bands.psd", "photoshop-hue-saturation-bands.bmp");
    if (a.worst < 0) { std::printf("  skipped: Patchy's fixtures are not beside this checkout\n"); return; }
    CHECK(a.worst <= 7);
    CHECK(a.mean < 0.15);
    CHECK(a.overTwo < 0.025);
}

TEST_CASE(color_balance_matches_photoshop) {
    const Apart midtones = againstPhotoshop("photoshop-color-balance.psd", nullptr);
    const Apart all = againstPhotoshop("photoshop-color-balance-full.psd", nullptr);
    if (midtones.worst < 0 || all.worst < 0) { std::printf("  skipped: Patchy's fixtures are not beside this checkout\n"); return; }
    CHECK(midtones.worst <= 1);
    CHECK(all.worst <= 1);   // shadows, midtones and highlights with Preserve Luminosity
}

TEST_CASE(color_balance_with_preserve_luminosity_moves_the_other_channels) {
    // One slider towards red in the shadows leaves red alone and darkens green and blue there; towards cyan it darkens red.
    auto run = [](int range, double cyanRed, bool preserve, uint8_t value) {
        ColorBalanceSettings s;
        s.ranges[size_t(range)] = {cyanRed, 0, 0};
        s.preserveLuminosity = preserve;
        Image img(1, 1);
        uint8_t* p = img.pixel(0, 0);
        p[0] = p[1] = p[2] = value;
        p[3] = 255;
        applyColorBalance(img, s);
        return std::array<int, 3>{img.pixel(0, 0)[0], img.pixel(0, 0)[1], img.pixel(0, 0)[2]};
    };
    auto shadowsRed = run(0, 40, true, 100);
    CHECK_EQ(shadowsRed[0], 100);
    CHECK_EQ(shadowsRed[1], int(std::lround((100 - 40) * 255.0 / 215)));
    CHECK_EQ(shadowsRed[1], shadowsRed[2]);
    auto shadowsCyan = run(0, -40, true, 100);
    CHECK_EQ(shadowsCyan[0], int(std::lround((100 - 40) * 255.0 / 215)));
    CHECK_EQ(shadowsCyan[1], 100);
    // Highlights towards red lift red's white point; towards cyan, green's and blue's.
    auto highlightsRed = run(2, 30, true, 200);
    CHECK_EQ(highlightsRed[0], int(std::lround(200 * 255.0 / 225)));
    CHECK_EQ(highlightsRed[1], 200);
    auto highlightsCyan = run(2, -30, true, 200);
    CHECK_EQ(highlightsCyan[0], 200);
    CHECK_EQ(highlightsCyan[1], int(std::lround(200 * 255.0 / 225)));
    // Midtones: red's gamma 2^-(v / 200) and the others' 2^(v / 200); without Preserve Luminosity red's alone, 2^-(v / 100).
    auto midtones = run(1, 50, true, 128);
    CHECK_EQ(midtones[0], int(std::lround(255 * std::pow(128 / 255.0, std::exp2(-0.25)))));
    CHECK_EQ(midtones[1], int(std::lround(255 * std::pow(128 / 255.0, std::exp2(0.25)))));
    auto plain = run(1, 50, false, 128);
    CHECK_EQ(plain[0], int(std::lround(255 * std::pow(128 / 255.0, std::exp2(-0.5)))));
    CHECK_EQ(plain[1], 128);
}

TEST_MAIN()
