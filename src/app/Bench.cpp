#include "Bench.h"
#include "BrushLibrary.h"
#include "CanvasWidget.h"
#include "EditorSession.h"
#include "MainWindow.h"
#include "compositor/image.h"
#include <QApplication>
#include <QElapsedTimer>
#include <QMouseEvent>
#include <QThread>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace app {

namespace {

struct Sample { double handler = 0, total = 0; };

double percentile(std::vector<double> v, double p) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    return v[std::min(v.size() - 1, size_t(p * double(v.size() - 1) + 0.5))];
}

} // namespace

int runBrushBench(MainWindow& window, const BrushBenchOptions& o) {
    EditorSession* session = window.session();
    CanvasWidget* canvas = window.canvasAt(window.currentTabIndex());
    // A photo-like base layer (opaque gradient with detail) and a blank layer above it, as when painting.
    auto base = std::make_shared<compositor::Image>(o.document.width(), o.document.height());
    for (int y = 0; y < base->height(); y++) {
        uint8_t* row = base->row(y);
        for (int x = 0; x < base->width(); x++, row += 4) {
            row[0] = uint8_t((x * 255) / std::max(1, base->width() - 1));
            row[1] = uint8_t((y * 255) / std::max(1, base->height() - 1));
            row[2] = uint8_t((x ^ y) & 0xFF);
            row[3] = 255;
        }
    }
    session->insertImage(base, "Base");
    const auto baseId = session->activeLayerId();
    if (!o.paintOnOpaque) session->addBlankLayer();
    else session->selectLayer(baseId);
    session->selectTool(Tool::Brush);
    session->brushPreset = o.preset == "round" ? QString() : o.preset;
    if (o.brushSize > 0) session->brushSettings.diameter = o.brushSize;
    session->brushErase = o.eraser;
    if (o.hardness >= 0) session->brushSettings.hardness = o.hardness;
    session->fitView();
    if (o.zoom > 0) session->zoomTo(o.zoom, QPointF(canvas->width() / 2.0, canvas->height() / 2.0));
    warmBrushEngines();   // as a normal launch does shortly after the window appears
    QApplication::processEvents();
    QApplication::processEvents();

    // The part of the document on screen: the whole of it when fitted, the middle when zoomed in.
    const QRectF view = session->viewport.documentRect(QSizeF(o.document)).intersected(QRectF(QPointF(0, 0), QSizeF(canvas->size())));
    const QPointF center = view.center();
    const double span = std::min(view.width(), view.height()) * o.reach;
    auto pump = [] { QApplication::processEvents(QEventLoop::AllEvents); };
    qint64 timestamp = 1000;
    auto send = [&](QEvent::Type type, QPointF pos, Qt::MouseButton button, Qt::MouseButtons buttons, bool repaint = true) {
        QMouseEvent event(type, pos, canvas->mapToGlobal(pos), button, buttons, Qt::NoModifier);
        event.setTimestamp(quint64(timestamp));
        timestamp += 8;   // 125 events a second, a common mouse and pen rate
        QElapsedTimer t;
        t.start();
        QApplication::sendEvent(canvas, &event);
        Sample s;
        s.handler = double(t.nsecsElapsed()) / 1e6;
        if (repaint) pump();   // the repaint the handler asked for
        s.total = double(t.nsecsElapsed()) / 1e6;
        return s;
    };

    std::printf("brush bench: preset %s%s, size %.0f, hardness %.2f, %dx%d document, painting on %s, canvas %dx%d at zoom %.3f, %d moves\n",
                qPrintable(o.preset), o.eraser ? " (erasing)" : "", session->brushSettings.diameter, session->brushSettings.hardness, o.document.width(), o.document.height(),
                o.paintOnOpaque ? "the opaque layer" : "a blank layer", canvas->width(), canvas->height(), session->viewport.zoom, o.moves);
    std::printf("%-8s %10s %10s | %10s %10s %10s | %10s\n", "stroke", "press ms", "(handler)", "move p50", "move p95", "move max", "release ms");
    std::vector<double> pressTotals, moveTotals, releaseTotals;
    for (int s = 0; s < o.strokes; s++) {
        // A zigzag across the middle, offset per stroke so strokes do not land on the same pixels.
        auto at = [&](int i) {
            const double f = double(i) / o.moves;
            return QPointF(center.x() - span + 2 * span * f, center.y() - span / 2 + s * 6 + std::sin(f * 12) * span / 3);
        };
        // Outside the timing: how many events pass before any paint shows (a grab of the canvas each time).
        const QImage before = s == 0 ? canvas->grab().toImage() : QImage();
        auto changed = [&] { return canvas->grab().toImage() != before; };
        Sample press = send(QEvent::MouseButtonPress, at(0), Qt::LeftButton, Qt::LeftButton);
        int firstPaint = s == 0 && changed() ? 0 : -1;
        std::vector<double> moves;
        for (int i = 1; i <= o.moves; i++) {
            // With a burst, the repaint comes once per `burst` moves, as when input outpaces frames; each
            // sample is then the moves' share of the frame: (handlers + one repaint) / burst.
            const bool frame = i % std::max(1, o.burst) == 0 || i == o.moves;
            moves.push_back(send(QEvent::MouseMove, at(i), Qt::NoButton, Qt::LeftButton, frame).total);
            if (s == 0 && firstPaint < 0 && changed()) firstPaint = i;
        }
        if (s == 0) std::printf("first paint shows after %s\n", firstPaint < 0 ? "no event (nothing painted)" : firstPaint == 0 ? "the press itself" : qPrintable(QString("%1 moves (%2 ms of pointer time)").arg(firstPaint).arg(firstPaint * 8)));
        Sample release = send(QEvent::MouseButtonRelease, at(o.moves), Qt::LeftButton, Qt::NoButton);
        std::printf("%-8d %10.2f %10.2f | %10.2f %10.2f %10.2f | %10.2f\n", s + 1, press.total, press.handler,
                    percentile(moves, 0.5), percentile(moves, 0.95), *std::max_element(moves.begin(), moves.end()), release.total);
        pressTotals.push_back(press.total);
        releaseTotals.push_back(release.total);
        moveTotals.insert(moveTotals.end(), moves.begin(), moves.end());
    }
    double moveSum = 0;
    for (double m : moveTotals) moveSum += m;
    std::printf("summary: press median %.2f ms, move median %.2f ms / p95 %.2f ms, release median %.2f ms\n",
                percentile(pressTotals, 0.5), percentile(moveTotals, 0.5), percentile(moveTotals, 0.95), percentile(releaseTotals, 0.5));
    // What decides whether painting keeps up with the pointer: under 1 ms a move keeps pace with a 1000 Hz mouse.
    std::printf("moves: mean %.2f ms each with a repaint every %d (keeps up with %.0f moves a second)\n",
                moveSum / std::max<size_t>(1, moveTotals.size()), std::max(1, o.burst), 1000.0 * moveTotals.size() / std::max(1e-9, moveSum));
    std::fflush(stdout);
    return 0;
}

int runViewBench(MainWindow& window, const ViewBenchOptions& o) {
    EditorSession* session = window.session();
    CanvasWidget* canvas = window.canvasAt(window.currentTabIndex());
    const int w = o.document.width(), h = o.document.height();
    // A photo-like base, then layers that each cover most of the canvas with soft, partly transparent paint,
    // one of them multiplied: every pixel of the view composites every layer.
    auto base = std::make_shared<compositor::Image>(w, h);
    for (int y = 0; y < h; y++) {
        uint8_t* row = base->row(y);
        for (int x = 0; x < w; x++, row += 4) { row[0] = uint8_t(x * 255 / w); row[1] = uint8_t(y * 255 / h); row[2] = uint8_t((x ^ y) & 0xFF); row[3] = 255; }
    }
    session->insertImage(base, "Base");
    for (int l = 1; l < o.layers; l++) {
        auto layer = std::make_shared<compositor::Image>(w, h);
        const double cx = w * (0.3 + 0.1 * l), cy = h * (0.6 - 0.08 * l), r = std::min(w, h) * 0.45;
        for (int y = 0; y < h; y++) {
            uint8_t* row = layer->row(y);
            for (int x = 0; x < w; x++, row += 4) {
                const double d = std::hypot(x - cx, y - cy) / r;
                const unsigned a = d >= 1 ? 0 : unsigned(200 * (1 - d));
                row[0] = uint8_t((a * (40 * l)) / 255); row[1] = uint8_t((a * (255 - 40 * l)) / 255); row[2] = uint8_t(a / 2); row[3] = uint8_t(a);
            }
        }
        session->insertImage(layer, QString("Paint %1").arg(l));
    }
    session->fitView();
    QApplication::processEvents();
    QApplication::processEvents();

    auto pump = [] { QApplication::processEvents(QEventLoop::AllEvents); };
    auto timed = [&](auto&& action) {
        QElapsedTimer t;
        t.start();
        action();
        pump();
        return double(t.nsecsElapsed()) / 1e6;
    };
    const QPointF center(canvas->width() / 2.0, canvas->height() / 2.0);
    auto wheel = [&](QPoint pixelDelta, QPoint angleDelta, Qt::KeyboardModifiers modifiers) {
        QWheelEvent event(center, canvas->mapToGlobal(center), pixelDelta, angleDelta, Qt::NoButton, modifiers, Qt::NoScrollPhase, false);
        QApplication::sendEvent(canvas, &event);
    };
    auto report = [](const char* what, std::vector<double> v) {
        std::printf("%-26s median %7.2f ms   p95 %7.2f ms   max %7.2f ms   (%zu)\n", what, percentile(v, 0.5), percentile(v, 0.95),
                    v.empty() ? 0.0 : *std::max_element(v.begin(), v.end()), v.size());
    };
    std::printf("view bench: %dx%d document, %d layers, canvas %dx%d at dpr %.2f\n", w, h, o.layers, canvas->width(), canvas->height(), canvas->devicePixelRatioF());

    std::vector<double> full;
    for (int i = 0; i < 5; i++) full.push_back(timed([&] { emit session->documentChanged({}); }));
    report("full view render (fit)", full);
    std::vector<double> zoomIn, zoomOut, pan;
    // A zoom gesture shows the last render scaled until it pauses, then renders once (as the full render above).
    auto settle = [&] {
        QElapsedTimer t;
        t.start();
        while (t.elapsed() < 400) { pump(); QThread::msleep(1); }
    };
    for (int i = 0; i < 6; i++) zoomIn.push_back(timed([&] { wheel({}, {0, 120}, Qt::ControlModifier); }));
    settle();
    for (int i = 0; i < 40; i++) pan.push_back(timed([&] { wheel({0, -24}, {0, -48}, Qt::NoModifier); }));
    for (int i = 0; i < 6; i++) zoomOut.push_back(timed([&] { wheel({}, {0, -120}, Qt::ControlModifier); }));
    settle();
    report("zoom in step", zoomIn);
    report("pan step (zoomed in)", pan);
    report("zoom out step", zoomOut);

    // A stroke on the top layer, then undo and redo of it.
    session->selectTool(Tool::Brush);
    session->brushPreset = QString();
    session->brushSettings.diameter = 200;
    std::vector<double> undo, redo;
    for (int i = 0; i < 4; i++) {
        session->beginBrush(QPointF(w * 0.2, h * 0.2 + i * 50), false);
        for (int k = 1; k <= 40; k++) session->continueBrush(QPointF(w * 0.2 + k * w * 0.015, h * 0.2 + i * 50 + std::sin(k * 0.3) * 200));
        session->endBrush();
        pump();
        undo.push_back(timed([&] { session->undo(); }));
        redo.push_back(timed([&] { session->redo(); }));
    }
    report("undo a stroke", undo);
    report("redo a stroke", redo);
    std::fflush(stdout);
    return 0;
}

} // namespace app
