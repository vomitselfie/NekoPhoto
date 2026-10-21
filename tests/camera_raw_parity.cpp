// Camera Raw parity: the synthetic colour chart of RAWmakase's test corpus (https://github.com/pch/rawmakase, MIT,
// LICENSES/RAWmakase-MIT.txt; the files are in tests/fixtures/camera-raw-parity) developed as File > Open develops a RAW
// file (developRaw), once per settings case, each patch measured as its mean encoded sRGB and compared by CIEDE2000 with
// Camera Raw 18.7's render of the same case (camera-raw-synthetic-d65.json, from the corpus).
//
// The cases are Camera Raw XMP attributes (cases.json); those this engine has no control for (looks, Point Color)
// are listed and skipped. Each case's mean and 95th-percentile ΔE00 over the patches (the wide-gamut row left out, as
// the corpus does) are held to tests/camera_raw_parity_baseline.txt: a case fails when it gets worse by more than 0.1 on
// the mean or 0.3 on the 95th percentile, RAWmakase's margins. NEKOPHOTO_BLESS_CAMERA_RAW_PARITY=1 rewrites the baseline
// after an intended change. NEKOPHOTO_CAMERA_RAW_PARITY_DUMP=<file> writes every case's patch values as JSON for fitting;
// NEKOPHOTO_CAMERA_RAW_PARITY_ONLY=<text> runs only the cases whose name contains it, and
// NEKOPHOTO_CAMERA_RAW_PARITY_TRY="<case>=<settings JSON>" patches one case's settings, to try a mapping by hand.
#include "check.h"
#include "nekophoto/camerarawxmp.h"
#include "nekophoto/raw.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <sstream>

using namespace nekophoto;
using json = nlohmann::json;

namespace {

const std::string corpus = CAMERA_RAW_PARITY_DIR;
const std::string chartName = "synthetic-d65";

json readJson(const std::string& path) {
    std::ifstream in(path);
    if (!in) return json();
    return json::parse(in, nullptr, false);
}

struct Patch {
    std::string name, group;
    int x = 0, y = 0, w = 0, h = 0;
};

std::vector<Patch> readLayout() {
    std::vector<Patch> patches;
    const json layout = readJson(corpus + "/layout.json");
    if (!layout.is_object()) return patches;
    for (const auto& p : layout["patches"])
        patches.push_back({p["name"].get<std::string>(), p["group"].get<std::string>(), p["x"].get<int>(), p["y"].get<int>(), p["w"].get<int>(),
                           p["h"].get<int>()});
    return patches;
}

using Rgb = std::array<double, 3>;

/// Each patch's mean encoded value on the 16-bit scale (0..65535), as the corpus records Camera Raw's. Where Geometry or
/// Distortion uncovers the layer (transparent here), Camera Raw renders white, so the pixels are composited on white.
std::vector<Rgb> measure(const Image16& image, const std::vector<Patch>& patches) {
    std::vector<Rgb> out;
    for (const Patch& p : patches) {
        Rgb sum{0, 0, 0};
        for (int y = p.y; y < p.y + p.h; y++)
            for (int x = p.x; x < p.x + p.w; x++) {
                const uint16_t* px = image.pixel(x, y);
                for (int c = 0; c < 3; c++) sum[size_t(c)] += px[c] + (32768 - px[3]);
            }
        for (double& v : sum) v = v / (p.w * p.h) * 65535.0 / 32768.0;
        out.push_back(sum);
    }
    return out;
}

// ---- CIEDE2000 over sRGB (D65) ---------------------------------------------------------------------------------------

Rgb labOf(const Rgb& encoded16) {
    double linear[3];
    for (int c = 0; c < 3; c++) {
        const double v = std::clamp(encoded16[size_t(c)] / 65535.0, 0.0, 1.0);
        linear[c] = v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4);
    }
    const double X = 0.4124564 * linear[0] + 0.3575761 * linear[1] + 0.1804375 * linear[2];
    const double Y = 0.2126729 * linear[0] + 0.7151522 * linear[1] + 0.0721750 * linear[2];
    const double Z = 0.0193339 * linear[0] + 0.1191920 * linear[1] + 0.9503041 * linear[2];
    auto f = [](double t) { return t > 216.0 / 24389 ? std::cbrt(t) : (24389.0 / 27 * t + 16) / 116; };
    const double fx = f(X / 0.95047), fy = f(Y), fz = f(Z / 1.08883);
    return {116 * fy - 16, 500 * (fx - fy), 200 * (fy - fz)};
}

double deltaE00(const Rgb& a, const Rgb& b) {
    const Rgb l1 = labOf(a), l2 = labOf(b);
    const double pi = 3.14159265358979323846;
    const double c1 = std::hypot(l1[1], l1[2]), c2 = std::hypot(l2[1], l2[2]), cBar = (c1 + c2) / 2;
    const double g = 0.5 * (1 - std::sqrt(std::pow(cBar, 7) / (std::pow(cBar, 7) + std::pow(25.0, 7))));
    const double a1 = (1 + g) * l1[1], a2 = (1 + g) * l2[1];
    const double cp1 = std::hypot(a1, l1[2]), cp2 = std::hypot(a2, l2[2]);
    auto hue = [&](double bb, double aa) { double h = std::atan2(bb, aa) * 180 / pi; return h < 0 ? h + 360 : h; };
    const double h1 = cp1 == 0 ? 0 : hue(l1[2], a1), h2 = cp2 == 0 ? 0 : hue(l2[2], a2);
    const double dL = l2[0] - l1[0], dC = cp2 - cp1;
    double dh = 0;
    if (cp1 * cp2 != 0) { dh = h2 - h1; if (dh > 180) dh -= 360; else if (dh < -180) dh += 360; }
    const double dH = 2 * std::sqrt(cp1 * cp2) * std::sin(dh * pi / 360);
    const double lBar = (l1[0] + l2[0]) / 2, cpBar = (cp1 + cp2) / 2;
    double hBar = h1 + h2;
    if (cp1 * cp2 != 0) {
        if (std::fabs(h1 - h2) > 180) hBar += h1 + h2 < 360 ? 360 : -360;
        hBar /= 2;
    }
    const double t = 1 - 0.17 * std::cos((hBar - 30) * pi / 180) + 0.24 * std::cos(2 * hBar * pi / 180) + 0.32 * std::cos((3 * hBar + 6) * pi / 180)
                   - 0.20 * std::cos((4 * hBar - 63) * pi / 180);
    const double sl = 1 + 0.015 * (lBar - 50) * (lBar - 50) / std::sqrt(20 + (lBar - 50) * (lBar - 50));
    const double sc = 1 + 0.045 * cpBar, sh = 1 + 0.015 * cpBar * t;
    const double dTheta = 30 * std::exp(-((hBar - 275) / 25) * ((hBar - 275) / 25));
    const double rc = 2 * std::sqrt(std::pow(cpBar, 7) / (std::pow(cpBar, 7) + std::pow(25.0, 7)));
    const double rt = -std::sin(2 * dTheta * pi / 180) * rc;
    return std::sqrt((dL / sl) * (dL / sl) + (dC / sc) * (dC / sc) + (dH / sh) * (dH / sh) + rt * (dC / sc) * (dH / sh));
}

// ---- Camera Raw XMP attributes onto CameraRawSettings -----------------------------------------------------------------

/// The Develop control an XMP attribute belongs to, for the per-control summary; empty for one this engine lacks.
std::string controlOf(const std::string& key) {
    static const std::map<std::string, std::string> exact{
        {"Exposure2012", "Exposure"}, {"Contrast2012", "Contrast"}, {"Highlights2012", "Highlights"}, {"Shadows2012", "Shadows"},
        {"Whites2012", "Whites"}, {"Blacks2012", "Blacks"}, {"Texture", "Texture"}, {"Clarity2012", "Clarity"}, {"Dehaze", "Dehaze"},
        {"Vibrance", "Vibrance"}, {"Saturation", "Saturation"}, {"WhiteBalance", "White balance"}, {"Temperature", "White balance"},
        {"Tint", "White balance"}, {"CurveRefineSaturation", "Point curve"}, {"LensManualDistortionAmount", "Distortion"},
        {"VignetteAmount", "Lens vignetting"}, {"VignetteMidpoint", "Lens vignetting"}, {"Sharpness", "Sharpening"},
        {"SharpenRadius", "Sharpening"}, {"LuminanceSmoothing", "Noise reduction"}, {"ColorNoiseReduction", "Noise reduction"},
        {"ShadowTint", "Calibration"}, {"RedHue", "Calibration"}, {"RedSaturation", "Calibration"}, {"GreenHue", "Calibration"},
        {"GreenSaturation", "Calibration"}, {"BlueHue", "Calibration"}, {"BlueSaturation", "Calibration"}};
    if (auto it = exact.find(key); it != exact.end()) return it->second;
    auto starts = [&](const char* prefix) { return key.rfind(prefix, 0) == 0; };
    if (starts("Parametric")) return "Parametric curve";
    if (starts("ToneCurvePV2012")) return "Point curve";
    if (starts("HueAdjustment") || starts("SaturationAdjustment") || starts("LuminanceAdjustment")) return "Color mixer";
    if (starts("SplitToning") || starts("ColorGrade")) return "Color grading";
    if (starts("PostCropVignette")) return "Vignette";
    if (starts("Perspective")) return "Transform";
    if (starts("Grain")) return "Grain";
    if (key == "ConvertToGrayscale" || starts("GrayMixer")) return "Black & White";
    return {};
}


struct MappedCase {
    CameraRawSettings settings;
    std::set<std::string> controls;
    std::vector<std::string> unsupported;
};

/// A case's attributes (with the corpus's base settings under them) as this engine's settings, through the same mapping
/// the Camera Raw dialog loads XMP presets with (camerarawxmp.h), over Camera Raw's defaults for a RAW file; what it has
/// no control for is listed in `unsupported`.
MappedCase mapCase(const json& base, const json& entry) {
    MappedCase m;
    std::map<std::string, std::string> attributes;
    for (auto it = base.begin(); it != base.end(); ++it) attributes[it.key()] = it.value().get<std::string>();
    if (entry.contains("settings"))
        for (auto it = entry["settings"].begin(); it != entry["settings"].end(); ++it) attributes[it.key()] = it.value().get<std::string>();
    std::map<std::string, std::vector<std::string>> sequences;
    if (entry.contains("curves"))
        for (auto it = entry["curves"].begin(); it != entry["curves"].end(); ++it) sequences[it.key()] = it.value().get<std::vector<std::string>>();
    CameraRawSettings defaults;
    defaults.whiteBalance = CameraRawWhiteBalance::AsShot;
    defaults.detail.sharpenRadius = 20;   // Camera Raw's default radius, 1.0 pixel
    const CameraRawXmp xmp = cameraRawFromXmpAttributes(attributes, sequences, defaults, true);
    m.settings = xmp.settings;
    m.unsupported = xmp.unsupported;
    if (entry.contains("look")) m.unsupported.insert(m.unsupported.begin(), "look profile " + entry["look"]["file"].get<std::string>());
    for (const auto& [key, text] : attributes) {
        const std::string control = controlOf(key);
        if (control.empty() || key == "ProcessVersion") continue;
        if (control == "White balance" ? text != "As Shot" : control == "Black & White" ? text == "True" : std::atof(text.c_str()) != 0)
            m.controls.insert(control);
    }
    for (const auto& item : sequences)
        if (item.first.rfind("ToneCurve", 0) == 0) m.controls.insert("Point curve");
    return m;
}

// ---- Baseline ---------------------------------------------------------------------------------------------------------

struct Score {
    double mean = 0, p95 = 0;
};

Score score(const std::vector<Rgb>& ours, const json& reference, const std::vector<Patch>& patches) {
    std::vector<double> errors;
    for (size_t i = 0; i < patches.size() && i < reference.size(); i++) {
        if (patches[i].group == "wide") continue;
        const Rgb theirs{reference[i][0].get<double>(), reference[i][1].get<double>(), reference[i][2].get<double>()};
        errors.push_back(deltaE00(ours[i], theirs));
    }
    if (errors.empty()) return {};
    Score s;
    for (double e : errors) s.mean += e;
    s.mean /= double(errors.size());
    std::sort(errors.begin(), errors.end());
    s.p95 = errors[size_t(std::ceil(0.95 * double(errors.size()))) - 1];
    return s;
}

const std::string baselinePath = CAMERA_RAW_PARITY_BASELINE;

std::map<std::string, Score> readBaseline() {
    std::map<std::string, Score> out;
    std::ifstream in(baselinePath);
    for (std::string line; std::getline(in, line);) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream fields(line);
        std::string name;
        Score s;
        if (fields >> name >> s.mean >> s.p95) out[name] = s;
    }
    return out;
}

void writeBaseline(const std::map<std::string, Score>& scores) {
    std::ofstream out(baselinePath);
    out << "# Camera Raw parity baseline: <chart>/<case> <mean ΔE00> <p95 ΔE00> against Camera Raw 18.7, from tests/camera_raw_parity.cpp.\n"
           "# A case fails when it is worse by more than 0.1 (mean) or 0.3 (p95). Regenerate after an intended change:\n"
           "# NEKOPHOTO_BLESS_CAMERA_RAW_PARITY=1 build/tests/camera_raw_parity\n";
    char line[256];
    for (const auto& [name, s] : scores) {
        std::snprintf(line, sizeof line, "%s %.3f %.3f\n", name.c_str(), s.mean, s.p95);
        out << line;
    }
}

struct Run {
    std::map<std::string, Score> scores;
    std::map<std::string, std::set<std::string>> controls;
    std::map<std::string, std::vector<std::string>> skipped;
    json dump = json::object();
};

/// Develops the chart for every case the engine can render and scores it. The decode is shared between the cases with
/// the same white balance; developRaw is that decode followed by applyCameraRaw.
Run runCorpus() {
    Run run;
    const std::vector<Patch> patches = readLayout();
    const json cases = readJson(corpus + "/cases.json");
    const json reference = readJson(corpus + "/camera-raw-" + chartName + ".json");
    const std::vector<uint8_t> bytes = readRawFileBytes(corpus + "/" + chartName + ".dng", nullptr);
    if (patches.empty() || !cases.is_object() || !reference.is_object() || bytes.empty()) {
        check::fail(__FILE__, __LINE__, "the corpus in " + corpus + " could not be read");
        return run;
    }
    RawWhiteBalance balance;
    if (!readRawWhiteBalance(bytes, balance, nullptr)) { check::fail(__FILE__, __LINE__, "the chart's white balance"); return run; }
    std::map<std::string, std::shared_ptr<Image16>> decodes;
    for (const json& entry : cases["cases"]) {
        const std::string name = entry["name"].get<std::string>();
        const std::string key = chartName + "/" + name;
        if (const char* only = std::getenv("NEKOPHOTO_CAMERA_RAW_PARITY_ONLY"); only && name.find(only) == std::string::npos) continue;
        MappedCase m = mapCase(cases["base"], entry);
        if (!m.unsupported.empty()) { run.skipped[key] = m.unsupported; continue; }
        // For fitting by hand: NEKOPHOTO_CAMERA_RAW_PARITY_TRY="<case>=<settings JSON>" patches that case's settings.
        if (const char* trial = std::getenv("NEKOPHOTO_CAMERA_RAW_PARITY_TRY")) {
            const std::string text = trial;
            const size_t eq = text.find('=');
            if (eq != std::string::npos && text.substr(0, eq) == name && !CameraRawSettings::parse(text.substr(eq + 1), m.settings))
                check::fail(__FILE__, __LINE__, key + ": the trial settings do not parse");
        }
        // Every case runs on synthetic-d65; the renders keep those the engine can map (a newly mapped control needs its
        // renders added from the corpus).
        if (!reference["cases"].contains(name)) { check::fail(__FILE__, __LINE__, key + ": no Camera Raw render"); continue; }
        const CameraRawSettings settings = m.settings.normalized();
        RawDecodeOptions options;
        options.multipliers = balance.multipliersFor(settings);
        std::string decodeKey = "as shot";
        if (options.multipliers) {
            char text[96];
            std::snprintf(text, sizeof text, "%.9g %.9g %.9g", (*options.multipliers)[0], (*options.multipliers)[1], (*options.multipliers)[2]);
            decodeKey = text;
        }
        auto& decoded = decodes[decodeKey];
        if (!decoded) {
            std::string error;
            decoded = decodeRaw16(bytes, options, &error);
            if (!decoded) { check::fail(__FILE__, __LINE__, key + ": " + error); continue; }
        }
        Image16 image = *decoded;
        if (!settings.isIdentity() && !applyCameraRaw(image, settings, 1.0, 0)) { check::fail(__FILE__, __LINE__, key + ": settings refused"); continue; }
        const std::vector<Rgb> ours = measure(image, patches);
        run.scores[key] = score(ours, reference["cases"][name], patches);
        run.controls[key] = m.controls.empty() ? std::set<std::string>{"Default"} : m.controls;
        json values = json::array();
        for (const Rgb& v : ours) values.push_back({std::lround(v[0]), std::lround(v[1]), std::lround(v[2])});
        run.dump[name] = values;
    }
    return run;
}

TEST_CASE(develop_is_the_shared_decode_then_the_grade) {
    if (!rawSupported()) return;
    const std::vector<uint8_t> bytes = readRawFileBytes(corpus + "/" + chartName + ".dng", nullptr);
    REQUIRE(!bytes.empty());
    CameraRawSettings settings;
    settings.exposure = 0.5;
    settings.contrast = 30;
    auto developed = developRaw(bytes, settings, {}, nullptr);
    auto decoded = decodeRaw16(bytes, {}, nullptr);
    REQUIRE(developed && decoded);
    REQUIRE(applyCameraRaw(*decoded, settings));
    CHECK(*developed == *decoded);
}

TEST_CASE(camera_raw_parity_does_not_regress) {
    if (!rawSupported()) { std::fprintf(stderr, "  no LibRaw in this build: skipped\n"); return; }
    Run run = runCorpus();
    if (run.scores.empty()) return;
    if (const char* dump = std::getenv("NEKOPHOTO_CAMERA_RAW_PARITY_DUMP")) {
        std::ofstream out(dump);
        out << run.dump.dump() << '\n';
    }

    // Per case, then per control.
    const std::map<std::string, Score> baseline = readBaseline();
    std::map<std::string, std::vector<std::pair<std::string, Score>>> byControl;
    for (const auto& [name, s] : run.scores) {
        const auto& controls = run.controls[name];
        std::string control = controls.size() == 1 ? *controls.begin() : "Combined";
        byControl[control].push_back({name, s});
        auto was = baseline.find(name);
        if (was == baseline.end()) std::fprintf(stderr, "  %-58s mean %6.3f  p95 %6.3f  (new)\n", name.c_str(), s.mean, s.p95);
        else std::fprintf(stderr, "  %-58s mean %6.3f  p95 %6.3f  (baseline %6.3f %6.3f)\n", name.c_str(), s.mean, s.p95, was->second.mean, was->second.p95);
    }
    std::fprintf(stderr, "  Per control (cases, mean of case means, worst case mean):\n");
    for (const auto& [control, list] : byControl) {
        double sum = 0, worst = 0;
        std::string worstName;
        for (const auto& [name, s] : list) {
            sum += s.mean;
            if (s.mean > worst) { worst = s.mean; worstName = name; }
        }
        std::fprintf(stderr, "    %-18s %3zu  %6.3f  %6.3f %s\n", control.c_str(), list.size(), sum / double(list.size()), worst, worstName.c_str());
    }
    std::fprintf(stderr, "  Skipped (%zu cases, controls this engine lacks):\n", run.skipped.size());
    for (const auto& [name, keys] : run.skipped) {
        std::string list;
        for (const auto& k : keys) list += (list.empty() ? "" : ", ") + k;
        if (list.size() > 90) list = list.substr(0, 87) + "...";
        std::fprintf(stderr, "    %s: %s\n", name.c_str(), list.c_str());
    }

    if (std::getenv("NEKOPHOTO_CAMERA_RAW_PARITY_ONLY")) return;   // a subset: nothing to hold to the baseline
    if (std::getenv("NEKOPHOTO_BLESS_CAMERA_RAW_PARITY")) {
        writeBaseline(run.scores);
        std::fprintf(stderr, "  baseline rewritten: %s\n", baselinePath.c_str());
        return;
    }
    int worse = 0, missing = 0;
    for (const auto& [name, s] : run.scores) {
        auto was = baseline.find(name);
        if (was == baseline.end()) { missing++; continue; }
        if (s.mean > was->second.mean + 0.1 || s.p95 > was->second.p95 + 0.3) {
            std::fprintf(stderr, "  WORSE %s: mean %.3f (baseline %.3f), p95 %.3f (baseline %.3f)\n", name.c_str(), s.mean, was->second.mean, s.p95,
                         was->second.p95);
            worse++;
        }
    }
    CHECK_EQ(worse, 0);
    CHECK_EQ(missing, 0);   // a case with no baseline line: bless once to record it
}

} // namespace

TEST_MAIN()
