// The editor's state and every edit it can make: the document, its history,
// the active layer, the tools and their settings. A port of the parts of
// Document/EditorSession.swift (and its extensions) that the Linux UI needs.
// Views observe it through signals and never mutate the document themselves.
#pragma once
#include "Viewport.h"
#include "compositor/adjustments.h"
#include "compositor/brush.h"
#include "compositor/filters.h"
#include "compositor/document.h"
#include "compositor/history.h"
#include "compositor/render.h"
#include "compositor/selection.h"
#include "compositor/shape.h"
#include "compositor/warp.h"
#include "compositor/warpstroke.h"
#include <QElapsedTimer>
#include <functional>
#include <map>
#include <QColor>
#include <QObject>
#include <QTimer>
#include <QRectF>
#include <QString>
#include <memory>
#include <optional>
#include <set>

namespace app {

enum class Tool { Move, Marquee, Lasso, Wand, Crop, Brush, SpotHealing, CloneStamp, Smudge, Gradient, Shape, Eyedropper, Hand, Zoom, Text };
enum class MarqueeKind { Rectangle, Ellipse };
enum class LassoKind { Freehand, Polygonal };
enum class BlurToolMode { Liquify, Blur, Smudge };
enum class GradientStyle { ForegroundToBackground, ForegroundToTransparent };

/// Several layers transformed together: the upright box around them when the edit began, and each one's transform then.
struct TransformGroup {
    compositor::LayerTransform box;
    std::map<compositor::Uuid, compositor::LayerTransform> originals;
};

/// Ctrl+T with a selection: the selected pixels float on a temporary layer and merge back on Apply.
struct FloatingTransform {
    compositor::Uuid sourceId;
    compositor::Document before;
    std::optional<compositor::Uuid> beforeActive;
    compositor::LayerTransform original;
    int pixelWidth = 0, pixelHeight = 0;
};

struct TransformEdit {
    compositor::Uuid layerId;
    compositor::LayerTransform draft;
    bool persistent = false;
    bool mask = false;
    /// Set once a handle is Ctrl-dragged: the four corners move freely and Apply resamples the pixels.
    std::optional<compositor::Corners> corners;
    std::optional<TransformGroup> group;
    std::optional<FloatingTransform> floating;
};

struct GradientSettings {
    compositor::GradientShape shape = compositor::GradientShape::Linear;
    GradientStyle style = GradientStyle::ForegroundToTransparent;
    bool reversed = false;
    double opacity = 1;
};

/// A shape being dragged out with the Shape tool, in whole document pixels.
struct ShapeDraft {
    compositor::ShapeKind kind;
    QPointF anchor;
    QRectF rect;
    double cornerRadius = 0;
};

class EditorSession : public QObject {
    Q_OBJECT
public:
    explicit EditorSession(QObject* parent = nullptr);

    // Document
    const std::optional<compositor::Document>& document() const { return document_; }
    bool hasDocument() const { return document_.has_value(); }
    QString projectPath() const { return projectPath_; }
    QString title() const;
    bool isModified() const { return history_.isModified(); }
    void createDocument(int width, int height, double resolution = 72, bool emptyLayer = true);
    bool openProject(const QString& path, QString* error);
    bool saveProject(const QString& path, QString* error);
    void closeDocument();
    /// Adds imported pixels as a new layer, centred on `at` (or the canvas); a first import creates the canvas.
    void insertImage(std::shared_ptr<const compositor::Image> image, const QString& name, std::optional<QPointF> at = std::nullopt);
    std::shared_ptr<compositor::Image> flattened() const;

    // Layers
    std::optional<compositor::Uuid> activeLayerId() const { return activeLayerId_; }
    const std::set<compositor::Uuid>& selectedLayerIds() const { return selectedLayerIds_; }
    const compositor::Layer* activeLayer() const;
    bool isMaskSelected() const { return isMaskSelected_; }
    void selectLayer(const std::optional<compositor::Uuid>& id, bool mask = false);
    void selectLayers(const std::set<compositor::Uuid>& ids, const std::optional<compositor::Uuid>& primary);
    std::set<compositor::Uuid> collapsedGroupIds;
    void toggleGroupExpansion(const compositor::Uuid& id);
    /// A blank layer above the active one (inside it, for a folder), or `below` it.
    void addBlankLayer(bool below = false);
    void addGroup();
    void groupSelectedLayers();
    void deleteLayer(const compositor::Uuid& id);
    void deleteSelectedLayers();
    /// Layers that clip to something being deleted but survive it; the UI asks whether to bake or release them.
    std::vector<compositor::Uuid> clippingDependents(const std::vector<compositor::Uuid>& ids) const;
    /// Deletes `ids`; with `bake`, dependents keep their masked appearance in their pixels first.
    void deleteLayersResolvingClipping(const std::vector<compositor::Uuid>& ids, bool bake);
    void duplicateActiveLayer();
    /// Ctrl+E: one layer merges with the one beneath it; several selected merge together; a folder merges its contents.
    struct MergePlan { std::vector<compositor::Uuid> ids; std::set<compositor::Uuid> removed; std::string name; std::optional<compositor::Uuid> parent; compositor::Uuid anchor; QString action; };
    std::optional<MergePlan> mergePlan() const;
    bool canMergeLayers() const;
    QString mergeTitle() const;
    void mergeLayers();
    void moveActiveLayerOutOfGroup();
    /// Alt-drag in the Layers panel: a copy of the layer placed where it was dropped.
    bool duplicateLayerTo(const compositor::Uuid& id, const std::optional<compositor::Uuid>& parent, const std::optional<compositor::Uuid>& above, bool atBottom);
    /// Alt-dragging a mask thumbnail onto another layer: a copy of the mask, where it sits on the document.
    bool copyMask(const compositor::Uuid& source, const compositor::Uuid& target);
    void mergeDown();
    void renameLayer(const compositor::Uuid& id, const QString& name);
    void toggleLayerVisibility(const compositor::Uuid& id);
    void beginVisibilitySwipe(const compositor::Uuid& id);
    void setVisibilityInSwipe(const compositor::Uuid& id, bool visible);
    void endVisibilitySwipe();
    void moveActiveLayer(int offset);
    bool canMoveActiveLayer(int offset) const;
    bool placeLayer(const compositor::Uuid& id, const std::optional<compositor::Uuid>& parent, const std::optional<compositor::Uuid>& above, bool atBottom = false);
    void setLayerOpacity(double opacity);
    void beginOpacityEdit();
    void endOpacityEdit();
    void setLayerBlendMode(compositor::BlendMode mode);
    /// Hovering the blend menu: the active layer drawn in `mode` until the menu closes.
    void previewBlendMode(std::optional<compositor::BlendMode> mode);
    void toggleClippingMask(const compositor::Uuid& id);
    bool canToggleClippingMask(const compositor::Uuid& id) const;
    void addLayerMask(bool revealing);
    void addMaskFromSelection(bool revealing);
    void toggleLayerMask();
    void deleteLayerMask();
    void toggleMaskLink(const compositor::Uuid& id);
    void applyMask();
    void invertMask();
    void flipLayer(bool horizontal);
    void flipCanvas(bool horizontal);
    void setLayerSampling(compositor::Sampling sampling);
    bool canEditLayers() const;

    // Transform (Move tool)
    const std::optional<TransformEdit>& transformEdit() const { return transformEdit_; }
    bool canTransform() const;
    void beginTransform(bool persistent);
    void previewTransform(const compositor::LayerTransform& value);
    void commitTransform();
    void cancelTransform();
    void nudgeLayer(double dx, double dy);
    /// Ctrl+T: transforms the selected pixels when there is a selection, else the layer(s).
    void transformCommand();
    bool canTransformSelection() const;
    void beginSelectionTransform();
    /// Alt-drag: a copy of the active layer is made and moved; cancelling removes it again.
    void beginDuplicateTransform();
    /// Several selected layers, or a folder: the transform moves them together in one box.
    bool transformsAsGroup() const;
    std::vector<const compositor::Layer*> groupTransformMembers() const;
    std::optional<compositor::LayerTransform> groupTransformBox() const;
    /// Ctrl-drag on a handle: the corners start moving freely; Apply resamples the pixels.
    void beginDistort();
    void previewCorners(const compositor::Corners& corners);
    /// Where the transform box's corners are right now (the distortion's, or the draft's).
    compositor::Corners editedCorners(const compositor::Layer& layer) const;
    /// The transform a layer shows right now: the pending draft or its own.
    compositor::LayerTransform displayedTransform(const compositor::Layer& layer) const;
    compositor::LayerTransform editedTransform(const compositor::Layer& layer) const;
    bool showsTransformControls = true;
    bool locksTransformRatio = true;
    bool transformAutoSelect = false;
    /// Guides the last move snapped to, for the canvas to draw.
    std::vector<double> snapGuidesX, snapGuidesY;
    void setSnapGuides(std::vector<double> xs, std::vector<double> ys);
    /// The topmost visible layer with pixels under a document point.
    std::optional<compositor::Uuid> layerAt(QPointF documentPoint) const;
    /// Pixels the transform places (what 100% draws 1:1).
    std::optional<compositor::Size> transformPixelSize() const;

    // Brush
    compositor::BrushSettings brushSettings;
    /// Photoshop's opacity keys: 1 = 10% ... 9 = 90%, 0 = 100%; two digits typed quickly set an exact value.
    void typeOpacityDigit(int digit);
    void changeBrushHardness(bool increase);
    void changeBrushSize(bool increase);
    /// The Blur tool's modes; Liquify and Smudge push pixels, Blur paints a softened copy.
    BlurToolMode blurMode = BlurToolMode::Liquify;
    bool warpActive() const { return warp_ != nullptr; }
    bool beginWarp(QPointF documentPoint);
    void continueWarp(QPointF documentPoint);
    void endWarp();
    void cancelWarp();

    // Gradient tool
    GradientSettings gradientSettings;
    bool gradientPending() const { return gradient_ != nullptr; }
    std::optional<std::pair<QPointF, QPointF>> gradientLine() const;
    void beginGradient(QPointF documentPoint);
    void moveGradient(QPointF end);
    void endGradientDrag();
    void refreshGradient();
    void commitGradient();
    void cancelGradient();
    /// Switching tools, layers or targets applies the pending gradient, as in Photoshop.
    void resolveGradient();

    // Text tool. New text takes `textStyle` (its colour follows the foreground colour) and opens the editor.
    compositor::LayerText textStyle;
    /// Adds a text layer above the active one with its top-left near `documentPoint`; with `openEditor` the
    /// text editor is requested for it. Returns the layer's id, or none when nothing could be added.
    std::optional<compositor::Uuid> addTextLayer(QPointF documentPoint, const compositor::LayerText& text, bool openEditor);
    std::optional<compositor::LayerText> layerText(const compositor::Uuid& id) const;
    /// Text edits: a session (the dialog) applies changes as they come and keeps or drops them at the end;
    /// outside a session each setLayerText is its own undo step.
    void beginTextEdit(const compositor::Uuid& id);
    void setLayerText(const compositor::Uuid& id, const compositor::LayerText& text);
    void endTextEdit(bool keep);
    bool textEditing() const { return textEditing_; }
    void requestTextEdit(const compositor::Uuid& id) { emit textEditRequested(id); }

    // Shape tool
    compositor::ShapeKind shapeKind = compositor::ShapeKind::Rectangle;
    double shapeCornerRadius = 0;
    const std::optional<ShapeDraft>& shapeDraft() const { return shapeDraft_; }
    void beginShape(QPointF documentPoint);
    void dragShape(QPointF documentPoint, bool square, bool fromCenter);
    void finishShape();
    void cancelShape();
    void toggleShapeKind();

    // Moving selected pixels (the Move tool with a selection)
    bool pixelMoveActive() const { return pixelMove_ != nullptr; }
    bool canMovePixels(QPointF documentPoint) const;
    bool beginPixelMove(bool duplicate);
    void movePixels(QPointF offset);
    void finishPixelMove();
    void cancelPixelMove();
    void nudgePixels(double dx, double dy);
    /// The outline to draw: during a pixel move or a floating transform, the selection carried along.
    std::optional<compositor::Selection> displayedSelection() const;
    /// The composite's straight colour under a document point, or none when transparent / outside.
    std::optional<QColor> compositeColorAt(QPointF documentPoint) const;
    /// Hooks a panel can install to take the next canvas press (returns true to claim it), the drag and the release.
    std::function<bool(QPointF)> canvasPressHook;
    std::function<void(QPointF, QPointF)> canvasDragHook;
    std::function<void()> canvasReleaseHook;
    bool brushErase = false;
    QColor foregroundColor{Qt::black};
    QColor backgroundColor{Qt::white};
    bool maskPaintWhite = true;
    bool brushActive() const { return stroke_ != nullptr; }
    bool beginBrush(QPointF documentPoint, bool straightFromLast);
    void continueBrush(QPointF documentPoint);
    void endBrush();
    void cancelBrush();
    std::optional<QPointF> lastBrushPoint() const { return lastBrushPoint_; }
    int spotHealingMode = 0;
    bool cloneAligned = true;
    bool cloneSampleAll = false;
    std::optional<QPointF> cloneSource;
    std::optional<QPointF> cloneOffset;
    void setCloneSource(QPointF documentPoint) { cloneSource = documentPoint; cloneOffset.reset(); emit toolChanged(); }
    /// Where Clone Stamp would copy from for a brush at `point`, for the canvas's crosshair.
    std::optional<QPointF> cloneSamplePoint(QPointF point) const;

    // Clipboard
    bool canCopyPixels() const;
    void copySelection();
    void copyMerged();
    void cutSelection();
    bool canPaste() const;
    void paste();
    void layerViaCopy();
    /// Content-Aware Fill of the selection on the active layer; the layer grows over any selection past its edge.
    bool contentAwareFill(QString* error);
    /// Dragging a layer between projects: `id` (a folder with its contents) copied from `source` into this
    /// document, centred on `at` (or the canvas); clipping to layers left behind is baked in. A first copy
    /// into an empty tab makes the canvas the source's size.
    bool copyLayerFrom(const EditorSession& source, const compositor::Uuid& id, std::optional<QPointF> at, QString* error);

    // Selection
    void applySelectionShape(const compositor::GrayImage& shape, compositor::SelectionMode mode, const QString& name);
    void selectAll();
    void deselect();
    void invertSelection();
    void setSelection(const std::optional<compositor::Selection>& selection, const QString& name);
    /// `sampleRadius` 0, 1 or 2: the point, a 3x3 or a 5x5 average sets the colour to match (Photoshop's Sample Size).
    void magicWand(QPointF documentPoint, int tolerance, bool contiguous, bool sampleAllLayers, compositor::SelectionMode mode, int sampleRadius = 0);
    void fillSelection(const QColor& color);
    void clearSelectionPixels();
    void selectionExpand(int amount);
    void selectionContract(int amount);
    /// Photoshop's Select > Modify: Feather (a Gaussian of that radius), Smooth (disc majority), Border (a band).
    void selectionFeather(double radius);
    void selectionSmooth(int radius);
    void selectionBorder(int width);
    /// Arrow keys with a selection tool: the outline moves by whole pixels, one undo step per press.
    void nudgeSelection(double dx, double dy);
    /// Load a layer's pixels (or its mask) as the selection.
    void loadLayerAsSelection(const compositor::Uuid& id, bool mask, compositor::SelectionMode mode);
    LassoKind lassoKind = LassoKind::Freehand;
    bool selectionAntialiased = true;
    MarqueeKind marqueeKind = MarqueeKind::Rectangle;
    int wandTolerance = 32;
    bool wandContiguous = true;
    bool wandSampleAll = false;
    int wandSampleRadius = 0;

    // Adjustment layers
    void addAdjustmentLayer(compositor::AdjustmentKind kind);
    /// Live edits of an adjustment layer's settings; wrap a slider drag in begin/end for one undo step.
    void beginAdjustmentEdit();
    void setAdjustment(const compositor::Uuid& id, const compositor::AdjustmentSettings& settings);
    void endAdjustmentEdit();
    std::optional<compositor::AdjustmentSettings> adjustmentSettings(const compositor::Uuid& id) const;

    // Destructive adjustments and filters on the active layer's pixels, inside the selection.
    bool canAdjustPixels() const;
    /// Shows `image` (placed by `transform`, or the layer's own) in place of the active layer while a dialog is open.
    void setPixelPreview(std::shared_ptr<const compositor::Image> image, std::optional<compositor::LayerTransform> transform);
    void clearPixelPreview();
    /// The active layer's pixels (grown by `margin` layer pixels for blurs), and the transform placing them.
    std::shared_ptr<const compositor::Image> adjustmentSource(int margin, compositor::LayerTransform& transform) const;
    /// The selection as coverage on that grid, or null when everything is selected.
    std::shared_ptr<compositor::GrayImage> selectionOnGrid(const compositor::LayerTransform& transform, int width, int height) const;
    /// Replaces the active layer's pixels with `image` at `transform` as one undo step.
    void commitPixels(std::shared_ptr<const compositor::Image> image, const compositor::LayerTransform& transform, const QString& name);
    void invertActive();
    std::array<std::vector<double>, 4> activeHistogram() const;
    /// Remove Background: `mask` (white over the subject, the active layer's pixel grid) becomes the layer mask,
    /// multiplied with any mask already there; with a selection only the selected part changes.
    void applySubjectMask(std::shared_ptr<const compositor::GrayImage> mask);

    // Crop / canvas
    void cropTo(const QRectF& rect);
    void resizeCanvas(int width, int height, double anchorX, double anchorY);
    void resizeImage(int width, int height, double resolution, int sampling = 2);

    // History
    bool canUndo() const;
    bool canRedo() const;
    QString undoName() const { return QString::fromStdString(history_.undoName()); }
    QString redoName() const { return QString::fromStdString(history_.redoName()); }
    std::vector<std::string> undoNames() const { return history_.pastNames(); }
    std::vector<std::string> redoNames() const { return history_.futureNames(); }
    void undo();
    void redo();
    void beginEdit(const QString& name);
    void endEdit();

    // Tools and view
    Tool tool() const { return tool_; }
    void selectTool(Tool tool);
    Viewport viewport;
    bool showsPixelGrid = true;
    void fitView();
    void zoomTo(double zoom, std::optional<QPointF> anchor = std::nullopt);
    /// The overrides the renderer needs while an edit is in progress.
    compositor::Overrides renderOverrides() const;
    /// Bumped on every document notification; what render caches key on.
    uint64_t documentRevision() const { return documentRevision_; }

signals:
    /// The document's pixels or structure changed; `region` is the document area affected (empty means all).
    void documentChanged(QRectF region);
    void layersChanged();
    void selectionChanged();
    void toolChanged();
    void viewportChanged();
    void transformChanged();
    void historyChanged();
    void titleChanged();
    void projectPathChanged();
    void error(QString message);
    /// The text editor should open for this layer (a new one, or a text layer clicked with the Text tool).
    void textEditRequested(compositor::Uuid id);

private:
    void restore(const compositor::DocumentHistory::Snapshot& snapshot);
    void setActiveLayer(const std::optional<compositor::Uuid>& id);
    void finishDeleting(const std::vector<compositor::Uuid>& ids);
    void notifyDocument(QRectF region = {});
    compositor::Layer* activeLayerMutable();
    static void adoptClipping(const compositor::Uuid& id, std::vector<compositor::Layer>& layers);
    static void releaseDetachedClipping(std::vector<compositor::Layer>& layers);
    void commitMaskTransform(const TransformEdit& edit);
    void commitDistort(const TransformEdit& edit);
    void mergeFloatingTransform(const TransformEdit& edit);
    void cancelFloatingTransform(const FloatingTransform& floating);
    std::optional<std::pair<compositor::LayerTransform, compositor::Corners>> distortTarget(const compositor::Layer& layer, const TransformEdit& edit) const;
    std::optional<compositor::LayerTransform> displayedMaskPlacement(const compositor::Layer& layer) const;
    void redrawShape(compositor::Layer& layer);
    bool redrawText(compositor::Layer& layer);
    bool textEditing_ = false;
    std::optional<compositor::Layer> textEditOriginal_;
    void finishDeleting(const std::vector<compositor::Uuid>& ids, const std::map<compositor::Uuid, compositor::Asset>& baked);
    std::optional<compositor::Asset> bakeClipping(const compositor::Uuid& target) const;
    void clearSelectedPixelsNow(compositor::Layer& layer);
    std::unique_ptr<compositor::BrushStroke> makeRasterEdit(const compositor::Layer& layer, bool mask, const compositor::BrushSettings& settings) const;
    void commitRasterEdit(compositor::BrushStroke& stroke, const compositor::Uuid& layerId, bool mask, const QString& name);

    std::optional<compositor::Document> document_;
    compositor::DocumentHistory history_;
    std::optional<compositor::Uuid> activeLayerId_;
    std::set<compositor::Uuid> selectedLayerIds_;
    bool isMaskSelected_ = false;
    QString projectPath_;
    Tool tool_ = Tool::Move;
    std::optional<TransformEdit> transformEdit_;
    std::unique_ptr<compositor::BrushStroke> stroke_;
    QTimer healPreview_;   // a healing stroke shows its result once the pointer pauses
    compositor::Uuid strokeLayerId_;
    bool strokeMask_ = false;
    std::optional<QPointF> lastBrushPoint_;
    struct PixelClipboard { std::shared_ptr<const compositor::Image> image; QPointF origin; };
    std::optional<PixelClipboard> pixelClipboard_;
    /// The active layer's pixels (or the composite) as they sit on the canvas, inside the selection's whole-pixel bounds.
    std::optional<PixelClipboard> renderSelectedPixels(bool merged) const;
    void addPixelLayer(std::shared_ptr<const compositor::Image> image, QPointF origin, const QString& editName, bool dropsSelection);
    bool opacityEditing_ = false;
    bool visibilitySwipe_ = false;
    /// Bumped on every document notification; cheap change detection for caches.
    uint64_t documentRevision_ = 0;
    std::shared_ptr<const compositor::Image> cloneSample_;
    bool cloneSampleAll_ = false;
    compositor::Uuid cloneSampleLayer_;
    uint64_t cloneSampleRevision_ = 0;
    std::shared_ptr<const compositor::Image> wandSample_;
    bool wandSampleAll_ = false;
    compositor::Uuid wandSampleLayer_;
    uint64_t wandSampleRevision_ = 0;
    bool adjustmentEditing_ = false;
    std::shared_ptr<const compositor::Image> previewImage_;
    std::optional<compositor::LayerTransform> previewTransform_;
    std::optional<std::pair<compositor::Uuid, compositor::Uuid>> transformDuplicate_; // copy, source
    struct DistortCache { compositor::Corners corners; compositor::LayerTransform transform; compositor::ImagePtr source; compositor::GrayPtr mask; std::optional<compositor::WarpedImage> image; compositor::GrayPtr warpedMask; };
    mutable std::map<compositor::Uuid, DistortCache> distortCache_;
    struct PixelMove { std::unique_ptr<compositor::BrushStroke> raster; compositor::Selection origin; bool duplicate; QPointF offset; compositor::Uuid layerId; };
    std::unique_ptr<PixelMove> pixelMove_;
    struct GradientEdit { std::unique_ptr<compositor::BrushStroke> raster; compositor::Uuid layerId; bool mask; QPointF start, end; };
    std::unique_ptr<GradientEdit> gradient_;
    std::optional<ShapeDraft> shapeDraft_;
    std::unique_ptr<compositor::WarpStroke> warp_;
    compositor::Uuid warpLayerId_;
    compositor::LayerTransform warpTransform_;
    std::optional<std::pair<int, qint64>> pendingOpacityDigit_;
    std::optional<compositor::BlendMode> blendPreview_;
    QElapsedTimer opacityTimer_;
};

} // namespace app
