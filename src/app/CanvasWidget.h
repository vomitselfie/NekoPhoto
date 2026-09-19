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
    void finishPolygonalLasso();
    void cancelLasso();
    QPointF documentPoint(QPointF viewPoint) const;
    QPointF viewPoint(QPointF documentPoint) const;

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
    enum class Drag { None, Pan, Move, Resize, Rotate, Brush, Marquee, Lasso, SelectionMove, Crop, CropMove, CropResize, ZoomRect };
    struct HandleHit { bool hit = false; int index = 0; bool rotate = false; };

    void invalidate(QRectF documentRegion);
    void ensureCache();
    void renderInto(QImage& target, QRect deviceRect, QPointF documentOrigin, double zoom);
    QSizeF documentSize() const;
    QRectF documentViewRect() const;
    void syncViewport();
    HandleHit hitHandle(QPointF viewPoint, const compositor::LayerTransform& transform) const;
    void updateCursor(QPointF viewPoint, Qt::KeyboardModifiers modifiers);
    void press(QPointF viewPoint, Qt::MouseButton button, Qt::KeyboardModifiers modifiers);
    void move(QPointF viewPoint, Qt::MouseButtons buttons, Qt::KeyboardModifiers modifiers);
    void release(QPointF viewPoint, Qt::MouseButton button, Qt::KeyboardModifiers modifiers);
    void finishMarquee(Qt::KeyboardModifiers modifiers);
    void finishFreehandLasso();
    void sampleColor(QPointF documentPoint, bool background);
    compositor::SelectionMode selectionMode(Qt::KeyboardModifiers modifiers) const;
    void drawOverlays(QPainter& painter);
    void drawTransformBox(QPainter& painter, const compositor::LayerTransform& transform, bool active);
    void drawSelectionAnts(QPainter& painter);
    void drawCropOverlay(QPainter& painter);
    QRectF dragBox(QPointF anchor, QPointF point, bool square, bool fromCenter) const;
    void refreshSelectionOutline();
    void snapMove(compositor::LayerTransform& draft, const compositor::LayerTransform& original);

    EditorSession* session_;
    QImage cache_;
    QRect cacheDeviceRect_;
    QPointF cacheDocumentOrigin_;
    double cacheZoom_ = 0;
    bool cacheValid_ = false;
    Drag drag_ = Drag::None;
    QPointF dragStartView_, dragStartDocument_, lastView_;
    std::optional<compositor::TransformDrag> transformDrag_;
    bool dragMoved_ = false;
    bool spaceHeld_ = false;
    std::optional<QPointF> hover_;
    std::vector<QPointF> lassoPoints_;
    std::optional<QPointF> lassoCursor_;
    bool polygonalLasso_ = false;
    std::optional<QRectF> marquee_;
    std::optional<compositor::Selection> selectionMoveOrigin_;
    std::optional<QRectF> crop_;
    QRectF cropOrigin_;
    int cropHandle_ = -1;
    std::optional<QRectF> zoomRect_;
    std::vector<QPolygonF> selectionOutline_;
    int antsPhase_ = 0;
    QTimer antsTimer_;
    bool layerPickedOnPress_ = false;
};

} // namespace app
