// Adjustment layers and the colour adjustments that share their math: Levels,
// Curves, Hue/Saturation, Exposure, Gradient Map and Grain. Settings are the
// Mac's Codable structures; the JSON encoding is the manifest's.
#pragma once
#include "document.h"
#include <array>
#include <map>
#include <string>
#include <vector>

namespace compositor {

// Channel indices: 0 RGB, 1 Red, 2 Green, 3 Blue.
const char* levelsChannelName(int channel);
bool parseLevelsChannel(const std::string& name, int& out);

struct LevelsRange {
    double black = 0, gamma = 1, white = 255, outputBlack = 0, outputWhite = 255;
    bool operator==(const LevelsRange&) const = default;
    LevelsRange normalized() const;
    /// Output (0..1) for an input (0..1).
    double apply(double value) const;
};

struct LevelsSettings {
    int channel = 0;
    std::array<LevelsRange, 4> ranges;
    bool operator==(const LevelsSettings&) const = default;
    bool isIdentity() const;
    /// The channel's own range followed by the composite RGB range.
    double apply(double value, int channel) const { return ranges[0].apply(ranges[size_t(channel)].apply(value)); }
};

struct CurvePoint { double x = 0, y = 0; bool operator==(const CurvePoint&) const = default; };

struct CurvesSettings {
    int channel = 0;
    std::array<std::vector<CurvePoint>, 4> channels{{{{0, 0}, {255, 255}}, {{0, 0}, {255, 255}}, {{0, 0}, {255, 255}}, {{0, 0}, {255, 255}}}};
    bool operator==(const CurvesSettings&) const = default;
    bool isValid() const;
    bool isIdentity() const;
    /// Shape-preserving cubic Hermite interpolation; `x` and the result are 0..255.
    double value(double x, int channel) const;
};

struct ExposureSettings {
    double exposure = 0, offset = 0, gamma = 1;
    bool operator==(const ExposureSettings&) const = default;
    ExposureSettings normalized() const;
    bool isIdentity() const { return exposure == 0 && offset == 0 && gamma == 1; }
    /// Output (0..1) per input byte.
    std::array<float, 256> table() const;
};

struct AdjustmentColor {
    double red = 0, green = 0, blue = 0;
    bool operator==(const AdjustmentColor&) const = default;
    AdjustmentColor clamped() const;
};

struct GradientMapSettings {
    AdjustmentColor shadows{0, 0, 0};
    AdjustmentColor highlights{1, 1, 1};
    bool reversed = false;
    bool operator==(const GradientMapSettings&) const = default;
    /// 256 x 3 straight sRGB bytes, darkest first.
    std::vector<uint8_t> table() const;
};

struct GrainSettings {
    double amount = 25, size = 1.5, roughness = 50;
    uint32_t seed = 0;
    bool operator==(const GrainSettings&) const = default;
    GrainSettings normalized() const;
};

// Hue/Saturation. Ranges: 0 Master, 1 Reds, 2 Yellows, 3 Greens, 4 Cyans, 5 Blues, 6 Magentas.
const char* colorRangeName(int range);
bool parseColorRange(const std::string& name, int& out);

struct HueBand {
    double falloffStart = 0, rangeStart = 0, rangeEnd = 360, falloffEnd = 360;
    bool operator==(const HueBand&) const = default;
    static HueBand defaultBand(int range);
    static double forward(double from, double to);
    /// 1 inside the range, ramping through the shoulders, 0 outside.
    double weight(double hue) const;
    HueBand centered(double hue) const;
    void include(double hue);
    void exclude(double hue);
    void setHandle(int index, double degrees);
private:
    void normalize();
};

struct RangeAdjustment { double hue = 0, saturation = 0, lightness = 0; bool operator==(const RangeAdjustment&) const = default; };

struct HueSaturationSettings {
    int range = 0;
    bool colorize = false;
    bool invertRange = false;
    std::map<int, RangeAdjustment> adjustments;
    std::map<int, HueBand> bands;
    bool operator==(const HueSaturationSettings&) const = default;
    HueSaturationSettings();
    static HueSaturationSettings colorizeStart();
    bool isIdentity() const;
    RangeAdjustment& current() { return adjustments[range]; }
    RangeAdjustment currentValue() const { auto it = adjustments.find(range); return it == adjustments.end() ? RangeAdjustment{} : it->second; }
    HueBand band() const { auto it = bands.find(range); return it == bands.end() ? HueBand::defaultBand(range) : it->second; }
    double weight(int colorRange, double hue) const;
    /// One straight colour (0..1) adjusted.
    void adjust(double& r, double& g, double& b) const;
    /// The hue a spectrum swatch becomes.
    double shiftedHue(double hue) const;
};

/// Every adjustment layer setting, decoded from the manifest's "adjustment" object.
struct AdjustmentSettings {
    AdjustmentKind kind = AdjustmentKind::Levels;
    LevelsSettings levels;
    CurvesSettings curves;
    HueSaturationSettings hsv;
    ExposureSettings exposure;
    GradientMapSettings gradientMap;
    GrainSettings grain;
    /// Unknown fields, kept for the round trip.
    std::string extraJson;
    bool operator==(const AdjustmentSettings&) const = default;

    static AdjustmentSettings defaults(AdjustmentKind kind);
    /// Parses the manifest object; unknown kinds or malformed settings fail.
    static bool parse(const std::string& json, AdjustmentSettings& out);
    std::string toJson() const;
    LayerAdjustment toLayerAdjustment() const { return {kind, toJson()}; }
    bool isValid() const;
    bool isIdentity() const;
};

// Applying to premultiplied RGBA in place.
void applyLevels(Image& image, const LevelsSettings& settings);
void applyCurves(Image& image, const CurvesSettings& settings);
void applyExposure(Image& image, const ExposureSettings& settings);
void applyGradientMap(Image& image, const GradientMapSettings& settings);
/// `origin` and `unitsPerPixel` place the image's pixels in document space, so the pattern stays put.
void applyGrain(Image& image, const GrainSettings& settings, Point origin = {}, double unitsPerPixel = 1);
void applyHueSaturation(Image& image, const HueSaturationSettings& settings);
void applyInvert(Image& image);
void applyInvert(GrayImage& mask);

/// Applies `settings` to `image` (the document area `region` at `scale`, for Grain).
bool applyAdjustment(const AdjustmentSettings& settings, Image& image, const Rect& region, double scale);
bool applyAdjustment(const LayerAdjustment& adjustment, Image& image, const Rect& region, double scale);

/// The default settings JSON for a new adjustment layer of `kind`.
std::string defaultAdjustmentJson(AdjustmentKind kind);

/// Levels helpers: the four histograms (RGB mean, R, G, B) of an image, optionally weighted by coverage.
std::array<std::vector<double>, 4> levelsHistogram(const Image& image, const GrayImage* coverage);
enum class LevelsAuto { Contrast, Color, Neutral };
LevelsSettings autoLevels(LevelsAuto mode, const std::array<std::vector<double>, 4>& histogram);
enum class LevelsSample { Black, Gray, White };
LevelsSettings sampleLevels(const LevelsSettings& settings, double r, double g, double b, LevelsSample mode);

} // namespace compositor
