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
    { std::error_code cleanup_; std::filesystem::remove_all(package, cleanup_); }

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

TEST_CASE(the_fixture_reads_its_white_balance_in_kelvin) {
    if (!rawSupported()) return;
    const auto bytes = fixture();
    RawWhiteBalance balance;
    std::string error;
    REQUIRE(readRawWhiteBalance(bytes, balance, &error));
    REQUIRE(balance.kelvin);
    // AsShotNeutral 0.5, 1, 0.7 in an sRGB camera: multipliers 2, 1, 1/0.7. Less red than D65 and a strong green: a cool,
    // green light, above D65 on the green side of the locus.
    CHECK_NEAR(balance.asShot[0], 2, 1e-5);
    CHECK_NEAR(balance.asShot[2], 1 / 0.7, 1e-5);
    CHECK(balance.asShotValue.temperature > 6600 && balance.asShotValue.temperature < 8000);
    CHECK(balance.asShotValue.tint > 50);
    CHECK(balance.presets.empty());   // a DNG records no presets: none are made up
    // neutral -> xy -> (T, tint) -> xy -> neutral gives the as-shot multipliers back.
    const auto back = balance.multipliersFor(balance.asShotValue);
    REQUIRE(back);
    for (int c = 0; c < 3; c++) CHECK_NEAR((*back)[size_t(c)], balance.asShot[size_t(c)], 1e-9);
    std::printf("synthetic.dng as shot: %.0f K, tint %+.1f\n", balance.asShotValue.temperature, balance.asShotValue.tint);
}

TEST_CASE(the_dual_illuminant_fixture_interpolates_its_matrices) {
    if (!rawSupported()) return;
    std::string error;
    const auto bytes = readRawFileBytes(RAW_DUAL_FIXTURE, &error);
    RawWhiteBalance balance;
    REQUIRE(readRawWhiteBalance(bytes, balance, &error));
    REQUIRE(balance.kelvin);
    REQUIRE(balance.model.isDual());
    CHECK_EQ(balance.model.temperature1, 2856.0);
    CHECK_EQ(balance.model.temperature2, 6504.0);
    const auto back = balance.multipliersFor(balance.asShotValue);
    REQUIRE(back);
    for (int c = 0; c < 3; c++) CHECK_NEAR((*back)[size_t(c)], balance.asShot[size_t(c)], 1e-9);
    // Above D65 only the D65 matrix counts, so both fixtures read the as-shot neutral alike; a warm light's neutral
    // reads differently, because the Standard light A matrix takes part, and still round-trips.
    RawWhiteBalance single;
    REQUIRE(readRawWhiteBalance(fixture(), single, &error));
    CHECK_NEAR(single.asShotValue.temperature, balance.asShotValue.temperature, 1e-6);
    const std::array<double, 3> warm{1.1, 1, 2.6};
    const auto dualWarm = balance.valueOf(warm), singleWarm = single.valueOf(warm);
    REQUIRE(dualWarm && singleWarm);
    CHECK(dualWarm->temperature < 5000);
    CHECK(std::fabs(singleWarm->temperature - dualWarm->temperature) > 20);
    const auto warmBack = balance.multipliersFor(*dualWarm);
    REQUIRE(warmBack);
    for (int c = 0; c < 3; c++) CHECK_NEAR((*warmBack)[size_t(c)], warm[size_t(c)], 1e-9);
    std::printf("synthetic-dual.dng as shot: %.0f K, tint %+.1f\n", balance.asShotValue.temperature, balance.asShotValue.tint);
}

TEST_CASE(kelvin_white_balance_develops_through_the_multipliers) {
    if (!rawSupported()) return;
    const auto bytes = fixture();
    std::string error;
    RawWhiteBalance balance;
    REQUIRE(readRawWhiteBalance(bytes, balance, &error));
    auto asShot = developRaw(bytes, {}, {}, &error);
    REQUIRE(asShot);
    // As Shot with its kelvin readout develops exactly as the camera's own balance.
    CameraRawSettings shot;
    shot.whiteBalance = CameraRawWhiteBalance::AsShot;
    shot.rawTemperature = balance.asShotValue.temperature;
    shot.rawTint = balance.asShotValue.tint;
    CHECK(!balance.multipliersFor(shot.normalized()));
    auto same = developRaw(bytes, shot, {}, &error);
    REQUIRE(same);
    CHECK(*same == *asShot);
    // Custom at the as-shot readout: the round-tripped multipliers, within a level.
    shot.whiteBalance = CameraRawWhiteBalance::Custom;
    auto custom = developRaw(bytes, shot, {}, &error);
    REQUIRE(custom);
    for (int x : {20, 64, 100}) CHECK(std::abs(int(custom->pixel(x, 20)[0]) - int(asShot->pixel(x, 20)[0])) <= 1);
    // A higher temperature setting warms the picture; a positive tint turns it magenta.
    CameraRawSettings warmer = shot;
    warmer.rawTemperature = balance.asShotValue.temperature + 1500;
    auto warm = developRaw(bytes, warmer, {}, &error);
    REQUIRE(warm);
    const auto w = at(*warm, 64, 20);
    CHECK(w[0] > w[2] + 0.03);
    CameraRawSettings magenta = shot;
    magenta.rawTint = balance.asShotValue.tint + 40;
    auto pink = developRaw(bytes, magenta, {}, &error);
    REQUIRE(pink);
    const auto m = at(*pink, 64, 20);
    CHECK(m[0] > m[1] + 0.02 && m[2] > m[1] + 0.02);
    // The quick preview's rebalance of the as-shot decode lands near the exact decode.
    Image16 quick = *asShot;
    rebalanceRawDecode(quick, balance, balance.asShot, *balance.multipliersFor(warmer.normalized()));
    for (int c = 0; c < 3; c++) CHECK_NEAR(at(quick, 64, 20)[size_t(c)], w[size_t(c)], 0.03);
}

TEST_CASE(older_relative_settings_keep_developing_and_move_to_kelvin_in_the_dialog) {
    if (!rawSupported()) return;
    const auto bytes = fixture();
    std::string error;
    RawWhiteBalance balance;
    REQUIRE(readRawWhiteBalance(bytes, balance, &error));
    // A record written before kelvin white balance: relative temperature over the as-shot decode.
    CameraRawSettings old;
    REQUIRE(CameraRawSettings::parse(R"({"whiteBalance": "Custom", "temperature": 30, "tint": -10, "exposure": 0.2})", old));
    CHECK_EQ(old.rawTemperature, 0.0);
    auto legacy = developRaw(bytes, old, {}, &error);
    REQUIRE(legacy);
    auto plain = developRaw(bytes, {}, {}, &error);
    Image16 filtered = *plain;
    applyCameraRaw(filtered, old.normalized(), 1, 0);
    CHECK(filtered == *legacy);   // develops exactly as it always did
    // Opened in the dialog it becomes the white point its gains neutralize: the grey ramp keeps its colour.
    const CameraRawSettings moved = balance.inKelvin(old);
    CHECK(moved.rawTemperature > 0);
    CHECK_EQ(moved.temperature, 0.0);
    CHECK_EQ(moved.exposure, 0.2);
    auto kelvin = developRaw(bytes, moved, {}, &error);
    REQUIRE(kelvin);
    for (int c = 0; c < 3; c++) CHECK_NEAR(at(*kelvin, 64, 20)[size_t(c)], at(*legacy, 64, 20)[size_t(c)], 0.04);
    // Zero relative values are As Shot, exactly.
    CameraRawSettings zero;
    zero.exposure = 0.2;
    const CameraRawSettings asShot = balance.inKelvin(zero);
    CHECK(asShot.whiteBalance == CameraRawWhiteBalance::AsShot);
    auto same = developRaw(bytes, asShot, {}, &error);
    auto before = developRaw(bytes, zero, {}, &error);
    REQUIRE(same && before);
    CHECK(*same == *before);
}

TEST_CASE(automation_settings_settle_against_the_file) {
    if (!rawSupported()) return;
    const auto bytes = fixture();
    RawWhiteBalance balance;
    std::string error;
    REQUIRE(readRawWhiteBalance(bytes, balance, &error));
    // Nothing about white balance: As Shot, shown in kelvin.
    CameraRawSettings none;
    none.exposure = 0.3;
    CHECK_EQ(resolveRawWhiteBalance(bytes, none, false, false), std::string());
    CHECK(none.whiteBalance == CameraRawWhiteBalance::AsShot);
    CHECK_NEAR(none.rawTemperature, balance.asShotValue.temperature, 1e-9);
    // A temperature above 100 is kelvin; tint then absolute.
    CameraRawSettings kelvin;
    kelvin.temperature = 5500;
    kelvin.tint = 12;
    CHECK_EQ(resolveRawWhiteBalance(bytes, kelvin, true, true), std::string());
    CHECK_EQ(kelvin.rawTemperature, 5500.0);
    CHECK_EQ(kelvin.rawTint, 12.0);
    CHECK_EQ(kelvin.temperature, 0.0);
    // Within -100..100 it is the older relative form, left alone.
    CameraRawSettings relative;
    relative.temperature = 30;
    CHECK_EQ(resolveRawWhiteBalance(bytes, relative, true, false), std::string());
    CHECK_EQ(relative.temperature, 30.0);
    CHECK_EQ(relative.rawTemperature, 0.0);
    // A preset the file does not record is refused; out of range too.
    CameraRawSettings daylight;
    daylight.whiteBalance = CameraRawWhiteBalance::Daylight;
    CHECK(!resolveRawWhiteBalance(bytes, daylight, false, false).empty());
    CameraRawSettings hot;
    hot.temperature = 60000;
    CHECK(!resolveRawWhiteBalance(bytes, hot, true, false).empty());
    // Auto solves a white point: the fixture's grey is neutral as shot, so Auto lands near As Shot.
    CameraRawSettings automatic;
    automatic.whiteBalance = CameraRawWhiteBalance::Auto;
    CHECK_EQ(resolveRawWhiteBalance(bytes, automatic, false, false), std::string());
    CHECK(automatic.rawTemperature > 0);
    CHECK_NEAR(automatic.rawTemperature, balance.asShotValue.temperature, 400);
    // The settings keep the kelvin balance through JSON; older JSON without it stays relative.
    CameraRawSettings parsed;
    REQUIRE(CameraRawSettings::parse(kelvin.normalized().toJson(), parsed));
    CHECK_EQ(parsed.rawTemperature, 5500.0);
    CHECK_EQ(parsed.rawTint, 12.0);
    CHECK(CameraRawSettings().toJson().find("rawTemperature") == std::string::npos);
}

TEST_CASE(real_camera_files_round_trip_their_as_shot_balance) {
    // Local sample files (not in the repository): NEKOPHOTO_RAW_SAMPLES names the folder holding them.
    const char* folder = std::getenv("NEKOPHOTO_RAW_SAMPLES");
    if (!rawSupported() || !folder) return;
    for (const char* name : {"Canon_EOS_R_RAW_ISO_100.CR3", "DSCF0268.RAF", "_DSC0009.ARW"}) {
        const auto path = std::filesystem::path(folder) / name;
        if (!std::filesystem::exists(path)) continue;
        std::string error;
        const auto bytes = readRawFileBytes(path.string(), &error);
        RawWhiteBalance balance;
        REQUIRE(readRawWhiteBalance(bytes, balance, &error));
        REQUIRE(balance.kelvin);
        // camera neutral -> xy -> T/tint -> xy -> neutral: the same multipliers.
        const auto back = balance.multipliersFor(balance.asShotValue);
        REQUIRE(back);
        for (int c = 0; c < 3; c++) CHECK_NEAR((*back)[size_t(c)], balance.asShot[size_t(c)], 1e-9 * balance.asShot[size_t(c)]);
        // Each preset reads back to its own multipliers too.
        for (const auto& p : balance.presets) {
            const auto again = balance.multipliersFor(p.value);
            REQUIRE(again);
            for (int c = 0; c < 3; c++) CHECK_NEAR((*again)[size_t(c)], p.multipliers[size_t(c)], 1e-6 * p.multipliers[size_t(c)]);
        }
        std::printf("%s as shot: multipliers %.4f %.4f %.4f -> %.0f K, tint %+.1f; presets:", name, balance.asShot[0], balance.asShot[1],
                    balance.asShot[2], balance.asShotValue.temperature, balance.asShotValue.tint);
        for (const auto& p : balance.presets) std::printf(" %s %.0f K %+.1f,", cameraRawName(p.mode), p.value.temperature, p.value.tint);
        RawDecodeOptions quick;
        quick.halfSize = true;
        auto decoded = decodeRaw16(bytes, quick, &error);
        REQUIRE(decoded);
        const auto automatic = rawAutoWhiteBalance(*decoded, balance.asShot, balance);
        REQUIRE(automatic);
        std::printf(" Auto %.0f K %+.1f\n", automatic->temperature, automatic->tint);
    }
}

TEST_MAIN()
