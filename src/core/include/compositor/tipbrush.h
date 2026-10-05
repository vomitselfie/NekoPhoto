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
    /// Where the grain is fixed. Canvas: to the document, so the stroke reveals it (Photoshop's texture, Procreate's
    /// texturized grain). Stroke: to the stroke, travelling `grainMovement` of each pixel walked along a smoothly turning
    /// tangent, so it rolls with the stroke round curves (Procreate's moving grain) and a dab's jitter does not spin it.
    /// Dab: to each dab, turning and flipping with it, the same in every dab.
    enum class GrainMode { Canvas, Stroke, Dab };
    GrainMode grainMode = GrainMode::Canvas;
    double grainMovement = 1;    // Stroke: 0..1; at 0 the grain stays with the dab and only turns with the stroke
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
    /// Taper: the stroke's start and end narrow (and fade) over a length, as Procreate's and Clip Studio's tapers
    /// (Clip Studio's "starting and ending"). Lengths are in document pixels, whatever the brush's size, as both applications
    /// measure them; `size` and
    /// `opacity` are how much the dab shrinks and fades at the very tip, 0..1, falling linearly to nothing over the
    /// length. The end taper is known only once the pen lifts, so with one the stroke holds back its last `end`
    /// diameters and paints them, tapered, at finish(). `mouseTaper` is used instead for a mouse when set (Procreate's
    /// touch taper).
    struct Taper {
        double start = 0, end = 0;
        double size = 1, opacity = 0;
        bool isNone() const { return !(start > 0) && !(end > 0); }
    };
    Taper taper;
    std::optional<Taper> mouseTaper;

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

/// One stamped dab as the stroke resolved it, for tests and benches (TipStroke::trace).
struct TipDab {
    Point center;           // on the path
    Point at;               // after scatter
    double time = 0;        // the pen's time there, seconds
    double distance = 0;    // document pixels along the stroke
    double size = 0;        // document pixels
    double flow = 0, opacity = 0, roundness = 0;
    double spacing = 0;     // the step to this dab over its size
    double rotation = 0;    // radians in the document's y-down frame (the tip's angle and the stroke's direction)
    double grainTangent = 0;// Stroke grain: the smoothed tangent the grain turns with, radians, unwrapped
    Point grainOffset;      // Stroke grain: where the dab's centre lies in the grain's frame (document pixels)
    double grainDirection = 0;  // Stroke grain: the path's direction the tangent follows, radians, unwrapped
};

class TipStroke {
public:
    /// Stamps `tip` into `grid`'s coverage at `diameter` document pixels (the size before pressure and
    /// jitter). `seed` makes the jitter repeatable. `grid` must outlive this object.
    TipStroke(BrushStroke& grid, BrushTip tip, double diameter, uint32_t seed = 1);
    bool isValid() const { return valid_; }
    /// The next sample of the stroke (derived by a BrushSampleTrack). A mouse's pressure counts as full, as in
    /// Photoshop (or follows its speed, with mousePressureFromSpeed): tip brushes read pressure only from a stylus.
    /// With Stroke grain, a tip that follows the stroke, or Roll on a pen that does not report its twist, the first dab
    /// waits for the stroke's direction (the next sample that moves), so it starts turned the way the stroke goes.
    void strokeTo(const BrushSample& input);
    /// The end of the stroke: a first dab still waiting (a click that never moved) is stamped.
    void finish();
    /// Every dab stamped from now on is appended to `out` (null stops). Costs nothing while off.
    void trace(std::vector<TipDab>* out) { trace_ = out; }
    /// Dabs stamped so far.
    size_t dabCount() const { return dabCount_; }

private:
    struct Level { GrayImage image; double scale; int peak = 255; };   // the tip, halved, its size relative to the original, its brightest pixel
    /// The dab size at `sample` before its random part: what the spacing is measured in.
    double steadySize(const BrushSample& sample) const;
    /// Places the dabs of one spacing step (resolving their dynamics, in the random draws' order); drawPending draws them.
    void dab(Point center, const BrushSample& sample, double direction, double spacing, Rect& changed);
    /// A placed dab: where it lands in the grid and everything its pixels are drawn with.
    struct Stamp {
        Point center, at, grainOffset;
        double c = 1, s = 0, sx = 1, sy = 1, lx = 1, ly = 1, flow = 1;
        double tc = 1, ts = 0, gc = 1, gs = 0, grainTurn = 0, grainStrength = 0;
        const Level* level = nullptr;
        const uint16_t* density = nullptr;   // density by spacing, indexed by the dab's value; null without
        const double* grainFactor = nullptr; // what each grain level multiplies the value by; null: computed per pixel
        unsigned ceiling = 0;
        unsigned most = 0;   // the largest value (after density) any pixel of the dab can add
        int x0 = 0, x1 = 0, y0 = 0, y1 = 0;
        bool flipX = false, flipY = false;
    };
    template <typename Coverage> void stampRows(const Stamp& stamp, Coverage& coverage, int y0, int y1) const;
    /// Draws the placed dabs in order into the coverage, bands of rows on every core.
    void drawPending();
    const Level& levelFor(double tipPixelsPerGridPixel) const;

    BrushStroke& grid_;
    BrushTip tip_;
    std::vector<Level> levels_;
    double diameter_ = 30;
    std::mt19937 rng_;
    std::optional<BrushSample> last_;
    double carried_ = 0;   // distance walked since the last dab
    /// Stroke grain: the path's direction unwrapped from dab to dab; the tangent the grain turns with, that direction
    /// smoothed over about two diameters of travel; the distance and the centre of the last dab; and the grain's offset,
    /// the steps between dabs (`grainMovement` of each) turned into the grain's frame as it was then, so the grain under
    /// the paper only turns, and never slides, while the frame catches up with a bend.
    double grainDirection_ = 0, grainTangent_ = 0, grainTangentAt_ = 0;
    Point grainCenter_, grainOffset_;
    bool grainTangentSet_ = false;
    bool firstPending_ = false;   // the first sample is in, its dab waits for the direction
    bool rollOn_ = false;         // a mapping reads Roll, which is the stroke's direction on a pen without twist
    std::array<bool, dynamicsTargetCount> randomOn_{};   // targets a Random mapping drives
    bool valid_ = false;
    /// Density by spacing: an entry per level of the grid's depth, for the spacing ratio `densityK_`; tables replaced
    /// while placed dabs still point at them wait in `retiredDensity_` until those are drawn.
    std::shared_ptr<std::vector<uint16_t>> densityTable_;
    std::vector<std::shared_ptr<std::vector<uint16_t>>> retiredDensity_;
    double densityK_ = -1;
    /// The grain's factor per level for grain depth `grainFactorDepth_`, kept the same way.
    std::shared_ptr<std::array<double, 256>> grainFactor_;
    std::vector<std::shared_ptr<std::array<double, 256>>> retiredGrain_;
    double grainFactorDepth_ = -1;
    std::vector<Stamp> pending_;   // placed, not yet drawn
    std::vector<TipDab>* trace_ = nullptr;
    size_t dabCount_ = 0;
    /// Inputs only some brushes read, set on each dab's sample: the stroke's own random draw (from a generator of its
    /// own, so the dabs' draws stay as they were) and the direction it set off in.
    double strokeRandom_ = 0, initialDirection_ = 0;
    bool initialDirectionSet_ = false, waitForDirection_ = false;
    /// Taper: the one in use for this stroke (pen or mouse, chosen at its first sample), the samples held back for the
    /// end taper, and the stroke's length once it is known (at finish; negative while painting).
    BrushTip::Taper activeTaper_;
    std::vector<BrushSample> held_;
    double strokeLength_ = -1;
    /// What tapering leaves of the size and of the opacity at `distance` along the stroke.
    std::pair<double, double> taperAt(double distance) const;
    /// Walks the path from last_ to `input`, stamping dabs (strokeTo's body).
    void walkTo(const BrushSample& input, Rect& changed);
    BrushSample withStrokeInputs(BrushSample s) const;
};

/// A preview stroke of `preset` (an S curve in black on transparent), for pickers.
std::shared_ptr<Image> renderTipPreview(const TipPreset& preset, int width, int height);

} // namespace compositor
