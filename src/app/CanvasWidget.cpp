#include "CanvasWidget.h"
#include <QRegion>
#include <cstring>
#include "ImageConvert.h"
#include <QApplication>
#include <QPainter>
#include <algorithm>
#include <cmath>

using namespace compositor;

namespace app {

namespace {


QPixmap checkerPixmap(double dpr) {
    int cell = int(std::round(8 * dpr));
    QPixmap pixmap(cell * 2, cell * 2);
    pixmap.setDevicePixelRatio(dpr);
    QPainter p(&pixmap);
    p.fillRect(0, 0, cell * 2, cell * 2, QColor(255, 255, 255));
    p.fillRect(0, 0, cell, cell, QColor(204, 204, 204));
    p.fillRect(cell, cell, cell, cell, QColor(204, 204, 204));
    return pixmap;
}

QCursor magnifierCursor(bool out, double dpr) {
    int s = 24;
    QPixmap pm(int(s * dpr), int(s * dpr));
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(Qt::white, 3.5)); p.setBrush(Qt::NoBrush);
    p.drawEllipse(QPointF(10, 10), 7, 7); p.drawLine(QPointF(15, 15), QPointF(22, 22));
    p.setPen(QPen(Qt::black, 1.5));
    p.drawEllipse(QPointF(10, 10), 7, 7); p.drawLine(QPointF(15, 15), QPointF(22, 22));
    p.drawLine(QPointF(6.5, 10), QPointF(13.5, 10));
    if (!out) p.drawLine(QPointF(10, 6.5), QPointF(10, 13.5));
    return QCursor(pm, 10, 10);
}

} // namespace

CanvasWidget::CanvasWidget(EditorSession* session, QWidget* parent) : QWidget(parent), session_(session) {
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setAttribute(Qt::WA_TabletTracking);
    connect(session_, &EditorSession::documentChanged, this, [this](QRectF region) {
        if (region.isEmpty()) { cacheValid_ = false; update(); }
        else invalidate(region);
    });
    // The layers draw through documentChanged; of the overlays only the transform box follows them, so a layer
    // change repaints the whole view only while a box is (or was) shown. At 2.25x a full repaint is ~5 ms.
    connect(session_, &EditorSession::layersChanged, this, [this] { if (boxShown() || boxPainted_) update(); });
    connect(session_, &EditorSession::selectionChanged, this, [this] { refreshSelectionOutline(); update(); });
    connect(session_, &EditorSession::scribblesChanged, this, [this] { update(); });
    // A viewport change leaves the cache valid: ensureCache compares zoom and origin, and a pan scrolls it.
    connect(session_, &EditorSession::viewportChanged, this, [this] { update(); });
    connect(session_, &EditorSession::toolChanged, this, [this] {
        if (session_->tool() != Tool::Crop) crop_.reset();
        if (session_->tool() != Tool::Lasso) { lassoPoints_.clear(); lassoCursor_.reset(); }
        if (session_->tool() == Tool::Crop && !crop_ && session_->hasDocument()) { crop_ = QRectF(QPointF(0, 0), documentSize()); emit cropChanged(); }
        if (hover_) updateCursor(*hover_, QApplication::keyboardModifiers());
        update();
    });
    connect(session_, &EditorSession::transformChanged, this, [this] { if (session_->transformEdit() && session_->transformEdit()->floating) refreshSelectionOutline(); update(); });
    zoomSettle_.setSingleShot(true);
    zoomSettle_.setInterval(120);
    connect(&zoomSettle_, &QTimer::timeout, this, [this] { update(); });
    antsTimer_.setInterval(120);
    connect(&antsTimer_, &QTimer::timeout, this, [this] { antsPhase_ = (antsPhase_ + 1) % 8; if (!selectionOutline_.empty() || selectionRasterAnts_) update(); });
    zoomInCursor_ = magnifierCursor(false, devicePixelRatioF());
    zoomOutCursor_ = magnifierCursor(true, devicePixelRatioF());
}

QSizeF CanvasWidget::documentSize() const {
    if (!session_->hasDocument()) return {1, 1};
    return {double(session_->document()->width), double(session_->document()->height)};
}

QRectF CanvasWidget::documentViewRect() const { return session_->viewport.documentRect(documentSize()); }
QPointF CanvasWidget::documentPoint(QPointF viewPoint) const { return session_->viewport.documentPoint(viewPoint, documentSize()); }
QPointF CanvasWidget::viewPoint(QPointF documentPoint) const { return session_->viewport.viewPoint(documentPoint, documentSize()); }
void CanvasWidget::setCropRatio(double ratio) { cropRatio_ = ratio; }

bool CanvasWidget::isBrushLike() const {
    Tool t = session_->tool();
    return t == Tool::Brush || t == Tool::SpotHealing || t == Tool::CloneStamp || t == Tool::Smudge;
}

void CanvasWidget::syncViewport() {
    QSizeF docSize = documentSize();
    session_->viewport.resize(QSizeF(size()), devicePixelRatioF(), session_->hasDocument() ? &docSize : nullptr);
}

void CanvasWidget::resizeEvent(QResizeEvent*) {
    syncViewport();
    cacheValid_ = false;
    emit session_->viewportChanged();
}

// ---- Rendering ---------------------------------------------------------------------

void CanvasWidget::renderInto(QImage& target, QRect deviceRect, QPointF documentOrigin, double zoom) {
    if (deviceRect.isEmpty() || !session_->hasDocument()) return;
    RenderOptions options;
    options.region = Rect((deviceRect.x() - documentOrigin.x()) / zoom, (deviceRect.y() - documentOrigin.y()) / zoom, deviceRect.width() / zoom, deviceRect.height() / zoom);
    options.scale = zoom;
    options.version = session_->documentRevision();
    Image out;
    Overrides overrides = session_->renderOverrides();
    compositor::render(*session_->document(), options, out, overrides.empty() ? nullptr : &overrides, &renderCache_);
    target = toQImage(out);
}

void CanvasWidget::ensureCache() {
    double dpr = devicePixelRatioF();
    double zoom = session_->viewport.zoom;
    QRectF docView = documentViewRect();
    QPointF origin(docView.x() * dpr, docView.y() * dpr);
    QRectF docDevice(origin, QSizeF(documentSize().width() * zoom, documentSize().height() * zoom));
    QRectF widgetDevice(0, 0, width() * dpr, height() * dpr);
    QRect visible = docDevice.intersected(widgetDevice).toAlignedRect();
    zoomPreview_ = false;
    if (cacheValid_ && cacheDeviceRect_ == visible && cacheZoom_ == zoom && cacheDocumentOrigin_ == origin) return;
    if (scrollCache(visible, origin, zoom)) return;
    if (zoomSettle_.isActive() && cacheValid_ && !cache_.isNull() && zoom != cacheZoom_) { zoomPreview_ = true; return; }
    cacheDeviceRect_ = visible;
    cacheZoom_ = zoom;
    cacheDocumentOrigin_ = origin;
    renderInto(cache_, visible, origin, zoom);
    cacheValid_ = true;
}

bool CanvasWidget::scrollCache(QRect visible, QPointF origin, double zoom) {
    // A pan at the same zoom by whole device pixels: what stays in view moves, and only the strips that came
    // into view are rendered. Anything else (a zoom, a fractional shift) renders the view afresh.
    if (!cacheValid_ || cache_.isNull() || zoom != cacheZoom_ || visible.isEmpty()) return false;
    const QPointF shift = origin - cacheDocumentOrigin_;
    const double dx = std::round(shift.x()), dy = std::round(shift.y());
    if (std::fabs(shift.x() - dx) > 1e-6 || std::fabs(shift.y() - dy) > 1e-6) return false;
    const QRect moved = cacheDeviceRect_.translated(int(dx), int(dy));   // where the cached pixels now sit
    const QRect kept = moved.intersected(visible);
    if (kept.isEmpty() || qint64(kept.width()) * kept.height() * 4 < qint64(visible.width()) * visible.height()) return false;
    QImage next(visible.size(), QImage::Format_RGBA8888_Premultiplied);
    next.fill(Qt::transparent);
    const QPoint from = kept.topLeft() - moved.topLeft(), to = kept.topLeft() - visible.topLeft();
    for (int y = 0; y < kept.height(); y++)
        std::memcpy(next.scanLine(to.y() + y) + size_t(to.x()) * 4, cache_.constScanLine(from.y() + y) + size_t(from.x()) * 4, size_t(kept.width()) * 4);
    for (const QRect& strip : QRegion(visible).subtracted(QRegion(kept))) {
        QImage piece;
        renderInto(piece, strip, origin, zoom);
        const QPoint at = strip.topLeft() - visible.topLeft();
        for (int y = 0; y < piece.height(); y++)
            std::memcpy(next.scanLine(at.y() + y) + size_t(at.x()) * 4, piece.constScanLine(y), size_t(piece.width()) * 4);
    }
    cache_ = next;
    cacheDeviceRect_ = visible;
    cacheDocumentOrigin_ = origin;
    return true;
}

void CanvasWidget::invalidate(QRectF documentRegion) {
    if (!cacheValid_) { update(); return; }
    double zoom = cacheZoom_;
    QRectF device(cacheDocumentOrigin_.x() + documentRegion.x() * zoom, cacheDocumentOrigin_.y() + documentRegion.y() * zoom, documentRegion.width() * zoom, documentRegion.height() * zoom);
    QRect part = device.adjusted(-2, -2, 2, 2).toAlignedRect().intersected(cacheDeviceRect_);
    if (part.isEmpty()) return;
    QImage piece;
    renderInto(piece, part, cacheDocumentOrigin_, zoom);
    cache_.setDevicePixelRatio(1);   // painted in device pixels; paintEvent sets the ratio back
    QPainter p(&cache_);
    p.setCompositionMode(QPainter::CompositionMode_Source);
    p.drawImage(part.topLeft() - cacheDeviceRect_.topLeft(), piece);
    p.end();
    if (zoomPreview_) { update(); return; }   // the cache is shown scaled: its device rect is not the widget's
    double dpr = devicePixelRatioF();
    update(QRectF(part.x() / dpr, part.y() / dpr, part.width() / dpr, part.height() / dpr).toAlignedRect().adjusted(-1, -1, 1, 1));
}

void CanvasWidget::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.fillRect(rect(), QColor(46, 46, 46));
    if (!session_->hasDocument()) {
        painter.setPen(QColor(150, 150, 150));
        painter.drawText(rect(), Qt::AlignCenter, tr("Open an image or project, or drop one here.\nFile > New creates a blank canvas."));
        return;
    }
    double dpr = devicePixelRatioF();
    // A move to a screen with another scale changes the device pixel ratio without a resize: the viewport
    // would keep the old scale for the overlay while the render used the new one, and the ants, prompts and
    // rulers would sit away from the image.
    if (std::fabs(session_->viewport.backingScale - std::max(1.0, dpr)) > 1e-9) {
        syncViewport();
        cacheValid_ = false;
        emit session_->viewportChanged();
    }
    QRectF docView = documentViewRect();
    static QPixmap checker;
    static double checkerDpr = 0;
    if (checkerDpr != dpr) { checker = checkerPixmap(dpr); checkerDpr = dpr; }
    QBrush brush(checker);
    painter.save();
    painter.setClipRect(docView);
    painter.setBrushOrigin(docView.topLeft());
    painter.fillRect(docView, brush);
    painter.restore();
    ensureCache();
    if (zoomPreview_) {
        // Mid-zoom: the last render, scaled to where its pixels fall at the new zoom.
        const double k = session_->viewport.zoom / cacheZoom_;
        const QPointF origin(docView.x() * dpr, docView.y() * dpr);
        const QRectF target((origin.x() + (cacheDeviceRect_.x() - cacheDocumentOrigin_.x()) * k) / dpr,
                            (origin.y() + (cacheDeviceRect_.y() - cacheDocumentOrigin_.y()) * k) / dpr,
                            cache_.width() * k / dpr, cache_.height() * k / dpr);
        painter.save();
        painter.setClipRect(docView);
        painter.setRenderHint(QPainter::SmoothPixmapTransform, k < 1);
        painter.drawImage(target, cache_, QRectF(0, 0, cache_.width(), cache_.height()));
        painter.restore();
    } else if (cacheValid_ && !cache_.isNull()) {
        // The ratio set on the cache itself: on a copy it would make Qt copy the whole image on every paint.
        cache_.setDevicePixelRatio(dpr);
        painter.drawImage(QPointF(cacheDeviceRect_.x() / dpr, cacheDeviceRect_.y() / dpr), cache_);
    }
    painter.setPen(QPen(QColor(0, 0, 0, 90), 1));
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(docView.adjusted(-0.5, -0.5, 0.5, 0.5));
    drawOverlays(painter);
}

} // namespace app
