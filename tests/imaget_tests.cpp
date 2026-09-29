// The typed image skeleton for high bit depth (docs/high-bit-depth-plan.md, P1): sample traits, ImageT,
// GrayImageT, the AnyImage / AnyGray holders that layers, masks and the selection keep, and the supports() registry.
#include "check.h"
#include "compositor/imaget.h"
#include "compositor/depth.h"
#include "compositor/document.h"
#include "compositor/history.h"
#include "compositor/supports.h"
#include <type_traits>

using namespace compositor;

static_assert(std::is_same_v<ImageOf<SampleType::U8>, Image>);
static_assert(std::is_same_v<GrayOf<SampleType::U8>, GrayImage>);
static_assert(std::is_same_v<ImageOf<SampleType::U16>::Sample, uint16_t>);
static_assert(std::is_same_v<GrayOf<SampleType::F32>::Sample, float>);
static_assert(SampleTraits<SampleType::U16>::one == 32768);
static_assert(sampleBytes(SampleType::F32) == 4);

TEST_CASE(typed_image_layout) {
    Image16 a(3, 2);
    CHECK_EQ(a.channels(), 4);
    CHECK_EQ(a.stride(), 3 * 4 * 2);
    CHECK_EQ(a.byteCount(), size_t(3 * 2 * 4 * 2));
    CHECK_EQ(int(a.pixel(1, 1)[0]), 0);
    const uint16_t white[4] = {32768, 32768, 32768, 32768};
    a.fill(white);
    CHECK_EQ(int(a.pixel(2, 1)[3]), 32768);
    ImageF cmyk(2, 2, 5);
    CHECK_EQ(cmyk.stride(), 2 * 5 * 4);
    CHECK(cmyk.pixel(1, 0) == cmyk.row(0) + 5);
    Image16 tooBig(maxImageSide + 1, 1);
    CHECK(tooBig.isEmpty());
    Gray16 g(4, 4, 100);
    CHECK_EQ(int(g.at(3, 3)), 100);
    Gray16 copy = g;
    CHECK(copy == g);
}

TEST_CASE(any_image_holds_each_depth) {
    AnyImage none;
    CHECK(!none);
    CHECK(none.sampleType() == SampleType::U8);
    auto eight = std::make_shared<Image>(2, 2);
    AnyImage a = eight;                 // a shared_ptr<Image> converts, as 8-bit code assigns them
    CHECK(bool(a));
    CHECK(a.sampleType() == SampleType::U8);
    CHECK(a.u8().get() == eight.get());
    CHECK(!a.u16());
    CHECK(a.identity() == eight.get());
    AnyImage deep = Image16Ptr(std::make_shared<Image16>(2, 2));
    CHECK(deep.sampleType() == SampleType::U16);
    CHECK(!deep.u8());                  // 8-bit code sees no pixels in a deeper buffer
    CHECK(bool(deep));
    int width = visit([](const auto& p) { return p ? p->width() : -1; }, deep);
    CHECK_EQ(width, 2);
    AnyImage same = a;
    CHECK(same == a);
    CHECK(!(same == deep));
    same.reset();
    CHECK(!same);
    AnyImage null = nullptr;
    CHECK(!null);
    AnyGray mask = std::make_shared<GrayImage>(1, 1, 255);
    CHECK(mask.u8()->at(0, 0) == 255);
    AnyGray floatMask = GrayFPtr(std::make_shared<GrayF>(1, 1, 1.0f));
    CHECK(floatMask.sampleType() == SampleType::F32);
    CHECK(floatMask.f32()->at(0, 0) == 1.0f);
}

TEST_CASE(support_registry_lists_what_p2_and_p3a_port) {
    // Every feature supports 8-bit; P2 ports the renderer, the layer structure, masks and the files to 16 bits.
    CHECK(supports("render.document", SampleType::U8));
    CHECK(supports("render.document", SampleType::U16));
    CHECK(!supports("render.document", SampleType::F32));
    for (const char* ported : {"document.mode", "layers.structure", "layers.transform", "layers.mask", "export.psd", "export.png", "tool.move"})
        CHECK(supports(ported, SampleType::U16));
    // P3a: adjustments, the built-in filters, selections, fill, the clipboard, Image Size, crops and distortion.
    for (const char* ported : {"edit.selection", "tool.marquee", "tool.wand", "tool.quickSelect", "edit.fill", "edit.clipboard", "filter.Gaussian Blur",
                               "filter.Lens Correction", "edit.imageSize", "edit.crop", "edit.distort"})
        CHECK(supports(ported, SampleType::U16));
    for (int k = 0; k < adjustmentKindCount; k++) {
        CHECK(supports(AdjustmentKind(k), SampleType::U8));
        CHECK(supports(AdjustmentKind(k), SampleType::U16));
    }
    // P3b: painting and retouching.
    for (const char* painting : {"tool.brush", "tool.spotHealing", "tool.cloneStamp", "tool.smudge", "tool.dodge", "tool.gradient", "tool.paintBucket", "edit.movePixels", "layers.merge", "layers.applyMask"})
        CHECK(supports(painting, SampleType::U16));
    // Text, shapes, paths, vector masks, layer styles and baking a clipping mask.
    for (const char* ported : {"edit.paint", "edit.pixels", "tool.shape", "tool.text", "tool.pen", "tool.directSelect", "edit.text", "edit.vector", "edit.style"})
        CHECK(supports(ported, SampleType::U16));
    // Smart objects and Smart Filters.
    CHECK(supports("edit.smartObject", SampleType::U16));
    // Camera Raw, G'MIC, Remove Background, artboards, the timeline, the Eyedropper and the last exports.
    for (const char* ported : {"filter.Camera Raw", "filter.G'MIC", "edit.removeBackground", "edit.artboard", "tool.artboard", "edit.timeline",
                               "tool.eyedropper", "export.svg", "export.artboards", "export.slices"})
        CHECK(supports(ported, SampleType::U16));
    // An id the registry does not list stays 8-bit only.
    CHECK(supports("edit.transformSelection", SampleType::U8));
    CHECK(!supports("edit.transformSelection", SampleType::U16));
    CHECK(supports("filter.never-heard-of-it", SampleType::U8));
    CHECK(!supports("filter.never-heard-of-it", SampleType::U16));
    CHECK(!supports("filter.never-heard-of-it", SampleType::F32));
    size_t count = 0;
    const FeatureSupport* table = featureSupportTable(count);
    for (size_t i = 0; i < count; i++) CHECK(table[i].types & onlyEightBit);
}

TEST_CASE(depth_conversions_round_trip) {
    for (int v = 0; v < 256; v++) CHECK_EQ(int(narrow16(widen8(uint8_t(v)))), v);
    CHECK_EQ(int(widen8(255)), 32768);
    CHECK_EQ(int(to65535(32768)), 65535);
    CHECK_EQ(int(from65535(65535)), 32768);
    CHECK_EQ(int(from65535(1)), 1);   // 0.5 rounds up
    CHECK_EQ(int(mul15(32768, 12345)), 12345);
    auto eight = std::make_shared<Image>(3, 2);
    eight->fill(200, 100, 50, 255);
    eight->pixel(1, 1)[3] = 128; eight->pixel(1, 1)[0] = 100;
    auto deep = widenImage(*eight);
    CHECK(*narrowImage(*deep) == *eight);
    // A 16-bit ramp dithered to 8 bits keeps its mean where rounding would band it.
    Image16 ramp(64, 64);
    for (int y = 0; y < 64; y++) for (int x = 0; x < 64; x++) { uint16_t* p = ramp.pixel(x, y); p[0] = p[1] = p[2] = uint16_t(widen8(100) + 64); p[3] = 32768; }
    auto dithered = ditherToEightBit(ramp);
    double mean = 0;
    for (int y = 0; y < 64; y++) for (int x = 0; x < 64; x++) mean += dithered->pixel(x, y)[0];
    mean /= 64 * 64;
    CHECK(mean > 100.3 && mean < 100.7);   // 64 / 128.5 of a level above 100
}

TEST_CASE(documents_convert_between_eight_and_sixteen_bits) {
    Document doc(40, 30);
    auto image = std::make_shared<Image>(20, 10);
    image->fill(10, 20, 30, 255);
    Layer layer(Asset::make(image, "text"), Point(5, 5));
    layer.text = LayerText();
    layer.textImage = layer.asset->image;
    auto mask = std::make_shared<GrayImage>(20, 10, 77);
    LayerMask m; m.asset = MaskAsset::make(mask); layer.mask = m;
    doc.layers.push_back(layer);
    Selection selection;
    selection.coverage = std::make_shared<GrayImage>(40, 30, 0);
    doc.selection = selection;
    Document deep = doc;
    std::string error;
    REQUIRE(convertSampleType(deep, SampleType::U16, &error));
    CHECK(deep.sampleType == SampleType::U16);
    REQUIRE(deep.layers[0].asset->image.u16() != nullptr);
    CHECK(deep.layers[0].isLiveText());   // the text still owns its raster
    CHECK(deep.layers[0].mask->asset.image.u16() != nullptr);
    CHECK(deep.selection->coverage.u16() != nullptr);
    CHECK(deep.selection->isEmpty());
    CHECK_EQ(deep.layerBytes(), 20LL * 10 * 8);
    Document back = deep;
    REQUIRE(convertSampleType(back, SampleType::U8, &error));
    CHECK(*back.layers[0].asset->image.u8() == *image);
    CHECK(*back.layers[0].mask->asset.image.u8() == *mask);
    // A canvas an 8-bit document holds but a 16-bit one does not: refused, with the reason, and left as it was.
    Document big(10000, 6000);
    CHECK(!convertSampleType(big, SampleType::U16, &error));
    CHECK(big.sampleType == SampleType::U8);
    CHECK(error.find("16-bit") != std::string::npos);
    CHECK(Document::imagePixelBudget(SampleType::U16) * 2 == Document::pixelBudget);
    CHECK(!convertSampleType(doc, SampleType::F32, &error));
}

TEST_CASE(history_counts_sixteen_bit_pixels) {
    Document doc(64, 64);
    auto image = std::make_shared<Image16>(64, 32);
    doc.sampleType = SampleType::U16;
    doc.layers.push_back(Layer(Asset::make(Image16Ptr(image), "deep"), Point(0, 0)));
    DocumentHistory history;
    size_t expected = 0;
    for (int i = 0; i < 3; i++) {
        history.begin("Edit", doc, doc.layers[0].id);
        const Asset before = *doc.layers[0].asset;
        // Every pixel changes, so history keeps whole buffers rather than region patches.
        auto next = std::make_shared<Image16>(64, 32);
        const uint16_t value[4] = {uint16_t(1000 * (i + 1)), 0, 0, 32768};
        next->fill(value);
        doc.layers[0].asset = Asset::make(Image16Ptr(next), "deep");
        LayerMask mask; mask.asset = MaskAsset::make(Gray16Ptr(std::make_shared<Gray16>(64, 32, uint16_t(100 + i))));
        doc.layers[0].mask = mask;
        history.end(doc, doc.layers[0].id);
        // The replaced raster (and its thumbnail) is now only in the history; so is the mask from the edit before.
        expected += before.image.byteCount() + before.thumbnail->byteCount();
    }
    // Two masks replaced (the third is the live one), each 64 x 32 16-bit samples and an 8-bit thumbnail.
    expected += 2 * (64 * 32 * 2 + 64 * 32);
    CHECK_EQ(history.retainedBytes(doc), expected);
    CHECK(history.retainedBytes(doc) >= 3 * size_t(64 * 32 * 8));
}

TEST_MAIN()
