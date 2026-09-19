// A brush stroke on a layer's pixels or its mask. A port of the algorithm in
// Document/BrushStroke.swift: dabs along a centripetal Catmull-Rom curve
// through the pointer samples, per-stroke coverage that accumulates (max for
// hard tips, screen for soft), and each pixel recomposed as
//   original + colour x coverage x opacity
// so overlapping dabs never exceed the stroke's opacity.
#pragma once
#include "document.h"
#include <vector>

namespace compositor {

struct BrushSettings {
    double diameter = 40;   // document pixels, 1..2000
    double hardness = 1;    // 0..1
    double opacity = 1;     // 0.01..1
    double red = 0, green = 0, blue = 0;
    bool erasing = false;
    /// Painting a mask: the gray value painted (1 reveals, 0 hides).
    double maskValue = 1;
};

/// Soft-brush falloff across the band between the hardness radius and the rim.
double brushFalloff(double u);

class BrushStroke {
public:
    /// Begins a stroke on `layer` (its pixels, or its mask when `mask`). The working grid is the
    /// layer's pixel grid grown to cover `canvas` so paint can go past the layer's edges.
    BrushStroke(const Layer& layer, bool mask, BrushSettings settings, Size canvas, const GrayImage* selection = nullptr);
    bool isValid() const { return valid_; }
    const std::string& error() const { return error_; }

    void append(Point documentPoint);
    /// Replaces the provisional tail with the final curve piece. Safe to repeat.
    void flush();

    /// The layer's pixels (or mask) as the stroke leaves them, for the canvas while painting.
    ImagePtr previewImage() const { return working_; }
    GrayPtr previewMask() const { return workingMask_; }
    /// Where the working grid sits on the document.
    const LayerTransform& paintTransform() const { return paintTransform_; }
    /// The document area changed since the last call, then reset.
    Rect takeDirtyRect();
    bool touched() const { return touched_; }

    struct Commit {
        std::optional<Asset> asset;       // painted pixels, cropped to their alpha bounds
        std::optional<MaskAsset> mask;    // painted mask (the whole grid)
        LayerTransform transform;         // the layer's new transform
        std::optional<LayerTransform> maskPlacement;
    };
    /// Finishes the stroke: the new raster cropped to its pixels, and the transform placing it.
    Commit commit();

private:
    void walk(Point to);
    void dab(Point center);
    void curve(Point from, Point to, Point before, Point after);
    void recompose(const Rect& gridRect);
    void markDirty(const Rect& gridRect);

    bool valid_ = false;
    std::string error_;
    bool isMask_;
    BrushSettings settings_;
    Rect canvas_;
    int width_ = 0, height_ = 0;
    Affine pixelToDocument_, documentToPixel_;
    LayerTransform paintTransform_;
    Rect sourceRect_;                 // where the original pixels sit in the grid
    std::shared_ptr<Image> base_;     // original pixels in the grid (image strokes)
    std::shared_ptr<Image> working_;
    std::shared_ptr<GrayImage> baseMask_;
    std::shared_ptr<GrayImage> workingMask_;
    std::shared_ptr<GrayImage> coverage_;
    std::shared_ptr<GrayImage> selection_; // selection coverage in grid pixels, if any
    std::vector<Point> samples_;
    std::optional<Point> previous_;
    double distanceToNext_ = 0;
    Rect dirtyGrid_;
    bool touched_ = false;
    // A provisional straight tail is drawn to the newest sample and undone when the next arrives.
    bool hasTail_ = false;
    Rect tailRect_;
    std::vector<uint8_t> tailBackup_;
    std::optional<Point> tailPrevious_;
    double tailDistance_ = 0;
    std::string name_;
    LayerTransform layerTransform_;
};

} // namespace compositor
