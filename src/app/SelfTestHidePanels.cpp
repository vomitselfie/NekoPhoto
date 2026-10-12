// hide-panels: Tab and Shift+Tab in the Standard Screen Mode (HidePanels.h) through synthesised keys. Tab hides every
// panel, the tool rail and the options bar, and leaves the menus, the document tabs and the status bar; Tab again gives
// back saveState() byte for byte, a panel that was closed staying closed. Shift+Tab hides the panels on the right only.
// Tab stays with a field and with typing on the canvas. Full screen while they are hidden shows every panel at its
// edges, and leaving it hides them again; F7 while they are hidden brings the layout back with the Layers panel.
#include "SelfTest.h"
#include "CanvasWidget.h"
#include "EditorSession.h"
#include "FullScreen.h"
#include "HidePanels.h"
#include "MainWindow.h"
#include "ToolOptionsBar.h"
#include <QAbstractSpinBox>
#include <QApplication>
#include <QDockWidget>
#include <QElapsedTimer>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMenuBar>
#include <QStatusBar>
#include <QThread>
#include <QToolBar>
#include <cstdio>
#include <functional>

// QtGui's shortcut pass for a key press (SelfTest.cpp): true when a shortcut took the key.
Q_GUI_EXPORT bool qt_sendShortcutOverrideEvent(QObject* o, ulong timestamp, int k, Qt::KeyboardModifiers mods, const QString& text, bool autorep, ushort count);

using namespace nekophoto;

namespace app {

void buildDemoDocument(EditorSession& session);   // main.cpp

namespace {

void pumpEvents(int ms) {
    QElapsedTimer t;
    t.start();
    do {
        QApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(2);
    } while (t.elapsed() < ms);
}

} // namespace

int hidePanels(MainWindow& w) {
    EditorSession& s = *w.session();
    buildDemoDocument(s);
    w.resize(1400, 900);
    w.show();
    w.activateWindow();
    pumpEvents(100);
    HidePanels* hide = w.hidePanels();
    FullScreenMode* fs = w.fullScreenMode();
    CanvasWidget* canvas = w.canvasAt(w.currentTabIndex());
    if (!hide || !fs || !canvas) { std::fprintf(stderr, "hide panels: no controller or canvas\n"); return 1; }
    fs->setPolling(false);
    fs->setTimings(0, 0);
    s.selectTool(Tool::Hand);
    canvas->setFocus();
    pumpEvents(50);
    int failures = 0;
    auto expect = [&](bool ok, const char* what) { if (!ok) { std::fprintf(stderr, "hide panels: %s\n", what); failures++; } };
    auto press = [](QWidget* target, int key, Qt::KeyboardModifiers modifiers, const QString& text = QString()) {
        if (qt_sendShortcutOverrideEvent(target, 0, key, modifiers, text, false, 1)) return true;
        QKeyEvent e(QEvent::KeyPress, key, modifiers, text);
        QApplication::sendEvent(target, &e);
        return false;
    };
    auto dock = [&w](const char* name) { return w.findChild<QDockWidget*>(QString::fromLatin1(name)); };
    QDockWidget* layersDock = dock("layersDock");
    QDockWidget* adjustDock = dock("adjustmentsDock");
    QDockWidget* histogramDock = dock("histogramDock");
    QDockWidget* actionsDock = dock("actionsDock");
    auto* rail = w.findChild<QToolBar*>(QStringLiteral("toolRail"));
    if (!layersDock || !adjustDock || !histogramDock || !actionsDock || !rail) { std::fprintf(stderr, "hide panels: the docks or the rail are missing\n"); return 1; }
    auto optionsShown = [&w] {
        for (ToolOptionsBar* bar : w.findChildren<ToolOptionsBar*>()) if (bar->isVisible()) return true;
        return false;
    };
    auto anyDockShown = [&w] {
        for (QDockWidget* d : w.findChildren<QDockWidget*>(Qt::FindDirectChildrenOnly)) if (d->isVisible()) return true;
        return false;
    };
    // A panel the person closed (Histogram, behind Adjustments) and one closed from the start (Actions).
    histogramDock->close();
    pumpEvents(50);
    expect(actionsDock->isHidden() && histogramDock->isHidden(), "the closed panels are open");
    expect(w.dockWidgetArea(layersDock) == Qt::RightDockWidgetArea && layersDock->isVisible() && adjustDock->isVisible(), "Layers and Adjustments are not showing on the right");
    expect(rail->isVisible() && optionsShown(), "the tools or the options bar are not showing");
    const QByteArray before = w.saveState();
    const bool menuBar = w.menuBar()->isVisible(), statusBar = w.statusBar()->isVisible();

    // Tab: the panels, the tools and the options bar go; the menus, the document tabs and the status bar stay.
    expect(press(canvas, Qt::Key_Tab, Qt::NoModifier), "no shortcut took Tab");
    pumpEvents(50);
    expect(hide->hidden() == HidePanels::All, "Tab did not hide the panels");
    expect(!anyDockShown() && !rail->isVisible() && !optionsShown(), "Tab left a panel, the tools or the options bar on screen");
    expect(w.menuBar()->isVisible() == menuBar && w.statusBar()->isVisible() == statusBar, "Tab hid the menus or the status bar");
    expect(QApplication::focusWidget() == canvas, "the canvas lost the keys");
    expect(press(canvas, Qt::Key_Tab, Qt::NoModifier), "no shortcut took Tab again");
    pumpEvents(50);
    expect(hide->hidden() == HidePanels::None && layersDock->isVisible() && adjustDock->isVisible() && rail->isVisible() && optionsShown(), "Tab again did not bring them back");
    expect(histogramDock->isHidden() && actionsDock->isHidden(), "a closed panel came back with Tab");
    expect(w.saveState() == before, "Tab twice did not give back saveState() byte for byte");

    // Shift+Tab: only the panels on the right.
    expect(press(canvas, Qt::Key_Backtab, Qt::ShiftModifier), "no shortcut took Shift+Tab");
    pumpEvents(50);
    expect(hide->hidden() == HidePanels::Right, "Shift+Tab did not hide the right panels");
    expect(!layersDock->isVisible() && !adjustDock->isVisible(), "Shift+Tab left a right panel on screen");
    expect(rail->isVisible() && optionsShown(), "Shift+Tab hid the tools or the options bar");
    press(canvas, Qt::Key_Backtab, Qt::ShiftModifier);
    pumpEvents(50);
    expect(hide->hidden() == HidePanels::None && layersDock->isVisible() && histogramDock->isHidden(), "Shift+Tab again did not bring the right panels back");
    expect(w.saveState() == before, "Shift+Tab twice did not give back saveState() byte for byte");

    // Tab in a field moves between fields, and hides nothing.
    {
        QWidget* field = nullptr;
        for (QWidget* c : layersDock->findChildren<QWidget*>())
            if ((qobject_cast<QLineEdit*>(c) || qobject_cast<QAbstractSpinBox*>(c)) && c->isVisible() && c->isEnabled()) { field = c; break; }
        if (!field)
            for (ToolOptionsBar* bar : w.findChildren<ToolOptionsBar*>())
                for (QWidget* c : bar->findChildren<QWidget*>())
                    if (!field && (qobject_cast<QLineEdit*>(c) || qobject_cast<QAbstractSpinBox*>(c)) && c->isVisible() && c->isEnabled()) field = c;
        if (!field) expect(false, "no field to type in");
        else {
            field->setFocus();
            pumpEvents(20);
            expect(!press(QApplication::focusWidget(), Qt::Key_Tab, Qt::NoModifier, QStringLiteral("\t")), "a shortcut took Tab from a field");
            field->setFocus();   // Tab moved the keys on, perhaps to a button
            pumpEvents(20);
            expect(!press(QApplication::focusWidget(), Qt::Key_Backtab, Qt::ShiftModifier), "a shortcut took Shift+Tab from a field");
            expect(hide->hidden() == HidePanels::None && layersDock->isVisible(), "Tab in a field hid the panels");
        }
        canvas->setFocus();
        pumpEvents(20);
    }
    // Typing on the canvas keeps Tab.
    s.selectTool(Tool::Text);
    if (canvas->startNewType(QPointF(100, 100), std::nullopt)) {
        canvas->setFocus();
        expect(!press(canvas, Qt::Key_Tab, Qt::NoModifier, QStringLiteral("\t")) && hide->hidden() == HidePanels::None, "Tab while typing on the canvas hid the panels");
        press(canvas, Qt::Key_Escape, Qt::NoModifier);
        expect(!canvas->typeEditing(), "Esc did not cancel the typing");
    } else expect(false, "could not start typing");
    s.selectTool(Tool::Hand);
    canvas->setFocus();
    pumpEvents(20);

    // Full screen while they are hidden: every panel at its edges; back in the standard mode, hidden again.
    press(canvas, Qt::Key_Tab, Qt::NoModifier);
    pumpEvents(50);
    expect(hide->hidden() == HidePanels::All, "Tab did not hide the panels before full screen");
    expect(press(canvas, Qt::Key_F, Qt::NoModifier, QStringLiteral("f")) && fs->active(), "F did not enter full screen");
    pumpEvents(100);
    expect(fs->edgeHolding(layersDock) == FullScreenMode::Right && fs->hasContent(FullScreenMode::Left) && fs->hasContent(FullScreenMode::Top),
           "full screen's edges did not get the panels, the tools and the options bar");
    expect(fs->edgeHolding(histogramDock) < 0 && fs->edgeHolding(actionsDock) < 0, "a closed panel came out in full screen");
    expect(press(canvas, Qt::Key_Tab, Qt::NoModifier) && fs->isPinned(FullScreenMode::Left), "Tab in full screen did not pin the edges");
    press(canvas, Qt::Key_Tab, Qt::NoModifier);
    pumpEvents(50);
    expect(hide->hidden() == HidePanels::All, "Tab in full screen changed what the standard mode hides");
    expect(press(canvas, Qt::Key_F, Qt::NoModifier, QStringLiteral("f")) && !fs->active(), "F did not leave full screen");
    pumpEvents(300);
    expect(hide->hidden() == HidePanels::All && !anyDockShown() && !rail->isVisible() && !optionsShown(), "leaving full screen did not hide the panels again");
    expect(w.menuBar()->isVisible() == menuBar && w.statusBar()->isVisible() == statusBar, "leaving full screen lost the menus or the status bar");
    press(canvas, Qt::Key_Tab, Qt::NoModifier);
    pumpEvents(50);
    expect(hide->hidden() == HidePanels::None && layersDock->isVisible() && rail->isVisible() && optionsShown() && histogramDock->isHidden(), "Tab after full screen did not bring the panels back");
    expect(w.saveState() == before, "Tab after full screen did not give back saveState() byte for byte");

    // F7 while hidden: the layout comes back, with Layers.
    press(canvas, Qt::Key_Tab, Qt::NoModifier);
    pumpEvents(50);
    expect(press(canvas, Qt::Key_F7, Qt::NoModifier), "no shortcut took F7");
    pumpEvents(50);
    expect(hide->hidden() == HidePanels::None && layersDock->isVisible() && rail->isVisible() && optionsShown(), "F7 while hidden did not bring the layout back");

    // A new tab while the options bar is hidden keeps it hidden.
    press(canvas, Qt::Key_Tab, Qt::NoModifier);
    pumpEvents(50);
    const int first = w.currentTabIndex();
    const int added = w.newTab();
    pumpEvents(50);
    expect(!optionsShown(), "a new tab showed its options bar while Tab had hidden it");
    w.closeTabAt(added);
    w.selectTab(first);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    pumpEvents(50);
    canvas = w.canvasAt(w.currentTabIndex());
    canvas->setFocus();
    press(canvas, Qt::Key_Tab, Qt::NoModifier);
    pumpEvents(50);
    expect(hide->hidden() == HidePanels::None && optionsShown() && rail->isVisible(), "Tab did not bring the options bar back on the first tab");
    std::printf("hide panels: %s\n", failures ? "FAILED" : "ok");
    return failures ? 1 : 0;
}

} // namespace app
