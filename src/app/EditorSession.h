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
#include <QColor>
#include <QObject>
#include <QRectF>
#include <QString>
#include <memory>
#include <optional>
#include <set>

namespace app {

enum class Tool { Move, Marquee, Lasso, Wand, Crop, Brush, SpotHealing, CloneStamp, Eyedropper, Hand, Zoom };
enum class MarqueeKind { Rectangle, Ellipse };

struct TransformEdit {
    compositor::Uuid layerId;
    compositor::LayerTransform draft;
    bool persistent = false;
    bool mask = false;
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
    void addBlankLayer();
    void addGroup();
    void groupSelectedLayers();
    void deleteLayer(const compositor::Uuid& id);
    void deleteSelectedLayers();
    void duplicateActiveLayer();
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

    // Selection
    void applySelectionShape(const compositor::GrayImage& shape, compositor::SelectionMode mode, const QString& name);
    void selectAll();
    void deselect();
    void invertSelection();
    void setSelection(const std::optional<compositor::Selection>& selection, const QString& name);
    void magicWand(QPointF documentPoint, int tolerance, bool contiguous, bool sampleAllLayers, compositor::SelectionMode mode);
    void fillSelection(const QColor& color);
    void clearSelectionPixels();
    void selectionExpand(int amount);
    void selectionContract(int amount);
    bool selectionAntialiased = true;
    MarqueeKind marqueeKind = MarqueeKind::Rectangle;
    int wandTolerance = 32;
    bool wandContiguous = true;
    bool wandSampleAll = true;

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

    // Crop / canvas
    void cropTo(const QRectF& rect);
    void resizeCanvas(int width, int height, double anchorX, double anchorY);
    void resizeImage(int width, int height, double resolution);

    // History
    bool canUndo() const;
    bool canRedo() const;
    QString undoName() const { return QString::fromStdString(history_.undoName()); }
    QString redoName() const { return QString::fromStdString(history_.redoName()); }
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

private:
    void restore(const compositor::DocumentHistory::Snapshot& snapshot);
    void setActiveLayer(const std::optional<compositor::Uuid>& id);
    void finishDeleting(const std::vector<compositor::Uuid>& ids);
    void notifyDocument(QRectF region = {});
    compositor::Layer* activeLayerMutable();
    static void adoptClipping(const compositor::Uuid& id, std::vector<compositor::Layer>& layers);
    static void releaseDetachedClipping(std::vector<compositor::Layer>& layers);
    void commitMaskTransform(const TransformEdit& edit);

    std::optional<compositor::Document> document_;
    compositor::DocumentHistory history_;
    std::optional<compositor::Uuid> activeLayerId_;
    std::set<compositor::Uuid> selectedLayerIds_;
    bool isMaskSelected_ = false;
    QString projectPath_;
    Tool tool_ = Tool::Move;
    std::optional<TransformEdit> transformEdit_;
    std::unique_ptr<compositor::BrushStroke> stroke_;
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
    bool adjustmentEditing_ = false;
    std::shared_ptr<const compositor::Image> previewImage_;
    std::optional<compositor::LayerTransform> previewTransform_;
};

} // namespace app
