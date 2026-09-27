// The Mosh effects (compositor/mosh.h, docs/mosh.md): the registry, the runtime's maths, Pixel Sort's semantics, render
// fingerprints for every effect at its defaults and at seeded settings (tests/mosh_hashes.txt), the same pixels on one
// thread and on the pool, and 16-bit against 8-bit.
//
// COMPOSITOR_UPDATE_MOSH_HASHES=1 rewrites the fingerprints after an intentional change.
//
// Also a tool for checking the ports against OpenMosh's own renders:
//   mosh_tests --run <in.png> <effect-id> <out.png> [seed=<n>] [<key>=<value> ...]
#include "check.h"
#include "compositor/depth.h"
#include "compositor/mosh.h"
#include "compositor/parallel.h"
#include "compositor/png.h"
#include "mosh_runtime.h"

#include <algorithm>
#include <cinttypes>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <sstream>
#include <string>

using namespace compositor;

namespace {

struct Fnv {
    uint64_t h = 1469598103934665603ull;
    void bytes(const uint8_t* p, size_t n) { for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ull; } }
};

uint64_t hashImage(const Image& image) {
    Fnv f;
    for (int y = 0; y < image.height(); y++) f.bytes(image.row(y), size_t(image.width()) * 4);
    return f.h;
}

uint32_t mix(uint32_t v) { v ^= v >> 16; v *= 0x7feb352du; v ^= v >> 15; v *= 0x846ca68bu; v ^= v >> 16; return v; }

/// An opaque photo stand-in: smooth gradients, a few hard shapes and some noise, so every effect has edges, flat
/// areas and every luma to work on.
std::shared_ptr<Image> testImage(int w, int h) {
    auto img = std::make_shared<Image>(w, h);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint32_t n = mix(uint32_t(y * w + x) * 0x9E3779B1u);
            int r = 255 * x / (w - 1), g = 255 * y / (h - 1), b = 255 - 255 * (x + y) / (w + h - 2);
            const int cx = x - w / 3, cy = y - h / 2;
            if (cx * cx + cy * cy < (h / 4) * (h / 4)) { r = 240; g = 200; b = 40; }
            if (x > w * 2 / 3 && y > h / 5 && y < h * 3 / 5) { r = 20; g = 30; b = 90; }
            uint8_t* p = img->pixel(x, y);
            p[0] = uint8_t(std::clamp(r + int(n & 15) - 8, 0, 255));
            p[1] = uint8_t(std::clamp(g + int((n >> 8) & 15) - 8, 0, 255));
            p[2] = uint8_t(std::clamp(b + int((n >> 16) & 15) - 8, 0, 255));
            p[3] = 255;
        }
    return img;
}

/// The same with a transparent corner and a half-transparent band, for the premultiplied boundary.
std::shared_ptr<Image> translucentImage(int w, int h) {
    auto img = testImage(w, h);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            unsigned a = x < w / 4 && y < h / 4 ? 0 : y > h * 3 / 4 ? 128 : 255;
            uint8_t* p = img->pixel(x, y);
            for (int c = 0; c < 4; c++) p[c] = uint8_t((p[c] * a + 127) / 255);
        }
    return img;
}

/// Non-default settings for each effect: every seeded one on another seed, and the switches flipped.
mosh::Settings variant(const mosh::EffectSpec& spec) {
    mosh::Settings s = mosh::Settings::defaults(spec);
    s.seed = 42.125f;
    for (size_t i = 0; i < spec.params.size(); i++) {
        const mosh::ParamSpec& p = spec.params[i];
        if (p.kind == mosh::ParamKind::Bool) s.values[i] = 1 - p.defaultValue;
        else if (p.kind == mosh::ParamKind::Choice) s.values[i] = std::fmod(p.defaultValue + 1, p.max + 1);
        else s.values[i] = p.min + (p.max - p.min) * 0.7f;
    }
    if (spec.id == "strobe") s.set("phase", 0.6f);   // time is 0: the flash shows from phase 0.5
    return s;
}

std::shared_ptr<Image> run(const mosh::Settings& s, const Image& source) {
    auto out = std::make_shared<Image>(source);
    if (!mosh::apply(s, *out)) return nullptr;
    return out;
}

/// Runs `body` with nested parallel loops serial (the caller marks itself as inside a loop).
template <class F>
auto serially(F body) {
    decltype(body()) result{};
    parallelFor(0, 2, 1, [&](int y0, int) { if (y0 == 0) result = body(); });
    return result;
}

/// Effects whose output quantises the input's colour or luma (a threshold, a palette, a sort order, a rounding), so a
/// 16-bit input a fraction of a level off an 8-bit one may land on the other side and move a pixel by more than a level.
const std::set<std::string_view> quantising = {"pixel-sort", "cga-8bit", "dither", "halftone", "dot-screen"};

} // namespace

TEST_CASE(registry_is_openmosh_shaped) {
    std::set<std::string_view> ids;
    for (const auto& e : mosh::effects()) {
        CHECK(ids.insert(e.id).second);
        CHECK(!e.params.empty());
        CHECK(e.params.size() <= 8);
        CHECK(mosh::findEffect(e.id) == &e);
        std::set<std::string_view> keys;
        for (const auto& p : e.params) {
            CHECK(keys.insert(p.key).second);
            CHECK(p.min <= p.defaultValue && p.defaultValue <= p.max);
            if (p.kind == mosh::ParamKind::Choice) CHECK_EQ(p.max, float(p.options.size() - 1));
        }
    }
    CHECK_EQ(mosh::effects().size(), size_t(20));
    CHECK(mosh::findEffect("blur") == nullptr);   // NekoPhoto's own Gaussian Blur covers it
    const auto* sort = mosh::findEffect("pixel-sort");
    REQUIRE(sort);
    CHECK(sort->seeded);
    CHECK_EQ(std::string(sort->params[1].label), std::string("Threshold High"));
    CHECK_NEAR(sort->params[1].defaultValue, 0.85, 1e-7);
}

TEST_CASE(settings_normalize) {
    mosh::Settings s = mosh::Settings::defaults(*mosh::findEffect("strobe"));
    s.values = {7, NAN, 1.6f};
    s.seed = 250.5f;
    mosh::Settings n = s.normalized();
    CHECK_EQ(n.values.size(), size_t(3));
    CHECK_EQ(n.values[0], 1.0f);
    CHECK_EQ(n.values[1], 10.0f);   // NaN falls back to the default
    CHECK_EQ(n.values[2], 2.0f);    // a choice rounds to an option
    CHECK_NEAR(n.seed, 50.5, 1e-4);
    CHECK(n.set("rate", 3));
    CHECK_EQ(n.value("rate"), 3.0f);
    CHECK(!n.set("nope", 1));
    Image img(4, 4);
    mosh::Settings unknown;
    unknown.effect = "not-an-effect";
    CHECK(!mosh::apply(unknown, img));
    for (uint32_t i = 0; i < 1000; i++) { float v = mosh::seedFrom(i); CHECK(v >= 0 && v < 100); }
}

TEST_CASE(runtime_maths) {
    using namespace mosh::rt;
    for (double x = -9500; x < 9500; x += 0.37) {
        CHECK_NEAR(sinD(x), std::sin(x), 1e-11);
        CHECK_NEAR(cosD(x), std::cos(x), 1e-11);
    }
    for (double y = -3; y <= 3; y += 0.25)
        for (double x = -3; x <= 3; x += 0.25) CHECK_NEAR(atan2D(y, x), std::atan2(y, x), 1e-14);
    // WGSL's semantics where C++ differs.
    CHECK_EQ(fract(-0.25f), 0.75f);
    CHECK_EQ(toI32(-1.5f), -1);
    CHECK_EQ(toU32(-3.0f), 0u);
    CHECK_EQ(mix(2.0f, 4.0f, 0.5f), 3.0f);
    CHECK_EQ(smoothstep(0.0f, 1.0f, 0.5f), 0.5f);
    // pcg2d against an independent evaluation of the shader's steps, and rand2's range.
    uint32_t x = 0, y = 0;
    pcg2d(x, y);
    CHECK_EQ(x, 0x18e431a7u);
    CHECK_EQ(y, 0x055df4d1u);
    x = 1, y = bitsOf(42.0f);
    pcg2d(x, y);
    CHECK_EQ(x, 0xed40af49u);
    CHECK_EQ(y, 0x5fc959f5u);
    float lo = 1, hi = 0;
    for (int i = 0; i < 20000; i++) { float r = rand2(vec2{float(i), 0.5f}); lo = std::min(lo, r); hi = std::max(hi, r); }
    CHECK(lo >= 0 && lo < 0.01f && hi <= 1 && hi > 0.99f);
    // HSV round trip.
    vec3 c{0.2f, 0.7f, 0.4f};
    vec3 back = hsv2rgb(rgb2hsv(c));
    CHECK_NEAR(back.x, c.x, 1e-5);
    CHECK_NEAR(back.y, c.y, 1e-5);
    CHECK_NEAR(back.z, c.z, 1e-5);
    // The sampler: texel centres read back exactly, halfway between two texels averages them, clamp holds the edge.
    Frame f(2, 1);
    f.store(0, 0, vec4{0, 0, 0, 1});
    f.store(1, 0, vec4{1, 1, 1, 1});
    CHECK_EQ(sample(f, vec2{0.25f, 0.5f}).x, 0.0f);
    CHECK_EQ(sample(f, vec2{0.75f, 0.5f}).x, 1.0f);
    CHECK_NEAR(sample(f, vec2{0.5f, 0.5f}).x, 0.5, 1e-6);
    CHECK_EQ(sample(f, vec2{-3.0f, 0.5f}).x, 0.0f);
    CHECK_EQ(sample(f, vec2{1.0f, 0.5f}, Address::Repeat).x, 0.5f);
    CHECK_EQ(sampleNearest(f, vec2{1.25f, 0.5f}, Address::Repeat).x, 0.0f);
}

TEST_CASE(pixel_sort_sorts_runs_between_barriers) {
    // A row of greys: the ones inside [low, high] sort by luma within their runs; the others stay put.
    const int w = 600;
    Image img(w, 3);
    for (int y = 0; y < 3; y++)
        for (int x = 0; x < w; x++) {
            uint8_t v = uint8_t(mix(uint32_t(x * 7 + y * 1031)) & 255);
            uint8_t* p = img.pixel(x, y);
            p[0] = p[1] = p[2] = v;
            p[3] = 255;
        }
    Image original = img;
    mosh::Settings s = mosh::Settings::defaults(*mosh::findEffect("pixel-sort"));
    s.seed = 7;
    REQUIRE(mosh::apply(s, img));
    for (int y = 0; y < 3; y++) {
        std::multiset<int> before, after;
        int descents = 0;
        for (int x = 0; x < w; x++) {
            int a = original.pixel(x, y)[0], b = img.pixel(x, y)[0];
            before.insert(a);
            after.insert(b);
            const float la = a / 255.0f;
            if (la < 0.25f || la > 0.85f) CHECK_EQ(b, a);   // barriers never move
            if (x > 0) {
                const int prev = img.pixel(x - 1, y)[0];
                const bool bothIn = prev / 255.0f >= 0.25f && prev / 255.0f <= 0.85f && b / 255.0f >= 0.25f && b / 255.0f <= 0.85f;
                if (bothIn && b < prev) descents++;
            }
        }
        CHECK(before == after);
        CHECK(descents <= 3);   // only at the (at most three) segment boundaries in 600 pixels
    }
    // Reverse sorts the other way; vertical works down the columns.
    s.set("reverse", 1);
    Image rev = original;
    REQUIRE(mosh::apply(s, rev));
    CHECK(hashImage(rev) != hashImage(img));
    s.set("vertical", 1);
    Image tall(3, w);
    for (int y = 0; y < w; y++) for (int x = 0; x < 3; x++) std::memcpy(tall.pixel(x, y), original.pixel(y, x), 4);
    REQUIRE(mosh::apply(s, tall));
}

TEST_CASE(fingerprints) {
    const auto base = testImage(193, 131);
    const auto clear = translucentImage(160, 120);
    std::map<std::string, uint64_t> got;
    for (const auto& e : mosh::effects()) {
        const std::string id(e.id);
        mosh::Settings d = mosh::Settings::defaults(e), v = variant(e);
        auto a = run(d, *base), b = run(v, *base), c = run(v, *clear);
        REQUIRE(a && b && c);
        got[id + "/default"] = hashImage(*a);
        got[id + "/variant"] = hashImage(*b);
        got[id + "/translucent"] = hashImage(*c);
        // Premultiplied out: colour never exceeds alpha.
        for (int y = 0; y < c->height(); y++)
            for (int x = 0; x < c->width(); x++) {
                const uint8_t* p = c->pixel(x, y);
                if (p[0] > p[3] || p[1] > p[3] || p[2] > p[3]) { CHECK(false); y = c->height(); break; }
            }
    }
    const std::string path = MOSH_HASHES_FILE;
    if (std::getenv("COMPOSITOR_UPDATE_MOSH_HASHES")) {
        std::ofstream out(path);
        out << "# Mosh render fingerprints (tests/mosh_tests.cpp): FNV-1a 64 of the 8-bit pixels.\n";
        for (const auto& [name, h] : got) { char line[32]; std::snprintf(line, sizeof line, "%016" PRIx64, h); out << name << ' ' << line << '\n'; }
        std::fprintf(stderr, "  wrote %zu fingerprints to %s\n", got.size(), path.c_str());
        return;
    }
    std::ifstream in(path);
    REQUIRE(in.good());
    std::map<std::string, uint64_t> want;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream ss(line);
        std::string name, hex;
        ss >> name >> hex;
        want[name] = std::strtoull(hex.c_str(), nullptr, 16);
    }
    for (const auto& [name, h] : got) {
        auto it = want.find(name);
        if (it == want.end()) { check::fail(__FILE__, __LINE__, "no fingerprint for " + name); continue; }
        if (it->second != h) check::fail(__FILE__, __LINE__, "fingerprint changed: " + name);
    }
    for (const auto& [name, h] : want)
        if (!got.count(name)) check::fail(__FILE__, __LINE__, "fingerprint for a scene that is gone: " + name);
}

TEST_CASE(thread_count_does_not_change_pixels) {
    const auto base = testImage(301, 157);
    for (const auto& e : mosh::effects()) {
        for (const mosh::Settings& s : {mosh::Settings::defaults(e), variant(e)}) {
            auto pooled = run(s, *base);
            uint64_t alone = serially([&] { return hashImage(*run(s, *base)); });
            if (hashImage(*pooled) != alone) check::fail(__FILE__, __LINE__, "thread count changed " + std::string(e.id));
        }
    }
}

TEST_CASE(sixteen_bit_matches_eight_bit) {
    // The 8-bit image widened to 16 bits, run there and narrowed back, against the 8-bit run: within a level where
    // the effect is continuous in its input; the quantising effects are only checked to run.
    const auto base = testImage(160, 112);
    const auto clear = translucentImage(160, 112);
    for (const auto& e : mosh::effects()) {
        for (const auto& source : {base, clear}) {
            for (const mosh::Settings& s : {mosh::Settings::defaults(e), variant(e)}) {
                auto eight = run(s, *source);
                auto deep = widenImage(*source);
                REQUIRE(mosh::apply(s, *deep));
                auto narrowed = narrowImage(*deep);
                if (quantising.count(e.id)) continue;
                int worst = 0;
                for (int y = 0; y < eight->height(); y++)
                    for (int x = 0; x < eight->width() * 4; x++) worst = std::max(worst, std::abs(int(eight->row(y)[x]) - int(narrowed->row(y)[x])));
                if (worst > 1) check::fail(__FILE__, __LINE__, std::string(e.id) + ": 16-bit differs from 8-bit by " + std::to_string(worst) + " levels");
            }
        }
    }
}

namespace {

int runTool(int argc, char** argv) {
    if (argc < 5) { std::fprintf(stderr, "usage: mosh_tests --run <in.png> <effect-id> <out.png> [seed=<n>] [<key>=<value> ...]\n"); return 2; }
    const mosh::EffectSpec* spec = mosh::findEffect(argv[3]);
    if (!spec) { std::fprintf(stderr, "unknown effect %s\n", argv[3]); return 2; }
    std::string error;
    auto image = readPngImage(argv[2], &error);
    if (!image) { std::fprintf(stderr, "%s\n", error.c_str()); return 1; }
    mosh::Settings s = mosh::Settings::defaults(*spec);
    for (int i = 5; i < argc; i++) {
        std::string arg = argv[i];
        auto eq = arg.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = arg.substr(0, eq);
        const float value = std::strtof(arg.c_str() + eq + 1, nullptr);
        if (key == "seed") s.seed = value;
        else if (!s.set(key, value)) { std::fprintf(stderr, "unknown parameter %s\n", key.c_str()); return 2; }
    }
    mosh::apply(s, *image);
    return writePngImage(argv[4], *image) ? 0 : 1;
}

} // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::string(argv[1]) == "--run") return runTool(argc, argv);
    return check::run(argc, argv);
}
