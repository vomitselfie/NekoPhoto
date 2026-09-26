// The canvas: renders the document through the viewport, draws the overlays
// (transform box, selection ants, brush cursor, guides, pixel grid) and turns
// pointer input into tool actions on the session.
#pragma once
#include "EditorSession.h"
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
    /// Width / height the crop keeps while dragging; 0 is free.
    void setCropRatio(double ratio);
    void finishPolygonalLasso();
    void cancelLasso();
    QPointF documentPoint(QPointF viewPoint) const;
    QPointF viewPoint(QPointF documentPoint) const;
    EditorSession* session() const { return session_; }

signals:
    void cursorMoved(QPointF documentPoint);
    void cropChanged();

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
    enum class Drag { None, Pan, Move, Resize, Rotate, Distort, PixelMove, Brush, Warp, Gradient, Shape, Marquee, Lasso, Scribble, ClickBox, SelectionMove, Patch, Crop, CropMove, CropResize, ZoomRect, Hook };
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
    std::vector<QPointF> lassoPoints_;
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
    double cropRatio_ = 0;
    std::optional<QRectF> zoomRect_;
    std::vector<QPolygonF> selectionOutline_;
    /// Set when the outline is too detailed to trace or draw as vectors: the ants come from a raster pass instead.
    bool selectionRasterAnts_ = false;
    void drawRasterAnts(QPainter& painter, const compositor::GrayImage& coverage);
    int antsPhase_ = 0;
    QTimer antsTimer_;
    bool layerPickedOnPress_ = false;
    QCursor zoomInCursor_, zoomOutCursor_;
};

} // namespace app
