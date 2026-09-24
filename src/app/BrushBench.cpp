#include "BrushBench.h"
#include "BrushLibrary.h"
#include "CanvasWidget.h"
#include "EditorSession.h"
#include "MainWindow.h"
#include "compositor/image.h"
#include <QApplication>
#include <QElapsedTimer>
#include <QMouseEvent>
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
    session->fitView();
    warmBrushEngines();   // as a normal launch does shortly after the window appears
    QApplication::processEvents();
    QApplication::processEvents();

    const QRectF view = session->viewport.documentRect(QSizeF(o.document));
    const QPointF center = view.center();
    const double span = std::min(view.width(), view.height()) * 0.35;
    auto pump = [] { QApplication::processEvents(QEventLoop::AllEvents); };
    qint64 timestamp = 1000;
    auto send = [&](QEvent::Type type, QPointF pos, Qt::MouseButton button, Qt::MouseButtons buttons) {
        QMouseEvent event(type, pos, canvas->mapToGlobal(pos), button, buttons, Qt::NoModifier);
        event.setTimestamp(quint64(timestamp));
        timestamp += 8;   // 125 events a second, a common mouse and pen rate
        QElapsedTimer t;
        t.start();
        QApplication::sendEvent(canvas, &event);
        Sample s;
        s.handler = double(t.nsecsElapsed()) / 1e6;
        pump();   // the repaint the handler asked for
        s.total = double(t.nsecsElapsed()) / 1e6;
        return s;
    };

    std::printf("brush bench: preset %s, %dx%d document, painting on %s, canvas %dx%d at zoom %.3f\n",
                qPrintable(o.preset), o.document.width(), o.document.height(), o.paintOnOpaque ? "the opaque layer" : "a blank layer",
                canvas->width(), canvas->height(), session->viewport.zoom);
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
            moves.push_back(send(QEvent::MouseMove, at(i), Qt::NoButton, Qt::LeftButton).total);
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
    std::printf("summary: press median %.2f ms, move median %.2f ms / p95 %.2f ms, release median %.2f ms\n",
                percentile(pressTotals, 0.5), percentile(moveTotals, 0.5), percentile(moveTotals, 0.95), percentile(releaseTotals, 0.5));
    std::fflush(stdout);
    return 0;
}

} // namespace app
