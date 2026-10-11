// The display's precalculated CMYK and Lab transform (displayTransformBetween, colormgmt_display.cpp) against Little
// CMS's float pipeline (transformBetween), which every other path keeps: within one 8-bit level on a dense sweep of
// 16-bit inks and Lab values for several monitors, Lab a = b = 0 exactly neutral, premultiplied pixels, and the cache.
//
// `display_grid_tests --measure [--no-sweep] [--monitor file.icc] [file.psd ...]` prints the error (maximum and mean) on
// the sweep for each monitor and on the given PSDs' renders (CMYK or Lab, any depth: 8-bit files are widened) and, for
// a CMYK file, on the sweep through its own profile, without checking anything. DISPLAY_GRID_WORST=1 lists the pixels
// more than a level off.
#include "check.h"
#include "compositor/colormgmt.h"
#include "compositor/colormodes.h"
#include "compositor/depth.h"
#include "compositor/document.h"
#include "compositor/psd.h"
#include "compositor/render.h"
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

using namespace compositor;

namespace {

struct Monitor {
    const char* name;
    ColorProfile profile;
};

std::vector<Monitor> monitors() {
    return {{"sRGB", srgbProfile()}, {"Adobe RGB", builtinProfile(WorkingSpace::AdobeRGB)}, {"Display P3", builtinProfile(WorkingSpace::DisplayP3)},
            {"ProPhoto", builtinProfile(WorkingSpace::ProPhoto)}};
}

const ConvertOptions displayOptions{RenderingIntent::RelativeColorimetric, true};

/// Opaque 16-bit pixels of `mode`: every `step`th 8-bit code per channel from `offset` (with 255 when the offset is 0),
/// so the codes fall on and between the grid's nodes.
std::vector<uint16_t> sweep(ColorMode mode, int step, int offset) {
    std::vector<int> codes;
    for (int v = offset; v <= 255; v += step) codes.push_back(v);
    if (offset == 0 && codes.back() != 255) codes.push_back(255);
    std::vector<uint16_t> px;
    if (mode == ColorMode::CMYK) {
        for (int c : codes) for (int m : codes) for (int y : codes) for (int k : codes)
            px.insert(px.end(), {widen8(uint8_t(c)), widen8(uint8_t(m)), widen8(uint8_t(y)), widen8(uint8_t(k)), uint16_t(one16)});
    } else {
        for (int l : codes) for (int a : codes) for (int b : codes)
            px.insert(px.end(), {widen8(uint8_t(l)), uint16_t(a * 128), uint16_t(b * 128), uint16_t(one16)});
    }
    return px;
}

uint16_t inverted(double ink) { return uint16_t(std::lround((1.0 - ink) * double(one16))); }

/// Pure inks and their overprints, rich blacks, every K-only grey and the CMY greys (CMYK); every L on the neutral axis,
/// the a and b axes and the corners of the Lab range (Lab).
std::vector<uint16_t> specials(ColorMode mode) {
    std::vector<uint16_t> px;
    const uint16_t one = uint16_t(one16);
    if (mode == ColorMode::CMYK) {
        for (int mask = 0; mask < 16; mask++) px.insert(px.end(), {inverted(mask & 1), inverted((mask >> 1) & 1), inverted((mask >> 2) & 1), inverted((mask >> 3) & 1), one});
        px.insert(px.end(), {inverted(0.6), inverted(0.4), inverted(0.4), inverted(1.0), one});
        px.insert(px.end(), {inverted(0.75), inverted(0.68), inverted(0.67), inverted(0.9), one});
        for (uint32_t k = 0; k <= one16; k++) px.insert(px.end(), {one, one, one, uint16_t(k), one});
        for (uint32_t v = 0; v <= one16; v += 7) px.insert(px.end(), {uint16_t(v), uint16_t(v), uint16_t(v), one, one});
    } else {
        for (uint32_t l = 0; l <= one16; l++) px.insert(px.end(), {uint16_t(l), 16384, 16384, one});
        for (uint32_t l : {0u, 8192u, 16384u, 24576u, 32768u})
            for (uint32_t a : {0u, 32768u})
                for (uint32_t b : {0u, 32768u}) px.insert(px.end(), {uint16_t(l), uint16_t(a), uint16_t(b), one});
        for (uint32_t v = 0; v <= one16; v += 3) {
            px.insert(px.end(), {16384, uint16_t(v), 16384, one});
            px.insert(px.end(), {16384, 16384, uint16_t(v), one});
        }
    }
    return px;
}

struct Error {
    int max = 0;
    double sum = 0;
    size_t samples = 0;
    void add(const uint8_t* a, const uint8_t* b, size_t pixels) {
        for (size_t i = 0; i < pixels * 4; i++) {
            const int d = std::abs(int(a[i]) - int(b[i]));
            max = std::max(max, d);
            sum += d;
        }
        samples += pixels * 4;
    }
    double mean() const { return samples ? sum / double(samples) : 0.0; }
};

/// The precalculated transform's bytes against Little CMS's for `pixels` of `layout`.
Error compare(const ColorTransform& fast, const ColorTransform& exact, const std::vector<uint16_t>& pixels, int channels) {
    const size_t count = pixels.size() / size_t(channels);
    std::vector<uint8_t> a(count * 4), b(count * 4);
    fast.apply(pixels.data(), a.data(), count);
    exact.apply(pixels.data(), b.data(), count);
    Error e;
    e.add(a.data(), b.data(), count);
    if (std::getenv("DISPLAY_GRID_WORST") && e.max > 1)
        for (size_t i = 0; i < count; i++)
            for (int c = 0; c < 3; c++)
                if (std::abs(int(a[i * 4 + size_t(c)]) - int(b[i * 4 + size_t(c)])) > 1) {
                    const uint16_t* p = &pixels[i * size_t(channels)];
                    std::printf("    off by %d at %u %u %u %u: fast %u %u %u exact %u %u %u\n", std::abs(int(a[i * 4 + size_t(c)]) - int(b[i * 4 + size_t(c)])), p[0], p[1], p[2], channels == 5 ? p[3] : 0u, a[i * 4], a[i * 4 + 1], a[i * 4 + 2], b[i * 4], b[i * 4 + 1], b[i * 4 + 2]);
                    break;
                }
    return e;
}

ColorTransformPtr exactOf(ColorMode mode, const ColorProfile& monitor) {
    return transformBetween(ColorProfile(), monitor, displayOptions, pixelFormatFor(SampleType::U16, mode), PixelFormat::RGBA8);
}

ColorTransformPtr fastOf(ColorMode mode, const ColorProfile& monitor) {
    return displayTransformBetween(ColorProfile(), monitor, displayOptions, pixelFormatFor(SampleType::U16, mode));
}

} // namespace

TEST_CASE(cmyk_sweep_within_one_level) {
    for (const Monitor& m : monitors()) {
        auto fast = fastOf(ColorMode::CMYK, m.profile), exact = exactOf(ColorMode::CMYK, m.profile);
        REQUIRE(fast && exact);
        CHECK(fast->precalculated());
        CHECK(!exact->precalculated());
        for (int offset : {0, 4}) {
            const Error e = compare(*fast, *exact, sweep(ColorMode::CMYK, 8, offset), 5);
            std::printf("  CMYK 16 to %s, every 8th code from %d: at most %d, mean %.4f\n", m.name, offset, e.max, e.mean());
            CHECK(e.max <= 1);
        }
        const Error s = compare(*fast, *exact, specials(ColorMode::CMYK), 5);
        std::printf("  CMYK 16 to %s, inks, rich blacks and greys: at most %d, mean %.4f\n", m.name, s.max, s.mean());
        CHECK(s.max <= 1);
    }
}

TEST_CASE(lab_sweep_within_one_level_and_neutral) {
    for (const Monitor& m : monitors()) {
        auto fast = fastOf(ColorMode::Lab, m.profile), exact = exactOf(ColorMode::Lab, m.profile);
        REQUIRE(fast && exact);
        CHECK(fast->precalculated());
        for (int offset : {0, 2}) {
            const Error e = compare(*fast, *exact, sweep(ColorMode::Lab, 4, offset), 4);
            std::printf("  Lab 16 to %s, every 4th code from %d: at most %d, mean %.4f\n", m.name, offset, e.max, e.mean());
            CHECK(e.max <= 1);
        }
        const std::vector<uint16_t> edges = specials(ColorMode::Lab);
        const Error s = compare(*fast, *exact, edges, 4);
        std::printf("  Lab 16 to %s, neutral axis and range edges: at most %d, mean %.4f\n", m.name, s.max, s.mean());
        CHECK(s.max <= 1);
        // Every L with a = b = 0 is drawn grey: red, green and blue equal.
        std::vector<uint16_t> axis;
        for (uint32_t l = 0; l <= one16; l++) axis.insert(axis.end(), {uint16_t(l), 16384, 16384, uint16_t(one16)});
        std::vector<uint8_t> out(axis.size());
        fast->apply(axis.data(), out.data(), axis.size() / 4);
        int coloured = 0;
        for (size_t i = 0; i < out.size(); i += 4) coloured += !(out[i] == out[i + 1] && out[i + 1] == out[i + 2]);
        CHECK_EQ(coloured, 0);
    }
}

TEST_CASE(premultiplied_pixels_match) {
    // Partly transparent pixels are made straight and premultiplied again as Little CMS's path does; clear ones are 0.
    std::mt19937 rng(3);
    for (ColorMode mode : {ColorMode::CMYK, ColorMode::Lab}) {
        const int channels = colorModeChannels(mode);
        std::vector<uint16_t> px;
        for (int i = 0; i < 20000; i++) {
            const uint32_t a = i % 50 == 0 ? 0 : rng() % (one16 + 1);
            for (int c = 0; c < channels - 1; c++) px.push_back(uint16_t((rng() % (one16 + 1)) * a / one16));
            px.push_back(uint16_t(a));
        }
        auto fast = fastOf(mode, srgbProfile()), exact = exactOf(mode, srgbProfile());
        REQUIRE(fast && exact);
        const Error e = compare(*fast, *exact, px, channels);
        std::printf("  %s premultiplied: at most %d, mean %.4f\n", mode == ColorMode::CMYK ? "CMYK" : "Lab", e.max, e.mean());
        CHECK(e.max <= 1);
    }
}

TEST_CASE(other_layouts_and_monitors_stay_exact) {
    // 8-bit layouts (Little CMS precalculates those itself) and RGB documents get Little CMS's transform.
    for (PixelFormat f : {PixelFormat::CMYKA8, PixelFormat::LabA8, PixelFormat::RGBA16}) {
        auto t = displayTransformBetween(ColorProfile(), builtinProfile(WorkingSpace::AdobeRGB), displayOptions, f);
        REQUIRE(t);
        CHECK(!t->precalculated());
    }
    // The same request twice is the same object (the cache); another intent is another transform.
    auto a = fastOf(ColorMode::CMYK, srgbProfile()), b = fastOf(ColorMode::CMYK, srgbProfile());
    CHECK(a == b);
    auto perceptual = displayTransformBetween(ColorProfile(), srgbProfile(), {RenderingIntent::Perceptual, true}, PixelFormat::CMYKA16);
    REQUIRE(perceptual);
    CHECK(perceptual != a);
}

TEST_CASE(canvas_render_through_the_grid) {
    // A 16-bit CMYK document drawn through RenderOptions::display: the precalculated transform gives the exact one's
    // frame within a level.
    Document doc(97, 61);
    doc.colorMode = ColorMode::CMYK;
    doc.sampleType = SampleType::U16;
    auto image = std::make_shared<Image16>(97, 61, 5);
    std::mt19937 rng(5);
    for (int y = 0; y < 61; y++) {
        uint16_t* p = image->row(y);
        for (int x = 0; x < 97 * 5; x++) p[x] = x % 5 == 4 ? uint16_t(one16) : uint16_t(rng() % (one16 + 1));
    }
    doc.layers.push_back(Layer(Asset::make(Image16Ptr(image), "inks"), Point(0, 0)));
    auto fast = fastOf(ColorMode::CMYK, srgbProfile()), exact = exactOf(ColorMode::CMYK, srgbProfile());
    Image a, b;
    RenderOptions o;
    o.display = fast.get();
    render(doc, o, a);
    o.display = exact.get();
    render(doc, o, b);
    REQUIRE(a.width() == b.width() && a.height() == b.height());
    Error e;
    for (int y = 0; y < a.height(); y++) e.add(a.row(y), b.row(y), size_t(a.width()));
    CHECK(e.max <= 1);
}

namespace {

/// Prints the error on the sweep for each monitor (and a monitor profile file, when given with --monitor), and on the
/// renders of the given PSDs.
int measure(int argc, char** argv) {
    std::vector<Monitor> list = monitors();
    std::vector<std::string> files;
    bool sweeps = true;
    for (int i = 2; i < argc; i++) {
        if (std::strcmp(argv[i], "--no-sweep") == 0) sweeps = false;
        else if (std::strcmp(argv[i], "--monitor") == 0 && i + 1 < argc) {
            FILE* f = std::fopen(argv[++i], "rb");
            if (!f) continue;
            std::vector<uint8_t> bytes;
            for (int c; (c = std::fgetc(f)) != EOF;) bytes.push_back(uint8_t(c));
            std::fclose(f);
            if (auto p = profileFromIcc(bytes)) list.push_back({argv[i], *p});
        } else files.push_back(argv[i]);
    }
    for (const Monitor& m : sweeps ? list : std::vector<Monitor>()) {
        for (ColorMode mode : {ColorMode::CMYK, ColorMode::Lab}) {
            auto fast = fastOf(mode, m.profile), exact = exactOf(mode, m.profile);
            if (!fast || !exact) continue;
            const int channels = colorModeChannels(mode);
            const int step = mode == ColorMode::CMYK ? 8 : 4;
            for (int offset : {0, step / 2}) {
                const Error e = compare(*fast, *exact, sweep(mode, step, offset), channels);
                std::printf("%-12s %s every %dth code from %d: max %d mean %.5f\n", m.name, mode == ColorMode::CMYK ? "CMYK" : "Lab ", step, offset, e.max, e.mean());
            }
            const Error s = compare(*fast, *exact, specials(mode), channels);
            std::printf("%-12s %s specials: max %d mean %.5f%s\n", m.name, mode == ColorMode::CMYK ? "CMYK" : "Lab ", s.max, s.mean(), fast->precalculated() ? "" : " (not precalculated)");
        }
    }
    for (const std::string& file : files) {
        std::string why;
        auto imported = importPsd(file, &why);
        if (!imported) { std::printf("%s: %s\n", file.c_str(), why.c_str()); continue; }
        Document& doc = imported->document;
        if (doc.colorMode == ColorMode::RGB) { std::printf("%s: RGB, skipped\n", file.c_str()); continue; }
        const AnyImage native = renderNative(doc);
        const AnyImage deep = imageAtFormat(native, SampleType::U16, doc.colorMode);
        if (!deep || !deep.u16()) { std::printf("%s: no 16-bit render\n", file.c_str()); continue; }
        for (const Monitor& m : list) {
            const PixelFormat layout = pixelFormatFor(SampleType::U16, doc.colorMode);
            if (sweeps && doc.colorMode == ColorMode::CMYK && !doc.profile.empty()) {
                // The file's own CMYK profile on the sweep too.
                auto fast = displayTransformBetween(doc.profile, m.profile, displayOptions, layout);
                auto exact = transformBetween(doc.profile, m.profile, displayOptions, layout, PixelFormat::RGBA8);
                for (int offset : {0, 4}) {
                    const Error e = compare(*fast, *exact, sweep(ColorMode::CMYK, 8, offset), 5);
                    std::printf("%s's profile (%s) to %s, sweep from %d: max %d mean %.5f\n", file.c_str(), doc.profile.description.c_str(), m.name, offset, e.max, e.mean());
                }
            }
            auto fast = displayTransformBetween(doc.profile, m.profile, displayOptions, layout);
            auto exact = transformBetween(doc.profile, m.profile, displayOptions, layout, PixelFormat::RGBA8);
            if (!fast || !exact) continue;
            Image a, b;
            convertImageTo8(deep, a, *fast);
            convertImageTo8(deep, b, *exact);
            Error e;
            for (int y = 0; y < a.height(); y++) e.add(a.row(y), b.row(y), size_t(a.width()));
            std::printf("%s (%dx%d %s %s) to %s: max %d mean %.5f%s\n", file.c_str(), doc.width, doc.height, doc.colorMode == ColorMode::CMYK ? "CMYK" : "Lab",
                        doc.sampleType == SampleType::U16 ? "16-bit" : "8-bit widened", m.name, e.max, e.mean(), fast->precalculated() ? "" : " (not precalculated)");
        }
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "--measure") == 0) return measure(argc, argv);
    return check::run(argc, argv);
}
