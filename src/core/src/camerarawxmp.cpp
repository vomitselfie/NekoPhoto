#include "nekophoto/camerarawxmp.h"
#include "svg_xml.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <set>
#include <span>
#include <stdexcept>

namespace nekophoto {

namespace {

using Setter = std::function<bool(CameraRawSettings&, double, const std::string&)>;

/// A slider: the value as it is.
template <typename F> Setter set(F field) {
    return [field](CameraRawSettings& s, double v, const std::string&) { field(s) = v; return true; };
}

/// Attributes that describe the preset or the file rather than a develop setting.
const std::set<std::string>& metadata() {
    static const std::set<std::string> keys{
        "Version", "ProcessVersion", "PresetType", "UUID", "Name", "ShortName", "SortName", "Group", "Description", "Copyright",
        "ContactInfo", "Cluster", "SupportsAmount", "SupportsAmount2", "SupportsColor", "SupportsMonochrome", "SupportsHighDynamicRange",
        "SupportsNormalDynamicRange", "SupportsSceneReferred", "SupportsOutputReferred", "RequiresRGBTables", "CameraModelRestriction",
        "HasSettings", "AlreadyApplied", "ToneCurveName2012", "ToneCurveName", "LensProfileSetup", "LensProfileName", "LensProfileFilename",
        "LensProfileDigest", "LensProfileIsEmbedded", "LensProfileChromaticAberrationScale",
        "UprightVersion", "UprightCenterMode", "UprightCenterNormX", "UprightCenterNormY", "UprightFocalMode", "UprightFocalLength35mm",
        "UprightPreview", "UprightTransformCount", "UprightFourSegmentsCount", "RawFileName", "OverrideLookVignette", "GrainSeed",
        "CropConstrainToWarp", "CropConstrainToUnitSquare", "HDREditMode", "PresetAmount", "CameraProfileDigest", "LookName",
        "DefringePurpleAmountSet", "Converter", "AutoTone", "AutoExposure", "AutoContrast", "AutoShadows", "AutoBrightness"};
    return keys;
}

const std::array<const char*, 8> bands{"Red", "Orange", "Yellow", "Green", "Aqua", "Blue", "Purple", "Magenta"};

std::map<std::string, Setter> setters(bool raw) {
    std::map<std::string, Setter> m{
        {"Exposure2012", set([](CameraRawSettings& s) -> double& { return s.exposure; })},
        {"Contrast2012", set([](CameraRawSettings& s) -> double& { return s.contrast; })},
        {"Highlights2012", set([](CameraRawSettings& s) -> double& { return s.highlights; })},
        {"Shadows2012", set([](CameraRawSettings& s) -> double& { return s.shadows; })},
        {"Whites2012", set([](CameraRawSettings& s) -> double& { return s.whites; })},
        {"Blacks2012", set([](CameraRawSettings& s) -> double& { return s.blacks; })},
        {"Texture", set([](CameraRawSettings& s) -> double& { return s.texture; })},
        {"Clarity2012", set([](CameraRawSettings& s) -> double& { return s.clarity; })},
        {"Dehaze", set([](CameraRawSettings& s) -> double& { return s.dehaze; })},
        {"Vibrance", set([](CameraRawSettings& s) -> double& { return s.vibrance; })},
        {"Saturation", set([](CameraRawSettings& s) -> double& { return s.saturation; })},
        {"ParametricShadows", set([](CameraRawSettings& s) -> double& { return s.curve.shadows; })},
        {"ParametricDarks", set([](CameraRawSettings& s) -> double& { return s.curve.darks; })},
        {"ParametricLights", set([](CameraRawSettings& s) -> double& { return s.curve.lights; })},
        {"ParametricHighlights", set([](CameraRawSettings& s) -> double& { return s.curve.highlights; })},
        {"ParametricShadowSplit", set([](CameraRawSettings& s) -> double& { return s.curve.shadowSplit; })},
        {"ParametricMidtoneSplit", set([](CameraRawSettings& s) -> double& { return s.curve.darkSplit; })},
        {"ParametricHighlightSplit", set([](CameraRawSettings& s) -> double& { return s.curve.lightSplit; })},
        // Lightroom's Refine Saturation is 0..200 with 100 the curve as drawn; this engine's is that less 100.
        {"CurveRefineSaturation", [](CameraRawSettings& s, double v, const std::string&) { s.curve.refineSaturation = v - 100; return true; }},
        {"SplitToningShadowHue", set([](CameraRawSettings& s) -> double& { return s.grading.shadows.hue; })},
        {"SplitToningShadowSaturation", set([](CameraRawSettings& s) -> double& { return s.grading.shadows.saturation; })},
        {"SplitToningHighlightHue", set([](CameraRawSettings& s) -> double& { return s.grading.highlights.hue; })},
        {"SplitToningHighlightSaturation", set([](CameraRawSettings& s) -> double& { return s.grading.highlights.saturation; })},
        {"SplitToningBalance", set([](CameraRawSettings& s) -> double& { return s.grading.balance; })},
        {"ColorGradeMidtoneHue", set([](CameraRawSettings& s) -> double& { return s.grading.midtones.hue; })},
        {"ColorGradeMidtoneSat", set([](CameraRawSettings& s) -> double& { return s.grading.midtones.saturation; })},
        {"ColorGradeGlobalHue", set([](CameraRawSettings& s) -> double& { return s.grading.global.hue; })},
        {"ColorGradeGlobalSat", set([](CameraRawSettings& s) -> double& { return s.grading.global.saturation; })},
        {"ColorGradeShadowLum", set([](CameraRawSettings& s) -> double& { return s.grading.shadows.luminance; })},
        {"ColorGradeMidtoneLum", set([](CameraRawSettings& s) -> double& { return s.grading.midtones.luminance; })},
        {"ColorGradeHighlightLum", set([](CameraRawSettings& s) -> double& { return s.grading.highlights.luminance; })},
        {"ColorGradeGlobalLum", set([](CameraRawSettings& s) -> double& { return s.grading.global.luminance; })},
        {"ColorGradeBlending", set([](CameraRawSettings& s) -> double& { return s.grading.blending; })},
        {"ShadowTint", set([](CameraRawSettings& s) -> double& { return s.calibration.shadowTint; })},
        {"RedHue", set([](CameraRawSettings& s) -> double& { return s.calibration.redHue; })},
        {"RedSaturation", set([](CameraRawSettings& s) -> double& { return s.calibration.redSaturation; })},
        {"GreenHue", set([](CameraRawSettings& s) -> double& { return s.calibration.greenHue; })},
        {"GreenSaturation", set([](CameraRawSettings& s) -> double& { return s.calibration.greenSaturation; })},
        {"BlueHue", set([](CameraRawSettings& s) -> double& { return s.calibration.blueHue; })},
        {"BlueSaturation", set([](CameraRawSettings& s) -> double& { return s.calibration.blueSaturation; })},
        {"PostCropVignetteAmount", set([](CameraRawSettings& s) -> double& { return s.vignetteAmount; })},
        {"PostCropVignetteMidpoint", set([](CameraRawSettings& s) -> double& { return s.vignetteMidpoint; })},
        {"PostCropVignetteFeather", set([](CameraRawSettings& s) -> double& { return s.vignetteFeather; })},
        {"PostCropVignetteRoundness", set([](CameraRawSettings& s) -> double& { return s.vignetteRoundness; })},
        {"PostCropVignetteHighlightContrast", set([](CameraRawSettings& s) -> double& { return s.vignetteHighlights; })},
        // Lightroom's style codes: 1 Highlight Priority, 2 Color Priority, 3 Paint Overlay (0 reads as Highlight Priority).
        {"PostCropVignetteStyle", [](CameraRawSettings& s, double v, const std::string&) {
             s.vignetteStyle = v == 2 ? CameraRawVignetteStyle::ColorPriority : v == 3 ? CameraRawVignetteStyle::PaintOverlay
                                                                                   : CameraRawVignetteStyle::HighlightPriority;
             return v >= 0 && v <= 3;
         }},
        {"GrainAmount", set([](CameraRawSettings& s) -> double& { return s.grainAmount; })},
        {"GrainSize", set([](CameraRawSettings& s) -> double& { return s.grainSize; })},
        {"GrainFrequency", set([](CameraRawSettings& s) -> double& { return s.grainRoughness; })},
        {"VignetteAmount", set([](CameraRawSettings& s) -> double& { return s.optics.vignetteAmount; })},
        {"VignetteMidpoint", set([](CameraRawSettings& s) -> double& { return s.optics.vignetteMidpoint; })},
        {"LensManualDistortionAmount", set([](CameraRawSettings& s) -> double& { return s.optics.distortion; })},
        {"LensProfileEnable", [](CameraRawSettings& s, double v, const std::string&) { s.optics.enableLensProfile = v != 0; return true; }},
        {"LensProfileDistortionScale", set([](CameraRawSettings& s) -> double& { return s.optics.profileDistortion; })},
        {"LensProfileVignettingScale", set([](CameraRawSettings& s) -> double& { return s.optics.profileVignetting; })},
        {"AutoLateralCA", [](CameraRawSettings& s, double v, const std::string&) { s.optics.removeChromaticAberration = v != 0; return true; }},
        {"DefringePurpleAmount", set([](CameraRawSettings& s) -> double& { return s.optics.purpleAmount; })},
        {"DefringePurpleHueLo", set([](CameraRawSettings& s) -> double& { return s.optics.purpleHueLow; })},
        {"DefringePurpleHueHi", set([](CameraRawSettings& s) -> double& { return s.optics.purpleHueHigh; })},
        {"DefringeGreenAmount", set([](CameraRawSettings& s) -> double& { return s.optics.greenAmount; })},
        {"DefringeGreenHueLo", set([](CameraRawSettings& s) -> double& { return s.optics.greenHueLow; })},
        {"DefringeGreenHueHi", set([](CameraRawSettings& s) -> double& { return s.optics.greenHueHigh; })},
        {"PerspectiveVertical", set([](CameraRawSettings& s) -> double& { return s.geometry.vertical; })},
        {"PerspectiveHorizontal", set([](CameraRawSettings& s) -> double& { return s.geometry.horizontal; })},
        {"PerspectiveRotate", set([](CameraRawSettings& s) -> double& { return s.geometry.rotate; })},
        {"PerspectiveAspect", set([](CameraRawSettings& s) -> double& { return s.geometry.aspect; })},
        // Camera Raw's Scale is 50..150 around 100; this engine's -100..100 around 0.
        {"PerspectiveScale", [](CameraRawSettings& s, double v, const std::string&) { s.geometry.scale = v - 100; return true; }},
        {"PerspectiveX", set([](CameraRawSettings& s) -> double& { return s.geometry.offsetX; })},
        {"PerspectiveY", set([](CameraRawSettings& s) -> double& { return s.geometry.offsetY; })},
        {"PerspectiveUpright", [](CameraRawSettings&, double v, const std::string&) { return v == 0; }},   // Upright analysis: Off only
        {"Sharpness", set([](CameraRawSettings& s) -> double& { return s.detail.sharpenAmount; })},
        // Camera Raw's radius is 0.5..3 pixels; the sharpening kernel's slider is 0..100 over the same span.
        {"SharpenRadius", [](CameraRawSettings& s, double v, const std::string&) { s.detail.sharpenRadius = (v - 0.5) / 2.5 * 100; return true; }},
        {"SharpenDetail", set([](CameraRawSettings& s) -> double& { return s.detail.sharpenDetail; })},
        {"SharpenEdgeMasking", set([](CameraRawSettings& s) -> double& { return s.detail.sharpenMasking; })},
        {"LuminanceSmoothing", set([](CameraRawSettings& s) -> double& { return s.detail.noiseLuminance; })},
        {"LuminanceNoiseReductionDetail", set([](CameraRawSettings& s) -> double& { return s.detail.noiseLuminanceDetail; })},
        {"LuminanceNoiseReductionContrast", set([](CameraRawSettings& s) -> double& { return s.detail.noiseLuminanceContrast; })},
        {"ColorNoiseReduction", set([](CameraRawSettings& s) -> double& { return s.detail.noiseColor; })},
        {"ColorNoiseReductionDetail", set([](CameraRawSettings& s) -> double& { return s.detail.noiseColorDetail; })},
        {"ColorNoiseReductionSmoothness", set([](CameraRawSettings& s) -> double& { return s.detail.noiseColorSmoothness; })},
        // Settings at the value that changes nothing are fine to read; other values are effects this engine lacks.
        {"ConvertToGrayscale", [](CameraRawSettings&, double, const std::string& t) { return t == "False" || t == "false"; }},
        {"HasCrop", [](CameraRawSettings&, double, const std::string& t) { return t == "False" || t == "false"; }},
        {"AutoGrayscaleMix", [](CameraRawSettings&, double, const std::string& t) { return t == "False" || t == "false"; }},
    };
    if (raw) {
        m["Temperature"] = [](CameraRawSettings& s, double v, const std::string&) { s.rawTemperature = v; return true; };
        m["Tint"] = [](CameraRawSettings& s, double v, const std::string&) { s.rawTint = v; return true; };
    } else {
        m["IncrementalTemperature"] = set([](CameraRawSettings& s) -> double& { return s.temperature; });
        m["IncrementalTint"] = set([](CameraRawSettings& s) -> double& { return s.tint; });
    }
    for (size_t b = 0; b < bands.size(); b++) {
        const std::string name = bands[b];
        m["HueAdjustment" + name] = [b](CameraRawSettings& s, double v, const std::string&) { s.mixer.hue[b] = v; return true; };
        m["SaturationAdjustment" + name] = [b](CameraRawSettings& s, double v, const std::string&) { s.mixer.saturation[b] = v; return true; };
        m["LuminanceAdjustment" + name] = [b](CameraRawSettings& s, double v, const std::string&) { s.mixer.luminance[b] = v; return true; };
    }
    return m;
}

std::vector<CameraRawCurvePoint> curvePoints(const std::vector<std::string>& items) {
    std::vector<CameraRawCurvePoint> points;
    for (const auto& item : items) {
        double x = 0, y = 0;
        if (std::sscanf(item.c_str(), "%lf , %lf", &x, &y) == 2) points.push_back({x / 255, y / 255});
    }
    return points;
}

bool isNumber(const std::string& text, double& out) {
    if (text.empty()) return false;
    char* end = nullptr;
    out = std::strtod(text.c_str(), &end);
    return end && *end == '\0' && std::isfinite(out);
}

/// Panel switches: off resets the panel to its defaults, as a switched-off panel renders nothing.
bool panelSwitch(const std::string& key, const std::string& text, CameraRawSettings& s) {
    static const std::map<std::string, std::function<void(CameraRawPanels&)>> panels{
        {"EnableToneCurve", [](CameraRawPanels& p) { p.curve = false; }},
        {"EnableColorAdjustments", [](CameraRawPanels& p) { p.mixer = false; }},
        {"EnableSplitToning", [](CameraRawPanels& p) { p.grading = false; }},
        {"EnableDetail", [](CameraRawPanels& p) { p.detail = false; }},
        {"EnableLensCorrections", [](CameraRawPanels& p) { p.optics = false; }},
        {"EnableTransform", [](CameraRawPanels& p) { p.geometry = false; }},
        {"EnableEffects", [](CameraRawPanels& p) { p.effects = false; }},
        {"EnableCalibration", [](CameraRawPanels& p) { p.calibration = false; }},
    };
    auto it = panels.find(key);
    if (it == panels.end()) return false;
    if (text == "False" || text == "false") {
        CameraRawPanels off;
        it->second(off);
        s = s.applying(off);
    }
    return true;
}

} // namespace

CameraRawXmp cameraRawFromXmpAttributes(const std::map<std::string, std::string>& attributes,
                                        const std::map<std::string, std::vector<std::string>>& sequences, const CameraRawSettings& base,
                                        bool raw) {
    CameraRawXmp out;
    out.settings = base;
    CameraRawSettings& s = out.settings;
    const auto table = setters(raw);
    std::vector<std::pair<std::string, std::string>> switches;
    bool grayscale = false;
    for (const auto& [key, text] : attributes) {
        if (key == "ConvertToGrayscale" && (text == "True" || text == "true")) grayscale = true;
        if (key.rfind("Enable", 0) == 0) { switches.push_back({key, text}); continue; }
        if (metadata().count(key)) continue;
        if (key == "WhiteBalance") {
            static const std::map<std::string, CameraRawWhiteBalance> modes{
                {"As Shot", CameraRawWhiteBalance::AsShot}, {"Auto", CameraRawWhiteBalance::Auto}, {"Custom", CameraRawWhiteBalance::Custom},
                {"Daylight", CameraRawWhiteBalance::Daylight}, {"Cloudy", CameraRawWhiteBalance::Cloudy}, {"Shade", CameraRawWhiteBalance::Shade},
                {"Tungsten", CameraRawWhiteBalance::Tungsten}, {"Fluorescent", CameraRawWhiteBalance::Fluorescent},
                {"Flash", CameraRawWhiteBalance::Flash}};
            auto mode = modes.find(text);
            const bool fits = mode != modes.end() && (raw || mode->second == CameraRawWhiteBalance::Custom || mode->second == CameraRawWhiteBalance::Auto
                                                      || mode->second == CameraRawWhiteBalance::AsShot);
            if (!fits) { out.unsupported.push_back(key + "=" + text); continue; }
            // On pixels As Shot is the picture as it is: Custom with nothing moved.
            s.whiteBalance = !raw && mode->second == CameraRawWhiteBalance::AsShot ? CameraRawWhiteBalance::Custom : mode->second;
            out.applied.push_back(key);
            continue;
        }
        auto setter = table.find(key);
        double value = 0;
        const bool numeric = isNumber(text, value);
        if (setter == table.end() || !setter->second(s, numeric ? value : 0, text)) {
            // A setting this engine lacks, unless it is at the value that changes nothing.
            if (!(numeric && value == 0) || key == "PerspectiveScale") out.unsupported.push_back(key + "=" + text);
            continue;
        }
        if (!numeric && text != "True" && text != "False" && text != "true" && text != "false") {
            out.unsupported.push_back(key + "=" + text);
            continue;
        }
        out.applied.push_back(key);
    }
    if (grayscale)
        for (const auto& [key, text] : attributes)
            if (key.rfind("GrayMixer", 0) == 0) out.unsupported.push_back(key + "=" + text);
    for (const auto& [key, items] : sequences) {
        if (key == "ToneCurvePV2012") s.curve.rgb = curvePoints(items);
        else if (key == "ToneCurvePV2012Red") s.curve.red = curvePoints(items);
        else if (key == "ToneCurvePV2012Green") s.curve.green = curvePoints(items);
        else if (key == "ToneCurvePV2012Blue") s.curve.blue = curvePoints(items);
        else if (key == "ToneCurve" || key == "ToneCurveRed" || key == "ToneCurveGreen" || key == "ToneCurveBlue") {
            // The 2010 process's curves: listed unless straight.
            if (!CameraRawCurveSettings::isLinear(curvePoints(items))) out.unsupported.push_back(key);
            continue;
        } else {
            out.unsupported.push_back(key);
            continue;
        }
        out.applied.push_back(key);
    }
    // A legacy split-toning record (SplitToning keys, no ColorGrade ones) renders as Colour Grading at Blending 100, as
    // Camera Raw 18.7 renders one (RAWmakase's measurement, docs/camera-raw.md#fitted-responses).
    const auto has = [&](const char* prefix) {
        return std::any_of(attributes.begin(), attributes.end(), [&](const auto& item) { return item.first.rfind(prefix, 0) == 0; });
    };
    if (has("SplitToning") && !has("ColorGrade")) s.grading.blending = 100;
    for (const auto& [key, text] : switches)
        if (!panelSwitch(key, text, s) && (text == "True" || text == "true")) out.unsupported.push_back(key + "=" + text);
    if (raw && s.rawTemperature > 0 && s.whiteBalance != CameraRawWhiteBalance::AsShot && s.whiteBalance != CameraRawWhiteBalance::Auto)
        s.whiteBalance = CameraRawWhiteBalance::Custom;
    return out;
}

namespace {

/// The local name of a crs: element or attribute, or empty.
std::string crsName(const std::string& qualified) {
    return qualified.rfind("crs:", 0) == 0 ? qualified.substr(4) : std::string();
}

void collect(const svgxml::XmlNode& node, std::map<std::string, std::string>& attributes, std::map<std::string, std::vector<std::string>>& sequences,
             std::string& name) {
    for (const auto& [key, value] : node.attributes)
        if (const std::string local = crsName(key); !local.empty()) {
            if (local == "Name") name = value;
            else attributes[local] = value;
        }
    for (const auto& child : node.children) {
        if (child.is_text()) continue;
        const std::string local = crsName(child.name);
        if (local.empty()) {
            collect(child, attributes, sequences, name);
            continue;
        }
        // crs:Name and the other language alternatives, the point curves as sequences, the rest as text.
        std::vector<std::string> items;
        bool listed = false;
        for (const auto& list : child.children)
            if (list.name == "rdf:Seq" || list.name == "rdf:Alt" || list.name == "rdf:Bag") {
                listed = true;
                for (const auto& li : list.children)
                    if (li.name == "rdf:li") items.push_back(li.all_text());
            }
        if (local == "Name") { if (!items.empty()) name = items.front(); else name = child.all_text(); continue; }
        if (listed) {
            if (local.rfind("ToneCurve", 0) == 0 || local == "PointColors" || local == "ColorVariance") sequences[local] = items;
            else if (!items.empty()) attributes[local] = items.front();
            continue;
        }
        if (child.children.empty() || std::all_of(child.children.begin(), child.children.end(), [](const auto& c) { return c.is_text(); }))
            attributes[local] = child.all_text();
        else
            attributes[local] = "(structure)";   // a Look, masks, retouching: not a setting this engine reads
    }
}

} // namespace

std::optional<CameraRawXmp> readCameraRawXmp(const std::vector<uint8_t>& bytes, const CameraRawSettings& base, bool raw, std::string* error) {
    svgxml::XmlNode root;
    try {
        root = svgxml::parse_xml(std::span<const uint8_t>(bytes.data(), bytes.size()));
    } catch (const std::exception& e) {
        if (error) *error = std::string("Not an XMP file: ") + e.what();
        return std::nullopt;
    }
    std::map<std::string, std::string> attributes;
    std::map<std::string, std::vector<std::string>> sequences;
    std::string name;
    collect(root, attributes, sequences, name);
    if (attributes.empty() && sequences.empty()) {
        if (error) *error = "The file holds no Camera Raw settings.";
        return std::nullopt;
    }
    CameraRawXmp out = cameraRawFromXmpAttributes(attributes, sequences, base, raw);
    out.name = name;
    return out;
}

} // namespace nekophoto
