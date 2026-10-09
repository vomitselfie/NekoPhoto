// work-counters: the work the whole window does (the canvas, its panels, the session) for a scripted brush stroke,
// an adjustment slider drag and a pan and zoom, bounded with the counters in compositor/workcounters.h instead of a
// clock (docs/work-counters.md). The core's own cases are tests/work_counter_tests.cpp; these cover what only the
// window does: the canvas's dirty rectangles and scrolled cache, the panels that follow an edit, the release and the
// undo that render only what changed.
//
// The canvas's size depends on the platform's fonts and docks, so view-sized bounds are fractions of the canvas's
// device pixels; dab-sized ones are absolute, at zoom 1 on a 1x display (ctest sets QT_SCALE_FACTOR=1).
#include "SelfTest.h"
#include "CanvasWidget.h"
#include "EditorSession.h"
#include "MainWindow.h"
#include "compositor/workcounters.h"
#include <QApplication>
#include <QElapsedTimer>
#include <QThread>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>

using namespace compositor;
using work::Counter;

namespace app {

namespace {

std::shared_ptr<Image> photo(int w, int h) {
    auto img = std::make_shared<Image>(w, h);
    for (int y = 0; y < h; y++) {
        uint8_t* row = img->row(y);
        for (int x = 0; x < w; x++, row += 4) { row[0] = uint8_t(x * 255 / w); row[1] = uint8_t(y * 255 / h); row[2] = uint8_t((x ^ y) & 0xFF); row[3] = 255; }
    }
    return img;
}

std::shared_ptr<Image> softPaint(int w, int h, int l) {
    auto img = std::make_shared<Image>(w, h);
    const double cx = w * (0.3 + 0.1 * l), cy = h * (0.6 - 0.08 * l), r = std::min(w, h) * 0.45;
    for (int y = 0; y < h; y++) {
        uint8_t* row = img->row(y);
        for (int x = 0; x < w; x++, row += 4) {
            const double d = std::hypot(x - cx, y - cy) / r;
            const unsigned a = d >= 1 ? 0 : unsigned(200 * (1 - d));
            row[0] = uint8_t((a * unsigned(40 * l)) / 255); row[1] = uint8_t((a * unsigned(255 - 40 * l)) / 255); row[2] = uint8_t(a / 2); row[3] = uint8_t(a);
        }
    }
    return img;
}

struct Bounds {
    int failures = 0;
    void at(const char* what, const work::Snapshot& delta, Counter counter, double limit) {
        const uint64_t actual = delta[counter], bound = uint64_t(std::floor(std::max(0.0, limit)));
        if (actual > bound) {
            std::fprintf(stderr, "FAIL %s: %s = %llu, expected at most %llu (all: %s)\n", what, work::name(counter), (unsigned long long)actual,
                         (unsigned long long)bound, delta.describe().c_str());
            failures++;
        } else std::printf("  %s: %s = %llu (at most %llu)\n", what, work::name(counter), (unsigned long long)actual, (unsigned long long)bound);
    }
    void value(const char* what, uint64_t actual, double limit) {
        const uint64_t bound = uint64_t(std::floor(std::max(0.0, limit)));
        if (actual > bound) { std::fprintf(stderr, "FAIL %s = %llu, expected at most %llu\n", what, (unsigned long long)actual, (unsigned long long)bound); failures++; }
        else std::printf("  %s = %llu (at most %llu)\n", what, (unsigned long long)actual, (unsigned long long)bound);
    }
    void expect(bool ok, const char* what) { if (!ok) { std::fprintf(stderr, "FAIL %s\n", what); failures++; } }
};

} // namespace

int workCounters(MainWindow& window) {
    EditorSession& s = *window.session();
    CanvasWidget* canvas = window.canvasAt(window.currentTabIndex());
    if (!canvas) { std::fprintf(stderr, "no canvas\n"); return 1; }
    Bounds b;
    // Everything the window does after an event, the deferred work included (a panel that updates on a short timer):
    // the events, then anything due within the next 300 ms.
    auto settle = [&] {
        canvas->repaint();
        QApplication::processEvents(QEventLoop::AllEvents);
        QElapsedTimer t;
        t.start();
        while (t.elapsed() < 300) { QApplication::processEvents(QEventLoop::AllEvents); QThread::msleep(2); }
    };
    // A step: what happens, then the canvas's repaint, without waiting on timers (a pointer event's own cost).
    auto step = [&](auto&& action) {
        const work::Snapshot before = work::snapshot();
        action();
        canvas->repaint();
        QApplication::processEvents(QEventLoop::AllEvents);
        return work::snapshot() - before;
    };

    // A 4000 x 3000 document: a photo and three soft paint layers (the view bench's), and a blank layer on top.
    const int W = 4000, H = 3000;
    s.insertImage(ImagePtr(photo(W, H)), "Photo");
    const auto photoId = s.activeLayerId();
    for (int l = 1; l <= 3; l++) s.insertImage(ImagePtr(softPaint(W, H, l)), QString("Paint %1").arg(l));
    s.addBlankLayer();
    const auto topId = s.activeLayerId();
    // As if opened from a file: the history starts here. (A layer made by a step still in the history keeps its first
    // buffer in that step once a later edit replaces it; docs/work-counters.md, "Found".)
    s.markOpened();
    const int pixelLayers = 5;
    s.fitView();
    settle();
    const double dpr = canvas->devicePixelRatioF();
    const double view = double(canvas->width()) * dpr * double(canvas->height()) * dpr;   // the canvas's device pixels
    std::printf("work-counters: canvas %dx%d at dpr %.2f, %.0f device pixels\n", canvas->width(), canvas->height(), dpr, view);
    b.expect(dpr == 1, "the display scale is not 1 (set QT_SCALE_FACTOR=1): the dab bounds assume it");

    // ---- A brush stroke at 100% ----------------------------------------------------------------------------------
    // 40 px round brush, hardness 0.8, a press and twenty moves 12 px apart, each followed by the canvas's repaint, on
    // the photo: a 48 MB layer, so the history has something to lose by keeping it whole.
    s.zoomTo(1.0, QPointF(canvas->width() / 2.0, canvas->height() / 2.0));
    s.selectLayer(photoId);
    settle();
    s.selectTool(Tool::Brush);
    s.brushPreset = QString();
    s.brushSettings.diameter = 40;
    s.brushSettings.hardness = 0.8;
    s.brushSmoothing = {};
    const QPointF start = s.viewport.documentPoint(QPointF(canvas->width() / 2.0 - 150, canvas->height() / 2.0), QSizeF(W, H));
    const size_t historyBefore = s.historyRetainedBytes();
    work::Snapshot d = step([&] { s.beginBrush(start, false); });
    // One dab: the session reports its box (42 x 42 here), the canvas renders that and 2 device pixels around it
    // (cachePart). Measured 2162 (46 x 47); bound 48 x 48, a 44 px box with that margin. The pixel layers under it at
    // most once each (measured 8648: four layers, the blank one draws nothing).
    b.at("press", d, Counter::RenderPixels, 48 * 48);
    b.at("press", d, Counter::LayerDrawPixels, pixelLayers * 48 * 48);
    b.at("press", d, Counter::BrushDabs, 1);
    work::Snapshot strokeTotal;
    uint64_t worstMove = 0;
    for (int i = 1; i <= 20; i++) {
        d = step([&] { s.continueBrush(start + QPointF(12.0 * i, 20 * std::sin(i * 0.3))); });
        worstMove = std::max(worstMove, d[Counter::RenderPixels]);
        for (size_t k = 0; k < work::counterCount; k++) strokeTotal.values[k] += d.values[k];
    }
    // A move renders the segment and its provisional tail: measured worst 4464 device pixels, 81410 for the twenty and
    // 325640 drawn; bounds about 1.3x (a whole-view render per move would be the canvas, half a million or more).
    b.value("worst move: render.pixels", worstMove, 6000);
    b.at("twenty moves", strokeTotal, Counter::RenderPixels, 106000);
    b.at("twenty moves", strokeTotal, Counter::LayerDrawPixels, 424000);
    b.at("twenty moves", strokeTotal, Counter::MipMisses, 0);
    // The release commits what the preview showed: nothing is rendered again (documentChangedAsShown).
    d = step([&] { s.endBrush(); });
    b.at("release", d, Counter::RenderPixels, 0);
    settle();
    // The stroke's history: its region's before and after on a 4000 x 3000 layer, not the 48 MB layer: about 290 x 75
    // pixels at 8 bytes, and the replaced asset's thumbnail. Measured 206208 bytes; bound 270000 (1.3x).
    b.value("history bytes for the stroke", s.historyRetainedBytes() - historyBefore, 270000);
    b.expect(s.undoNames().size() == 1, "the stroke is not the history's one step");
    // Undo and redo render only the stroke's region (DocumentHistory::noteRegion): measured 26390 each; bound 34000
    // (1.3x; the whole view would be the canvas's pixels).
    d = step([&] { s.undo(); });
    b.at("undo", d, Counter::RenderPixels, 34000);
    d = step([&] { s.redo(); });
    b.at("redo", d, Counter::RenderPixels, 34000);
    settle();

    // ---- An adjustment layer's slider drag at fit zoom --------------------------------------------------------------
    // A Levels layer on top of the five; the canvas keeps the frame below it while its settings are dragged.
    s.fitView();
    s.selectLayer(topId);
    settle();
    s.addAdjustmentLayer(AdjustmentKind::Levels);
    settle();
    const auto id = s.activeLayerId();
    auto settings = id ? s.adjustmentSettings(*id) : std::nullopt;
    b.expect(bool(settings), "no Levels layer");
    if (settings) {
        s.beginAdjustmentEdit();
        const uint64_t revision = s.documentRevision();
        work::Snapshot drag;
        const int ticks = 20;
        for (int i = 0; i < ticks; i++) {
            settings->levels.ranges[0].white = 255 - i * 3;
            d = step([&] { s.setAdjustment(*id, *settings); });
            for (size_t k = 0; k < work::counterCount; k++) drag.values[k] += d.values[k];
        }
        // Each tick renders the view once (no panel renders the document again) and makes the Levels pass over it. The
        // first tick draws the pixel layers below once each and keeps that frame (RenderCache::resume); the next ones
        // draw no layer at all: nothing is above the Levels layer. Measured over the twenty ticks: 20 renders of the
        // document's 303 372 visible pixels, 4 draws (the blank layer draws nothing), 20 adjustment passes; before the
        // frame was kept, 80 draws (4 a tick). Bounds: one render of the view a tick, each layer once for the drag.
        // A quarter more renders than ticks: the platform may repaint the window on its own during the drag (21 on
        // GitHub's Windows runner, 22 on its Linux one, 20 here), while a regression that renders twice a tick is 40.
        // Such a repaint draws from the kept frame too, so it adds no draw.
        const int slack = ticks / 4;
        b.at("slider ticks", drag, Counter::Renders, ticks + slack);
        b.at("slider ticks", drag, Counter::RenderPixels, view * ticks);
        b.at("slider ticks", drag, Counter::LayerDraws, pixelLayers);
        b.at("slider ticks", drag, Counter::LayerDrawPixels, pixelLayers * view);
        b.at("slider ticks", drag, Counter::Adjustments, ticks + slack);   // those renders' Levels passes
        // Every layer from its cached reduction: none built during the drag.
        b.at("slider ticks", drag, Counter::MipMisses, 0);
        b.at("slider ticks", drag, Counter::MipBuiltPixels, 0);
        b.expect(s.documentRevision() == revision, "a slider tick moved the document revision on: the canvas cannot keep the frame below the layer");
        // Anything else changing during the drag moves the revision on and the frame below is drawn again: the photo's
        // eye swiped off and back on (the three paint layers, then all four), then a tick draws nothing again.
        d = step([&] { s.beginVisibilitySwipe(*photoId); });
        b.expect(s.documentRevision() != revision, "hiding a layer during an adjustment drag left the document revision");
        b.expect(d[Counter::LayerDraws] >= 3, "the frame after hiding a layer below the dragged adjustment did not draw the layers below again");
        d = step([&] { s.setVisibilityInSwipe(*photoId, true); s.endVisibilitySwipe(); });
        b.expect(d[Counter::LayerDraws] >= 4, "the frame after showing a layer below the dragged adjustment did not draw the layers below again");
        settings->levels.ranges[0].white = 200;
        d = step([&] { s.setAdjustment(*id, *settings); });
        b.at("a tick after the swipe", d, Counter::LayerDraws, 0);
        s.endAdjustmentEdit();
        settle();
    }

    // ---- Pan and zoom -----------------------------------------------------------------------------------------------
    // At 40% (layers drawn from level 1): a zoom renders the view once and builds each layer's reduction once; pans by
    // 24 device pixels scroll the cached view and render the strip that came in; zooming back builds nothing.
    // Measured: one render of the view, no reduction built (fitting at 16% built levels 1 and 2, both still held);
    // bounds: one render, and at most one build a layer should the budget have let level 1 go.
    d = step([&] { s.zoomTo(0.4, QPointF(canvas->width() / 2.0, canvas->height() / 2.0)); });
    b.at("zoom to 40%", d, Counter::Renders, 1);
    b.at("zoom to 40%", d, Counter::RenderPixels, view);
    b.at("zoom to 40%", d, Counter::MipMisses, pixelLayers);
    settle();
    // At 40% the document (1600 x 1200 device pixels) is larger than the view both ways and centred, so ten 16-pixel
    // pans down and ten to the right each bring a strip of the document into view.
    const double widthDevice = canvas->width() * dpr, heightDevice = canvas->height() * dpr;
    work::Snapshot pans;
    for (int i = 0; i < 20; i++) {
        const bool down = i < 10;
        d = step([&] { s.viewport.translate(down ? QPointF(0, 16) : QPointF(16, 0)); emit s.viewportChanged(); });
        // The strip that came into view, 16 device pixels across the view: measured exactly that (11 712 and 10 256
        // with a 732 x 641 canvas); the bound is the strip, so a pan that renders the view again fails.
        b.at(down ? "pan down" : "pan right", d, Counter::RenderPixels, 16 * (down ? widthDevice : heightDevice));
        b.at(down ? "pan down" : "pan right", d, Counter::Renders, 1);
        for (size_t k = 0; k < work::counterCount; k++) pans.values[k] += d.values[k];
    }
    b.at("twenty pans", pans, Counter::LayerDrawPixels, pixelLayers * 10 * 16 * (widthDevice + heightDevice));
    b.at("twenty pans", pans, Counter::MipMisses, 0);
    b.expect(pans[Counter::MipHits] >= 20, "the pans drew no layer from a reduction");
    d = step([&] { s.zoomTo(0.2, QPointF(canvas->width() / 2.0, canvas->height() / 2.0)); });
    // 20% wants level 2: measured no build (held since the fit); bound one build a layer, from the level 1 held.
    b.at("zoom to 20%", d, Counter::MipMisses, pixelLayers);
    b.at("zoom to 20%", d, Counter::MipBuiltPixels, pixelLayers * (W / 4.0) * (H / 4.0) + 1);
    d = step([&] { s.zoomTo(0.4, QPointF(canvas->width() / 2.0, canvas->height() / 2.0)); });
    b.at("back to 40%", d, Counter::MipMisses, 0);
    b.at("back to 40%", d, Counter::RenderPixels, view);

    std::printf("work-counters: %s\n", b.failures ? "FAILED" : "ok");
    return b.failures ? 1 : 0;
}

} // namespace app
