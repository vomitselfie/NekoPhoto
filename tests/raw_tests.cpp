// Camera RAW: which paths count as RAW, a file that is not one is refused, and (with LibRaw) the synthetic DNG in
// tests/fixtures/raw (tools/make_test_dng.py) develops as shot, half-size, graded, cancelled, and as a smart object
// that keeps its file and settings through a project and a PSD.
#include "check.h"
#include "compositor/project.h"
#include "compositor/psd.h"
#include "compositor/psd_writer.h"
#include "compositor/raw.h"
#include "compositor/render.h"
#include "compositor/smartobject_edit.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>

using namespace compositor;

namespace {

std::vector<uint8_t> fixture() {
    std::string error;
    return readRawFileBytes(RAW_FIXTURE, &error);
}

/// A pixel's channels on 0..1.
std::array<double, 3> at(const Image16& image, int x, int y) {
    const uint16_t* p = image.pixel(x, y);
    return {p[0] / 32768.0, p[1] / 32768.0, p[2] / 32768.0};
}

} // namespace

TEST_CASE(raw_extensions_are_recognised) {
    CHECK(isRawPath("/photos/IMG_0001.CR2"));
    CHECK(isRawPath("a.nef"));
    CHECK(isRawPath("b.Dng"));
    CHECK(!isRawPath("c.png"));
    CHECK(!isRawPath("no-extension"));
}

TEST_CASE(a_file_that_is_not_raw_is_refused) {
    const auto path = std::filesystem::temp_directory_path() / "nekophoto_not_raw.cr2";
    if (FILE* f = std::fopen(path.string().c_str(), "wb")) { std::fputs("Poser character, not a Canon RAW", f); std::fclose(f); }
    std::string error;
    CHECK(decodeRaw(path.string(), &error) == nullptr);
    CHECK(!error.empty());
    error.clear();
    const std::vector<uint8_t> junk{'n', 'o', 't', ' ', 'r', 'a', 'w'};
    CHECK(decodeRaw16(junk, {}, &error) == nullptr);
    CHECK(!error.empty());
    std::filesystem::remove(path);
}

TEST_CASE(the_fixture_develops_neutral_as_shot) {
    if (!rawSupported()) return;
    const auto bytes = fixture();
    REQUIRE(!bytes.empty());
    RawInfo info;
    std::string error;
    REQUIRE(readRawInfo(bytes, info, &error));
    CHECK_EQ(info.make, std::string("NekoPhoto"));
    CHECK_EQ(info.width, 128);
    CHECK_EQ(info.height, 96);
    auto image = decodeRaw16(bytes, {}, &error);
    REQUIRE(image);
    CHECK_EQ(image->width(), 128);
    CHECK_EQ(image->height(), 96);
    // The sensor saw grey as red 0.5, green 1, blue 0.7; the as-shot balance brings it back to grey.
    for (int x : {20, 64, 100}) {
        const auto c = at(*image, x, 20);
        CHECK_NEAR(c[0], c[1], 0.02);
        CHECK_NEAR(c[2], c[1], 0.02);
    }
    // The red patch is red, the blue one blue; every pixel opaque.
    const auto red = at(*image, 10, 70), blue = at(*image, 50, 70);
    CHECK(red[0] > red[1] + 0.3 && red[0] > red[2] + 0.3);
    CHECK(blue[2] > blue[0] + 0.3 && blue[2] > blue[1] + 0.2);
    CHECK_EQ(int(image->pixel(5, 5)[3]), 32768);
    // The 8-bit path agrees.
    const auto path = std::filesystem::temp_directory_path() / "nekophoto_raw_fixture.dng";
    std::filesystem::copy_file(RAW_FIXTURE, path, std::filesystem::copy_options::overwrite_existing);
    auto eight = decodeRaw(path.string(), &error);
    REQUIRE(eight);
    CHECK_NEAR(eight->pixel(64, 20)[1] / 255.0, at(*image, 64, 20)[1], 0.01);
    std::filesystem::remove(path);
}

TEST_CASE(the_quick_decode_is_half_size_and_a_cancel_stops_it) {
    if (!rawSupported()) return;
    const auto bytes = fixture();
    std::string error;
    RawDecodeOptions half;
    half.halfSize = true;
    auto small = decodeRaw16(bytes, half, &error);
    REQUIRE(small);
    CHECK_EQ(small->width(), 64);
    CHECK_EQ(small->height(), 48);
    std::atomic<bool> cancel{true};
    RawDecodeOptions cancelled;
    cancelled.cancel = &cancel;
    CHECK(developRaw(bytes, {}, cancelled, &error) == nullptr);
    CHECK_EQ(error, std::string("Cancelled."));
}

TEST_CASE(develop_applies_the_camera_raw_grade) {
    if (!rawSupported()) return;
    const auto bytes = fixture();
    std::string error;
    auto plain = developRaw(bytes, {}, {}, &error);
    REQUIRE(plain);
    CameraRawSettings brighter;
    brighter.exposure = 1;
    auto bright = developRaw(bytes, brighter, {}, &error);
    REQUIRE(bright);
    CHECK(at(*bright, 20, 20)[1] > at(*plain, 20, 20)[1] + 0.05);
    CameraRawSettings warm;
    warm.temperature = 50;
    auto warmer = developRaw(bytes, warm, {}, &error);
    REQUIRE(warmer);
    const auto c = at(*warmer, 64, 20);
    CHECK(c[0] > c[2] + 0.03);   // warmer than as shot
    // The same grade as the filter applied to the as-shot decode.
    Image16 filtered = *plain;
    applyCameraRaw(filtered, warm.normalized(), 1, 0);
    CHECK(filtered == *warmer);
}

TEST_CASE(a_raw_smart_object_keeps_its_file_and_settings) {
    if (!rawSupported()) return;
    auto bytes = std::make_shared<const std::vector<uint8_t>>(fixture());
    std::string error;
    CameraRawSettings settings;
    settings.exposure = 0.5;
    auto image = developRaw(*bytes, settings, {}, &error);
    REQUIRE(image);
    auto source = makeRawSmartObjectSource(bytes, "synthetic.dng", settings, Image16Ptr(image));
    REQUIRE(source);
    CHECK(source->isCameraRaw());
    Document doc = smartObjectDocument(source, SampleType::U16);
    CHECK_EQ(doc.width, 128);
    CHECK_EQ(doc.height, 96);
    REQUIRE(doc.layers.size() == 1);
    REQUIRE(doc.layers[0].isLiveSmartObject());
    CHECK_EQ(doc.layers[0].name, std::string("synthetic"));
    auto shown = renderFlattened16(doc);
    REQUIRE(shown);
    CHECK_EQ(int(shown->pixel(64, 20)[1]), int(image->pixel(64, 20)[1]));

    // The record round-trips with the settings; one without them stays version 2.
    auto record = serializeSmartObjectSource(*source);
    auto parsed = parseSmartObjectSource(record);
    REQUIRE(parsed);
    CHECK_EQ(parsed->rawSettings, source->rawSettings);
    CHECK(*parsed->bytes == *bytes);
    CameraRawSettings back;
    REQUIRE(CameraRawSettings::parse(parsed->rawSettings, back));
    CHECK_EQ(back.exposure, 0.5);
    SmartObjectSource plainSource = *source;
    plainSource.rawSettings.clear();
    const auto plainRecord = serializeSmartObjectSource(plainSource);
    CHECK_EQ(int(plainRecord[7]), 2);

    // Developed again: new pixels, the same file, the new settings, one source.
    CameraRawSettings darker;
    darker.exposure = -1;
    auto again = developRaw(*bytes, darker, {}, &error);
    REQUIRE(again);
    const std::string oldId = source->id;
    CHECK_EQ(redevelopRawSmartObject(doc, oldId, darker, Image16Ptr(again)), 1);
    CHECK_EQ(doc.smartObjects.size(), size_t(1));
    const auto& redeveloped = *doc.smartObjects.begin()->second;
    CHECK(redeveloped.id != oldId);
    CHECK(redeveloped.bytes == bytes);
    CHECK(CameraRawSettings::parse(redeveloped.rawSettings, back) && back.exposure == -1);
    auto darkerShown = renderFlattened16(doc);
    CHECK(darkerShown->pixel(64, 20)[1] < shown->pixel(64, 20)[1]);
    CHECK_EQ(redevelopRawSmartObject(doc, "no-such-source", darker, Image16Ptr(again)), 0);

    // A project keeps the file, the settings and the developed contents.
    const auto package = std::filesystem::temp_directory_path() / "nekophoto-raw-smartobject.comp";
    ProjectError projectError;
    REQUIRE(saveProject(doc, std::nullopt, package.string(), projectError));
    auto loaded = loadProject(package.string(), projectError);
    REQUIRE(loaded);
    REQUIRE(loaded->smartObjects.size() == 1);
    const auto& kept = *loaded->smartObjects.begin()->second;
    CHECK(kept.isCameraRaw());
    CHECK_EQ(kept.rawSettings, redeveloped.rawSettings);
    CHECK(*kept.bytes == *bytes);
    CHECK(kept.image.u16() != nullptr);
    std::filesystem::remove_all(package);

    // A PSD keeps the pixels and carries the RAW file as the embedded source.
    PsdExportSummary summary;
    auto psd = encodePsd(doc, {}, &summary, &error);
    REQUIRE(!psd.empty());
    auto imported = importPsdBytes(psd, &error);
    REQUIRE(imported);
    REQUIRE(imported->document.smartObjects.size() == 1);
    CHECK(*imported->document.smartObjects.begin()->second->bytes == *bytes);
    CHECK_EQ(imported->document.smartObjects.begin()->second->fileName, std::string("synthetic.dng"));
    auto psdShown = renderFlattened16(imported->document);
    REQUIRE(psdShown);
    CHECK(std::abs(int(psdShown->pixel(64, 20)[1]) - int(darkerShown->pixel(64, 20)[1])) <= 2);
}

TEST_MAIN()
