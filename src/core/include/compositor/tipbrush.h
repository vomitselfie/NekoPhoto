// Tip brushes: the brushes of Photoshop, Procreate, Clip Studio, Krita and GIMP, where a stroke is an image
// (the tip) stamped along the path. Spacing, angle, roundness, jitter, scatter, pressure and a grain
// texture are the parameters those applications share; importers map their own settings onto these. A
// TipStroke stamps into a BrushStroke's coverage, so the stroke's colour, opacity, selection, erasing, mask
// painting, preview and undo are the round brush's, unchanged.
//
// On disk a tip brush is a folder: brush.json (the settings), tip.png (8-bit grey, white paints), and
// optionally grain.png and preview.png.
#pragma once
#include "brush.h"
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
    double angleJitter = 0;      // degrees either way, 0..180
    double roundness = 1;        // the tip's height relative to its width, 0.01..1
    double sizeJitter = 0;       // how far a dab may randomly shrink, 0..1
    double scatter = 0;          // random offset as a fraction of the size, 0..10
    bool scatterBothAxes = false;// scatter along the stroke as well as across it
    int count = 1;               // dabs per spacing step, 1..16
    double flow = 1;             // each dab's opacity, 0..1
    double flowJitter = 0;       // how far a dab's flow may randomly drop, 0..1
    double pressureSize = 0;     // 0: pressure leaves the size alone; 1: pressure scales it fully
    double minimumSize = 0;      // the size at zero pressure, as a fraction, when pressure drives it
    double pressureFlow = 0;     // the same for flow
    bool flipX = false, flipY = false;
    bool randomFlipX = false, randomFlipY = false;
    double grainScale = 1;       // grain pixels per document pixel
    double grainDepth = 1;       // how strongly the grain modulates the dab, 0..1

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

struct TipInput {
    Point document;
    double pressure = 1;   // 0..1; a mouse is 1 for tip brushes, as in Photoshop
};

class TipStroke {
public:
    /// Stamps `tip` into `grid`'s coverage at `diameter` document pixels (the size before pressure and
    /// jitter). `seed` makes the jitter repeatable. `grid` must outlive this object.
    TipStroke(BrushStroke& grid, BrushTip tip, double diameter, uint32_t seed = 1);
    bool isValid() const { return valid_; }
    void strokeTo(const TipInput& input);

private:
    struct Level { GrayImage image; double scale; };   // the tip, halved, and its size relative to the original
    double sizeAt(double pressure) const;
    void dab(Point center, double pressure, double direction, Rect& changed);
    const Level& levelFor(double tipPixelsPerGridPixel) const;

    BrushStroke& grid_;
    BrushTip tip_;
    std::vector<Level> levels_;
    double diameter_ = 30;
    std::mt19937 rng_;
    std::optional<TipInput> last_;
    double carried_ = 0;   // distance walked since the last dab
    bool valid_ = false;
};

/// A preview stroke of `preset` (an S curve in black on transparent), for pickers.
std::shared_ptr<Image> renderTipPreview(const TipPreset& preset, int width, int height);

} // namespace compositor
