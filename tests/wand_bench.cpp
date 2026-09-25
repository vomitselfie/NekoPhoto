// The Magic Wand benchmark: synthetic scenes with known answers, one click each, every method scored at
// the default tolerance, at its best tolerance, and by how wide a tolerance range gets it right (how easy
// the slider is to set). Pixels within a pixel of a true boundary are not scored (antialiasing).
//
//   wand_bench            the table
//   wand_bench dump DIR   also writes each scene, its answer and each method's selection as PNGs
#include "compositor/png.h"
#include "compositor/smartwand.h"
#include "compositor/wand.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <functional>
#include <random>
#include <string>
#include <vector>

using namespace compositor;

namespace {

struct Scene {
    std::string name;
    Image image{1, 1};
    GrayImage truth{1, 1};    // 255 should be selected, 0 not
    GrayImage scored{1, 1};   // 255 where the pixel counts
    int x = 0, y = 0;         // the click
};

const int S = 256;

void put(Image& im, int x, int y, double r, double g, double b, double a = 255) {
    uint8_t* p = im.pixel(x, y);
    auto q = [&](double v) { return uint8_t(std::clamp(std::lround(v * a / 255), 0L, 255L)); };
    p[0] = q(r); p[1] = q(g); p[2] = q(b); p[3] = uint8_t(std::clamp(std::lround(a), 0L, 255L));
}

/// A scene from a per-pixel colour function and a truth function; coverage at the boundary is antialiased
/// by 4x4 supersampling of `inside`, and pixels whose supersamples disagree are not scored.
Scene make(const std::string& name, int cx, int cy, std::function<bool(double, double)> inside,
           std::function<void(double, double, double[3])> in, std::function<void(double, double, double[3])> out) {
    Scene s;
    s.name = name; s.x = cx; s.y = cy;
    s.image = Image(S, S); s.truth = GrayImage(S, S, 0); s.scored = GrayImage(S, S, 255);
    for (int y = 0; y < S; y++)
        for (int x = 0; x < S; x++) {
            int hits = 0;
            for (int j = 0; j < 4; j++) for (int i = 0; i < 4; i++) hits += inside(x + (i + 0.5) / 4, y + (j + 0.5) / 4);
            double a[3], b[3];
            in(x + 0.5, y + 0.5, a); out(x + 0.5, y + 0.5, b);
            const double f = hits / 16.0;
            put(s.image, x, y, a[0] * f + b[0] * (1 - f), a[1] * f + b[1] * (1 - f), a[2] * f + b[2] * (1 - f));
            s.truth.at(x, y) = hits >= 8 ? 255 : 0;
            if (hits != 0 && hits != 16) s.scored.at(x, y) = 0;
        }
    // A pixel either side of the true boundary is antialiasing, not scored.
    GrayImage grown = s.scored;
    for (int y = 0; y < S; y++) for (int x = 0; x < S; x++)
        if (s.scored.at(x, y) == 0) for (int j = -1; j <= 1; j++) for (int i = -1; i <= 1; i++) { int X = x + i, Y = y + j; if (X >= 0 && Y >= 0 && X < S && Y < S) grown.at(X, Y) = 0; }
    s.scored = grown;
    return s;
}

void noise(Scene& s, double sigma, unsigned seed, bool blocks = false) {
    std::mt19937 rng(seed);
    std::normal_distribution<double> n(0, sigma);
    std::vector<double> block(size_t((S / 8) * (S / 8) * 3));
    for (double& b : block) b = n(rng) * 0.6;
    for (int y = 0; y < S; y++) for (int x = 0; x < S; x++) {
        uint8_t* p = s.image.pixel(x, y);
        for (int c = 0; c < 3; c++) {
            double v = p[c] + n(rng) * (blocks ? 0.5 : 1) + (blocks ? block[size_t(((y / 8) * (S / 8) + x / 8) * 3 + c)] : 0);
            p[c] = uint8_t(std::clamp(std::lround(v), 0L, 255L));
        }
    }
}

std::vector<Scene> scenes() {
    std::vector<Scene> out;
    auto flat = [](double r, double g, double b) { return [=](double, double, double c[3]) { c[0] = r; c[1] = g; c[2] = b; }; };
    // 1. A flat fill inside antialiased line art (a ring 3 px wide): the fill, up to the line.
    {
        auto ring = [](double x, double y) { double d = std::hypot(x - 128, y - 128); return d < 90; };
        Scene s = make("flat fill in line art", 128, 128, ring, flat(240, 200, 170), flat(255, 255, 255));
        // Draw the line over the boundary, antialiased.
        for (int y = 0; y < S; y++) for (int x = 0; x < S; x++) {
            double d = std::fabs(std::hypot(x + 0.5 - 128, y + 0.5 - 128) - 91.5);
            double cover = std::clamp(2.0 - d, 0.0, 1.0);
            if (cover <= 0) continue;
            uint8_t* p = s.image.pixel(x, y);
            for (int c = 0; c < 3; c++) p[c] = uint8_t(p[c] * (1 - cover) + 20 * cover);
            s.scored.at(x, y) = 0;
        }
        out.push_back(s);
    }
    // 2. Two colours only 20 levels apart, with a sharp edge: the clicked side only.
    out.push_back(make("near colours, sharp edge", 64, 128, [](double x, double) { return x < 128; }, flat(120, 140, 170), flat(135, 155, 185)));
    // 3. One region with a gentle brightness ramp (60 levels across): all of it, over a clearly different ground.
    out.push_back(make("shaded region", 128, 128, [](double x, double y) { return std::hypot(x - 128, y - 128) < 100; },
                       [](double x, double, double c[3]) { double t = (x - 28) / 200; c[0] = 230 - 60 * t; c[1] = 180 - 50 * t; c[2] = 150 - 40 * t; },
                       flat(60, 110, 200)));
    // 4. Soft skin against a similar warm ground with no line: the disc.
    out.push_back(make("skin on warm ground", 128, 120, [](double x, double y) { return std::hypot(x - 128, y - 128) < 80; },
                       [](double x, double y, double c[3]) { double t = std::hypot(x - 110, y - 100) / 110; c[0] = 245 - 40 * t; c[1] = 205 - 45 * t; c[2] = 180 - 45 * t; },
                       flat(200, 150, 110)));
    // 5. Noise (sigma 8) on the line-art fill.
    { Scene s = out[0]; s.name = "noisy fill in line art"; noise(s, 8, 1); out.push_back(s); }
    // 6. JPEG-like blocks and noise on the near-colour edge.
    { Scene s = out[1]; s.name = "blocky near colours"; noise(s, 6, 2, true); out.push_back(s); }
    // 7. A textured region (two colours alternating in 2 px stripes, 50 apart) on a flat ground of the same
    //    mean colour: the texture.
    out.push_back(make("texture on its mean colour", 128, 128, [](double x, double y) { return std::fabs(x - 128) < 70 && std::fabs(y - 128) < 70; },
                       [](double x, double, double c[3]) { bool a = int(x / 2) % 2; c[0] = a ? 150 : 100; c[1] = a ? 150 : 100; c[2] = a ? 150 : 100; },
                       flat(125, 125, 125)));
    // 8. A small enclosed region: a 12 px square of a slightly different colour inside a big one.
    out.push_back(make("small region", 128, 128, [](double x, double y) { return std::fabs(x - 128) < 6 && std::fabs(y - 128) < 6; }, flat(90, 160, 90), flat(70, 140, 70)));
    // 9. A shadowed object: a strong ramp (100 levels) across it, a background 45 levels away in hue.
    out.push_back(make("shadowed object", 90, 128, [](double x, double y) { return std::fabs(x - 128) < 90 && std::fabs(y - 128) < 60; },
                       [](double x, double, double c[3]) { double t = (x - 38) / 180; c[0] = 200 - 100 * t; c[1] = 60 - 30 * t; c[2] = 60 - 30 * t; },
                       flat(170, 110, 60)));
    // 10. A texture atlas piece: dark cloth with a thin red grid every 24 px, on grey. The click lands inside
    //     a grid cell; the answer is the whole piece, grid lines included.
    out.push_back(make("grid-lined piece on grey", 131, 129, [](double x, double y) { return std::fabs(x - 128) < 90 && std::fabs(y - 128) < 70; },
                       [](double x, double y, double c[3]) { bool line = int(x) % 24 == 0 || int(y) % 24 == 0; c[0] = line ? 150 : 40; c[1] = line ? 30 : 25; c[2] = line ? 40 : 30; },
                       flat(100, 100, 100)));
    return out;
}

struct Score { double iouDefault, iouBest; int bestTolerance, goodRange; double ms; };

double iou(const GrayImage& sel, const Scene& s) {
    long both = 0, either = 0;
    for (int y = 0; y < S; y++) for (int x = 0; x < S; x++) {
        if (!s.scored.at(x, y)) continue;
        bool a = sel.at(x, y) >= 128, b = s.truth.at(x, y) >= 128;
        both += a && b; either += a || b;
    }
    return either ? double(both) / double(either) : 1.0;
}

using Method = std::function<std::function<void(int, GrayImage&)>(const Scene&)>;   // prepare once, then select at a tolerance

} // namespace

int main(int argc, char** argv) {
    if (argc > 2 && std::string(argv[1]) == "time") {
        // wand_bench time image.png: the preparation and one click's field at the defaults, on a real image.
        std::string error;
        auto image = readPngImage(argv[2], &error);
        if (!image) { std::fprintf(stderr, "%s\n", error.c_str()); return 1; }
        auto t0 = std::chrono::steady_clock::now();
        SmartWandImage prepared(*image);
        auto t1 = std::chrono::steady_clock::now();
        auto field = prepared.propagate(image->width() / 2, image->height() / 2, 2, 255);
        auto t2 = std::chrono::steady_clock::now();
        GrayImage m(image->width(), image->height(), 0);
        thresholdWandField(field, 32, true, m);
        auto t3 = std::chrono::steady_clock::now();
        auto ms = [](auto a, auto b) { return std::chrono::duration<double, std::milli>(b - a).count(); };
        std::printf("%dx%d: prepare %.0f ms, field %.0f ms, threshold %.1f ms\n", image->width(), image->height(), ms(t0, t1), ms(t1, t2), ms(t2, t3));
        return 0;
    }
    const bool dump = argc > 2 && std::string(argv[1]) == "dump";
    const std::string dir = dump ? argv[2] : "";
    std::vector<std::pair<std::string, Method>> methods;
    methods.push_back({"classic", [](const Scene& s) {
        return [&s](int t, GrayImage& m) { wandMask(s.image, s.x, s.y, 1, t, true, m); };
    }});
    struct Variant { const char* label; double edge, neighbour, seed, texture, region = 0; };
    for (Variant v : {Variant{"B seed only", 0, 0, 1, 0}, Variant{"shipped (M1)", 0, 8, 0.7, 2, 0}, Variant{"M3 region 1", 0, 8, 0.7, 2, 1}, Variant{"M3 region 2", 0, 8, 0.7, 2, 2}}) {
        methods.push_back({v.label, [v](const Scene& s) {
            auto image = std::make_shared<SmartWandImage>(s.image, v.edge > 0);
            SmartWandOptions o; o.edgeWeight = v.edge; o.neighbourWeight = v.neighbour; o.seedWeight = v.seed; o.textureWeight = v.texture; o.regionWeight = v.region;
            auto field = std::make_shared<SmartWandImage::Field>(image->propagate(s.x, s.y, 2, 300, o));
            return [field](int t, GrayImage& m) { thresholdWandField(*field, t, true, m); };
        }});
    }
    const auto all = scenes();
    std::printf("%-26s %-22s %8s %8s %6s %7s %8s\n", "scene", "method", "IoU@32", "best", "at", "range", "ms");
    std::vector<double> sumDefault(methods.size(), 0), sumBest(methods.size(), 0), sumRange(methods.size(), 0);
    for (const Scene& s : all) {
        if (dump) { writePngImage(dir + "/" + s.name + ".png", s.image, 72, nullptr); }
        for (size_t k = 0; k < methods.size(); k++) {
            auto t0 = std::chrono::steady_clock::now();
            auto select = methods[k].second(s);
            GrayImage m(S, S, 0);
            select(32, m);
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            Score sc{iou(m, s), 0, 0, 0, ms};
            if (dump) {
                Image view(S, S);
                for (int y = 0; y < S; y++) for (int x = 0; x < S; x++) { uint8_t v = m.at(x, y); uint8_t* p = view.pixel(x, y); p[0] = p[1] = p[2] = v; p[3] = 255; }
                writePngImage(dir + "/" + s.name + " - " + methods[k].first + ".png", view, 72, nullptr);
            }
            for (int t = 0; t <= 255; t += 2) {
                select(t, m);
                const double v = iou(m, s);
                if (v > sc.iouBest) { sc.iouBest = v; sc.bestTolerance = t; }
                if (v >= 0.95) sc.goodRange += 2;
            }
            sumDefault[k] += sc.iouDefault; sumBest[k] += sc.iouBest; sumRange[k] += sc.goodRange;
            std::printf("%-26s %-22s %8.3f %8.3f %6d %7d %8.1f\n", s.name.c_str(), methods[k].first.c_str(), sc.iouDefault, sc.iouBest, sc.bestTolerance, sc.goodRange, sc.ms);
        }
    }
    std::printf("\nmean over %zu scenes\n%-22s %8s %8s %7s\n", all.size(), "method", "IoU@32", "best", "range");
    for (size_t k = 0; k < methods.size(); k++)
        std::printf("%-22s %8.3f %8.3f %7.1f\n", methods[k].first.c_str(), sumDefault[k] / all.size(), sumBest[k] / all.size(), sumRange[k] / all.size());
    return 0;
}
