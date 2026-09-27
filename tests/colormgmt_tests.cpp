// Colour management (colormgmt.h): profiles, transforms against Little CMS and analytic references, premultiplied
// 8- and 16-bit conversion, soft proofing, transfer curves, Convert to Profile over a document.
#include "check.h"
#include "compositor/colormgmt.h"
#include "compositor/depth.h"
#include "compositor/document.h"
#include "compositor/png.h"
#include "compositor/project.h"
#include "compositor/psd.h"
#include "compositor/psd_writer.h"
#include "compositor/render.h"
#include <filesystem>
#include "lcms2.h"
#include <array>
#include <cmath>
#include <cstring>
#include <random>

using namespace compositor;

namespace {

const ColorProfile& adobe() { return builtinProfile(WorkingSpace::AdobeRGB); }
const ColorProfile& prophoto() { return builtinProfile(WorkingSpace::ProPhoto); }
const ColorProfile& p3() { return builtinProfile(WorkingSpace::DisplayP3); }

double srgbLinear(double v) { return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4); }

/// sRGB to Adobe RGB (1998) by the published matrices (both D65, so no adaptation): the analytic reference.
std::array<double, 3> analyticSrgbToAdobe(double r, double g, double b) {
    const double lr = srgbLinear(r), lg = srgbLinear(g), lb = srgbLinear(b);
    const double X = 0.4124564 * lr + 0.3575761 * lg + 0.1804375 * lb;
    const double Y = 0.2126729 * lr + 0.7151522 * lg + 0.0721750 * lb;
    const double Z = 0.0193339 * lr + 0.1191920 * lg + 0.9503041 * lb;
    const double ar = 2.0413690 * X - 0.5649464 * Y - 0.3446944 * Z;
    const double ag = -0.9692660 * X + 1.8760108 * Y + 0.0415560 * Z;
    const double ab = 0.0134474 * X - 0.1183897 * Y + 1.0154096 * Z;
    auto enc = [](double v) { return std::pow(std::clamp(v, 0.0, 1.0), 256.0 / 563.0); };
    return {enc(ar), enc(ag), enc(ab)};
}

std::vector<std::array<uint8_t, 3>> sampleColours() {
    std::vector<std::array<uint8_t, 3>> out;
    for (int r = 0; r <= 255; r += 51) for (int g = 0; g <= 255; g += 51) for (int b = 0; b <= 255; b += 51) out.push_back({uint8_t(r), uint8_t(g), uint8_t(b)});
    return out;
}

/// Little CMS directly, straight RGB bytes, as the reference for our premultiplied path.
std::array<uint8_t, 3> lcmsReference8(const ColorProfile& from, const ColorProfile& to, std::array<uint8_t, 3> in, int intent = INTENT_RELATIVE_COLORIMETRIC, bool bpc = true) {
    cmsHPROFILE a = cmsOpenProfileFromMem(from.icc.data(), cmsUInt32Number(from.icc.size()));
    cmsHPROFILE b = cmsOpenProfileFromMem(to.icc.data(), cmsUInt32Number(to.icc.size()));
    cmsHTRANSFORM t = cmsCreateTransform(a, TYPE_RGB_8, b, TYPE_RGB_8, cmsUInt32Number(intent), bpc ? cmsFLAGS_BLACKPOINTCOMPENSATION : 0);
    std::array<uint8_t, 3> out{};
    cmsDoTransform(t, in.data(), out.data(), 1);
    cmsDeleteTransform(t); cmsCloseProfile(a); cmsCloseProfile(b);
    return out;
}

/// The same unoptimised in double precision, for 16-bit pixels (0..1 in and out).
std::array<double, 3> lcmsReferenceExact(const ColorProfile& from, const ColorProfile& to, std::array<double, 3> in) {
    cmsHPROFILE a = cmsOpenProfileFromMem(from.icc.data(), cmsUInt32Number(from.icc.size()));
    cmsHPROFILE b = cmsOpenProfileFromMem(to.icc.data(), cmsUInt32Number(to.icc.size()));
    cmsHTRANSFORM t = cmsCreateTransform(a, TYPE_RGB_DBL, b, TYPE_RGB_DBL, INTENT_RELATIVE_COLORIMETRIC, cmsFLAGS_BLACKPOINTCOMPENSATION | cmsFLAGS_NOOPTIMIZE);
    std::array<double, 3> out{};
    cmsDoTransform(t, in.data(), out.data(), 1);
    cmsDeleteTransform(t); cmsCloseProfile(a); cmsCloseProfile(b);
    return out;
}

/// The worst channel difference of an sRGB grid sent through `via` and back by Little CMS directly at 8 bits.
int lcmsRoundTrip8(const ColorProfile& via) {
    int worst = 0;
    for (auto c : sampleColours()) {
        const auto there = lcmsReference8(srgbProfile(), via, c);
        const auto back = lcmsReference8(via, srgbProfile(), there);
        for (size_t i = 0; i < 3; i++) worst = std::max(worst, std::abs(int(back[i]) - int(c[i])));
    }
    return worst;
}

Image opaqueImage(const std::vector<std::array<uint8_t, 3>>& colours) {
    Image img(int(colours.size()), 1);
    for (size_t i = 0; i < colours.size(); i++) { uint8_t* p = img.row(0) + i * 4; p[0] = colours[i][0]; p[1] = colours[i][1]; p[2] = colours[i][2]; p[3] = 255; }
    return img;
}

} // namespace

TEST_CASE(builtin_profiles_read_back_with_their_names) {
    for (WorkingSpace s : {WorkingSpace::SRGB, WorkingSpace::AdobeRGB, WorkingSpace::DisplayP3, WorkingSpace::ProPhoto}) {
        const ColorProfile& p = builtinProfile(s);
        REQUIRE(p.icc.size() > 128);
        auto parsed = profileFromIcc(p.icc);
        REQUIRE(parsed.has_value());
        CHECK_EQ(parsed->description, std::string(workingSpaceName(s)));
        CHECK(parsed->model == ColorModel::RGB);
        CHECK(matchingWorkingSpace(p) == s);
        CHECK(workingSpaceFromKey(workingSpaceKey(s)) == s);
    }
    // A fixed creation date: the same profile bytes in every session (the header's date field).
    const auto& icc = adobe().icc;
    CHECK_EQ(int(icc[24]) * 256 + icc[25], 2026);
    CHECK(!profileFromIcc(std::vector<uint8_t>(40, 7)).has_value());
}

TEST_CASE(untagged_is_srgb_and_equivalence) {
    CHECK(equivalentProfiles(ColorProfile{}, srgbProfile()));
    CHECK(!equivalentProfiles(srgbProfile(), adobe()));
    CHECK(!equivalentProfiles(srgbProfile(), p3()));
    CHECK(transformBetween(ColorProfile{}, srgbProfile(), {}, PixelFormat::RGBA8, PixelFormat::RGBA8) == nullptr);
    // sRGB from another source (Little CMS's own built-in, untouched description) is the same colours.
    cmsHPROFILE h = cmsCreate_sRGBProfile();
    cmsUInt32Number size = 0;
    cmsSaveProfileToMem(h, nullptr, &size);
    std::vector<uint8_t> bytes(size);
    cmsSaveProfileToMem(h, bytes.data(), &size);
    cmsCloseProfile(h);
    auto other = profileFromIcc(bytes);
    REQUIRE(other.has_value());
    CHECK(other->icc != srgbProfile().icc);
    CHECK(equivalentProfiles(*other, ColorProfile{}));
    CHECK(matchingWorkingSpace(*other) == WorkingSpace::SRGB);
}

TEST_CASE(known_patches_srgb_to_adobe_rgb) {
    // Photoshop's well-known values: sRGB red is (219, 0, 0) in Adobe RGB, green (144, 255, 60).
    uint8_t red[3] = {255, 0, 0}, green[3] = {0, 255, 0};
    convertColor(srgbProfile(), adobe(), red);
    convertColor(srgbProfile(), adobe(), green);
    // (Zero channels within two levels: a pure gamma curve lifts a residue of 1e-5 to one or two levels.)
    CHECK_NEAR(red[0], 219, 1); CHECK_NEAR(red[1], 0, 2); CHECK_NEAR(red[2], 0, 2);
    CHECK_NEAR(green[0], 144, 1); CHECK_NEAR(green[1], 255, 1); CHECK_NEAR(green[2], 60, 1);
    // Against the analytic matrices over a grid.
    for (auto c : sampleColours()) {
        double v[3] = {c[0] / 255.0, c[1] / 255.0, c[2] / 255.0};
        convertColor(srgbProfile(), adobe(), v, {RenderingIntent::RelativeColorimetric, false});
        const auto ref = analyticSrgbToAdobe(c[0] / 255.0, c[1] / 255.0, c[2] / 255.0);
        // Encoded values within a level where they are not near black; in linear light everywhere (a pure gamma
        // curve magnifies the smallest difference near zero).
        for (int i = 0; i < 3; i++) {
            if (ref[size_t(i)] > 0.05) CHECK_NEAR(v[i], ref[size_t(i)], 0.003);
            CHECK_NEAR(std::pow(v[i], 563.0 / 256.0), std::pow(ref[size_t(i)], 563.0 / 256.0), 0.0005);
        }
    }
}

TEST_CASE(convert_image_8_matches_lcms) {
    const auto colours = sampleColours();
    for (const ColorProfile* to : {&adobe(), &prophoto(), &p3()}) {
        Image img = opaqueImage(colours);
        REQUIRE(convertImage(img, srgbProfile(), *to));
        for (size_t i = 0; i < colours.size(); i++) {
            const auto ref = lcmsReference8(srgbProfile(), *to, colours[i]);
            const uint8_t* p = img.row(0) + i * 4;
            for (int c = 0; c < 3; c++) CHECK_NEAR(p[c], ref[size_t(c)], 1);
            CHECK_EQ(int(p[3]), 255);
        }
    }
}

TEST_CASE(convert_image_8_premultiplied) {
    // A half-transparent pixel converts as its straight colour does, then is premultiplied again; alpha is kept.
    Image img(3, 1);
    const uint8_t straight[3] = {200, 40, 90};
    const uint8_t alphas[3] = {128, 255, 0};
    for (int x = 0; x < 3; x++) {
        uint8_t* p = img.row(0) + x * 4;
        for (int c = 0; c < 3; c++) p[c] = uint8_t((straight[c] * alphas[x] + 127) / 255);
        p[3] = alphas[x];
    }
    REQUIRE(convertImage(img, srgbProfile(), adobe()));
    const auto ref = lcmsReference8(srgbProfile(), adobe(), {200, 40, 90});
    const uint8_t* half = img.row(0);
    CHECK_EQ(int(half[3]), 128);
    for (int c = 0; c < 3; c++) CHECK_NEAR(half[c], ref[size_t(c)] * 128 / 255.0, 2);
    const uint8_t* clear = img.row(0) + 8;
    CHECK_EQ(int(clear[3]), 0);
    for (int c = 0; c < 3; c++) CHECK_EQ(int(clear[c]), 0);
}

TEST_CASE(convert_image_16_matches_lcms) {
    std::mt19937 rng(7);
    Image16 img(64, 1);
    std::vector<std::array<uint16_t, 3>> straight;
    for (int x = 0; x < 64; x++) {
        std::array<uint16_t, 3> s{uint16_t(rng() % 32769), uint16_t(rng() % 32769), uint16_t(rng() % 32769)};
        straight.push_back(s);
        uint16_t* p = img.row(0) + x * 4;
        for (int c = 0; c < 3; c++) p[c] = s[size_t(c)];
        p[3] = uint16_t(one16);
    }
    REQUIRE(convertImage(img, srgbProfile(), adobe()));
    for (int x = 0; x < 64; x++) {
        const auto& s = straight[size_t(x)];
        const auto ref = lcmsReferenceExact(srgbProfile(), adobe(), {s[0] / 32768.0, s[1] / 32768.0, s[2] / 32768.0});
        const uint16_t* p = img.row(0) + x * 4;
        for (int c = 0; c < 3; c++) CHECK_NEAR(p[c], ref[size_t(c)] * 32768, 4);
        CHECK_EQ(int(p[3]), int(one16));
    }
}

TEST_CASE(round_trips_8_and_16) {
    // In-gamut colours (an sRGB grid) through Adobe RGB, ProPhoto and Display P3 and back. At 8 bits the loss is what
    // Little CMS's own 8-bit round trip loses (ProPhoto's 8-bit shadows are coarse: Photoshop warns about it too).
    for (const ColorProfile* via : {&adobe(), &prophoto(), &p3()}) {
        const auto colours = sampleColours();
        Image img = opaqueImage(colours);
        REQUIRE(convertImage(img, srgbProfile(), *via));
        REQUIRE(convertImage(img, *via, srgbProfile()));
        int worst = 0;
        for (size_t i = 0; i < colours.size(); i++)
            for (int c = 0; c < 3; c++) worst = std::max(worst, std::abs(int(img.row(0)[i * 4 + size_t(c)]) - int(colours[i][size_t(c)])));
        std::fprintf(stderr, "    via %s: worst 8-bit %d\n", via->description.c_str(), worst);
        CHECK(worst <= lcmsRoundTrip8(*via));

        Image16 deep(int(colours.size()), 1);
        for (size_t i = 0; i < colours.size(); i++) { for (int c = 0; c < 3; c++) deep.row(0)[i * 4 + size_t(c)] = widen8(colours[i][size_t(c)]); deep.row(0)[i * 4 + 3] = uint16_t(one16); }
        const Image16 before = deep;
        REQUIRE(convertImage(deep, srgbProfile(), *via));
        REQUIRE(convertImage(deep, *via, srgbProfile()));
        int worst16 = 0;
        for (size_t i = 0; i < colours.size() * 4; i++) worst16 = std::max(worst16, std::abs(int(deep.row(0)[i]) - int(before.row(0)[i])));
        std::fprintf(stderr, "    via %s: worst 16-bit %d\n", via->description.c_str(), worst16);
        CHECK(worst16 <= 16);   // 16 of 32768: an eighth of an 8-bit level
    }
}

TEST_CASE(sixteen_to_eight_fused) {
    Image16 deep(4, 1);
    for (int x = 0; x < 4; x++) { uint16_t* p = deep.row(0) + x * 4; p[0] = uint16_t(x * 8000); p[1] = 16384; p[2] = 32768; p[3] = uint16_t(one16); }
    ColorTransformPtr fused = transformBetween(adobe(), srgbProfile(), {}, PixelFormat::RGBA16, PixelFormat::RGBA8);
    REQUIRE(fused != nullptr);
    Image out;
    convertImage16To8(deep, out, *fused);
    Image16 step = deep;
    REQUIRE(convertImage(step, adobe(), srgbProfile()));
    for (int x = 0; x < 4; x++)
        for (int c = 0; c < 4; c++) CHECK_NEAR(out.row(0)[x * 4 + c], narrow16(step.row(0)[x * 4 + c]), 1);
}

TEST_CASE(transform_cache_reuses) {
    clearTransformCache();
    auto a = transformBetween(srgbProfile(), adobe(), {}, PixelFormat::RGBA8, PixelFormat::RGBA8);
    auto b = transformBetween(srgbProfile(), adobe(), {}, PixelFormat::RGBA8, PixelFormat::RGBA8);
    CHECK(a && a == b);
    auto c = transformBetween(srgbProfile(), adobe(), {RenderingIntent::Perceptual, false}, PixelFormat::RGBA8, PixelFormat::RGBA8);
    CHECK(c && c != a);
    CHECK_EQ(transformCacheSize(), size_t(2));
    for (int i = 0; i < 40; i++) transformBetween(srgbProfile(), adobe(), {RenderingIntent(i % 4), i % 8 < 4}, PixelFormat(i % 2), PixelFormat(i % 2));
    CHECK(transformCacheSize() <= 24);
}

TEST_CASE(soft_proof_and_gamut_warning) {
    // ProPhoto's pure green is far outside sRGB: proofed through sRGB it is clipped; with the warning, it is flagged.
    Image img(2, 1);
    uint8_t* p = img.row(0);
    p[0] = 0; p[1] = 255; p[2] = 0; p[3] = 255;          // out of sRGB's gamut
    p[4] = 128; p[5] = 128; p[6] = 128; p[7] = 255;      // a grey: in gamut
    ProofSettings proof;
    proof.profile = srgbProfile();
    proof.gamutWarning = true;
    proof.warning[0] = 255; proof.warning[1] = 0; proof.warning[2] = 255;
    auto t = proofTransform(prophoto(), srgbProfile(), proof, PixelFormat::RGBA8, PixelFormat::RGBA8);
    REQUIRE(t != nullptr);
    Image shown = img;
    convertImage(shown, t.get());
    CHECK_EQ(int(shown.row(0)[0]), 255); CHECK_EQ(int(shown.row(0)[1]), 0); CHECK_EQ(int(shown.row(0)[2]), 255);
    CHECK(!(shown.row(0)[4] == 255 && shown.row(0)[5] == 0 && shown.row(0)[6] == 255));
    // Without the warning, the proof matches converting to sRGB (clipped) and showing that.
    proof.gamutWarning = false;
    auto plain = proofTransform(prophoto(), srgbProfile(), proof, PixelFormat::RGBA8, PixelFormat::RGBA8);
    Image a = img, b = img;
    convertImage(a, plain.get());
    REQUIRE(convertImage(b, prophoto(), srgbProfile()));
    for (int i = 0; i < 8; i++) CHECK_NEAR(a.row(0)[i], b.row(0)[i], 2);
}

TEST_CASE(transfer_curves) {
    const TransferCurve s = TransferCurve::ofProfile(ColorProfile{});
    CHECK(s.kind() == TransferCurve::Kind::SRGB);
    CHECK_NEAR(s.toLinear(0.5f), 0.2140, 0.0005);
    CHECK_NEAR(s.fromLinear(0.2140f), 0.5, 0.0005);
    const TransferCurve a = TransferCurve::ofProfile(adobe());
    CHECK(a.kind() == TransferCurve::Kind::Gamma);
    CHECK_NEAR(a.gamma(), 563.0 / 256.0, 1e-9);
    CHECK_NEAR(a.toLinear(0.5f), std::pow(0.5, 563.0 / 256.0), 1e-5);
    CHECK(TransferCurve::ofProfile(prophoto()).gamma() == 1.8);
    CHECK(TransferCurve::ofProfile(p3()).kind() == TransferCurve::Kind::SRGB);
    // Any other profile: tabulated from its tone curve (a gamma 2.6 profile here).
    cmsCIExyY white{0.3127, 0.3290, 1};
    cmsCIExyYTRIPLE primaries{{0.68, 0.32, 1}, {0.265, 0.69, 1}, {0.15, 0.06, 1}};
    cmsToneCurve* g = cmsBuildGamma(nullptr, 2.6);
    cmsToneCurve* curves[3] = {g, g, g};
    cmsHPROFILE h = cmsCreateRGBProfile(&white, &primaries, curves);
    cmsFreeToneCurve(g);
    cmsUInt32Number size = 0;
    cmsSaveProfileToMem(h, nullptr, &size);
    std::vector<uint8_t> bytes(size);
    cmsSaveProfileToMem(h, bytes.data(), &size);
    cmsCloseProfile(h);
    auto dci = profileFromIcc(bytes);
    REQUIRE(dci.has_value());
    const TransferCurve t = TransferCurve::ofProfile(*dci);
    CHECK(t.kind() == TransferCurve::Kind::Table);
    CHECK_NEAR(t.gamma(), 2.6, 0.02);
    for (float x : {0.0f, 0.1f, 0.5f, 0.9f, 1.0f}) {
        CHECK_NEAR(t.toLinear(x), std::pow(x, 2.6), 0.001);
        CHECK_NEAR(t.fromLinear(t.toLinear(x)), x, 0.002);
    }
    Document d(4, 4);
    d.profile = adobe();
    CHECK(documentTransfer(d).kind() == TransferCurve::Kind::Gamma);
}

TEST_CASE(convert_document_profile) {
    Document d(2, 1);
    auto img = std::make_shared<Image>(opaqueImage({{255, 0, 0}, {0, 255, 0}}));
    Layer pixels(Asset::make(img, "pixels"), {0, 0});
    pixels.mask = LayerMask{};
    pixels.mask->asset = MaskAsset::solid(true);
    const auto maskBefore = pixels.mask->asset.image;
    d.layers.push_back(pixels);
    Layer text;
    text.text = LayerText{};
    text.text->red = 1;
    TextRun run; run.length = 1; run.green = 1;
    text.text->runs = {run};
    d.layers.push_back(text);
    std::string why;
    REQUIRE(convertDocumentProfile(d, adobe(), {}, &why));
    CHECK(d.profile == adobe());
    const Image& out = *d.layers[0].asset->image.u8();
    CHECK_NEAR(out.row(0)[0], 219, 1);
    CHECK_NEAR(out.row(0)[4], 144, 1);
    CHECK(img->row(0)[0] == 255);   // the original buffer (history's) is untouched
    CHECK(d.layers[0].mask->asset.image == maskBefore);
    CHECK_NEAR(d.layers[1].text->red, 219 / 255.0, 0.005);
    CHECK_NEAR(d.layers[1].text->runs[0].red, 144 / 255.0, 0.005);
    // Back to sRGB and nearly where it started.
    REQUIRE(convertDocumentProfile(d, ColorProfile{}, {}, &why));
    CHECK(d.profile.empty());
    CHECK_NEAR(d.layers[0].asset->image.u8()->row(0)[0], 255, 1);
    // Assigning an equivalent profile changes only the tag.
    Document e(1, 1);
    REQUIRE(convertDocumentProfile(e, srgbProfile(), {}, &why));
    CHECK(e.profile == srgbProfile());
}

TEST_CASE(render_display_transform) {
    // Null: the render is exactly as before. A transform: applied to the frame (8-bit), or fused with the 16-bit
    // reduction.
    Document d(8, 4);
    auto img = std::make_shared<Image>(8, 4);
    for (int y = 0; y < 4; y++) for (int x = 0; x < 8; x++) { uint8_t* p = img->row(y) + x * 4; p[0] = uint8_t(x * 30); p[1] = uint8_t(y * 60); p[2] = 90; p[3] = 255; }
    d.layers.push_back(Layer(Asset::make(img, "a"), {0, 0}));
    RenderOptions options;
    Image plain, again;
    render(d, options, plain);
    options.display = nullptr;
    render(d, options, again);
    CHECK(plain == again);
    auto t = transformBetween(srgbProfile(), adobe(), {}, PixelFormat::RGBA8, PixelFormat::RGBA8);
    options.display = t.get();
    Image shown;
    render(d, options, shown);
    Image expected = plain;
    convertImage(expected, t.get());
    CHECK(shown == expected);

    REQUIRE(convertSampleType(d, SampleType::U16, nullptr));
    auto t16 = transformBetween(srgbProfile(), adobe(), {}, PixelFormat::RGBA16, PixelFormat::RGBA8);
    options.display = t16.get();
    Image shown16;
    render(d, options, shown16);
    Image16 deep;
    render16(d, RenderOptions{}, deep);
    Image fused;
    convertImage16To8(deep, fused, *t16);
    CHECK(shown16 == fused);
    for (int i = 0; i < 8 * 4 * 4; i++) CHECK_NEAR(shown16.row(0)[i], expected.row(0)[i], 1);
}

namespace {

/// Every image resource of a PSD: id to data.
std::vector<std::pair<int, std::vector<uint8_t>>> psdResources(const std::vector<uint8_t>& psd) {
    auto u32 = [&](size_t at) { return uint32_t(psd[at]) << 24 | uint32_t(psd[at + 1]) << 16 | uint32_t(psd[at + 2]) << 8 | psd[at + 3]; };
    std::vector<std::pair<int, std::vector<uint8_t>>> out;
    size_t at = 26;
    at += 4 + u32(at);                        // colour mode data
    const size_t end = at + 4 + u32(at);
    at += 4;
    while (at + 12 <= end) {
        const int id = psd[at + 4] << 8 | psd[at + 5];
        const size_t nameLength = psd[at + 6];
        at += 6 + ((nameLength + 2) & ~size_t(1));
        const uint32_t length = u32(at);
        out.push_back({id, std::vector<uint8_t>(psd.begin() + long(at + 4), psd.begin() + long(at + 4 + length))});
        at += 4 + length + (length & 1);
    }
    return out;
}

std::optional<std::vector<uint8_t>> resource(const std::vector<uint8_t>& psd, int id) {
    for (auto& [rid, data] : psdResources(psd)) if (rid == id) return data;
    return std::nullopt;
}

Document smallDocument() {
    Document d(4, 2);
    auto img = std::make_shared<Image>(opaqueImage({{255, 0, 0}, {0, 255, 0}, {0, 0, 255}, {128, 128, 128}, {1, 2, 3}, {250, 20, 10}, {9, 99, 199}, {0, 0, 0}}));
    Image wide(4, 2);
    for (int i = 0; i < 8; i++) std::memcpy(wide.row(i / 4) + (i % 4) * 4, img->row(0) + i * 4, 4);
    d.layers.push_back(Layer(Asset::make(std::make_shared<Image>(wide), "pixels"), {0, 0}));
    return d;
}

std::vector<uint8_t> foreignProfile() {
    // sRGB as Little CMS makes it, under another name: bytes NekoPhoto would never make itself.
    cmsHPROFILE h = cmsCreate_sRGBProfile();
    cmsMLU* description = cmsMLUalloc(nullptr, 1);
    cmsMLUsetASCII(description, "en", "US", "Camera sRGB (test)");
    cmsWriteTag(h, cmsSigProfileDescriptionTag, description);
    cmsMLUfree(description);
    cmsUInt32Number size = 0;
    cmsSaveProfileToMem(h, nullptr, &size);
    std::vector<uint8_t> bytes(size);
    cmsSaveProfileToMem(h, bytes.data(), &size);
    cmsCloseProfile(h);
    return bytes;
}

} // namespace

TEST_CASE(psd_profile_round_trip) {
    std::string error;
    // A tagged document writes resource 1039; it reads back as the document's profile.
    Document d = smallDocument();
    d.profile = adobe();
    auto bytes = encodePsd(d, {}, nullptr, &error);
    REQUIRE(!bytes.empty());
    CHECK(resource(bytes, 1039) == adobe().icc);
    auto back = importPsdBytes(bytes, &error);
    REQUIRE(back.has_value());
    CHECK(back->document.profile == adobe());
    CHECK_EQ(back->document.profile.description, std::string("Adobe RGB (1998)"));
    // Written again unchanged: the resource is byte for byte the file's.
    const auto foreign = foreignProfile();
    Document f = smallDocument();
    f.profile = *profileFromIcc(foreign);
    auto first = encodePsd(f, {}, nullptr, &error);
    auto opened = importPsdBytes(first, &error);
    REQUIRE(opened.has_value());
    CHECK(opened->document.profile.icc == foreign);
    CHECK_EQ(opened->document.profile.description, std::string("Camera sRGB (test)"));
    auto second = encodePsd(opened->document, {}, nullptr, &error);
    CHECK(resource(second, 1039) == foreign);
    int count = 0;
    for (auto& r : psdResources(second)) count += r.first == 1039;
    CHECK_EQ(count, 1);
    // Assigned another profile: that one is written in its place; untagged: none.
    Document assigned = opened->document;
    assigned.profile = prophoto();
    CHECK(resource(encodePsd(assigned, {}, nullptr, &error), 1039) == prophoto().icc);
    Document untagged = opened->document;
    untagged.profile = {};
    CHECK(!resource(encodePsd(untagged, {}, nullptr, &error), 1039).has_value());
    CHECK(!resource(encodePsd(smallDocument(), {}, nullptr, &error), 1039).has_value());
}

TEST_CASE(project_profile_round_trip) {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / ("colormgmt_tests_" + std::to_string(std::random_device{}()));
    fs::create_directories(dir);
    Document d = smallDocument();
    d.profile = *profileFromIcc(foreignProfile());
    ProjectError error;
    REQUIRE(saveProject(d, std::nullopt, (dir / "tagged.comp").string(), error));
    CHECK(fs::exists(dir / "tagged.comp" / "profile.icc"));
    auto loaded = loadProject((dir / "tagged.comp").string(), error);
    REQUIRE(loaded.has_value());
    CHECK(loaded->profile.icc == d.profile.icc);
    CHECK_EQ(loaded->profile.description, std::string("Camera sRGB (test)"));
    // Untagged: the manifest says sRGB and no profile file is written, as before.
    Document plain = smallDocument();
    REQUIRE(saveProject(plain, std::nullopt, (dir / "plain.comp").string(), error));
    CHECK(!fs::exists(dir / "plain.comp" / "profile.icc"));
    CHECK(manifestJson(plain, std::nullopt).find("\"sRGB\"") != std::string::npos);
    auto plainBack = loadProject((dir / "plain.comp").string(), error);
    REQUIRE(plainBack.has_value());
    CHECK(plainBack->profile.empty());
    fs::remove_all(dir);
}

TEST_CASE(png_profile_round_trip) {
    namespace fs = std::filesystem;
    const fs::path file = fs::temp_directory_path() / ("colormgmt_tests_" + std::to_string(std::random_device{}()) + ".png");
    const Document d = smallDocument();
    std::string error;
    REQUIRE(writePngImage(file.string(), *d.layers[0].asset->image.u8(), 72, &error, &adobe().icc));
    PngInfo info;
    REQUIRE(readPngInfo(file.string(), info));
    CHECK(info.icc == adobe().icc);
    auto pixels = readPngImage(file.string(), &error);
    REQUIRE(pixels != nullptr);
    CHECK(*pixels == *d.layers[0].asset->image.u8());
    // 16 bits, and untagged (sRGB chunks, no iCCP).
    Image16 deep(2, 1);
    deep.row(0)[3] = deep.row(0)[7] = uint16_t(one16);
    REQUIRE(writePngImage16(file.string(), deep, 0, &error, &prophoto().icc));
    REQUIRE(readPngInfo(file.string(), info));
    CHECK(info.icc == prophoto().icc);
    REQUIRE(writePngImage(file.string(), *d.layers[0].asset->image.u8(), 72, &error));
    REQUIRE(readPngInfo(file.string(), info));
    CHECK(info.icc.empty());
    fs::remove(file);
}

TEST_MAIN()
