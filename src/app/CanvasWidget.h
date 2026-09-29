// The canvas: renders the document through the viewport, draws the overlays
// (transform box, selection ants, brush cursor, guides, pixel grid) and turns
// pointer input into tool actions on the session.
#pragma once
#include "EditorSession.h"
#include "TextLayer.h"
#include <QImage>
#include <QPointF>
#include <QRect>
#include <QTimer>
#include <QWidget>
#include <optional>
#include <vector>

namespace app {

class CanvasWidget : public QWidget {
    Q_OBJECT
public:
    explicit CanvasWidget(EditorSession* session, QWidget* parent = nullptr);
    QSize sizeHint() const override { return {1100, 750}; }
    /// The pending crop rectangle (Crop tool), in document pixels.
    std::optional<QRectF> cropRect() const { return crop_; }
    void applyCrop();
    void cancelCrop();
    /// The ratio the crop keeps, as the options bar's W and H (0 and 0 is free). A pending crop box is fitted to
    /// it at once, centred on itself, as Photoshop does when a preset is picked.
    void setCropRatio(double width, double height);
    double cropRatioWidth() const { return cropRatioW_; }
    double cropRatioHeight() const { return cropRatioH_; }
    /// Photoshop's X with the Crop tool: portrait becomes landscape (the ratio and the pending box).
    void swapCropOrientation();
    /// The largest box of `ratio` (width / height) inside `within`, centred on it, in whole pixels.
    static QRectF fitCropRatio(const QRectF& within, double ratio);
    void finishPolygonalLasso();
    void cancelLasso();
    QPointF documentPoint(QPointF viewPoint) const;
    QPointF viewPoint(QPointF documentPoint) const;
    EditorSession* session() const { return session_; }

    /// Typing on the canvas with the Type tool (CanvasWidgetText.cpp).
    bool typeEditing() const { return typeEdit_.has_value(); }
    void commitType();   // Ctrl+Enter, Enter on the keypad, a click outside, another tool or tab
    void cancelType();   // Esc: the text as it was (new type goes)
    /// The options bar's change to the selected letters, or to the letters typed next when none are selected;
    /// `alignment` for the paragraphs.
    void applyTypeStyle(const compositor::TextRunPatch& patch, std::optional<int> alignment = std::nullopt);
    static compositor::TextRunPatch typePatch(const compositor::LayerText& before, const compositor::LayerText& after);
    /// The style at the caret (the letter before it) or the selection's start, for the options bar.
    std::optional<compositor::TextRun> typeStyleAtCaret() const;
    std::optional<int> typeAlignment() const;
    /// How long the last change took to reach the layer's pixels (layout and raster), in milliseconds.
    double lastTypeLatencyMs() const { return lastTypeMs_; }
    /// For tests and automation-free checks: start typing on a layer, or new type at a document point.
    bool startTypeOn(const compositor::Uuid& id, std::optional<QPointF> documentPoint);
    bool startNewType(QPointF documentPoint, std::optional<QRectF> box);
    QPointF viewPointForTest(QPointF documentPoint) const { return viewPoint(documentPoint); }

signals:
    void cursorMoved(QPointF documentPoint);
    void cropChanged();
    void cropRatioChanged();
    /// Typing started or ended, or the caret or selection moved.
    void typeEditChanged();

protected:
    void paintEvent(QPaintEvent*) override;
    void resizeEvent(QResizeEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent*) override;
    void wheelEvent(QWheelEvent*) override;
    void keyPressEvent(QKeyEvent*) override;
    void keyReleaseEvent(QKeyEvent*) override;
    void tabletEvent(QTabletEvent*) override;
    void leaveEvent(QEvent*) override;
    bool event(QEvent*) override;
    void focusOutEvent(QFocusEvent*) override;
    void inputMethodEvent(QInputMethodEvent*) override;
    QVariant inputMethodQuery(Qt::InputMethodQuery query) const override;

private:
    static constexpr double handleRadius = 5;   // points
    static constexpr double rotateReach = 22;   // points beyond a corner that still rotates
    static constexpr double dragThreshold = 3;  // points before a press becomes a drag
    static constexpr double snapDistance = 10;  // points
    /// The area the brush outline (and Clone Stamp's sample marker) covers with the pointer at `at`.
    QRect brushCursorRect(QPointF at) const;
    /// Keeps the cache through a pan by whole device pixels, rendering only what came into view; false when
    /// the view must be rendered afresh.
    bool scrollCache(QRect visible, QPointF origin, double zoom);
    bool boxPainted_ = false;   // whether the last paint drew a transform box
    enum class Drag { None, Pan, Move, Resize, Rotate, Distort, PixelMove, Brush, Warp, Gradient, Shape, Marquee, Lasso, Scribble, ClickBox, SelectionMove, Patch, Pen, PathEdit, WarpCage, Crop, CropMove, CropResize, ZoomRect, Hook, Box, Type };
    struct HandleHit { bool hit = false; int index = 0; bool rotate = false; };

    /// Notes a changed part of the document for the next paint, which renders all of it at once (flushDirty).
    void invalidate(QRectF documentRegion);
    void flushDirty();
    /// Where `documentRegion` falls in the cache, in device pixels, clipped to it.
    QRect cachePart(QRectF documentRegion) const;
    void ensureCache();
    void renderInto(QImage& target, QRect deviceRect, QPointF documentOrigin, double zoom);
    QSizeF documentSize() const;
    QRectF documentViewRect() const;
    void syncViewport();
    HandleHit hitHandle(QPointF viewPoint, const compositor::Corners& corners, bool insideBox) const;
    bool boxShown() const;
    void updateCursor(QPointF viewPoint, Qt::KeyboardModifiers modifiers);
    void press(QPointF viewPoint, Qt::MouseButton button, Qt::KeyboardModifiers modifiers);
    void move(QPointF viewPoint, Qt::MouseButtons buttons, Qt::KeyboardModifiers modifiers);
    void release(QPointF viewPoint, Qt::MouseButton button, Qt::KeyboardModifiers modifiers);
    void finishMarquee(Qt::KeyboardModifiers modifiers);
    void finishFreehandLasso();
    void sampleColor(QPointF documentPoint, bool background);
    compositor::SelectionMode selectionMode(Qt::KeyboardModifiers modifiers) const;
    void drawOverlays(QPainter& painter);
    void drawTransformBox(QPainter& painter, const compositor::Corners& corners, bool active, bool distorting);
    void drawSelectionAnts(QPainter& painter);
    void drawScribbles(QPainter& painter);
    void drawCropOverlay(QPainter& painter);
    QRectF dragBox(QPointF anchor, QPointF point, bool square, bool fromCenter, double ratio = 0) const;

    void refreshSelectionOutline();
    void guideTargets(std::vector<double>& xs, std::vector<double>& ys) const;
    void snapMove(compositor::LayerTransform& draft);
    QPointF snapPoint(QPointF documentPoint);
    bool isBrushLike() const;

    EditorSession* session_;
    QImage cache_;
    compositor::RenderCache renderCache_;   // the layers around the one being edited, kept between frames
    QRect cacheDeviceRect_;
    QPointF cacheDocumentOrigin_;
    double cacheZoom_ = 0;
    bool cacheValid_ = false;
    QRectF pendingDirty_;   // document area changed since the last paint, not yet rendered into the cache
    /// Running from each wheel or pinch zoom step until the gesture pauses; meanwhile the view shows the cache
    /// scaled to the new zoom (`zoomPreview_`) rather than rendering every step afresh.
    QTimer zoomSettle_;
    bool zoomPreview_ = false;
    void zoomGesture(double factor, QPointF viewPoint);
    Drag drag_ = Drag::None;
    QPointF dragStartView_, dragStartDocument_, lastView_;
    std::optional<compositor::TransformDrag> transformDrag_;
    compositor::Corners distortStart_{};
    int distortIndex_ = 0;
    bool dragMoved_ = false;
    bool spaceHeld_ = false;
    std::optional<QPointF> hover_;
    /// Direct Selection: what a view point is over on the target path, the knot chosen, and a drag in progress.
    struct PathHit { int sub = -1, knot = -1; enum Part { None, Anchor, In, Out, Subpath } part = None; };
    PathHit pathHit(QPointF view) const;
    std::optional<std::pair<int, int>> selectedKnot_;
    int cageIndex_ = -1;
    void drawWarpCage(QPainter& painter);
    std::optional<compositor::VectorPath> pathDragStart_;
    PathHit pathDrag_;
    void drawPathOverlay(QPainter& painter);
    std::vector<QPointF> lassoPoints_;
    bool contentMoveDrag_ = false;   // the Patch drag is Content-Aware Move's
    std::vector<QPointF> scribblePoints_;
    bool scribbleBackground_ = false;
    QPointF clickStart_, clickCurrent_;
    bool clickBackground_ = false;
    std::optional<QPointF> lassoCursor_;
    std::optional<QRectF> marquee_;
    std::optional<compositor::Selection> selectionMoveOrigin_;
    std::optional<QRectF> crop_;
    QRectF cropOrigin_;
    int cropHandle_ = -1;
    double cropRatio_ = 0;   // width / height, 0 free
    double cropRatioW_ = 0, cropRatioH_ = 0;
    std::optional<QRectF> zoomRect_;
    std::vector<QPolygonF> selectionOutline_;
    /// Set when the outline is too detailed to trace or draw as vectors: the ants come from a raster pass instead.
    bool selectionRasterAnts_ = false;
    void drawRasterAnts(QPainter& painter, const compositor::GrayImage& coverage);
    int antsPhase_ = 0;
    QTimer antsTimer_;
    bool layerPickedOnPress_ = false;
    QCursor zoomInCursor_, zoomOutCursor_;
    /// The Artboard and Slice tools (CanvasWidgetBoxes.cpp): a rectangle drawn, moved or resized by a corner or edge,
    /// committed on release as one undo step.
    struct BoxDrag {
        enum class Kind { Draw, Move, Resize } kind = Kind::Draw;
        std::optional<compositor::Uuid> artboard;
        std::optional<uint32_t> slice;
        QRectF origin;
        int edges = 0;          // Resize: 1 left, 2 top, 4 right, 8 bottom
    };
    std::optional<BoxDrag> boxDrag_;
    std::optional<QRectF> boxDraft_;
    void pressBox(QPointF view, QPointF documentPoint, Qt::KeyboardModifiers modifiers);
    void moveBox(QPointF documentPoint, Qt::KeyboardModifiers modifiers);
    void releaseBox();
    void drawBoxes(QPainter& painter);

    // Typing on the canvas (CanvasWidgetText.cpp): the caret and selection in UTF-16 units of the text, the layout
    // they are drawn from, and the raster-to-document map of the layer.
    struct TypeState {
        compositor::Uuid layer;
        int caret = 0, anchor = 0;
        double preferredX = -1;                            // Up and Down keep to this x on the raster
        std::optional<compositor::TextRunPatch> pending;   // the style for the letters typed next
        struct Snapshot { compositor::LayerText text; int caret = 0, anchor = 0; };
        std::vector<Snapshot> undo, redo;                  // Ctrl+Z inside the session
        enum class Drag { None, Select, Box } drag = Drag::None;
        int boxEdges = 0;
        compositor::LayerText boxStart;
        QPointF boxStartRaster, boxShift;
        compositor::Affine boxStartMap;
        bool boxRecorded = false;
        bool caretOn = true;
        TextCaretGeometry geometry;
        compositor::Affine toDocument;
        QSizeF rasterSize;
        QString preedit;
    };
    std::optional<TypeState> typeEdit_;
    std::optional<QPointF> typeCreateStart_;
    std::optional<QRectF> typeCreateRect_;
    QTimer caretBlink_;
    double lastTypeMs_ = 0;
    const compositor::Layer* typeLayer() const;
    void refreshTypeGeometry();
    QPointF typeRaster(QPointF documentPoint) const;
    QPointF typeView(QPointF rasterPoint) const;
    bool typeBoxed() const;
    bool typeContains(QPointF documentPoint) const;
    void startTypeSession();
    void finishTypeSession(bool keep);
    void setTypeText(compositor::LayerText text, int caret, int anchor, QPointF rasterShift = {}, bool record = true);
    void typeReplace(int from, int to, const QString& insert);
    bool typeUndo(bool redo);
    bool typeShortcut(QKeyEvent* e) const;
    bool typeKey(QKeyEvent* e);
    void typePress(QPointF view, QPointF documentPoint, Qt::KeyboardModifiers modifiers);
    void typeMove(QPointF documentPoint, Qt::KeyboardModifiers modifiers);
    void typeRelease(QPointF documentPoint);
    bool typeDoubleClick(QPointF documentPoint);
    int typeBoxHandle(QPointF view) const;
    void drawTypeOverlay(QPainter& painter);
};

} // namespace app
