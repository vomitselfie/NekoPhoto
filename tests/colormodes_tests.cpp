// CMYK and Lab documents, step A (docs/high-bit-depth-plan.md, "P7 plan"): the ImageC8 buffer and the fourth AnyImage
// alternative, the accessors in colormodes.h, byte budgets by channel count, conforming buffers to a mode's format,
// the supports() mode axis, undo accounting, and the project round trip of CMYK and Lab documents at 8 and 16 bits.
#include "check.h"
#include "compositor/colormodes.h"
#include "compositor/depth.h"
#include "compositor/document.h"
#include "compositor/history.h"
#include "compositor/project.h"
#include "compositor/supports.h"
#include <filesystem>
#include <fstream>
#include <random>
#include <type_traits>

using namespace compositor;
namespace fs = std::filesystem;

static_assert(std::is_same_v<ImageC8::Sample, uint8_t>);
static_assert(!std::is_same_v<ImageC8, Image>);
static_assert(colorModeChannels(ColorMode::CMYK) == 5 && colorModeChannels(ColorMode::Lab) == 4 && colorModeChannels(ColorMode::RGB) == 4);
static_assert(!colorModeSupportsDepth(ColorMode::CMYK, SampleType::F32) && colorModeSupportsDepth(ColorMode::RGB, SampleType::F32));
static_assert(inkFromStored<SampleType::U8>(0) == 255 && storedFromInk<SampleType::U16>(0) == 32768);
static_assert(labOffset<SampleType::U8>() == 128 && labOffset<SampleType::U16>() == 16384);

namespace {

/// A CMYK buffer with a gradient of inks and alpha, premultiplied as held.
template <class Buffer>
std::shared_ptr<Buffer> cmykPattern(int w, int h) {
    using Sample = typename Buffer::Sample;
    constexpr uint32_t one = sizeof(Sample) == 1 ? 255 : 32768;
    auto out = std::make_shared<Buffer>(w, h, 5);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            Sample* p = out->pixel(x, y);
            const uint32_t alpha = (x + y) % 7 == 0 ? 0 : one - uint32_t((y * 37 + x) % 5) * (one / 9);
            for (int c = 0; c < 4; c++) {
                const uint32_t straight = uint32_t((x * 53 + y * 29 + c * 71) % int(one + 1));
                p[c] = Sample((straight * alpha + one / 2) / one);
            }
            p[4] = Sample(alpha);
        }
    return out;
}

/// A Lab buffer: L across, a and b around neutral, opaque (so the PNG round trip keeps every value).
template <class Buffer>
std::shared_ptr<Buffer> labPattern(int w, int h) {
    constexpr SampleType S = std::is_same_v<Buffer, Image> ? SampleType::U8 : SampleType::U16;
    using Sample = SampleOf<S>;
    auto out = std::make_shared<Buffer>(w, h);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            Sample* p = out->pixel(x, y);
            const Sample alpha = SampleTraits<S>::one;
            p[0] = storedLabL<S>(double(x) * 100.0 / (w - 1), alpha);
            p[1] = storedLabAB<S>(double(y - h / 2) * 7.0, alpha);
            p[2] = storedLabAB<S>(double(x - w / 2) * -5.0, alpha);
            p[3] = alpha;
        }
    return out;
}

fs::path scratchDir(const char* name) {
    const fs::path dir = fs::temp_directory_path() / (std::string(name) + "_" + std::to_string(std::random_device{}()));
    fs::create_directories(dir);
    return dir;
}

} // namespace

TEST_CASE(cmyk_eight_bit_is_its_own_alternative) {
    ImageC8 c(3, 2, 5);
    CHECK_EQ(c.channels(), 5);
    CHECK_EQ(c.stride(), 15);
    CHECK_EQ(c.byteCount(), size_t(30));
    auto shared = std::make_shared<ImageC8>(4, 4, 5);
    AnyImage a = ImageC8Ptr(shared);
    CHECK(bool(a));
    CHECK(a.sampleType() == SampleType::U8);   // an 8-bit buffer...
    CHECK(!a.u8());                            // ...that RGB code never sees
    CHECK(!a.u16());
    CHECK(a.c8().get() == shared.get());
    CHECK_EQ(a.channels(), 5);
    CHECK_EQ(a.byteCount(), size_t(4 * 4 * 5));
    CHECK(a.identity() == shared.get());
    AnyImage rgb = std::make_shared<Image>(2, 2);
    CHECK(!rgb.c8());
    CHECK_EQ(rgb.channels(), 4);
    AnyImage deepCmyk = Image16Ptr(std::make_shared<Image16>(2, 2, 5));
    CHECK_EQ(deepCmyk.channels(), 5);
    CHECK_EQ(visit([](const auto& p) { return p ? p->width() : -1; }, a), 4);
    AnyGray gray = std::make_shared<GrayImage>(2, 2);
    CHECK_EQ(gray.channels(), 1);
}

TEST_CASE(accessors_hide_inversion_and_offset) {
    // Inverted ink: stored 0 is full ink; premultiplied under half alpha.
    CHECK_EQ(int(inkValue<SampleType::U8>(0, 255)), 255);
    CHECK_EQ(int(inkValue<SampleType::U8>(255, 255)), 0);
    CHECK_EQ(int(inkValue<SampleType::U16>(storedInk<SampleType::U16>(8192, 16384), 16384)), 8192);
    CHECK_EQ(int(inkValue<SampleType::U8>(0, 0)), 0);
    for (int ink = 0; ink <= 255; ink++) CHECK_EQ(int(inkValue<SampleType::U8>(storedInk<SampleType::U8>(uint8_t(ink), 255), 255)), ink);
    CHECK_NEAR(inkPercent<SampleType::U16>(16384), 50.0, 1e-9);
    // Lab: a and b are signed, neutral at the offset, read straight.
    CHECK_NEAR(labA<SampleType::U8>(128, 255), 0.0, 1e-9);
    CHECK_NEAR(labA<SampleType::U8>(0, 255), -128.0, 1e-9);
    CHECK_NEAR(labB<SampleType::U16>(32768, 32768), 128.0, 1e-9);
    CHECK_NEAR(labA<SampleType::U16>(storedLabAB<SampleType::U16>(-20.5, 16384), 16384), -20.5, 0.01);
    CHECK_NEAR(labA<SampleType::U8>(0, 0), 0.0, 1e-9);   // transparent: neutral
    CHECK_NEAR(labL<SampleType::U16>(storedLabL<SampleType::U16>(62.5, 32768), 32768), 62.5, 0.01);
}

TEST_CASE(budgets_count_channels) {
    // Bytes: an 8-bit RGB image is the reference; CMYK holds four fifths the pixels, 16-bit CMYK two fifths.
    CHECK_EQ(Document::imagePixelBudget(SampleType::U8, ColorMode::RGB), Document::pixelBudget);
    CHECK_EQ(Document::imagePixelBudget(SampleType::U8, ColorMode::Lab), Document::pixelBudget);
    CHECK_EQ(Document::imagePixelBudget(SampleType::U8, ColorMode::CMYK), Document::pixelBudget * 4 / 5);
    CHECK_EQ(Document::imagePixelBudget(SampleType::U16, ColorMode::CMYK), Document::pixelBudget * 2 / 5);
    CHECK_EQ(Document::projectPixelBudgetAt(SampleType::U16, ColorMode::CMYK), Document::projectPixelBudget * 2 / 5);
    CHECK_EQ(Document::imagePixelBudget(SampleType::U16), Document::pixelBudget / 2);   // RGB unchanged
    CHECK_EQ(Document::maskPixelBudgetAt(SampleType::U8), Document::projectPixelBudget);
    // 9000 x 10000 is 90 megapixels: an 8-bit RGB canvas, not a CMYK one.
    CHECK(bool(Document::canCreate(9000, 10000, SampleType::U8)));
    const BudgetCheck cmyk = Document::canCreate(9000, 10000, SampleType::U8, ColorMode::CMYK);
    CHECK(!cmyk);
    CHECK(cmyk.kind == BudgetCheck::Image);
    CHECK(cmyk.message().find("CMYK") != std::string::npos);
    Document doc(9000, 10000);
    CHECK(formatBudgetProblem(doc, SampleType::U8, ColorMode::RGB).empty());
    CHECK(formatBudgetProblem(doc, SampleType::U8, ColorMode::CMYK).find("8-bit CMYK") != std::string::npos);
    doc.colorMode = ColorMode::CMYK;
    CHECK(!Document::withinBudget(doc));
    Document small(10, 10);
    small.colorMode = ColorMode::CMYK;
    small.layers.push_back(Layer(Asset::makeAny(ImageC8Ptr(cmykPattern<ImageC8>(10, 10)), "ink"), Point(0, 0)));
    CHECK_EQ(small.layerBytes(), 10LL * 10 * 5);
    small.sampleType = SampleType::U16;
    CHECK_EQ(small.layerBytes(), 10LL * 10 * 5 * 2);
    CHECK(!small.fitsMacBudget());
}

TEST_CASE(conform_to_format_changes_depth_within_a_mode) {
    Document doc(6, 5);
    doc.colorMode = ColorMode::CMYK;
    auto eight = cmykPattern<ImageC8>(6, 5);
    doc.layers.push_back(Layer(Asset::makeAny(ImageC8Ptr(eight), "ink"), Point(0, 0)));
    CHECK(doc.layers[0].asset->image.c8() != nullptr);
    CHECK(!conformToFormat(doc));                   // already the document's format
    std::string error;
    REQUIRE(convertSampleType(doc, SampleType::U16, &error));
    const Image16Ptr deep = doc.layers[0].asset->image.u16();
    REQUIRE(deep != nullptr);
    CHECK_EQ(deep->channels(), 5);
    CHECK_EQ(int(deep->pixel(1, 0)[4]), int(widen8(eight->pixel(1, 0)[4])));
    REQUIRE(convertSampleType(doc, SampleType::U8, &error));
    REQUIRE(doc.layers[0].asset->image.c8() != nullptr);
    CHECK(*doc.layers[0].asset->image.c8() == *eight);   // 8 -> 16 -> 8 is exact
    // An RGB buffer in a CMYK document is another mode's colours: left for Image > Mode.
    Document mixed(4, 4);
    mixed.colorMode = ColorMode::CMYK;
    mixed.layers.push_back(Layer(Asset::make(std::make_shared<Image>(4, 4), "rgb"), Point(0, 0)));
    CHECK(!conformToFormat(mixed));
    CHECK(mixed.layers[0].asset->image.u8() != nullptr);
    // Lab keeps a and b neutral across depths: 128 at 8 bits is 16384 at 16, and back.
    Document lab(9, 9);
    lab.colorMode = ColorMode::Lab;
    auto lab8 = labPattern<Image>(9, 9);
    lab.layers.push_back(Layer(Asset::make(lab8, "lab"), Point(0, 0)));
    REQUIRE(convertSampleType(lab, SampleType::U16, &error));
    const Image16Ptr lab16 = lab.layers[0].asset->image.u16();
    REQUIRE(lab16 != nullptr);
    CHECK_NEAR(labA<SampleType::U16>(lab16->pixel(4, 4)[1], lab16->pixel(4, 4)[3]), labA<SampleType::U8>(lab8->pixel(4, 4)[1], 255), 1e-9);
    CHECK_EQ(int(imageAtFormat(ImagePtr(lab8), SampleType::U16, ColorMode::Lab).u16()->pixel(0, 4)[1]), 16384 + int(labA<SampleType::U8>(lab8->pixel(0, 4)[1], 255)) * 128);
    REQUIRE(convertSampleType(lab, SampleType::U8, &error));
    CHECK(*lab.layers[0].asset->image.u8() == *lab8);
}

TEST_CASE(supports_has_a_mode_axis) {
    CHECK(supports("layers.structure", SampleType::U8, ColorMode::CMYK));
    CHECK(supports("layers.structure", SampleType::U16, ColorMode::Lab));
    CHECK(supports("document.save", SampleType::U16, ColorMode::CMYK));
    // Photoshop's exclusions and what is not ported yet are refused, whatever the depth supports.
    CHECK(supports("filter.G'MIC", SampleType::U8, ColorMode::RGB));
    CHECK(!supports("filter.G'MIC", SampleType::U8, ColorMode::CMYK));
    CHECK(!supports("filter.Camera Raw", SampleType::U16, ColorMode::Lab));
    // Step E: adjustments and filters in the document's own samples, each kind where Photoshop offers it.
    CHECK(supports("adjustment.pixels", SampleType::U8, ColorMode::CMYK));
    CHECK(supports("adjustment.Selective Color", SampleType::U16, ColorMode::CMYK));
    CHECK(!supports("adjustment.Selective Color", SampleType::U8, ColorMode::Lab));
    CHECK_EQ(unavailableReason("adjustment.Selective Color", SampleType::U8, ColorMode::Lab), std::string("Not available in Lab mode"));
    CHECK_EQ(unavailableReason("adjustment.Vibrance", SampleType::U16, ColorMode::CMYK), std::string("Not available in CMYK mode"));
    CHECK_EQ(unavailableReason("adjustment.Exposure", SampleType::U8, ColorMode::CMYK), std::string("Not available in CMYK mode"));
    CHECK(supports("adjustment.Exposure", SampleType::U8, ColorMode::Lab));
    CHECK(supports("adjustment.Color Lookup", SampleType::U8, ColorMode::CMYK) && supports("adjustment.Color Lookup", SampleType::U16, ColorMode::Lab));
    CHECK(supports("filter.Gaussian Blur", SampleType::U16, ColorMode::Lab));
    // Step E: painting in the document's own samples.
    CHECK(supports("tool.brush", SampleType::U8, ColorMode::CMYK));
    CHECK(supports("tool.brush", SampleType::U16, ColorMode::Lab));
    CHECK(!supports("brush.mypaint", SampleType::U8, ColorMode::CMYK));
    CHECK(supports("tool.spotHealing", SampleType::U8, ColorMode::CMYK));
    CHECK(supports("tool.spotHealing", SampleType::U8, ColorMode::Lab));
    // Greyed for good ("... mode") or until a port ("... mode yet").
    CHECK_EQ(unavailableReason("brush.mypaint", SampleType::U8, ColorMode::Lab), std::string("Not available in Lab mode"));
    CHECK_EQ(unavailableReason("filter.Mosh", SampleType::U8, ColorMode::CMYK), std::string("Not available in CMYK mode"));
    // Smart objects, artboards, slices, their exports and SVG (P9).
    for (const char* f : {"edit.smartObject", "edit.artboard", "tool.artboard", "tool.slice", "export.artboards", "export.slices", "export.svg", "edit.pixels"})
        for (ColorMode m : {ColorMode::CMYK, ColorMode::Lab}) CHECK(supports(f, SampleType::U8, m) && supports(f, SampleType::U16, m));
    // Text, shapes and layer styles (P8).
    for (const char* f : {"tool.text", "edit.text", "tool.shape", "tool.pen", "tool.directSelect", "edit.vector", "edit.paint", "edit.style"})
        for (ColorMode m : {ColorMode::CMYK, ColorMode::Lab}) CHECK(supports(f, SampleType::U8, m) && supports(f, SampleType::U16, m));
    CHECK(supports("tool.crop", SampleType::U16, ColorMode::CMYK) && supports("edit.clipboard", SampleType::U8, ColorMode::Lab));
    CHECK(supports("tool.dodge", SampleType::U16, ColorMode::CMYK));
    CHECK_EQ(unavailableReason("tool.dodge", SampleType::F32, ColorMode::RGB), std::string("Not available in 32-bit mode"));
    CHECK(unavailableReason("tool.brush", SampleType::U16, ColorMode::CMYK).empty());
    CHECK(!supports("filter.never-heard-of-it", SampleType::U8, ColorMode::Lab));
    CHECK(supports(AdjustmentKind(0), SampleType::U8, ColorMode::CMYK));
    CHECK(!supports(AdjustmentKind(0), SampleType::U8, ColorMode::Lab));
    CHECK(supports(AdjustmentKind(0), SampleType::U8, ColorMode::RGB));
    // A feature listed for a mode still needs the depth: transforming a selection is 8-bit RGB only.
    CHECK(!supports("edit.transformSelection", SampleType::U8, ColorMode::CMYK));
    CHECK_EQ(notAvailableInMode(ColorMode::CMYK), std::string("Not available in CMYK mode"));
    CHECK_EQ(notAvailableInMode(ColorMode::Lab), std::string("Not available in Lab mode"));
    CHECK(notAvailableInMode(ColorMode::RGB).empty());
    // Every feature the mode table lists is one the depth registry knows, or a navigation feature every depth has.
    size_t count = 0;
    const FeatureModes* table = featureModeTable(count);
    CHECK(count > 0);
    for (size_t i = 0; i < count; i++) CHECK(supports(table[i].feature, SampleType::U16));
}

TEST_CASE(history_counts_cmyk_channels) {
    Document doc(32, 16);
    doc.colorMode = ColorMode::CMYK;
    doc.layers.push_back(Layer(Asset::makeAny(ImageC8Ptr(cmykPattern<ImageC8>(32, 16)), "ink"), Point(0, 0)));
    DocumentHistory history;
    history.begin("Edit", doc, doc.layers[0].id);
    // A replacement that differs everywhere: identical pixels would be an empty region patch (history keeps only what
    // an edit changed), and this test is about whole buffers being counted at five samples a pixel.
    auto changed = cmykPattern<ImageC8>(32, 16);
    for (int y = 0; y < 16; y++) for (int x = 0; x < 32; x++) changed->pixel(x, y)[4] ^= 1;
    doc.layers[0].asset = Asset::makeAny(ImageC8Ptr(changed), "ink");
    history.end(doc, doc.layers[0].id);
    // Five samples a pixel, and the layer thumbnail drawn through the Working CMYK (32 x 16 RGBA, P7 step C).
    constexpr size_t thumbnail = 32 * 16 * 4;
    CHECK_EQ(history.retainedBytes(doc), size_t(32 * 16 * 5) + thumbnail);
    doc.sampleType = SampleType::U16;
    history.begin("Deep", doc, doc.layers[0].id);
    doc.layers[0].asset = Asset::makeAny(Image16Ptr(cmykPattern<Image16>(32, 16)), "ink");
    history.end(doc, doc.layers[0].id);
    CHECK_EQ(history.retainedBytes(doc), (size_t(32 * 16 * 5) + thumbnail) * 2);   // both 8-bit rasters are now history's alone
}

namespace {

template <class Check>
void roundTrip(const Document& doc, const char* name, Check&& check) {
    const fs::path dir = scratchDir(name);
    ProjectError error;
    REQUIRE(saveProject(doc, std::nullopt, (dir / "doc.comp").string(), error));
    std::string text;
    {
        // Closed before the folder is removed: Windows cannot delete an open file.
        std::ifstream manifest(dir / "doc.comp" / "manifest.json");
        text.assign((std::istreambuf_iterator<char>(manifest)), std::istreambuf_iterator<char>());
    }
    CHECK(text.find(std::string("\"colorMode\": \"") + colorModeKey(doc.colorMode) + "\"") != std::string::npos);
    CHECK(text.find("\"version\": 9") != std::string::npos);
    auto loaded = loadProject((dir / "doc.comp").string(), error);
    REQUIRE(loaded.has_value());
    CHECK(loaded->colorMode == doc.colorMode);
    CHECK(loaded->sampleType == doc.sampleType);
    check(*loaded, dir / "doc.comp");
    std::error_code ignored;
    fs::remove_all(dir, ignored);
}

} // namespace

TEST_CASE(project_round_trips_cmyk) {
    for (SampleType type : {SampleType::U8, SampleType::U16}) {
        Document doc(40, 30);
        doc.colorMode = ColorMode::CMYK;
        doc.sampleType = type;
        AnyImage pixels = type == SampleType::U8 ? AnyImage(ImageC8Ptr(cmykPattern<ImageC8>(40, 30))) : AnyImage(Image16Ptr(cmykPattern<Image16>(40, 30)));
        Layer layer(Asset::makeAny(pixels, "Plates"), Point(3, 4));
        LayerMask mask;
        mask.asset = type == SampleType::U8 ? MaskAsset::make(GrayPtr(std::make_shared<GrayImage>(40, 30, 200))) : MaskAsset::make(Gray16Ptr(std::make_shared<Gray16>(40, 30, 20000)));
        layer.mask = mask;
        doc.layers.push_back(layer);
        Layer group("Folder", doc.size());
        group.isGroup = true;
        doc.layers.push_back(group);
        roundTrip(doc, "colormodes_cmyk", [&](const Document& back, const fs::path& package) {
            REQUIRE(back.layers.size() == 2);
            const AnyImage& image = back.layers[0].asset->image;
            if (type == SampleType::U8) { REQUIRE(image.c8() != nullptr); CHECK(*image.c8() == *pixels.c8()); }
            else { REQUIRE(image.u16() != nullptr); CHECK(*image.u16() == *pixels.u16()); }
            CHECK(back.layers[0].mask && back.layers[0].mask->asset.image.sampleType() == type);
            CHECK(fs::exists(package / "images" / (back.layers[0].id + ".cmyk")));
            CHECK(!fs::exists(package / "images" / (back.layers[0].id + ".png")));
            CHECK(fs::exists(package / "images" / (back.layers[0].id + ".mask.png")));
        });
    }
}

TEST_CASE(project_round_trips_lab) {
    for (SampleType type : {SampleType::U8, SampleType::U16}) {
        Document doc(33, 21);
        doc.colorMode = ColorMode::Lab;
        doc.sampleType = type;
        AnyImage pixels = type == SampleType::U8 ? AnyImage(ImagePtr(labPattern<Image>(33, 21))) : AnyImage(Image16Ptr(labPattern<Image16>(33, 21)));
        doc.layers.push_back(Layer(Asset::makeAny(pixels, "Lab"), Point(0, 0)));
        roundTrip(doc, "colormodes_lab", [&](const Document& back, const fs::path& package) {
            REQUIRE(back.layers.size() == 1);
            const AnyImage& image = back.layers[0].asset->image;
            if (type == SampleType::U8) { REQUIRE(image.u8() != nullptr); CHECK(*image.u8() == *pixels.u8()); }
            else { REQUIRE(image.u16() != nullptr); CHECK(*image.u16() == *pixels.u16()); }
            CHECK(fs::exists(package / "images" / (back.layers[0].id + ".png")));
        });
    }
}

TEST_CASE(project_refuses_damaged_or_inconsistent_modes) {
    // An RGB document writes nothing new.
    Document rgb(8, 8);
    rgb.layers.push_back(Layer(Asset::make(std::make_shared<Image>(8, 8), "a"), Point(0, 0)));
    const std::string manifest = manifestJson(rgb, std::nullopt);
    CHECK(manifest.find("colorMode") == std::string::npos);
    CHECK(manifest.find("\"version\": 9") == std::string::npos);
    // A CMYK document whose layer holds RGB pixels is not saved.
    Document wrong(8, 8);
    wrong.colorMode = ColorMode::CMYK;
    wrong.layers.push_back(Layer(Asset::make(std::make_shared<Image>(8, 8), "a"), Point(0, 0)));
    const fs::path dir = scratchDir("colormodes_bad");
    ProjectError error;
    CHECK(!saveProject(wrong, std::nullopt, (dir / "wrong.comp").string(), error));
    // A colorMode key in an older version, or an unknown mode, is damage.
    ProjectError parseError;
    CHECK(!parseManifest(R"({"format":"com.compositor.project","version":8,"colorSpace":"sRGB","documentID":"00000000-0000-0000-0000-000000000001","width":4,"height":4,"layers":[],"colorMode":"cmyk"})", parseError));
    CHECK(!parseManifest(R"({"format":"com.compositor.project","version":9,"colorSpace":"sRGB","documentID":"00000000-0000-0000-0000-000000000001","width":4,"height":4,"layers":[],"colorMode":"hsv"})", parseError));
    // Damaged planes: a truncated stream, and a header that claims another size, are refused as missing images.
    Document cmyk(12, 12);
    cmyk.colorMode = ColorMode::CMYK;
    cmyk.layers.push_back(Layer(Asset::makeAny(ImageC8Ptr(cmykPattern<ImageC8>(12, 12)), "ink"), Point(0, 0)));
    REQUIRE(saveProject(cmyk, std::nullopt, (dir / "c.comp").string(), error));
    const fs::path planes = dir / "c.comp" / "images" / (cmyk.layers[0].id + ".cmyk");
    fs::resize_file(planes, 30);
    CHECK(!loadProject((dir / "c.comp").string(), error));
    REQUIRE(saveProject(cmyk, std::nullopt, (dir / "c.comp").string(), error));
    {
        std::fstream f(planes, std::ios::in | std::ios::out | std::ios::binary);
        f.seekp(8);
        const char big[4] = {char(0xff), char(0xff), 0, 0};
        f.write(big, 4);
    }
    CHECK(!loadProject((dir / "c.comp").string(), error));
    // The layers' bytes are limited in total: this document's planes are 12 x 12 x 5 bytes.
    REQUIRE(saveProject(cmyk, std::nullopt, (dir / "c.comp").string(), error));
    ProjectLoadLimits tight;
    tight.layerBytes = 12 * 12 * 5 - 1;
    CHECK(!loadProject((dir / "c.comp").string(), error, tight));
    CHECK(error.kind == ProjectError::TooLarge);
    tight.layerBytes = 12 * 12 * 5;
    CHECK(loadProject((dir / "c.comp").string(), error, tight).has_value());
    { std::error_code cleanup_; fs::remove_all(dir, cleanup_); }
}

TEST_MAIN()
