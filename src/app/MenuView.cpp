// The View, Window and Help menus (MainWindow::buildMenus, MainWindowMenus.h).
#include "MainWindowMenus.h"
#include "CanvasFrame.h"
#include "Dialogs.h"
#include "HdrDialogs.h"
#include "ColorDialogs.h"
#include "ColorManagement.h"
#include "ViewOptions.h"
#include "FullScreen.h"
#include "HidePanels.h"
#include "ColorRing.h"
#include "LayerPicker.h"
#include "ToolRing.h"
#include "nekophoto/project.h"
#include <QActionGroup>
#include <QDockWidget>
#include <QMenuBar>
#include <QMessageBox>
#include <QApplication>
#include <QJsonObject>
#include <QSettings>
#include <QSignalBlocker>

using namespace nekophoto;

namespace app {

using namespace menus;

void MainWindow::buildViewMenu(MenuContext& m) {
    QMenu* view = menuBar()->addMenu(tr("&View"));
    // The zoom runs view.zoom (the view, not the document: nothing is recorded).
    m.add(view, Spec("view.zoomIn", tr("Zoom &In"), {QKeySequence::ZoomIn}).document("view").request("view.zoom", [this] { return QJsonObject{{"zoom", session_->viewport.zoom * 1.25}}; }))
        ->setObjectName("command.view.zoomIn");
    m.add(view, Spec("view.zoomOut", tr("Zoom &Out"), {QKeySequence::ZoomOut}).document("view").request("view.zoom", [this] { return QJsonObject{{"zoom", session_->viewport.zoom / 1.25}}; }));
    m.add(view, Spec("view.fitOnScreen", tr("&Fit on Screen"), {QKeySequence("Ctrl+0")}).document("view").request("view.zoom", [] { return QJsonObject{{"fit", true}}; }))
        ->setObjectName("command.view.fit");
    m.add(view, Spec("view.actualPixels", tr("&Actual Pixels"), {QKeySequence("Ctrl+1")}).document("view").request("view.zoom", [] { return QJsonObject{{"zoom", 1}}; }))
        ->setObjectName("command.view.actualPixels");
    view->addSeparator();
    rulersAction_ = m.add(view, Spec("view.rulers", tr("&Rulers"), {QKeySequence("Ctrl+R")}));
    connect(rulersAction_, &QAction::triggered, this, [this](bool on) {
        for (auto& tab : tabs_) tab.frame->setRulersVisible(on);
        QSettings().setValue("view/rulers", on);
    });
    rulersAction_->setCheckable(true);
    rulersAction_->setChecked(QSettings().value("view/rulers", true).toBool());
    for (auto& tab : tabs_) tab.frame->setRulersVisible(rulersAction_->isChecked());
    // Photoshop's guides and snapping: View > Show > Guides (Ctrl+;) and Smart Guides, Snap (Shift+Ctrl+;) and Snap To,
    // Lock Guides (Alt+Ctrl+;), Clear Guides and New Guide. The switches are the app's (ViewOptions), the guides the
    // document's: adding and clearing them are undo steps, run as the guides.* commands.
    auto viewOption = [this, &m](QMenu* menu, const char* id, const QString& label, const QKeySequence& key, bool ViewOptions::*field) {
        QAction* a = m.reg.add(menu, Spec(id, label, key.isEmpty() ? QList<QKeySequence>{} : QList<QKeySequence>{key}).command);
        a->setCheckable(true);
        a->setChecked(ViewOptions::get().*field);
        connect(a, &QAction::toggled, this, [this, field](bool on) {
            ViewOptions::get().*field = on;
            ViewOptions::get().save();
            for (auto& tab : tabs_) tab.canvas->update();
        });
        connect(menu, &QMenu::aboutToShow, a, [a, field] { a->setChecked(ViewOptions::get().*field); });
        return a;
    };
    QMenu* show = view->addMenu(tr("S&how"));
    QAction* showGuides = viewOption(show, "view.show.guides", tr("&Guides"), QKeySequence("Ctrl+;"), &ViewOptions::showGuides);
    viewOption(show, "view.show.smartGuides", tr("&Smart Guides"), QKeySequence(), &ViewOptions::smartGuides);
    addAction(showGuides);   // the shortcut works with the menu closed
    QAction* snap = viewOption(view, "view.snap", tr("Sn&ap"), QKeySequence("Shift+Ctrl+;"), &ViewOptions::snap);
    addAction(snap);
    QMenu* snapTo = view->addMenu(tr("Snap &To"));
    QList<QAction*> snapTargets{viewOption(snapTo, "view.snapTo.guides", tr("&Guides"), QKeySequence(), &ViewOptions::snapToGuides),
                                viewOption(snapTo, "view.snapTo.layers", tr("&Layers"), QKeySequence(), &ViewOptions::snapToLayers),
                                viewOption(snapTo, "view.snapTo.documentBounds", tr("&Document Bounds"), QKeySequence(), &ViewOptions::snapToBounds)};
    snapTo->addSeparator();
    auto setAllTargets = [snapTargets](bool on) {
        ViewOptions& o = ViewOptions::get();
        o.snapToGuides = o.snapToLayers = o.snapToBounds = on;
        o.save();
        for (QAction* a : snapTargets) { QSignalBlocker block(a); a->setChecked(on); }
    };
    m.add(snapTo, Spec("view.snapTo.all", tr("&All", "snap to")).runs([setAllTargets] { setAllTargets(true); }));
    m.add(snapTo, Spec("view.snapTo.none", tr("&None", "snap to")).runs([setAllTargets] { setAllTargets(false); }));
    view->addSeparator();
    QAction* lockGuides = viewOption(view, "view.lockGuides", tr("Lock Gu&ides"), QKeySequence("Alt+Ctrl+;"), &ViewOptions::lockGuides);
    addAction(lockGuides);
    m.add(view, Spec("view.clearGuides", tr("C&lear Guides")).document().request("guides.delete", [this]() -> std::optional<QJsonObject> {
        if (session_->guides().empty()) return std::nullopt;
        return QJsonObject{{"all", true}};
    }, tr("Clear Guides")));
    m.add(view, Spec("view.newGuide", tr("&New Guide…")).document().runs([this] {
        auto guide = askNewGuide(this);
        if (!guide) return;
        runCommand("guides.add", {{"orientation", guide->vertical() ? "vertical" : "horizontal"}, {"position", guide->position}}, tr("New Guide"));
    }, "guides.add"));
    view->addSeparator();
    // Photoshop's screen modes: Standard, and Full Screen (FullScreen.h), where nothing but the canvas is on screen and
    // each screen edge slides its part of the interface out when the pointer reaches it. F steps through them, Esc
    // leaves full screen; there Tab pins every edge out (or hides them again), Shift+Tab all but the tools; F10 or Alt
    // opens the menus.
    QMenu* screenMode = view->addMenu(tr("Scree&n Mode"));
    auto* screenModes = new QActionGroup(this);
    // They run view.screenMode (the window, not the document: nothing is recorded).
    QAction* standardMode = m.add(screenMode, Spec("view.screenMode.standard", tr("&Standard Screen Mode")).request("view.screenMode", [] { return QJsonObject{{"mode", "standard"}}; }));
    QAction* fullMode = m.add(screenMode, Spec("view.screenMode.fullScreen", tr("&Full Screen Mode")).request("view.screenMode", [] { return QJsonObject{{"mode", "full"}}; }));
    for (QAction* a : {standardMode, fullMode}) { a->setCheckable(true); screenModes->addAction(a); }
    standardMode->setChecked(true);
    connect(fullScreen_, &FullScreenMode::activeChanged, this, [standardMode, fullMode](bool on) { fullMode->setChecked(on); standardMode->setChecked(!on); });
    m.add(nullptr, Spec("view.screenMode.cycle", tr("Cycle Screen Modes"), {QKeySequence("F")})
                     .request("view.screenMode", [this] { return QJsonObject{{"mode", fullScreen_->active() ? "standard" : "full"}}; }));
    screenMode->addSeparator();
    // In the standard mode Tab hides the panels, the tools and the options bar, Shift+Tab the panels on the right
    // (HidePanels.h); again gives them back.
    m.add(screenMode, Spec("view.screenMode.panels", tr("Show or Hide &Panels"), {QKeySequence(Qt::Key_Tab)})
                        .request("view.screenMode", [this] {
                            if (!fullScreen_->active()) return QJsonObject{{"panels", hidePanels_->hidden() != HidePanels::None ? "all" : "hidden"}};
                            return QJsonObject{{"panels", fullScreen_->pinnedAll(true) ? "hidden" : "all"}};
                        }));
    m.add(screenMode, Spec("view.screenMode.panelsExceptTools", tr("Show or Hide Panels &Except Tools"), {QKeySequence(Qt::SHIFT | Qt::Key_Tab)})
                        .request("view.screenMode", [this] {
                            if (!fullScreen_->active()) return QJsonObject{{"panels", hidePanels_->hidden() != HidePanels::None ? "all" : "hiddenRight"}};
                            return QJsonObject{{"panels", fullScreen_->pinnedAll(false) ? "hidden" : "exceptTools"}};
                        }));
    // The edges' pages (FullScreen.h, EdgePages.h): while an edge is out, Tab and Shift+Tab flip it. Their keys work
    // only then (keyContext), so Tab keeps Show or Hide Panels otherwise; Edit > Keyboard Shortcuts can change them.
    for (int step : {1, -1}) {
        Command c;
        c.id = step > 0 ? QStringLiteral("view.edgePage.next") : QStringLiteral("view.edgePage.previous");
        c.label = step > 0 ? tr("Next Edge Page") : tr("Previous Edge Page");
        c.shortcuts = {step > 0 ? QKeySequence(Qt::Key_Tab) : QKeySequence(Qt::SHIFT | Qt::Key_Tab)};
        c.keyContext = tr("while an edge is out");
        c.run = [this, step] { if (const int edge = fullScreen_->flipTarget(); edge >= 0) fullScreen_->flip(FullScreenMode::Edge(edge), step); };
        c.unavailable = [this] { return fullScreen_->flipTarget() >= 0 ? QString() : tr("No edge is out"); };
        m.reg.add(nullptr, std::move(c));
    }
    // F10 opens the menus (in full screen over the canvas, the menu bar showing while one is open).
    m.add(nullptr, Spec("view.showMenus", tr("Show Menus"), {QKeySequence(Qt::Key_F10)}).runs([this] { fullScreen_->showMenus(); }));
    // View > Tool Ring (Alt+D) and the tool option steps its rings run (ToolRing.h).
    toolRing_->registerCommands(m.reg, view);
    // View > Colour Ring (Alt+C): the foreground colour on the same compass (ColorRing.h).
    colorRing_->registerCommands(m.reg, view);
    // View > Layer Picker (Alt+A): the layers under the pointer, stepped through from the keyboard (LayerPicker.h).
    layerPicker_->registerCommands(m.reg, view);
    view->addSeparator();
    layersDock_->toggleViewAction()->setText(tr("&Layers Panel"));
    layersDock_->toggleViewAction()->setShortcut(QKeySequence("F7"));   // Photoshop's Window > Layers
    layersDock_->toggleViewAction()->setObjectName("window.layers");
    layersDock_->toggleViewAction()->installEventFilter(m.fieldGuard);
    adjustDock_->toggleViewAction()->setText(tr("&Adjustments Panel"));
    m.reg.adopt(view, layersDock_->toggleViewAction(), Spec("window.layers", layersDock_->toggleViewAction()->text(), {QKeySequence("F7")}).command);
    pathsDock_->toggleViewAction()->setText(tr("&Paths Panel"));
    m.reg.adopt(view, pathsDock_->toggleViewAction(), Spec("window.paths", pathsDock_->toggleViewAction()->text()).command);
    m.reg.adopt(view, adjustDock_->toggleViewAction(), Spec("window.adjustments", adjustDock_->toggleViewAction()->text()).command);
    view->addSeparator();
    // A 32-bit document's view (docs/bit-depth.md, "32 bits"): not an undo step; the canvas shows it as it changes.
    previewOptionsAction_ = m.add(view, Spec("view.previewOptions32", tr("32-bit Pre&view Options…")).runs([this] {
        const View32 before = session_->view32();
        auto chosen = askPreviewOptions(this, before, [this](const View32& v) { session_->setView32(v); });
        session_->setView32(chosen ? *chosen : before);
    }, "view.exposure").when([this] {
        return session_->hasDocument() && session_->sampleType() == SampleType::F32 ? QString() : tr("For 32-bit documents");
    }));
    // Soft proofing (docs/color-management.md): Photoshop's Proof Setup, Proof Colors and Gamut Warning.
    QMenu* proofSetup = view->addMenu(tr("Proof Set&up"));
    m.add(proofSetup, Spec("view.proofSetup.custom", tr("Custom…")).runs([this] {
        color::showProofSetup(this, session_->hasDocument() ? std::optional<ColorMode>(session_->document()->colorMode) : std::nullopt);
    }, "color.settings"));
    proofSetup->addSeparator();
    // Photoshop's default proof: the press the Working CMYK describes.
    QAction* proofCmyk = m.add(proofSetup, Spec("view.proofSetup.workingCmyk", tr("Working CMYK")).runs([] { color::Settings s = color::settings(); s.proofProfile = QStringLiteral("working-cmyk"); color::setSettings(s); }, "color.settings"));
    proofCmyk->setCheckable(true);
    proofCmyk->setChecked(color::settings().proofProfile == QLatin1String("working-cmyk"));
    // A document the proof cannot be made for (its profile unreadable, say) greys them with the reason; once on, they
    // can always be turned off.
    auto proofReason = [this](bool color::Settings::* on) { return [this, on] { return session_->hasDocument() && !(color::settings().*on) ? color::proofUnavailable(*session_->document()) : QString(); }; };
    QAction* proof = m.add(view, Spec("view.proofColors", tr("Proof Colo&rs"), {QKeySequence("Ctrl+Y")}).when(proofReason(&color::Settings::proofColors)));
    connect(proof, &QAction::triggered, this, [](bool on) { color::Settings s = color::settings(); s.proofColors = on; color::setSettings(s); });
    QAction* gamut = m.add(view, Spec("view.gamutWarning", tr("Gamut Wa&rning"), {QKeySequence("Ctrl+Shift+Y")}).when(proofReason(&color::Settings::gamutWarning)));
    connect(gamut, &QAction::triggered, this, [](bool on) { color::Settings s = color::settings(); s.gamutWarning = on; color::setSettings(s); });
    for (QAction* a : {proof, gamut}) a->setCheckable(true);
    connect(color::notifier(), &color::Notifier::changed, this, [this, proof, gamut, proofCmyk] {
        proof->setChecked(color::settings().proofColors);
        gamut->setChecked(color::settings().gamutWarning);
        proofCmyk->setChecked(color::settings().proofProfile == QLatin1String("working-cmyk"));
        updateColorSwatches();
    });
    view->addSeparator();
    QAction* grid = m.add(view, Spec("view.pixelGrid", tr("Pixel &Grid")));
    connect(grid, &QAction::triggered, this, [this](bool on) { session_->showsPixelGrid = on; canvas_->update(); });
    grid->setCheckable(true);
    grid->setChecked(true);
    QAction* controls = m.add(view, Spec("view.transformControls", tr("Transform &Controls"), {QKeySequence("Ctrl+H")}));
    connect(controls, &QAction::triggered, this, [this](bool on) { session_->showsTransformControls = on; emit session_->transformChanged(); });
    controls->setCheckable(true);
    controls->setChecked(true);
}

void MainWindow::buildWindowMenu(MenuContext& m) {
    // Photoshop's Window menu: every panel.
    QMenu* window = menuBar()->addMenu(tr("&Window"));
    actionsDock_->toggleViewAction()->setText(tr("&Actions"));
    actionsDock_->toggleViewAction()->setShortcut(QKeySequence("Alt+F9"));
    m.reg.adopt(window, actionsDock_->toggleViewAction(), Spec("window.actions", actionsDock_->toggleViewAction()->text(), {QKeySequence("Alt+F9")}).command);
    window->addAction(adjustDock_->toggleViewAction());
    window->addAction(layersDock_->toggleViewAction());
    channelsDock_->toggleViewAction()->setText(tr("&Channels"));
    m.reg.adopt(window, channelsDock_->toggleViewAction(), Spec("window.channels", channelsDock_->toggleViewAction()->text()).command);
    histogramDock_->toggleViewAction()->setText(tr("&Histogram"));
    m.reg.adopt(window, histogramDock_->toggleViewAction(), Spec("window.histogram", histogramDock_->toggleViewAction()->text()).command);
    window->addAction(pathsDock_->toggleViewAction());
    historyDock_->toggleViewAction()->setText(tr("Histo&ry"));
    m.reg.adopt(window, historyDock_->toggleViewAction(), Spec("window.history", historyDock_->toggleViewAction()->text()).command);
    timelineDock_->toggleViewAction()->setText(tr("&Timeline"));
    m.reg.adopt(window, timelineDock_->toggleViewAction(), Spec("window.timeline", timelineDock_->toggleViewAction()->text()).command);
}

void MainWindow::buildHelpMenu(MenuContext& m) {
    QMenu* help = menuBar()->addMenu(tr("&Help"));
    m.add(help, Spec("help.welcome", tr("&Welcome to NekoPhoto")).runs([this] { showWelcome(); }));
    m.add(help, Spec("help.about", tr("&About NekoPhoto")).runs([this] {
        QMessageBox::about(this, tr("About NekoPhoto"), tr("<b>NekoPhoto</b> %3<br>A layered photo editor and painting app for Linux. "
            "It began as a Linux port of <a href=\"https://github.com/robbietilton/Compositor\">Compositor</a> for macOS, and still opens its projects.<br><br>"
            "Qt %1 &middot; project format version %2<br><br>"
            "Free software under the GNU General Public License, version 3 or later, with ABSOLUTELY NO WARRANTY. "
            "Compositor's own code is MIT licensed by Wonder Assembly LLC; the licences of the bundled components "
            "are in THIRD-PARTY-NOTICES.md, installed with the program.").arg(QT_VERSION_STR).arg(projectFormatVersion).arg(QApplication::applicationVersion()));
    }));
}

} // namespace app
