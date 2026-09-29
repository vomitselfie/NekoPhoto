// Selecting by colour in CMYK and Lab documents (P7 E): the Magic Wand and Quick Select decide on decisionImage(),
// which holds L*a*b* for these modes (a Lab document's own values, a CMYK one's composite through its profile), and
// never write the document's pixels; loading a CMYK layer's pixels as a selection reads its fifth sample, alpha.
#include "check.h"
#include "compositor/colormgmt.h"
#include "compositor/colormodes.h"
#include "compositor/depth.h"
#include "compositor/render.h"
#include "compositor/scribble.h"
#include "compositor/selection.h"
#include "compositor/wand.h"
#include <cstdlib>
#include <string>

using namespace compositor;

namespace {

/// A document whose left half is white and right half a strong colour (full cyan ink, or L 50 a +60), one opaque layer;
/// with `hole`, the layer is transparent outside x 4..27, y 4..27.
Document twoTone(ColorMode mode, SampleType type, bool hole = false) {
    const int w = 32, h = 32;
    Document doc(w, h);
    doc.sampleType = type;
    doc.colorMode = mode;
    doc.profile = mode == ColorMode::CMYK ? defaultCmykProfile() : labProfile();
    const int n = colorModeChannels(mode);
    auto fillPixel = [&](auto* p, bool right, bool opaque, uint32_t one, int abOffset, int abScale) {
        using T = std::remove_reference_t<decltype(*p)>;
        for (int c = 0; c < n; c++) p[c] = T(one);
        if (mode == ColorMode::CMYK && right) p[0] = 0;   // stored ink is inverted: 0 is full cyan
        if (mode == ColorMode::Lab) {
            p[1] = p[2] = T(abOffset);
            if (right) { p[0] = T(one / 2); p[1] = T(abOffset + 60 * abScale); }
        }
        if (!opaque) for (int c = 0; c < n; c++) p[c] = 0;
    };
    auto opaqueAt = [&](int x, int y) { return !hole || (x >= 4 && x < 28 && y >= 4 && y < 28); };
    AnyImage image;
    if (type == SampleType::U16) {
        auto b = std::make_shared<Image16>(w, h, n);
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) fillPixel(b->pixel(x, y), x >= 16, opaqueAt(x, y), one16, labOffset<SampleType::U16>(), labScale<SampleType::U16>());
        image = Image16Ptr(b);
    } else if (mode == ColorMode::CMYK) {
        auto b = std::make_shared<ImageC8>(w, h, 5);
        for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) fillPixel(b->pixel(x, y), x >= 16, opaqueAt(x, y), 255, 128, 1);
        image = ImageC8Ptr(b);
    } else {
        auto b = std::make_shared<Image>(w, h);
        for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) fillPixel(b->pixel(x, y), x >= 16, opaqueAt(x, y), 255, 128, 1);
        image = ImagePtr(b);
    }
    doc.layers = {Layer(Asset::makeAny(image, "two tone"), Point(0, 0))};
    return doc;
}

const ColorMode modes[] = {ColorMode::CMYK, ColorMode::Lab};
const SampleType types[] = {SampleType::U8, SampleType::U16};

} // namespace

TEST_CASE(wand_selects_the_clicked_half_in_cmyk_and_lab) {
    for (ColorMode mode : modes)
        for (SampleType type : types) {
            Document doc = twoTone(mode, type);
            const AnyImage before = doc.layers[0].asset->image;
            auto decision = decisionImage(doc);
            CHECK(decision && decision->width() == 32 && decision->height() == 32);
            GrayImage mask(32, 32);
            CHECK_EQ(wandMask(*decision, 24, 10, 0, 32, true, mask), 16L * 32L);
            CHECK_EQ(int(mask.at(20, 5)), 255);
            CHECK_EQ(int(mask.at(8, 5)), 0);
            // The white half from the other side, not contiguous: the same split.
            CHECK_EQ(wandMask(*decision, 2, 2, 1, 32, false, mask), 16L * 32L);
            CHECK_EQ(int(mask.at(8, 30)), 255);
            // The decision is L*a*b*: white is L 100, a and b neutral.
            CHECK(decision->pixel(2, 2)[0] >= 250);
            CHECK(std::abs(int(decision->pixel(2, 2)[1]) - 128) <= 2);
            // Deciding never writes the document's pixels.
            CHECK(doc.layers[0].asset->image.identity() == before.identity());
        }
}

TEST_CASE(wand_tolerance_counts_lab_levels) {
    // The colour half differs from white by a +60 and L 50 (127 levels): a tolerance past both takes everything.
    Document doc = twoTone(ColorMode::Lab, SampleType::U8);
    auto decision = decisionImage(doc);
    GrayImage mask(32, 32);
    CHECK_EQ(wandMask(*decision, 2, 2, 0, 59, true, mask), 16L * 32L);
    CHECK_EQ(wandMask(*decision, 2, 2, 0, 140, true, mask), 32L * 32L);
}

TEST_CASE(quick_select_grows_over_a_region_in_cmyk_and_lab) {
    if (!scribbleSelectionSupported()) return;
    for (ColorMode mode : modes)
        for (SampleType type : types) {
            Document doc = twoTone(mode, type);
            auto decision = decisionImage(doc);
            GrayImage labels(32, 32, 0);
            for (int y = 8; y < 24; y++) labels.at(24, y) = 1;   // a stroke on the colour half
            for (int y = 8; y < 24; y++) labels.at(6, y) = 2;    // and one on the white half, as background
            std::string why;
            auto coverage = scribbleSelection(*decision, labels, 250, 3, &why);
            CHECK(coverage != nullptr);
            if (!coverage) continue;
            CHECK(coverage->at(20, 2) > 128);   // grown past the stroke over the colour half
            CHECK(coverage->at(29, 30) > 128);
            CHECK(coverage->at(4, 16) < 128);
        }
}

TEST_CASE(cmyk_layer_alpha_loads_as_a_selection) {
    for (SampleType type : types) {
        Document doc = twoTone(ColorMode::CMYK, type, true);
        const AnyGray coverage = coverageFromLayerAlpha(doc, doc.layers[0]);
        CHECK(bool(coverage));
        if (type == SampleType::U16) {
            CHECK(coverage.u16() != nullptr);
            if (!coverage.u16()) continue;
            CHECK_EQ(int(coverage.u16()->at(10, 10)), int(one16));
            CHECK_EQ(int(coverage.u16()->at(1, 1)), 0);
            CHECK_EQ(int(coverage.u16()->at(28, 16)), 0);
        } else {
            CHECK(coverage.u8() != nullptr);
            if (!coverage.u8()) continue;
            CHECK_EQ(int(coverage.u8()->at(10, 10)), 255);
            CHECK_EQ(int(coverage.u8()->at(1, 1)), 0);
            CHECK_EQ(int(coverage.u8()->at(27, 27)), 255);
        }
    }
    // Another layout has nothing to read this way.
    Document lab = twoTone(ColorMode::Lab, SampleType::U8);
    CHECK(!coverageFromLayerAlpha(lab, lab.layers[0]));
}

TEST_MAIN()
