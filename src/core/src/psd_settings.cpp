// The PSD reader's settings (psd_read.h): Photoshop's blend modes, adjustment layers, fills and text as NekoPhoto's, and
// which of the file's resources and blocks are carried for PSD export (psd_carry.h).
#include "psd_read.h"
#include "nekophoto/adjustments.h"
#include "nekophoto/colour.h"
#include "nekophoto/colormgmt.h"
#include "nekophoto/depth.h"
#include "nekophoto/parallel.h"
#include "psd/psd_descriptor.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <span>
#include <string>
#include <vector>

namespace nekophoto::psd_read {

using namespace photoshop;

// ---- Photoshop settings as ours ---------------------------------------------------------------------

BlendMode blendFor(const std::string& key, bool* lossy) {
    static const std::map<std::string, BlendMode> exact{
        {"norm", BlendMode::Normal}, {"pass", BlendMode::Normal}, {"mul ", BlendMode::Multiply}, {"scrn", BlendMode::Screen}, {"over", BlendMode::Overlay},
        {"dark", BlendMode::Darken}, {"lite", BlendMode::Lighten}, {"diff", BlendMode::Difference}, {"div ", BlendMode::ColorDodge}, {"idiv", BlendMode::ColorBurn},
        {"hue ", BlendMode::Hue}, {"sat ", BlendMode::Saturation}, {"colr", BlendMode::Color}, {"lum ", BlendMode::Luminosity},
        {"diss", BlendMode::Dissolve}, {"lbrn", BlendMode::LinearBurn}, {"dkCl", BlendMode::DarkerColor}, {"lddg", BlendMode::LinearDodge},
        {"lgCl", BlendMode::LighterColor}, {"sLit", BlendMode::SoftLight}, {"hLit", BlendMode::HardLight}, {"vLit", BlendMode::VividLight},
        {"lLit", BlendMode::LinearLight}, {"pLit", BlendMode::PinLight}, {"hMix", BlendMode::HardMix}, {"smud", BlendMode::Exclusion},
        {"fsub", BlendMode::Subtract}, {"fdiv", BlendMode::Divide}};
    static const std::map<std::string, BlendMode> nearest{};
    if (auto it = exact.find(key); it != exact.end()) { *lossy = false; return it->second; }
    *lossy = true;
    if (auto it = nearest.find(key); it != nearest.end()) return it->second;
    return BlendMode::Normal;
}

// ---- What is carried for PSD export ----------------------------------------------------------------

/// Image resources written back on PSD export. Left out: the resolution (ours), thumbnails, what indexes
/// layers or alpha channels by position (both are rewritten), the ID seed (layer ids are reassigned), and
/// a non-RGB file's colour profile and colour settings (the export is RGB).
bool carriedResource(uint16_t id, int mode) {
    // Resolution; thumbnails; layer state, groups, selection, group-enabled ids; ID seed; alpha channel names,
    // unicode names, ids, display info and old display info; transparency index.
    static const std::set<uint16_t> never{1005, 1033, 1036, 1024, 1026, 1069, 1072, 1044, 1006, 1045, 1053, 1077, 1007, 1047};
    // Colour profile and untagged flag; colour and duotone halftoning and transfer; duotone image info.
    static const std::set<uint16_t> colourBound{1039, 1041, 1013, 1014, 1016, 1017, 1018};
    if (never.count(id)) return false;
    if (mode != RGB && colourBound.count(id)) return false;
    return true;
}

/// Global tagged blocks written back: everything but the layer information itself (written anew) and
/// the merged image's 16/32-bit transparency.
bool carriedGlobalBlock(const std::string& key) {
    static const std::set<std::string> never{"Lr16", "Lr32", "Layr", "LMsk", "Mt16", "Mt32", "Mtrn", "Alph"};
    return !never.count(key);
}

/// Per-layer blocks written back: all but what NekoPhoto reads into its own model and writes itself.
namespace {

bool smartObjectBlock(const std::string& key) { return key == "SoLd" || key == "SoLE" || key == "PlLd" || key == "plLd"; }

} // namespace

bool carriedLayerBlock(const std::string& key, bool modelledAdjustment, bool smartObject) {
    if (smartObject && smartObjectBlock(key)) return false;   // the instance keeps them (smartobject.h)
    static const std::set<std::string> never{"luni", "lsct", "lsdk", "iOpa", "lyid"};
    // A Gradient Map's grdm is carried: written back while unedited (its stops beyond the ends are not modelled).
    static const std::set<std::string> adjustments{"levl", "curv", "hue2", "expA"};
    if (never.count(key)) return false;
    if (modelledAdjustment && adjustments.count(key)) return false;
    return true;
}

const char* blendDescription(const std::string& key) {
    static const std::map<std::string, const char*> names{{"diss", "Dissolve"}, {"lbrn", "Linear Burn"}, {"dkCl", "Darker Color"}, {"lddg", "Linear Dodge (Add)"}, {"lgCl", "Lighter Color"},
        {"sLit", "Soft Light"}, {"hLit", "Hard Light"}, {"vLit", "Vivid Light"}, {"lLit", "Linear Light"}, {"pLit", "Pin Light"}, {"hMix", "Hard Mix"}, {"smud", "Exclusion"}, {"fsub", "Subtract"}, {"fdiv", "Divide"}};
    auto it = names.find(key);
    return it == names.end() ? key.c_str() : it->second;
}

namespace {

/// A Photoshop colour record (colour space and four values) as straight sRGB, and in a CMYK or Lab document of `mode` the
/// document's own values (CMYK ink 0..1; Lab L 0..100, signed a and b) in `native`. Grayscale's one value is the grey a
/// Grayscale file opens with as R = G = B. False for a space it does not read.
bool colourRecord(int space, const uint16_t v[4], int mode, const ColorProfile& profile, AdjustmentColor& rgb, std::optional<std::array<double, 4>>& native) {
    native.reset();
    switch (space) {
    case 0: rgb = {v[0] / 65535.0, v[1] / 65535.0, v[2] / 65535.0}; return true;
    case 1: {   // HSB
        const double h = v[0] / 65535.0 * 6, sat = v[1] / 65535.0, bright = v[2] / 65535.0;
        const int sector = int(std::floor(h)) % 6;
        const double f = h - std::floor(h), p = bright * (1 - sat), q = bright * (1 - sat * f), t = bright * (1 - sat * (1 - f));
        const double table[6][3] = {{bright, t, p}, {q, bright, p}, {p, bright, t}, {p, q, bright}, {t, p, bright}, {bright, p, q}};
        rgb = {table[sector][0], table[sector][1], table[sector][2]};
        return true;
    }
    case 2: case 7: {   // CMYK (65535: no ink) and Lab (L in hundredths, a and b signed hundredths)
        const bool cmyk = space == 2;
        float in[4];
        std::array<double, 4> values{};
        if (cmyk) for (int k = 0; k < 4; k++) { values[size_t(k)] = 1 - v[k] / 65535.0; in[k] = float(values[size_t(k)] * 100); }
        else {
            values = {v[0] / 100.0, int16_t(v[1]) / 100.0, int16_t(v[2]) / 100.0, 0};
            for (int k = 0; k < 3; k++) in[k] = float(values[size_t(k)]);
            in[3] = 0;
        }
        const ColorTransformPtr t = transformBetween(cmyk && mode == CMYK ? profile : ColorProfile(), ColorProfile(), ConvertOptions(),
                                                     cmyk ? PixelFormat::CMYKFloat : PixelFormat::LabFloat, PixelFormat::RGBFloat);
        if (!t) return false;
        float out[3] = {0, 0, 0};
        t->apply(in, out, 1);
        rgb = AdjustmentColor{out[0], out[1], out[2]}.clamped();
        if ((cmyk && mode == CMYK) || (!cmyk && mode == Lab)) native = values;
        return true;
    }
    case 8: rgb = AdjustmentColor{1 - v[0] / 10000.0, 1 - v[0] / 10000.0, 1 - v[0] / 10000.0}.clamped(); return true;   // Grayscale: 10000 is black
    default: return false;
    }
}

} // namespace

std::optional<AdjustmentSettings> levelsFrom(const uint8_t* data, size_t size, bool grey) {
    Reader r(data, size);
    if (r.u16() != 2) return std::nullopt;
    AdjustmentSettings s = AdjustmentSettings::defaults(AdjustmentKind::Levels);
    // The composite and the document's channels: R, G, B; C, M, Y, K; or Lab's L, a, b.
    for (int i = 0; i < levelsChannelCount && r.remaining() >= 10; i++) {
        double black = r.u16(), white = r.u16(), outBlack = r.u16(), outWhite = r.u16(), gamma = r.u16() / 100.0;
        s.levels.ranges[size_t(i)] = LevelsRange{black, gamma > 0 ? gamma : 1, white, outBlack, outWhite}.normalized();
    }
    // A Grayscale (or Duotone) file's one channel is record 1, Gray. It opens as RGB, where record 1 would be Red: it
    // goes to the composite, or to all three channels when the composite holds something too.
    if (grey) {
        const LevelsRange gray = s.levels.ranges[1], identity;
        for (size_t i = 1; i < s.levels.ranges.size(); i++) s.levels.ranges[i] = identity;
        if (s.levels.ranges[0] == identity) s.levels.ranges[0] = gray;
        else for (size_t i = 1; i <= 3; i++) s.levels.ranges[i] = gray;
    }
    return s;
}

std::optional<AdjustmentSettings> curvesFrom(const uint8_t* data, size_t size, bool grey) {
    Reader r(data, size);
    r.u8();
    if (r.u16() != 1) return std::nullopt;
    uint32_t mask = r.u32();
    AdjustmentSettings s = AdjustmentSettings::defaults(AdjustmentKind::Curves);
    // Bit 0 is the composite curve, then the document's channels (bits 1..3 red, green, blue; 1..4 cyan to black; 1..3
    // L, a, b); the rest (alpha, spot) have no home here.
    for (int channel = 0; channel < 32 && r.remaining() >= 2; channel++) {
        if (!(mask & (1u << channel))) continue;
        uint16_t count = r.u16();
        std::vector<CurvePoint> points;
        for (int i = 0; i < count && r.remaining() >= 4; i++) { double out = r.u16(), in = r.u16(); points.push_back({in, out}); }
        if (channel < levelsChannelCount && points.size() >= 2) s.curves.channels[size_t(channel)] = points;
    }
    // Grayscale (or Duotone): bit 1 is Gray, not Red (see levelsFrom).
    if (grey) {
        const std::vector<CurvePoint> gray = s.curves.channels[1], identity{{0, 0}, {255, 255}};
        for (size_t i = 1; i < s.curves.channels.size(); i++) s.curves.channels[i] = identity;
        if (s.curves.channels[0] == identity) s.curves.channels[0] = gray;
        else for (size_t i = 1; i <= 3; i++) s.curves.channels[i] = gray;
    }
    return s.curves.isValid() ? std::optional<AdjustmentSettings>(s) : std::nullopt;
}

std::optional<AdjustmentSettings> hueSaturationFrom(const uint8_t* data, size_t size) {
    Reader r(data, size);
    if (r.u16() != 2) return std::nullopt;
    AdjustmentSettings s = AdjustmentSettings::defaults(AdjustmentKind::HueSaturation);
    s.hsv.colorize = r.u8() != 0;
    r.u8();
    double colorizeHue = r.i16(), colorizeSat = r.i16(), colorizeLight = r.i16();
    double hue = r.i16(), sat = r.i16(), light = r.i16();
    s.hsv.photoshopSaturation = true;
    if (s.hsv.colorize) { s.hsv.adjustments[0] = {colorizeHue, colorizeSat, colorizeLight}; return s; }
    if (hue != 0 || sat != 0 || light != 0) s.hsv.adjustments[0] = {hue, sat, light};
    for (int range = 1; range <= 6 && r.remaining() >= 14; range++) {
        double a = r.i16(), b = r.i16(), c = r.i16(), d = r.i16();
        double h = r.i16(), sa = r.i16(), l = r.i16();
        s.hsv.bands[range] = HueBand{a, b, c, d};
        if (h != 0 || sa != 0 || l != 0) s.hsv.adjustments[range] = {h, sa, l};
    }
    return s;
}

std::optional<AdjustmentSettings> exposureFrom(const uint8_t* data, size_t size, bool grey) {
    Reader r(data, size);
    if (r.u16() != 1) return std::nullopt;
    AdjustmentSettings s = AdjustmentSettings::defaults(AdjustmentKind::Exposure);
    // A Grayscale file's Exposure linearises through Photoshop's grey tone curve, a 1.732 power (ExposureSettings).
    s.exposure = ExposureSettings{r.f32(), r.f32(), r.f32(), grey ? 1.732 : 2.2}.normalized();
    return s;
}

// ---- Photoshop's other adjustment layers (adjustments_more.cpp); a block that does not read stays carried ----

namespace {

std::optional<patchy::psd::DescriptorObject> versionedDescriptor(const uint8_t* data, size_t size) {
    try {
        patchy::psd::BigEndianReader r(std::span<const uint8_t>(data, size));
        if (r.read_u32() != 16) return std::nullopt;
        return patchy::psd::read_descriptor(r);
    } catch (std::exception&) { return std::nullopt; }
}

} // namespace

/// Brightness/Contrast: Photoshop 2026's descriptor ('CgEd') when it reads, else the legacy 'brit' (Patchy's rule).
std::optional<AdjustmentSettings> brightnessContrastFrom(const std::pair<const uint8_t*, size_t>* cged, const std::pair<const uint8_t*, size_t>* brit) {
    AdjustmentSettings s = AdjustmentSettings::defaults(AdjustmentKind::BrightnessContrast);
    if (cged)
        if (auto d = versionedDescriptor(cged->first, cged->second)) {
            const auto* b = patchy::psd::descriptor_value(*d, "Brgh");
            const auto* c = patchy::psd::descriptor_value(*d, "Cntr");
            if (b && c && b->type == patchy::psd::DescriptorValue::Type::Integer && c->type == patchy::psd::DescriptorValue::Type::Integer) {
                s.brightnessContrast.legacy = patchy::psd::descriptor_bool(*d, "useLegacy", false);
                s.brightnessContrast.brightness = b->integer_value;
                s.brightnessContrast.contrast = c->integer_value;
                s.brightnessContrast = s.brightnessContrast.normalized();
                return s;
            }
        }
    if (brit && brit->second >= 4) {
        Reader r(brit->first, brit->second);
        s.brightnessContrast.legacy = true;
        s.brightnessContrast.brightness = r.i16();
        s.brightnessContrast.contrast = r.i16();
        s.brightnessContrast = s.brightnessContrast.normalized();
        return s;
    }
    return std::nullopt;
}

std::optional<AdjustmentSettings> simpleAdjustmentFrom(const std::string& key, const uint8_t* data, size_t size) {
    try {
        Reader r(data, size);
        if (key == "nvrt") return AdjustmentSettings::defaults(AdjustmentKind::Invert);
        if (key == "post") {
            AdjustmentSettings s = AdjustmentSettings::defaults(AdjustmentKind::Posterize);
            s.posterize.levels = std::clamp(int(r.u16()), 2, 255);
            return s;
        }
        if (key == "thrs") {
            AdjustmentSettings s = AdjustmentSettings::defaults(AdjustmentKind::Threshold);
            s.threshold.level = std::clamp(int(r.u16()), 1, 255);
            return s;
        }
        if (key == "blnc") {
            // Shadows, midtones, highlights: cyan-red, magenta-green, yellow-blue each; then Preserve Luminosity.
            AdjustmentSettings s = AdjustmentSettings::defaults(AdjustmentKind::ColorBalance);
            for (auto& range : s.colorBalance.ranges) for (double& v : range) v = std::clamp(int(r.i16()), -100, 100);
            s.colorBalance.preserveLuminosity = r.remaining() > 0 && r.u8() != 0;
            return s;
        }
        if (key == "mixr") {
            // Version, monochrome, then per output channel: red, green, blue, (CMYK's fourth), constant, in percent.
            if (r.u16() != 1) return std::nullopt;
            AdjustmentSettings s = AdjustmentSettings::defaults(AdjustmentKind::ChannelMixer);
            s.channelMixer.monochrome = r.u16() != 0;
            for (int out = 0; out < 3 && r.remaining() >= 10; out++) {
                auto& row = s.channelMixer.rows[size_t(out)];
                row[0] = r.i16(); row[1] = r.i16(); row[2] = r.i16(); r.i16(); row[3] = r.i16();
                for (double& v : row) v = std::clamp(v, -200.0, 200.0);
            }
            // Monochrome keeps its grey in the first record.
            if (s.channelMixer.monochrome) s.channelMixer.rows[3] = s.channelMixer.rows[0];
            return s;
        }
        if (key == "mixr/cmyk") {
            // A CMYK file's: four output inks, each from cyan, magenta, yellow, black and a constant; monochrome's black
            // in the first record.
            if (r.u16() != 1) return std::nullopt;
            AdjustmentSettings s = AdjustmentSettings::defaults(AdjustmentKind::ChannelMixer);
            s.channelMixer.monochrome = r.u16() != 0;
            for (int out = 0; out < 4 && r.remaining() >= 10; out++) {
                auto& row = s.channelMixer.inks[size_t(out)];
                for (double& v : row) v = std::clamp(double(r.i16()), -200.0, 200.0);
            }
            if (s.channelMixer.monochrome) s.channelMixer.inks[3] = s.channelMixer.inks[0];
            return s;
        }
        if (key == "selc") {
            // Version, method (0 relative, 1 absolute), then ten CMYK records: a reserved one, then reds .. blacks.
            if (r.u16() != 1) return std::nullopt;
            AdjustmentSettings s = AdjustmentSettings::defaults(AdjustmentKind::SelectiveColor);
            s.selectiveColor.absolute = r.u16() == 1;
            for (int i = 0; i < 4; i++) r.i16();
            for (auto& range : s.selectiveColor.ranges) for (double& v : range) v = std::clamp(int(r.i16()), -100, 100);
            return s;
        }
        if (key == "phfl") {
            // Version 2 (a colour space and four components) or 3 (XYZ); density in percent; Preserve Luminosity.
            const uint16_t version = r.u16();
            AdjustmentSettings s = AdjustmentSettings::defaults(AdjustmentKind::PhotoFilter);
            if (version == 3) {
                // XYZ as three 32-bit numbers. Their scale is not documented: taken as fixed 16.16 when that gives a Y
                // in range, else as hundredths (unchecked against Photoshop; docs/adjustment-layers.md).
                double xyz[3];
                for (double& v : xyz) v = double(int32_t(r.u32()));
                double k = 65536;
                if (!(xyz[1] / k > 0.001 && xyz[1] / k <= 1.2)) k = xyz[1] > 0 && xyz[1] <= 120 ? 100 : 10000;
                const double X = xyz[0] / k, Y = xyz[1] / k, Z = xyz[2] / k;
                // D50 XYZ (Photoshop's connection space) to linear sRGB, then encoded.
                double lin[3] = {3.1338561 * X - 1.6168667 * Y - 0.4906146 * Z, -0.9787684 * X + 1.9161415 * Y + 0.0334540 * Z,
                                 0.0719453 * X - 0.2289914 * Y + 1.4052427 * Z};
                for (double& v : lin) { v = std::clamp(v, 0.0, 1.0); v = v <= 0.0031308 ? 12.92 * v : 1.055 * std::pow(v, 1 / 2.4) - 0.055; }
                s.photoFilter.color = {lin[0], lin[1], lin[2]};
            } else if (version == 2) {
                // A colour record: Photoshop 27 saves its filters in Lab whatever the document's mode.
                const int space = r.i16();
                uint16_t v[4];
                for (uint16_t& x : v) x = r.u16();
                std::optional<std::array<double, 4>> native;
                if (!colourRecord(space, v, 3, ColorProfile(), s.photoFilter.color, native)) return std::nullopt;
            } else return std::nullopt;
            s.photoFilter.density = std::clamp(double(r.u32()), 0.0, 100.0);
            s.photoFilter.preserveLuminosity = r.remaining() > 0 && r.u8() != 0;
            return s;
        }
        if (key == "clrL") {
            // A version, then Photoshop's descriptor holding the LUT file itself (LUT3DFileData, its LUTFormat) or an
            // ICC profile ('profile'), its name and Dither.
            if (r.u16() != 1) return std::nullopt;
            auto d = versionedDescriptor(data + 2, size - 2);
            if (!d) return std::nullopt;
            AdjustmentSettings s = AdjustmentSettings::defaults(AdjustmentKind::ColorLookup);
            auto raw = [&](const char* k) -> std::vector<uint8_t> {
                const auto* v = patchy::psd::descriptor_value(*d, k);
                return v && v->type == patchy::psd::DescriptorValue::Type::Raw ? v->raw_value : std::vector<uint8_t>{};
            };
            auto text = [&](const char* k) {
                const auto* v = patchy::psd::descriptor_value(*d, k);
                return v && v->type == patchy::psd::DescriptorValue::Type::String ? v->string_value : std::string();
            };
            const std::vector<uint8_t> lut = raw("LUT3DFileData");
            const std::vector<uint8_t> profile = raw("profile");
            s.colorLookup.dither = patchy::psd::descriptor_bool(*d, "Dthr", false);
            s.colorLookup.name = text("LUT3DFileName");
            if (s.colorLookup.name.empty()) s.colorLookup.name = text("Nm  ");
            if (!lut.empty()) {
                std::string body(lut.begin(), lut.end());
                const auto* fmt = patchy::psd::descriptor_value(*d, "LUTFormat");
                const std::string f = fmt && fmt->type == patchy::psd::DescriptorValue::Type::Enum ? fmt->enum_value : std::string();
                s.colorLookup.format = f.find("3DL") != std::string::npos ? "3dl"
                    : f.find("CUBE") != std::string::npos || body.find("LUT_3D_SIZE") != std::string::npos || body.find("LUT_1D_SIZE") != std::string::npos ? "cube" : "3dl";
                s.colorLookup.data = std::move(body);
            } else if (!profile.empty()) {
                s.colorLookup.format = "icc";
                s.colorLookup.data = toBase64(profile);
            }
            return s;
        }
        if (key == "blwh" || key == "vibA") {
            auto d = versionedDescriptor(data, size);
            if (!d) return std::nullopt;
            auto integer = [&](const char* k, double fallback) {
                const auto* v = patchy::psd::descriptor_value(*d, k);
                return v && v->type == patchy::psd::DescriptorValue::Type::Integer ? double(v->integer_value) : fallback;
            };
            if (key == "vibA") {
                AdjustmentSettings s = AdjustmentSettings::defaults(AdjustmentKind::Vibrance);
                s.vibrance = {std::clamp(integer("vibrance", 0), -100.0, 100.0), std::clamp(integer("Strt", 0), -100.0, 100.0)};
                return s;
            }
            AdjustmentSettings s = AdjustmentSettings::defaults(AdjustmentKind::BlackWhite);
            const char* keys[6] = {"Rd  ", "Yllw", "Grn ", "Cyn ", "Bl  ", "Mgnt"};
            for (size_t i = 0; i < 6; i++) s.blackWhite.weights[i] = std::clamp(integer(keys[i], s.blackWhite.weights[i]), -200.0, 300.0);
            s.blackWhite.tint = patchy::psd::descriptor_bool(*d, "useTint", false);
            if (const auto* tint = patchy::psd::descriptor_object(*d, "tintColor"))
                s.blackWhite.tintColor = AdjustmentColor{patchy::psd::descriptor_number(*tint, "Rd  ", 225) / 255, patchy::psd::descriptor_number(*tint, "Grn ", 211) / 255,
                                                         patchy::psd::descriptor_number(*tint, "Bl  ", 179) / 255}.clamped();
            return s;
        }
    } catch (...) {}
    return std::nullopt;
}

std::optional<AdjustmentSettings> gradientMapFrom(const uint8_t* data, size_t size, int mode, const ColorProfile& profile) {
    Reader r(data, size);
    const uint16_t version = r.u16();
    if (version != 1 && version != 3) return std::nullopt;
    AdjustmentSettings s = AdjustmentSettings::defaults(AdjustmentKind::GradientMap);
    s.gradientMap.reversed = r.u8() != 0;
    s.gradientMap.dither = r.u8() != 0;
    // Version 3 (Photoshop 2023 and later) adds the Method after Dither: the gradientInterpolationMethodType's
    // four-character code (a Photoshop-saved Perceptual map in ag-psd's tests holds 'Perc', Classic 'Gcls'). A zero first
    // byte is the name's length instead (as ag-psd reads it: src/additionalInfo.ts at 3870496, MIT; THIRD-PARTY-NOTICES.md).
    if (version == 3 && r.remaining() >= 4 && data[r.position()] != 0) {
        std::string code;
        for (int i = 0; i < 4; i++) code += char(r.u8());
        s.gradientMap.method = code == "Perc" ? GradientMethod::Perceptual : code == "Lnr " ? GradientMethod::Linear : GradientMethod::Classic;
    }
    r.unicode();
    const uint16_t count = r.u16();
    // Each stop is 20 bytes: location (0..4096), midpoint (percent), the colour (space and four values), two bytes of
    // padding. The colours are in the document's space (a CMYK file's in CMYK, a Grayscale file's in Grey).
    std::vector<GradientColorStop> stops;
    std::vector<std::array<double, 4>> native;
    bool allNative = true;
    for (int i = 0; i < count && r.remaining() >= 20; i++) {
        GradientColorStop stop;
        stop.location = float(std::clamp(int32_t(r.u32()) / 4096.0, 0.0, 1.0));
        stop.midpoint = float(std::clamp(int32_t(r.u32()) / 100.0, 0.0, 1.0));
        const int space = r.i16();
        uint16_t v[4];
        for (uint16_t& x : v) x = r.u16();
        r.u16();
        AdjustmentColor rgb;
        std::optional<std::array<double, 4>> values;
        if (!colourRecord(space, v, mode, profile, rgb, values)) rgb = AdjustmentColor{v[0] / 65535.0, v[1] / 65535.0, v[2] / 65535.0};
        stop.rgb[0] = float(rgb.red); stop.rgb[1] = float(rgb.green); stop.rgb[2] = float(rgb.blue);
        if (values) native.push_back(*values);
        else allNative = false;
        stops.push_back(stop);
    }
    // The opacity stops (a map's colours are opaque), then the expansion count and the Smoothness (0..4096).
    const uint16_t opacities = r.remaining() >= 2 ? r.u16() : 0;
    for (int i = 0; i < opacities && r.remaining() >= 10; i++) { r.u32(); r.u32(); r.u16(); }
    if (r.remaining() >= 4) { r.u16(); s.gradientMap.smoothness = std::clamp(r.u16() / 4096.0, 0.0, 1.0); }
    if (stops.empty()) return s;
    std::stable_sort(stops.begin(), stops.end(), [](const GradientColorStop& a, const GradientColorStop& b) { return a.location < b.location; });
    s.gradientMap.shadows = AdjustmentColor{stops.front().rgb[0], stops.front().rgb[1], stops.front().rgb[2]}.clamped();
    s.gradientMap.highlights = AdjustmentColor{stops.back().rgb[0], stops.back().rgb[1], stops.back().rgb[2]}.clamped();
    // A plain two-stop ramp from end to end in an RGB document needs no stops of its own.
    const bool plain = stops.size() == 2 && stops.front().location == 0 && stops.back().location == 1 && stops.back().midpoint == 0.5f;
    if (!plain || !native.empty()) {
        s.gradientMap.stops = stops;
        if (allNative && native.size() == stops.size()) {
            s.gradientMap.native = native;
            s.gradientMap.nativeMode = mode == CMYK ? ColorMode::CMYK : ColorMode::Lab;
        }
    }
    return s;
}

/// A solid colour fill layer's colour, from its descriptor.
std::optional<AdjustmentColor> solidColourFrom(const uint8_t* data, size_t size) {
    try {
        Reader r(data, size);
        Descriptor d = readDescriptor(r);
        const Descriptor* colour = d.item("Clr ");
        if (!colour) return std::nullopt;
        auto channel = [&](const char* key) { const Descriptor* v = colour->item(key); return v ? std::clamp(v->number / 255.0, 0.0, 1.0) : 0.0; };
        return AdjustmentColor{channel("Rd  "), channel("Grn "), channel("Bl  ")};
    } catch (Truncated&) { return std::nullopt; }
}

/// The text of a type layer, from its descriptor.
std::optional<std::string> textFrom(const uint8_t* data, size_t size) {
    try {
        Reader r(data, size);
        if (r.u16() != 1) return std::nullopt;
        for (int i = 0; i < 6; i++) r.f64();
        if (r.u16() != 50) return std::nullopt;
        Descriptor d = readDescriptor(r);
        const Descriptor* text = d.item("Txt ");
        return text ? std::optional<std::string>(text->text) : std::nullopt;
    } catch (Truncated&) { return std::nullopt; }
}

std::string jsonEscape(const std::string& s) {
    std::string out;
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { out += '\\'; out += char(c); }
        else if (c < 0x20) { char buf[8]; std::snprintf(buf, sizeof buf, "\\u%04x", c); out += buf; }
        else out += char(c);
    }
    return out;
}

} // namespace nekophoto::psd_read
