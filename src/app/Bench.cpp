#include "compositor/supports.h"
#include "Bench.h"
#include "BrushLibrary.h"
#include "CanvasWidget.h"
#include "EditorSession.h"
#include "MainWindow.h"
#include "compositor/image.h"
#include <QTextCharFormat>
#include <QInputMethodEvent>
#include <QApplication>
#include <QElapsedTimer>
#include <QKeyEvent>
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
    compositor::BrushSmoothing smoothing;
    const QString mode = o.smoothing.toLower();
    if (mode == "input" || mode == "all") smoothing.input = 50;
    if (mode == "stabilizer" || mode == "pulled" || mode == "all") smoothing.stabilizer = 50;
    if (mode == "pulled") smoothing.pulledString = true;
    if (mode == "all") smoothing.catchUpOnEnd = true;
    if (mode == "pressure" || mode == "all") smoothing.pressure = 50;
    session->brushSmoothing = smoothing;
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

    if (smoothing.active()) std::printf("smoothing: %s\n", qPrintable(mode));
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

int runTypeBench(MainWindow& window, const QString& screenshot) {
    EditorSession* session = window.session();
    CanvasWidget* canvas = window.canvasAt(window.currentTabIndex());
    if (!session->hasDocument()) session->createDocument(1400, 900, 72, true);
    session->selectTool(Tool::Text);
    emit session->toolChanged();
    session->textStyle.fontSize = 44;
    session->foregroundColor = QColor(30, 40, 90);
    session->fitView();
    QApplication::processEvents();
    auto pump = [] { QApplication::processEvents(QEventLoop::AllEvents); };
    auto key = [&](int k, const QString& text, Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
        QElapsedTimer t;
        t.start();
        QKeyEvent press(QEvent::KeyPress, k, modifiers, text);
        QApplication::sendEvent(canvas, &press);
        QKeyEvent release(QEvent::KeyRelease, k, modifiers, text);
        QApplication::sendEvent(canvas, &release);
        canvas->repaint();   // the frame that shows the keystroke
        pump();
        return double(t.nsecsElapsed()) / 1e6;
    };
    auto type = [&](const QString& s, std::vector<double>* times, std::vector<double>* layer) {
        for (QChar c : s) {
            const double ms = key(c == '\n' ? Qt::Key_Return : c.isLetter() ? Qt::Key_A + (c.toUpper().unicode() - 'A') : Qt::Key_Space, c == '\n' ? QString() : QString(c));
            if (times) times->push_back(ms);
            if (layer) layer->push_back(canvas->lastTypeLatencyMs());
        }
    };
    if (!canvas->startNewType(QPointF(80, 160), std::nullopt)) { std::printf("could not start typing\n"); return 1; }
    const QString paragraph1 = QStringLiteral("On-canvas type: a click puts the caret on the canvas,");
    const QString paragraph2 = QStringLiteral("the letters show as they are typed, and the options");
    const QString paragraph3 = QStringLiteral("bar styles the selected letters as rich text runs.");
    std::vector<double> first, third, thirdLayer;
    type(paragraph1 + '\n', &first, nullptr);
    type(paragraph2 + '\n', nullptr, nullptr);
    type(paragraph3, &third, &thirdLayer);
    auto report = [](const char* what, const std::vector<double>& v) {
        std::printf("%-44s median %6.2f ms   p95 %6.2f ms   max %6.2f ms   (%zu)\n", what, percentile(v, 0.5), percentile(v, 0.95),
                    v.empty() ? 0.0 : *std::max_element(v.begin(), v.end()), v.size());
    };
    std::printf("type bench: canvas %dx%d at dpr %.2f, %.0f px text\n", canvas->width(), canvas->height(), canvas->devicePixelRatioF(), session->textStyle.fontSize);
    report("keystroke to screen, first paragraph", first);
    report("keystroke to screen, third paragraph", third);
    report("  of which the layer's layout and raster", thirdLayer);
    // Select "rich text runs" (Shift+Ctrl+Left three times past the full stop) and make it bold and red, as the bar does.
    key(Qt::Key_Left, {});
    for (int i = 0; i < 3; i++) key(Qt::Key_Left, {}, Qt::ShiftModifier | Qt::ControlModifier);
    compositor::TextRunPatch patch;
    patch.bold = true;
    patch.color = std::array<double, 3>{0.85, 0.1, 0.1};
    canvas->applyTypeStyle(patch);
    canvas->repaint();
    pump();
    if (!screenshot.isEmpty()) window.grab().save(screenshot);
    const auto layerId = session->activeLayerId();
    key(Qt::Key_Return, {}, Qt::ControlModifier);   // commit
    const auto undo = session->undoNames();
    const compositor::Layer* layer = layerId ? session->document()->find(*layerId) : nullptr;
    std::printf("committed: %s, undo step \"%s\", %zu style runs, %d letters\n", layer && layer->text ? "yes" : "no",
                undo.empty() ? "" : undo.back().c_str(), layer && layer->text ? layer->text->runs.size() : size_t(0),
                layer && layer->text ? int(QString::fromStdString(layer->text->text).size()) : 0);
    // Paragraph text: a box dragged out below, typed into until it wraps, a word selected by a double-click.
    session->textStyle.fontSize = 30;
    session->textStyle.alignment = 1;
    if (!canvas->startNewType(QPointF(0, 0), QRectF(120, 420, 520, 300))) { std::printf("could not start paragraph text\n"); return 1; }
    std::vector<double> boxed;
    type(QStringLiteral("Paragraph text wraps inside its box and the handles resize it while the words flow again"), &boxed, nullptr);
    report("keystroke to screen, paragraph box", boxed);
    const QPointF word = canvas->viewPointForTest(QPointF(300, 470));
    QMouseEvent click(QEvent::MouseButtonDblClick, word, canvas->mapToGlobal(word), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(canvas, &click);
    canvas->repaint();
    pump();
    if (!screenshot.isEmpty()) {
        QString boxShot = screenshot;
        boxShot.insert(boxShot.lastIndexOf('.'), QStringLiteral("-box"));
        window.grab().save(boxShot);
    }
    key(Qt::Key_Escape, {});   // cancelled: the new box goes, leaving nothing in the history
    const auto after = session->undoNames();
    std::printf("paragraph cancelled: undo step \"%s\", %d layers\n", after.empty() ? "" : after.back().c_str(), int(session->document()->layers.size()));
    // An input method's composition, as fcitx5 or ibus sends it: kana while typing, a converted clause highlighted,
    // then the commit. The composition shows inline and is never recorded; the commit is one step inside the edit.
    session->textStyle.fontSize = 44;
    session->textStyle.alignment = 0;
    if (!canvas->startNewType(QPointF(80, 700), std::nullopt)) { std::printf("could not start the composition\n"); return 1; }
    auto compose = [&](const QString& preedit, const QString& commit, bool converting) {
        QList<QInputMethodEvent::Attribute> attributes;
        attributes << QInputMethodEvent::Attribute(QInputMethodEvent::Cursor, int(preedit.size()), 1);
        QTextCharFormat format;
        format.setFontUnderline(true);
        if (converting) format.setBackground(QColor(120, 160, 230));
        if (!preedit.isEmpty()) attributes << QInputMethodEvent::Attribute(QInputMethodEvent::TextFormat, 0, int(preedit.size()), format);
        QInputMethodEvent event(preedit, attributes);
        if (!commit.isEmpty()) event.setCommitString(commit);
        QApplication::sendEvent(canvas, &event);
        canvas->repaint();
        pump();
    };
    auto layerText = [&] {
        const auto id = session->activeLayerId();
        const compositor::Layer* l = id ? session->document()->find(*id) : nullptr;
        return l && l->text ? QString::fromStdString(l->text->text) : QString();
    };
    compose(QStringLiteral("に"), {}, false);
    compose(QStringLiteral("にほ"), {}, false);
    compose(QStringLiteral("にほんご"), {}, false);
    const QString composing = layerText();
    compose(QStringLiteral("日本語"), {}, true);
    if (!screenshot.isEmpty()) {
        QString imeShot = screenshot;
        imeShot.insert(imeShot.lastIndexOf('.'), QStringLiteral("-ime"));
        window.grab().save(imeShot);
    }
    compose({}, QStringLiteral("日本語"), false);
    compose(QStringLiteral("で"), {}, false);
    compose({}, QStringLiteral("です"), false);
    const QString committed = layerText();
    key(Qt::Key_Z, {}, Qt::ControlModifier);   // undo inside the edit: the last commit, not a letter of its kana
    const QString undone = layerText();
    key(Qt::Key_Return, {}, Qt::ControlModifier);
    const bool ok = composing == QStringLiteral("にほんご") && committed == QStringLiteral("日本語です") && undone == QStringLiteral("日本語");
    std::printf("input method: composing \"%s\", committed \"%s\", after one undo \"%s\": %s\n", qPrintable(composing), qPrintable(committed),
                qPrintable(undone), ok ? "ok" : "WRONG");
    return ok ? 0 : 1;
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

int runAdjustBench(MainWindow& window, const AdjustBenchOptions& o) {
    EditorSession* session = window.session();
    CanvasWidget* canvas = window.canvasAt(window.currentTabIndex());
    const int w = o.document.width(), h = o.document.height();
    // The view bench's document: a photo-like base and soft, partly transparent layers over it.
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
    QString error;
    if (o.mode == "cmyk" || o.mode == "lab") {
        if (!session->convertColorMode(o.mode == "cmyk" ? compositor::ColorMode::CMYK : compositor::ColorMode::Lab, &error)) { std::printf("mode: %s\n", qPrintable(error)); return 1; }
    }
    if (o.bits != 8 && !session->convertMode(o.bits == 16 ? compositor::SampleType::U16 : compositor::SampleType::F32, &error)) { std::printf("depth: %s\n", qPrintable(error)); return 1; }
    session->fitView();
    QApplication::processEvents();
    auto pump = [] { QApplication::processEvents(QEventLoop::AllEvents); };
    auto report = [](const char* what, std::vector<double> v) {
        std::printf("%-30s median %7.2f ms   p95 %7.2f ms   max %7.2f ms   (%zu)\n", what, percentile(v, 0.5), percentile(v, 0.95),
                    v.empty() ? 0.0 : *std::max_element(v.begin(), v.end()), v.size());
    };
    std::printf("adjust bench: %dx%d, %d layers, %d-bit %s, canvas %dx%d at dpr %.2f\n", w, h, o.layers, o.bits, qPrintable(o.mode),
                canvas->width(), canvas->height(), canvas->devicePixelRatioF());
    std::vector<double> full;
    for (int i = 0; i < 5; i++) {
        QElapsedTimer t;
        t.start();
        emit session->documentChanged({});
        canvas->repaint();
        pump();
        full.push_back(double(t.nsecsElapsed()) / 1e6);
    }
    report("full view render (fit)", full);
    // A slider drag: the adjustment layer's settings change on each tick inside one edit, as the Properties
    // panel does, and the canvas repaints before the next.
    auto drag = [&](compositor::AdjustmentKind kind, const char* label, auto&& tick) {
        // A kind the mode lacks (Exposure in CMYK) is refused with a message; skip it rather than wait on that.
        if (!compositor::unavailableReason(std::string("adjustment.") + compositor::adjustmentKindName(kind), session->sampleType(), session->colorMode()).empty()) {
            std::printf("%-30s not available in this mode\n", label);
            return;
        }
        session->addAdjustmentLayer(kind);
        const auto id = session->activeLayerId();
        if (!id) return;
        auto settings = session->adjustmentSettings(*id);
        if (!settings) { std::printf("%s: not available here\n", label); return; }
        std::vector<double> ticks;
        session->beginAdjustmentEdit();
        for (int i = 0; i < 30; i++) {
            tick(*settings, i);
            QElapsedTimer t;
            t.start();
            session->setAdjustment(*id, *settings);
            canvas->repaint();
            pump();
            ticks.push_back(double(t.nsecsElapsed()) / 1e6);
        }
        session->endAdjustmentEdit();
        report(label, ticks);
        session->undo();
        session->undo();
    };
    drag(compositor::AdjustmentKind::Levels, "Levels: drag white input", [](compositor::AdjustmentSettings& s, int i) {
        s.levels.ranges[0].white = 255 - i * 3;
    });
    drag(compositor::AdjustmentKind::Exposure, "Exposure: drag exposure", [](compositor::AdjustmentSettings& s, int i) {
        s.exposure.exposure = 0.05 * (i + 1);
    });
    return 0;
}


} // namespace app
