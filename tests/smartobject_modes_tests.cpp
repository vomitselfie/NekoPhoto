// Smart objects, Smart Filters, artboards and SVG in CMYK and Lab documents (P9): contents placed in the document's
// layout (their own samples when they are of its mode and profile, else converted through the profiles once), layers
// converted to a smart object looking exactly as they did, warps and Smart Filters on the document's samples, and the
// PSD and project round trips keeping the instances live and CMYK contents as their inks.
#include "check.h"
#include "compositor/colormgmt.h"
#include "compositor/depth.h"
#include "compositor/modeedit.h"
#include "compositor/png.h"
#include "compositor/project.h"
#include "compositor/psd.h"
#include "compositor/psd_writer.h"
#include "compositor/render.h"
#include "compositor/smartfilter.h"
#include "compositor/smartobject_edit.h"
#include "compositor/svg.h"
#include "lcms2.h"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

using namespace compositor;

namespace {

/// An RGB image with smooth, saturated colour and a soft left edge.
std::shared_ptr<Image> paint(int w, int h) {
    auto image = std::make_shared<Image>(w, h);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint8_t* p = image->pixel(x, y);
            const unsigned a = x < 6 ? unsigned(x * 40) : 255;
            p[0] = uint8_t(((x * 255) / std::max(1, w - 1)) * a / 255);
            p[1] = uint8_t(((y * 255) / std::max(1, h - 1)) * a / 255);
            p[2] = uint8_t(90 * a / 255);
            p[3] = uint8_t(a);
        }
    return image;
}

std::shared_ptr<const SmartObjectSource> pngSource(int w, int h) {
    SmartObjectContents c;
    c.image = ImagePtr(paint(w, h));
    encodePngImage(*c.image.u8(), c.bytes);
    c.fileName = "paint.png";
    return makeSmartObjectSource(std::move(c));
}

/// A document of `mode` and `type`: an opaque base and a soft layer above it, converted with Image > Mode.
Document modeDocument(ColorMode mode, SampleType type) {
    Document doc(80, 60);
    auto base = std::make_shared<Image>(80, 60);
    for (int y = 0; y < 60; y++) for (int x = 0; x < 80; x++) { uint8_t* p = base->pixel(x, y); p[0] = uint8_t(x * 3); p[1] = uint8_t(200 - y * 2); p[2] = 140; p[3] = 255; }
    doc.layers.push_back(Layer(Asset::make(base, "Base"), Point(0, 0)));
    Layer soft(Asset::make(paint(40, 30), "Soft"), Point(12, 9));
    soft.opacity = 0.8;
    soft.blendMode = BlendMode::Multiply;
    doc.layers.push_back(soft);
    std::string error;
    if (!convertDocumentMode(doc, mode, ColorProfile(), ConvertOptions(), &error)) check::fail(__FILE__, __LINE__, "convertDocumentMode: " + error);
    if (type != SampleType::U8 && !convertSampleType(doc, type, &error)) check::fail(__FILE__, __LINE__, "convertSampleType: " + error);
    return doc;
}

bool sameSamples(const AnyImage& a, const AnyImage& b, int tolerance = 0) {
    if (!a || !b || a.width() != b.width() || a.height() != b.height() || a.channels() != b.channels() || a.sampleType() != b.sampleType()) return false;
    const int n = a.channels();
    for (int y = 0; y < a.height(); y++)
        for (int x = 0; x < a.width(); x++)
            for (int c = 0; c < n; c++) {
                int va = 0, vb = 0;
                if (a.c8()) { va = a.c8()->pixel(x, y)[c]; vb = b.c8()->pixel(x, y)[c]; }
                else if (a.u8()) { va = a.u8()->pixel(x, y)[c]; vb = b.u8()->pixel(x, y)[c]; }
                else if (a.u16()) { va = a.u16()->pixel(x, y)[c]; vb = b.u16()->pixel(x, y)[c]; }
                if (std::abs(va - vb) > tolerance) return false;
            }
    return true;
}

const ColorMode modes[] = {ColorMode::CMYK, ColorMode::Lab};
const SampleType depths[] = {SampleType::U8, SampleType::U16};

} // namespace

TEST_CASE(converted_layers_look_exactly_as_they_did) {
    for (ColorMode mode : modes)
        for (SampleType type : depths) {
            Document doc = modeDocument(mode, type);
            const AnyImage before = renderNative(doc);
            std::vector<Uuid> ids;
            for (const Layer& l : doc.layers) ids.push_back(l.id);
            std::string error;
            auto id = convertToSmartObject(doc, ids, &error);
            REQUIRE(id.has_value());
            REQUIRE(doc.layers.size() == 1);
            const Layer& layer = doc.layers[0];
            REQUIRE(layer.isLiveSmartObject());
            CHECK_EQ(layer.asset->image.channels(), colorModeChannels(mode));
            const SmartObjectSource& source = *doc.smartObjects.at(layer.smartObject->sourceId);
            // The child is a PSB of the document's mode; its samples are kept as they are.
            CHECK(source.nativeMode == mode && bool(source.native));
            CHECK(sameSamples(renderNative(doc), before));
            // Edit Contents opens it in its own mode.
            auto contents = smartObjectContentsDocument(doc, source.id);
            REQUIRE(contents.has_value());
            CHECK(contents->colorMode == mode && contents->sampleType == type);
        }
}

TEST_CASE(rgb_contents_are_converted_through_the_profiles_once) {
    for (ColorMode mode : modes)
        for (SampleType type : depths) {
            Document doc = modeDocument(mode, type);
            auto source = pngSource(30, 20);
            const Uuid id = placeSmartObject(doc, source, doc.layers.size(), std::nullopt);
            const Layer& layer = *doc.find(id);
            REQUIRE(layer.isLiveSmartObject());
            const AnyImage expected = convertImage(imageAtDepth(source->image, type), ColorMode::RGB, ColorProfile(), mode, doc.profile);
            CHECK(sameSamples(layer.asset->image, expected));
            CHECK(layer.smartImage == layer.asset->image);
            // Shared: a second instance takes the same converted buffer.
            const Uuid second = placeSmartObject(doc, source, doc.layers.size(), std::nullopt);
            CHECK(doc.find(second)->asset->image == layer.asset->image);
        }
}

TEST_CASE(warps_and_smart_filters_run_on_the_documents_samples) {
    for (ColorMode mode : modes)
        for (SampleType type : depths) {
            Document doc = modeDocument(mode, type);
            const Uuid id = placeSmartObject(doc, pngSource(30, 20), doc.layers.size(), std::nullopt);
            Layer& layer = *doc.find(id);
            const AnyImage flat = layer.asset->image;
            SmartFilterEntry blur;
            blur.parameters = smartfilter::GaussianBlur{2};
            std::string error;
            REQUIRE(addSmartFilter(doc, layer, blur, &error));
            CHECK(layer.isLiveSmartObject() && !layer.smartObject->locked() && smartObjectFiltered(*layer.smartObject));
            CHECK_EQ(layer.asset->image.channels(), colorModeChannels(mode));
            CHECK(layer.asset->image.sampleType() == type);
            CHECK(!sameSamples(layer.asset->image, flat));
            // The filter cache record holds the document's channels: four inks in CMYK, L, a and b in Lab.
            REQUIRE(doc.psdCarry);
            CHECK(findSmartFilterCache(doc.psdCarry->globals, layer.smartObject->placedId).has_value());
            // Plastic Wrap is a Filter Gallery filter, RGB only in Photoshop.
            SmartFilterEntry wrap;
            wrap.parameters = smartfilter::PlasticWrap{9, 7, 5};
            CHECK(!addSmartFilter(doc, layer, wrap, &error));
            CHECK(error.find("not available in") != std::string::npos);
            // Moving the instance draws it again from its contents.
            layer.transform.origin.x += 7;
            CHECK_EQ(refreshSmartObjectRasters(doc), 1);
            CHECK_EQ(layer.asset->image.channels(), colorModeChannels(mode));

            Document warped = modeDocument(mode, type);
            const Uuid w = placeSmartObject(warped, pngSource(30, 20), warped.layers.size(), std::nullopt);
            REQUIRE(warpLayer(warped, *warped.find(w), TextWarp{"warpArc", 40, 0, 0, false}, &error));
            CHECK(warped.find(w)->isLiveSmartObject());
            CHECK_EQ(warped.find(w)->asset->image.channels(), colorModeChannels(mode));
        }
}

TEST_CASE(cmyk_smart_filters_treat_black_like_the_other_inks) {
    // A flat grey of black ink alone, blurred: inside, every ink keeps its value; the passes for C, M, Y and for K agree
    // on where the edge softens (the same alpha).
    Document doc(40, 30);
    doc.colorMode = ColorMode::CMYK;
    auto ink = std::make_shared<ImageC8>(20, 10, 5);
    for (int y = 0; y < 10; y++) for (int x = 0; x < 20; x++) { uint8_t* p = ink->pixel(x, y); p[0] = p[1] = p[2] = 255; p[3] = 128; p[4] = 255; }
    SmartObjectContents c;
    c.image = ImagePtr(paint(20, 10));
    c.native = ImageC8Ptr(ink);
    c.nativeMode = ColorMode::CMYK;
    c.bytes = {1};
    c.fileName = "ink.psb";
    auto source = makeSmartObjectSource(std::move(c));
    REQUIRE(source && source->native);
    const Uuid id = placeSmartObject(doc, source, 0, std::nullopt);
    Layer& layer = *doc.find(id);
    REQUIRE(layer.asset->image.c8());
    CHECK_EQ(int(layer.asset->image.c8()->pixel(10, 5)[3]), 128);   // the inks as they are
    SmartFilterEntry blur;
    blur.parameters = smartfilter::GaussianBlur{1.5};
    std::string error;
    REQUIRE(addSmartFilter(doc, layer, blur, &error));
    const ImageC8Ptr& out = layer.asset->image.c8();
    REQUIRE(out);
    const int cx = out->width() / 2, cy = out->height() / 2;
    CHECK_EQ(int(out->pixel(cx, cy)[0]), 255);
    CHECK_EQ(int(out->pixel(cx, cy)[3]), 128);
    CHECK_EQ(int(out->pixel(cx, cy)[4]), 255);
    for (int y = 0; y < out->height(); y++)
        for (int x = 0; x < out->width(); x++) {
            const uint8_t* p = out->pixel(x, y);
            // Premultiplied: no ink sample above its alpha, and C equals alpha (no cyan) everywhere.
            CHECK(p[0] <= p[4] && p[3] <= p[4]);
            CHECK_EQ(int(p[0]), int(p[4]));
        }
}

TEST_CASE(cmyk_smart_objects_survive_psd_and_projects) {
    for (SampleType type : depths) {
        Document doc = modeDocument(ColorMode::CMYK, type);
        std::vector<Uuid> ids{doc.layers[1].id};
        std::string error;
        REQUIRE(convertToSmartObject(doc, ids, &error));
        placeSmartObject(doc, pngSource(30, 20), doc.layers.size(), std::nullopt);
        const AnyImage before = renderNative(doc);
        // PSD: both instances come back live, the converted one's contents as their inks.
        auto bytes = encodePsd(doc, {}, nullptr, &error);
        REQUIRE(!bytes.empty());
        auto back = importPsdBytes(bytes, &error);
        REQUIRE(back.has_value());
        CHECK(back->document.colorMode == ColorMode::CMYK);
        int live = 0, native = 0;
        for (const Layer& l : back->document.layers)
            if (l.isLiveSmartObject() && !l.smartObject->locked()) {
                live++;
                const SmartObjectSource& s = *back->document.smartObjects.at(l.smartObject->sourceId);
                if (s.nativeMode == ColorMode::CMYK && s.native) native++;
            }
        CHECK_EQ(live, 2);
        CHECK_EQ(native, 1);
        CHECK(sameSamples(renderNative(back->document), before, type == SampleType::U8 ? 1 : 2));
        // Moved after the round trip: drawn again from the contents, still CMYK.
        Layer& moved = back->document.layers.back();
        moved.transform.origin.x += 3;
        refreshSmartObjectRasters(back->document);
        CHECK_EQ(moved.asset->image.channels(), 5);
        // A project keeps the contents' mode and reads their inks again from the file.
        const auto package = std::filesystem::temp_directory_path() / ("nekophoto-cmykso-" + std::to_string(std::rand()) + ".comp");
        ProjectError perror;
        REQUIRE(saveProject(doc, std::nullopt, package.string(), perror));
        auto loaded = loadProject(package.string(), perror);
        REQUIRE(loaded.has_value());
        int nativeLoaded = 0;
        for (auto& [id, s] : loaded->smartObjects)
            if (s->nativeMode == ColorMode::CMYK && s->native) {
                nativeLoaded++;
                // Read back from the PSB's merged image: straight colour there, so soft edges round by a level or two.
                CHECK(sameSamples(s->native, doc.smartObjects.at(id)->native, 2));
            }
        CHECK_EQ(nativeLoaded, 1);
        CHECK(sameSamples(renderNative(*loaded), before));
        { std::error_code cleanup; std::filesystem::remove_all(package, cleanup); }
    }
}

TEST_CASE(new_smart_object_via_copy_has_contents_of_its_own) {
    for (ColorMode mode : {ColorMode::RGB, ColorMode::CMYK, ColorMode::Lab}) {
        Document doc = mode == ColorMode::RGB ? Document(80, 60) : modeDocument(mode, SampleType::U8);
        auto source = pngSource(30, 20);
        const Uuid id = placeSmartObject(doc, source, doc.layers.size(), std::nullopt);
        SmartFilterEntry blur;
        blur.parameters = smartfilter::GaussianBlur{2};
        std::string error;
        REQUIRE(addSmartFilter(doc, *doc.find(id), blur, &error));
        auto copyId = newSmartObjectViaCopy(doc, id, "paint copy", &error);
        REQUIRE(copyId.has_value());
        const Layer& original = *doc.find(id);
        const Layer& copy = *doc.find(*copyId);
        CHECK_EQ(doc.indexOf(*copyId), doc.indexOf(id) + 1);
        CHECK(copy.name == "paint copy" && copy.isLiveSmartObject() && !copy.smartObject->locked());
        CHECK(copy.smartObject->sourceId != original.smartObject->sourceId);
        CHECK(copy.smartObject->placedId != original.smartObject->placedId);
        CHECK(smartObjectFiltered(*copy.smartObject));
        REQUIRE(doc.psdCarry);
        CHECK(findSmartFilterCache(doc.psdCarry->globals, copy.smartObject->placedId).has_value());
        CHECK(copy.asset->image == original.asset->image);
        // Replacing the original's contents leaves the copy's.
        const AnyImage before = copy.asset->image;
        CHECK_EQ(replaceSmartObjectSource(doc, original.smartObject->sourceId, pngSource(20, 20)), 1);
        CHECK(doc.find(*copyId)->asset->image == before);
        // A PSD keeps both, each with its own contents.
        auto back = importPsdBytes(encodePsd(doc, {}, nullptr, &error), &error);
        REQUIRE(back.has_value());
        int live = 0;
        for (const Layer& l : back->document.layers) live += l.isLiveSmartObject() && !l.smartObject->locked();
        CHECK_EQ(live, 2);
        CHECK_EQ(int(back->document.smartObjects.size()), 2);
    }
}

TEST_CASE(replace_and_rasterize_in_lab) {
    Document doc = modeDocument(ColorMode::Lab, SampleType::U16);
    auto source = pngSource(30, 20);
    const Uuid id = placeSmartObject(doc, source, doc.layers.size(), std::nullopt);
    CHECK_EQ(replaceSmartObjectSource(doc, source->id, pngSource(20, 20)), 1);
    Layer& layer = *doc.find(id);
    CHECK(layer.isLiveSmartObject());
    CHECK(layer.asset->image.u16() && layer.asset->image.channels() == 4);
    const AnyImage shown = layer.asset->image;
    rasterizeSmartObject(layer);
    CHECK(!layer.smartObject && layer.asset->image == shown);
}

TEST_CASE(color_lookup_takes_abstract_profiles_in_cmyk_and_lab) {
    // A 3DLUT file applies in RGB only; Color Lookup itself is offered in both modes.
    ColorLookupSettings cube;
    cube.format = "cube";
    cube.data = "LUT_3D_SIZE 2\n0 0 0\n1 0 0\n0 1 0\n1 1 0\n0 0 1\n1 0 1\n0 1 1\n1 1 1\n";
    CHECK(colorLookupReadable(cube));
    CHECK(!colorLookupAppliesInMode(cube, ColorMode::CMYK, ColorProfile()));
    CHECK(colorLookupAppliesInMode(cube, ColorMode::RGB, ColorProfile()));
    CHECK(adjustmentAppliesInMode(AdjustmentKind::ColorLookup, ColorMode::Lab));
    // An abstract profile that brightens (Little CMS's BCHSW): L rises in Lab, ink falls in CMYK.
    cmsHPROFILE bright = cmsCreateBCHSWabstractProfile(17, 20, 1, 0, 0, 0, 0);
    REQUIRE(bright != nullptr);
    cmsUInt32Number size = 0;
    cmsSaveProfileToMem(bright, nullptr, &size);
    std::vector<uint8_t> bytes(size);
    cmsSaveProfileToMem(bright, bytes.data(), &size);
    cmsCloseProfile(bright);
    AdjustmentSettings lookup = AdjustmentSettings::defaults(AdjustmentKind::ColorLookup);
    lookup.colorLookup.format = "icc";
    lookup.colorLookup.name = "bright.icc";
    lookup.colorLookup.data = toBase64(bytes);
    for (ColorMode mode : modes)
        for (SampleType type : depths) {
            Document doc = modeDocument(mode, type);
            CHECK(colorLookupAppliesInMode(lookup.colorLookup, mode, doc.profile));
            const AnyImage& base = doc.layers[0].asset->image;
            const AnyImage out = adjustedInMode(lookup, base, mode, doc.profile);
            REQUIRE(bool(out));
            CHECK_EQ(out.channels(), base.channels());
            auto first = [](const AnyImage& i) { return i.c8() ? int(i.c8()->pixel(5, 5)[0]) : i.u8() ? int(i.u8()->pixel(5, 5)[0]) : int(i.u16()->pixel(5, 5)[0]); };
            // Stored values: CMYK's are inverted ink (less ink, higher), Lab's L: both rise.
            CHECK(first(out) > first(base));
        }
}

TEST_CASE(svg_export_converts_colours_to_srgb) {
    Document doc = modeDocument(ColorMode::CMYK, SampleType::U8);
    const auto path = std::filesystem::temp_directory_path() / ("nekophoto-cmyk-" + std::to_string(std::rand()) + ".svg");
    SvgExportSummary summary;
    std::string error;
    REQUIRE(exportSvg(doc, path.string(), &summary, &error));
    std::ifstream in(path);
    std::stringstream text;
    text << in.rdbuf();
    in.close();   // Windows removes no open file
    CHECK(text.str().find("<svg") != std::string::npos);
    CHECK_EQ(summary.images, 2);
    { std::error_code cleanup; std::filesystem::remove(path, cleanup); }
}

TEST_MAIN()
