// The Tool Ring's data and commands (ToolRing.h): the rings per tool group, each key a command of the registry and the
// value it shows, and the tool option steps the rings run, registered as commands (no default keys; Edit > Keyboard
// Shortcuts can give them some). A later pass can make the rings the person's own: everything they say is here.
#include "ToolRing.h"
#include "BrushLibrary.h"
#include "CanvasWidget.h"
#include "CommandPalette.h"
#include "CommandRegistry.h"
#include "MainWindow.h"
#include "ToolOptionsBar.h"
#include <QAction>
#include <QKeySequence>
#include <QMenu>
#include <QSettings>
#include <algorithm>
#include <cmath>

using namespace nekophoto;

namespace app {

namespace toolring {

namespace {

/// Satellites as "key command icon value" rows, the value optional.
Slot slot(char key, const char* command, const char* icon, const char* value = "") {
    return Slot{key, QString::fromLatin1(command), QString::fromLatin1(icon), QString::fromLatin1(value)};
}

} // namespace

const std::vector<Ring>& rings() {
    // The compass rule: towards F and R (right, up) is more, towards S and V (left, down) less; each value takes one of
    // the pairs S/F, X/E, V/R. W and C, and the centre, hold actions.
    static const std::vector<Ring> all = {
        // Brush and Eraser: Photoshop's [ ] for the size, the opacity, the hardness (the brushes have no flow here),
        // Brush / Eraser, and the brush preset used before.
        {QStringLiteral("paint"),
         {slot('S', "tool.size.decrease", "scale", "size"), slot('F', "tool.size.increase", "scale", "size"),
          slot('V', "tool.opacity.decrease", "drop", "opacity"), slot('R', "tool.opacity.increase", "drop", "opacity"),
          slot('X', "tool.hardness.decrease", "dot-circle", "hardness"), slot('E', "tool.hardness.increase", "dot-circle", "hardness"),
          slot('C', "tool.brush.eraseMode", "eraser", "erase"), slot('W', "tool.brush.previousPreset", "history")},
         {}},
        // The other painting tools (Spot Healing, Clone Stamp, Blur / Smudge, Dodge / Burn): the same brush values.
        {QStringLiteral("retouch"),
         {slot('S', "tool.size.decrease", "scale", "size"), slot('F', "tool.size.increase", "scale", "size"),
          slot('V', "tool.opacity.decrease", "drop", "opacity"), slot('R', "tool.opacity.increase", "drop", "opacity"),
          slot('X', "tool.hardness.decrease", "dot-circle", "hardness"), slot('E', "tool.hardness.increase", "dot-circle", "hardness"),
          slot('W', "view.fitOnScreen", "fullscreen"), slot('C', "view.screenMode.panels", "layout-leftbar-close")},
         {}},
        // Marquee and Lasso: Feather, the four modes (New on top, Add up and Subtract down on the right, as more and
        // less), Inverse and Deselect; the centre is Quick Mask.
        {QStringLiteral("selection"),
         {slot('S', "tool.feather.decrease", "circle-dash", "feather"), slot('F', "tool.feather.increase", "circle-dash", "feather"),
          slot('E', "tool.selection.new", "square", "mode.replace"), slot('R', "tool.selection.add", "union", "mode.add"),
          slot('V', "tool.selection.subtract", "subtract", "mode.subtract"), slot('X', "tool.selection.intersect", "intersect", "mode.intersect"),
          slot('C', "select.inverse", "exclude"), slot('W', "select.deselect", "close-square")},
         QStringLiteral("select.quickMask")},
        // The Magic Wand: its Tolerance in Feather's place.
        {QStringLiteral("wand"),
         {slot('S', "tool.tolerance.decrease", "target", "tolerance"), slot('F', "tool.tolerance.increase", "target", "tolerance"),
          slot('E', "tool.selection.new", "square", "mode.replace"), slot('R', "tool.selection.add", "union", "mode.add"),
          slot('V', "tool.selection.subtract", "subtract", "mode.subtract"), slot('X', "tool.selection.intersect", "intersect", "mode.intersect"),
          slot('C', "select.inverse", "exclude"), slot('W', "select.deselect", "close-square")},
         QStringLiteral("select.quickMask")},
        // Quick Select: the stroke size, what a stroke marks (the subject up, the background down), starting again and
        // taking back the last stroke.
        {QStringLiteral("quickSelect"),
         {slot('S', "tool.size.decrease", "scale", "size"), slot('F', "tool.size.increase", "scale", "size"),
          slot('R', "tool.quickSelect.subject", "union", "subject"), slot('V', "tool.quickSelect.background", "subtract", "background"),
          slot('E', "tool.quickSelect.clear", "square"), slot('X', "tool.quickSelect.removeLast", "anticlockwise"),
          slot('C', "select.inverse", "exclude"), slot('W', "select.deselect", "close-square")},
         QStringLiteral("select.quickMask")},
        // The Move tool: turning a quarter left and right, stacking up and down, the flips, Free Transform, Auto-Select.
        {QStringLiteral("move"),
         {slot('S', "layer.rotate90CounterClockwise", "anticlockwise"), slot('F', "layer.rotate90Clockwise", "clockwise"),
          slot('R', "layer.bringForward", "arrow-to-up"), slot('V', "layer.sendBackward", "arrow-to-down"),
          slot('W', "layer.flipHorizontal", "flip-horizontal"), slot('X', "layer.flipVertical", "flip-vertical"),
          slot('E', "edit.freeTransform", "transformation"), slot('C', "tool.move.autoSelect", "cursor", "autoSelect")},
         {}},
        // The Type tool: the size, the recent fonts back and forward, bold, italic and the alignment.
        {QStringLiteral("type"),
         {slot('S', "tool.type.size.decrease", "font-size", "typeSize"), slot('F', "tool.type.size.increase", "font-size", "typeSize"),
          slot('X', "tool.type.font.previous", "skip-previous", "font"), slot('E', "tool.type.font.next", "skip-forward", "font"),
          slot('W', "tool.type.bold", "bold", "bold"), slot('R', "tool.type.italic", "italic", "italic"),
          slot('C', "tool.type.alignment", "align-center", "alignment")},
         {}},
        // The Paint Bucket: Tolerance, opacity, Contiguous, All Layers and the colours.
        {QStringLiteral("bucket"),
         {slot('S', "tool.tolerance.decrease", "target", "tolerance"), slot('F', "tool.tolerance.increase", "target", "tolerance"),
          slot('V', "tool.opacity.decrease", "drop", "opacity"), slot('R', "tool.opacity.increase", "drop", "opacity"),
          slot('W', "tool.bucket.contiguous", "link", "contiguous"), slot('E', "tool.bucket.allLayers", "layer", "allLayers"),
          slot('X', "colors.swap", "transfer"), slot('C', "colors.default", "palette")},
         {}},
        // The Gradient: the zoom, its opacity, Reverse and the colours.
        {QStringLiteral("gradient"),
         {slot('S', "view.zoomOut", "zoom-out", "zoom"), slot('F', "view.zoomIn", "zoom-in", "zoom"),
          slot('V', "tool.opacity.decrease", "drop", "opacity"), slot('R', "tool.opacity.increase", "drop", "opacity"),
          slot('W', "tool.gradient.reverse", "arrow-left-right", "reverse"), slot('E', "view.fitOnScreen", "fullscreen"),
          slot('X', "colors.swap", "transfer"), slot('C', "colors.default", "palette")},
         {}},
        // Every other tool (Crop, Slice, Artboard, Pen, Direct Selection, Shape, Eyedropper, Hand, Zoom): the view and
        // the colours.
        {QStringLiteral("generic"),
         {slot('S', "view.zoomOut", "zoom-out", "zoom"), slot('F', "view.zoomIn", "zoom-in", "zoom"),
          slot('E', "view.fitOnScreen", "fullscreen"), slot('R', "view.actualPixels", "aspect-ratio"),
          slot('X', "colors.swap", "transfer"), slot('C', "colors.default", "palette"),
          slot('W', "view.screenMode.panels", "layout-leftbar-close")},
         {}},
    };
    return all;
}

const Ring& ringFor(Tool tool) {
    QString id;
    switch (tool) {
    case Tool::Brush: id = QStringLiteral("paint"); break;
    case Tool::SpotHealing: case Tool::CloneStamp: case Tool::Smudge: case Tool::Dodge: id = QStringLiteral("retouch"); break;
    case Tool::Marquee: case Tool::Lasso: id = QStringLiteral("selection"); break;
    case Tool::Wand: id = QStringLiteral("wand"); break;
    case Tool::Scribble: id = QStringLiteral("quickSelect"); break;
    case Tool::Move: id = QStringLiteral("move"); break;
    case Tool::Text: id = QStringLiteral("type"); break;
    case Tool::PaintBucket: id = QStringLiteral("bucket"); break;
    case Tool::Gradient: id = QStringLiteral("gradient"); break;
    default: id = QStringLiteral("generic"); break;
    }
    for (const Ring& r : rings()) if (r.id == id) return r;
    return rings().back();
}

char oppositeKey(char key) {
    switch (key) {
    case 'S': return 'F';
    case 'F': return 'S';
    case 'X': return 'E';
    case 'E': return 'X';
    case 'V': return 'R';
    case 'R': return 'V';
    default: return 0;
    }
}

bool isMoreKey(char key) { return key == 'F' || key == 'E' || key == 'R'; }

} // namespace toolring

namespace {

bool brushLike(Tool t) { return t == Tool::Brush || t == Tool::SpotHealing || t == Tool::CloneStamp || t == Tool::Smudge || t == Tool::Dodge; }

/// A painting stroke (or a smudge, a moved selection) under way: the brush values wait until it ends.
bool stroking(const EditorSession& s) { return s.brushActive() || s.warpActive() || s.pixelMoveActive(); }

/// Photoshop's Feather steps, so a few presses go from a hard edge to a soft one.
constexpr double featherSteps[] = {0, 1, 2, 3, 5, 8, 10, 15, 20, 30, 50, 75, 100, 150, 200, 250};

double stepFeather(double now, bool more) {
    if (more) { for (double f : featherSteps) if (f > now + 1e-9) return f; return 250; }
    for (auto it = std::rbegin(featherSteps); it != std::rend(featherSteps); ++it) if (*it < now - 1e-9) return *it;
    return 0;
}

/// Opacity in tens (Photoshop's digit keys), down to 1%.
double stepOpacity(double now, bool more) {
    const double percent = std::round(now * 100);
    const double stepped = more ? std::floor(percent / 10 + 1e-9) * 10 + 10 : std::ceil(percent / 10 - 1e-9) * 10 - 10;
    return std::clamp(stepped, 1.0, 100.0) / 100;
}

int stepTolerance(int now, bool more) { return std::clamp(more ? (now / 5 + 1) * 5 : ((now + 4) / 5 - 1) * 5, 0, 255); }

} // namespace

void ToolRing::registerCommands(CommandRegistry& registry, QMenu* viewMenu) {
    registry_ = &registry;
    MainWindow* w = window_;
    // The current tab's session, canvas and options bar (the ring follows the tab on screen).
    auto session = [w]() -> EditorSession& { return *w->session_; };
    auto add = [&](const char* id, const QString& label, std::function<void()> run, std::function<QString()> unavailable) {
        Command c;
        c.id = QString::fromLatin1(id);
        c.label = label;
        // A key given in Edit > Keyboard Shortcuts runs it even when its reason holds: the step checks again.
        c.run = [run, unavailable] { if (!unavailable || unavailable().isEmpty()) run(); };
        c.unavailable = std::move(unavailable);
        QAction* a = registry.add(nullptr, std::move(c));
        a->setAutoRepeat(true);
        ownCommands_.push_back(a);
        return a;
    };
    auto onlyFor = [session](std::function<bool(Tool)> applies, QString why) {
        return [session, applies, why] { return applies(session().tool()) ? QString() : why; };
    };
    const QString notWhilePainting = tr("Not while a stroke is being painted");

    // View > Tool Ring (Alt+D).
    {
        Command c;
        c.id = QStringLiteral("view.toolRing");
        c.label = tr("&Tool Ring");
        c.shortcuts = {QKeySequence(QStringLiteral("Alt+D"))};
        c.run = [this] { toggle(); };
        c.unavailable = [] { return toolring::enabled() ? QString() : tr("The Tool Ring is turned off in Edit > Preferences"); };
        registry.add(viewMenu, std::move(c))->setObjectName(QStringLiteral("command.view.toolRing"));
    }

    // ---- Brush values: size, hardness, opacity (Photoshop's [ ], { } and the digit keys, a step at a time)
    auto sizeReason = [session, notWhilePainting] {
        const EditorSession& s = session();
        if (!brushLike(s.tool()) && s.tool() != Tool::Scribble) return tr("This tool has no brush size");
        return stroking(s) ? notWhilePainting : QString();
    };
    auto stepSize = [session](bool more) {
        EditorSession& s = session();
        if (s.tool() == Tool::Scribble) {
            const int now = s.scribbleSize;
            s.scribbleSize = std::clamp(more ? std::max(now + 1, int(std::lround(now * 1.2))) : std::min(now - 1, int(std::lround(now / 1.2))), 1, 500);
            emit s.toolChanged();
            return;
        }
        s.changeBrushSize(more);
    };
    add("tool.size.decrease", tr("Decrease Brush Size"), [stepSize] { stepSize(false); }, sizeReason);
    add("tool.size.increase", tr("Increase Brush Size"), [stepSize] { stepSize(true); }, sizeReason);
    auto hardnessReason = [session, notWhilePainting] {
        const EditorSession& s = session();
        if (!brushLike(s.tool())) return tr("This tool has no brush hardness");
        if (s.tool() == Tool::Brush && !s.brushPreset.isEmpty()) return tr("This brush preset brings its own hardness");
        return stroking(s) ? notWhilePainting : QString();
    };
    add("tool.hardness.decrease", tr("Decrease Brush Hardness"), [session] { session().changeBrushHardness(false); }, hardnessReason);
    add("tool.hardness.increase", tr("Increase Brush Hardness"), [session] { session().changeBrushHardness(true); }, hardnessReason);
    auto opacityReason = [session, notWhilePainting] {
        const EditorSession& s = session();
        if (!brushLike(s.tool()) && s.tool() != Tool::PaintBucket && s.tool() != Tool::Gradient) return tr("This tool has no opacity");
        return stroking(s) ? notWhilePainting : QString();
    };
    auto stepOpacityOf = [session](bool more) {
        EditorSession& s = session();
        if (s.tool() == Tool::Gradient) {
            s.gradientSettings.opacity = stepOpacity(s.gradientSettings.opacity, more);
            s.refreshGradient();
        } else {
            s.brushSettings.opacity = stepOpacity(s.brushSettings.opacity, more);
        }
        emit s.toolChanged();
    };
    add("tool.opacity.decrease", tr("Decrease Tool Opacity"), [stepOpacityOf] { stepOpacityOf(false); }, opacityReason);
    add("tool.opacity.increase", tr("Increase Tool Opacity"), [stepOpacityOf] { stepOpacityOf(true); }, opacityReason);
    // Brush / Eraser: the Brush's erase mode, as the rail's two tools (the ring stays open across it).
    add("tool.brush.eraseMode", tr("Switch between Brush and Eraser"), [w] {
        if (w->session_->brushErase) { if (QAction* brush = w->toolActions_.value(Tool::Brush)) brush->trigger(); }
        else w->eraserAction_->trigger();
    }, onlyFor([](Tool t) { return t == Tool::Brush; }, tr("Only the Brush and the Eraser")));
    add("tool.brush.previousPreset", tr("Previous Brush Preset"), [this, session] {
        EditorSession& s = session();
        if (!presetBefore_) return;
        s.brushPreset = *presetBefore_;
        // A preset starts at its own size, as when it is picked in the brush picker.
        if (const BrushPreset* preset = BrushLibrary::find(s.brushPreset)) s.brushSettings.diameter = preset->diameter;
        emit s.toolChanged();
    }, [this, session] {
        if (session().tool() != Tool::Brush) return tr("Only the Brush and the Eraser");
        return presetBefore_ ? QString() : tr("No other brush preset has been used yet");
    });

    // ---- Selection tools: the mode, Feather, Tolerance; Quick Select's marks
    auto selectionModes = onlyFor([](Tool t) { return t == Tool::Marquee || t == Tool::Lasso || t == Tool::Wand; }, tr("Only the Marquee, Lasso and Magic Wand have a selection mode"));
    auto mode = [session](SelectionMode m) { return [session, m] { session().selectionToolMode = m; emit session().toolChanged(); }; };
    add("tool.selection.new", tr("New Selection"), mode(SelectionMode::Replace), selectionModes);
    add("tool.selection.add", tr("Add to Selection"), mode(SelectionMode::Add), selectionModes);
    add("tool.selection.subtract", tr("Subtract from Selection"), mode(SelectionMode::Subtract), selectionModes);
    add("tool.selection.intersect", tr("Intersect with Selection"), mode(SelectionMode::Intersect), selectionModes);
    auto featherReason = onlyFor([](Tool t) { return t == Tool::Marquee || t == Tool::Lasso; }, tr("Only the Marquee and the Lasso have a Feather (Select > Modify > Feather softens a selection)"));
    auto stepFeatherOf = [session](bool more) { EditorSession& s = session(); s.selectionToolFeather = stepFeather(s.selectionToolFeather, more); emit s.toolChanged(); };
    add("tool.feather.decrease", tr("Decrease Feather"), [stepFeatherOf] { stepFeatherOf(false); }, featherReason);
    add("tool.feather.increase", tr("Increase Feather"), [stepFeatherOf] { stepFeatherOf(true); }, featherReason);
    auto toleranceReason = onlyFor([](Tool t) { return t == Tool::Wand || t == Tool::PaintBucket; }, tr("Only the Magic Wand and the Paint Bucket have a Tolerance"));
    auto stepToleranceOf = [session](bool more) {
        EditorSession& s = session();
        if (s.tool() == Tool::PaintBucket) s.bucket.tolerance = stepTolerance(s.bucket.tolerance, more);
        else {
            s.wandTolerance = stepTolerance(s.wandTolerance, more);
            if (s.wandEdgeAware) s.retolerateWand(s.wandTolerance);   // as the field: the last click's selection follows
        }
        emit s.toolChanged();
    };
    add("tool.tolerance.decrease", tr("Decrease Tolerance"), [stepToleranceOf] { stepToleranceOf(false); }, toleranceReason);
    add("tool.tolerance.increase", tr("Increase Tolerance"), [stepToleranceOf] { stepToleranceOf(true); }, toleranceReason);
    auto quickSelect = onlyFor([](Tool t) { return t == Tool::Scribble; }, tr("Only Quick Select"));
    add("tool.quickSelect.subject", tr("Quick Select: Mark the Subject"), [session] { session().scribbleBackground = false; emit session().toolChanged(); }, quickSelect);
    add("tool.quickSelect.background", tr("Quick Select: Mark the Background"), [session] { session().scribbleBackground = true; emit session().toolChanged(); }, quickSelect);
    add("tool.quickSelect.clear", tr("Quick Select: Clear the Strokes"), [session] { session().clearScribbles(); session().clearClickPrompts(); }, quickSelect);
    add("tool.quickSelect.removeLast", tr("Quick Select: Remove the Last Stroke"), [session] {
        EditorSession& s = session();
        if (s.quickSelectClicks) s.removeLastClickPrompt(); else s.removeLastScribble();
    }, quickSelect);

    // ---- The Move tool's Auto-Select
    add("tool.move.autoSelect", tr("Auto-Select Layer"), [session] { session().transformAutoSelect = !session().transformAutoSelect; emit session().toolChanged(); },
        onlyFor([](Tool t) { return t == Tool::Move; }, tr("Only the Move tool")));

    // ---- The Type tool: as the options bar's fields (the active type layer takes each change, one undo step)
    auto typeOnly = onlyFor([](Tool t) { return t == Tool::Text; }, tr("Only the Type tool"));
    auto stepType = [w](double delta) {
        // The selected type layers, as Ctrl+Shift+> and <; with none, the size new type takes.
        if (w->canvas_->stepLayerTypeSize(delta)) return;
        EditorSession& s = *w->session_;
        s.textStyle.fontSize = std::clamp(s.textStyle.fontSize + delta, 1.0, 2000.0);
        emit s.toolChanged();
    };
    add("tool.type.size.decrease", tr("Decrease Type Size"), [stepType] { stepType(-2); }, typeOnly);
    add("tool.type.size.increase", tr("Increase Type Size"), [stepType] { stepType(2); }, typeOnly);
    // The recent fonts (the font list's Recent group, newest first): back is older, forward newer.
    auto recentFonts = [] { return QSettings().value(QStringLiteral("text/recentFonts")).toStringList(); };
    auto stepFont = [this, w, recentFonts](int by) {
        const QStringList recent = recentFonts();
        if (recent.isEmpty()) return;
        const qsizetype at = recent.indexOf(valueText(QStringLiteral("fontFamily")));
        const qsizetype next = at < 0 ? 0 : std::clamp<qsizetype>(at + by, 0, recent.size() - 1);
        const std::string family = recent[next].toStdString();
        w->options_->applyTextStyle([family](LayerText& t) { t.fontFamily = family; });
        emit w->session_->toolChanged();
    };
    auto fontReason = [typeOnly, recentFonts] {
        if (QString why = typeOnly(); !why.isEmpty()) return why;
        return recentFonts().isEmpty() ? tr("No recent fonts yet: choose one in the options bar's font list") : QString();
    };
    add("tool.type.font.previous", tr("Previous Recent Font"), [stepFont] { stepFont(1); }, fontReason);
    add("tool.type.font.next", tr("Next Recent Font"), [stepFont] { stepFont(-1); }, fontReason);
    add("tool.type.bold", tr("Bold"), [this, w] {
        const bool on = !valueChecked(QStringLiteral("bold"));
        w->options_->applyTextStyle([on](LayerText& t) { t.bold = on; });
        emit w->session_->toolChanged();
    }, typeOnly);
    add("tool.type.italic", tr("Italic"), [this, w] {
        const bool on = !valueChecked(QStringLiteral("italic"));
        w->options_->applyTextStyle([on](LayerText& t) { t.italic = on; });
        emit w->session_->toolChanged();
    }, typeOnly);
    add("tool.type.alignment", tr("Next Text Alignment"), [w] {
        const Layer* l = w->session_->activeLayer();
        const int now = l && l->isLiveText() ? l->text->alignment : w->session_->textStyle.alignment;
        const int next = (std::clamp(now, 0, 2) + 1) % 3;   // left, centre, right
        w->options_->applyTextStyle([next](LayerText& t) { t.alignment = next; });
        emit w->session_->toolChanged();
    }, typeOnly);

    // ---- Paint Bucket and Gradient options
    auto bucketOnly = onlyFor([](Tool t) { return t == Tool::PaintBucket; }, tr("Only the Paint Bucket"));
    add("tool.bucket.contiguous", tr("Paint Bucket: Contiguous"), [session] { session().bucket.contiguous = !session().bucket.contiguous; emit session().toolChanged(); }, bucketOnly);
    add("tool.bucket.allLayers", tr("Paint Bucket: All Layers"), [session] { session().bucket.allLayers = !session().bucket.allLayers; emit session().toolChanged(); }, bucketOnly);
    add("tool.gradient.reverse", tr("Gradient: Reverse"), [session] {
        EditorSession& s = session();
        s.gradientSettings.reversed = !s.gradientSettings.reversed;
        s.refreshGradient();
        emit s.toolChanged();
    }, onlyFor([](Tool t) { return t == Tool::Gradient; }, tr("Only the Gradient tool")));
}

QString ToolRing::valueText(const QString& value) const {
    const EditorSession* s = window_->session_;
    if (!s) return {};
    const Tool tool = s->tool();
    auto percent = [](double v) { return QStringLiteral("%1%").arg(int(std::lround(v * 100))); };
    auto pixels = [](double v) { return tr("%1 px").arg(v < 10 && std::abs(v - std::round(v)) > 1e-6 ? QString::number(v, 'f', 1) : QString::number(std::lround(v))); };
    const Layer* active = s->activeLayer();
    const LayerText* text = active && active->isLiveText() && active->text ? &*active->text : nullptr;
    if (value == QLatin1String("size")) return tool == Tool::Scribble ? pixels(s->scribbleSize) : brushLike(tool) ? pixels(s->brushSettings.diameter) : QString();
    if (value == QLatin1String("hardness")) return brushLike(tool) && !(tool == Tool::Brush && !s->brushPreset.isEmpty()) ? percent(s->brushSettings.hardness) : QString();
    if (value == QLatin1String("opacity")) return tool == Tool::Gradient ? percent(s->gradientSettings.opacity) : percent(s->brushSettings.opacity);
    if (value == QLatin1String("feather")) return pixels(s->selectionToolFeather);
    if (value == QLatin1String("tolerance")) return QString::number(tool == Tool::PaintBucket ? s->bucket.tolerance : s->wandTolerance);
    if (value == QLatin1String("zoom")) return QStringLiteral("%1%").arg(s->viewport.zoom * 100 >= 10 ? QString::number(std::lround(s->viewport.zoom * 100)) : QString::number(s->viewport.zoom * 100, 'f', 1));
    if (value == QLatin1String("typeSize")) return pixels(text ? text->fontSize : s->textStyle.fontSize);
    if (value == QLatin1String("fontFamily") || value == QLatin1String("font")) {
        const QString family = QString::fromStdString(text ? text->fontFamily : s->textStyle.fontFamily);
        return family;
    }
    if (value == QLatin1String("alignment")) {
        const int a = std::clamp(text ? text->alignment : s->textStyle.alignment, 0, 2);
        return a == 0 ? tr("Left") : a == 1 ? tr("Centre") : tr("Right");
    }
    return {};
}

bool ToolRing::valueChecked(const QString& value) const {
    const EditorSession* s = window_->session_;
    if (!s) return false;
    const Layer* active = s->activeLayer();
    const LayerText* text = active && active->isLiveText() && active->text ? &*active->text : nullptr;
    if (value == QLatin1String("erase")) return s->tool() == Tool::Brush && s->brushErase;
    if (value == QLatin1String("mode.replace")) return s->selectionToolMode == SelectionMode::Replace;
    if (value == QLatin1String("mode.add")) return s->selectionToolMode == SelectionMode::Add;
    if (value == QLatin1String("mode.subtract")) return s->selectionToolMode == SelectionMode::Subtract;
    if (value == QLatin1String("mode.intersect")) return s->selectionToolMode == SelectionMode::Intersect;
    if (value == QLatin1String("subject")) return !s->scribbleBackground;
    if (value == QLatin1String("background")) return s->scribbleBackground;
    if (value == QLatin1String("autoSelect")) return s->transformAutoSelect;
    if (value == QLatin1String("bold")) return text ? text->bold : s->textStyle.bold;
    if (value == QLatin1String("italic")) return text ? text->italic : s->textStyle.italic;
    if (value == QLatin1String("contiguous")) return s->bucket.contiguous;
    if (value == QLatin1String("allLayers")) return s->bucket.allLayers;
    if (value == QLatin1String("reverse")) return s->gradientSettings.reversed;
    return false;
}

} // namespace app
