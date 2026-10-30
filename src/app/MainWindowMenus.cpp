// The main window's tool rail and colour swatches, and buildMenus, which runs the builder of each menu (Menu<Name>.cpp).
#include "MainWindowMenus.h"
#include "CommandPalette.h"
#include "CanvasFrame.h"
#include "HdrDialogs.h"
#include "ColorManagement.h"
#include "ColorSwatches.h"
#include "ActionLibrary.h"
#include "WindowEdges.h"
#include <QActionGroup>
#include <QColorDialog>
#include <QMenuBar>
#include <QToolBar>
#include <QJsonObject>
#include <algorithm>
#include <set>

using namespace nekophoto;

namespace app {

using namespace menus;

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
        nekophoto::ToningKind toning; BlurToolMode blur;
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
                                                    .runs([this] {
                                                        // On a mask, white paints and black is the background, as in Photoshop.
                                                        if (session_->paintsMask()) setColors(Qt::white, Qt::black);
                                                        else setColors(Qt::black, Qt::white);
                                                    }, "colors.set").command);
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
        session_->toning.kind = nekophoto::ToningKind((int(session_->toning.kind) + 1) % 3);
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
    const nekophoto::ColorProfile profile = session_->hasDocument() ? session_->document()->profile : nekophoto::ColorProfile{};
    // On a mask they paint their greys, which Photoshop's swatches show there (the colours themselves are kept).
    if (session_->paintsMask()) {
        auto grey = [](const QColor& c) { const double g = EditorSession::maskGrey(c); return QColor::fromRgbF(float(g), float(g), float(g)); };
        swatches_->setColors(grey(session_->foregroundColor), grey(session_->backgroundColor));
        if (windowEdges_) windowEdges_->coloursChanged();
        return;
    }
    swatches_->setColors(color::displayColor(session_->foregroundColor, profile), color::displayColor(session_->backgroundColor, profile));
    if (windowEdges_) windowEdges_->coloursChanged();   // the left edge's Colour page, the recent colours
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

MainWindow::MenuContext::MenuContext(MainWindow& window)
    : w(window), reg(*window.commands_),
      fieldGuard(new ShortcutWatch(&window, [](QAction* a, const QKeySequence&) { if (!typingInField()) a->trigger(); })),
      hasLayer([&window] { return window.session_->activeLayerId() ? QString() : MainWindow::tr("No active layer"); }) {}

void MainWindow::buildMenus() {
    // The menus are the command registry's (CommandRegistry.h): each item is a command with an id, its label, its
    // keys and what it needs, added to its menu in order. A document command names the supports() feature it is;
    // without one it is 8-bit RGB only (greyed in a 16-bit document). One builder per menu of the menu bar, in its
    // order (Menu<Name>.cpp), sharing the MenuContext (MainWindowMenus.h).
    MenuContext m(*this);
    buildFileMenu(m);
    buildEditMenu(m);
    buildImageMenu(m);
    buildLayerMenu(m);
    buildTypeMenu(m);
    buildSelectMenu(m);
    buildFilterMenu(m);
    buildViewMenu(m);
    buildWindowMenu(m);
    buildHelpMenu(m);
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
