// Tip brushes: the brushes of Photoshop, Procreate, Clip Studio, Krita and GIMP, where a stroke is an image
// (the tip) stamped along the path. Spacing, angle, roundness, scatter, a grain
// texture and dynamics (brushdynamics.h: size, flow, angle and the rest following pressure, tilt, speed, chance...)
// are the parameters those applications share; importers map their own settings onto these. A
// TipStroke stamps into a BrushStroke's coverage, so the stroke's colour, opacity, selection, erasing, mask
// painting, preview and undo are the round brush's, unchanged.
//
// On disk a tip brush is a folder: brush.json (the settings), tip.png (8-bit grey, white paints), and
// optionally grain.png and preview.png.
#pragma once
#include "brush.h"
#include "brushdynamics.h"
#include "brushsample.h"
#include <array>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <vector>

namespace compositor {

struct BrushTip {
    std::shared_ptr<const GrayImage> shape;   // 255 paints fully
    std::shared_ptr<const GrayImage> grain;   // optional texture, tiled across the document
    double spacing = 0.25;       // distance between dabs as a fraction of the dab's size, 0.01..10
    double angle = 0;            // degrees, counterclockwise
    bool followStroke = false;   // the stroke's direction is added to the angle
    double roundness = 1;        // the tip's height relative to its width, 0.01..1
    double scatter = 0;          // random offset as a fraction of the size, 0..10
    bool scatterBothAxes = false;// scatter along the stroke as well as across it
    int count = 1;               // dabs per spacing step, 1..16
    double flow = 1;             // each dab's opacity, 0..1
    bool flipX = false, flipY = false;
    bool randomFlipX = false, randomFlipY = false;
    double grainScale = 1;       // grain pixels per document pixel
    double grainDepth = 1;       // how strongly the grain modulates the dab, 0..1
    /// How size, flow, opacity, angle, roundness, spacing, scatter and the grain follow the pen and chance
    /// (brushdynamics.h). Pressure on size, the jitters and the rest are all mappings here.
    BrushDynamics dynamics;
    /// Spacing leaves the stroke's density alone: each dab's alpha a is scaled to 1 - (1 - a)^(s / r) at spacing s,
    /// so a stroke painted at `densityReference` spacing looks the same at any other. Off, closer dabs build up more.
    bool densityBySpacing = false;
    double densityReference = 0.25;
    /// A mouse's speed stands in for pressure (slow presses harder, fast lifts), with a short ramp in at the start.
    /// Simulated, and only for a mouse: a stylus always gives its own pressure. Off, a mouse is full pressure.
    bool mousePressureFromSpeed = false;

    /// Clamped to the documented ranges; false when there is no usable shape.
    bool normalize();
};

/// A tip brush as stored: its name, its default size in pixels and the tip.
struct TipPreset {
    std::string name;
    double diameter = 30;
    BrushTip tip;
};

/// Reads a preset folder; nullopt with `error` when brush.json or tip.png is missing or unreadable.
std::optional<TipPreset> loadTipPreset(const std::string& folder, std::string* error = nullptr);
/// Writes brush.json, tip.png and grain.png (when there is one) into `folder`, creating it.
bool saveTipPreset(const std::string& folder, const TipPreset& preset, std::string* error = nullptr);

class TipStroke {
public:
    /// Stamps `tip` into `grid`'s coverage at `diameter` document pixels (the size before pressure and
    /// jitter). `seed` makes the jitter repeatable. `grid` must outlive this object.
    TipStroke(BrushStroke& grid, BrushTip tip, double diameter, uint32_t seed = 1);
    bool isValid() const { return valid_; }
    /// The next sample of the stroke (derived by a BrushSampleTrack). A mouse's pressure counts as full, as in
    /// Photoshop (or follows its speed, with mousePressureFromSpeed): tip brushes read pressure only from a stylus.
    void strokeTo(const BrushSample& input);

private:
    struct Level { GrayImage image; double scale; };   // the tip, halved, and its size relative to the original
    /// The dab size at `sample` before its random part: what the spacing is measured in.
    double steadySize(const BrushSample& sample) const;
    void dab(Point center, const BrushSample& sample, double direction, double spacing, Rect& changed);
    const Level& levelFor(double tipPixelsPerGridPixel) const;

    BrushStroke& grid_;
    BrushTip tip_;
    std::vector<Level> levels_;
    double diameter_ = 30;
    std::mt19937 rng_;
    std::optional<BrushSample> last_;
    double carried_ = 0;   // distance walked since the last dab
    std::array<bool, dynamicsTargetCount> randomOn_{};   // targets a Random mapping drives
    bool valid_ = false;
    /// Density by spacing on a 16-bit grid: an entry per 15-bit level, for the spacing ratio `density16K_`.
    std::vector<uint16_t> density16_;
    double density16K_ = -1;
};

/// A preview stroke of `preset` (an S curve in black on transparent), for pickers.
std::shared_ptr<Image> renderTipPreview(const TipPreset& preset, int width, int height);

} // namespace compositor
