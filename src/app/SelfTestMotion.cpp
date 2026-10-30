// motion: the interface's animations (Motion.h) on a manual clock, stepped as a frame would be. The Tool Ring pops
// open and fades closed (slower on the idle timeout), a key acts the moment it is pressed while the pop or the close
// is still running, a value rolls, a greyed key shakes as its reason fades in, the ring glides out of the pointer's
// way; the Layer Picker's bar glides between rows and back on Esc, the layer put back at once; the Colour Ring's
// swatches trade places on a swap and turn to a new colour; an edge slides out with its shadow deepening, a page turn
// runs with the strip's pill sliding along, and holding Tab brings the edges out one after another and back together.
// With Reduce motion none of it moves (the pops are quick fades). None of it renders the document.
//
// With NEKOPHOTO_MOTION_FRAMES=<dir> it also saves each step as a picture (<dir>/<clip>/NNN.png, about 30 a second)
// for the preview clips.
#include "SelfTestSupport.h"
#include "CanvasWidget.h"
#include "ColorRing.h"
#include "EdgePages.h"
#include "EditorSession.h"
#include "WindowEdges.h"
#include "LayerPicker.h"
#include "MainWindow.h"
#include "Motion.h"
#include "ToolRing.h"
#include "nekophoto/workcounters.h"
#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QSettings>
#include <QThread>
#include <QTimer>
#include <cmath>
#include <cstdio>

// QtGui's shortcut pass for a key press (SelfTestKeys.cpp has the same).
Q_GUI_EXPORT bool qt_sendShortcutOverrideEvent(QObject* o, ulong timestamp, int k, Qt::KeyboardModifiers mods, const QString& text, bool autorep, ushort count);

using namespace nekophoto;

namespace app {

using namespace selftest;

int interfaceMotion(MainWindow& w) {
    EditorSession& s = *w.session();
    buildDemoDocument(s);
    w.resize(1100, 720);
    w.show();
    w.activateWindow();
    QApplication::processEvents();
    s.fitView();
    QApplication::processEvents();
    CanvasWidget* canvas = w.canvasAt(w.currentTabIndex());
    ToolRing* ring = w.toolRing();
    ColorRing* colours = w.colorRing();
    LayerPicker* picker = w.layerPicker();
    WindowEdges* fs = w.windowEdges();
    if (!canvas || !ring || !colours || !picker || !fs || !s.document()) { std::fprintf(stderr, "motion: no canvas, rings or document\n"); return 1; }
    canvas->setFocus();
    QApplication::processEvents();
    QSettings settings;
    settings.setValue(QStringLiteral("toolRing/enabled"), true);
    settings.setValue(QStringLiteral("toolRing/idleMs"), 0);
    settings.setValue(QStringLiteral("toolRing/corner"), 0);
    edges::setReduceMotion(false);
    int failures = 0;
    auto expect = [&](bool ok, const char* what) { if (!ok) { std::fprintf(stderr, "motion: %s\n", what); failures++; } };

    auto press = [](QWidget* target, int key, Qt::KeyboardModifiers modifiers, const QString& text = QString(), bool autoRepeat = false) {
        if (qt_sendShortcutOverrideEvent(target, 0, key, modifiers, text, autoRepeat, 1)) return true;
        QKeyEvent e(QEvent::KeyPress, key, modifiers, text, autoRepeat);
        QApplication::sendEvent(target, &e);
        return false;
    };
    auto release = [](QWidget* target, int key, Qt::KeyboardModifiers modifiers) {
        QKeyEvent e(QEvent::KeyRelease, key, modifiers);
        QApplication::sendEvent(target, &e);
    };
    auto tap = [&](int key, const char* text) { press(canvas, key, Qt::NoModifier, text ? QString::fromLatin1(text) : QString()); release(canvas, key, Qt::NoModifier); };
    // The work the test's own edits leave for the next paint (a layer added, the tool changed) is done first, so that
    // what the steps count is the animations' alone.
    // (Some is on a short timer: the events for 300 ms of the real clock, as work-counters settles.)
    auto settle = [&] {
        w.repaint();
        QElapsedTimer t;
        t.start();
        while (t.elapsed() < 300) { QApplication::processEvents(QEventLoop::AllEvents); QThread::msleep(2); }
        w.repaint();
    };
    auto alt = [&](int key, const char* text) {
        settle();
        press(canvas, Qt::Key_Alt, Qt::AltModifier);
        press(canvas, key, Qt::AltModifier, QString::fromLatin1(text));
        release(canvas, key, Qt::AltModifier);
        release(canvas, Qt::Key_Alt, Qt::NoModifier);
    };
    auto mouse = [canvas](QPointF view) {
        QMouseEvent e(QEvent::MouseMove, view, canvas->mapToGlobal(view), Qt::NoButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(canvas, &e);
    };
    auto wait = [](int ms) { QEventLoop loop; QTimer::singleShot(ms, &loop, &QEventLoop::quit); loop.exec(); };

    // The clock: Qt's animations move only when stepped. A clip saves the window's `region` at each step.
    motion::setManualClock(true);
    const QString framesDir = qEnvironmentVariable("NEKOPHOTO_MOTION_FRAMES");
    struct Clip { QString name; QRect region; int frame = 0; };
    auto shoot = [&](Clip* c) {
        if (!c || framesDir.isEmpty()) return;
        const QString dir = framesDir + QLatin1Char('/') + c->name;
        QDir().mkpath(dir);
        w.grab(c->region).save(QStringLiteral("%1/%2.png").arg(dir).arg(c->frame++, 3, 10, QLatin1Char('0')));
    };
    // `ms` of time in steps of a 30th of a second (the frames of a clip), each step's repaint done before the next.
    // What the steps do is only the animations: the document renders they cause are counted (none is allowed).
    uint64_t animationRenders = 0, animationLayerDraws = 0, frames = 0;
    auto run = [&](int ms, Clip* clip = nullptr) {
        for (int t = 0; t < ms; t += 33) {
            const work::Snapshot before = work::snapshot();
            motion::advance(std::min(33, ms - t));
            w.repaint();
            const work::Snapshot step = work::snapshot() - before;
            animationRenders += step[work::Counter::Renders];
            animationLayerDraws += step[work::Counter::LayerDraws];
            frames++;
            shoot(clip);
        }
    };
    auto around = [&](QRect r, int margin) { return r.adjusted(-margin, -margin, margin, margin).intersected(w.rect()); };

    // ---- The Tool Ring: the pop, a key during it, the close and a key during that ---------------------------------
    s.addBlankLayer();
    s.selectTool(Tool::Brush);
    s.brushErase = false;
    s.brushPreset.clear();
    s.brushSettings.diameter = 40;
    emit s.toolChanged();
    const QPointF here(canvas->width() * 0.62, canvas->height() * 0.42);
    mouse(here);
    ToolRingOverlay* overlay = ring->overlay();
    alt(Qt::Key_D, "d");
    Clip open{QStringLiteral("ring-open-close"), around(overlay->targetGeometry(), 28)};
    shoot(&open);
    expect(ring->isOpen() && overlay->isVisible() && overlay->appearance() < 1, "the ring did not start its pop");
    run(66, &open);
    expect(overlay->appearance() > 0 && overlay->appearance() < 1, "the ring's pop is not part of the way at 66 ms");
    run(160, &open);
    expect(overlay->appearance() == 1, "the ring's pop did not end by 226 ms");
    run(400, &open);
    tap(Qt::Key_Escape, nullptr);
    expect(!ring->isOpen() && overlay->isVisible() && overlay->dismissing(), "Esc did not close the ring at once, fading");
    run(50, &open);
    expect(overlay->isVisible() && overlay->appearance() < 1, "the ring's close is not under way at 50 ms");
    run(80, &open);
    expect(overlay->isHidden(), "the ring's close did not end by 130 ms");
    run(200, &open);

    // A key in the middle of the pop acts at once; so does a tool key in the middle of the close.
    alt(Qt::Key_D, "d");
    run(33);
    tap(Qt::Key_F, "f");
    expect(s.brushSettings.diameter == 48 && overlay->appearance() < 1, "F during the pop did not step the size at once");
    expect(overlay->pressing('F') && overlay->ticking('F'), "F did not bounce its circle and roll its value");
    tap(Qt::Key_Escape, nullptr);
    tap(Qt::Key_V, "v");
    expect(s.tool() == Tool::Move && overlay->dismissing(), "V during the close did not pick the Move tool at once");
    run(300);
    expect(overlay->isHidden(), "the ring did not fade after the close");
    s.selectTool(Tool::Brush);
    s.brushSettings.diameter = 40;
    emit s.toolChanged();

    // ---- A key: the bounce, the ripple and the value rolling ---------------------------------------------------------
    alt(Qt::Key_D, "d");
    run(300);
    Clip pressing{QStringLiteral("slot-press-tick"), around(overlay->targetGeometry(), 28)};
    shoot(&pressing);
    run(200, &pressing);
    tap(Qt::Key_F, "f");
    expect(overlay->pressing('F') && overlay->ticking('F'), "F did not bounce and roll");
    run(330, &pressing);
    expect(!overlay->pressing('F') && !overlay->ticking('F'), "F's bounce or roll did not end");
    tap(Qt::Key_F, "f");
    run(330, &pressing);
    tap(Qt::Key_S, "s");
    run(400, &pressing);

    // ---- A greyed key: it shakes and its reason fades in ------------------------------------------------------------
    Clip refused{QStringLiteral("refused-shake"), around(overlay->targetGeometry(), 28)};
    shoot(&refused);
    run(200, &refused);
    tap(Qt::Key_W, "w");   // Previous Brush Preset, with none before
    expect(overlay->shaking('W') && !overlay->caption().isEmpty() && overlay->captionOpacity() < 1, "W greyed did not shake, or its reason did not fade in");
    run(150, &refused);
    expect(overlay->captionOpacity() > 0.99, "the reason did not fade in by 150 ms");
    run(250, &refused);
    expect(!overlay->shaking('W'), "the shake did not end");
    run(500, &refused);

    // ---- The pointer comes at it: it glides away, its target clear of the pointer at once ------------------------
    {
        const QPointF view = canvas->mapFrom(&w, overlay->mapTo(&w, overlay->centreOf('D')));
        mouse(view);
        const QPoint pointer = canvas->mapTo(&w, view.toPoint());
        expect(!overlay->targetGeometry().contains(pointer), "the ring's new place is under the pointer");
        run(200);
        expect(overlay->pos() == overlay->target() && !overlay->gliding(), "the ring did not get to its new place");
        mouse(here);
        run(200);
    }

    // ---- The idle timeout: a slower fade -------------------------------------------------------------------------------
    tap(Qt::Key_Escape, nullptr);
    run(200);
    settings.setValue(QStringLiteral("toolRing/idleMs"), 120);
    alt(Qt::Key_D, "d");
    run(200);
    wait(250);   // the idle timer runs on the real clock
    expect(!ring->isOpen() && overlay->dismissing(), "the idle timeout did not close the ring, fading");
    run(160);
    expect(overlay->isVisible(), "the idle close was not slower than Esc's");
    run(250);
    expect(overlay->isHidden(), "the idle close did not end");
    settings.setValue(QStringLiteral("toolRing/idleMs"), 0);

    // ---- The Layer Picker: the bar glides, and back on Esc -----------------------------------------------------------
    {
        const Document& doc = *s.document();
        const QPointF point(doc.width * 0.08, doc.height * 0.45);
        auto square = [&](const char* name, QPointF centre, int r, int g, int b) {
            s.selectLayer(s.document()->layers.back().id);
            auto image = std::make_shared<Image>(40, 40);
            image->fill(uint8_t(r), uint8_t(g), uint8_t(b), 255);
            s.insertImage(image, QString::fromLatin1(name), centre);
            return *s.activeLayerId();
        };
        const Uuid a = square("Lavender", point, 185, 163, 255);
        square("Candy", point + QPointF(6, 4), 255, 126, 182);
        square("Plum", point + QPointF(-5, 7), 90, 50, 110);
        s.selectLayer(a);
        s.selectTool(Tool::Move);
        emit s.toolChanged();
        mouse(canvas->viewPointForTest(point + QPointF(0.5, 0.5)));
        LayerPickerOverlay* list = picker->overlay();
        alt(Qt::Key_A, "a");
        expect(picker->isOpen() && list->isVisible() && list->appearance() < 1, "Alt+A did not pop the picker open");
        Clip step{QStringLiteral("layer-picker-step"), around(list->targetGeometry(), 24)};
        shoot(&step);
        run(300, &step);
        const double start = list->highlightRow();
        tap(Qt::Key_F, "f");
        expect(s.activeLayerId() != a, "F did not choose the layer above at once");
        run(66, &step);
        expect(list->highlightRow() < start && list->highlightRow() > start - 1, "the bar did not glide part of the way up");
        run(200, &step);
        expect(std::abs(list->highlightRow() - (start - 1)) < 1e-9, "the bar did not get to the row above");
        tap(Qt::Key_F, "f");
        run(300, &step);
        tap(Qt::Key_V, "v");
        run(300, &step);
        tap(Qt::Key_Escape, nullptr);
        expect(!picker->isOpen() && s.activeLayerId() == a, "Esc did not put the layer back at once");
        expect(list->isVisible() && list->highlightRow() < start, "the picker did not stay to glide the bar back");
        run(150, &step);
        expect(std::abs(list->highlightRow() - start) < 1e-9, "the bar did not glide back to the layer put back");
        run(200, &step);
        expect(list->isHidden(), "the picker did not fade after the bar went back");
        run(150, &step);
    }

    // ---- The Colour Ring: a swap on an arc, a colour turning, the default colours' pulse --------------------------
    {
        s.selectTool(Tool::Brush);
        emit s.toolChanged();
        mouse(here);
        s.foregroundColor = QColor(255, 126, 182);
        s.backgroundColor = QColor(185, 163, 255);
        emit s.toolChanged();
        ToolRingOverlay* wheel = colours->overlay();
        alt(Qt::Key_C, "c");
        Clip swap{QStringLiteral("color-ring-swap"), around(wheel->targetGeometry(), 28)};
        shoot(&swap);
        run(300, &swap);
        tap(Qt::Key_C, "c");
        expect(s.foregroundColor == QColor(185, 163, 255) && wheel->swatchMoving(), "C did not swap the colours at once and move the swatches");
        run(260, &swap);
        expect(!wheel->swatchMoving(), "the swap did not end");
        run(200, &swap);
        const QColor before = wheel->shownSwatches()->first;
        tap(Qt::Key_F, "f");
        run(33, &swap);
        const QColor mid = wheel->shownSwatches()->first, after = s.foregroundColor;
        expect(wheel->swatchMoving() && mid != before, "F did not turn the swatch towards the new hue");
        run(100, &swap);
        expect(!wheel->swatchMoving(), "the swatch did not end its turn");
        Q_UNUSED(after);
        tap(Qt::Key_F, "f");
        run(200, &swap);
        tap(Qt::Key_D, "d");
        expect(wheel->pressing('D'), "D did not pulse");
        run(400, &swap);
        tap(Qt::Key_Escape, nullptr);
        run(200, &swap);
    }

    // ---- The edges: a slide with its shadow, a page turn, hold Tab -------------------------------------------------
    fs->setPolling(false);
    fs->setTimings(-1, 60000, 0);   // the slides as they are; nothing slides back on its own while the clips run
    for (auto e : {WindowEdges::Left, WindowEdges::Top, WindowEdges::Right, WindowEdges::Bottom}) edges::setAutohide(e, true);
    QApplication::processEvents();
    run(100);
    {
        const QRect whole = w.rect();
        Clip slideClip{QStringLiteral("edge-slide"), whole};
        shoot(&slideClip);
        fs->showEdge(WindowEdges::Right);
        Flyout* right = fs->flyout(WindowEdges::Right);
        expect(fs->isOut(WindowEdges::Right) && right->isVisible() && right->reveal() < 1, "the right edge did not start sliding out");
        run(66, &slideClip);
        expect(right->reveal() > 0 && right->reveal() < 1, "the right edge is not part of the way out at 66 ms");
        run(200, &slideClip);
        expect(right->reveal() == 1 && right->geometry() == fs->shownGeometry(WindowEdges::Right), "the right edge did not get all the way out");
        run(300, &slideClip);

        Clip flipClip{QStringLiteral("page-flip"), whole};
        shoot(&flipClip);
        run(100, &flipClip);
        fs->flip(WindowEdges::Right, 1);
        expect(fs->deck(WindowEdges::Right)->flipping() && fs->strip(WindowEdges::Right)->sliding(), "the page turn or the strip's pill did not animate");
        run(66, &flipClip);
        expect(fs->deck(WindowEdges::Right)->flipping(), "the page turn ended too soon");
        run(250, &flipClip);
        expect(!fs->deck(WindowEdges::Right)->flipping() && !fs->strip(WindowEdges::Right)->sliding(), "the page turn did not end");
        run(300, &flipClip);
        fs->flip(WindowEdges::Right, 1);
        run(500, &flipClip);
        fs->closeFlyouts();
        run(300, &flipClip);

        // Hold Tab: left, top, right, bottom, a little apart; let go, all back together.
        Clip peekClip{QStringLiteral("tab-peek"), whole};
        shoot(&peekClip);
        run(100, &peekClip);
        fs->peek(true, true);
        run(33, &peekClip);
        Flyout* left = fs->flyout(WindowEdges::Left);
        Flyout* bottom = fs->flyout(WindowEdges::Bottom);
        expect(left->reveal() > 0 && bottom->reveal() == 0 && fs->isOut(WindowEdges::Bottom), "hold Tab did not bring the edges out one after another");
        run(300, &peekClip);
        bool allOut = true;
        for (auto e : {WindowEdges::Left, WindowEdges::Top, WindowEdges::Right, WindowEdges::Bottom})
            if (fs->hasContent(e) && fs->flyout(e)->reveal() < 1) allOut = false;
        expect(allOut, "hold Tab did not bring every edge out");
        run(400, &peekClip);
        fs->peek(false, true);
        run(66, &peekClip);
        expect(left->reveal() < 1 && bottom->reveal() < 1 && std::abs(left->reveal() - bottom->reveal()) < 0.05, "letting Tab go did not take the edges back together");
        run(300, &peekClip);
        run(200, &peekClip);
    }

    // Every frame repainted the window, the canvas under the overlays and flyouts included: none rendered the document.
    // (Saving the frames grabs the window into pictures, which draws the canvas afresh: the count holds without them.)
    std::printf("  motion: %llu frames of animation: %llu document renders, %llu layer draws\n", (unsigned long long)frames, (unsigned long long)animationRenders,
                (unsigned long long)animationLayerDraws);
    if (framesDir.isEmpty()) expect(animationRenders == 0 && animationLayerDraws == 0, "the overlays' and edges' animations rendered the document");

    // ---- Reduce motion: quick fades, the rest at once ---------------------------------------------------------------
    edges::setReduceMotion(true);
    QApplication::processEvents();
    fs->applyPreferences();
    {
        fs->showEdge(WindowEdges::Right);
        expect(fs->flyout(WindowEdges::Right)->reveal() == 1, "with Reduce motion the edge slid out");
        fs->flip(WindowEdges::Right, 1);
        run(50);
        expect(!fs->strip(WindowEdges::Right)->sliding(), "with Reduce motion the strip's pill slid");
        fs->closeFlyouts();
        fs->peek(true, true);
        expect(fs->flyout(WindowEdges::Bottom)->reveal() == 1 && fs->flyout(WindowEdges::Left)->reveal() == 1, "with Reduce motion hold Tab staggered the edges");
        fs->peek(false, true);
        run(100);
        for (auto e : {WindowEdges::Left, WindowEdges::Top, WindowEdges::Right, WindowEdges::Bottom}) edges::setAutohide(e, false);
        QApplication::processEvents();

        s.selectTool(Tool::Brush);
        emit s.toolChanged();
        mouse(here);
        alt(Qt::Key_D, "d");
        run(33);
        expect(overlay->appearance() > 0, "with Reduce motion the ring did not fade in");
        run(66);
        expect(overlay->appearance() == 1, "with Reduce motion the ring's fade took longer than a quick fade");
        tap(Qt::Key_F, "f");
        expect(!overlay->ticking('F'), "with Reduce motion the value rolled");
        tap(Qt::Key_W, "w");
        expect(!overlay->shaking('W'), "with Reduce motion the greyed key shook");
        const QPointF view = canvas->mapFrom(&w, overlay->mapTo(&w, overlay->centreOf('D')));
        mouse(view);
        expect(!overlay->gliding() && overlay->pos() == overlay->target(), "with Reduce motion the ring glided");
        tap(Qt::Key_Escape, nullptr);
        run(100);
        expect(overlay->isHidden(), "with Reduce motion the ring's close took longer than a quick fade");
    }
    edges::setReduceMotion(false);
    motion::setManualClock(false);
    QSettings().remove(QStringLiteral("edges"));
    std::printf("motion: %s\n", failures ? "FAILED" : "ok");
    return failures ? 1 : 0;
}

} // namespace app
