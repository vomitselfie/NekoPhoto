// A brush stroke on a layer's pixels or its mask. A port of the algorithm in
// Document/BrushStroke.swift: dabs along a centripetal Catmull-Rom curve
// through the pointer samples, per-stroke coverage that accumulates (max for
// hard tips, screen for soft), and each pixel recomposed as
//   original + colour x coverage x opacity
// so overlapping dabs never exceed the stroke's opacity.
#pragma once
#include "document.h"
#include "shape.h"
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
    /// Spot Healing: the stroke shows a dark wash while painting and rebuilds the area from its
    /// surroundings when it ends (mode 0 Content-Aware, 1 Create Texture, 2 Proximity Match).
    bool healing = false;
    int healingMode = 0;
    uint32_t healingSeed = 0;
    /// Dabs on a grid aligned with the document come from a precomputed tile at a quarter-pixel phase;
    /// off, every dab is computed per pixel (the reference the stamps are held to in tests).
    bool stampedDabs = true;
};

/// Clone Stamp: a document-sized image to copy from, and the offset from each painted point to its source.
struct CloneSource {
    std::shared_ptr<const Image> image;
    Point offset;
};

/// Soft-brush falloff across the band between the hardness radius and the rim.
double brushFalloff(double u);

class BrushStroke {
public:
    /// Begins a stroke on `layer` (its pixels, or its mask when `mask`). The working grid is the
    /// layer's pixel grid grown to cover `canvas` so paint can go past the layer's edges.
    BrushStroke(const Layer& layer, bool mask, BrushSettings settings, Size canvas, const GrayImage* selection = nullptr);
    /// Clone Stamp: the sample painted through the tip instead of the colour. With `replaces`, the sample
    /// replaces what is under the tip rather than drawing over it (so it can clear pixels too).
    void setClone(CloneSource clone, bool replaces = false) { clone_ = std::move(clone); replacesWithClone_ = replaces; }
    /// Painting a mask from a document-sized gray sample (the Blur tool on a mask) instead of a flat value.
    void setMaskClone(std::shared_ptr<const GrayImage> sample) { maskClone_ = std::move(sample); }

    // Moving selected pixels (the Move tool with a selection).
    /// Cuts the selected pixels out of the original image. False when nothing is lifted.
    bool liftSelection();
    /// Rebuilds the working image: the selection becomes a transparent hole (unless duplicating) and the lifted
    /// pixels are placed `offset` document pixels away.
    void moveLifted(Point offset, bool duplicate);

    /// Replaces this edit with a gradient over the whole canvas (or the selection) on the original pixels.
    void fillGradientOver(int shape, Point from, Point to, const float startColor[4], const float endColor[4], double opacity);
    /// The same with any stops (a multi-stop gradient preset).
    void fillGradientOver(int shape, Point from, Point to, const GradientStops& stops, double opacity);
    /// Fills the selection (or the whole canvas) with a colour (straight 0..1; the red channel on masks).
    void fillColor(double red, double green, double blue);
    bool isValid() const { return valid_; }
    const std::string& error() const { return error_; }

    void append(Point documentPoint);
    /// Every point of a finished path at once: one recompose at the end instead of one per point.
    void appendAll(const std::vector<Point>& documentPoints);
    /// Replaces the provisional tail with the final curve piece. Safe to repeat.
    void flush();

    /// The layer's pixels (or mask) as the stroke leaves them, for the canvas while painting.
    ImagePtr previewImage() const { return working_; }
    GrayPtr previewMask() const { return workingMask_; }
    /// Where the working grid sits on the document.
    const LayerTransform& paintTransform() const { return paintTransform_; }
    /// The document area changed since the last call, then reset.
    Rect takeDirtyRect();
    /// Brings the renderer's reduced copies of the working pixels up to date over `grid` (takeDirtyRect does).
    void refreshLevels(const Rect& grid) const;
    bool touched() const { return touched_; }

    struct Commit {
        std::optional<Asset> asset;       // painted pixels, cropped to their alpha bounds
        std::optional<MaskAsset> mask;    // painted mask (the whole grid)
        LayerTransform transform;         // the layer's new transform
        std::optional<LayerTransform> maskPlacement;
    };
    /// Finishes the stroke: the new raster cropped to its pixels, and the transform placing it.
    Commit commit();
    /// A healing stroke's result so far, in the preview image (what commit will do with the spot as painted).
    void previewHeal();

    // Another paint engine on this stroke's grid (the MyPaint presets): it reads the original pixels, writes
    // the working ones and reports what it changed; the preview, the selection and commit() stay this class's.
    const Image* gridBase() const { return base_.get(); }
    Image* gridWorking() { return working_.get(); }
    const GrayImage* gridSelection() const { return selection_.get(); }
    const Affine& documentToGrid() const { return documentToPixel_; }
    void markPainted(const Rect& gridRect) { painted_ = true; markDirty(gridRect); }
    // Or an engine that stamps its own dabs (imported tip brushes) into this stroke's coverage: the colour,
    // opacity, selection, erasing and masks then apply exactly as for the round tip.
    GrayImage* gridCoverage() { return coverage_.get(); }
    const Affine& gridToDocument() const { return pixelToDocument_; }
    const Rect& canvasRect() const { return canvas_; }
    void recomposeCovered(const Rect& gridRect) { markDirty(gridRect); recompose(gridRect); }

private:
    void walk(Point to);
    void dab(Point center);
    void curve(Point from, Point to, Point before, Point after);
    void recompose(const Rect& gridRect);
    void recomposeRows(const Rect& gridRect);   // recompose's work over whole rows of the grid
    void markDirty(const Rect& gridRect);
    void heal();
    void healFromClone(const PixelBounds& bounds);

    bool valid_ = false;
    std::string error_;
    bool isMask_;
    BrushSettings settings_;
    Rect canvas_;
    int width_ = 0, height_ = 0;
    Affine pixelToDocument_, documentToPixel_;
    LayerTransform paintTransform_;
    Rect sourceRect_;                 // where the original pixels sit in the grid
    std::shared_ptr<const Image> base_;   // original pixels in the grid (image strokes); the layer's own image when the grid matches it
    PixelBounds baseBounds_;               // where base_ has any alpha
    Rect touchedGrid_;                     // every grid pixel a dab or fill may have changed
    std::shared_ptr<Image> working_;
    std::shared_ptr<GrayImage> baseMask_;
    std::shared_ptr<GrayImage> workingMask_;
    std::shared_ptr<GrayImage> visible_;   // the layer mask's visible pixels on the grid, for the healers; null without an enabled mask on the layer's own grid
    std::shared_ptr<GrayImage> coverage_;
    std::shared_ptr<GrayImage> selection_; // selection coverage in grid pixels, if any
    std::vector<Point> samples_;
    std::optional<Point> previous_;
    double distanceToNext_ = 0;
    Rect dirtyGrid_;
    bool deferRecompose_ = false;
    /// Dab profile over squared distance (0..255), rebuilt when the tip changes; the dab loop reads it instead
    /// of computing a square root and a falloff per pixel.
    std::vector<uint8_t> dabTable_;
    double dabTableRadius_ = -1, dabTableHardness_ = -1, dabTableFootprint_ = -1, dabTableScale_ = 0;
    /// On a grid aligned with the document, a dab is a precomputed tile at one of 4x4 subpixel phases (2x2 for
    /// soft tips over 512 pixels, whose rim hides a quarter pixel), merged row by row (Krita's dab cache). Each phase is built the first time a dab needs it; all are dropped when the
    /// tip or the grid scale changes.
    struct Stamp {
        int side = 0, steps = 4;   // steps: subpixel positions per axis
        double radius = -1, hardness = -1, scale = 0;
        std::vector<uint8_t> tiles[16];
        bool built[16] = {};
    };
    Stamp stamp_;
    bool stampDab(Point center, double radius, const Rect& affected);
    void refreshDabTable(double radius, double hardness, double footprint);
    bool touched_ = false;
    bool painted_ = false;   // another engine wrote the working pixels: never recompose them from coverage_
    // A provisional straight tail is drawn to the newest sample and undone when the next arrives.
    bool hasTail_ = false;
    Rect tailRect_;
    std::vector<uint8_t> tailBackup_;
    std::optional<Point> tailPrevious_;
    double tailDistance_ = 0;
    std::string name_;
    LayerTransform layerTransform_;
    std::optional<CloneSource> clone_;
    bool replacesWithClone_ = false;
    std::shared_ptr<const GrayImage> maskClone_;
    std::shared_ptr<Image> lifted_;
    Rect liftedRect_;
};

} // namespace compositor
