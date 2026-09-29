// CMYK and Lab colour management (P7 step B, colormgmt.h): the bundled Working CMYK and the built-in Lab profile, the
// CMYKA and LabA layouts against Little CMS itself, round trips, premultiplied pixels, the fused reduction to 8-bit
// RGBA from any layout, converting buffers between modes, the CMYK soft proof of an RGB document, and Convert to
// Profile on a CMYK document.
#include "check.h"
#include "compositor/colormgmt.h"
#include "compositor/colormodes.h"
#include "compositor/depth.h"
#include "compositor/document.h"
#include "lcms2.h"
#include <array>
#include <cmath>
#include <cstring>

using namespace compositor;

namespace {

std::vector<std::array<uint8_t, 3>> sampleColours() {
    std::vector<std::array<uint8_t, 3>> out;
    for (int r = 0; r <= 255; r += 51) for (int g = 0; g <= 255; g += 51) for (int b = 0; b <= 255; b += 51) out.push_back({uint8_t(r), uint8_t(g), uint8_t(b)});
    return out;
}

/// Little CMS directly with the given layouts and the flags colormgmt uses (relative colorimetric, black point
/// compensation, no cache): the reference for the staged layouts.
template <class In, class Out>
void lcmsDirect(const ColorProfile& from, cmsUInt32Number inFormat, const ColorProfile& to, cmsUInt32Number outFormat, const In* in, Out* out, size_t count) {
    cmsHPROFILE a = cmsOpenProfileFromMem(from.icc.data(), cmsUInt32Number(from.icc.size()));
    cmsHPROFILE b = cmsOpenProfileFromMem(to.icc.data(), cmsUInt32Number(to.icc.size()));
    cmsHTRANSFORM t = cmsCreateTransform(a, inFormat, b, outFormat, INTENT_RELATIVE_COLORIMETRIC, cmsFLAGS_BLACKPOINTCOMPENSATION | cmsFLAGS_NOCACHE);
    cmsDoTransform(t, in, out, cmsUInt32Number(count));
    cmsDeleteTransform(t); cmsCloseProfile(a); cmsCloseProfile(b);
}

/// Opaque RGBA8 pixels of the sample colours.
Image rgbaSamples() {
    const auto colours = sampleColours();
    Image image(int(colours.size()), 1);
    for (size_t i = 0; i < colours.size(); i++) {
        uint8_t* p = image.pixel(int(i), 0);
        p[0] = colours[i][0]; p[1] = colours[i][1]; p[2] = colours[i][2]; p[3] = 255;
    }
    return image;
}

} // namespace

TEST_CASE(bundled_profiles) {
    const ColorProfile& cmyk = defaultCmykProfile();
    CHECK_EQ(cmyk.icc.size(), size_t(1052612));
    CHECK(cmyk.model == ColorModel::CMYK);
    CHECK(cmyk.description.find("ISO Coated v2 300%") != std::string::npos);
    const ColorProfile& lab = labProfile();
    CHECK(lab.model == ColorModel::Lab);
    CHECK_EQ(lab.description, std::string("Lab D50"));
    auto again = profileFromIcc(lab.icc);
    REQUIRE(again.has_value());
    CHECK(again->model == ColorModel::Lab);
    CHECK_EQ(again->description, std::string("Lab D50"));
    // Stable bytes: the fixed creation date is in the header, and the profile ID is set.
    REQUIRE(lab.icc.size() > 128);
    CHECK_EQ(int(lab.icc[24]) << 8 | lab.icc[25], 2026);
    bool id = false;
    for (int i = 84; i < 100; i++) id |= lab.icc[size_t(i)] != 0;
    CHECK(id);
    // Untagged values of each model stand for these.
    CHECK(&effectiveProfile(ColorProfile{}, ColorModel::CMYK) == &defaultCmykProfile());
    CHECK(&effectiveProfile(ColorProfile{}, ColorModel::Lab) == &labProfile());
    CHECK(&effectiveProfile(ColorProfile{}, ColorModel::RGB) == &srgbProfile());
    CHECK(pixelFormatFor(SampleType::U8, ColorMode::CMYK) == PixelFormat::CMYKA8);
    CHECK(pixelFormatFor(SampleType::U16, ColorMode::Lab) == PixelFormat::LabA16);
    CHECK(pixelFormatFor(SampleType::U16, ColorMode::RGB) == PixelFormat::RGBA16);
    CHECK_EQ(pixelFormatChannels(PixelFormat::CMYKA16), 5);
    // A profile of another model than its layout is refused.
    CHECK(!transformBetween(srgbProfile(), srgbProfile(), {}, PixelFormat::CMYKA8, PixelFormat::RGBA8));
    CHECK(!transformBetween({}, defaultCmykProfile(), {}, PixelFormat::RGBA8, PixelFormat::RGBA8));
    CHECK(!transformBetween({}, {}, {}, PixelFormat::CMYKA8, PixelFormat::CMYKA8));   // the same colours: nothing to do
}

TEST_CASE(cmyk_eight_bit_matches_lcms) {
    // sRGB to the Working CMYK and back, 8 bits: our inverted, alpha-carrying layout gives exactly what Little CMS gives
    // for its reversed CMYK layout, and the round trip is Little CMS's own.
    const Image rgb = rgbaSamples();
    const size_t n = size_t(rgb.width());
    auto toCmyk = transformBetween({}, defaultCmykProfile(), {}, PixelFormat::RGBA8, PixelFormat::CMYKA8);
    auto toRgb = transformBetween(defaultCmykProfile(), {}, {}, PixelFormat::CMYKA8, PixelFormat::RGBA8);
    REQUIRE(toCmyk && toRgb);
    ImageC8 cmyk(int(n), 1, 5);
    toCmyk->apply(rgb.row(0), cmyk.row(0), n);
    std::vector<uint8_t> straight(n * 3), reference(n * 4), back(n * 3);
    for (size_t i = 0; i < n; i++) std::memcpy(&straight[i * 3], rgb.pixel(int(i), 0), 3);
    lcmsDirect(srgbProfile(), TYPE_RGB_8, defaultCmykProfile(), TYPE_CMYK_8_REV, straight.data(), reference.data(), n);
    int same = 0;
    for (size_t i = 0; i < n; i++) {
        for (int c = 0; c < 4; c++) same += cmyk.pixel(int(i), 0)[c] == reference[i * 4 + size_t(c)];
        CHECK_EQ(int(cmyk.pixel(int(i), 0)[4]), 255);
    }
    CHECK_EQ(same, int(n * 4));
    // White is no ink (all 255 inverted); black is heavy ink.
    CHECK_EQ(int(cmyk.pixel(int(n) - 1, 0)[0]), 255);
    CHECK_EQ(int(cmyk.pixel(int(n) - 1, 0)[3]), 255);
    CHECK(inkValue<SampleType::U8>(cmyk.pixel(0, 0)[3], 255) > 200);
    Image rgbBack(int(n), 1);
    toRgb->apply(cmyk.row(0), rgbBack.row(0), n);
    lcmsDirect(defaultCmykProfile(), TYPE_CMYK_8_REV, srgbProfile(), TYPE_RGB_8, reference.data(), back.data(), n);
    int maxLoss = 0;
    for (size_t i = 0; i < n; i++)
        for (int c = 0; c < 3; c++) {
            CHECK_EQ(int(rgbBack.pixel(int(i), 0)[c]), int(back[i * 3 + size_t(c)]));
            maxLoss = std::max(maxLoss, std::abs(int(rgbBack.pixel(int(i), 0)[c]) - int(rgb.pixel(int(i), 0)[c])));
        }
    // In-gamut colours come back close; the loss is the press's gamut and Little CMS's tables, and is exactly theirs.
    const uint8_t grey[4] = {128, 128, 128, 255};
    uint8_t greyCmyk[5] = {}, greyBack[4] = {};
    toCmyk->apply(grey, greyCmyk, 1);
    toRgb->apply(greyCmyk, greyBack, 1);
    for (int c = 0; c < 3; c++) CHECK(std::abs(int(greyBack[c]) - 128) <= 2);
    CHECK(maxLoss > 0);   // sRGB's saturated corners are out of FOGRA39's gamut
}

TEST_CASE(cmyk_sixteen_bit_matches_lcms_float) {
    const auto colours = sampleColours();
    const size_t n = colours.size();
    Image16 rgb(int(n), 1);
    std::vector<float> straight(n * 3), reference(n * 4);
    for (size_t i = 0; i < n; i++)
        for (int c = 0; c < 4; c++) {
            const uint16_t v = c == 3 ? uint16_t(32768) : widen8(colours[i][size_t(c)]);
            rgb.pixel(int(i), 0)[c] = v;
            if (c < 3) straight[i * 3 + size_t(c)] = float(v) / 32768.0f;
        }
    auto toCmyk = transformBetween({}, defaultCmykProfile(), {}, PixelFormat::RGBA16, PixelFormat::CMYKA16);
    auto toRgb = transformBetween(defaultCmykProfile(), {}, {}, PixelFormat::CMYKA16, PixelFormat::RGBA16);
    REQUIRE(toCmyk && toRgb);
    Image16 cmyk(int(n), 1, 5);
    toCmyk->apply(rgb.row(0), cmyk.row(0), n);
    lcmsDirect(srgbProfile(), TYPE_RGB_FLT, defaultCmykProfile(), TYPE_CMYK_FLT, straight.data(), reference.data(), n);
    double worst = 0;
    for (size_t i = 0; i < n; i++)
        for (int c = 0; c < 4; c++) {
            const double ink = inkPercent<SampleType::U16>(inkValue<SampleType::U16>(cmyk.pixel(int(i), 0)[c], 32768));
            worst = std::max(worst, std::abs(ink - std::clamp(double(reference[i * 4 + size_t(c)]), 0.0, 100.0)));
        }
    CHECK(worst <= 100.0 / 32768.0 + 1e-6);   // within one 15-bit step of Little CMS's own float result
    // Back to RGB at 16 bits: what Little CMS's float pipeline gives for the inks as we hold them (rounded to 15 bits),
    // within a 15-bit step. The round trip itself is the press's gamut and Little CMS's tables.
    Image16 back(int(n), 1);
    toRgb->apply(cmyk.row(0), back.row(0), n);
    std::vector<float> reference2(n * 3), inks(n * 4);
    for (size_t i = 0; i < n; i++)
        for (int c = 0; c < 4; c++) inks[i * 4 + size_t(c)] = float(inkPercent<SampleType::U16>(inkValue<SampleType::U16>(cmyk.pixel(int(i), 0)[c], 32768)));
    lcmsDirect(defaultCmykProfile(), TYPE_CMYK_FLT, srgbProfile(), TYPE_RGB_FLT, inks.data(), reference2.data(), n);
    double worstBack = 0;
    for (size_t i = 0; i < n; i++)
        for (int c = 0; c < 3; c++)
            worstBack = std::max(worstBack, std::abs(double(back.pixel(int(i), 0)[c]) / 32768.0 - std::clamp(double(reference2[i * 3 + size_t(c)]), 0.0, 1.0)));
    CHECK(worstBack <= 1.0 / 32768.0);
    // In-gamut grey survives the round trip to within a few 15-bit steps.
    const uint16_t grey[4] = {16384, 16384, 16384, 32768};
    uint16_t greyCmyk[5] = {}, greyBack[4] = {};
    toCmyk->apply(grey, greyCmyk, 1);
    toRgb->apply(greyCmyk, greyBack, 1);
    for (int c = 0; c < 3; c++) CHECK(std::abs(int(greyBack[c]) - 16384) <= 64);
}

TEST_CASE(premultiplied_cmyk_follows_alpha) {
    // A half-transparent pixel converts as its opaque colour, premultiplied with the same alpha, at both depths.
    auto toCmyk = transformBetween({}, {}, {}, PixelFormat::RGBA8, PixelFormat::CMYKA8);
    REQUIRE(toCmyk);
    const uint8_t opaque[4] = {200, 60, 30, 255}, half[4] = {100, 30, 15, 128};
    uint8_t a[5] = {}, b[5] = {};
    toCmyk->apply(opaque, a, 1);
    toCmyk->apply(half, b, 1);
    CHECK_EQ(int(b[4]), 128);
    for (int c = 0; c < 4; c++) CHECK(std::abs(int(b[c]) - (a[c] * 128 + 127) / 255) <= 2);
    const uint8_t clear[4] = {0, 0, 0, 0};
    toCmyk->apply(clear, b, 1);
    for (int c = 0; c < 5; c++) CHECK_EQ(int(b[c]), 0);
    // In place, CMYK to CMYK through a proof-free identity is nothing to do; RGB round trips stay premultiplied.
    auto back16 = transformBetween({}, {}, {}, PixelFormat::CMYKA16, PixelFormat::RGBA16);
    auto to16 = transformBetween({}, {}, {}, PixelFormat::RGBA16, PixelFormat::CMYKA16);
    REQUIRE(back16 && to16);
    const uint16_t px[4] = {16000, 4000, 2000, 16384};
    uint16_t c16[5] = {}, r16[4] = {};
    to16->apply(px, c16, 1);
    CHECK_EQ(int(c16[4]), 16384);
    for (int c = 0; c < 4; c++) CHECK(c16[c] <= 16384);
    back16->apply(c16, r16, 1);
    CHECK_EQ(int(r16[3]), 16384);
    for (int c = 0; c < 3; c++) CHECK(r16[c] <= 16384);
}

TEST_CASE(lab_matches_lcms_and_known_values) {
    // sRGB red in Lab D50 is (54.29, 80.80, 69.89), Little CMS's and the ICC's published value (relative colorimetric).
    auto toLab = transformBetween({}, {}, {}, PixelFormat::RGBA16, PixelFormat::LabA16);
    auto toRgb = transformBetween({}, {}, {}, PixelFormat::LabA16, PixelFormat::RGBA16);
    REQUIRE(toLab && toRgb);
    const uint16_t red[4] = {32768, 0, 0, 32768}, white[4] = {32768, 32768, 32768, 32768};
    uint16_t lab[4] = {};
    toLab->apply(red, lab, 1);
    CHECK_NEAR(labL<SampleType::U16>(lab[0], lab[3]), 54.29, 0.1);
    CHECK_NEAR(labA<SampleType::U16>(lab[1], lab[3]), 80.80, 0.2);
    CHECK_NEAR(labB<SampleType::U16>(lab[2], lab[3]), 69.89, 0.2);
    toLab->apply(white, lab, 1);
    CHECK_NEAR(labL<SampleType::U16>(lab[0], lab[3]), 100.0, 0.01);
    CHECK_NEAR(labA<SampleType::U16>(lab[1], lab[3]), 0.0, 0.02);
    CHECK_NEAR(labB<SampleType::U16>(lab[2], lab[3]), 0.0, 0.02);
    // 16 bits against Little CMS's float pipeline: our Lab is its float Lab within half a 16-bit step (L 100/32768,
    // a and b 1/128), and back to RGB is its float result for the Lab we hold, within a 15-bit step.
    const auto colours = sampleColours();
    const size_t n = colours.size();
    Image16 rgb(int(n), 1), labs(int(n), 1), back(int(n), 1);
    std::vector<float> straight(n * 3), reference(n * 3), held(n * 3), referenceBack(n * 3);
    for (size_t i = 0; i < n; i++)
        for (int c = 0; c < 4; c++) {
            rgb.pixel(int(i), 0)[c] = c == 3 ? uint16_t(32768) : widen8(colours[i][size_t(c)]);
            if (c < 3) straight[i * 3 + size_t(c)] = float(rgb.pixel(int(i), 0)[c]) / 32768.0f;
        }
    toLab->apply(rgb.row(0), labs.row(0), n);
    toRgb->apply(labs.row(0), back.row(0), n);
    lcmsDirect(srgbProfile(), TYPE_RGB_FLT, labProfile(), TYPE_Lab_FLT, straight.data(), reference.data(), n);
    double worstL = 0, worstAB = 0;
    for (size_t i = 0; i < n; i++) {
        const uint16_t* p = labs.pixel(int(i), 0);
        held[i * 3] = float(labL<SampleType::U16>(p[0], p[3]));
        held[i * 3 + 1] = float(labA<SampleType::U16>(p[1], p[3]));
        held[i * 3 + 2] = float(labB<SampleType::U16>(p[2], p[3]));
        worstL = std::max(worstL, std::abs(double(held[i * 3]) - reference[i * 3]));
        for (int c = 1; c < 3; c++) worstAB = std::max(worstAB, std::abs(double(held[i * 3 + size_t(c)]) - reference[i * 3 + size_t(c)]));
    }
    CHECK(worstL <= 0.5 * 100.0 / 32768.0 + 1e-4);
    CHECK(worstAB <= 0.5 / 128.0 + 1e-4);
    lcmsDirect(labProfile(), TYPE_Lab_FLT, srgbProfile(), TYPE_RGB_FLT, held.data(), referenceBack.data(), n);
    double worstBack = 0;
    int worstTrip = 0;
    for (size_t i = 0; i < n; i++)
        for (int c = 0; c < 3; c++) {
            worstBack = std::max(worstBack, std::abs(double(back.pixel(int(i), 0)[c]) / 32768.0 - std::clamp(double(referenceBack[i * 3 + size_t(c)]), 0.0, 1.0)));
            worstTrip = std::max(worstTrip, std::abs(int(back.pixel(int(i), 0)[c]) - int(rgb.pixel(int(i), 0)[c])));
        }
    CHECK(worstBack <= 1.0 / 32768.0);
    // The whole round trip loses less than a fifth of an 8-bit level: the most is at sRGB's gamut edge next to 0,
    // where one 16-bit step of a or b moves a channel furthest.
    CHECK(worstTrip < 128 / 5);
    // 8-bit Lab is Little CMS's own 8-bit Lab encoding (the same 128 offset): identical bytes.
    const Image rgb8 = rgbaSamples();
    auto toLab8 = transformBetween({}, {}, {}, PixelFormat::RGBA8, PixelFormat::LabA8);
    REQUIRE(toLab8);
    Image lab8(int(n), 1);
    toLab8->apply(rgb8.row(0), lab8.row(0), n);
    std::vector<uint8_t> straight8(n * 3), reference8(n * 3);
    for (size_t i = 0; i < n; i++) std::memcpy(&straight8[i * 3], rgb8.pixel(int(i), 0), 3);
    lcmsDirect(srgbProfile(), TYPE_RGB_8, labProfile(), TYPE_Lab_8, straight8.data(), reference8.data(), n);
    int same = 0;
    for (size_t i = 0; i < n; i++) for (int c = 0; c < 3; c++) same += lab8.pixel(int(i), 0)[c] == reference8[i * 3 + size_t(c)];
    CHECK_EQ(same, int(n * 3));
    CHECK_NEAR(labA<SampleType::U8>(lab8.pixel(int(n) - 1, 0)[1], 255), 0.0, 1.0);   // white is neutral
}

TEST_CASE(any_layout_to_display_bytes) {
    // The fused reduction: a 16-bit CMYK or Lab buffer straight to 8-bit RGBA equals converting at 16 bits and
    // narrowing, within a level.
    Image16 rgb(40, 3);
    for (int y = 0; y < 3; y++)
        for (int x = 0; x < 40; x++) {
            uint16_t* p = rgb.pixel(x, y);
            p[3] = uint16_t(y == 1 ? 16384 : 32768);
            p[0] = uint16_t(x * 800 * p[3] / 32768); p[1] = uint16_t((32000 - x * 700) * p[3] / 32768); p[2] = uint16_t(9000 * p[3] / 32768);
        }
    for (ColorMode mode : {ColorMode::CMYK, ColorMode::Lab}) {
        AnyImage converted = convertImage(AnyImage(Image16Ptr(std::make_shared<Image16>(rgb))), ColorMode::RGB, {}, mode, {});
        REQUIRE(converted.u16() != nullptr);
        CHECK_EQ(converted.channels(), colorModeChannels(mode));
        auto display = transformBetween({}, {}, {}, pixelFormatFor(SampleType::U16, mode), PixelFormat::RGBA8);
        auto wide = transformBetween({}, {}, {}, pixelFormatFor(SampleType::U16, mode), PixelFormat::RGBA16);
        REQUIRE(display && wide);
        Image fused;
        REQUIRE(convertImageTo8(converted, fused, *display));
        Image16 twoStep(40, 3);
        for (int y = 0; y < 3; y++) wide->apply(converted.u16()->row(y), twoStep.row(y), 40);
        int worst = 0;
        for (int y = 0; y < 3; y++) for (int x = 0; x < 40; x++) for (int c = 0; c < 4; c++)
            worst = std::max(worst, std::abs(int(fused.pixel(x, y)[c]) - int(narrow16(twoStep.pixel(x, y)[c]))));
        CHECK(worst <= 1);
        // A transform made for another layout is refused: a 4-channel buffer for a CMYK transform, an 8-bit buffer for
        // a 16-bit one. (RGB and Lab buffers have the same shape; the caller knows the document's mode.)
        Image untouched;
        if (mode == ColorMode::CMYK) CHECK(!convertImageTo8(AnyImage(Image16Ptr(std::make_shared<Image16>(rgb))), untouched, *display));
        CHECK(!convertImageTo8(AnyImage(std::make_shared<Image>(4, 4)), untouched, *display));
        CHECK(untouched.isEmpty());
    }
    // 8-bit CMYK to the display.
    AnyImage cmyk8 = convertImage(AnyImage(std::make_shared<Image>(rgbaSamples())), ColorMode::RGB, {}, ColorMode::CMYK, {});
    REQUIRE(cmyk8.c8() != nullptr);
    auto display8 = transformBetween({}, {}, {}, PixelFormat::CMYKA8, PixelFormat::RGBA8);
    REQUIRE(display8);
    Image out8;
    CHECK(convertImageTo8(cmyk8, out8, *display8));
    CHECK_EQ(out8.width(), cmyk8.width());
    // Channels that are not the mode's are refused; the same colours come back shared.
    CHECK(!convertImage(cmyk8, ColorMode::RGB, {}, ColorMode::Lab, {}));
    CHECK(convertImage(cmyk8, ColorMode::CMYK, {}, ColorMode::CMYK, {}) == cmyk8);
    CHECK(!convertImage(cmyk8, ColorMode::CMYK, srgbProfile(), ColorMode::RGB, {}));   // an RGB profile for CMYK values
}

TEST_CASE(proof_rgb_as_working_cmyk) {
    // View > Proof Setup > Working CMYK on an RGB document, with the gamut warning: sRGB's pure blue is outside FOGRA39
    // and shows the warning colour; a mid grey does not.
    ProofSettings proof;
    proof.profile = defaultCmykProfile();
    proof.gamutWarning = true;
    proof.warning[0] = 0; proof.warning[1] = 255; proof.warning[2] = 0;
    auto t = proofTransform({}, srgbProfile(), proof, PixelFormat::RGBA8, PixelFormat::RGBA8);
    REQUIRE(t);
    const uint8_t in[8] = {0, 0, 255, 255, 128, 128, 128, 255};
    uint8_t out[8] = {};
    t->apply(in, out, 2);
    CHECK(out[0] == 0 && out[1] == 255 && out[2] == 0);
    CHECK(!(out[4] == 0 && out[5] == 255 && out[6] == 0));
    CHECK(std::abs(int(out[4]) - 128) <= 3);
    // Without the warning the blue is shown as the press prints it: duller than sRGB's.
    proof.gamutWarning = false;
    auto soft = proofTransform({}, srgbProfile(), proof, PixelFormat::RGBA16, PixelFormat::RGBA8);
    REQUIRE(soft);
    const uint16_t blue[4] = {0, 0, 32768, 32768};
    uint8_t shown[4] = {};
    soft->apply(blue, shown, 1);
    CHECK(shown[0] > 10 || shown[1] > 10);
    CHECK_EQ(int(shown[3]), 255);
}

TEST_CASE(convert_profile_of_a_cmyk_document) {
    Document doc(16, 4);
    doc.colorMode = ColorMode::CMYK;
    auto image = std::make_shared<Image>(16, 4);
    for (int x = 0; x < 16; x++) for (int y = 0; y < 4; y++) { uint8_t* p = image->pixel(x, y); p[0] = uint8_t(x * 16); p[1] = 90; p[2] = uint8_t(255 - x * 16); p[3] = 255; }
    AnyImage cmyk = convertImage(AnyImage(ImagePtr(image)), ColorMode::RGB, {}, ColorMode::CMYK, {});
    REQUIRE(cmyk.c8() != nullptr);
    doc.layers.push_back(Layer(Asset::makeAny(cmyk, "ink"), Point(0, 0)));
    std::string why;
    // An RGB profile is not a CMYK document's: refused, and the document is as it was.
    CHECK(!convertDocumentProfile(doc, builtinProfile(WorkingSpace::AdobeRGB), {}, &why));
    CHECK(why.find("CMYK") != std::string::npos);
    CHECK(doc.profile.empty());
    // Tagging with the Working CMYK itself is the same colours: the pixels stay, the profile is recorded.
    REQUIRE(convertDocumentProfile(doc, defaultCmykProfile(), {}, &why));
    CHECK(doc.profile == defaultCmykProfile());
    CHECK(doc.layers[0].asset->image == cmyk);
}

TEST_MAIN()
