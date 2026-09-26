// Seam carving: sizes, a uniform band carved first, protected pixels kept, growth, and a 12 MP timing.
#include "check.h"
#include "compositor/seamcarve.h"
#include <chrono>
#include <cstdio>
#include <cstring>
#include <random>

using namespace compositor;

namespace {

Image noise(int w, int h, uint32_t seed) {
    Image img(w, h);
    std::mt19937 rng(seed);
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
        uint8_t* p = img.pixel(x, y);
        p[0] = uint8_t(rng() % 100); p[1] = uint8_t(rng() % 100); p[2] = uint8_t(rng() % 100); p[3] = 255;
    }
    return img;
}

void paintBand(Image& img, int x0, int x1) {
    for (int y = 0; y < img.height(); y++) for (int x = x0; x < x1; x++) {
        uint8_t* p = img.pixel(x, y); p[0] = p[1] = p[2] = 200; p[3] = 255;
    }
}

int grayCount(const Image& img, int y) {
    int n = 0;
    for (int x = 0; x < img.width(); x++) if (img.pixel(x, y)[0] == 200) n++;
    return n;
}

} // namespace

TEST_CASE(sizes_come_out_as_asked) {
    Image img = noise(50, 40, 1);
    Image a = seamCarve(img, 37, 40);
    CHECK(a.width() == 37 && a.height() == 40);
    Image b = seamCarve(img, 50, 29);
    CHECK(b.width() == 50 && b.height() == 29);
    Image c = seamCarve(img, 81, 55);
    CHECK(c.width() == 81 && c.height() == 55);
    CHECK(seamCarve(img, 0, 10).isEmpty());
    Image same = seamCarve(img, 50, 40);
    CHECK(same == img);
}

TEST_CASE(uniform_band_goes_first) {
    // Noise with a flat band 12 wide: 8 seams all come out of the band, and every noise pixel stays in order.
    Image img = noise(80, 60, 2);
    paintBand(img, 30, 42);
    Image out = seamCarve(img, 72, 60);
    CHECK(out.width() == 72);
    for (int y = 0; y < 60; y++) {
        int j = 0;
        bool same = true;
        for (int x = 0; x < 80; x++) {
            if (img.pixel(x, y)[0] == 200) continue;
            while (j < 72 && out.pixel(j, y)[0] == 200) j++;
            if (j >= 72 || std::memcmp(out.pixel(j, y), img.pixel(x, y), 4) != 0) { same = false; break; }
            j++;
        }
        CHECK(same);
        CHECK(grayCount(out, y) == 4);
    }
}

TEST_CASE(protected_pixels_are_kept) {
    Image img = noise(60, 40, 3);
    paintBand(img, 20, 31); // flat, so it would be carved first
    GrayImage protect(60, 40, 0);
    for (int y = 0; y < 40; y++) for (int x = 20; x < 31; x++) protect.at(x, y) = 255;
    SeamCarveOptions options;
    options.protect = &protect;
    Image out = seamCarve(img, 50, 40, options);
    for (int y = 0; y < 40; y++) CHECK(grayCount(out, y) == 11);
    Image unprotected = seamCarve(img, 50, 40);
    CHECK(grayCount(unprotected, 0) < 11);
}

TEST_CASE(growing_widens_the_flat_part) {
    Image img = noise(60, 30, 4);
    paintBand(img, 10, 20);
    Image out = seamCarve(img, 70, 30);
    CHECK(out.width() == 70);
    for (int y = 0; y < 30; y++) CHECK(grayCount(out, y) >= 18);
}

TEST_CASE(twelve_megapixels_twenty_percent) {
    Image img = noise(4000, 3000, 5);
    auto t0 = std::chrono::steady_clock::now();
    Image out = seamCarve(img, 3200, 3000);
    double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::fprintf(stderr, "  4000x3000 -> 3200x3000: %.2f s\n", s);
    CHECK(out.width() == 3200);
    CHECK(s < 10);
}

TEST_MAIN()
