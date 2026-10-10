// The main window's menus, tool rail and colour swatches.
#include "Theme.h"
#include "ExportAs.h"
#include "Automation.h"
#include "CommandPalette.h"
#include "CommandRegistry.h"
#include "compositor/channels.h"
#include "compositor/vectorlayer.h"
#include <QRegularExpression>
#include "ContentFillDialog.h"
#include "ChannelDialogs.h"
#include "Names.h"
#include "ContentAwareScaleDialog.h"
#include "MainWindow.h"
#include "ModelStore.h"
#include <set>
#include <algorithm>
#include <QDialog>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QLabel>
#include <QComboBox>
#include <QSpinBox>
#include <QCheckBox>
#include <QFormLayout>
#include <QDialogButtonBox>
#include "LayerStyleDialog.h"
#include "compositor/raw.h"
#include "WarpDialog.h"
#include "CanvasFrame.h"
#include "Dialogs.h"
#include "HdrDialogs.h"
#include "ImageConvert.h"
#include "CameraRawDialog.h"
#include "FilterDialog.h"
#include "GridFilters.h"
#include "compositor/smartfilter.h"
#include <random>
#include "MoshDialog.h"
#include "GmicDialog.h"
#include "ColorDialogs.h"
#include "ColorManagement.h"
#include "ColorSwatches.h"
#include "PreferencesDialog.h"
#include "BrushImporter.h"
#include "PresetLibrary.h"
#include "ActionLibrary.h"
#include "ViewOptions.h"
#include "FullScreen.h"
#include <QSignalBlocker>
#include <QJsonObject>
#include <QShortcutEvent>
#include <functional>
#include "compositor/project.h"
#include <QActionGroup>
#include <QApplication>
#include <QColorDialog>
#include <QDockWidget>
#include <QStatusBar>
#include <QInputDialog>
#include <QLineEdit>
#include <QMenuBar>
#include <QMessageBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QSettings>
#include <QToolBar>

using namespace compositor;

namespace app {

namespace {

/// Sees an action's shortcut before the action does, and runs `f` in its place (the action and the keys pressed).
class ShortcutWatch : public QObject {
public:
    ShortcutWatch(QObject* parent, std::function<void(QAction*, const QKeySequence&)> f) : QObject(parent), f_(std::move(f)) {}
protected:
    bool eventFilter(QObject* watched, QEvent* e) override {
        if (e->type() != QEvent::Shortcut) return false;
        auto* shortcut = static_cast<QShortcutEvent*>(e);
        auto* action = qobject_cast<QAction*>(watched);
        if (!action || shortcut->isAmbiguous() || !action->isEnabled()) return false;
        f_(action, shortcut->key());
        return true;
    }
private:
    std::function<void(QAction*, const QKeySequence&)> f_;
};

/// Whether the keyboard is in a text field (a name, a number), where window shortcuts that edit the document or
/// rearrange the window should leave the keys alone.
bool typingInField() {
    QWidget* focus = QApplication::focusWidget();
    return focus && (focus->inherits("QLineEdit") || focus->inherits("QAbstractSpinBox") || focus->inherits("QTextEdit") || focus->inherits("QPlainTextEdit"));
}

/// A command for the registry, written as a row: its id, label and keys, then what it needs and how it runs.
struct Spec {
    Command command;
    Spec(const QString& id, const QString& label, QList<QKeySequence> keys = {}) {
        command.id = id;
        command.label = label;
        command.shortcuts = std::move(keys);
    }
    Spec(const char* id, const QString& label, QList<QKeySequence> keys = {}) : Spec(QString::fromLatin1(id), label, std::move(keys)) {}
    /// It needs a document; `feature` is the supports() feature it is (none: 8-bit RGB only).
    Spec& document(const char* feature = nullptr) {
        command.needsDocument = true;
        if (feature) command.feature = QString::fromUtf8(feature);
        return *this;
    }
    /// It runs `run` (a dialog, or a choice made first), which ends in automation method `method` when given.
    Spec& runs(std::function<void()> run, const char* method = nullptr) {
        command.run = std::move(run);
        if (method) command.method = QString::fromLatin1(method);
        return *this;
    }
    /// It is the request `method` with what `params` gives (nothing: nothing to do now); an error is titled `title`.
    Spec& request(const char* method, std::function<std::optional<QJsonObject>()> params, const QString& title = {}) {
        command.method = QString::fromLatin1(method);
        command.params = std::move(params);
        command.errorTitle = title;
        return *this;
    }
    /// Why it cannot run now (empty when it can), beyond the document and its mode.
    Spec& when(std::function<QString()> unavailable) {
        command.unavailable = std::move(unavailable);
        return *this;
    }
};

/// A command id's last part from the interface's English words: "Gaussian Blur" is gaussianBlur, "soft-glitch" softGlitch.
QString commandSlug(const QString& words) {
    static const QRegularExpression separators(QStringLiteral("[^A-Za-z0-9]+"));
    QString out;
    for (const QString& word : words.split(separators, Qt::SkipEmptyParts))
        out += out.isEmpty() ? word.toLower() : word.left(1).toUpper() + word.mid(1).toLower();
    return out;
}

} // namespace

void MainWindow::buildToolRail() {
    auto* rail = new QToolBar(tr("Tools"), this);
    toolRail_ = rail;
    rail->setObjectName("toolRail");
    rail->setOrientation(Qt::Vertical);
    rail->setMovable(false);
    rail->setIconSize(QSize(22, 22));
    rail->setToolButtonStyle(Qt::ToolButtonIconOnly);
    auto* group = new QActionGroup(this);
    group->setExclusive(true);
    // Each tool is a command (tool.<name>) with its letter as its default key, which Edit > Keyboard Shortcuts can
    // change; a tool whose letter steps its group (Quick Select: Shift+W) has none of its own and names the switch,
    // whose key its tooltip shows (refreshToolTips).
    auto tool = [&](Tool t, const char* id, const QString& label, const QString& iconName, const char* key, const char* switchId = nullptr) {
        QAction* a = rail->addAction(toolIcon(iconName), label);
        a->setCheckable(true);
        a->setShortcutContext(Qt::WindowShortcut);
        a->setProperty("toolLabel", label);
        if (switchId) a->setProperty("toolSwitch", QString::fromLatin1(switchId));
        group->addAction(a);
        commands_->adopt(nullptr, a, Spec(id, plainText(label).section(QStringLiteral(" ("), 0, 0), key ? QList<QKeySequence>{QKeySequence(QString::fromLatin1(key))} : QList<QKeySequence>{}).command);
        connect(a, &QAction::triggered, this, [this, t] { session_->selectTool(t); if (t == Tool::Brush) { session_->brushErase = false; emit session_->toolChanged(); } canvas_->setFocus(); groupLast_[toolGroupKey(t)] = t; });
        toolActions_[t] = a;
        return a;
    };
    tool(Tool::Move, "tool.move", tr("Move / Transform"), "move", "V")->setChecked(true);
    tool(Tool::Marquee, "tool.marquee", tr("Marquee"), "square-dashed", "M");
    tool(Tool::Lasso, "tool.lasso", tr("Lasso"), "lasso", "L");
    tool(Tool::Wand, "tool.magicWand", tr("Magic Wand"), "wand-sparkles", "W");
    tool(Tool::Scribble, "tool.quickSelect", tr("Quick Select"), "scribble", nullptr, "tool.next.wand");   // Photoshop's W group (Shift+W steps through it)
    tool(Tool::Crop, "tool.crop", tr("Crop"), "crop", "C");
    tool(Tool::Slice, "tool.slice", tr("Slice (drag a slice; drag inside to move it, an edge to resize)"), "slice", nullptr, "tool.next.crop");   // Photoshop's C group
    tool(Tool::Artboard, "tool.artboard", tr("Artboard (drag a new artboard; drag inside to move it with its contents, an edge to resize)"), "frame", nullptr, "tool.next.move");   // Photoshop's V group
    rail->addSeparator();
    tool(Tool::Brush, "tool.brush", tr("Brush"), "paintbrush", "B");
    eraserAction_ = rail->addAction(toolIcon("eraser"), tr("Eraser"));
    eraserAction_->setProperty("toolLabel", tr("Eraser"));
    eraserAction_->setCheckable(true);
    group->addAction(eraserAction_);
    commands_->adopt(nullptr, eraserAction_, Spec("tool.eraser", tr("Eraser"), {QKeySequence("E")}).command);
    connect(eraserAction_, &QAction::triggered, this, [this] { session_->brushErase = true; session_->selectTool(Tool::Brush); emit session_->toolChanged(); canvas_->setFocus(); });
    tool(Tool::SpotHealing, "tool.spotHealing", tr("Spot Healing Brush"), "bandage", "J");
    tool(Tool::CloneStamp, "tool.cloneStamp", tr("Clone Stamp (Alt-click sets the source)"), "stamp", "S");
    tool(Tool::Smudge, "tool.blur", tr("Liquify / Blur / Smudge"), "droplet", "R");
    tool(Tool::Dodge, "tool.dodge", tr("Dodge / Burn / Sponge"), "lollipop", "O");
    tool(Tool::Gradient, "tool.gradient", tr("Gradient"), "blend", "G");
    tool(Tool::PaintBucket, "tool.paintBucket", tr("Paint Bucket"), "paint-bucket", nullptr, "tool.next.gradient");   // Photoshop's G group
    tool(Tool::Pen, "tool.pen", tr("Pen (click corners, drag curves; click the first point or Enter to finish)"), "pen-tool", "P");
    tool(Tool::DirectSelect, "tool.directSelection", tr("Direct Selection (drag points, handles or a whole path; Alt-click converts a point)"), "mouse-pointer-2", "A");
    tool(Tool::Shape, "tool.shape", tr("Shape (Shift-U steps through Rectangle, Ellipse, Polygon, Line, Custom)"), "shapes", "U");
    tool(Tool::Text, "tool.type", tr("Text"), "type", "T");
    tool(Tool::Eyedropper, "tool.eyedropper", tr("Eyedropper"), "pipette", "I");
    rail->addSeparator();
    tool(Tool::Hand, "tool.hand", tr("Hand"), "hand", "H");
    tool(Tool::Zoom, "tool.zoom", tr("Zoom"), "zoom-in", "Z");
    rail->addSeparator();
    swatches_ = new ColorSwatches;
    // Photoshop's spring-loaded tool keys: a tool's letter held a moment (or held while the tool is used) goes back
    // to the tool before it when let go; a tap keeps the new tool. Held keys do not repeat the switch.
    // Shift+letter (the next tool or kind of the letter's group) springs back the same way, to the tool and kind before.
    struct ToolState {
        Tool tool; bool erase; MarqueeKind marquee; LassoKind lasso; VectorShapeKind shape; int spotHealing, spotHealingType;
        compositor::ToningKind toning; BlurToolMode blur;
        bool operator==(const ToolState&) const = default;
    };
    auto toolState = [this] {
        return ToolState{session_->tool(), session_->brushErase, session_->marqueeKind, session_->lassoKind, session_->shapeTool.kind,
                         session_->spotHealingMode, spotHealingType_, session_->toning.kind, session_->blurMode};
    };
    auto* springs = new ShortcutWatch(this, [this, toolState](QAction* a, const QKeySequence& keys) {
        const ToolState before = toolState();
        a->trigger();
        if (keys.isEmpty() || toolState() == before) return;
        canvas_->armToolSpring(keys[0].key(), [this, before] {
            if (session_->lassoKind != before.lasso) canvas_->cancelLasso();
            if (session_->shapeTool.kind != before.shape) session_->cancelShape();
            session_->marqueeKind = before.marquee;
            session_->lassoKind = before.lasso;
            session_->shapeTool.kind = before.shape;
            session_->spotHealingMode = before.spotHealing;
            spotHealingType_ = before.spotHealingType;
            session_->toning.kind = before.toning;
            session_->blurMode = before.blur;
            if (before.tool == Tool::Brush && before.erase) {
                eraserAction_->trigger();
            } else if (QAction* back = toolActions_.value(before.tool)) {
                back->trigger();
            } else {
                session_->selectTool(before.tool);
            }
            emit session_->toolChanged();
        });
    });
    for (QAction* a : rail->actions()) if (a->isCheckable()) { a->setAutoRepeat(false); a->installEventFilter(springs); }
    connect(swatches_, &ColorSwatches::foregroundClicked, this, [this] { chooseColor(false); });
    connect(swatches_, &ColorSwatches::backgroundClicked, this, [this] { chooseColor(true); });
    // X and D run colors.set (a colour finer than the request can carry is set directly, and recorded as before).
    QAction* swap = commands_->add(nullptr, Spec("colors.swap", tr("Swap colours"), {QKeySequence("X")}).runs([this] {
        // With the Crop tool, X turns the crop box (Photoshop's Swap Height and Width).
        if (session_->tool() == Tool::Crop && canvas_->cropRect()) { canvas_->swapCropOrientation(); return; }
        setColors(session_->backgroundColor, session_->foregroundColor);
    }, "colors.set").command);
    swap->setObjectName("command.colors.swap");
    connect(swatches_, &ColorSwatches::swapRequested, swap, &QAction::trigger);
    QAction* defaults = commands_->add(nullptr, Spec("colors.default", tr("Default colours"), {QKeySequence("D")})
                                                    .runs([this] { setColors(Qt::black, Qt::white); }, "colors.set").command);
    defaults->setObjectName("command.colors.default");
    connect(swatches_, &ColorSwatches::defaultsRequested, defaults, &QAction::trigger);
    rail->addWidget(swatches_);
    addToolBar(Qt::LeftToolBarArea, rail);
    // Shift-letter switches a tool's kind without leaving it.
    // Each springs back when held, as the plain letters do, and does not repeat while held.
    // Each is a command (tool.next.<group>) whose key Edit > Keyboard Shortcuts can change.
    auto kindKey = [this, springs](const char* id, const QString& label, const char* key, auto slot) {
        auto* a = new QAction(label, this);
        a->setAutoRepeat(false);
        a->installEventFilter(springs);
        connect(a, &QAction::triggered, this, slot);
        addAction(a);
        commands_->adopt(nullptr, a, Spec(id, label, {QKeySequence(QString::fromLatin1(key))}).command);
    };
    kindKey("tool.next.marquee", tr("Next in the Marquee group (Rectangle, Ellipse)"), "Shift+M", [this] { session_->marqueeKind = session_->marqueeKind == MarqueeKind::Rectangle ? MarqueeKind::Ellipse : MarqueeKind::Rectangle; session_->selectTool(Tool::Marquee); emit session_->toolChanged(); });
    kindKey("tool.next.lasso", tr("Next in the Lasso group (Freehand, Polygonal)"), "Shift+L", [this] { session_->lassoKind = session_->lassoKind == LassoKind::Freehand ? LassoKind::Polygonal : LassoKind::Freehand; canvas_->cancelLasso(); session_->selectTool(Tool::Lasso); emit session_->toolChanged(); });
    kindKey("tool.next.shape", tr("Next in the Shape group (Rectangle, Ellipse, Polygon, Line, Custom)"), "Shift+U", [this] { session_->selectTool(Tool::Shape); session_->toggleShapeKind(); });
    // Photoshop's Shift+letter tool switch: the next tool of the letter's group (from another tool, the one after
    // the group's last used). Groups of one tool select it; tools that hold a group as kinds step the kind, as
    // Shift+M, Shift+L and Shift+U above do.
    auto cycleTools = [this](std::vector<Tool> tools) {
        const Tool current = session_->tool();
        auto at = std::find(tools.begin(), tools.end(), current);
        if (at == tools.end()) at = std::find(tools.begin(), tools.end(), groupLast_.value(toolGroupKey(tools.front()), tools.front()));
        const Tool next = at == tools.end() ? tools.front() : *(++at == tools.end() ? tools.begin() : at);
        toolActions_[next]->trigger();
    };
    kindKey("tool.next.move", tr("Next in the Move group (Move, Artboard)"), "Shift+V", [cycleTools] { cycleTools({Tool::Move, Tool::Artboard}); });
    kindKey("tool.next.wand", tr("Next in the Magic Wand group (Magic Wand, Quick Select)"), "Shift+W", [cycleTools] { cycleTools({Tool::Wand, Tool::Scribble}); });
    kindKey("tool.next.crop", tr("Next in the Crop group (Crop, Slice)"), "Shift+C", [cycleTools] { cycleTools({Tool::Crop, Tool::Slice}); });
    kindKey("tool.next.gradient", tr("Next in the Gradient group (Gradient, Paint Bucket)"), "Shift+G", [cycleTools] { cycleTools({Tool::Gradient, Tool::PaintBucket}); });
    kindKey("tool.next.pen", tr("Next in the Pen group"), "Shift+P", [this] { toolActions_[Tool::Pen]->trigger(); });
    kindKey("tool.next.type", tr("Next in the Type group"), "Shift+T", [this] { toolActions_[Tool::Text]->trigger(); });
    // The J group: Spot Healing (its three types), Healing Brush, Patch, Content-Aware Move.
    kindKey("tool.next.healing", tr("Next in the Healing group (Spot Healing, Healing Brush, Patch, Content-Aware Move)"), "Shift+J", [this] {
        int& mode = session_->spotHealingMode;
        if (mode <= 2) { spotHealingType_ = mode; mode = 3; }
        else mode = mode >= 5 ? spotHealingType_ : mode + 1;
        toolActions_[Tool::SpotHealing]->trigger();
        emit session_->toolChanged();
    });
    // The O group: Dodge, Burn, Sponge.
    kindKey("tool.next.toning", tr("Next in the Dodge group (Dodge, Burn, Sponge)"), "Shift+O", [this] {
        session_->toning.kind = compositor::ToningKind((int(session_->toning.kind) + 1) % 3);
        toolActions_[Tool::Dodge]->trigger();
        emit session_->toolChanged();
    });
    // The R tool's modes in the order of Photoshop's Blur, Sharpen, Smudge group, then Liquify.
    kindKey("tool.next.blur", tr("Next in the Blur group (Blur, Sharpen, Smudge, Liquify)"), "Shift+R", [this] {
        static const BlurToolMode order[] = {BlurToolMode::Blur, BlurToolMode::Sharpen, BlurToolMode::Smudge, BlurToolMode::Liquify};
        const auto at = std::find(std::begin(order), std::end(order), session_->blurMode);
        session_->blurMode = at == std::end(order) || at + 1 == std::end(order) ? order[0] : *(at + 1);
        toolActions_[Tool::Smudge]->trigger();
        emit session_->toolChanged();
    });
}

void MainWindow::updateColorSwatches() {
    // Document values, shown through the monitor profile (the identity when none is known).
    const compositor::ColorProfile profile = session_->hasDocument() ? session_->document()->profile : compositor::ColorProfile{};
    swatches_->setColors(color::displayColor(session_->foregroundColor, profile), color::displayColor(session_->backgroundColor, profile));
}

void MainWindow::chooseColor(bool background) {
    QColor current = background ? session_->backgroundColor : session_->foregroundColor;
    QColor c = QColorDialog::getColor(current, this, background ? tr("Background Colour") : tr("Foreground Colour"));
    if (!c.isValid()) return;
    if (background) setColors(session_->foregroundColor, c, false, true);
    else setColors(c, session_->backgroundColor, true, false);
}

QString MainWindow::colorRequest(const QColor& color) {
    // #rrggbb when it says the colour exactly, else #rrrrggggbbbb (16 bits per channel, as QColor reads it back).
    if (QColor(color.name()) == color) return color.name();
    const QRgba64 deep = color.rgba64();
    const QString wide = QStringLiteral("#%1%2%3").arg(deep.red(), 4, 16, QLatin1Char('0')).arg(deep.green(), 4, 16, QLatin1Char('0')).arg(deep.blue(), 4, 16, QLatin1Char('0'));
    return deep.alpha() == 0xffff && QColor(wide) == color ? wide : QString();
}

void MainWindow::setColors(const QColor& foreground, const QColor& background, bool setForeground, bool setBackground) {
    // colors.set with the colours that change; one it cannot carry is set directly and recorded as the request was.
    QJsonObject params;
    const QString fg = colorRequest(foreground), bg = colorRequest(background);
    if (setForeground) params["foreground"] = fg.isEmpty() ? foreground.name() : fg;
    if (setBackground) params["background"] = bg.isEmpty() ? background.name() : bg;
    if ((!setForeground || !fg.isEmpty()) && (!setBackground || !bg.isEmpty()) && session_->commandsRouted()) {
        runCommand("colors.set", params, tr("Colours"));
    } else {
        if (setForeground) session_->foregroundColor = foreground;
        if (setBackground) session_->backgroundColor = background;
        recordAction("colors.set", params);
    }
    updateColorSwatches();
}

void MainWindow::buildMenus() {
    // The menus are the command registry's (CommandRegistry.h): each item is a command with an id, its label, its
    // keys and what it needs, added to its menu in order. A document command names the supports() feature it is;
    // without one it is 8-bit RGB only (greyed in a 16-bit document).
    CommandRegistry& reg = *commands_;
    auto add = [&reg](QMenu* menu, Spec spec) { return reg.add(menu, std::move(spec.command)); };
    // Actions the canvas's context menu shows again (MainWindowCanvasMenu.cpp), so their shortcuts and enabled state match.
    auto nameAction = [this](const char* key, QAction* a) { named_[QString::fromLatin1(key)] = a; return a; };
    // F7, F12 and Ctrl+Alt+Z leave the keys alone while a text field has them.
    auto* fieldGuard = new ShortcutWatch(this, [](QAction* a, const QKeySequence&) { if (!typingInField()) a->trigger(); });
    auto hasLayer = [this] { return session_->activeLayerId() ? QString() : tr("No active layer"); };

    QMenu* file = menuBar()->addMenu(tr("&File"));
    add(file, Spec("file.new", tr("&New…"), {QKeySequence::New}).runs([this] { newDocument(); }, "document.new"));
    add(file, Spec("file.open", tr("&Open…"), {QKeySequence::Open}).runs([this] { openFiles(); }, "document.open"));
    add(file, Spec("file.openProject", tr("Open Project Folder…")).runs([this] { openProject(); }, "document.open"));   // a .comp folder; .nekophoto files open with Open
    recentMenu_ = file->addMenu(tr("Open &Recent"));
    recentMenu_->setProperty("commandsDynamic", true);   // the recent files, listed as they are
    add(file, Spec("file.import", tr("Import &File…"), {QKeySequence("Ctrl+Shift+O")}).runs([this] { importFiles(); }, "document.import"));
    add(file, Spec("file.importBrushes", tr("Import &Brushes…")).runs([this] { importBrushesInteractively(this, session_); }, "brush.import"));
    add(file, Spec("file.importPresets", tr("I&mport Presets…")).runs([this] { importPresetsInteractively(this, session_); }, "presets.import"));
    add(file, Spec("file.placeEmbedded", tr("Place &Embedded…")).document("edit.smartObject").runs([this] {
        const QString path = QFileDialog::getOpenFileName(this, tr("Place Embedded"), QSettings().value("lastDir").toString(),
                                                          tr("Images, Photoshop and Affinity documents (*.psd *.psb *.afphoto *.afdesign *.afpub *.af *.png *.jpg *.jpeg *.tif *.tiff *.webp *.bmp *.gif %1)").arg(compositor::rawSupported() ? QStringLiteral("*.cr2 *.cr3 *.crw *.nef *.nrw *.arw *.srf *.sr2 *.raf *.orf *.rw2 *.rwl *.pef *.dng *.3fr *.iiq *.erf *.kdc *.dcr *.mrw *.srw *.x3f") : QString()));
        if (path.isEmpty()) return;
        QSettings().setValue("lastDir", QFileInfo(path).path());
        runCommand("smartObject.place", {{"path", path}}, tr("Couldn’t place %1").arg(QFileInfo(path).fileName()));
    }, "smartObject.place"));
    file->addSeparator();
    add(file, Spec("file.save", tr("&Save"), {QKeySequence::Save}).document("document.save").runs([this] { save(false); }, "document.save"));
    add(file, Spec("file.saveAs", tr("Save &As…"), {QKeySequence::SaveAs}).document("document.save").runs([this] { save(true); }, "document.save"));
    // File > Revert: the file it was opened from or last saved to, read again as one undo step (no question: Undo
    // brings the document back, as in Photoshop CC). Greyed until the document has changed.
    revertAction_ = add(file, Spec("file.revert", tr("Re&vert"), {QKeySequence("F12")}).request("document.revert", [] { return QJsonObject{}; }, tr("Revert")).when([this] {
        if (!session_->hasDocument()) return tr("No document is open");
        return session_->canRevert() ? QString() : tr("Nothing to revert to: the document has not changed since it was opened or saved");
    }));
    revertAction_->setObjectName("file.revert");
    revertAction_->installEventFilter(fieldGuard);
    file->addSeparator();
    // File > Export, as Photoshop's: Quick Export in the format Preferences choose, Export As for the flat formats
    // (PNG, JPEG, GIF, WebP, TIFF, TGA) with a size and a preview, then the formats with dialogs of their own.
    QMenu* exportMenu = file->addMenu(tr("E&xport"));
    QAction* quickExportAction = add(exportMenu, Spec("file.export.quick", tr("Quick Export")).document("export.png").runs([this] { quickExport(false); }, "document.export"));
    quickExportAction->setObjectName("export.quick");
    // Ctrl+Alt+Shift+W is Photoshop's Export As; Ctrl+Alt+Shift+S (its Save for Web) opened Export JPEG here before.
    QAction* exportAsAction = add(exportMenu, Spec("file.export.as", tr("Export &As…"), {QKeySequence("Ctrl+Alt+Shift+W"), QKeySequence("Ctrl+Alt+Shift+S")})
                                                  .document("export.png").runs([this] { exportAs(false); }, "document.export"));
    exportAsAction->setObjectName("export.as");
    connect(exportMenu, &QMenu::aboutToShow, this, [quickExportAction] { quickExportAction->setText(MainWindow::tr("Quick Export as %1").arg(exportas::formatLabel(exportas::quickExportFormat()))); });
    quickExportAction->setText(tr("Quick Export as %1").arg(exportas::formatLabel(exportas::quickExportFormat())));
    exportMenu->addSeparator();
    add(exportMenu, Spec("file.export.psd", tr("Export as Photoshop &Document (PSD)…")).document("export.psd").runs([this] { exportPsd(); }));
    add(exportMenu, Spec("file.export.svg", tr("Export S&VG…")).document("export.svg").runs([this] { exportSvg(); }));
    add(exportMenu, Spec("file.export.ico", tr("Export &Icon (ICO)…")).document("export.ico").runs([this] { exportIco(); }));
    add(exportMenu, Spec("file.export.gif", tr("E&xport Animated GIF…")).document("export.gif").runs([this] { exportGif(); }));
    exportMenu->addSeparator();
    add(exportMenu, Spec("file.export.artboards", tr("Export Artboards to Files…")).document("export.artboards").runs([this] { exportBoxes(false); }));
    add(exportMenu, Spec("file.export.slices", tr("Export S&lices…")).document("export.slices").runs([this] { exportBoxes(true); }));
    file->addSeparator();
    QMenu* automate = file->addMenu(tr("A&utomate"));
    add(automate, Spec("file.automate.batch", tr("&Batch…")).runs([this] { showBatchDialog(); }, "actions.batch"));
    file->addSeparator();
    add(file, Spec("file.closeTab", tr("&Close Tab"), {QKeySequence::Close}).runs([this] { closeTab(current_); }));
    // No default key: Ctrl+T (the platform's New Tab) is Photoshop's Free Transform.
    add(file, Spec("file.newTab", tr("New Tab")).runs([this] { addTab(false); }));
    add(file, Spec("file.nextTab", tr("Next Tab"), {QKeySequence("Ctrl+Tab")}).runs([this] { if (tabs_.size() > 1) switchTo((current_ + 1) % int(tabs_.size())); }));
    add(file, Spec("file.previousTab", tr("Previous Tab"), {QKeySequence("Ctrl+Shift+Tab")}).runs([this] { if (tabs_.size() > 1) switchTo((current_ + int(tabs_.size()) - 1) % int(tabs_.size())); }));
    add(file, Spec("file.quit", tr("&Quit"), {QKeySequence::Quit}).runs([this] { close(); }));

    QMenu* edit = menuBar()->addMenu(tr("&Edit"));
    // Undo and Redo run history.undo and history.redo (history steps are not recorded in actions).
    undoAction_ = add(edit, Spec("edit.undo", tr("&Undo"), {QKeySequence::Undo}).request("history.undo", [] { return QJsonObject{}; }, tr("Undo"))
                                .when([this] { return session_->canUndo() ? QString() : tr("Nothing to undo"); }));
    undoAction_->setObjectName("command.history.undo");
    redoAction_ = add(edit, Spec("edit.redo", tr("&Redo"), {QKeySequence("Ctrl+Shift+Z")}).request("history.redo", [] { return QJsonObject{}; }, tr("Redo"))
                                .when([this] { return session_->canRedo() ? QString() : tr("Nothing to redo"); }));
    redoAction_->setObjectName("command.history.redo");
    // Photoshop CC's Toggle Last State: undoes the last step, or redoes the one just undone.
    toggleStateAction_ = add(edit, Spec("edit.toggleLastState", tr("Toggle &Last State"), {QKeySequence("Ctrl+Alt+Z")}).runs([this] { session_->toggleLastState(); })
                                       .when([this] { return session_->canUndo() || session_->canRedo() ? QString() : tr("Nothing to undo or redo"); }));
    toggleStateAction_->setObjectName("edit.toggleLastState");
    toggleStateAction_->installEventFilter(fieldGuard);
    edit->addSeparator();
    // The pixel clipboard runs pixels.cut, pixels.copy, pixels.copyMerged and pixels.paste (layers.copy and
    // layers.paste for whole layers); with nothing to copy or paste the items do nothing, as before.
    nameAction("edit.cut", add(edit, Spec("edit.cut", tr("Cu&t"), {QKeySequence::Cut}).document("edit.clipboard").runs([this] {
        if (!session_->document()->selection || !session_->canCopyPixels()) return;
        // A smart object: copied, then the question about rasterizing it (pixels.cut refuses one).
        if (session_->smartObjectBlocksPixels()) { session_->cutSelection(); return; }
        runCommand("pixels.cut", {}, tr("Cut"));
    }, "pixels.cut")))->setObjectName("command.pixels.cut");
    nameAction("edit.copy", add(edit, Spec("edit.copy", tr("&Copy"), {QKeySequence::Copy}).document("edit.clipboard").runs([this] {
        // As Photoshop: with no selection, the selected layers themselves (pasted whole in any document).
        if (!session_->document()->selection && !session_->selectedLayerIds().empty()) { runCommand("layers.copy", {}, tr("Copy")); return; }
        if (session_->canCopyPixels()) runCommand("pixels.copy", {}, tr("Copy"));
    }, "pixels.copy")))->setObjectName("command.pixels.copy");
    add(edit, Spec("edit.copyMerged", tr("Copy &Merged"), {QKeySequence("Ctrl+Shift+C")}).document("edit.clipboard").runs([this] {
        if (session_->canEditLayers() && !(session_->document()->selection && session_->document()->selection->isEmpty())) runCommand("pixels.copyMerged", {}, tr("Copy Merged"));
    }, "pixels.copyMerged"))->setObjectName("command.pixels.copyMerged");
    nameAction("edit.paste", add(edit, Spec("edit.paste", tr("&Paste"), {QKeySequence::Paste}).document("edit.clipboard").runs([this] {
        if (EditorSession::hasLayerClipboard()) { if (session_->canPaste()) runCommand("layers.paste", {}, tr("Paste")); return; }
        if (session_->hasPixelsToPaste()) runCommand("pixels.paste", {}, tr("Paste"));
    }, "pixels.paste")))->setObjectName("command.pixels.paste");
    edit->addSeparator();
    // Free Transform is interactive: Ctrl+T invokes it, the canvas updates it, and Enter or Apply commits it through
    // the command layers.setTransform (EditorSession::commitTransformCommand).
    nameAction("edit.freeTransform", add(edit, Spec("edit.freeTransform", tr("&Free Transform"), {QKeySequence("Ctrl+T")}).document("layers.transform")
                                                   .runs([this] { session_->transformCommand(); }, "layers.setTransform")))->setObjectName("command.transform");
    // Warp's OK runs layers.warp; the Warp Cage's Enter runs layers.setCage (EditorSession::commitWarpCageCommand).
    nameAction("edit.warp", add(edit, Spec("edit.warp", tr("&Warp…")).document("edit.distort").runs([this] { WarpDialog(session_, this).exec(); }, "layers.warp").when(hasLayer)));
    add(edit, Spec("edit.warpCage", tr("Warp Ca&ge")).document("edit.distort").runs([this] {
        QString error;
        if (!session_->beginWarpCage(&error)) showError(tr("Warp Cage"), error);
        else statusBar()->showMessage(tr("Drag the cage's points; Enter applies, Esc cancels."), 8000);
    }, "layers.setCage"));
    nameAction("edit.fillForeground", add(edit, Spec("edit.fillForeground", tr("Fill with Foreground"), {QKeySequence("Alt+Backspace")}).document("edit.fill")
                                                    .runs([this] { fillWith(session_->foregroundColor); }, "pixels.fill")))->setObjectName("command.pixels.fill");
    nameAction("edit.fillBackground", add(edit, Spec("edit.fillBackground", tr("Fill with Background"), {QKeySequence("Ctrl+Backspace")}).document("edit.fill")
                                                    .runs([this] { fillWith(session_->backgroundColor); }, "pixels.fill")));
    QAction* clear = add(edit, Spec("edit.clear", tr("Clear"), {QKeySequence(Qt::Key_Delete), QKeySequence(Qt::Key_Backspace)}).document("layers.structure").runs([this] {
        if (session_->document() && session_->document()->selection) runCommand("pixels.clear", {}, tr("Clear"));
        else deleteLayersCommand();
    }, "pixels.clear"));
    clear->setObjectName("command.pixels.clear");
    nameAction("edit.contentAwareFill", add(edit, Spec("edit.contentAwareFill", tr("Content-Aware Fill…"), {QKeySequence("Shift+F5")}).document("edit.fill").runs([this] {
        if (!session_->canAdjustPixels() || !session_->document()->selection || !session_->document()->selection->coverage) { showError(tr("Content-Aware Fill"), tr("Select a visible image layer and an area to fill.")); return; }
        (new ContentFillDialog(session_, this))->show();
    }, "pixels.contentAwareFill")));
    add(edit, Spec("edit.contentAwareScale", tr("Content-Aware Scale…"), {QKeySequence("Ctrl+Alt+Shift+C")}).document("edit.contentAware")
                  .runs([this] { (new ContentAwareScaleDialog(session_, this))->show(); }, "pixels.contentAwareScale"));

    // Colour management (docs/color-management.md), as in Photoshop's Edit menu.
    edit->addSeparator();
    add(edit, Spec("edit.colorSettings", tr("Color Settings…"), {QKeySequence("Ctrl+Shift+K")}).runs([this] { color::showColorSettings(this); }, "color.settings"));
    auto profileKey = [](const compositor::ColorProfile& p) -> QString {
        if (p.empty()) return QStringLiteral("none");
        auto s = compositor::matchingWorkingSpace(p);
        return s && p == compositor::builtinProfile(*s) ? QString::fromLatin1(compositor::workingSpaceKey(*s)) : QString();
    };
    add(edit, Spec("edit.assignProfile", tr("Assign Profile…")).document("document.profile").runs([this, profileKey] {
        auto profile = color::askAssignProfile(this, session_->document()->profile);
        if (!profile) return;
        // A profile the method names runs as the command; an ICC file chosen from disk is assigned directly (and
        // not recorded: the method takes a key or a path, and the dialog keeps no path).
        if (const QString key = profileKey(*profile); !key.isEmpty()) runCommand("document.profile", {{"action", "assign"}, {"profile", key}}, tr("Assign Profile"));
        else session_->assignProfile(*profile);
    }, "document.profile"));
    add(edit, Spec("edit.convertToProfile", tr("Convert to Profile…")).document("document.profile").runs([this, profileKey] {
        auto choice = color::askConvertProfile(this, session_->document()->profile);
        if (!choice) return;
        const QString key = profileKey(choice->profile);
        QApplication::setOverrideCursor(themedCursor(Qt::WaitCursor));
        if (!key.isEmpty()) {
            runCommand("document.profile", {{"action", "convert"}, {"profile", key}, {"intent", QString::fromLatin1(compositor::renderingIntentKey(choice->options.intent))},
                                            {"blackPointCompensation", choice->options.blackPointCompensation}}, tr("Convert to Profile"));
            QApplication::restoreOverrideCursor();
            return;
        }
        QString error;
        const bool ok = session_->convertToProfile(choice->profile, choice->options, &error);
        QApplication::restoreOverrideCursor();
        if (!ok) showError(tr("Convert to Profile"), error);
    }, "document.profile"));

    edit->addSeparator();
    // Photoshop's Edit > Search (Ctrl+F): every command, tool and G'MIC filter by name.
    searchAction_ = add(edit, Spec("edit.search", tr("&Search…"), {QKeySequence("Ctrl+F")}).runs([this] { showCommandPalette(); }));
    searchAction_->setObjectName("edit.search");
    // Photoshop's Edit > Keyboard Shortcuts… (Alt+Shift+Ctrl+K): every command's, tool's and panel's keys.
    add(edit, Spec("edit.keyboardShortcuts", tr("&Keyboard Shortcuts…"), {QKeySequence("Ctrl+Alt+Shift+K")}).runs([this] { showKeyboardShortcuts(); }))
        ->setObjectName("edit.keyboardShortcuts");
    // Photoshop's Ctrl+K, and the platform's own Preferences key where it has one.
    QList<QKeySequence> preferenceKeys{QKeySequence("Ctrl+K")};
    for (const QKeySequence& key : QKeySequence::keyBindings(QKeySequence::Preferences)) if (!preferenceKeys.contains(key)) preferenceKeys << key;
    add(edit, Spec("edit.preferences", tr("Prefere&nces…"), preferenceKeys).runs([this] { showPreferences(); }));

    QMenu* image = menuBar()->addMenu(tr("&Image"));
    // Photoshop's Image > Mode: the document's bits per channel (docs/bit-depth.md).
    QMenu* mode = image->addMenu(tr("&Mode"));
    reg.adopt(nullptr, mode->menuAction(), Spec("image.mode", plainText(mode->title())).document("document.mode").command);
    // The colour modes (docs/color-modes.md): RGB, CMYK and Lab, as Photoshop lists them above the depths. 32 bits is
    // RGB only, as in Photoshop: CMYK Color and Lab Color are greyed in a 32-bit document, 32 Bits/Channel in a CMYK
    // or Lab one.
    auto rgbOnlyAt32 = [this] { return session_->sampleType() == SampleType::F32 ? session_->unavailableTip("mode.cmykLab") : QString(); };
    auto* colorModes = new QActionGroup(this);
    modeRgbAction_ = add(mode, Spec("image.mode.rgb", tr("&RGB Color")).document("document.mode").runs([this] { convertColorMode(ColorMode::RGB); }, "image.mode"));
    modeCmykAction_ = add(mode, Spec("image.mode.cmyk", tr("&CMYK Color")).document("document.mode").runs([this] { convertColorMode(ColorMode::CMYK); }, "image.mode").when(rgbOnlyAt32));
    modeLabAction_ = add(mode, Spec("image.mode.lab", tr("&Lab Color")).document("document.mode").runs([this] { convertColorMode(ColorMode::Lab); }, "image.mode").when(rgbOnlyAt32));
    for (QAction* a : {modeRgbAction_, modeCmykAction_, modeLabAction_}) { a->setCheckable(true); colorModes->addAction(a); }
    modeRgbAction_->setChecked(true);
    mode->addSeparator();
    auto* depths = new QActionGroup(this);
    mode8Action_ = add(mode, Spec("image.mode.bits8", tr("&8 Bits/Channel")).document("document.mode").runs([this] { convertMode(SampleType::U8); }, "image.mode"));
    mode16Action_ = add(mode, Spec("image.mode.bits16", tr("&16 Bits/Channel")).document("document.mode").runs([this] { convertMode(SampleType::U16); }, "image.mode"));
    mode32Action_ = add(mode, Spec("image.mode.bits32", tr("&32 Bits/Channel")).document("document.mode").runs([this] { convertMode(SampleType::F32); }, "image.mode").when([this] {
        return session_->colorMode() != ColorMode::RGB ? session_->unavailableTip("document.mode") : QString();
    }));
    for (QAction* a : {mode8Action_, mode16Action_, mode32Action_}) { a->setCheckable(true); depths->addAction(a); }
    mode8Action_->setChecked(true);
    image->addSeparator();
    add(image, Spec("image.canvasSize", tr("&Canvas Size…"), {QKeySequence("Ctrl+Alt+C")}).document("canvas.size").runs([this] {
        auto o = askCanvasSize(this, session_->document()->width, session_->document()->height);
        if (!o) return;
        runCommand("canvas.resize", {{"width", o->width}, {"height", o->height}, {"anchorX", o->anchorX}, {"anchorY", o->anchorY}}, tr("Canvas Size"));
    }, "canvas.resize"));
    add(image, Spec("image.imageSize", tr("&Image Size…"), {QKeySequence("Ctrl+Alt+I")}).document("edit.imageSize").runs([this] {
        auto o = askImageSize(this, session_->document()->width, session_->document()->height, session_->document()->resolution);
        if (!o) return;
        runCommand("image.resize", {{"width", o->width}, {"height", o->height}, {"resolution", o->resolution},
                                    {"sampling", o->sampling == 0 ? "nearest" : o->sampling == 1 ? "smooth" : "high"}}, tr("Image Size"));
    }, "image.resize"));
    add(image, Spec("image.trim", tr("&Trim…")).document("edit.crop").runs([this] {
        // Photoshop's dialog: what to trim by, and which sides.
        QDialog dialog(this);
        dialog.setWindowTitle(tr("Trim"));
        auto* layout = new QVBoxLayout(&dialog);
        auto* basedOn = new QComboBox;
        basedOn->addItems({tr("Transparent Pixels"), tr("Top Left Pixel Color"), tr("Bottom Right Pixel Color")});
        auto* form = new QFormLayout;
        form->addRow(tr("Based on"), basedOn);
        auto* tolerance = new QSpinBox;
        tolerance->setRange(0, 255);
        tolerance->setToolTip(tr("How far a pixel's channels may be from the corner's and still be trimmed"));
        form->addRow(tr("Tolerance"), tolerance);
        layout->addLayout(form);
        auto* sides = new QHBoxLayout;
        QCheckBox* side[4];
        const QString names[4] = {tr("Top"), tr("Left"), tr("Bottom"), tr("Right")};
        for (int i = 0; i < 4; i++) { side[i] = new QCheckBox(names[i]); side[i]->setChecked(true); sides->addWidget(side[i]); }
        layout->addWidget(new QLabel(tr("Trim away")));
        layout->addLayout(sides);
        auto sync = [&] { tolerance->setEnabled(basedOn->currentIndex() != 0); };
        connect(basedOn, QOverload<int>::of(&QComboBox::currentIndexChanged), &dialog, sync);
        sync();
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
        connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        layout->addWidget(buttons);
        if (dialog.exec() != QDialog::Accepted) return;
        TrimOptions o;
        o.basedOn = TrimOptions::BasedOn(basedOn->currentIndex());
        o.top = side[0]->isChecked(); o.left = side[1]->isChecked(); o.bottom = side[2]->isChecked(); o.right = side[3]->isChecked();
        o.tolerance = uint8_t(tolerance->value());
        static const char* const bases[] = {"transparent", "topLeft", "bottomRight"};
        trimCommand({{"basedOn", bases[std::clamp(basedOn->currentIndex(), 0, 2)]}, {"top", o.top}, {"left", o.left}, {"bottom", o.bottom}, {"right", o.right}, {"tolerance", int(o.tolerance)}});
    }, "image.trim"));
    add(image, Spec("image.cropToSelection", tr("Crop to Selection")).document("edit.crop").runs([this] {
        const auto& d = session_->document();
        if (d && d->selection) {
            Rect b = d->selection->bounds();
            if (!b.isEmpty()) {
                if (runCommand("canvas.crop", {{"x", b.x}, {"y", b.y}, {"width", b.width}, {"height", b.height}}, tr("Crop to Selection")))
                    runCommand("selection.none", {}, tr("Crop to Selection"));
            }
        }
    }, "canvas.crop"))->setObjectName("command.canvas.crop");
    image->addSeparator();
    QMenu* adjustments = image->addMenu(tr("&Adjustments"));
    auto pixelAdjustment = [this, adjustments, &add](const QString& label, const QKeySequence& key, AdjustmentKind kind) {
        const QString id = QStringLiteral("image.adjustments.") + commandSlug(QString::fromUtf8(adjustmentKindName(kind)));
        add(adjustments, Spec(id, label, key.isEmpty() ? QList<QKeySequence>{} : QList<QKeySequence>{key}).document((std::string("adjustment.") + adjustmentKindName(kind)).c_str()).runs([this, kind] {
            if (session_->smartObjectBlocksPixels(true)) return;
            if (!session_->canAdjustPixels()) { showError(tr("Adjustments"), tr("Select a visible image layer (not a mask) to adjust its pixels.")); return; }
            (new PixelAdjustmentDialog(session_, kind, this))->show();
        }, "pixels.adjust"));
    };
    pixelAdjustment(tr("&Levels…"), QKeySequence("Ctrl+L"), AdjustmentKind::Levels);
    pixelAdjustment(tr("&Curves…"), QKeySequence("Ctrl+M"), AdjustmentKind::Curves);
    pixelAdjustment(tr("&Hue/Saturation…"), QKeySequence("Ctrl+U"), AdjustmentKind::HueSaturation);
    pixelAdjustment(tr("&Exposure…"), QKeySequence(), AdjustmentKind::Exposure);
    pixelAdjustment(tr("&Gradient Map…"), QKeySequence(), AdjustmentKind::GradientMap);
    pixelAdjustment(tr("G&rain…"), QKeySequence(), AdjustmentKind::Grain);
    adjustments->addSeparator();
    pixelAdjustment(tr("&Brightness/Contrast…"), QKeySequence(), AdjustmentKind::BrightnessContrast);
    pixelAdjustment(tr("&Vibrance…"), QKeySequence(), AdjustmentKind::Vibrance);
    pixelAdjustment(tr("Color &Balance…"), QKeySequence("Ctrl+B"), AdjustmentKind::ColorBalance);
    pixelAdjustment(tr("Black && &White…"), QKeySequence("Alt+Shift+Ctrl+B"), AdjustmentKind::BlackWhite);
    pixelAdjustment(tr("&Photo Filter…"), QKeySequence(), AdjustmentKind::PhotoFilter);
    pixelAdjustment(tr("Channel &Mixer…"), QKeySequence(), AdjustmentKind::ChannelMixer);
    pixelAdjustment(tr("&Selective Color…"), QKeySequence(), AdjustmentKind::SelectiveColor);
    pixelAdjustment(tr("P&osterize…"), QKeySequence(), AdjustmentKind::Posterize);
    pixelAdjustment(tr("&Threshold…"), QKeySequence(), AdjustmentKind::Threshold);
    adjustments->addSeparator();
    add(adjustments, Spec("image.adjustments.invert", tr("&Invert"), {QKeySequence("Ctrl+I")}).document("adjustment.Invert")
                         .request("pixels.invert", [] { return QJsonObject{}; }, tr("Invert")))->setObjectName("command.pixels.invert");
    image->addSeparator();
    add(image, Spec("image.flipCanvasHorizontal", tr("Flip Canvas Horizontal")).document("canvas.flip")
                   .request("canvas.flip", [] { return QJsonObject{}; }, tr("Flip Canvas Horizontal")))->setObjectName("command.canvas.flip");
    add(image, Spec("image.flipCanvasVertical", tr("Flip Canvas Vertical")).document("canvas.flip")
                   .request("canvas.flip", [] { return QJsonObject{{"vertical", true}}; }, tr("Flip Canvas Vertical")));

    QMenu* layer = menuBar()->addMenu(tr("&Layer"));
    // Converted to the command path (CONTRIBUTING.md, "Commands"): the item runs the automation method itself.
    nameAction("layer.new", add(layer, Spec("layer.new", tr("&New Layer"), {QKeySequence("Ctrl+Shift+N")}).document("layers.structure")
                                           .request("layers.add", [] { return QJsonObject{}; }, tr("New Layer"))))->setObjectName("command.layers.add");
    add(layer, Spec("layer.newBelow", tr("New Layer &Below")).document("layers.structure")
                   .request("layers.add", [] { return QJsonObject{{"below", true}}; }, tr("New Layer Below")))->setObjectName("command.layers.addBelow");
    add(layer, Spec("layer.newFolder", tr("New &Folder")).document("layers.structure")
                   .request("layers.add", [] { return QJsonObject{{"kind", "group"}}; }, tr("New Folder")))->setObjectName("command.layers.addGroup");
    add(layer, Spec("layer.group", tr("&Group Layers"), {QKeySequence("Ctrl+G")}).document("layers.structure")
                   .request("layers.group", [] { return QJsonObject{}; }, tr("Group Layers")))->setObjectName("command.layers.group");
    nameAction("layer.viaCopy", add(layer, Spec("layer.viaCopy", tr("Layer via &Copy"), {QKeySequence("Ctrl+J")}).document("edit.clipboard").request("layers.viaCopy", [this]() -> std::optional<QJsonObject> {
        const Layer* l = session_->activeLayer();
        if (!l || l->isGroup) return std::nullopt;
        return QJsonObject{};
    }, tr("Layer via Copy"))));
    nameAction("layer.duplicate", add(layer, Spec("layer.duplicate", tr("&Duplicate Layer")).document("layers.structure")
                                                 .request("layers.duplicate", [] { return QJsonObject{}; }, tr("Duplicate Layer"))))->setObjectName("command.layers.duplicate");
    nameAction("layer.delete", add(layer, Spec("layer.delete", tr("De&lete Layer")).document("layers.structure").runs([this] { deleteLayersCommand(); }, "layers.delete")))
        ->setObjectName("command.layers.delete");
    mergeAction_ = add(layer, Spec("layer.mergeDown", tr("Merge Do&wn"), {QKeySequence("Ctrl+E")}).document("layers.merge")
                                  .runs([this] { runCommand("layers.merge", {}, mergeAction_->text().remove('&')); }, "layers.merge")
                                  .when([this] { return session_->canMergeLayers() ? QString() : tr("Nothing to merge: select the layers to merge, or a layer with a layer below it"); }));
    mergeAction_->setObjectName("command.layers.merge");
    named_["layer.merge"] = mergeAction_;
    mergeVisibleAction_ = add(layer, Spec("layer.mergeVisible", tr("Merge &Visible"), {QKeySequence("Ctrl+Shift+E")}).document("layers.merge")
                                         .request("layers.merge", [] { return QJsonObject{{"visible", true}}; }, tr("Merge Visible"))
                                         .when([this] { return session_->canMergeVisible() ? QString() : tr("Nothing to merge: fewer than two layers are visible"); }));
    mergeVisibleAction_->setObjectName("command.layers.mergeVisible");
    editTextAction_ = nameAction("layer.editText", add(layer, Spec("layer.editText", tr("Edit &Text…")).document("edit.text").runs([this] {
        const Layer* l = session_->activeLayer();
        if (l && l->isLiveText()) session_->requestTextEdit(l->id);
    }).when([this] {
        if (session_->featuresGated()) return session_->unavailableTip();
        const Layer* l = session_->activeLayer();
        return l && l->isLiveText() ? QString() : tr("The active layer is not a text layer");
    })));
    nameAction("layer.rename", add(layer, Spec("layer.rename", tr("&Rename Layer…")).document("layers.structure").runs([this] {
        const Layer* active = session_->activeLayer();
        if (!active) return;
        bool ok;
        QString name = QInputDialog::getText(this, tr("Rename Layer"), tr("Name"), QLineEdit::Normal, QString::fromStdString(active->name), &ok);
        if (ok) runCommand("layers.set", {{"name", name}}, tr("Rename Layer"));
    }, "layers.set")));
    // Move Out of Folder: layers.move, the layer placed directly above its folder in the folder's own parent.
    add(layer, Spec("layer.moveOutOfFolder", tr("Move &Out of Folder"), {QKeySequence("Ctrl+Shift+[")}).document("layers.structure").request("layers.move", [this]() -> std::optional<QJsonObject> {
        const Layer* l = session_->activeLayer();
        const Layer* folder = l && l->parentId ? session_->document()->find(*l->parentId) : nullptr;
        if (!folder) return std::nullopt;
        QJsonObject params{{"id", QString::fromStdString(l->id)}, {"above", QString::fromStdString(folder->id)}};
        if (folder->parentId) params["parent"] = QString::fromStdString(*folder->parentId);
        return params;
    }, tr("Move Out of Folder")))->setObjectName("command.layers.moveOut");
    // The active layer alone, cropped to its visible pixels, as Photoshop's Layer > Quick Export and Export As.
    layer->addSeparator();
    QAction* layerQuickExport = add(layer, Spec("layer.quickExport", tr("Quick Export"), {QKeySequence("Ctrl+Shift+'")}).document("export.png").runs([this] { quickExport(true); }, "document.export"));
    layerQuickExport->setObjectName("export.layerQuick");
    layerQuickExport->setText(tr("Quick Export as %1").arg(exportas::formatLabel(exportas::quickExportFormat())));
    add(layer, Spec("layer.exportAs", tr("Export As…"), {QKeySequence("Ctrl+Alt+Shift+'")}).document("export.png").runs([this] { exportAs(true); }, "document.export"))->setObjectName("export.layerAs");
    connect(layer, &QMenu::aboutToShow, this, [layerQuickExport] { layerQuickExport->setText(MainWindow::tr("Quick Export as %1").arg(exportas::formatLabel(exportas::quickExportFormat()))); });
    layer->addSeparator();
    QMenu* adjustmentLayers = layer->addMenu(tr("New &Adjustment Layer"));
    for (int i = 0; i < adjustmentKindCount; i++) {
        AdjustmentKind kind = AdjustmentKind(i);
        add(adjustmentLayers, Spec(QStringLiteral("layer.newAdjustment.") + commandSlug(QString::fromUtf8(adjustmentKindName(kind))), names::adjustmentKind(kind))
                                  .document((std::string("adjustment.") + adjustmentKindName(kind)).c_str())
                                  .request("layers.add", [kind] { return QJsonObject{{"kind", "adjustment"}, {"adjustmentKind", QString::fromUtf8(adjustmentKindName(kind))}}; }, tr("New Adjustment Layer")));
    }
    QMenu* styles = layer->addMenu(tr("Layer St&yle"));
    const char* const stylePages[] = {QT_TRANSLATE_NOOP("app::MainWindow", "Blending Options…"), QT_TRANSLATE_NOOP("app::MainWindow", "Bevel & Emboss…"), QT_TRANSLATE_NOOP("app::MainWindow", "Stroke…"), QT_TRANSLATE_NOOP("app::MainWindow", "Inner Shadow…"), QT_TRANSLATE_NOOP("app::MainWindow", "Inner Glow…"), QT_TRANSLATE_NOOP("app::MainWindow", "Satin…"), QT_TRANSLATE_NOOP("app::MainWindow", "Color Overlay…"),
                                      QT_TRANSLATE_NOOP("app::MainWindow", "Gradient Overlay…"), QT_TRANSLATE_NOOP("app::MainWindow", "Pattern Overlay…"), QT_TRANSLATE_NOOP("app::MainWindow", "Outer Glow…"), QT_TRANSLATE_NOOP("app::MainWindow", "Drop Shadow…")};
    for (int page = 0; page < int(std::size(stylePages)); page++) {
        // The dialog's OK runs layers.setStyle with the style it shows.
        add(styles, Spec(QStringLiteral("layer.style.") + commandSlug(QString::fromLatin1(stylePages[page])), tr(stylePages[page])).document("edit.style").runs([this, page] {
            if (session_->activeLayerId()) LayerStyleDialog(session_, *session_->activeLayerId(), this, page).exec();
        }, "layers.setStyle").when(hasLayer));
        if (page == 0) styles->addSeparator();
    }
    styles->addSeparator();
    // Copy Layer Style keeps what layers.style answers; Paste and Clear run layers.setStyle (Photoshop's step names).
    add(styles, Spec("layer.style.copy", tr("&Copy Layer Style")).document("edit.style").runs([this] {
        if (!session_->activeLayerHasStyle()) return;
        const auto answer = runCommand("layers.style", {}, tr("Copy Layer Style"));
        if (!answer) return;
        compositor::LayerStyle style;
        if (QString error; layerStyleFromRequest(answer->toObject(), session_->document()->colorMode, style, &error)) session_->setStyleClipboard(style);
        else showError(tr("Copy Layer Style"), error);
    }, "layers.style"))->setObjectName("command.layers.copyStyle");
    add(styles, Spec("layer.style.paste", tr("&Paste Layer Style")).document("edit.style").request("layers.setStyle", [this]() -> std::optional<QJsonObject> {
        const auto& copied = session_->styleClipboard();
        const auto id = session_->activeLayerId();
        if (!copied || !id || !session_->canStyleLayer(*id)) return std::nullopt;
        return QJsonObject{{"style", layerStyleRequest(*copied, session_->document()->colorMode)}, {"paste", true}};
    }, tr("Paste Layer Style")))->setObjectName("command.layers.pasteStyle");
    add(styles, Spec("layer.style.clear", tr("C&lear Layer Style")).document("edit.style").request("layers.setStyle", [this]() -> std::optional<QJsonObject> {
        if (!session_->activeLayerHasStyle() || !session_->canStyleLayer(*session_->activeLayerId())) return std::nullopt;
        return QJsonObject{{"style", QJsonObject{}}};
    }, tr("Clear Layer Style")))->setObjectName("command.layers.clearStyle");
    styles->addSeparator();
    // Imported style presets (.asl): the submenu lists the library as it is when it opens; each runs layers.applyStyle.
    QMenu* applyStyle = styles->addMenu(tr("&Apply Style"));
    applyStyle->setProperty("commandsDynamic", true);
    reg.adopt(nullptr, applyStyle->menuAction(), Spec("layer.style.apply", plainText(applyStyle->title())).document("edit.style").command);
    connect(applyStyle, &QMenu::aboutToShow, this, [this, applyStyle] {
        applyStyle->clear();
        const auto& presets = PresetLibrary::instance().styles();
        if (presets.empty()) applyStyle->addAction(tr("No styles yet: Import Styles…"))->setEnabled(false);
        for (const auto& preset : presets) {
            const QString name = QString::fromStdString(preset.name);
            applyStyle->addAction(name, this, [this, name] {
                if (!PresetLibrary::instance().findStyle(name) || !session_->activeLayerId()) return;
                runCommand("layers.applyStyle", {{"style", name}}, tr("Couldn’t apply the style"));
            });
        }
    });
    add(styles, Spec("layer.style.import", tr("&Import Styles…")).runs([this] { importPresetsInteractively(this, session_); }, "presets.import"));
    QMenu* smart = layer->addMenu(tr("Smart Ob&jects"));
    nameAction("smart.convert", add(smart, Spec("layer.smartObject.convert", tr("&Convert to Smart Object")).document("edit.smartObject")
                                               .request("smartObject.convert", [] { return QJsonObject{}; }, tr("Couldn’t convert to a smart object"))));
    nameAction("smart.viaCopy", add(smart, Spec("layer.smartObject.viaCopy", tr("New Smart Object via &Copy")).document("edit.smartObject").request("smartObject.viaCopy", [this]() -> std::optional<QJsonObject> {
        const Layer* l = session_->activeLayer();
        if (!l || !l->isLiveSmartObject()) return std::nullopt;
        return QJsonObject{};
    }, tr("Couldn’t copy the smart object"))));
    nameAction("smart.edit", add(smart, Spec("layer.smartObject.editContents", tr("&Edit Contents")).document("edit.smartObject").runs([this] {
        // A camera RAW smart object reopens in Camera Raw (the dialog is the interface); the rest run
        // smartObject.editContents, the contents in a tab of their own.
        if (session_->activeRawSmartObject()) { editSmartObjectContents(); return; }
        runCommand("smartObject.editContents", {}, tr("Couldn’t open the contents"));
    }, "smartObject.editContents")))->setObjectName("command.smartObject.editContents");
    nameAction("smart.replace", add(smart, Spec("layer.smartObject.replace", tr("&Replace Contents…")).document("edit.smartObject").runs([this] {
        const QString path = QFileDialog::getOpenFileName(this, tr("Replace Contents"), QSettings().value("lastDir").toString(),
                                                          tr("Images, Photoshop and Affinity documents (*.psd *.psb *.afphoto *.afdesign *.afpub *.af *.png *.jpg *.jpeg *.tif *.tiff *.webp *.bmp *.gif %1)").arg(compositor::rawSupported() ? QStringLiteral("*.cr2 *.cr3 *.crw *.nef *.nrw *.arw *.srf *.sr2 *.raf *.orf *.rw2 *.rwl *.pef *.dng *.3fr *.iiq *.erf *.kdc *.dcr *.mrw *.srw *.x3f") : QString()));
        if (path.isEmpty()) return;
        runCommand("smartObject.replace", {{"path", path}}, tr("Couldn’t replace the contents"));
    }, "smartObject.replace")));
    nameAction("smart.rasterize", add(smart, Spec("layer.smartObject.rasterize", tr("R&asterize")).document("edit.smartObject").request("smartObject.rasterize", [this]() -> std::optional<QJsonObject> {
        const Layer* l = session_->activeLayer();
        if (!l || !l->smartObject) return std::nullopt;
        return QJsonObject{};
    }, tr("Rasterize"))));
    layer->addSeparator();
    QMenu* mask = layer->addMenu(tr("Layer &Mask"));
    // layers.mask, on the active layer (none: nothing to do).
    auto maskItem = [&](QMenu* menu, const char* id, const QString& label, const char* feature, QJsonObject params) {
        return add(menu, Spec(id, label).document(feature).request("layers.mask", [this, params]() -> std::optional<QJsonObject> {
            if (!session_->activeLayerId()) return std::nullopt;
            return params;
        }, tr("Layer Mask")).when(hasLayer));
    };
    nameAction("mask.add", maskItem(mask, "layer.mask.revealAll", tr("Reveal All"), "layers.mask", {{"action", "add"}}));
    maskItem(mask, "layer.mask.hideAll", tr("Hide All"), "layers.mask", {{"action", "add"}, {"revealing", false}});
    maskItem(mask, "layer.mask.fromSelectionReveal", tr("From Selection (Reveal)"), "edit.selection", {{"action", "addFromSelection"}});
    maskItem(mask, "layer.mask.fromSelectionHide", tr("From Selection (Hide)"), "edit.selection", {{"action", "addFromSelection"}, {"revealing", false}});
    mask->addSeparator();
    nameAction("mask.toggle", maskItem(mask, "layer.mask.toggle", tr("Enable / Disable"), "layers.mask", {{"action", "toggle"}}));
    nameAction("mask.invert", maskItem(mask, "layer.mask.invert", tr("Invert"), "layers.mask", {{"action", "invert"}}));
    nameAction("mask.apply", maskItem(mask, "layer.mask.apply", tr("Apply"), "layers.applyMask", {{"action", "apply"}}));
    nameAction("mask.delete", maskItem(mask, "layer.mask.delete", tr("Delete"), "layers.mask", {{"action", "delete"}}));
    // Photoshop's Layer > Vector Mask: a path that cuts the layer, edited with the Pen and Direct Selection. The items
    // run vectorMask.set, vectorMask.target and vectorMask.delete on the active layer.
    QMenu* vectorMask = layer->addMenu(tr("&Vector Mask"));
    auto addVector = [this](const char* kind) -> std::optional<QJsonObject> {
        const Layer* l = session_->activeLayer();
        if (l && compositor::hasLayerVectorMask(*l)) { QMessageBox::information(this, tr("Vector Mask"), EditorSession::tr("The layer has a vector mask already.")); return std::nullopt; }
        return QJsonObject{{"mode", kind}};
    };
    auto ownVectorMask = [this]() -> std::optional<QJsonObject> {
        const Layer* l = session_->activeLayer();
        if (!l || !compositor::hasLayerVectorMask(*l)) return std::nullopt;
        return QJsonObject{};
    };
    add(vectorMask, Spec("layer.vectorMask.revealAll", tr("Reveal All")).document("edit.vector").request("vectorMask.set", [addVector] { return addVector("revealAll"); }, tr("Vector Mask")).when(hasLayer))
        ->setObjectName("command.vectorMask.revealAll");
    add(vectorMask, Spec("layer.vectorMask.hideAll", tr("Hide All")).document("edit.vector").request("vectorMask.set", [addVector] { return addVector("hideAll"); }, tr("Vector Mask")).when(hasLayer));
    add(vectorMask, Spec("layer.vectorMask.currentPath", tr("Current Path")).document("edit.vector").request("vectorMask.set", [addVector] { return addVector("currentPath"); }, tr("Vector Mask")).when(hasLayer));
    vectorMask->addSeparator();
    add(vectorMask, Spec("layer.vectorMask.edit", tr("Edit")).document("edit.vector").request("vectorMask.target", ownVectorMask, tr("Vector Mask")).when(hasLayer))
        ->setObjectName("command.vectorMask.edit");
    add(vectorMask, Spec("layer.vectorMask.delete", tr("Delete")).document("edit.vector").request("vectorMask.delete", ownVectorMask, tr("Vector Mask")).when(hasLayer))
        ->setObjectName("command.vectorMask.delete");
    nameAction("layer.clipping", add(layer, Spec("layer.clippingMask", tr("Create / Release Cl&ipping Mask"), {QKeySequence("Ctrl+Alt+G")}).document("layers.structure").request("layers.set", [this]() -> std::optional<QJsonObject> {
        const Layer* l = session_->activeLayer();
        if (!l || !session_->canToggleClippingMask(l->id)) return std::nullopt;
        return QJsonObject{{"clipping", !l->maskSourceId.has_value()}};
    }, tr("Clipping Mask"))))->setObjectName("command.layers.clipping");
    layer->addSeparator();
    add(layer, Spec("layer.bringForward", tr("Bring Forward"), {QKeySequence("Ctrl+]")}).document("layers.structure").request("layers.reorder", [this]() -> std::optional<QJsonObject> {
        if (!session_->canMoveActiveLayer(1)) return std::nullopt;
        return QJsonObject{{"offset", 1}};
    }, tr("Bring Forward")))->setObjectName("command.layers.forward");
    add(layer, Spec("layer.sendBackward", tr("Send Backward"), {QKeySequence("Ctrl+[")}).document("layers.structure").request("layers.reorder", [this]() -> std::optional<QJsonObject> {
        if (!session_->canMoveActiveLayer(-1)) return std::nullopt;
        return QJsonObject{{"offset", -1}};
    }, tr("Send Backward")));
    layer->addSeparator();
    add(layer, Spec("layer.flipHorizontal", tr("Flip Layer Horizontal")).document("layers.transform")
                   .request("layers.flip", [] { return QJsonObject{}; }, tr("Flip Layer Horizontal")))->setObjectName("command.layers.flip");
    add(layer, Spec("layer.flipVertical", tr("Flip Layer Vertical")).document("layers.transform")
                   .request("layers.flip", [] { return QJsonObject{{"vertical", true}}; }, tr("Flip Layer Vertical")));
    QMenu* sampling = layer->addMenu(tr("Resampling"));
    auto samplingItem = [&](const char* id, const QString& label, Sampling s) {
        add(sampling, Spec(id, label).document("layers.structure").runs([this, s] { samplingCommand(s); }, "layers.set"));
    };
    samplingItem("layer.resampling.high", tr("High Quality"), Sampling::High);
    samplingItem("layer.resampling.smooth", tr("Smooth"), Sampling::Smooth);
    samplingItem("layer.resampling.nearest", tr("Nearest Neighbour"), Sampling::Nearest);

    // Photoshop's Type menu: the active text layer's outlines as a path (text.toPath) or a shape (text.toShape).
    QMenu* type = menuBar()->addMenu(tr("&Type"));
    auto fromText = [this](bool shape) -> std::optional<QJsonObject> {
        if (!session_->activeLayerId()) { QMessageBox::information(this, shape ? tr("Convert to Shape") : tr("Create Work Path"), tr("Choose a text layer first.")); return std::nullopt; }
        return QJsonObject{};
    };
    nameAction("type.workPath", add(type, Spec("type.createWorkPath", tr("Create &Work Path")).document("edit.text")
                                              .request("text.toPath", [fromText] { return fromText(false); }, tr("Create Work Path"))))->setObjectName("command.text.toPath");
    nameAction("type.shape", add(type, Spec("type.convertToShape", tr("Convert to &Shape")).document("edit.paint")
                                           .request("text.toShape", [fromText] { return fromText(true); }, tr("Convert to Shape"))))->setObjectName("command.text.toShape");

    // Everything about the selection in one place, as Photoshop's Select menu: the whole-canvas commands,
    // then Modify, then loading a layer's pixels or mask as the selection.
    QMenu* select = menuBar()->addMenu(tr("&Select"));
    nameAction("select.all", add(select, Spec("select.all", tr("&All"), {QKeySequence::SelectAll}).document("edit.selection")
                                             .request("selection.all", [] { return QJsonObject{}; }, tr("Select All"))));
    nameAction("select.deselect", add(select, Spec("select.deselect", tr("&Deselect"), {QKeySequence("Ctrl+D")}).document("edit.selection")
                                                  .request("selection.none", [] { return QJsonObject{}; }, tr("Deselect"))));
    nameAction("select.reselect", add(select, Spec("select.reselect", tr("&Reselect"), {QKeySequence("Shift+Ctrl+D")}).document("edit.selection").request("selection.reselect", [this]() -> std::optional<QJsonObject> {
        if (!session_->canReselect()) return std::nullopt;
        return QJsonObject{};
    }, tr("Reselect"))));
    nameAction("select.inverse", add(select, Spec("select.inverse", tr("&Inverse"), {QKeySequence("Ctrl+Shift+I")}).document("edit.selection")
                                                 .request("selection.invert", [] { return QJsonObject{}; }, tr("Inverse"))));
    // Quick Mask: selection.quickMask, on or off.
    add(select, Spec("select.quickMask", tr("Edit in &Quick Mask Mode"), {QKeySequence("Q")}).document("edit.selection")
                    .request("selection.quickMask", [] { return QJsonObject{}; }, tr("Quick Mask")))->setObjectName("command.selection.quickMask");
    // Photoshop's Select > Subject, through selection.subject; greyed, saying why, without the click-to-select model.
    select->addSeparator();
    nameAction("select.subject", add(select, Spec("select.subject", tr("Subject")).document("tool.quickSelect").when([]() -> QString {
        if (!ModelStore::supported()) return MainWindow::tr("Unavailable: this build has no OpenCV");
        if (!ModelStore::promptReady())
            return MainWindow::tr("The click-to-select model isn’t downloaded: choose the Click engine in the Quick Selection tool’s options and download it");
        return {};
    }).runs([this] { selectSubject(); }, "selection.subject")));
    select->addSeparator();
    QMenu* modify = select->addMenu(tr("&Modify"));
    add(modify, Spec("select.modify.expand", tr("&Expand…")).document("edit.selection").runs([this] { bool ok; int n = QInputDialog::getInt(this, tr("Expand Selection"), tr("Pixels"), 1, 1, 500, 1, &ok); if (ok) runCommand("selection.grow", {{"amount", n}}, tr("Expand Selection")); }, "selection.grow"));
    add(modify, Spec("select.modify.contract", tr("&Contract…")).document("edit.selection").runs([this] { bool ok; int n = QInputDialog::getInt(this, tr("Contract Selection"), tr("Pixels"), 1, 1, 500, 1, &ok); if (ok) runCommand("selection.grow", {{"amount", -n}}, tr("Contract Selection")); }, "selection.grow"));
    nameAction("select.feather", add(modify, Spec("select.modify.feather", tr("&Feather…"), {QKeySequence("Shift+F6")}).document("edit.selection").runs([this] { bool ok; double r = QInputDialog::getDouble(this, tr("Feather Selection"), tr("Radius (pixels)"), 5, 0.1, 250, 1, &ok); if (ok) runCommand("selection.feather", {{"radius", r}}, tr("Feather Selection")); }, "selection.feather")));
    add(modify, Spec("select.modify.smooth", tr("&Smooth…")).document("edit.selection").runs([this] { bool ok; int n = QInputDialog::getInt(this, tr("Smooth Selection"), tr("Sample radius (pixels)"), 3, 1, 100, 1, &ok); if (ok) runCommand("selection.smooth", {{"radius", n}}, tr("Smooth Selection")); }, "selection.smooth"));
    add(modify, Spec("select.modify.border", tr("&Border…")).document("edit.selection").runs([this] { bool ok; int n = QInputDialog::getInt(this, tr("Border Selection"), tr("Width (pixels)"), 4, 1, 200, 1, &ok); if (ok) runCommand("selection.border", {{"width", n}}, tr("Border Selection")); }, "selection.border"));
    select->addSeparator();
    QMenu* load = select->addMenu(tr("&Load as Selection"));
    auto loadItem = [&](const char* id, const QString& label, QJsonObject params) {
        add(load, Spec(id, label).document("edit.selection").request("selection.fromLayer", [this, params]() -> std::optional<QJsonObject> {
            if (!session_->activeLayerId()) return std::nullopt;
            return params;
        }, tr("Load as Selection")).when(hasLayer));
    };
    loadItem("select.load.layerPixels", tr("Layer Pixels"), {});
    loadItem("select.load.layerMask", tr("Layer Mask"), {{"mask", true}});
    load->addSeparator();
    loadItem("select.load.addLayerPixels", tr("Add Layer Pixels"), {{"mode", "add"}});
    loadItem("select.load.subtractLayerPixels", tr("Subtract Layer Pixels"), {{"mode", "subtract"}});
    loadItem("select.load.intersectLayerPixels", tr("Intersect with Layer Pixels"), {{"mode", "intersect"}});
    // Selections kept as alpha channels (the Channels panel; docs/channels.md): the dialogs' OK runs
    // channels.loadSelection and channels.saveSelection.
    add(select, Spec("select.loadSelection", tr("Load Selection…")).document("edit.channels").runs([this] { (new LoadSelectionDialog(session_, this))->open(); }, "channels.loadSelection"));
    nameAction("select.save", add(select, Spec("select.saveSelection", tr("Save Selection…")).document("edit.channels").runs([this] {
        if (!session_->document()->selection) { showError(tr("Save Selection"), tr("Make a selection first.")); return; }
        (new SaveSelectionDialog(session_, this))->open();
    }, "channels.saveSelection")));
    // Photoshop's channel keys: Ctrl+2 the composite, then one key per colour channel (Ctrl+3, 4, 5 red, green and
    // blue; Ctrl+3 to 6 cyan to black in CMYK), then the first alpha channels up to Ctrl+9; with Alt, the channel is
    // loaded as a selection instead. They run channels.select and channels.loadSelection.
    auto channelOfKey = [this](int n) -> std::optional<QString> {
        if (!session_->hasDocument()) return std::nullopt;
        const Document& d = *session_->document();
        const int firstAlpha = 3 + colorModeColorChannels(d.colorMode);
        if (n == 2) return QString::fromLatin1(colorModeKey(d.colorMode));
        if (n < firstAlpha) return QString::fromLatin1(colorChannelName(d.colorMode, n - 3)).toLower();
        if (size_t(n - firstAlpha) >= d.channels.size()) return std::nullopt;
        return QString::fromStdString(d.channels[size_t(n - firstAlpha)].id);
    };
    for (int n = 2; n <= 9; n++)
        for (bool loadKey : {false, true}) {
            const QString id = QStringLiteral("select.channel.%1%2").arg(loadKey ? QStringLiteral("load") : QStringLiteral("target")).arg(n - 1);
            add(nullptr, Spec(id, loadKey ? tr("Load Channel %1 as Selection").arg(n - 1) : tr("Select Channel %1").arg(n - 1),
                              {QKeySequence(QString::fromLatin1(loadKey ? "Ctrl+Alt+%1" : "Ctrl+%1").arg(n))})
                             .document("edit.channels")
                             .request(loadKey ? "channels.loadSelection" : "channels.select", [channelOfKey, n]() -> std::optional<QJsonObject> {
                                 const auto channel = channelOfKey(n);
                                 if (!channel) return std::nullopt;
                                 return QJsonObject{{"channel", *channel}};
                             }));
        }

    QMenu* filter = menuBar()->addMenu(tr("Filte&r"));
    // Photoshop's filters on a smart object go on as Smart Filters: the blurs, noise and the ones the Smart Filter
    // kernels draw. The rest change pixels, so on a smart object they ask to rasterize first.
    auto smartCapable = [](FilterKind kind) {
        return kind == FilterKind::GaussianBlur || kind == FilterKind::MotionBlur || kind == FilterKind::AddNoise
               || compositor::smartFilterParametersFor(kind, compositor::FilterSettings::defaults(kind)).has_value();
    };
    auto filterAction = [this, &add, smartCapable](QMenu* menu, const QString& group, const QString& label, FilterKind kind) {
        add(menu, Spec(QStringLiteral("filter.") + group + commandSlug(QString::fromUtf8(filterKindName(kind))), label)
                      .document((std::string("filter.") + filterKindName(kind)).c_str()).runs([this, kind, smartCapable] {
            if (smartCapable(kind) && session_->canAddSmartFilter()) { (new FilterDialog(session_, kind, this, true))->show(); return; }
            if (session_->smartObjectBlocksPixels(true)) return;
            if (!session_->canAdjustPixels()) { showError(tr("Filters"), tr("Select a visible image layer (not a mask) to filter its pixels.")); return; }
            if (compositor::filterRunsDirectly(kind)) {
                // No dialog, as in Photoshop: Clouds draws a new pattern each time.
                const uint32_t seed = uint32_t(std::random_device{}() % 1000000000u);
                runCommand("pixels.filter", gridFilterStep(kind, compositor::FilterSettings::defaults(kind), seed), names::filterKind(kind));
                return;
            }
            (new FilterDialog(session_, kind, this))->show();
        }, "pixels.filter"));
    };
    // Photoshop's places: the top-level filters, then the submenus in its order, each in its own order.
    // Photoshop's shortcut. A destructive filter here: on a smart object it asks first, like the others. OK runs
    // pixels.cameraRaw.
    add(filter, Spec("filter.cameraRaw", tr("Camera &Raw Filter…"), {QKeySequence("Shift+Ctrl+A")}).document("filter.Camera Raw").runs([this] {
        if (session_->smartObjectBlocksPixels(true)) return;
        if (!session_->canAdjustPixels()) { showError(tr("Camera Raw Filter"), tr("Select a visible image layer (not a mask) to filter its pixels.")); return; }
        (new CameraRawDialog(session_, this))->show();
    }, "pixels.cameraRaw"));
    filterAction(filter, QString(), tr("&Lens Correction…"), FilterKind::LensCorrection);
    filter->addSeparator();
    QMenu* blurMenu = filter->addMenu(tr("&Blur"));
    filterAction(blurMenu, QStringLiteral("blur."), tr("&Box Blur…"), FilterKind::BoxBlur);
    filterAction(blurMenu, QStringLiteral("blur."), tr("&Gaussian Blur…"), FilterKind::GaussianBlur);
    filterAction(blurMenu, QStringLiteral("blur."), tr("&Motion Blur…"), FilterKind::MotionBlur);
    filterAction(blurMenu, QStringLiteral("blur."), tr("&Radial Blur…"), FilterKind::RadialBlur);
    filterAction(blurMenu, QStringLiteral("blur."), tr("&Surface Blur…"), FilterKind::SurfaceBlur);
    QMenu* distortMenu = filter->addMenu(tr("&Distort"));
    filterAction(distortMenu, QStringLiteral("distort."), tr("&Pinch…"), FilterKind::Pinch);
    filterAction(distortMenu, QStringLiteral("distort."), tr("P&olar Coordinates…"), FilterKind::PolarCoordinates);
    filterAction(distortMenu, QStringLiteral("distort."), tr("&Ripple…"), FilterKind::Ripple);
    filterAction(distortMenu, QStringLiteral("distort."), tr("S&hear…"), FilterKind::Shear);
    filterAction(distortMenu, QStringLiteral("distort."), tr("&Spherize…"), FilterKind::Spherize);
    filterAction(distortMenu, QStringLiteral("distort."), tr("&Twirl…"), FilterKind::Twirl);
    filterAction(distortMenu, QStringLiteral("distort."), tr("&Wave…"), FilterKind::Wave);
    filterAction(distortMenu, QStringLiteral("distort."), tr("&ZigZag…"), FilterKind::ZigZag);
    QMenu* noiseMenu = filter->addMenu(tr("&Noise"));
    filterAction(noiseMenu, QStringLiteral("noise."), tr("Add &Noise…"), FilterKind::AddNoise);
    filterAction(noiseMenu, QStringLiteral("noise."), tr("&Dust && Scratches…"), FilterKind::DustAndScratches);
    filterAction(noiseMenu, QStringLiteral("noise."), tr("&Median…"), FilterKind::Median);
    QMenu* pixelateMenu = filter->addMenu(tr("&Pixelate"));
    filterAction(pixelateMenu, QStringLiteral("pixelate."), tr("&Mosaic…"), FilterKind::Mosaic);
    QMenu* renderMenu = filter->addMenu(tr("R&ender"));
    filterAction(renderMenu, QStringLiteral("render."), tr("&Clouds"), FilterKind::Clouds);
    filterAction(renderMenu, QStringLiteral("render."), tr("&Difference Clouds"), FilterKind::DifferenceClouds);
    QMenu* sharpenMenu = filter->addMenu(tr("S&harpen"));
    filterAction(sharpenMenu, QStringLiteral("sharpen."), tr("&Unsharp Mask…"), FilterKind::UnsharpMask);
    QMenu* stylizeMenu = filter->addMenu(tr("St&ylize"));
    filterAction(stylizeMenu, QStringLiteral("stylize."), tr("&Emboss…"), FilterKind::Emboss);
    filterAction(stylizeMenu, QStringLiteral("stylize."), tr("&Find Edges"), FilterKind::FindEdges);
    QMenu* otherMenu = filter->addMenu(tr("O&ther"));
    filterAction(otherMenu, QStringLiteral("other."), tr("&High Pass…"), FilterKind::HighPass);
    filterAction(otherMenu, QStringLiteral("other."), tr("Ma&ximum…"), FilterKind::Maximum);
    filterAction(otherMenu, QStringLiteral("other."), tr("M&inimum…"), FilterKind::Minimum);
    filterAction(otherMenu, QStringLiteral("other."), tr("&Offset…"), FilterKind::Offset);
    filter->addSeparator();
    // OpenMosh's glitch, distortion and retro effects (docs/mosh.md), a submenu per category; OK runs pixels.mosh.
    QMenu* moshMenu = filter->addMenu(tr("M&osh"));
    for (int c = 0; c < compositor::mosh::categoryCount; c++) {
        const auto category = compositor::mosh::Category(c);
        QMenu* sub = nullptr;
        for (const compositor::mosh::EffectSpec& spec : compositor::mosh::effects()) {
            if (spec.category != category) continue;
            if (!sub) sub = moshMenu->addMenu(names::mosh(compositor::mosh::categoryName(category)));
            const compositor::mosh::EffectSpec* effect = &spec;
            add(sub, Spec(QStringLiteral("filter.mosh.") + commandSlug(QString::fromUtf8(spec.id.data(), qsizetype(spec.id.size()))), tr("%1…").arg(names::mosh(spec.name)))
                         .document("filter.Mosh").runs([this, effect] {
                if (session_->smartObjectBlocksPixels(true)) return;
                if (!session_->canAdjustPixels()) { showError(tr("Filters"), tr("Select a visible image layer (not a mask) to filter its pixels.")); return; }
                (new MoshDialog(session_, *effect, this))->show();
            }, "pixels.mosh"));
        }
    }
    filter->addSeparator();
    gmicAction_ = add(filter, Spec("filter.gmic", tr("&G'MIC…"), {QKeySequence("Ctrl+Shift+G")}).document("filter.G'MIC").runs([this] { openGmic(); }, "pixels.gmic"));
    removeBackgroundAction_ = add(filter, Spec("filter.removeBackground", tr("Remove &Background…")).document("edit.removeBackground").runs([this] {
        if (!ModelStore::ready()) {
            // Off, or no model yet: the preferences page is where it gets turned on and fetched.
            auto answer = QMessageBox::question(this, tr("Remove Background"),
                ModelStore::supported() ? tr("AI background removal is turned off. Open Preferences to enable it and download the model?")
                                        : tr("This build was made without OpenCV, which runs the segmentation model."),
                ModelStore::supported() ? (QMessageBox::Yes | QMessageBox::Cancel) : QMessageBox::Ok);
            if (answer == QMessageBox::Yes) showPreferences();
            return;
        }
        if (session_->smartObjectBlocksPixels(true)) return;
        if (!session_->canAdjustPixels()) { showError(tr("Remove Background"), tr("Select a visible image layer (not a mask) to remove its background.")); return; }
        const ModelInfo* quick = ModelStore::modelById("pphumanseg");
        QString quickPath = quick && ModelStore::isPresent(*quick) ? ModelStore::pathFor(*quick) : QString();
        (new BackgroundDialog(session_, ModelStore::pathFor(ModelStore::selected()), quickPath, this))->show();
    }, "pixels.removeBackground"));
    refreshBackgroundAction();

    QMenu* view = menuBar()->addMenu(tr("&View"));
    // The zoom runs view.zoom (the view, not the document: nothing is recorded).
    add(view, Spec("view.zoomIn", tr("Zoom &In"), {QKeySequence::ZoomIn}).document("view").request("view.zoom", [this] { return QJsonObject{{"zoom", session_->viewport.zoom * 1.25}}; }))
        ->setObjectName("command.view.zoomIn");
    add(view, Spec("view.zoomOut", tr("Zoom &Out"), {QKeySequence::ZoomOut}).document("view").request("view.zoom", [this] { return QJsonObject{{"zoom", session_->viewport.zoom / 1.25}}; }));
    add(view, Spec("view.fitOnScreen", tr("&Fit on Screen"), {QKeySequence("Ctrl+0")}).document("view").request("view.zoom", [] { return QJsonObject{{"fit", true}}; }))
        ->setObjectName("command.view.fit");
    add(view, Spec("view.actualPixels", tr("&Actual Pixels"), {QKeySequence("Ctrl+1")}).document("view").request("view.zoom", [] { return QJsonObject{{"zoom", 1}}; }))
        ->setObjectName("command.view.actualPixels");
    view->addSeparator();
    rulersAction_ = add(view, Spec("view.rulers", tr("&Rulers"), {QKeySequence("Ctrl+R")}));
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
    auto viewOption = [this, &reg](QMenu* menu, const char* id, const QString& label, const QKeySequence& key, bool ViewOptions::*field) {
        QAction* a = reg.add(menu, Spec(id, label, key.isEmpty() ? QList<QKeySequence>{} : QList<QKeySequence>{key}).command);
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
    add(snapTo, Spec("view.snapTo.all", tr("&All", "snap to")).runs([setAllTargets] { setAllTargets(true); }));
    add(snapTo, Spec("view.snapTo.none", tr("&None", "snap to")).runs([setAllTargets] { setAllTargets(false); }));
    view->addSeparator();
    QAction* lockGuides = viewOption(view, "view.lockGuides", tr("Lock Gu&ides"), QKeySequence("Alt+Ctrl+;"), &ViewOptions::lockGuides);
    addAction(lockGuides);
    add(view, Spec("view.clearGuides", tr("C&lear Guides")).document().request("guides.delete", [this]() -> std::optional<QJsonObject> {
        if (session_->guides().empty()) return std::nullopt;
        return QJsonObject{{"all", true}};
    }, tr("Clear Guides")));
    add(view, Spec("view.newGuide", tr("&New Guide…")).document().runs([this] {
        auto guide = askNewGuide(this);
        if (!guide) return;
        runCommand("guides.add", {{"orientation", guide->vertical() ? "vertical" : "horizontal"}, {"position", guide->position}}, tr("New Guide"));
    }, "guides.add"));
    view->addSeparator();
    // Photoshop's screen modes: Standard, and Full Screen (FullScreen.h), where nothing but the canvas is on screen and
    // each screen edge slides its part of the interface out when the pointer reaches it. F steps through them, Esc
    // leaves full screen; Tab pins every edge out (or hides them again), Shift+Tab all but the tools; F10 or Alt opens
    // the menus.
    QMenu* screenMode = view->addMenu(tr("Scree&n Mode"));
    auto* screenModes = new QActionGroup(this);
    QAction* standardMode = add(screenMode, Spec("view.screenMode.standard", tr("&Standard Screen Mode")).runs([this] { setScreenMode(false); }));
    QAction* fullMode = add(screenMode, Spec("view.screenMode.fullScreen", tr("&Full Screen Mode")).runs([this] { setScreenMode(true); }));
    for (QAction* a : {standardMode, fullMode}) { a->setCheckable(true); screenModes->addAction(a); }
    standardMode->setChecked(true);
    connect(fullScreen_, &FullScreenMode::activeChanged, this, [standardMode, fullMode](bool on) { fullMode->setChecked(on); standardMode->setChecked(!on); });
    add(nullptr, Spec("view.screenMode.cycle", tr("Cycle Screen Modes"), {QKeySequence("F")}).runs([this] { setScreenMode(!fullScreen_->active()); }));
    screenMode->addSeparator();
    auto onlyFullScreen = [this] { return fullScreen_->active() ? QString() : tr("In Full Screen Mode only"); };
    add(screenMode, Spec("view.screenMode.panels", tr("Show or Hide &Panels"), {QKeySequence(Qt::Key_Tab)}).runs([this] { fullScreen_->togglePinned(true); }).when(onlyFullScreen));
    add(screenMode, Spec("view.screenMode.panelsExceptTools", tr("Show or Hide Panels &Except Tools"), {QKeySequence(Qt::SHIFT | Qt::Key_Tab)})
                        .runs([this] { fullScreen_->togglePinned(false); }).when(onlyFullScreen));
    // F10 opens the menus (in full screen over the canvas, the menu bar showing while one is open).
    add(nullptr, Spec("view.showMenus", tr("Show Menus"), {QKeySequence(Qt::Key_F10)}).runs([this] { fullScreen_->showMenus(); }));
    view->addSeparator();
    layersDock_->toggleViewAction()->setText(tr("&Layers Panel"));
    layersDock_->toggleViewAction()->setShortcut(QKeySequence("F7"));   // Photoshop's Window > Layers
    layersDock_->toggleViewAction()->setObjectName("window.layers");
    layersDock_->toggleViewAction()->installEventFilter(fieldGuard);
    adjustDock_->toggleViewAction()->setText(tr("&Adjustments Panel"));
    reg.adopt(view, layersDock_->toggleViewAction(), Spec("window.layers", layersDock_->toggleViewAction()->text(), {QKeySequence("F7")}).command);
    pathsDock_->toggleViewAction()->setText(tr("&Paths Panel"));
    reg.adopt(view, pathsDock_->toggleViewAction(), Spec("window.paths", pathsDock_->toggleViewAction()->text()).command);
    reg.adopt(view, adjustDock_->toggleViewAction(), Spec("window.adjustments", adjustDock_->toggleViewAction()->text()).command);
    view->addSeparator();
    // A 32-bit document's view (docs/bit-depth.md, "32 bits"): not an undo step; the canvas shows it as it changes.
    previewOptionsAction_ = add(view, Spec("view.previewOptions32", tr("32-bit Pre&view Options…")).runs([this] {
        const View32 before = session_->view32();
        auto chosen = askPreviewOptions(this, before, [this](const View32& v) { session_->setView32(v); });
        session_->setView32(chosen ? *chosen : before);
    }, "view.exposure").when([this] {
        return session_->hasDocument() && session_->sampleType() == SampleType::F32 ? QString() : tr("For 32-bit documents");
    }));
    // Soft proofing (docs/color-management.md): Photoshop's Proof Setup, Proof Colors and Gamut Warning.
    QMenu* proofSetup = view->addMenu(tr("Proof Set&up"));
    add(proofSetup, Spec("view.proofSetup.custom", tr("Custom…")).runs([this] { color::showProofSetup(this); }, "color.settings"));
    proofSetup->addSeparator();
    // Photoshop's default proof: the press the Working CMYK describes.
    QAction* proofCmyk = add(proofSetup, Spec("view.proofSetup.workingCmyk", tr("Working CMYK")).runs([] { color::Settings s = color::settings(); s.proofProfile = QStringLiteral("working-cmyk"); color::setSettings(s); }, "color.settings"));
    proofCmyk->setCheckable(true);
    proofCmyk->setChecked(color::settings().proofProfile == QLatin1String("working-cmyk"));
    QAction* proof = add(view, Spec("view.proofColors", tr("Proof Colo&rs"), {QKeySequence("Ctrl+Y")}));
    connect(proof, &QAction::triggered, this, [](bool on) { color::Settings s = color::settings(); s.proofColors = on; color::setSettings(s); });
    QAction* gamut = add(view, Spec("view.gamutWarning", tr("Gamut Wa&rning"), {QKeySequence("Ctrl+Shift+Y")}));
    connect(gamut, &QAction::triggered, this, [](bool on) { color::Settings s = color::settings(); s.gamutWarning = on; color::setSettings(s); });
    for (QAction* a : {proof, gamut}) a->setCheckable(true);
    connect(color::notifier(), &color::Notifier::changed, this, [this, proof, gamut, proofCmyk] {
        proof->setChecked(color::settings().proofColors);
        gamut->setChecked(color::settings().gamutWarning);
        proofCmyk->setChecked(color::settings().proofProfile == QLatin1String("working-cmyk"));
        updateColorSwatches();
    });
    view->addSeparator();
    QAction* grid = add(view, Spec("view.pixelGrid", tr("Pixel &Grid")));
    connect(grid, &QAction::triggered, this, [this](bool on) { session_->showsPixelGrid = on; canvas_->update(); });
    grid->setCheckable(true);
    grid->setChecked(true);
    QAction* controls = add(view, Spec("view.transformControls", tr("Transform &Controls"), {QKeySequence("Ctrl+H")}));
    connect(controls, &QAction::triggered, this, [this](bool on) { session_->showsTransformControls = on; emit session_->transformChanged(); });
    controls->setCheckable(true);
    controls->setChecked(true);

    // Photoshop's Window menu: every panel.
    QMenu* window = menuBar()->addMenu(tr("&Window"));
    actionsDock_->toggleViewAction()->setText(tr("&Actions"));
    actionsDock_->toggleViewAction()->setShortcut(QKeySequence("Alt+F9"));
    reg.adopt(window, actionsDock_->toggleViewAction(), Spec("window.actions", actionsDock_->toggleViewAction()->text(), {QKeySequence("Alt+F9")}).command);
    window->addAction(adjustDock_->toggleViewAction());
    window->addAction(layersDock_->toggleViewAction());
    channelsDock_->toggleViewAction()->setText(tr("&Channels"));
    reg.adopt(window, channelsDock_->toggleViewAction(), Spec("window.channels", channelsDock_->toggleViewAction()->text()).command);
    histogramDock_->toggleViewAction()->setText(tr("&Histogram"));
    reg.adopt(window, histogramDock_->toggleViewAction(), Spec("window.histogram", histogramDock_->toggleViewAction()->text()).command);
    window->addAction(pathsDock_->toggleViewAction());
    timelineDock_->toggleViewAction()->setText(tr("&Timeline"));
    reg.adopt(window, timelineDock_->toggleViewAction(), Spec("window.timeline", timelineDock_->toggleViewAction()->text()).command);

    QMenu* help = menuBar()->addMenu(tr("&Help"));
    add(help, Spec("help.welcome", tr("&Welcome to NekoPhoto")).runs([this] { showWelcome(); }));
    add(help, Spec("help.about", tr("&About NekoPhoto")).runs([this] {
        QMessageBox::about(this, tr("About NekoPhoto"), tr("<b>NekoPhoto</b> %3<br>A layered photo editor and painting app for Linux. "
            "It began as a Linux port of <a href=\"https://github.com/robbietilton/Compositor\">Compositor</a> for macOS, and still opens its projects.<br><br>"
            "Qt %1 &middot; project format version %2<br><br>"
            "Free software under the GNU General Public License, version 3 or later, with ABSOLUTELY NO WARRANTY. "
            "Compositor's own code is MIT licensed by Wonder Assembly LLC; the licences of the bundled components "
            "are in THIRD-PARTY-NOTICES.md, installed with the program.").arg(QT_VERSION_STR).arg(projectFormatVersion).arg(QApplication::applicationVersion()));
    }));
    refreshRecent();
}

void MainWindow::convertColorMode(ColorMode colorMode) {
    if (!session_->hasDocument() || session_->document()->colorMode == colorMode) { refreshDepthGating(); return; }
    if (runCommand("image.mode", {{"colorMode", QString::fromLatin1(colorModeKey(colorMode))}}, tr("Mode"))) updateColorSwatches();
    refreshActions();
}

void MainWindow::convertMode(SampleType type) {
    if (!session_->hasDocument() || session_->sampleType() == type) { refreshDepthGating(); return; }
    // From 32 bits: HDR Toning, previewed on the canvas through the view (docs/bit-depth.md, "32 bits").
    std::optional<View32> toning;
    if (session_->sampleType() == SampleType::F32) {
        const View32 before = session_->view32();
        toning = askHdrToning(this, type == SampleType::U16 ? 16 : 8, View32(), [this](const View32& v) { session_->setView32(v); });
        session_->setView32(before);
        if (!toning) { refreshDepthGating(); return; }
    }
    QJsonObject params{{"bits", type == SampleType::F32 ? 32 : type == SampleType::U16 ? 16 : 8}};
    if (toning && !toning->isDefault()) {
        params["method"] = QString::fromLatin1(toneMethodKey(toning->method));
        params["exposure"] = toning->exposure;
        params["gamma"] = toning->gamma;
    }
    runCommand("image.mode", params, tr("Mode"));
    refreshActions();
}

void MainWindow::refreshDepthGating() {
    const bool has = session_ && session_->hasDocument();
    const bool deep = has && session_->featuresGated();
    // An action greyed for the depth says why ("... in 32-bit mode" for what Photoshop lacks there, else "... yet");
    // its own tooltip comes back at 8 bits.
    auto gate = [&](QAction* a, bool allowed, bool enabled, const std::string& feature = {}) {
        a->setEnabled(enabled && allowed);
        if (!allowed) {
            if (!a->property("depthTip").isValid()) a->setProperty("depthTip", a->toolTip());
            a->setToolTip(has ? session_->unavailableTip(feature) : QString());
        } else if (a->property("depthTip").isValid()) {
            a->setToolTip(a->property("depthTip").toString());
            a->setProperty("depthTip", QVariant());
        }
    };
    // The registry's commands: one that needs a document is greyed without one or where its feature is not supported
    // (saying so in its tooltip); any command is greyed while its own reason holds.
    std::set<QMenu*> reasonMenus;
    for (const Command* c : commands_->all()) {
        QAction* a = c->action;
        if (!a) continue;
        if (!c->needsDocument) {
            if (c->unavailable) a->setEnabled(!session_ || c->unavailable().isEmpty());
            continue;
        }
        const std::string feature = c->feature.toStdString();
        const bool allowed = !deep || (!feature.empty() && session_->supportsFeature(feature));
        const QString why = has && allowed && c->unavailable ? c->unavailable() : QString();
        gate(a, allowed, has && why.isEmpty(), feature);
        // Greyed for its own reason (no model, nothing to undo): the tooltip says it, and its menu shows tooltips.
        if (!why.isEmpty()) {
            if (!a->property("reasonTip").isValid()) a->setProperty("reasonTip", a->toolTip());
            a->setToolTip(why);
            if (auto* menu = qobject_cast<QMenu*>(a->parent())) reasonMenus.insert(menu);
        } else if (allowed && a->property("reasonTip").isValid()) {
            a->setToolTip(a->property("reasonTip").toString());
            a->setProperty("reasonTip", QVariant());
        }
    }
    for (auto it = toolActions_.begin(); it != toolActions_.end(); ++it) gate(it.value(), !deep || session_->toolSupportedAtDepth(it.key()), true, EditorSession::toolFeature(it.key()));
    gate(eraserAction_, !deep || session_->toolSupportedAtDepth(Tool::Brush), true, "tool.brush");
    // Menus show their items' tooltips while something in them is greyed for the depth.
    for (QMenu* menu : menuBar()->findChildren<QMenu*>()) menu->setToolTipsVisible(deep || reasonMenus.count(menu));
    const SampleType depth = has ? session_->sampleType() : SampleType::U8;
    const ColorMode colorMode = has ? session_->document()->colorMode : ColorMode::RGB;
    if (mode8Action_) {
        mode8Action_->setChecked(depth == SampleType::U8);
        mode16Action_->setChecked(depth == SampleType::U16);
        mode32Action_->setChecked(depth == SampleType::F32);
    }
    if (modeRgbAction_) {
        modeRgbAction_->setChecked(colorMode == ColorMode::RGB);
        modeCmykAction_->setChecked(colorMode == ColorMode::CMYK);
        modeLabAction_->setChecked(colorMode == ColorMode::Lab);
    }
    // 32 bits is RGB only, as in Photoshop: 32 Bits/Channel is greyed in a CMYK or Lab document, and CMYK Color and
    // Lab Color in a 32-bit one.
    if (has && mode32Action_ && colorMode != ColorMode::RGB) gate(mode32Action_, false, true, "document.mode");
    if (has && modeCmykAction_ && depth == SampleType::F32) for (QAction* a : {modeCmykAction_, modeLabAction_}) gate(a, false, true, "mode.cmykLab");
    refreshExposure();
}


} // namespace app
