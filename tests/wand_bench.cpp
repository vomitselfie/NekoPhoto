// The Magic Wand benchmark: synthetic scenes with known answers, one click each, every method scored at
// the default tolerance, at its best tolerance, and by how wide a tolerance range gets it right (how easy
// the slider is to set). Pixels within a pixel of a true boundary are not scored (antialiasing).
//
//   wand_bench            the table
//   wand_bench dump DIR   also writes each scene, its answer and each method's selection as PNGs
#include "compositor/png.h"
#include "compositor/smartwand.h"
#include "compositor/wand.h"
#include "compositor/morphology.h"
#include "compositor/selection.h"
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
        auto field = prepared.propagate(image->width() / 2, image->height() / 2, 2, wandCost(255));
        auto t2 = std::chrono::steady_clock::now();
        GrayImage m(image->width(), image->height(), 0);
        thresholdWandField(field, 32, true, m);
        auto t3 = std::chrono::steady_clock::now();
        auto ms = [](auto a, auto b) { return std::chrono::duration<double, std::milli>(b - a).count(); };
        std::printf("%dx%d: prepare %.0f ms, field %.0f ms, threshold %.1f ms\n", image->width(), image->height(), ms(t0, t1), ms(t1, t2), ms(t2, t3));
        return 0;
    }
    if (argc > 4 && std::string(argv[1]) == "curve") {
        // wand_bench curve image.png x y: selected pixels against tolerance, classic and edge-aware, for one click.
        std::string error;
        auto image = readPngImage(argv[2], &error);
        if (!image) { std::fprintf(stderr, "%s\n", error.c_str()); return 1; }
        const int x = std::atoi(argv[3]), y = std::atoi(argv[4]);
        SmartWandImage prepared(*image);
        auto field = prepared.propagate(x, y, 1, wandCost(255));
        GrayImage m(image->width(), image->height(), 0);
        std::printf("%9s %12s %12s\n", "tolerance", "classic", "edge-aware");
        for (int t : {0, 4, 8, 12, 16, 20, 24, 32, 40, 48, 64, 80, 100, 128, 160, 200, 255}) {
            const long classic = wandMask(*image, x, y, 1, t, true, m);
            const long smart = thresholdWandField(field, t, false, m);
            std::printf("%9d %12ld %12ld\n", t, classic, smart);
        }
        return 0;
    }
    if (argc > 1 && std::string(argv[1]) == "edges") {
        // Line art on a coloured ground, antialiased and smudged the way AI renders often are: the background is
        // clicked and cleared, and what is left is compared with the true line (black, alpha = its coverage).
        const int N = 384;
        const double lr = 20, lg = 20, lb = 25, br = 60, bg = 110, bb = 220;
        GrayImage truth(N, N, 0);
        {
            std::vector<float> cov(size_t(N) * N, 0.0f);
            for (int y = 0; y < N; y++) for (int x = 0; x < N; x++) {
                int hits = 0;
                for (int j = 0; j < 4; j++) for (int i = 0; i < 4; i++) {
                    const double X = x + (i + 0.5) / 4, Y = y + (j + 0.5) / 4;
                    bool on = false;
                    for (int k = 0; k < 4; k++) {   // four waves of widths 1.5 to 6
                        const double w = 1.5 + k * 1.5, cy = 60 + k * 85 + 18 * std::sin(X / (22.0 + k * 7));
                        on = on || std::fabs(Y - cy) < w / 2;
                    }
                    on = on || std::fabs(std::hypot(X - 190, Y - 190) - 150) < 1.5;   // a thin ring
                    hits += on;
                }
                cov[size_t(y) * N + x] = hits / 16.0f;
            }
            // Smudge: a small blur, the unclear lines of a render.
            std::vector<float> blurred(cov.size());
            const float k3[3] = {0.25f, 0.5f, 0.25f};
            for (int y = 0; y < N; y++) for (int x = 0; x < N; x++) { float a = 0; for (int i = -1; i <= 1; i++) a += k3[i + 1] * cov[size_t(y) * N + size_t(std::clamp(x + i, 0, N - 1))]; blurred[size_t(y) * N + x] = a; }
            for (int y = 0; y < N; y++) for (int x = 0; x < N; x++) { float a = 0; for (int j = -1; j <= 1; j++) a += k3[j + 1] * blurred[size_t(std::clamp(y + j, 0, N - 1)) * N + x]; truth.at(x, y) = uint8_t(std::lround(a * 255)); }
        }
        Image image(N, N);
        std::mt19937 rng(7);
        std::normal_distribution<double> noise(0, 3);
        for (int y = 0; y < N; y++) for (int x = 0; x < N; x++) {
            const double c = truth.at(x, y) / 255.0;
            uint8_t* p = image.pixel(x, y);
            p[0] = uint8_t(std::clamp(std::lround(lr * c + br * (1 - c) + noise(rng)), 0L, 255L));
            p[1] = uint8_t(std::clamp(std::lround(lg * c + bg * (1 - c) + noise(rng)), 0L, 255L));
            p[2] = uint8_t(std::clamp(std::lround(lb * c + bb * (1 - c) + noise(rng)), 0L, 255L));
            p[3] = 255;
        }
        // What one click is answerable for: the background region it lands in (no line coverage to speak of,
        // connected to the click) and everything within 5 pixels of it (the lines bounding it).
        std::vector<uint8_t> scope(size_t(N) * N, 0), clearedSide(size_t(N) * N, 0);
        {
            std::vector<uint8_t> region(size_t(N) * N, 0);
            std::vector<std::pair<int, int>> stack{{5, 5}};
            while (!stack.empty()) {
                auto [x, y] = stack.back(); stack.pop_back();
                if (x < 0 || y < 0 || x >= N || y >= N || region[size_t(y) * N + x] || truth.at(x, y) >= 13) continue;
                region[size_t(y) * N + x] = 1;
                stack.push_back({x + 1, y}); stack.push_back({x - 1, y}); stack.push_back({x, y + 1}); stack.push_back({x, y - 1});
            }
            for (int y = 0; y < N; y++) for (int x = 0; x < N; x++) {
                if (!region[size_t(y) * N + x]) continue;
                for (int j = -5; j <= 5; j++) for (int i = -5; i <= 5; i++) { const int X = x + i, Y = y + j; if (X >= 0 && Y >= 0 && X < N && Y < N) scope[size_t(Y) * N + X] = 1; }
            }
            // The cleared side: where blue left over counts (the far side of a line keeps its own region's blue).
            for (int y = 0; y < N; y++) for (int x = 0; x < N; x++) {
                if (!region[size_t(y) * N + x]) continue;
                for (int j = -2; j <= 2; j++) for (int i = -2; i <= 2; i++) { const int X = x + i, Y = y + j; if (X >= 0 && Y >= 0 && X < N && Y < N) clearedSide[size_t(Y) * N + X] = 1; }
            }
            // Not the other regions beyond the lines: those are another click's.
            for (int y = 0; y < N; y++) for (int x = 0; x < N; x++)
                if (!region[size_t(y) * N + x] && truth.at(x, y) < 13) scope[size_t(y) * N + x] = 0;
        }
        struct Result { double alphaError, lineEaten, blueLeft; };
        auto score = [&](const Image& out) {
            double err = 0, eaten = 0, blue = 0; long ne = 0, nl = 0, nb = 0;
            for (int y = 0; y < N; y++) for (int x = 0; x < N; x++) {
                if (!scope[size_t(y) * N + x]) continue;
                const double want = truth.at(x, y) / 255.0;
                const uint8_t* p = out.pixel(x, y);
                const double a = p[3] / 255.0;
                if (want > 0 || a > 0) { err += std::fabs(a - want); ne++; }
                if (want > 0.9) { eaten += std::max(0.0, want - a); nl++; }
                // What is left, as it shows over white: blue above the line's own colour is the old ground.
                if (a > 0.02 && clearedSide[size_t(y) * N + x]) { const double sb = p[2] / 255.0 / a * 255, sr = p[0] / 255.0 / a * 255; blue += a * std::max(0.0, (sb - sr) - (lb - lr)); nb++; }
            }
            return Result{err / std::max(1L, ne), eaten / std::max(1L, nl), blue / std::max(1L, nb)};
        };
        SmartWandImage prepared(image);
        auto field = prepared.propagate(5, 5, 1, wandCost(255));
        std::printf("%-44s %12s %12s %12s\n", "flow (best tolerance)", "alpha error", "line eaten", "blue left");
        auto report = [&](const char* name, auto run) {
            for (int t : {16, 32, 48}) { Result r = run(t); std::printf("%-38s (%3d) %12.4f %12.4f %12.2f\n", name, t, r.alphaError, r.lineEaten, r.blueLeft); }
        };
        auto clearPlain = [&](const GrayImage& m) {
            Image out = image;
            for (int y = 0; y < N; y++) for (int x = 0; x < N; x++) { const double c = m.at(x, y) / 255.0; uint8_t* p = out.pixel(x, y); for (int k = 0; k < 4; k++) p[k] = uint8_t(p[k] * (1 - c) + 0.5); }
            return out;
        };
        report("wand, delete", [&](int t) { GrayImage m(N, N, 0); thresholdWandField(field, t, true, m); return score(clearPlain(m)); });
        report("wand, expand 2, smooth 3, delete", [&](int t) {
            GrayImage m(N, N, 0); thresholdWandField(field, t, true, m);
            Selection s; s.coverage = std::make_shared<GrayImage>(m);
            Selection grown = resizeSelection(s, 2);
            auto smooth = smoothSelection(*grown.coverage, 3);
            return score(clearPlain(*smooth));
        });
        report("classic wand, expand 2, smooth 3, delete", [&](int t) {
            GrayImage m(N, N, 0); wandMask(image, 5, 5, 1, t, true, m);
            Selection s; s.coverage = std::make_shared<GrayImage>(m);
            Selection grown = resizeSelection(s, 2);
            auto smooth = smoothSelection(*grown.coverage, 3);
            return score(clearPlain(*smooth));
        });
        report("wand, refined edge, delete", [&](int t) { GrayImage m(N, N, 0); thresholdWandField(field, t, true, m); refineWandEdge(image, m, 3); return score(clearPlain(m)); });
        report("wand, refined edge, clean delete", [&](int t) {
            GrayImage m(N, N, 0); thresholdWandField(field, t, true, m); std::vector<uint32_t> colours; refineWandEdge(image, m, 3, &colours);
            Image out = image; clearDecontaminated(out, m, &colours); return score(out);
        });
        if (argc > 2) {
            GrayImage m(N, N, 0); thresholdWandField(field, 32, true, m); std::vector<uint32_t> colours; refineWandEdge(image, m, 3, &colours);
            Image out = image; clearDecontaminated(out, m, &colours);
            writePngImage(std::string(argv[2]) + "/lines.png", image, 72, nullptr);
            writePngImage(std::string(argv[2]) + "/lines-clean.png", out, 72, nullptr);
            GrayImage m2(N, N, 0); thresholdWandField(field, 32, true, m2);
            Selection s; s.coverage = std::make_shared<GrayImage>(m2);
            auto smooth = smoothSelection(*resizeSelection(s, 2).coverage, 3);
            writePngImage(std::string(argv[2]) + "/lines-routine.png", clearPlain(*smooth), 72, nullptr);
        }
        return 0;
    }
    // wand_bench check: the benchmark's recorded results as a test (tests/CMakeLists.txt runs it).
    const bool check = argc > 1 && std::string(argv[1]) == "check";
    const bool dump = argc > 2 && std::string(argv[1]) == "dump";
    const std::string dir = dump ? argv[2] : "";
    std::vector<std::pair<std::string, Method>> methods;
    methods.push_back({"classic", [](const Scene& s) {
        return [&s](int t, GrayImage& m) { wandMask(s.image, s.x, s.y, 1, t, true, m); };
    }});
    struct Variant { const char* label; double edge, neighbour, seed, texture, region = 0; };
    for (Variant v : {Variant{"seed only", 0, 0, 1, 0}, Variant{"milestone 1", 0, 8, 0.7, 2, 0}, Variant{"region weight 2", 0, 8, 0.7, 2, 2}, Variant{"shipped", 0, 8, 0.7, 2, 1}}) {
        methods.push_back({v.label, [v](const Scene& s) {
            auto image = std::make_shared<SmartWandImage>(s.image, v.edge > 0);
            SmartWandOptions o; o.edgeWeight = v.edge; o.neighbourWeight = v.neighbour; o.seedWeight = v.seed; o.textureWeight = v.texture; o.regionWeight = v.region;
            auto field = std::make_shared<SmartWandImage::Field>(image->propagate(s.x, s.y, 2, wandCost(255), o));
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
    if (check) {
        // The shipped wand (the last method) must stay at least as good as recorded, and ahead of the classic one.
        const size_t shipped = methods.size() - 1;
        const double atDefault = sumDefault[shipped] / all.size(), best = sumBest[shipped] / all.size();
        bool ok = atDefault >= 0.86 && best >= 0.99 && atDefault > sumDefault[0] / all.size() + 0.2;
        std::printf("check: shipped IoU@32 %.3f (at least 0.86), best %.3f (at least 0.99): %s\n", atDefault, best, ok ? "ok" : "REGRESSED");
        return ok ? 0 : 1;
    }
    return 0;
}
