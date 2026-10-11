// The content-aware tools and Remove Background's edge colours in CMYK and Lab documents: Content-Aware Fill, Move and
// Scale on the document's own samples at 8 and 16 bits, Content-Aware spot healing on CMYK's inks, and foreground
// estimation on the inks or L, a and b. Nothing here passes through RGB: what is written comes from the source samples,
// what is not touched stays bit for bit, and 16-bit values that no 8-bit level widens to survive.
#include "check.h"
#include "compositor/colormodes.h"
#include "compositor/contentmove.h"
#include "compositor/heal.h"
#include "compositor/inpaint.h"
#include "compositor/matte.h"
#include "compositor/seamcarve.h"
#include <algorithm>
#include <array>
#include <cmath>

using namespace compositor;

namespace {

/// An image of `channels` samples filled with `field`, a square of `blot` at (x0, y0).
template <class Img, class S, size_t N>
Img flat(int w, int h, std::array<S, N> field, std::array<S, N> blot, int x0, int y0, int side) {
    Img image(w, h, int(N));
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            const bool in = x >= x0 && x < x0 + side && y >= y0 && y < y0 + side;
            for (size_t c = 0; c < N; c++) image.pixel(x, y)[c] = in ? blot[c] : field[c];
        }
    return image;
}

/// A mask of `w` x `h` with a square of `on` at (x0, y0).
template <class G, class V>
G square(int w, int h, int x0, int y0, int side, V on) {
    G out(w, h, 0);
    for (int y = y0; y < y0 + side; y++) for (int x = x0; x < x0 + side; x++) out.at(x, y) = on;
    return out;
}

/// Every pixel where `mask` is zero is `before`'s, bit for bit; every one where it is set is `expect`.
template <class Img, class G, class S, size_t N>
void checkFill(const Img& after, const Img& before, const G& mask, std::array<S, N> expect) {
    int wrong = 0, moved = 0;
    for (int y = 0; y < after.height(); y++)
        for (int x = 0; x < after.width(); x++)
            for (size_t c = 0; c < N; c++) {
                if (!mask.at(x, y)) moved += after.pixel(x, y)[c] != before.pixel(x, y)[c];
                else wrong += after.pixel(x, y)[c] != expect[c];
            }
    CHECK_EQ(moved, 0);
    CHECK_EQ(wrong, 0);
}

// CMYK as stored: each ink inverted (one is no ink), then alpha. K-only black: no C, M or Y, full K.
constexpr std::array<uint8_t, 5> kOnly8{255, 255, 255, 0, 255};
constexpr std::array<uint8_t, 5> richBlack8{20, 30, 40, 10, 255};
// At 16 bits a K-only grey whose black no 8-bit level widens to (widen8 gives multiples of about 128.5).
constexpr std::array<uint16_t, 5> kOnly16{32768, 32768, 32768, 12345, 32768};
constexpr std::array<uint16_t, 5> richBlack16{2000, 3000, 4000, 1000, 32768};

} // namespace

TEST_CASE(cmyk8_fill_takes_the_surrounding_k_only_ink) {
    const ImageC8 before = flat<ImageC8, uint8_t, 5>(96, 96, kOnly8, richBlack8, 40, 40, 14);
    const GrayImage hole = square<GrayImage>(96, 96, 38, 38, 18, uint8_t(255));
    ImageC8 image = before;
    REQUIRE(contentFill(image, hole));
    CHECK_EQ(image.channels(), 5);
    checkFill(image, before, hole, kOnly8);
}

TEST_CASE(cmyk16_fill_keeps_sixteen_bit_inks) {
    const Image16 before = flat<Image16, uint16_t, 5>(96, 96, kOnly16, richBlack16, 40, 40, 14);
    const Gray16 hole = square<Gray16>(96, 96, 38, 38, 18, uint16_t(32768));
    Image16 image = before;
    REQUIRE(contentFill(image, hole));
    CHECK_EQ(image.channels(), 5);
    checkFill(image, before, hole, kOnly16);
}

TEST_CASE(cmyk_fill_chooses_on_the_inks_not_their_look) {
    // Rich black and K-only black look alike; the hole in the K-only half fills with K-only black, the hole in the rich
    // half with the rich black.
    ImageC8 image(120, 64, 5);
    const std::array<uint8_t, 5> rich{0, 0, 0, 0, 255};
    for (int y = 0; y < 64; y++)
        for (int x = 0; x < 120; x++)
            for (int c = 0; c < 5; c++) image.pixel(x, y)[c] = x < 60 ? kOnly8[size_t(c)] : rich[size_t(c)];
    const ImageC8 before = image;
    GrayImage hole(120, 64, 0);
    for (int y = 26; y < 38; y++) { for (int x = 20; x < 32; x++) hole.at(x, y) = 255; for (int x = 88; x < 100; x++) hole.at(x, y) = 255; }
    REQUIRE(contentFill(image, hole));
    for (int c = 0; c < 5; c++) {
        CHECK_EQ(int(image.pixel(26, 32)[c]), int(kOnly8[size_t(c)]));
        CHECK_EQ(int(image.pixel(94, 32)[c]), int(rich[size_t(c)]));
    }
    int moved = 0;
    for (int y = 0; y < 64; y++) for (int x = 0; x < 120; x++) if (!hole.at(x, y)) for (int c = 0; c < 5; c++) moved += image.pixel(x, y)[c] != before.pixel(x, y)[c];
    CHECK_EQ(moved, 0);
}

TEST_CASE(lab_fill_at_8_and_16_bits) {
    // L, a and b as stored (a and b offset to neutral), alpha last.
    const std::array<uint8_t, 4> field8{180, 100, 160, 255}, blot8{20, 200, 30, 255};
    const Image before8 = [&] {
        Image out(80, 80);
        for (int y = 0; y < 80; y++) for (int x = 0; x < 80; x++) {
            const bool in = x >= 34 && x < 46 && y >= 34 && y < 46;
            for (int c = 0; c < 4; c++) out.pixel(x, y)[c] = in ? blot8[size_t(c)] : field8[size_t(c)];
        }
        return out;
    }();
    const GrayImage hole8 = square<GrayImage>(80, 80, 32, 32, 16, uint8_t(255));
    Image image8 = before8;
    InpaintOptions options;
    options.colorMode = ColorMode::Lab;
    REQUIRE(contentFill(image8, hole8, options));
    checkFill(image8, before8, hole8, field8);

    const std::array<uint16_t, 4> field16{23001, 12999, 20001, 32768}, blot16{1000, 30000, 2000, 32768};
    const Image16 before16 = flat<Image16, uint16_t, 4>(80, 80, field16, blot16, 34, 34, 12);
    const Gray16 hole16 = square<Gray16>(80, 80, 32, 32, 16, uint16_t(32768));
    Image16 image16 = before16;
    REQUIRE(contentFill(image16, hole16, options));
    CHECK_EQ(image16.channels(), 4);
    checkFill(image16, before16, hole16, field16);
}

TEST_CASE(cmyk_content_aware_move_carries_every_ink) {
    // A rich-black square on K-only grey moves 30 pixels right: where it was fills with K-only grey, its middle lands
    // with its own inks (Very Strict), and pixels away from both stay as they were.
    for (int depth : {8, 16}) {
        ContentMoveOptions options;
        options.adaptation = 0;
        if (depth == 8) {
            const ImageC8 before = flat<ImageC8, uint8_t, 5>(128, 96, kOnly8, richBlack8, 30, 36, 20);
            ImageC8 image = before;
            const GrayImage selection = square<GrayImage>(128, 96, 28, 34, 24, uint8_t(255));
            REQUIRE(contentAwareMove(image, selection, 30, 0, options));
            for (int c = 0; c < 5; c++) {
                CHECK_EQ(int(image.pixel(40, 46)[c]), int(kOnly8[size_t(c)]));
                CHECK_NEAR(image.pixel(70, 46)[c], richBlack8[size_t(c)], 1);
                CHECK_EQ(int(image.pixel(5, 5)[c]), int(before.pixel(5, 5)[c]));
                CHECK_EQ(int(image.pixel(120, 90)[c]), int(before.pixel(120, 90)[c]));
            }
        } else {
            const Image16 before = flat<Image16, uint16_t, 5>(128, 96, kOnly16, richBlack16, 30, 36, 20);
            Image16 image = before;
            const Gray16 selection = square<Gray16>(128, 96, 28, 34, 24, uint16_t(32768));
            REQUIRE(contentAwareMove(image, selection, 30, 0, options));
            CHECK_EQ(image.channels(), 5);
            for (int c = 0; c < 5; c++) {
                CHECK_EQ(int(image.pixel(40, 46)[c]), int(kOnly16[size_t(c)]));
                CHECK_NEAR(image.pixel(70, 46)[c], richBlack16[size_t(c)], 1);
                CHECK_EQ(int(image.pixel(5, 5)[c]), int(before.pixel(5, 5)[c]));
            }
        }
    }
}

TEST_CASE(cmyk_seam_carving_carries_the_inks) {
    // A K-only field with a rich-black bar: narrowing and widening keep every sample one of the two, at 16 bits too.
    const Image16 before = flat<Image16, uint16_t, 5>(64, 40, kOnly16, richBlack16, 28, 0, 8);
    for (int width : {48, 80}) {
        const Image16 out = seamCarve(before, width, 40);
        REQUIRE(out.width() == width && out.height() == 40 && out.channels() == 5);
        int other = 0, bar = 0;
        for (int y = 0; y < 40; y++)
            for (int x = 0; x < width; x++) {
                const uint16_t* p = out.pixel(x, y);
                bool isK = true, isRich = true;
                for (int c = 0; c < 5; c++) { isK &= p[c] == kOnly16[size_t(c)]; isRich &= p[c] == richBlack16[size_t(c)]; }
                other += !isK && !isRich;
                bar += isRich;
            }
        // Narrowing only removes samples; a doubled seam beside the bar averages the two, everywhere else the inks are
        // carried exactly.
        if (width < 64) CHECK_EQ(other, 0);
        else CHECK(other <= 40 * 4);
        CHECK(bar >= 40);
    }
    const ImageC8 eight = flat<ImageC8, uint8_t, 5>(64, 40, kOnly8, richBlack8, 28, 0, 8);
    const ImageC8 narrow = seamCarve(eight, 50, 30);
    REQUIRE(narrow.width() == 50 && narrow.height() == 30 && narrow.channels() == 5);
    for (int c = 0; c < 5; c++) CHECK_EQ(int(narrow.pixel(2, 2)[c]), int(kOnly8[size_t(c)]));
}

TEST_CASE(cmyk_content_aware_spot_healing_synthesises_inks) {
    const ImageC8 before = flat<ImageC8, uint8_t, 5>(96, 96, kOnly8, richBlack8, 44, 44, 8);
    ImageC8 image = before;
    const GrayImage spot = square<GrayImage>(96, 96, 42, 42, 12, uint8_t(255));
    spotHeal(image, spot, 1.0f, 0, 7);
    checkFill(image, before, spot, kOnly8);
}

TEST_CASE(foreground_estimation_on_inks_and_lab) {
    // A K-only subject on the left over a cyan background on the right, a soft matte between: the soft pixels' inks move
    // toward the subject's (less cyan, as much black), opaque ones keep theirs, alpha is kept.
    const int w = 64, h = 16;
    const std::array<uint16_t, 5> subject{32768, 32768, 32768, 4000, 32768}, ground{3000, 32768, 32768, 32768, 32768};
    Image16 image(w, h, 5);
    AlphaPlane matte(w, h, 0);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            const float a = std::clamp((40.0f - float(x)) / 16.0f, 0.0f, 1.0f);   // 1 up to x = 24, 0 from x = 40
            matte.at(x, y) = a;
            for (int c = 0; c < 5; c++) image.pixel(x, y)[c] = uint16_t(std::lround(subject[size_t(c)] * a + ground[size_t(c)] * (1 - a)));
        }
    const auto out = estimateForeground(image, matte);
    REQUIRE(out && out->channels() == 5);
    for (int c = 0; c < 5; c++) CHECK_EQ(int(out->pixel(5, 8)[c]), int(image.pixel(5, 8)[c]));   // opaque: untouched
    const uint16_t* soft = out->pixel(32, 8);
    CHECK_EQ(int(soft[4]), 32768);
    CHECK(soft[0] > image.pixel(32, 8)[0] + 4000);   // less cyan ink than the mix
    CHECK_NEAR(soft[0], subject[0], 2000);
    CHECK_NEAR(soft[3], subject[3], 2000);

    // 8-bit CMYK and a Lab image through estimateForegroundAny keep their layouts.
    const ImageC8 eight = flat<ImageC8, uint8_t, 5>(32, 8, kOnly8, kOnly8, 0, 0, 0);
    const AnyImage inks = estimateForegroundAny(ImageC8Ptr(std::make_shared<ImageC8>(eight)), AlphaPlane(32, 8, 0.5f));
    REQUIRE(inks.c8() && inks.c8()->channels() == 5);
    for (int c = 0; c < 5; c++) CHECK_EQ(int(inks.c8()->pixel(10, 4)[c]), int(kOnly8[size_t(c)]));
    const Image16 lab = flat<Image16, uint16_t, 4>(32, 8, {23001, 12999, 20001, 32768}, {23001, 12999, 20001, 32768}, 0, 0, 0);
    const AnyImage labOut = estimateForegroundAny(Image16Ptr(std::make_shared<Image16>(lab)), AlphaPlane(32, 8, 0.5f));
    REQUIRE(labOut.u16() && labOut.u16()->channels() == 4);
    CHECK_NEAR(labOut.u16()->pixel(10, 4)[1], 12999, 4);
}

TEST_MAIN()
