// A brush stroke on a layer's pixels or its mask. A port of the algorithm in
// Document/BrushStroke.swift: dabs along a centripetal Catmull-Rom curve
// through the pointer samples, per-stroke coverage that accumulates (max for
// hard tips, screen for soft), and each pixel recomposed as
//   original + colour x coverage x opacity
// so overlapping dabs never exceed the stroke's opacity.
#pragma once
#include "brushsample.h"
#include "document.h"
#include "shape.h"
#include <algorithm>
#include <functional>
#include <memory>
#include <type_traits>
#include <utility>
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

/// A document-sized image made a tile at a time, on demand: the processed layer the Blur, Sharpen, Dodge, Burn
/// and Sponge tools paint from, so a stroke's start costs a tile rather than the whole canvas. `compute` fills
/// `out` (sized to the rectangle) with the image's pixels over document pixels [x, x + w) x [y, y + h); it must give
/// each pixel the value the whole image would have (a filter reads a margin around the rectangle to do so).
/// `Img` is `Image` (TiledSource) or `Image16` (TiledSource16, a 16-bit document's).
template <class Img>
class TiledSourceOf {
public:
    using Sample = std::remove_cv_t<std::remove_pointer_t<decltype(std::declval<const Img&>().data())>>;
    using Compute = std::function<void(int x, int y, int w, int h, Img& out)>;
    TiledSourceOf(int width, int height, Compute compute, int tile = 256)
        : width_(std::max(0, width)), height_(std::max(0, height)), tile_(std::max(16, tile)), compute_(std::move(compute)) {
        columns_ = (width_ + tile_ - 1) / tile_;
        rows_ = (height_ + tile_ - 1) / tile_;
        tiles_.resize(size_t(columns_) * size_t(rows_));
    }
    int width() const { return width_; }
    int height() const { return height_; }
    /// Makes every tile meeting the document rectangle [x0, x1) x [y0, y1) (not thread-safe; call before reading).
    void ensure(int x0, int y0, int x1, int y1) {
        const int tx0 = std::max(0, x0 / tile_), ty0 = std::max(0, y0 / tile_);
        const int tx1 = std::min(columns_ - 1, (std::max(x0, x1) - 1) / tile_), ty1 = std::min(rows_ - 1, (std::max(y0, y1) - 1) / tile_);
        if (x1 <= 0 || y1 <= 0 || x0 >= width_ || y0 >= height_) return;
        for (int ty = ty0; ty <= ty1; ty++)
            for (int tx = tx0; tx <= tx1; tx++) {
                auto& slot = tiles_[size_t(ty) * size_t(columns_) + size_t(tx)];
                if (slot) continue;
                const int x = tx * tile_, y = ty * tile_, w = std::min(tile_, width_ - x), h = std::min(tile_, height_ - y);
                auto image = std::make_unique<Img>(w, h);
                compute_(x, y, w, h, *image);
                slot = std::move(image);
            }
    }
    /// A pixel of an ensured tile; null outside the image or in a tile not made yet.
    const Sample* pixel(int x, int y) const {
        if (x < 0 || y < 0 || x >= width_ || y >= height_) return nullptr;
        const Img* t = tiles_[size_t(y / tile_) * size_t(columns_) + size_t(x / tile_)].get();
        return t ? t->pixel(x % tile_, y % tile_) : nullptr;
    }
    /// How many tiles have been made (for tests and benches).
    int madeTiles() const {
        int n = 0;
        for (auto& t : tiles_) n += t ? 1 : 0;
        return n;
    }

private:
    int width_, height_, tile_, columns_, rows_;
    Compute compute_;
    std::vector<std::unique_ptr<Img>> tiles_;
};
using TiledSource = TiledSourceOf<Image>;
using TiledSource16 = TiledSourceOf<Image16>;

/// `document` flattened at 1:1 and then `process`ed, as a TiledSource: each tile is rendered and processed with
/// `margin` document pixels around it (a filter's reach), so it matches processing the whole flattened image.
std::shared_ptr<TiledSource> tiledProcessedDocument(Document document, std::function<void(Image&)> process, int margin);
/// The same for a 16-bit document, rendered and processed at 16 bits.
std::shared_ptr<TiledSource16> tiledProcessedDocument16(Document document, std::function<void(Image16&)> process, int margin);

/// Clone Stamp: a document-sized image to copy from, and the offset from each painted point to its source.
/// `tiled` instead of `image`: the same, made on demand. A stroke on a 16-bit document reads `image16` or `tiled16`.
struct CloneSource {
    std::shared_ptr<const Image> image;
    Point offset;
    std::shared_ptr<TiledSource> tiled;
    std::shared_ptr<const Image16> image16;
    std::shared_ptr<TiledSource16> tiled16;
};

/// Soft-brush falloff across the band between the hardness radius and the rim.
double brushFalloff(double u);

template <SampleType S> class StrokeRaster;

class BrushStroke {
public:
    /// Begins a stroke on `layer` (its pixels, or its mask when `mask`). The working grid is the
    /// layer's pixel grid grown to cover `canvas` so paint can go past the layer's edges.
    BrushStroke(const Layer& layer, bool mask, BrushSettings settings, Size canvas, const GrayImage* selection = nullptr);
    /// The same at a document's depth: `depth` U16 makes a 16-bit stroke (its pixels or mask, coverage and selection at
    /// 0..32768; the 16-bit accessors below), taking the 16-bit `selection`. U8 is the constructor above.
    BrushStroke(const Layer& layer, bool mask, BrushSettings settings, Size canvas, SampleType depth, const Gray16* selection);
    ~BrushStroke();
    BrushStroke(const BrushStroke&) = delete;
    BrushStroke& operator=(const BrushStroke&) = delete;
    /// The stroke's depth: its working pixels, mask, coverage and selection are all at it.
    SampleType sampleType() const { return depth_; }
    /// Clone Stamp: the sample painted through the tip instead of the colour. With `replaces`, the sample
    /// replaces what is under the tip rather than drawing over it (so it can clear pixels too).
    void setClone(CloneSource clone, bool replaces = false) { clone_ = std::move(clone); replacesWithClone_ = replaces; }
    /// Painting a mask from a document-sized gray sample (the Blur tool on a mask) instead of a flat value.
    void setMaskClone(std::shared_ptr<const GrayImage> sample);
    void setMaskClone(std::shared_ptr<const Gray16> sample);

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
    /// The round tip takes a brush sample's position; its size and opacity are the settings', whatever the pen does.
    void append(const BrushSample& sample) { append(sample.position); }
    /// Every point of a finished path at once: one recompose at the end instead of one per point.
    void appendAll(const std::vector<Point>& documentPoints);
    /// Replaces the provisional tail with the final curve piece. Safe to repeat.
    void flush();

    /// The layer's pixels (or mask) as the stroke leaves them, for the canvas while painting.
    ImagePtr previewImage() const;
    GrayPtr previewMask() const;
    /// The same for a 16-bit stroke (null for an 8-bit one, whose 8-bit previews are null in turn).
    Image16Ptr previewImage16() const;
    Gray16Ptr previewMask16() const;
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
    const Image* gridBase() const;
    Image* gridWorking();
    const GrayImage* gridSelection() const;
    const Image16* gridBase16() const;
    Image16* gridWorking16();
    const Gray16* gridSelection16() const;
    const Affine& documentToGrid() const { return documentToPixel_; }
    void markPainted(const Rect& gridRect) { painted_ = true; markDirty(gridRect); }
    // Or an engine that stamps its own dabs (imported tip brushes) into this stroke's coverage: the colour,
    // opacity, selection, erasing and masks then apply exactly as for the round tip.
    GrayImage* gridCoverage();
    Gray16* gridCoverage16();
    const Affine& gridToDocument() const { return pixelToDocument_; }
    const Rect& canvasRect() const { return canvas_; }
    void recomposeCovered(const Rect& gridRect) { markDirty(gridRect); recompose(gridRect); }

private:
    // The pixels, coverage and selection live in a StrokeRaster at the stroke's depth (stroke_raster.h); this class
    // keeps the grid's geometry, the curve through the samples, the provisional tail and the dirty area.
    template <SampleType> friend class StrokeRaster;
    template <class F> decltype(auto) withRaster(F&& f);
    template <class F> decltype(auto) withRaster(F&& f) const;
    void walk(Point to);
    void dab(Point center);
    void curve(Point from, Point to, Point before, Point after);
    void recompose(const Rect& gridRect);
    void markDirty(const Rect& gridRect);
    void heal();
    void restoreTail();

    bool valid_ = false;
    std::string error_;
    bool isMask_;
    BrushSettings settings_;
    Rect canvas_;
    int width_ = 0, height_ = 0;
    Affine pixelToDocument_, documentToPixel_;
    LayerTransform paintTransform_;
    Rect sourceRect_;                 // where the original pixels sit in the grid
    PixelBounds baseBounds_;          // where the original pixels have any alpha
    Rect touchedGrid_;                // every grid pixel a dab or fill may have changed
    std::vector<Point> samples_;
    std::optional<Point> previous_;
    double distanceToNext_ = 0;
    Rect dirtyGrid_;
    bool deferRecompose_ = false;
    bool touched_ = false;
    bool painted_ = false;   // another engine wrote the working pixels: never recompose them from the coverage
    // A provisional straight tail is drawn to the newest sample and undone when the next arrives.
    bool hasTail_ = false;
    Rect tailRect_;
    std::optional<Point> tailPrevious_;
    double tailDistance_ = 0;
    std::string name_;
    LayerTransform layerTransform_;
    std::optional<CloneSource> clone_;
    bool replacesWithClone_ = false;
    Rect liftedRect_;

    SampleType depth_ = SampleType::U8;
    // Exactly one is set: the raster at depth_.
    std::unique_ptr<StrokeRaster<SampleType::U8>> raster8_;
    std::unique_ptr<StrokeRaster<SampleType::U16>> raster16_;
};

} // namespace compositor
