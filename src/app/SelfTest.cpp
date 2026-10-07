// In-app checks that need the whole window (run by ctest through `nekophoto --self-test <name>`).
//
// command-path: the converted commands (New Layer, Duplicate Layer, Merge Down, Gaussian Blur's OK, Levels' OK and
// Free Transform's commit) give the same document and the same history whether they come from the interface
// (menu actions, the dialogs, the canvas's Enter) or from automation requests, and an action recording the
// interface path holds the requests automation would send. Then the same for the menu items, dialogs and panel
// buttons converted after them (menuCommands): each through the menu bar (answering its dialog), through the
// requests the recording holds, and through the session calls the interface made before, compared as documents and
// history names, with each recorded once as the method it names. The file commands (New, Open, Import File, Save As,
// Edit Contents) through their dialogs (fileCommands), and the command registry: every menu item a command with an id
// of its own, every key one action's (registry).
//
// held-keys: Photoshop's held tools (Alt, Ctrl, Ctrl+Space), spring-loaded tool letters, F7, F12, Ctrl+Alt+Z and the
// type size keys, through synthesised key and mouse events.
#include "SelfTest.h"
#include "ActionLibrary.h"
#include "Autosave.h"
#include "PreferencesDialog.h"
#include "AdjustmentEditor.h"
#include "Automation.h"
#include "CameraRawDialog.h"
#include "CameraRawPanels.h"
#include "CanvasWidget.h"
#include "ChannelDialogs.h"
#include "ColorManagement.h"
#include "ChannelsPanel.h"
#include "CommandPalette.h"
#include "CommandRegistry.h"
#include "ContentAwareScaleDialog.h"
#include "Gmic.h"
#include "GmicDialog.h"
#include "LayerStyleDialog.h"
#include "MoshDialog.h"
#include "PresetLibrary.h"
#include "WarpDialog.h"
#include "compositor/presets.h"
#include "compositor/vectorlayer.h"
#include "compositor/warpmesh.h"
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QListWidget>
#include <QPushButton>
#include <QRegularExpression>
#include <QToolBar>
#include <set>
#include "ExportAsDialog.h"
#include "FilterDialog.h"
#include "LayersPanel.h"
#include "ModelStore.h"
#include "Names.h"
#include "PathsPanel.h"
#include "MainWindow.h"
#include "Ruler.h"
#include <QApplication>
#include <QDoubleSpinBox>
#include <QComboBox>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QSlider>
#include <QSpinBox>
#include <QTreeWidgetItemIterator>
#include <QThread>
#include <QTimer>
#include <QTemporaryDir>
#include <QImageReader>
#include <QSettings>
#include <QFileInfo>
#include <QToolButton>
#include <functional>
#include <memory>
#include <QJsonArray>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QAbstractButton>
#include <QDir>
#include <QDockWidget>
#include <QFile>
#include <cmath>
#include <cstdio>

// QtGui's shortcut pass for a key press (what QTest's key clicks use): the focus widget's ShortcutOverride, then the
// window's shortcuts; true when a shortcut took the key.
Q_GUI_EXPORT bool qt_sendShortcutOverrideEvent(QObject* o, ulong timestamp, int k, Qt::KeyboardModifiers mods, const QString& text, bool autorep, ushort count);

using namespace compositor;

namespace app {

void buildDemoDocument(EditorSession& session);   // main.cpp

namespace {

uint64_t hashBytes(const uint8_t* data, size_t size, uint64_t h = 1469598103934665603ull) {
    for (size_t i = 0; i < size; i++) { h ^= data[i]; h *= 1099511628211ull; }
    return h;
}

/// The document as text, without its random ids: every layer's name, kind, placement, look and pixels, the
/// selection, the guides, the composite, and the history's step names.
QString describe(EditorSession& s) {
    QStringList out;
    const Document& d = *s.document();
    for (const Layer& l : d.layers) {
        uint64_t pixels = 0, mask = 0;
        if (l.asset && l.asset->image && l.asset->image.u8()) pixels = hashBytes(l.asset->image.u8()->data(), l.asset->image.u8()->byteCount());
        if (l.mask && l.mask->asset.image.u8()) mask = hashBytes(l.mask->asset.image.u8()->data(), l.mask->asset.image.u8()->byteCount());
        const Layer* parent = l.parentId ? d.find(*l.parentId) : nullptr;
        out << QString("layer %1 in %2 group %3 at %4,%5 %6x%7 rot %8 flip %9%10 opacity %11 blend %12 visible %13 pixels %14 %15x%16 mask %17")
                   .arg(QString::fromStdString(l.name), parent ? QString::fromStdString(parent->name) : QStringLiteral("-")).arg(l.isGroup)
                   .arg(l.transform.origin.x, 0, 'g', 17).arg(l.transform.origin.y, 0, 'g', 17).arg(l.transform.size.width, 0, 'g', 17).arg(l.transform.size.height, 0, 'g', 17)
                   .arg(l.transform.rotation, 0, 'g', 17).arg(l.transform.flipX).arg(l.transform.flipY).arg(l.opacity).arg(int(l.blendMode)).arg(l.visible)
                   .arg(pixels).arg(l.asset && l.asset->image ? l.asset->image.width() : 0).arg(l.asset && l.asset->image ? l.asset->image.height() : 0).arg(mask);
    }
    // Clipping, resampling, smart objects and a mask's state, which the pixels alone do not show.
    for (const Layer& l : d.layers)
        out << QString("  %1: clipped %2 sampling %3 smart %4 mask on %5 adjustment %6").arg(QString::fromStdString(l.name)).arg(l.maskSourceId.has_value()).arg(int(l.transform.sampling))
                   .arg(l.smartObject.has_value()).arg(l.mask ? l.mask->enabled : false).arg(l.adjustment.has_value());
    uint64_t coverage = 0;
    if (d.selection && d.selection->coverage.u8()) coverage = hashBytes(d.selection->coverage.u8()->data(), d.selection->coverage.u8()->byteCount());
    out << QString("selection %1 %2").arg(d.selection.has_value()).arg(coverage);
    out << QString("size %1x%2 mode %3 depth %4").arg(d.width).arg(d.height).arg(int(d.colorMode)).arg(int(d.sampleType));
    QStringList paths, channels;
    for (const DocumentPath& path : s.paths()) paths << QString::fromStdString(path.name) + ":" + QString::number(path.path.subpaths.size());
    for (const Channel& c : d.channels) channels << QString::fromStdString(c.name) + ":" + QString::number(c.image.u8() ? hashBytes(c.image.u8()->data(), c.image.u8()->byteCount()) : 0);
    out << "paths " + paths.join(", ");
    out << "channels " + channels.join(", ");
    out << QString("guides %1").arg(d.guides.size());
    // The colours, the vector masks and the effects, which the pixels alone do not show either.
    out << QString("colors %1 %2").arg(s.foregroundColor.name(QColor::HexArgb), s.backgroundColor.name(QColor::HexArgb));
    for (const Layer& l : d.layers)
        if (hasLayerVectorMask(l) || hasAnyEffect(s.layerStyle(l.id)))
            out << QString("  %1: vector mask %2 style %3").arg(QString::fromStdString(l.name)).arg(hasLayerVectorMask(l)).arg(QString::fromStdString(layerStyleToJson(s.layerStyle(l.id))));
    if (auto flat = s.flattened()) out << QString("composite %1").arg(hashBytes(flat->data(), flat->byteCount()));
    QStringList history;
    for (const std::string& n : s.undoNames()) history << QString::fromStdString(n);
    out << "history " + history.join(" | ");
    return out.join('\n');
}

bool select(EditorSession& s, const char* name) {
    for (const Layer& l : s.document()->layers) if (l.name == name) { s.selectLayer(l.id); return true; }
    std::fprintf(stderr, "no layer named %s\n", name);
    return false;
}

QAction* action(MainWindow& w, const char* name) {
    QAction* a = w.findChild<QAction*>(QString::fromLatin1(name));
    if (!a) std::fprintf(stderr, "no action %s\n", name);
    return a;
}

AdjustmentSettings levelsSettings() {
    AdjustmentSettings levels = AdjustmentSettings::defaults(AdjustmentKind::Levels);
    levels.levels.ranges[0].black = 18;
    levels.levels.ranges[0].gamma = 1.3;
    levels.levels.ranges[0].white = 230;
    return levels;
}

constexpr double blurRadius = 3.5;
/// The style preset Layer > Layer Style > Apply Style applies (imported for the test, removed after it).
constexpr const char* kStylePreset = "Self-test Shadow";
const LayerTransform moved(const LayerTransform& t) {
    LayerTransform out = t;
    out.origin.x += 13;
    out.origin.y -= 7;
    out.rotation = 15;
    return out;
}



int commandPath(MainWindow& w) {
    AutomationServer* engine = w.automationEngine();
    auto call = [engine](const QString& method, const QJsonObject& params = {}) {
        const QJsonObject reply = engine->handle(QJsonObject{{"jsonrpc", "2.0"}, {"id", 1}, {"method", method}, {"params", params}});
        if (reply.contains("error")) std::fprintf(stderr, "%s: %s\n", qPrintable(method), qPrintable(reply.value("error").toObject().value("message").toString()));
        return reply.value("result");
    };

    // The interface, recorded as an action.
    EditorSession& a = *w.session();
    buildDemoDocument(a);
    // Free Transform with a selection moves the selected pixels; these steps are about the layer.
    a.deselect();
    ActionLibrary::instance().startRecording(QStringLiteral("command-path self-test"));
    if (!select(a, "Paint")) return 1;
    QAction* add = action(w, "command.layers.add");
    QAction* duplicate = action(w, "command.layers.duplicate");
    QAction* merge = action(w, "command.layers.merge");
    QAction* transform = action(w, "command.transform");
    if (!add || !duplicate || !merge || !transform) return 1;
    add->trigger();
    if (!select(a, "Red")) return 1;
    duplicate->trigger();
    merge->trigger();
    if (!select(a, "Background")) return 1;
    {
        auto* blur = new FilterDialog(&a, FilterKind::GaussianBlur, &w);
        blur->findChild<QDoubleSpinBox*>()->setValue(blurRadius);
        blur->accept();
    }
    {
        auto* levels = new PixelAdjustmentDialog(&a, AdjustmentKind::Levels, &w);
        levels->findChild<AdjustmentEditor*>()->setSettings(levelsSettings());
        levels->accept();
    }
    if (!select(a, "Paint")) return 1;
    transform->trigger();
    if (!a.transformEdit()) { std::fprintf(stderr, "Free Transform did not start\n"); return 1; }
    a.previewTransform(moved(a.transformEdit()->draft));
    {
        CanvasWidget* canvas = w.canvasAt(w.currentTabIndex());
        QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
        QApplication::sendEvent(canvas, &enter);
    }
    QApplication::processEvents();
    ActionLibrary::instance().stopRecording();
    QStringList recorded;
    if (const RecordedAction* r = ActionLibrary::instance().find(QStringLiteral("command-path self-test")))
        for (const ActionStep& step : r->steps) recorded << step.method;
    ActionLibrary::instance().remove(QStringLiteral("command-path self-test"));
    const QString viaInterface = describe(a);

    // The same through automation, in a tab of its own.
    w.newTab();
    EditorSession& b = *w.session();
    buildDemoDocument(b);
    b.deselect();
    select(b, "Paint");
    call("layers.add");
    select(b, "Red");
    call("layers.duplicate");
    call("layers.merge");
    select(b, "Background");
    call("pixels.filter", {{"kind", "Gaussian Blur"}, {"radius", blurRadius}});
    call("pixels.adjust", {{"kind", "Levels"}, {"settings", QJsonDocument::fromJson(QByteArray::fromStdString(levelsSettings().toJson())).object()}});
    select(b, "Paint");
    const LayerTransform target = moved(b.activeLayer()->transform);
    call("layers.setTransform", {{"x", target.origin.x}, {"y", target.origin.y}, {"width", target.size.width}, {"height", target.size.height}, {"rotation", target.rotation}});
    const QString viaAutomation = describe(b);

    // And as the interface did it before the command path: the session's own calls, the dialogs committing their
    // own result, the transform committed directly. Behaviour must not have changed.
    w.newTab();
    EditorSession& c = *w.session();
    c.commandReady = [] { return false; };
    buildDemoDocument(c);
    c.deselect();
    select(c, "Paint");
    c.addBlankLayer();
    select(c, "Red");
    c.duplicateActiveLayer();
    c.mergeLayers();
    select(c, "Background");
    {
        auto* blur = new FilterDialog(&c, FilterKind::GaussianBlur, &w);
        blur->findChild<QDoubleSpinBox*>()->setValue(blurRadius);
        blur->accept();
    }
    {
        auto* levels = new PixelAdjustmentDialog(&c, AdjustmentKind::Levels, &w);
        levels->findChild<AdjustmentEditor*>()->setSettings(levelsSettings());
        levels->accept();
    }
    select(c, "Paint");
    c.transformCommand();
    if (c.transformEdit()) c.previewTransform(moved(c.transformEdit()->draft));
    c.commitTransformCommand();
    const QString direct = describe(c);

    int failures = 0;
    if (viaInterface != viaAutomation) {
        std::fprintf(stderr, "the interface and automation differ:\n--- interface\n%s\n--- automation\n%s\n", qPrintable(viaInterface), qPrintable(viaAutomation));
        failures++;
    }
    if (viaInterface != direct) {
        std::fprintf(stderr, "the command path changed what the interface does:\n--- now\n%s\n--- before\n%s\n", qPrintable(viaInterface), qPrintable(direct));
        failures++;
    }
    const QStringList expected{"layers.add", "layers.duplicate", "layers.merge", "pixels.filter", "pixels.adjust", "layers.setTransform"};
    if (recorded != expected) {
        std::fprintf(stderr, "recorded %s, expected %s\n", qPrintable(recorded.join(", ")), qPrintable(expected.join(", ")));
        failures++;
    }
    // Every step happened: six named steps after the demo's.
    for (const char* name : {"New Blank Layer", "Duplicate Layer", "Merge Down", "Gaussian Blur", "Levels", "Transform Layer"})
        if (!viaInterface.contains(QString::fromLatin1(name))) { std::fprintf(stderr, "no \"%s\" step in the history\n", name); failures++; }
    std::printf("command-path: %s\n", failures ? "FAILED" : "ok");
    return failures ? 1 : 0;
}

// ---- The menu items, dialogs and panel buttons converted after the first six ---------------------------------

/// A menu bar item by its path of titles ("Layer", "Layer Mask", "Invert"), mnemonics and shortcuts left out.
QAction* menuItem(MainWindow& w, const QStringList& path) {
    QList<QAction*> actions = w.menuBar()->actions();
    QAction* found = nullptr;
    for (int i = 0; i < path.size(); i++) {
        found = nullptr;
        for (QAction* a : actions)
            if (a->text().remove('&').section('\t', 0, 0) == path[i]) { found = a; break; }
        if (!found || (i + 1 < path.size() && !found->menu())) { std::fprintf(stderr, "no menu item %s\n", qPrintable(path.join(" > "))); return nullptr; }
        if (i + 1 < path.size()) {
            emit found->menu()->aboutToShow();
            actions = found->menu()->actions();
        }
    }
    return found;
}

/// Answers the next modal dialog (an input dialog, Canvas Size, Trim ...): `fill` sets its fields, then OK.
void answerModal(std::function<void(QDialog*)> fill) {
    QTimer::singleShot(0, [fill] {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        if (!dialog) { std::fprintf(stderr, "no modal dialog to answer\n"); return; }
        if (fill) fill(dialog);
        dialog->accept();
    });
}

template <class T> T* nth(QObject* parent, int index) {
    const auto all = parent->findChildren<T*>();
    return index < all.size() ? all[index] : nullptr;
}

QToolButton* buttonWithTip(QWidget* panel, const QString& tip) {
    for (QToolButton* b : panel->findChildren<QToolButton*>()) if (b->toolTip() == tip) return b;
    std::fprintf(stderr, "no button \"%s\"\n", qPrintable(tip));
    return nullptr;
}

struct Converted {
    QString label;                                                // for messages
    const char* layer = nullptr;                                  // made active first (none: as it is)
    std::function<void(EditorSession&)> setup;                    // plain session calls before the step, in every run
    std::function<bool(MainWindow&, EditorSession&)> ui;          // the interface: false when it could not be done
    std::function<void(EditorSession&, const QList<ActionStep>&)> direct;   // the edit as the interface made it before
    QStringList methods;                                          // the steps Actions records for it
    bool edits = true;                                            // false: it leaves no history step (a copy)
    /// Requests the automation run sends for what Actions does not record (history, the view).
    QList<std::pair<QString, QJsonObject>> unrecorded = {};
};

/// A command by its registry id (CommandRegistry.h), as its menu item or key runs it.
std::function<bool(MainWindow&, EditorSession&)> command(QString id) {
    return [id](MainWindow& w, EditorSession&) {
        const Command* c = w.commandRegistry().find(id);
        if (!c || !c->action) { std::fprintf(stderr, "no command %s\n", qPrintable(id)); return false; }
        if (!c->action->isEnabled()) { std::fprintf(stderr, "%s is disabled: %s\n", qPrintable(id), qPrintable(w.commandRegistry().disabledReason(*c))); return false; }
        c->action->trigger();
        QApplication::processEvents();
        return true;
    };
}

/// Presses Return on the canvas of the tab on screen (Free Transform's and the warp cage's Enter).
void pressEnter(MainWindow& w) {
    QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
    QApplication::sendEvent(w.canvasAt(w.currentTabIndex()), &enter);
    QApplication::processEvents();
}

/// Clicks a dialog's OK.
void clickOk(QDialog* d) {
    if (auto* box = d->findChild<QDialogButtonBox*>()) if (QPushButton* ok = box->button(QDialogButtonBox::Ok)) { ok->click(); return; }
    std::fprintf(stderr, "no OK button\n");
}

std::function<bool(MainWindow&, EditorSession&)> trigger(QStringList path, std::function<void(QDialog*)> modal = {}, bool hasModal = false) {
    return [path, modal, hasModal](MainWindow& w, EditorSession&) {
        QAction* a = menuItem(w, path);
        if (!a) return false;
        if (!a->isEnabled()) { std::fprintf(stderr, "%s is disabled\n", qPrintable(path.join(" > "))); return false; }
        if (modal || hasModal) answerModal(modal);
        a->trigger();
        QApplication::processEvents();
        return true;
    };
}

/// The Layers panel of the session (the tab's panel calls the same session), shown so its rows are laid out, with
/// `act` done on it.
std::function<bool(MainWindow&, EditorSession&)> layersPanel(std::function<bool(LayersPanel&, EditorSession&)> act) {
    return [act](MainWindow&, EditorSession& s) {
        auto panel = std::make_unique<LayersPanel>(&s);
        panel->resize(360, 900);
        panel->show();
        QApplication::processEvents();
        const bool ok = act(*panel, s);
        QApplication::processEvents();
        return ok;
    };
}

std::function<bool(MainWindow&, EditorSession&)> layersButton(QString tip) {
    return layersPanel([tip](LayersPanel& p, EditorSession&) {
        QToolButton* b = buttonWithTip(&p, tip);
        if (b) b->click();
        return b != nullptr;
    });
}

Uuid layerId(EditorSession& s, const char* name) {
    for (const Layer& l : s.document()->layers) if (l.name == name) return l.id;
    std::fprintf(stderr, "no layer named %s\n", name);
    return {};
}

/// The panel's row for a layer.
QTreeWidgetItem* layerRow(LayersPanel& panel, const Uuid& id) {
    auto* tree = panel.findChild<LayerTree*>();
    for (QTreeWidgetItemIterator it(tree); *it; ++it) if ((*it)->data(0, Qt::UserRole).toString().toStdString() == id) return *it;
    std::fprintf(stderr, "no row for layer %s\n", id.c_str());
    return nullptr;
}

/// A click on a layer's eye: the press starts the swipe, the release on it ends it.
std::function<bool(MainWindow&, EditorSession&)> clickEye(const char* name) {
    return layersPanel([name](LayersPanel& p, EditorSession& s) {
        QToolButton* eye = p.eyeButton(layerId(s, name));
        if (!eye) return false;
        emit eye->pressed();
        QMouseEvent release(QEvent::MouseButtonRelease, QPointF(4, 4), eye->mapToGlobal(QPointF(4, 4)), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(eye, &release);
        return true;
    });
}

std::function<void(QDialog*)> inputValue(QVariant value) {
    return [value](QDialog* d) {
        auto* input = qobject_cast<QInputDialog*>(d);
        if (!input) { std::fprintf(stderr, "not an input dialog\n"); return; }
        if (value.typeId() == QMetaType::Int) input->setIntValue(value.toInt());
        else if (value.typeId() == QMetaType::Double) input->setDoubleValue(value.toDouble());
        else input->setTextValue(value.toString());
    };
}

template <class Dialog, class Kind> std::function<bool(MainWindow&, EditorSession&)> pixelDialog(Kind kind, std::function<void(Dialog*)> set) {
    return [kind, set](MainWindow& w, EditorSession& s) {
        auto* dialog = new Dialog(&s, kind, &w);
        set(dialog);
        dialog->accept();
        QApplication::processEvents();
        return true;
    };
}

std::function<void(FilterDialog*)> filterValues(std::vector<double> values, uint32_t seed = 0) {
    return [values, seed](FilterDialog* d) {
        if (seed) d->setSeed(seed);
        for (size_t i = 0; i < values.size(); i++) if (auto* spin = nth<QDoubleSpinBox>(d, int(i))) spin->setValue(values[i]);
    };
}

std::function<void(PixelAdjustmentDialog*)> adjustmentValues(AdjustmentSettings settings) {
    return [settings](PixelAdjustmentDialog* d) { d->findChild<AdjustmentEditor*>()->setSettings(settings); };
}

/// A pixel dialog the way it committed before the command path: the session is not routed, so it commits itself.
template <class Dialog, class Kind> std::function<void(EditorSession&, const QList<ActionStep>&)> directDialog(MainWindow* w, Kind kind, std::function<void(Dialog*)> set) {
    return [w, kind, set](EditorSession& s, const QList<ActionStep>&) { pixelDialog<Dialog>(kind, set)(*w, s); };
}

std::function<bool(MainWindow&, EditorSession&)> panelButton(bool paths, QString tip) {
    return [paths, tip](MainWindow&, EditorSession& s) {
        // A panel of the session's own: the tab's panel calls the same session.
        std::unique_ptr<QWidget> panel(paths ? static_cast<QWidget*>(new PathsPanel(&s)) : static_cast<QWidget*>(new ChannelsPanel(&s)));
        QToolButton* b = buttonWithTip(panel.get(), tip);
        if (!b) return false;
        b->click();
        QApplication::processEvents();
        return true;
    };
}

int menuCommands(MainWindow& w) {
    const QColor fg(255, 230, 40);
    AdjustmentSettings curves = AdjustmentSettings::defaults(AdjustmentKind::Curves);
    curves.curves.channels[0] = {{0, 0}, {120, 160}, {255, 255}};
    AdjustmentSettings brightness = AdjustmentSettings::defaults(AdjustmentKind::BrightnessContrast);
    brightness.brightnessContrast.brightness = 25;
    brightness.brightnessContrast.contrast = 10;
    AdjustmentSettings posterize = AdjustmentSettings::defaults(AdjustmentKind::Posterize);
    posterize.posterize.levels = 6;
    auto layerSelection = [](const char* name) {
        return [name](EditorSession& s) { select(s, name); s.loadLayerAsSelection(*s.activeLayerId(), false, SelectionMode::Replace); };
    };
    auto params = [](const QList<ActionStep>& steps, int i = 0) { return i < steps.size() ? steps[i].params : QJsonObject{}; };
    // Red's pixels selected, Background active.
    auto backgroundUnderRed = [](EditorSession& s) { select(s, "Red"); s.loadLayerAsSelection(*s.activeLayerId(), false, SelectionMode::Replace); select(s, "Background"); };
    MainWindow* window = &w;
    auto revertFiles = std::make_shared<QStringList>();   // the projects the Revert step saves, removed at the end
    // A style preset for Apply Style: an .asl of one drop shadow, imported into the library and removed at the end.
    QTemporaryDir presetDir;
    {
        StyleLibrary library;
        StylePreset preset;
        preset.id = "self-test-style";
        preset.name = kStylePreset;
        DropShadow shadow;
        shadow.distance = 6;
        shadow.color = {200, 30, 30};
        preset.style.dropShadows.push_back(shadow);
        library.styles.push_back(preset);
        const std::vector<uint8_t> bytes = writeAsl(library);
        QFile file(presetDir.filePath(QStringLiteral("self-test.asl")));
        if (!file.open(QIODevice::WriteOnly) || file.write(reinterpret_cast<const char*>(bytes.data()), qint64(bytes.size())) != qint64(bytes.size())) std::fprintf(stderr, "could not write the style preset\n");
        file.close();
        PresetLibrary::instance().importFiles({file.fileName()});
        if (!PresetLibrary::instance().findStyle(QString::fromLatin1(kStylePreset))) std::fprintf(stderr, "the style preset was not imported\n");
    }

    std::vector<Converted> steps = {
        {"New Layer Below", "Paint", {}, trigger({"Layer", "New Layer Below"}), [](EditorSession& s, auto&) { s.addBlankLayer(true); }, {"layers.add"}},
        {"New Folder", "Paint", {}, trigger({"Layer", "New Folder"}), [](EditorSession& s, auto&) { s.addGroup(); }, {"layers.add"}},
        {"New Adjustment Layer", "Paint", {}, trigger({"Layer", "New Adjustment Layer", names::adjustmentKind(AdjustmentKind::Curves)}),
         [](EditorSession& s, auto&) { s.addAdjustmentLayer(AdjustmentKind::Curves); }, {"layers.add"}},
        {"Rename Layer", nullptr, {}, trigger({"Layer", "Rename Layer…"}, inputValue(QStringLiteral("Renamed"))),
         [](EditorSession& s, auto&) { s.renameLayer(*s.activeLayerId(), "Renamed"); }, {"layers.set"}},
        {"Delete Layer", "Renamed", {}, trigger({"Layer", "Delete Layer"}), [](EditorSession& s, auto&) { s.deleteLayersResolvingClipping({*s.activeLayerId()}, false); }, {"layers.delete"}},
        {"Group Layers", "Paint", {}, trigger({"Layer", "Group Layers"}), [](EditorSession& s, auto&) { s.groupSelectedLayers(); }, {"layers.group"}},
        {"Move Out of Folder", "Paint", {}, trigger({"Layer", "Move Out of Folder"}), [](EditorSession& s, auto&) { s.moveActiveLayerOutOfGroup(); }, {"layers.move"}},
        {"Bring Forward", "Background", {}, trigger({"Layer", "Bring Forward"}), [](EditorSession& s, auto&) { s.moveActiveLayer(1); }, {"layers.reorder"}},
        {"Send Backward", "Background", {}, trigger({"Layer", "Send Backward"}), [](EditorSession& s, auto&) { s.moveActiveLayer(-1); }, {"layers.reorder"}},
        {"Flip Layer Horizontal", "Background", {}, trigger({"Layer", "Flip Layer Horizontal"}), [](EditorSession& s, auto&) { s.flipLayer(true); }, {"layers.flip"}},
        {"Flip Layer Vertical", "Background", {}, trigger({"Layer", "Flip Layer Vertical"}), [](EditorSession& s, auto&) { s.flipLayer(false); }, {"layers.flip"}},
        {"Resampling", "Background", {}, trigger({"Layer", "Resampling", "Smooth"}), [](EditorSession& s, auto&) { s.setLayerSampling(Sampling::Smooth); }, {"layers.set"}},
        {"Mask Reveal All", "Background", {}, trigger({"Layer", "Layer Mask", "Reveal All"}), [](EditorSession& s, auto&) { s.addLayerMask(true); }, {"layers.mask"}},
        {"Mask Invert", "Background", {}, trigger({"Layer", "Layer Mask", "Invert"}), [](EditorSession& s, auto&) { s.invertMask(); }, {"layers.mask"}},
        {"Mask Disable", "Background", {}, trigger({"Layer", "Layer Mask", "Enable / Disable"}), [](EditorSession& s, auto&) { s.toggleLayerMask(); }, {"layers.mask"}},
        {"Mask Enable", "Background", {}, trigger({"Layer", "Layer Mask", "Enable / Disable"}), [](EditorSession& s, auto&) { s.toggleLayerMask(); }, {"layers.mask"}},
        {"Mask Apply", "Background", {}, trigger({"Layer", "Layer Mask", "Apply"}), [](EditorSession& s, auto&) { s.applyMask(); }, {"layers.mask"}},
        {"Mask Hide All", "Background", {}, trigger({"Layer", "Layer Mask", "Hide All"}), [](EditorSession& s, auto&) { s.addLayerMask(false); }, {"layers.mask"}},
        {"Mask Delete", "Background", {}, trigger({"Layer", "Layer Mask", "Delete"}), [](EditorSession& s, auto&) { s.deleteLayerMask(); }, {"layers.mask"}},
        {"Clipping Mask", "Background", [](EditorSession& s) { s.addBlankLayer(); }, trigger({"Layer", "Create / Release Clipping Mask"}),
         [](EditorSession& s, auto&) { s.toggleClippingMask(*s.activeLayerId()); }, {"layers.set"}},
        {"Rasterize", "Background", [](EditorSession& s) { s.convertToSmartObject(nullptr); }, trigger({"Layer", "Smart Objects", "Rasterize"}),
         [](EditorSession& s, auto&) { s.rasterizeSmartObject(); }, {"smartObject.rasterize"}},
        {"Layer via Copy", "Background", layerSelection("Red"), trigger({"Layer", "Layer via Copy"}), [](EditorSession& s, auto&) { s.layerViaCopy(); }, {"layers.viaCopy"}},
        {"Convert to Smart Object", nullptr, {}, trigger({"Layer", "Smart Objects", "Convert to Smart Object"}),
         [](EditorSession& s, auto&) { s.convertToSmartObject(nullptr); }, {"smartObject.convert"}},
        // Select
        {"Load Layer Pixels", "Red", {}, trigger({"Select", "Load as Selection", "Layer Pixels"}),
         [](EditorSession& s, auto&) { s.loadLayerAsSelection(*s.activeLayerId(), false, SelectionMode::Replace); }, {"selection.fromLayer"}},
        {"Expand", nullptr, {}, trigger({"Select", "Modify", "Expand…"}, inputValue(3)), [](EditorSession& s, auto&) { s.selectionExpand(3); }, {"selection.grow"}},
        {"Contract", nullptr, {}, trigger({"Select", "Modify", "Contract…"}, inputValue(1)), [](EditorSession& s, auto&) { s.selectionContract(1); }, {"selection.grow"}},
        {"Feather", nullptr, {}, trigger({"Select", "Modify", "Feather…"}, inputValue(2.5)), [](EditorSession& s, auto&) { s.selectionFeather(2.5); }, {"selection.feather"}},
        {"Smooth", nullptr, {}, trigger({"Select", "Modify", "Smooth…"}, inputValue(2)), [](EditorSession& s, auto&) { s.selectionSmooth(2); }, {"selection.smooth"}},
        {"Border", nullptr, {}, trigger({"Select", "Modify", "Border…"}, inputValue(4)), [](EditorSession& s, auto&) { s.selectionBorder(4); }, {"selection.border"}},
        {"Inverse", nullptr, {}, trigger({"Select", "Inverse"}), [](EditorSession& s, auto&) { s.invertSelection(); }, {"selection.invert"}},
        {"Deselect", nullptr, {}, trigger({"Select", "Deselect"}), [](EditorSession& s, auto&) { s.deselect(); }, {"selection.none"}},
        {"Select All", nullptr, {}, trigger({"Select", "All"}), [](EditorSession& s, auto&) { s.selectAll(); }, {"selection.all"}},
        {"Subtract Layer Pixels", "Paint", {}, trigger({"Select", "Load as Selection", "Subtract Layer Pixels"}),
         [](EditorSession& s, auto&) { s.loadLayerAsSelection(*s.activeLayerId(), false, SelectionMode::Subtract); }, {"selection.fromLayer"}},
        {"Add Layer Pixels", "Paint", {}, trigger({"Select", "Load as Selection", "Add Layer Pixels"}),
         [](EditorSession& s, auto&) { s.loadLayerAsSelection(*s.activeLayerId(), false, SelectionMode::Add); }, {"selection.fromLayer"}},
        {"Intersect Layer Pixels", "Red", {}, trigger({"Select", "Load as Selection", "Intersect with Layer Pixels"}),
         [](EditorSession& s, auto&) { s.loadLayerAsSelection(*s.activeLayerId(), false, SelectionMode::Intersect); }, {"selection.fromLayer"}},
        {"Load Layer Mask", "Red", {}, trigger({"Select", "Load as Selection", "Layer Mask"}),
         [](EditorSession& s, auto&) { s.loadLayerAsSelection(*s.activeLayerId(), true, SelectionMode::Replace); }, {"selection.fromLayer"}},
        // Edit
        {"Fill with Foreground", "Background", {}, trigger({"Edit", "Fill with Foreground"}), [fg](EditorSession& s, auto&) { s.fillSelection(fg); }, {"pixels.fill"}},
        {"Fill with Background", "Background", [](EditorSession& s) { s.backgroundColor = QColor(20, 90, 200); }, trigger({"Edit", "Fill with Background"}),
         [](EditorSession& s, auto&) { s.fillSelection(s.backgroundColor); }, {"pixels.fill"}},
        {"Clear", "Paint", {}, trigger({"Edit", "Clear"}), [](EditorSession& s, auto&) { s.clearSelectionPixels(); }, {"pixels.clear"}},
        // Image
        {"Invert", "Background", {}, trigger({"Image", "Adjustments", "Invert"}), [](EditorSession& s, auto&) { s.invertActive(); }, {"pixels.invert"}},
        {"Flip Canvas Horizontal", nullptr, {}, trigger({"Image", "Flip Canvas Horizontal"}), [](EditorSession& s, auto&) { s.flipCanvas(true); }, {"canvas.flip"}},
        {"Flip Canvas Vertical", nullptr, {}, trigger({"Image", "Flip Canvas Vertical"}), [](EditorSession& s, auto&) { s.flipCanvas(false); }, {"canvas.flip"}},
        {"Crop to Selection", nullptr, layerSelection("Red"), trigger({"Image", "Crop to Selection"}),
         [](EditorSession& s, auto&) { const Rect b = s.document()->selection->bounds(); s.cropTo(QRectF(b.x, b.y, b.width, b.height)); s.deselect(); }, {"canvas.crop", "selection.none"}},
        {"Canvas Size", nullptr, {}, trigger({"Image", "Canvas Size…"}, [](QDialog* d) { nth<QSpinBox>(d, 0)->setValue(nth<QSpinBox>(d, 0)->value() + 40); nth<QSpinBox>(d, 1)->setValue(nth<QSpinBox>(d, 1)->value() + 24); }),
         [](EditorSession& s, auto&) { s.resizeCanvas(s.document()->width + 40, s.document()->height + 24, 0.5, 0.5); }, {"canvas.resize"}},
        {"Trim", nullptr, [](EditorSession& s) {
             // Only the masked circle shows, inside the transparent border Canvas Size added.
             std::vector<Uuid> hide;
             for (const Layer& l : s.document()->layers) if (!l.isGroup && l.name != "Red" && l.visible) hide.push_back(l.id);
             for (const Uuid& id : hide) s.toggleLayerVisibility(id);
         }, trigger({"Image", "Trim…"}, {}, true), [](EditorSession& s, auto&) { s.trim(TrimOptions{}); }, {"image.trim"}},
        {"Image Size", nullptr, [](EditorSession& s) {
             std::vector<Uuid> show;
             for (const Layer& l : s.document()->layers) if (!l.isGroup && !l.visible) show.push_back(l.id);
             for (const Uuid& id : show) s.toggleLayerVisibility(id);
         }, trigger({"Image", "Image Size…"}, [](QDialog* d) { nth<QSpinBox>(d, 0)->setValue(150); }),
         [params](EditorSession& s, const QList<ActionStep>& r) {
             const QJsonObject p = params(r);
             s.resizeImage(p["width"].toInt(), p["height"].toInt(), p["resolution"].toDouble(), p["sampling"].toString() == "nearest" ? 0 : p["sampling"].toString() == "smooth" ? 1 : 2);
         }, {"image.resize"}},
        {"16 Bits", nullptr, {}, trigger({"Image", "Mode", "16 Bits/Channel"}), [](EditorSession& s, auto&) { s.convertMode(SampleType::U16); }, {"image.mode"}},
        {"8 Bits", nullptr, {}, trigger({"Image", "Mode", "8 Bits/Channel"}), [](EditorSession& s, auto&) { s.convertMode(SampleType::U8); }, {"image.mode"}},
        {"Lab Color", nullptr, {}, trigger({"Image", "Mode", "Lab Color"}), [](EditorSession& s, auto&) { s.convertColorMode(ColorMode::Lab); }, {"image.mode"}},
        {"RGB Color", nullptr, {}, trigger({"Image", "Mode", "RGB Color"}), [](EditorSession& s, auto&) { s.convertColorMode(ColorMode::RGB); }, {"image.mode"}},
        // Dialogs' OK
        {"Motion Blur", "Background", {}, pixelDialog<FilterDialog>(FilterKind::MotionBlur, filterValues({20, 15})),
         directDialog<FilterDialog>(window, FilterKind::MotionBlur, filterValues({20, 15})), {"pixels.filter"}},
        {"Add Noise", "Background", {}, pixelDialog<FilterDialog>(FilterKind::AddNoise, filterValues({12}, 4242)),
         directDialog<FilterDialog>(window, FilterKind::AddNoise, filterValues({12}, 4242)), {"pixels.filter"}},
        {"Lens Correction", "Background", {}, pixelDialog<FilterDialog>(FilterKind::LensCorrection, filterValues({30})),
         directDialog<FilterDialog>(window, FilterKind::LensCorrection, filterValues({30})), {"pixels.filter"}},
        {"Curves", "Background", {}, pixelDialog<PixelAdjustmentDialog>(AdjustmentKind::Curves, adjustmentValues(curves)),
         directDialog<PixelAdjustmentDialog>(window, AdjustmentKind::Curves, adjustmentValues(curves)), {"pixels.adjust"}},
        {"Brightness/Contrast", "Background", {}, pixelDialog<PixelAdjustmentDialog>(AdjustmentKind::BrightnessContrast, adjustmentValues(brightness)),
         directDialog<PixelAdjustmentDialog>(window, AdjustmentKind::BrightnessContrast, adjustmentValues(brightness)), {"pixels.adjust"}},
        {"Posterize", "Background", {}, pixelDialog<PixelAdjustmentDialog>(AdjustmentKind::Posterize, adjustmentValues(posterize)),
         directDialog<PixelAdjustmentDialog>(window, AdjustmentKind::Posterize, adjustmentValues(posterize)), {"pixels.adjust"}},
        // The Paths and Channels panels' buttons
        {"Work Path from Selection", "Background", layerSelection("Red"), panelButton(true, PathsPanel::tr("Make a work path from the selection")),
         [](EditorSession& s, auto&) { s.selectionToWorkPath(); }, {"paths.fromSelection"}},
        {"Fill Path", "Background", {}, panelButton(true, PathsPanel::tr("Fill the path with the foreground colour")),
         [](EditorSession& s, auto&) { s.fillPath(*s.activePathId()); }, {"paths.fill"}},
        {"Stroke Path", "Background", {}, panelButton(true, PathsPanel::tr("Stroke the path with the brush's size in the foreground colour")),
         [](EditorSession& s, auto&) { s.strokePath(*s.activePathId()); }, {"paths.stroke"}},
        {"Path to Selection", "Background", [](EditorSession& s) { s.deselect(); }, panelButton(true, PathsPanel::tr("Load the path as a selection")),
         [](EditorSession& s, auto&) { s.pathToSelection(*s.activePathId(), SelectionMode::Replace); }, {"paths.toSelection"}},
        {"Save Selection as Channel", "Background", {}, panelButton(false, ChannelsPanel::tr("Save selection as channel")),
         [](EditorSession& s, auto&) { s.saveSelectionToChannel(std::nullopt, QString(), SelectionMode::Replace); }, {"channels.saveSelection"}},
        {"New Channel", "Background", {}, panelButton(false, ChannelsPanel::tr("Create new channel")), [](EditorSession& s, auto&) { s.newChannel(); }, {"channels.new"}},
        {"Path to Shape", "Background", {}, panelButton(true, PathsPanel::tr("Make a shape layer from the path")),
         [](EditorSession& s, auto&) { s.pathToShapeLayer(*s.activePathId()); }, {"paths.toShape"}},
        {"Vector Mask Current Path", "Background", [](EditorSession& s) { s.selectPath(kWorkPathId); }, trigger({"Layer", "Vector Mask", "Current Path"}),
         [](EditorSession& s, auto&) { s.addVectorMask(EditorSession::VectorMaskKind::CurrentPath); }, {"vectorMask.set"}},
        {"Vector Mask Delete", "Background", {}, trigger({"Layer", "Vector Mask", "Delete"}), [](EditorSession& s, auto&) { s.deleteVectorMask(); }, {"vectorMask.delete"}},
        {"Delete Path", "Background", [](EditorSession& s) { s.selectPath(kWorkPathId); }, panelButton(true, PathsPanel::tr("Delete the path")),
         [](EditorSession& s, auto&) { s.deletePath(*s.activePathId()); }, {"paths.delete"}},
        // The pixel clipboard and Reselect
        {"Copy", "Background", backgroundUnderRed, trigger({"Edit", "Copy"}), [](EditorSession& s, auto&) { s.copySelection(); }, {"pixels.copy"}, false},
        {"Paste", nullptr, {}, trigger({"Edit", "Paste"}), [](EditorSession& s, auto&) { s.paste(); }, {"pixels.paste"}},
        {"Reselect", nullptr, {}, trigger({"Select", "Reselect"}), [](EditorSession& s, auto&) { s.reselect(); }, {"selection.reselect"}},
        {"Cut", "Background", {}, trigger({"Edit", "Cut"}), [](EditorSession& s, auto&) { s.cutSelection(); }, {"pixels.cut"}},
        {"Copy Merged", nullptr, {}, trigger({"Edit", "Copy Merged"}), [](EditorSession& s, auto&) { s.copyMerged(); }, {"pixels.copyMerged"}, false},
        {"Paste Merged", nullptr, {}, trigger({"Edit", "Paste"}), [](EditorSession& s, auto&) { s.paste(); }, {"pixels.paste"}},
        // The Layers panel
        {"Panel New Layer", "Paint", [](EditorSession& s) { s.deselect(); }, layersButton(LayersPanel::tr("New layer (Ctrl-click: below the current layer)")),
         [](EditorSession& s, auto&) { s.addBlankLayer(false); }, {"layers.add"}},
        {"Panel New Folder", "Paint", {}, layersButton(LayersPanel::tr("New folder")), [](EditorSession& s, auto&) { s.addGroup(); }, {"layers.add"}},
        {"Panel Adjustment Layer", "Paint", {}, layersPanel([](LayersPanel& p, EditorSession&) {
             QToolButton* b = buttonWithTip(&p, LayersPanel::tr("New adjustment layer"));
             if (!b || !b->menu()) return false;
             for (QAction* a : b->menu()->actions()) if (a->text() == names::adjustmentKind(AdjustmentKind::Levels)) { a->trigger(); return true; }
             return false;
         }), [](EditorSession& s, auto&) { s.addAdjustmentLayer(AdjustmentKind::Levels); }, {"layers.add"}},
        {"Panel Delete", nullptr, {}, layersButton(LayersPanel::tr("Delete the selected layers")), [](EditorSession& s, auto&) { s.deleteSelectedLayers(); }, {"layers.delete"}},
        {"Panel Mask", "Background", backgroundUnderRed, layersButton(LayersPanel::tr("Add layer mask (reveal all, or hide the selection)")),
         [](EditorSession& s, auto&) { s.addMaskFromSelection(true); }, {"layers.mask"}},
        {"Panel Opacity Drag", "Paint", {}, layersPanel([](LayersPanel& p, EditorSession&) {
             auto* slider = p.findChild<QSlider*>();
             if (!slider) return false;
             slider->setSliderDown(true);
             for (int v : {80, 40, 55}) slider->setValue(v);
             slider->setSliderDown(false);
             return true;
         }), [](EditorSession& s, auto&) { s.beginOpacityEdit(); for (int v : {80, 40, 55}) s.setLayerOpacity(v / 100.0); s.endOpacityEdit(); }, {"layers.set"}},
        {"Panel Opacity Field", "Paint", {}, layersPanel([](LayersPanel& p, EditorSession&) {
             auto* spin = p.findChild<QSpinBox*>();
             if (spin) spin->setValue(70);
             return spin != nullptr;
         }), [](EditorSession& s, auto&) { s.setLayerOpacity(0.7); }, {"layers.set"}},
        {"Panel Blend Mode", "Paint", {}, layersPanel([](LayersPanel& p, EditorSession&) {
             auto* combo = p.findChild<QComboBox*>();
             if (!combo) return false;
             emit combo->activated(combo->findData(int(BlendMode::Screen)));
             return true;
         }), [](EditorSession& s, auto&) { s.setLayerBlendMode(BlendMode::Screen); }, {"layers.set"}},
        {"Panel Hide", "Paint", {}, clickEye("Paint"), [](EditorSession& s, auto&) { s.toggleLayerVisibility(*s.activeLayerId()); }, {"layers.set"}},
        {"Panel Show", "Paint", {}, clickEye("Paint"), [](EditorSession& s, auto&) { s.toggleLayerVisibility(*s.activeLayerId()); }, {"layers.set"}},
        {"Panel Hide Another", "Paint", {}, clickEye("Red"), [](EditorSession& s, auto&) { s.toggleLayerVisibility(layerId(s, "Red")); }, {"layers.set"}},
        {"Panel Clip", "Paint", [](EditorSession& s) { s.addBlankLayer(); s.renameLayer(*s.activeLayerId(), "Clipped"); },
         layersPanel([](LayersPanel& p, EditorSession& s) {
             QTreeWidgetItem* row = layerRow(p, layerId(s, "Clipped"));
             auto* tree = p.findChild<LayerTree*>();
             if (!row || !tree) return false;
             const QPointF at = tree->visualItemRect(row).center();
             QMouseEvent press(QEvent::MouseButtonPress, at, tree->viewport()->mapToGlobal(at), Qt::LeftButton, Qt::LeftButton, Qt::AltModifier);
             QApplication::sendEvent(tree->viewport(), &press);
             QMouseEvent release(QEvent::MouseButtonRelease, at, tree->viewport()->mapToGlobal(at), Qt::LeftButton, Qt::NoButton, Qt::AltModifier);
             QApplication::sendEvent(tree->viewport(), &release);
             return true;
         }), [](EditorSession& s, auto&) { s.toggleClippingMask(*s.activeLayerId()); }, {"layers.set"}},
        {"Panel Rename", "Clipped", {}, layersPanel([](LayersPanel& p, EditorSession& s) {
             QTreeWidgetItem* row = layerRow(p, layerId(s, "Clipped"));
             auto* tree = p.findChild<LayerTree*>();
             if (!row || !tree) return false;
             emit tree->itemDoubleClicked(row, 0);
             QLineEdit* edit = nullptr;
             for (QLineEdit* e : p.findChildren<QLineEdit*>()) if (e->property("renameEditor").toBool()) edit = e;
             if (!edit) { std::fprintf(stderr, "no rename field\n"); return false; }
             edit->setText(QStringLiteral("Clip Renamed"));
             emit edit->editingFinished();
             return true;
         }), [](EditorSession& s, auto&) { s.renameLayer(*s.activeLayerId(), "Clip Renamed"); }, {"layers.set"}},
        {"Panel Drag", "Clip Renamed", {}, layersPanel([](LayersPanel& p, EditorSession& s) {
             auto* tree = p.findChild<LayerTree*>();
             if (!tree) return false;
             // Paint dropped on the row above Background: directly above it, at the top level.
             emit tree->dropRequested(layerId(s, "Paint"), std::nullopt, layerId(s, "Background"), false);
             return true;
         }), [](EditorSession& s, auto&) { s.placeLayer(layerId(s, "Paint"), std::nullopt, layerId(s, "Background"), false); }, {"layers.move"}},
        // ---- Converted with the command registry (1.8.10) ----
        // History and the view: not recorded, so the automation run sends the requests itself.
        {"Undo", nullptr, {}, command("edit.undo"), [](EditorSession& s, auto&) { s.undo(); }, {}, true, {{"history.undo", {}}}},
        {"Redo", nullptr, {}, command("edit.redo"), [](EditorSession& s, auto&) { s.redo(); }, {}, true, {{"history.redo", {}}}},
        {"Zoom In", nullptr, {}, trigger({"View", "Zoom In"}), [](EditorSession& s, auto&) { s.zoomTo(s.viewport.zoom * 1.25); }, {}, false},
        {"Fit on Screen", nullptr, {}, trigger({"View", "Fit on Screen"}), [](EditorSession& s, auto&) { s.fitView(); }, {}, false},
        {"Actual Pixels", nullptr, {}, trigger({"View", "Actual Pixels"}), [](EditorSession& s, auto&) { s.zoomTo(1); }, {}, false},
        // Colours: X, D, and a 16-bit foreground filled (#rrrrggggbbbb).
        {"Fill 16-bit Colour", "Background", [](EditorSession& s) { s.foregroundColor = QColor::fromRgba64(40000, 1234, 65535); }, trigger({"Edit", "Fill with Foreground"}),
         [](EditorSession& s, auto&) { s.fillSelection(s.foregroundColor); }, {"pixels.fill"}},
        {"Swap Colours", nullptr, {}, command("colors.swap"), [](EditorSession& s, auto&) { std::swap(s.foregroundColor, s.backgroundColor); }, {"colors.set"}, false},
        {"Default Colours", nullptr, {}, command("colors.default"), [](EditorSession& s, auto&) { s.foregroundColor = Qt::black; s.backgroundColor = Qt::white; }, {"colors.set"}, false},
        // Layer styles: Copy (nothing recorded, nothing to undo), Paste and Clear, the dialog's OK, a preset.
        {"Copy Layer Style", "Paint", [](EditorSession& s) {
             LayerStyle style;
             DropShadow shadow;
             shadow.distance = 9;
             shadow.color = {20, 40, 200};
             style.dropShadows.push_back(shadow);
             s.applyLayerStyle(*s.activeLayerId(), style);
         }, trigger({"Layer", "Layer Style", "Copy Layer Style"}), [](EditorSession& s, auto&) { s.copyLayerStyle(); }, {}, false},
        {"Paste Layer Style", "Red", {}, trigger({"Layer", "Layer Style", "Paste Layer Style"}), [](EditorSession& s, auto&) { s.pasteLayerStyle(); }, {"layers.setStyle"}},
        {"Clear Layer Style", "Paint", {}, trigger({"Layer", "Layer Style", "Clear Layer Style"}), [](EditorSession& s, auto&) { s.clearLayerStyle(); }, {"layers.setStyle"}},
        {"Layer Style Dialog", "Red", {}, [](MainWindow& w, EditorSession& s) {
             LayerStyleDialog dialog(&s, *s.activeLayerId(), &w, 0);
             auto* list = dialog.findChild<QListWidget*>();
             if (!list || list->count() < 3) return false;
             list->item(2)->setCheckState(Qt::Checked);   // Stroke
             static_cast<QDialog&>(dialog).accept();
             QApplication::processEvents();
             return true;
         }, [window](EditorSession& s, auto&) {
             LayerStyleDialog dialog(&s, *s.activeLayerId(), window, 0);
             dialog.findChild<QListWidget*>()->item(2)->setCheckState(Qt::Checked);
             static_cast<QDialog&>(dialog).accept();
         }, {"layers.setStyle"}},
        {"Apply Style", "Background", {}, trigger({"Layer", "Layer Style", "Apply Style", QString::fromLatin1(kStylePreset)}), [](EditorSession& s, auto&) {
             const StylePreset* p = PresetLibrary::instance().findStyle(QString::fromLatin1(kStylePreset));
             if (p) s.applyStylePreset(*s.activeLayerId(), p->style, PresetLibrary::instance().patternsFor(p->style));
         }, {"layers.applyStyle"}},
        // Vector masks and type.
        {"Vector Mask Reveal All", "Background", {}, trigger({"Layer", "Vector Mask", "Reveal All"}), [](EditorSession& s, auto&) { s.addVectorMask(EditorSession::VectorMaskKind::RevealAll); }, {"vectorMask.set"}},
        {"Vector Mask Edit Background", "Background", {}, trigger({"Layer", "Vector Mask", "Edit"}), [](EditorSession& s, auto&) { s.targetVectorMask(*s.activeLayerId()); }, {"vectorMask.target"}, false},
        {"Vector Mask Delete Background", "Background", {}, trigger({"Layer", "Vector Mask", "Delete"}), [](EditorSession& s, auto&) { s.deleteVectorMask(); }, {"vectorMask.delete"}},
        {"Vector Mask Hide All", "Red", {}, trigger({"Layer", "Vector Mask", "Hide All"}), [](EditorSession& s, auto&) { s.addVectorMask(EditorSession::VectorMaskKind::HideAll); }, {"vectorMask.set"}},
        {"Create Work Path", nullptr, [](EditorSession& s) {
             LayerText text;
             text.text = "Neko";
             text.fontSize = 40;
             s.addTextLayer(QPointF(20, 60), text, false);
         }, trigger({"Type", "Create Work Path"}), [](EditorSession& s, auto&) { s.textToWorkPath(*s.activeLayerId()); }, {"text.toPath"}},
        {"Convert to Shape", nullptr, {}, trigger({"Type", "Convert to Shape"}), [](EditorSession& s, auto&) { s.textToShape(*s.activeLayerId()); }, {"text.toShape"}},
        // The shape made from the type goes again: a first glyph layout in a process can sit 1/64 px off the later ones
        // on some Qt versions (6.4 on CI, despite the warm-up above), and Merge Visible at the end would carry that on.
        {"Delete the Shape", nullptr, {}, trigger({"Layer", "Delete Layer"}), [](EditorSession& s, auto&) { s.deleteLayersResolvingClipping({*s.activeLayerId()}, false); }, {"layers.delete"}},
        // Quick Mask, the channel keys (Ctrl+3 red, Ctrl+2 the composite, Ctrl+Alt+3 red as the selection).
        {"Quick Mask On", "Background", {}, trigger({"Select", "Edit in Quick Mask Mode"}), [](EditorSession& s, auto&) { s.toggleQuickMask(); }, {"selection.quickMask"}},
        {"Quick Mask Off", nullptr, {}, trigger({"Select", "Edit in Quick Mask Mode"}), [](EditorSession& s, auto&) { s.toggleQuickMask(); }, {"selection.quickMask"}},
        {"Select Red", "Background", {}, command("select.channel.target2"), [](EditorSession& s, auto&) { s.selectColorChannels(1u); }, {"channels.select"}, false},
        {"Select Composite", "Background", {}, command("select.channel.target1"), [](EditorSession& s, auto&) { s.selectColorChannels(s.allColors()); }, {"channels.select"}, false},
        {"Load Red", "Background", {}, command("select.channel.load2"), [](EditorSession& s, auto&) {
             SelectionSource source;
             source.kind = SelectionSource::Red;
             s.loadSelectionFromSource(source, false, SelectionMode::Replace);
         }, {"channels.loadSelection"}},
        // Several layers deleted at once (by id), from the menu and from the Layers panel's bin.
        {"Delete Two Layers", nullptr, [](EditorSession& s) {
             s.addBlankLayer(); s.renameLayer(*s.activeLayerId(), "Gone A");
             s.addBlankLayer(); s.renameLayer(*s.activeLayerId(), "Gone B");
             s.selectLayers({layerId(s, "Gone A"), layerId(s, "Gone B")}, layerId(s, "Gone B"));
         }, trigger({"Layer", "Delete Layer"}), [](EditorSession& s, auto&) { s.deleteLayersResolvingClipping({layerId(s, "Gone A"), layerId(s, "Gone B")}, false); }, {"layers.delete"}},
        {"Panel Delete Two", nullptr, [](EditorSession& s) {
             s.addBlankLayer(); s.renameLayer(*s.activeLayerId(), "Gone C");
             s.addBlankLayer(); s.renameLayer(*s.activeLayerId(), "Gone D");
             s.selectLayers({layerId(s, "Gone C"), layerId(s, "Gone D")}, layerId(s, "Gone D"));
         }, layersButton(LayersPanel::tr("Delete the selected layers")), [](EditorSession& s, auto&) { s.deleteSelectedLayers(); }, {"layers.delete"}},
        // The dialogs' OK.
        {"Warp", "Background", {}, trigger({"Edit", "Warp…"}, [](QDialog* d) { auto* style = d->findChild<QComboBox*>(); style->setCurrentIndex(style->findData(QStringLiteral("warpFlag"))); }),
         [window](EditorSession& s, auto&) {
             WarpDialog dialog(&s, window);
             auto* style = dialog.findChild<QComboBox*>();
             style->setCurrentIndex(style->findData(QStringLiteral("warpFlag")));
             static_cast<QDialog&>(dialog).accept();
         }, {"layers.warp"}},
        {"Content-Aware Scale", "Background", {}, [](MainWindow& w, EditorSession& s) {
             auto* dialog = new ContentAwareScaleDialog(&s, &w);
             nth<QDoubleSpinBox>(dialog, 0)->setValue(80);
             clickOk(dialog);
             QApplication::processEvents();
             delete dialog;
             return true;
         }, [window](EditorSession& s, auto&) {
             auto* dialog = new ContentAwareScaleDialog(&s, window);
             nth<QDoubleSpinBox>(dialog, 0)->setValue(80);
             clickOk(dialog);
             delete dialog;
         }, {"pixels.contentAwareScale"}},
        {"Camera Raw", "Background", {}, [](MainWindow& w, EditorSession& s) {
             auto* dialog = new CameraRawDialog(&s, &w);
             auto* panels = dialog->findChild<CameraRawPanels*>();
             if (!panels) return false;
             CameraRawSettings grade = panels->settings();
             grade.exposure = 0.4;
             grade.contrast = 20;
             panels->setSettings(grade);
             dialog->accept();
             QApplication::processEvents();
             return true;
         }, [window](EditorSession& s, auto&) {
             auto* dialog = new CameraRawDialog(&s, window);
             auto* panels = dialog->findChild<CameraRawPanels*>();
             CameraRawSettings grade = panels->settings();
             grade.exposure = 0.4;
             grade.contrast = 20;
             panels->setSettings(grade);
             dialog->accept();
         }, {"pixels.cameraRaw"}},
        {"Mosh", "Background", {}, [](MainWindow& w, EditorSession& s) {
             auto* dialog = new MoshDialog(&s, *compositor::mosh::findEffect("wave"), &w);
             dialog->accept();
             QApplication::processEvents();
             return true;
         }, [window](EditorSession& s, auto&) { (new MoshDialog(&s, *compositor::mosh::findEffect("wave"), window))->accept(); }, {"pixels.mosh"}},
        {"Load Selection", "Background", [](EditorSession& s) { s.deselect(); }, [](MainWindow& w, EditorSession& s) {
             auto* dialog = new LoadSelectionDialog(&s, &w);
             auto* source = dialog->findChild<QComboBox*>();
             source->setCurrentIndex(source->findData(QStringList{QStringLiteral("transparency"), QString::fromStdString(layerId(s, "Red"))}));
             clickOk(dialog);
             QApplication::processEvents();
             return true;
         }, [](EditorSession& s, auto&) {
             SelectionSource source;
             source.kind = SelectionSource::Transparency;
             source.id = layerId(s, "Red");
             s.loadSelectionFromSource(source, false, SelectionMode::Replace);
         }, {"channels.loadSelection"}},
        {"Save Selection", "Background", {}, [](MainWindow& w, EditorSession& s) {
             auto* dialog = new SaveSelectionDialog(&s, &w);
             clickOk(dialog);
             QApplication::processEvents();
             return true;
         }, [window](EditorSession& s, auto&) { clickOk(new SaveSelectionDialog(&s, window)); }, {"channels.saveSelection"}},
        {"Warp Cage", "Background", [](EditorSession& s) { s.deselect(); }, [](MainWindow& w, EditorSession& s) {
             if (!s.beginWarpCage()) return false;
             WarpMesh cage = *s.warpCage();
             cage.xs[5] += 12;
             cage.ys[5] -= 8;
             s.setWarpCage(cage);
             pressEnter(w);
             return !s.warpCage();
         }, [](EditorSession& s, auto&) {
             s.beginWarpCage();
             WarpMesh cage = *s.warpCage();
             cage.xs[5] += 12;
             cage.ys[5] -= 8;
             s.setWarpCage(cage);
             s.commitWarpCage();
         }, {"layers.setCage"}},
        // A saved project saved again: document.save, no history step.
        {"Save", "Paint", [revertFiles](EditorSession& s) {
             const QString path = QDir(QDir::tempPath()).filePath(QStringLiteral("nekophoto-command-path-%1-%2.nekophoto").arg(QCoreApplication::applicationPid()).arg(revertFiles->size()));
             revertFiles->push_back(path);
             QString error;
             if (!s.saveProject(path, &error)) std::fprintf(stderr, "Save: could not save %s: %s\n", qPrintable(path), qPrintable(error));
             s.addBlankLayer();
         }, trigger({"File", "Save"}), [](EditorSession& s, auto&) { s.saveProject(s.projectPath(), nullptr); }, {"document.save"}, false},
        // Last: it merges most of the document. Red hidden first, so a hidden layer stays out of the merge.
        // File > Revert: the project as saved before a change, as one undo step (each tab saves a project of its own).
        {"Revert", "Paint", [revertFiles](EditorSession& s) {
             const QString path = QDir(QDir::tempPath()).filePath(QStringLiteral("nekophoto-command-path-%1-%2.nekophoto").arg(QCoreApplication::applicationPid()).arg(revertFiles->size()));
             revertFiles->push_back(path);
             QString error;
             if (!s.saveProject(path, &error)) std::fprintf(stderr, "Revert: could not save %s: %s\n", qPrintable(path), qPrintable(error));
             s.addBlankLayer();
         }, trigger({"File", "Revert"}), [window](EditorSession&, auto&) { window->revertDocument(nullptr); }, {"document.revert"}},
        {"Merge Visible", "Paint", [](EditorSession& s) { const Layer* red = s.document()->find(layerId(s, "Red")); if (red && red->visible) s.toggleLayerVisibility(red->id); },
         trigger({"Layer", "Merge Visible"}), [](EditorSession& s, auto&) { s.mergeVisible(); }, {"layers.merge"}},
    };
    // G'MIC's OK, where G'MIC is installed: a built-in filter through pixels.gmic (before, the dialog's own run).
    if (GmicRunner::available()) {
        auto gmicDialog = [](MainWindow& w, EditorSession& s) {
            auto* dialog = new GmicDialog(&s, &w);
            dialog->showFilter(QStringLiteral("Sepia"));
            auto done = std::make_shared<bool>(false);
            QObject::connect(dialog, &QDialog::finished, [done] { *done = true; });
            static_cast<QDialog*>(dialog)->accept();
            // The direct path runs G'MIC in the background and closes when it is done.
            for (int i = 0; i < 12000 && !*done; i++) { QApplication::processEvents(QEventLoop::AllEvents, 10); QThread::msleep(5); }
            if (!*done) std::fprintf(stderr, "G'MIC did not finish\n");
            return *done;
        };
        steps.insert(steps.end() - 2, Converted{"G'MIC", "Background", {}, gmicDialog, [window, gmicDialog](EditorSession& s, auto&) { gmicDialog(*window, s); }, {"pixels.gmic"}});
    }

    // Select > Subject: selection.subject with a box 5% inside the canvas, when the click-to-select model is there
    // (a developer's machine); without it (CI) the item is greyed and says why, checked below.
    if (ModelStore::promptReady())
        steps.insert(steps.end() - 2, Converted{"Subject", "Background", {}, trigger({"Select", "Subject"}), [](EditorSession& s, auto&) {
            const Document& d = *s.document();
            s.clearClickPrompts();
            s.setQuickSelectClicks(true);
            s.setClickBox(QPointF(d.width / 20, d.height / 20), QPointF(d.width - d.width / 20, d.height - d.height / 20), false);
            s.runClickSelection(SelectionMode::Replace, nullptr);
        }, {"selection.subject"}});

    int failures = 0;
    auto prepare = [](EditorSession& s) {
        buildDemoDocument(s);
        s.deselect();
        s.foregroundColor = QColor(255, 230, 40);
    };
    auto before = [](EditorSession& s, const Converted& step) {
        if (step.layer) select(s, step.layer);
        if (step.setup) step.setup(s);
    };

    // Type converted once before the runs, through the menus as the first run does: the first layout of a font in the
    // process can place glyph outlines 1/64 px apart from the ones after it, which would set the first run apart.
    {
        w.newTab();
        EditorSession& warm = *w.session();
        prepare(warm);
        LayerText text;
        text.text = "Neko";
        text.fontSize = 40;
        if (warm.addTextLayer(QPointF(20, 60), text, false)) {
            trigger({"Type", "Create Work Path"})(w, warm);
            trigger({"Type", "Convert to Shape"})(w, warm);
        }
    }

    // The interface, recorded.
    w.newTab();
    EditorSession& a = *w.session();
    prepare(a);
    if (!ModelStore::promptReady()) {
        QAction* subject = menuItem(w, {"Select", "Subject"});
        if (!subject) failures++;
        // Greyed, saying why: the model isn't downloaded, or (Windows, no OpenCV) the build can't run it at all.
        else if (subject->isEnabled() || subject->toolTip().isEmpty()
                 || !(subject->toolTip().contains(QLatin1String("model")) || subject->toolTip().contains(QLatin1String("OpenCV")))) {
            std::fprintf(stderr, "Select > Subject without the click-to-select model: enabled %d, tooltip \"%s\"\n", subject->isEnabled(), qPrintable(subject->toolTip()));
            failures++;
        }
    }
    ActionLibrary::instance().startRecording(QStringLiteral("menu-commands self-test"));
    std::vector<int> recordedBefore;
    auto recorded = [] {
        const RecordedAction* r = ActionLibrary::instance().find(QStringLiteral("menu-commands self-test"));
        return r ? QList<ActionStep>(r->steps.begin(), r->steps.end()) : QList<ActionStep>{};
    };
    // An error the window shows is a failure: the message box is closed and reported, so nothing waits on it.
    QTimer watchdog;
    QString current;
    QObject::connect(&watchdog, &QTimer::timeout, [&] {
        if (auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
            std::fprintf(stderr, "%s: the window showed \"%s\"\n", qPrintable(current), qPrintable(box->text()));
            failures++;
            box->reject();
        }
    });
    watchdog.start(20);
    // The layers' names when each step was made: steps that name layers by id (an eye, a drag) are replayed on the
    // other tabs' layers of the same name.
    std::vector<std::map<std::string, std::string>> namesAt;
    // The document after each step on each path, to name the first step where two paths part.
    std::vector<QString> afterInterface, afterAutomation, afterDirect;
    for (const Converted& step : steps) {
        current = step.label;
        before(a, step);
        namesAt.emplace_back();
        for (const Layer& l : a.document()->layers) namesAt.back()[l.id] = l.name;
        recordedBefore.push_back(int(recorded().size()));
        const auto history = a.undoNames();   // (the list is capped: compare it, not its length)
        if (!step.ui(w, a)) { std::fprintf(stderr, "%s: could not be done through the interface\n", qPrintable(step.label)); failures++; }
        if ((a.undoNames() == history) == step.edits) { std::fprintf(stderr, step.edits ? "%s left no history step\n" : "%s left a history step\n", qPrintable(step.label)); failures++; }
        afterInterface.push_back(describe(a));
    }
    watchdog.stop();
    ActionLibrary::instance().stopRecording();
    const QList<ActionStep> steps_ = recorded();
    ActionLibrary::instance().remove(QStringLiteral("menu-commands self-test"));
    recordedBefore.push_back(int(steps_.size()));
    const QString viaInterface = describe(a);

    // Actions recorded each command once, as the method automation runs.
    for (size_t i = 0; i < steps.size(); i++) {
        QStringList got;
        for (int k = recordedBefore[i]; k < recordedBefore[i + 1]; k++) got << steps_[k].method;
        if (got != steps[i].methods) { std::fprintf(stderr, "%s recorded [%s], expected [%s]\n", qPrintable(steps[i].label), qPrintable(got.join(", ")), qPrintable(steps[i].methods.join(", "))); failures++; }
    }

    // Automation: the recorded requests, sent as the socket sends them.
    AutomationServer* engine = w.automationEngine();
    w.newTab();
    EditorSession& b = *w.session();
    prepare(b);
    for (size_t i = 0; i < steps.size(); i++) {
        before(b, steps[i]);
        for (int k = recordedBefore[i]; k < recordedBefore[i + 1]; k++) {
            QJsonObject sent = steps_[k].params;
            auto mapped = [&](const QString& id) -> QString {
                auto name = namesAt[i].find(id.toStdString());
                if (name == namesAt[i].end()) return id;
                for (const Layer& l : b.document()->layers) if (l.name == name->second) return QString::fromStdString(l.id);
                return id;
            };
            for (const char* key : {"id", "parent", "above", "layer"})
                if (sent.value(key).isString()) sent[key] = mapped(sent.value(key).toString());
            if (sent.value("ids").isArray()) {
                QJsonArray ids;
                for (const QJsonValue& v : sent.value("ids").toArray()) ids.append(mapped(v.toString()));
                sent["ids"] = ids;
            }
            const QJsonObject reply = engine->handle(QJsonObject{{"jsonrpc", "2.0"}, {"id", 1}, {"method", steps_[k].method}, {"params", sent}});
            if (reply.contains("error")) { std::fprintf(stderr, "%s: %s\n", qPrintable(steps_[k].method), qPrintable(reply.value("error").toObject().value("message").toString())); failures++; }
        }
        for (const auto& [method, params] : steps[i].unrecorded) {
            const QJsonObject reply = engine->handle(QJsonObject{{"jsonrpc", "2.0"}, {"id", 1}, {"method", method}, {"params", params}});
            if (reply.contains("error")) { std::fprintf(stderr, "%s: %s\n", qPrintable(method), qPrintable(reply.value("error").toObject().value("message").toString())); failures++; }
        }
        afterAutomation.push_back(describe(b));
    }
    const QString viaAutomation = describe(b);

    // The interface's own calls, as before the command path.
    w.newTab();
    EditorSession& c = *w.session();
    c.commandReady = [] { return false; };
    prepare(c);
    for (size_t i = 0; i < steps.size(); i++) {
        before(c, steps[i]);
        steps[i].direct(c, steps_.mid(recordedBefore[i], recordedBefore[i + 1] - recordedBefore[i]));
        QApplication::processEvents();
        afterDirect.push_back(describe(c));
    }
    const QString direct = describe(c);
    // Where two paths first part, and the lines that differ there.
    auto firstDifference = [&](const std::vector<QString>& one, const std::vector<QString>& other, const char* what) {
        for (size_t i = 0; i < steps.size() && i < one.size() && i < other.size(); i++) {
            if (one[i] == other[i]) continue;
            const QStringList x = one[i].split('\n'), y = other[i].split('\n');
            QStringList lines;
            for (int k = 0; k < std::max(x.size(), y.size()); k++)
                if (x.value(k) != y.value(k) && !x.value(k).startsWith(QLatin1String("history"))) lines << QStringLiteral("  %1\n  %2").arg(x.value(k), y.value(k));
            std::fprintf(stderr, "%s first differ after \"%s\":\n%s\n", what, qPrintable(steps[i].label), qPrintable(lines.join('\n')));
            return;
        }
    };
    firstDifference(afterInterface, afterAutomation, "the interface and automation");
    firstDifference(afterInterface, afterDirect, "the interface and the direct calls");

    if (viaInterface != viaAutomation) {
        std::fprintf(stderr, "menu commands: the interface and automation differ:\n--- interface\n%s\n--- automation\n%s\n", qPrintable(viaInterface), qPrintable(viaAutomation));
        failures++;
    }
    if (viaInterface != direct) {
        std::fprintf(stderr, "menu commands: the command path changed what the interface does:\n--- now\n%s\n--- before\n%s\n", qPrintable(viaInterface), qPrintable(direct));
        failures++;
    }
    for (const QString& path : *revertFiles) QFile::remove(path);
    PresetLibrary::instance().removeStyle(QString::fromLatin1(kStylePreset));
    std::printf("menu commands: %d checked, %s\n", int(steps.size()), failures ? "FAILED" : "ok");
    return failures;
}

/// Ruler guides through the pointer: one pulled out of the top ruler, moved with the Move tool, dragged off the
/// canvas; each an undo step named as Photoshop names it, and undone again.
int guides(MainWindow& w) {
    EditorSession& s = *w.session();
    buildDemoDocument(s);
    CanvasWidget* canvas = w.canvasAt(w.currentTabIndex());
    Ruler* top = nullptr;
    for (Ruler* r : w.findChildren<Ruler*>()) if (r->height() == Ruler::thickness && r->isVisible()) top = r;
    if (!canvas || !top) { std::fprintf(stderr, "no canvas or ruler\n"); return 1; }
    int failures = 0;
    auto expect = [&](bool ok, const char* what) { if (!ok) { std::fprintf(stderr, "%s\n", what); failures++; } };
    auto mouse = [](QWidget* target, QEvent::Type type, QPointF local, Qt::KeyboardModifiers modifiers = Qt::ControlModifier) {
        // Ctrl keeps snapping out of it, so the positions are exact.
        QMouseEvent e(type, local, target->mapToGlobal(local), Qt::LeftButton, type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton, modifiers);
        QApplication::sendEvent(target, &e);
    };
    const QPointF at = canvas->viewPointForTest(QPointF(100, 150));
    // Out of the top ruler and down onto the canvas.
    const QPointF onRuler(at.x(), Ruler::thickness / 2.0);
    mouse(top, QEvent::MouseButtonPress, onRuler);
    mouse(top, QEvent::MouseMove, top->mapFromGlobal(canvas->mapToGlobal(at)));
    mouse(top, QEvent::MouseButtonRelease, top->mapFromGlobal(canvas->mapToGlobal(at)));
    // Within a pixel: the ruler hands the canvas whole screen points.
    expect(s.guides().size() == 1 && !s.guides()[0].vertical() && std::abs(s.guides()[0].position - 150) <= 1, "the ruler did not make a horizontal guide at 150");
    if (!s.guides().empty()) std::printf("ruler guide at %g\n", s.guides()[0].position);
    expect(!s.undoNames().empty() && s.undoNames().back() == "New Guide", "no New Guide step");
    // The Move tool picks it up and moves it.
    s.selectTool(Tool::Move);
    const QPointF below = canvas->viewPointForTest(QPointF(100, 200));
    mouse(canvas, QEvent::MouseButtonPress, at);
    mouse(canvas, QEvent::MouseMove, below);
    mouse(canvas, QEvent::MouseButtonRelease, below);
    expect(s.guides().size() == 1 && s.guides()[0].position == 200, "the Move tool did not move the guide to 200");
    expect(s.undoNames().back() == "Move Guide", "no Move Guide step");
    // Dragged off the canvas (onto the ruler), it goes.
    mouse(canvas, QEvent::MouseButtonPress, below);
    mouse(canvas, QEvent::MouseMove, QPointF(below.x(), -5));
    mouse(canvas, QEvent::MouseButtonRelease, QPointF(below.x(), -5));
    expect(s.guides().empty(), "the guide dragged off the canvas is still there");
    expect(s.undoNames().back() == "Delete Guide", "no Delete Guide step");
    for (int i = 0; i < 3; i++) s.undo();
    expect(s.guides().empty() && s.canRedo(), "undo did not take the three steps back");
    s.redo();
    expect(s.guides().size() == 1, "redo did not bring the guide back");
    std::printf("guides: %s\n", failures ? "FAILED" : "ok");
    return failures ? 1 : 0;
}

/// The canvas's context menu offers what the tool and the point allow, and leaves the rest out.
int canvasMenus(MainWindow& w) {
    EditorSession& s = *w.session();
    buildDemoDocument(s);
    CanvasWidget* canvas = w.canvasAt(w.currentTabIndex());
    int failures = 0;
    auto texts = [&](QPointF doc) {
        QMenu* menu = w.buildCanvasMenu(canvas->viewPointForTest(doc), &w);
        QStringList out;
        for (QAction* a : menu->actions()) if (!a->isSeparator()) out << a->text().remove('&').section('\t', 0, 0);
        delete menu;
        return out;
    };
    auto expect = [&](const QStringList& menu, const QStringList& present, const QStringList& absent, const char* where) {
        for (const QString& t : present) if (!menu.contains(t)) { std::fprintf(stderr, "%s: no \"%s\" in [%s]\n", where, qPrintable(t), qPrintable(menu.join(", "))); failures++; }
        for (const QString& t : absent) if (menu.contains(t)) { std::fprintf(stderr, "%s: \"%s\" should not be there\n", where, qPrintable(t)); failures++; }
    };
    select(s, "Background");
    s.selectTool(Tool::Move);
    expect(texts(QPointF(20, 20)), {"Background", "Free Transform", "Duplicate Layer", "Add Layer Mask"}, {"Deselect"}, "Move tool");
    s.selectTool(Tool::Marquee);
    s.selectAll();
    expect(texts(QPointF(20, 20)), {"Deselect", "Select Inverse", "Feather…", "Layer via Copy"}, {"Select All", "Background"}, "Marquee with a selection");
    s.deselect();
    expect(texts(QPointF(20, 20)), {"Select All", "Reselect"}, {"Deselect", "Select Inverse"}, "Marquee without a selection");
    // An open path: anchors, a segment, and the path's own commands.
    s.selectTool(Tool::Pen);
    s.penMode = EditorSession::PenMode::Path;
    s.penPress(QPointF(40, 40));
    s.penPress(QPointF(200, 40));
    s.penPress(QPointF(200, 200));
    expect(texts(QPointF(100, 100)), {"Close Path", "End Path"}, {}, "Pen drawing");
    s.penFinish(false);
    s.selectTool(Tool::DirectSelect);
    expect(texts(QPointF(200, 40)), {"Delete Anchor Point", "Convert Point", "Close Path", "Make Selection", "Fill Path", "Stroke Path", "Delete Path"}, {"Add Anchor Point"}, "Direct Selection on an anchor");
    expect(texts(QPointF(120, 40)), {"Add Anchor Point"}, {"Delete Anchor Point"}, "Direct Selection on a segment");
    // A transform in progress.
    s.selectTool(Tool::Move);
    select(s, "Paint");
    s.transformCommand();
    expect(texts(QPointF(20, 20)), {"Distort", "Rotate 180°", "Flip Horizontal", "Apply Transform", "Cancel Transform"}, {"Duplicate Layer"}, "Free Transform");
    s.cancelTransform();
    std::printf("canvas-menus: %s\n", failures ? "FAILED" : "ok");
    return failures ? 1 : 0;
}

/// Edit > Search…: Ctrl+F opens the palette, typed queries put the expected command first, Enter runs it and it
/// joins Recently Used.
int search(MainWindow& w) {
    int failures = 0;
    w.show();
    QApplication::processEvents();
    QAction* searchAction = action(w, "edit.search");
    if (!searchAction || searchAction->shortcut() != QKeySequence("Ctrl+F")) { std::fprintf(stderr, "Edit > Search has no Ctrl+F\n"); return 1; }
    auto open = [&]() -> CommandPalette* {
        searchAction->trigger();
        QApplication::processEvents();
        for (CommandPalette* p : w.findChildren<CommandPalette*>()) if (p->isVisible()) return p;   // a closed one waits for deletion
        return static_cast<CommandPalette*>(nullptr);
    };
    auto type = [](CommandPalette* p, const QString& text) {
        auto* field = p->findChild<QLineEdit*>(QStringLiteral("commandPaletteField"));
        field->clear();
        for (QChar c : text) {
            QKeyEvent press(QEvent::KeyPress, 0, Qt::NoModifier, QString(c));
            QApplication::sendEvent(field, &press);
        }
        QApplication::processEvents();
    };
    {
        // Undo is greyed with nothing to undo, and says so; its entry carries the registry's id.
        CommandPalette* p = open();
        if (!p) { std::fprintf(stderr, "Ctrl+F opened no palette\n"); return 1; }
        type(p, QStringLiteral("undo"));
        const PaletteEntry* first = p->resultEntry(0);
        if (!first || first->commandId != QLatin1String("edit.undo")) { std::fprintf(stderr, "search \"undo\": the first row is not edit.undo\n"); failures++; }
        else if (!w.session()->canUndo() && !p->rowText(0, 1).contains(QLatin1String("Nothing to undo"))) {
            std::fprintf(stderr, "search \"undo\": the greyed row says \"%s\", not why\n", qPrintable(p->rowText(0, 1)));
            failures++;
        }
        p->close();
        QApplication::processEvents();
        // A command that needs a document, with none open, says so.
        if (!w.session()->hasDocument()) {
            p = open();
            type(p, QStringLiteral("gauss"));
            if (!p->rowText(0, 1).contains(QLatin1String("No document is open"))) { std::fprintf(stderr, "search \"gauss\": \"%s\"\n", qPrintable(p->rowText(0, 1))); failures++; }
            p->close();
            QApplication::processEvents();
        }
    }
    const std::vector<std::pair<QString, QString>> cases = {
        {"gauss", "Gaussian Blur…"}, {"merge vis", "Merge Visible"}, {"sel inv", "Inverse"}, {"prefer", "Preferences…"},
        {"lasso", "Lasso"}};
    for (const auto& [query, expected] : cases) {
        CommandPalette* p = open();
        if (!p) { std::fprintf(stderr, "Ctrl+F opened no palette\n"); return 1; }
        type(p, query);
        const QStringList rows = p->resultLabels();
        if (rows.isEmpty() || rows.first() != expected) {
            std::fprintf(stderr, "search \"%s\": expected \"%s\" first, got \"%s\"\n", qPrintable(query), qPrintable(expected), qPrintable(rows.value(0)));
            failures++;
        }
        p->close();
        QApplication::processEvents();
    }
    // Enter runs the top row: "lasso" picks the Lasso tool, which then leads Recently Used.
    CommandPalette* p = open();
    type(p, QStringLiteral("lasso"));
    QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
    QApplication::sendEvent(p->findChild<QLineEdit*>(QStringLiteral("commandPaletteField")), &enter);
    QApplication::processEvents();
    QApplication::processEvents();
    if (w.session()->tool() != Tool::Lasso) { std::fprintf(stderr, "Enter on \"lasso\" did not pick the Lasso tool\n"); failures++; }
    if (CommandPalette::recent().value(0) != QLatin1String("tool:Lasso")) { std::fprintf(stderr, "recent: %s\n", qPrintable(CommandPalette::recent().join(", "))); failures++; }
    p = open();
    if (!p || p->resultLabels().value(0) != QLatin1String("Lasso")) { std::fprintf(stderr, "Recently Used does not lead the empty query\n"); failures++; }
    if (p) p->close();
    std::printf("search: %s\n", failures ? "FAILED" : "ok");
    return failures ? 1 : 0;
}

/// Photoshop's held and missing keys, typed as a person would (each press offered to the shortcuts first, as QTest
/// does): Alt with the Brush samples the foreground and lets go back to the Brush, Ctrl moves the layer under the
/// pointer with the Move tool for as long as it is held, Ctrl+Space and Ctrl+Alt+Space zoom, a tool's letter held while
/// the tool is used springs back and a tap does not, F7 shows and hides the Layers panel (not from a text field),
/// Ctrl+Alt+Z toggles the last state, F12 reverts (one undo step), and Ctrl+Shift+> and < size the type being typed.
int heldKeys(MainWindow& w) {
    EditorSession& s = *w.session();
    buildDemoDocument(s);
    w.show();
    w.activateWindow();
    QApplication::processEvents();
    CanvasWidget* canvas = w.canvasAt(w.currentTabIndex());
    if (!canvas) { std::fprintf(stderr, "no canvas\n"); return 1; }
    canvas->setFocus();
    QApplication::processEvents();
    int failures = 0;
    auto expect = [&](bool ok, const char* what) { if (!ok) { std::fprintf(stderr, "held keys: %s\n", what); failures++; } };
    // A key press goes to the shortcuts first, then (when none took it) to the widget; true when a shortcut took it.
    auto press = [](QWidget* target, int key, Qt::KeyboardModifiers modifiers, const QString& text = QString()) {
        if (qt_sendShortcutOverrideEvent(target, 0, key, modifiers, text, false, 1)) return true;
        QKeyEvent e(QEvent::KeyPress, key, modifiers, text);
        QApplication::sendEvent(target, &e);
        return false;
    };
    auto release = [](QWidget* target, int key, Qt::KeyboardModifiers modifiers) {
        QKeyEvent e(QEvent::KeyRelease, key, modifiers);
        QApplication::sendEvent(target, &e);
    };
    auto mouse = [canvas](QEvent::Type type, QPointF documentPoint, Qt::KeyboardModifiers modifiers) {
        const QPointF local = canvas->viewPointForTest(documentPoint);
        QMouseEvent e(type, local, canvas->mapToGlobal(local), Qt::LeftButton, type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton, modifiers);
        QApplication::sendEvent(canvas, &e);
    };
    auto click = [&](QPointF documentPoint, Qt::KeyboardModifiers modifiers) {
        mouse(QEvent::MouseButtonPress, documentPoint, modifiers);
        mouse(QEvent::MouseButtonRelease, documentPoint, modifiers);
    };

    // Alt with the Brush: the Eyedropper, sampling the foreground (not the background, as Alt with the Eyedropper itself).
    s.selectTool(Tool::Brush);
    s.brushErase = false;
    s.foregroundColor = Qt::black;
    s.backgroundColor = Qt::white;
    const QPointF sample(30, 400);
    const QColor expected = s.compositeColorAt(sample).value_or(QColor());
    const size_t stepsBefore = s.undoNames().size();
    press(canvas, Qt::Key_Alt, Qt::AltModifier);
    expect(canvas->heldTool() == Tool::Eyedropper, "Alt with the Brush did not hold the Eyedropper");
    expect(s.tool() == Tool::Brush, "Alt changed the session's tool");
    click(sample, Qt::AltModifier);
    expect(expected.isValid() && expected != QColor(Qt::black) && s.foregroundColor == expected, "the held Eyedropper did not sample the foreground");
    expect(s.backgroundColor == QColor(Qt::white), "the held Eyedropper changed the background");
    expect(s.undoNames().size() == stepsBefore, "Alt-click with the Brush painted");
    release(canvas, Qt::Key_Alt, Qt::NoModifier);
    expect(!canvas->heldTool() && s.tool() == Tool::Brush, "letting go of Alt did not go back to the Brush");
    // Dragged, the held Eyedropper keeps sampling: the foreground follows the pointer to a differently coloured spot.
    {
        std::optional<QPointF> other;
        for (int y = 20; y < s.document()->height && !other; y += 37)
            for (int x = 20; x < s.document()->width && !other; x += 41)
                if (auto c = s.compositeColorAt(QPointF(x, y)); c && *c != expected) other = QPointF(x, y);
        if (!other) { std::fprintf(stderr, "held keys: no second colour in the demo document\n"); return 1; }
        const QColor second = s.compositeColorAt(*other).value_or(QColor());
        s.foregroundColor = Qt::black;
        press(canvas, Qt::Key_Alt, Qt::AltModifier);
        mouse(QEvent::MouseButtonPress, sample, Qt::AltModifier);
        expect(s.foregroundColor == expected, "the held Eyedropper's press did not sample");
        mouse(QEvent::MouseMove, *other, Qt::AltModifier);
        expect(s.foregroundColor == second, "the held Eyedropper did not follow the drag");
        expect(canvas->heldTool() == Tool::Eyedropper, "the held Eyedropper let go mid-drag");
        mouse(QEvent::MouseButtonRelease, *other, Qt::AltModifier);
        release(canvas, Qt::Key_Alt, Qt::NoModifier);
        expect(s.undoNames().size() == stepsBefore && s.backgroundColor == QColor(Qt::white), "the held Eyedropper's drag painted or changed the background");
        // The Eyedropper itself does the same, and with Alt into the background.
        s.selectTool(Tool::Eyedropper);
        mouse(QEvent::MouseButtonPress, *other, Qt::NoModifier);
        mouse(QEvent::MouseMove, sample, Qt::NoModifier);
        mouse(QEvent::MouseButtonRelease, sample, Qt::NoModifier);
        expect(s.foregroundColor == expected, "a drag with the Eyedropper did not follow the pointer");
        mouse(QEvent::MouseButtonPress, sample, Qt::AltModifier);
        mouse(QEvent::MouseMove, *other, Qt::AltModifier);
        mouse(QEvent::MouseButtonRelease, *other, Qt::AltModifier);
        expect(s.backgroundColor == second && s.foregroundColor == expected, "an Alt-drag with the Eyedropper did not sample the background");
        s.backgroundColor = Qt::white;
        s.selectTool(Tool::Brush);
    }
    // The Eraser keeps Alt for itself.
    s.brushErase = true;
    press(canvas, Qt::Key_Alt, Qt::AltModifier);
    expect(!canvas->heldTool(), "Alt with the Eraser held the Eyedropper");
    release(canvas, Qt::Key_Alt, Qt::NoModifier);
    s.brushErase = false;

    // Ctrl with the Brush: the Move tool, which picks the layer under the pointer and drags it.
    const QPointF from(330, 260), to(350, 270);
    const std::optional<Uuid> under = s.layerAt(from);
    const Layer* before = under ? s.document()->find(*under) : nullptr;
    const Point origin = before ? before->transform.origin : Point();
    press(canvas, Qt::Key_Control, Qt::ControlModifier);
    expect(canvas->heldTool() == Tool::Move, "Ctrl with the Brush did not hold the Move tool");
    mouse(QEvent::MouseButtonPress, from, Qt::ControlModifier);
    mouse(QEvent::MouseMove, to, Qt::ControlModifier);
    mouse(QEvent::MouseButtonRelease, to, Qt::ControlModifier);
    const Layer* after = under ? s.document()->find(*under) : nullptr;
    expect(after && std::abs(after->transform.origin.x - origin.x - 20) < 0.5 && std::abs(after->transform.origin.y - origin.y - 10) < 0.5,
           "the held Move tool did not move the layer under the pointer by 20, 10");
    if (after) std::printf("held move: %g, %g\n", after->transform.origin.x - origin.x, after->transform.origin.y - origin.y);
    expect(!s.transformEdit(), "the held Move tool left a transform open");
    release(canvas, Qt::Key_Control, Qt::NoModifier);
    expect(!canvas->heldTool() && s.tool() == Tool::Brush, "letting go of Ctrl did not go back to the Brush");
    // Ctrl with the Pen: the Direct Selection tool while held, dragging an anchor of the path.
    {
        s.selectTool(Tool::Pen);
        s.penMode = EditorSession::PenMode::Path;   // the Work Path, so every component lands in one path
        for (QPointF p : {QPointF(100, 100), QPointF(200, 100), QPointF(200, 200)}) s.penPress(p);
        s.penFinish(false);
        const auto knotAt = [&](int sub, int knot) {
            const auto path = s.targetPath();
            return path && sub < int(path->subpaths.size()) && knot < int(path->subpaths[size_t(sub)].knots.size())
                ? std::optional<VectorPath::Knot>(path->subpaths[size_t(sub)].knots[size_t(knot)]) : std::nullopt;
        };
        if (!knotAt(0, 2)) { std::fprintf(stderr, "held keys: the Pen made no path\n"); return 1; }
        press(canvas, Qt::Key_Control, Qt::ControlModifier);
        expect(canvas->heldTool() == Tool::DirectSelect, "Ctrl with the Pen did not hold the Direct Selection tool");
        mouse(QEvent::MouseButtonPress, QPointF(200, 100), Qt::ControlModifier);
        mouse(QEvent::MouseMove, QPointF(210, 120), Qt::ControlModifier);
        mouse(QEvent::MouseButtonRelease, QPointF(210, 120), Qt::ControlModifier);
        auto moved = knotAt(0, 1);
        expect(moved && std::abs(moved->x - 210) < 0.5 && std::abs(moved->y - 120) < 0.5, "Ctrl-drag with the Pen did not move the anchor");
        expect(knotAt(0, 0) && knotAt(0, 0)->x == 100 && !s.penDraft(), "Ctrl-drag with the Pen moved more than the anchor, or added one");
        release(canvas, Qt::Key_Control, Qt::NoModifier);
        expect(!canvas->heldTool() && s.tool() == Tool::Pen, "letting go of Ctrl did not go back to the Pen");
        // A path being drawn: Ctrl-click away from it ends it open, as Photoshop's does.
        s.penPress(QPointF(300, 300));
        s.penPress(QPointF(360, 300));
        press(canvas, Qt::Key_Control, Qt::ControlModifier);
        expect(canvas->heldTool() == Tool::DirectSelect, "Ctrl while drawing a path did not hold the Direct Selection tool");
        click(QPointF(500, 60), Qt::ControlModifier);
        release(canvas, Qt::Key_Control, Qt::NoModifier);
        expect(!s.penDraft() && s.targetPath() && s.targetPath()->subpaths.size() == 2, "Ctrl-click while drawing did not end the path open");
        // Alt over an anchor with the Pen: Convert Point (the corner becomes smooth), no new anchor.
        const auto corner = knotAt(0, 1);
        click(QPointF(210, 120), Qt::AltModifier);
        const auto converted = knotAt(0, 1);
        expect(corner && converted && corner->inX == corner->x && (converted->inX != converted->x || converted->inY != converted->y),
               "Alt-click with the Pen on a corner did not make it smooth");
        expect(!s.undoNames().empty() && s.undoNames().back() == "Convert Point" && !s.penDraft() && s.targetPath()->subpaths[0].knots.size() == 3,
               "Alt-click with the Pen did not convert the point as one step");
        // Ctrl with the Direct Selection tool: Path Selection, the whole component dragged from one of its anchors.
        s.selectTool(Tool::DirectSelect);
        press(canvas, Qt::Key_Control, Qt::ControlModifier);
        expect(!canvas->heldTool(), "Ctrl with the Direct Selection tool held another tool");
        const auto first = knotAt(0, 0), last = knotAt(0, 2);
        mouse(QEvent::MouseButtonPress, QPointF(100, 100), Qt::ControlModifier);
        mouse(QEvent::MouseMove, QPointF(105, 108), Qt::ControlModifier);
        mouse(QEvent::MouseButtonRelease, QPointF(105, 108), Qt::ControlModifier);
        release(canvas, Qt::Key_Control, Qt::NoModifier);
        const auto first2 = knotAt(0, 0), last2 = knotAt(0, 2), other2 = knotAt(1, 0);
        expect(first && last && first2 && last2 && std::abs(first2->x - first->x - 5) < 0.5 && std::abs(last2->y - last->y - 8) < 0.5,
               "Ctrl-drag with the Direct Selection tool did not move the whole component");
        expect(other2 && other2->x == 300, "Ctrl-drag with the Direct Selection tool moved another component");
        s.selectTool(Tool::Brush);
    }

    // Ctrl+Space zooms in where clicked; Ctrl+Alt+Space out.
    const double zoom = s.viewport.zoom;
    press(canvas, Qt::Key_Control, Qt::ControlModifier);
    press(canvas, Qt::Key_Space, Qt::ControlModifier, QStringLiteral(" "));
    expect(canvas->heldTool() == Tool::Zoom, "Ctrl+Space did not hold the Zoom tool");
    click(QPointF(320, 210), Qt::ControlModifier);
    expect(std::abs(s.viewport.zoom - zoom * 2) < 1e-6, "Ctrl+Space click did not zoom in");
    press(canvas, Qt::Key_Alt, Qt::ControlModifier | Qt::AltModifier);
    expect(canvas->heldTool() == Tool::Zoom, "Ctrl+Alt+Space did not hold the Zoom tool");
    click(QPointF(320, 210), Qt::ControlModifier | Qt::AltModifier);
    expect(std::abs(s.viewport.zoom - zoom) < 1e-6, "Ctrl+Alt+Space click did not zoom out");
    release(canvas, Qt::Key_Alt, Qt::ControlModifier);
    release(canvas, Qt::Key_Space, Qt::ControlModifier);
    release(canvas, Qt::Key_Control, Qt::NoModifier);
    expect(!canvas->heldTool() && s.tool() == Tool::Brush, "letting go of Ctrl+Space did not go back to the Brush");

    // A tool's letter held while the tool is used springs back on release; a tap keeps the tool.
    expect(press(canvas, Qt::Key_M, Qt::NoModifier, QStringLiteral("m")), "M was not taken by the Marquee's shortcut");
    expect(s.tool() == Tool::Marquee, "M did not pick the Marquee");
    click(QPointF(40, 40), Qt::NoModifier);
    release(canvas, Qt::Key_M, Qt::NoModifier);
    expect(s.tool() == Tool::Brush, "letting go of M after using the Marquee did not go back to the Brush");
    press(canvas, Qt::Key_M, Qt::NoModifier, QStringLiteral("m"));
    release(canvas, Qt::Key_M, Qt::NoModifier);
    expect(s.tool() == Tool::Marquee, "a tap of M did not keep the Marquee");
    s.selectTool(Tool::Brush);
    // Shift+letter likewise: held while used it springs back to the tool and kind before; a tap keeps the new one.
    {
        s.marqueeKind = MarqueeKind::Rectangle;
        expect(press(canvas, Qt::Key_M, Qt::ShiftModifier, QStringLiteral("M")), "Shift+M was not taken by its shortcut");
        expect(s.tool() == Tool::Marquee && s.marqueeKind == MarqueeKind::Ellipse, "Shift+M did not pick the Elliptical Marquee");
        click(QPointF(40, 40), Qt::ShiftModifier);
        release(canvas, Qt::Key_M, Qt::ShiftModifier);
        expect(s.tool() == Tool::Brush && s.marqueeKind == MarqueeKind::Rectangle, "letting go of Shift+M after using it did not go back to the Brush");
        // On the tool itself only the kind changes, and comes back.
        s.selectTool(Tool::Marquee);
        press(canvas, Qt::Key_M, Qt::ShiftModifier, QStringLiteral("M"));
        click(QPointF(40, 40), Qt::ShiftModifier);
        release(canvas, Qt::Key_M, Qt::ShiftModifier);
        expect(s.tool() == Tool::Marquee && s.marqueeKind == MarqueeKind::Rectangle, "letting go of Shift+M on the Marquee did not go back to the Rectangular Marquee");
        s.selectTool(Tool::Brush);
        press(canvas, Qt::Key_G, Qt::ShiftModifier, QStringLiteral("G"));
        const Tool cycled = s.tool();
        release(canvas, Qt::Key_G, Qt::ShiftModifier);
        expect((cycled == Tool::Gradient || cycled == Tool::PaintBucket) && s.tool() == cycled, "a tap of Shift+G did not keep the tool it picked");
        s.selectTool(Tool::Brush);
        s.marqueeKind = MarqueeKind::Rectangle;
    }

    // F7: the Layers panel, not while a text field has the keys.
    QDockWidget* layers = w.findChild<QDockWidget*>(QStringLiteral("layersDock"));
    if (!layers) { std::fprintf(stderr, "no Layers panel\n"); return 1; }
    const bool shown = !layers->isHidden();
    press(canvas, Qt::Key_F7, Qt::NoModifier);
    expect(layers->isHidden() == shown, "F7 did not toggle the Layers panel");
    press(canvas, Qt::Key_F7, Qt::NoModifier);
    expect(layers->isHidden() != shown, "F7 again did not toggle the Layers panel back");
    {
        auto* field = new QLineEdit(&w);
        field->show();
        field->setFocus();
        QApplication::processEvents();
        if (QApplication::focusWidget() == field) {
            press(field, Qt::Key_F7, Qt::NoModifier);
            expect(layers->isHidden() != shown, "F7 in a text field toggled the Layers panel");
            const size_t layerCount = s.document()->layers.size();
            press(field, Qt::Key_Z, Qt::ControlModifier | Qt::AltModifier);
            expect(s.document()->layers.size() == layerCount, "Ctrl+Alt+Z in a text field changed the document");
        } else {
            std::fprintf(stderr, "held keys: the text field did not take the focus\n");
            failures++;
        }
        delete field;
        canvas->setFocus();
        QApplication::processEvents();
    }

    // Ctrl+Alt+Z: undo the last step, then redo it, then undo it again.
    const size_t layerCount = s.document()->layers.size();
    s.addBlankLayer();
    press(canvas, Qt::Key_Z, Qt::ControlModifier | Qt::AltModifier);
    expect(s.document()->layers.size() == layerCount, "Ctrl+Alt+Z did not undo the last step");
    press(canvas, Qt::Key_Z, Qt::ControlModifier | Qt::AltModifier);
    expect(s.document()->layers.size() == layerCount + 1, "Ctrl+Alt+Z again did not redo it");
    press(canvas, Qt::Key_Z, Qt::ControlModifier | Qt::AltModifier);
    expect(s.document()->layers.size() == layerCount, "Ctrl+Alt+Z a third time did not undo it");

    // F12: File > Revert, without a question (as Photoshop CC: it is one undo step); the saved layers come back, the
    // steps before stay, and Undo brings back the unsaved layer. Greyed while the document is as saved.
    {
        const QString path = QDir(QDir::tempPath()).filePath(QStringLiteral("nekophoto-held-keys-%1.nekophoto").arg(QCoreApplication::applicationPid()));
        QString error;
        if (!s.saveProject(path, &error)) { std::fprintf(stderr, "held keys: could not save %s: %s\n", qPrintable(path), qPrintable(error)); return 1; }
        QApplication::processEvents();
        QAction* revert = w.findChild<QAction*>(QStringLiteral("file.revert"));
        if (!revert) { std::fprintf(stderr, "held keys: no File > Revert\n"); return 1; }
        expect(!revert->isEnabled(), "File > Revert is available with nothing changed since the save");
        const size_t saved = s.document()->layers.size();
        s.addBlankLayer();
        QApplication::processEvents();
        expect(revert->isEnabled(), "File > Revert is greyed after a change");
        std::vector<std::string> steps = s.undoNames();
        bool asked = false;
        QTimer::singleShot(0, [&asked] {
            if (auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) { asked = true; box->reject(); }
        });
        expect(press(canvas, Qt::Key_F12, Qt::NoModifier), "F12 was not taken by File > Revert");
        QApplication::processEvents();
        expect(!asked, "Revert asked a question");
        expect(s.document()->layers.size() == saved && !s.isModified(), "F12 did not revert to the saved project");
        steps.push_back("Revert");
        expect(s.undoNames() == steps, "Revert is not one undo step after the ones before it");
        expect(!revert->isEnabled(), "File > Revert is available right after reverting");
        s.undo();
        expect(s.document()->layers.size() == saved + 1 && s.isModified(), "Undo did not bring back the document as it was before Revert");
        s.redo();
        expect(s.document()->layers.size() == saved && !s.isModified(), "Redo did not revert again");
        QFile::remove(path);
    }

    // Ctrl+Shift+> and < while typing: 2 pixels, 10 with Alt; Ctrl held while typing holds no tool.
    s.selectTool(Tool::Text);
    if (!canvas->startNewType(QPointF(100, 100), std::nullopt)) { std::fprintf(stderr, "held keys: could not start typing\n"); return 1; }
    for (QChar c : QStringLiteral("Hello")) press(canvas, 0, Qt::NoModifier, QString(c));
    press(canvas, Qt::Key_A, Qt::ControlModifier, QStringLiteral("a"));
    const double size = canvas->typeStyleAtCaret() ? canvas->typeStyleAtCaret()->fontSize : 0;
    press(canvas, Qt::Key_Control, Qt::ControlModifier);
    expect(!canvas->heldTool(), "Ctrl while typing held the Move tool");
    expect(!press(canvas, Qt::Key_Greater, Qt::ControlModifier | Qt::ShiftModifier, QStringLiteral(">")), "a shortcut took Ctrl+Shift+> while typing");
    expect(canvas->typeStyleAtCaret() && canvas->typeStyleAtCaret()->fontSize == size + 2, "Ctrl+Shift+> did not make the type 2 pixels larger");
    press(canvas, Qt::Key_Less, Qt::ControlModifier | Qt::AltModifier | Qt::ShiftModifier, QStringLiteral("<"));
    expect(canvas->typeStyleAtCaret() && canvas->typeStyleAtCaret()->fontSize == size - 8, "Ctrl+Alt+Shift+< did not make the type 10 pixels smaller");
    release(canvas, Qt::Key_Control, Qt::NoModifier);
    std::printf("type size: %g -> %g\n", size, canvas->typeStyleAtCaret() ? canvas->typeStyleAtCaret()->fontSize : 0);
    canvas->commitType();
    expect(!s.document()->layers.empty() && s.activeLayer() && s.activeLayer()->text && s.activeLayer()->text->fontSize == size - 8, "the committed type lost its size");

    // Not typing, with the Move or Type tool: the same keys size every selected type layer, one undo step per press
    // (quick repeats merge into it).
    {
        const Layer* typed = s.activeLayer();
        if (!typed || !typed->text) { std::fprintf(stderr, "held keys: no type layer\n"); return 1; }
        const Uuid first = typed->id;
        LayerText words = *typed->text;
        words.text = "World";
        words.runs.clear();
        words.fontSize = 30;
        const std::optional<Uuid> second = s.addTextLayer(QPointF(100, 200), words, false);
        if (!second) { std::fprintf(stderr, "held keys: could not add a second type layer\n"); return 1; }
        s.selectLayers({first, *second}, *second);
        s.selectTool(Tool::Move);
        canvas->setFocus();
        QApplication::processEvents();
        auto sizeOf = [&](const Uuid& id) { const auto t = s.layerText(id); return t ? t->fontSize : 0.0; };
        const double a = sizeOf(first);
        const size_t steps = s.undoNames().size();
        expect(!press(canvas, Qt::Key_Greater, Qt::ControlModifier | Qt::ShiftModifier, QStringLiteral(">")), "a shortcut took Ctrl+Shift+> with the Move tool");
        expect(sizeOf(first) == a + 2 && sizeOf(*second) == 32, "Ctrl+Shift+> with the Move tool did not make both selected type layers 2 pixels larger");
        expect(s.undoNames().size() == steps + 1 && s.undoNames().back() == "Edit Text", "Ctrl+Shift+> on two layers was not one undo step");
        press(canvas, Qt::Key_Less, Qt::ControlModifier | Qt::AltModifier | Qt::ShiftModifier, QStringLiteral("<"));
        expect(sizeOf(first) == std::max(1.0, a - 8) && sizeOf(*second) == 22, "Ctrl+Alt+Shift+< did not make both layers 10 pixels smaller");
        expect(s.undoNames().size() == steps + 1, "a quick second press made another undo step");
        std::printf("layer type size: %g, 30 -> %g, %g\n", a, sizeOf(first), sizeOf(*second));
        s.undo();
        expect(sizeOf(first) == a && sizeOf(*second) == 30, "one undo did not take back both presses");
        // The Type tool, nothing typed: the same, on the active layer alone.
        s.selectLayers({*second}, *second);
        s.selectTool(Tool::Text);
        press(canvas, Qt::Key_Period, Qt::ControlModifier | Qt::ShiftModifier, QStringLiteral(">"));
        expect(sizeOf(*second) == 32 && sizeOf(first) == a && !canvas->typeEditing(), "Ctrl+Shift+> with the Type tool did not size the selected layer alone");
        // Another tool leaves the keys alone.
        s.selectTool(Tool::Brush);
        press(canvas, Qt::Key_Greater, Qt::ControlModifier | Qt::ShiftModifier, QStringLiteral(">"));
        expect(sizeOf(*second) == 32, "Ctrl+Shift+> with the Brush changed the type");
    }

    std::printf("held keys: %s\n", failures ? "FAILED" : "ok");
    return failures ? 1 : 0;
}

/// File > Export > Export As, Quick Export and Layer > Export As: the dialog's estimate is the file's size, a scale
/// gives the file that size, each format's settings come back next time, and a layer exports cropped to its pixels.
int exportAs(MainWindow& w) {
    EditorSession& s = *w.session();
    buildDemoDocument(s);
    int failures = 0;
    auto expect = [&](bool ok, const QString& what) { if (!ok) { std::fprintf(stderr, "%s\n", qPrintable(what)); failures++; } };
    QTemporaryDir dir;
    if (!dir.isValid()) { std::fprintf(stderr, "no temporary folder\n"); return 1; }
    // Held open for the whole test: the settings stay in memory even where the file cannot be written.
    QSettings held;
    held.remove("export");
    // Errors the window would show in a dialog are collected and fail the test instead.
    QString errors;
    w.setErrorSink(&errors);
    const Document& doc = *s.document();
    // Writes what the dialog chose through document.export, as File > Export > Export As does.
    auto write = [&](ExportAsDialog& dialog, const QString& name) -> QString {
        QJsonObject params = dialog.commandParams();
        params["path"] = dir.filePath(name);
        params["overwrite"] = true;
        const auto reply = w.runCommand("document.export", params);
        w.setErrorSink(&errors);   // the request clears it when it ends
        expect(reply.has_value(), "document.export refused " + name);
        return dir.filePath(name);
    };
    {
        ExportAsDialog dialog(&s, false, QStringLiteral("png"), &w);
        expect(dialog.ready() && dialog.sourceSize() == QSize(doc.width, doc.height), "Export As does not start at the document's size");
        dialog.refreshNow();
        const QString png = write(dialog, "full.png");
        expect(dialog.estimateExact() && dialog.estimatedBytes() == QFileInfo(png).size(),
               QString("PNG estimate %1 is not the file's %2 bytes").arg(dialog.estimatedBytes()).arg(QFileInfo(png).size()));
        // JPEG at quality 40 and half the size: the estimate is still the file, and the file is that size.
        dialog.setFormat("jpg");
        if (auto* quality = dialog.findChild<QSlider*>("quality")) quality->setValue(40);
        dialog.setScalePercent(50);
        const int halfWidth = int(std::lround(doc.width * 0.5)), halfHeight = int(std::lround(doc.height * 0.5));
        expect(dialog.outputWidth() == halfWidth && dialog.outputHeight() == halfHeight, "50% does not halve the size");
        dialog.refreshNow();
        const QString jpg = write(dialog, "half.jpg");
        const QSize jpgSize = QImageReader(jpg).size();
        expect(jpgSize == QSize(halfWidth, halfHeight), QString("the JPEG is %1x%2").arg(jpgSize.width()).arg(jpgSize.height()));
        expect(dialog.estimatedBytes() == QFileInfo(jpg).size(), QString("JPEG estimate %1 is not the file's %2 bytes").arg(dialog.estimatedBytes()).arg(QFileInfo(jpg).size()));
        // Nearest neighbour at 200%: every pixel doubled.
        dialog.setFormat("png");
        if (auto* resample = dialog.findChild<QComboBox*>("resample")) resample->setCurrentIndex(resample->findData("nearest"));
        dialog.setScalePercent(200);
        const QString big = write(dialog, "double.png");
        const QImage doubled = QImage(big).convertToFormat(QImage::Format_ARGB32);
        const QImage single = QImage(png).convertToFormat(QImage::Format_ARGB32);
        expect(doubled.size() == single.size() * 2 && doubled.pixel(21, 33) == single.pixel(10, 16), "nearest neighbour at 200% does not double the pixels");
        dialog.setFormat("jpg");
        dialog.rememberSettings();
    }
    {
        // The next Export As opens on JPEG with its quality; PNG kept its own choices; the size is the image's own again.
        ExportAsDialog again(&s, false, {}, &w);
        expect(again.format() == "jpg" && again.settings().quality == 40, QString("remembered %1 at %2").arg(again.format()).arg(again.settings().quality));
        expect(exportas::remembered("jpg").quality == 40 && exportas::remembered("png").resample == exportas::Resample::Bicubic, "the settings were not kept per format");
        expect(again.outputWidth() == doc.width, "the size was remembered; it starts at the image's own");
    }
    {
        // Layer > Export As: the Red layer alone, cropped to its pixels.
        select(s, "Red");
        ExportAsDialog dialog(&s, true, QStringLiteral("png"), &w);
        const Layer* red = s.activeLayer();
        const Rect bounds = red->transform.bounds().integral().intersection(doc.rect());
        expect(dialog.ready() && dialog.sourceSize().width() <= int(bounds.width) && dialog.sourceSize().height() <= int(bounds.height) && dialog.sourceSize().width() < doc.width,
               QString("the layer exports at %1x%2, its bounds are %3x%4").arg(dialog.sourceSize().width()).arg(dialog.sourceSize().height()).arg(bounds.width).arg(bounds.height));
        const QString layerPng = write(dialog, "red.png");
        expect(QImageReader(layerPng).size() == dialog.sourceSize(), "the layer's file is not its visible size");
    }
    {
        // Quick Export: beside the saved document, in Preferences' format, with that format's remembered settings.
        QString error;
        expect(s.saveProject(dir.filePath("Quick.nekophoto"), &error), "couldn't save the project: " + error);
        exportas::setQuickExportFormat("jpg");
        w.setErrorSink(&errors);
        if (QAction* quick = action(w, "export.quick")) quick->trigger();
        const QString quick = dir.filePath("Quick.jpg");
        expect(QFileInfo::exists(quick) && QImageReader(quick).size() == QSize(doc.width, doc.height), "Quick Export did not write Quick.jpg at full size");
    }
    w.setErrorSink(nullptr);
    expect(errors.isEmpty(), "errors: " + errors);
    std::printf("export-as: %s\n", failures ? "FAILED" : "ok");
    return failures ? 1 : 0;
}


/// The command registry (CommandRegistry.h): every menu item is a command with a well-formed id of its own, and every
/// key the window answers to belongs to one action (an ambiguous key runs neither).
int registry(MainWindow& w) {
    int failures = 0;
    const CommandRegistry& reg = w.commandRegistry();
    static const QRegularExpression form(QStringLiteral("^[a-z][A-Za-z0-9]*(\\.[a-z0-9][A-Za-z0-9]*)+$"));
    std::set<QString> ids;
    for (const Command* c : reg.all()) {
        if (!form.match(c->id).hasMatch()) { std::fprintf(stderr, "registry: badly formed id \"%s\"\n", qPrintable(c->id)); failures++; }
        if (!ids.insert(c->id).second) { std::fprintf(stderr, "registry: id %s twice\n", qPrintable(c->id)); failures++; }
        if (!c->action || c->action->property("commandId").toString() != c->id) { std::fprintf(stderr, "registry: %s has no action\n", qPrintable(c->id)); failures++; }
        if (c->params && c->method.isEmpty()) { std::fprintf(stderr, "registry: %s sends a request without a method\n", qPrintable(c->id)); failures++; }
    }
    // Every item of every menu, the submenus' own items too (a submenu listed as it fills, Open Recent and Apply
    // Style, holds files and presets rather than commands).
    QList<QAction*> answering;
    int items = 0;
    std::function<void(QMenu*, const QString&)> walk = [&](QMenu* menu, const QString& path) {
        if (menu->property("commandsDynamic").toBool()) return;
        emit menu->aboutToShow();
        for (QAction* a : menu->actions()) {
            if (a->isSeparator()) continue;
            const QString here = path + QStringLiteral(" > ") + plainText(a->text());
            if (QMenu* sub = a->menu()) { walk(sub, here); continue; }
            items++;
            answering << a;
            if (!reg.forAction(a)) { std::fprintf(stderr, "registry: the menu item %s has no command\n", qPrintable(here)); failures++; }
        }
    };
    for (QAction* top : w.menuBar()->actions())
        if (QMenu* menu = top->menu()) walk(menu, plainText(top->text()));
    // The keys: the menus', the window's own (the channel keys, X and D, the tool kinds) and the tool rail's.
    answering << w.actions();
    for (QToolBar* bar : w.findChildren<QToolBar*>()) answering << bar->actions();
    std::map<QString, QAction*> keys;
    for (QAction* a : answering)
        for (const QKeySequence& key : a->shortcuts()) {
            if (key.isEmpty()) continue;
            const QString text = key.toString(QKeySequence::PortableText);
            auto [at, added] = keys.emplace(text, a);
            if (!added && at->second != a) {
                std::fprintf(stderr, "registry: %s is the key of both \"%s\" and \"%s\"\n", qPrintable(text), qPrintable(plainText(at->second->text())), qPrintable(plainText(a->text())));
                failures++;
            }
        }
    // The registry's default keys, likewise.
    std::map<QString, QString> defaults;
    for (const Command* c : reg.all())
        for (const QKeySequence& key : c->shortcuts) {
            if (key.isEmpty()) continue;   // a standard key the platform leaves unset (Save As, Quit on a bare Qt)
            auto [at, added] = defaults.emplace(key.toString(QKeySequence::PortableText), c->id);
            if (!added && at->second != c->id) { std::fprintf(stderr, "registry: %s is the default key of %s and %s\n", qPrintable(at->first), qPrintable(at->second), qPrintable(c->id)); failures++; }
        }
    std::printf("command registry: %d commands, %d menu items, %d keys, %s\n", int(ids.size()), items, int(keys.size()), failures ? "FAILED" : "ok");
    return failures;
}

/// File > New, Open, Import File, Save As and Layer > Smart Objects > Edit Contents through their dialogs: each runs its
/// method (document.new, document.open, document.import, document.save, smartObject.editContents), recorded once, and
/// gives what the window's own calls gave before.
int fileCommands(MainWindow& w) {
    int failures = 0;
    auto expect = [&](bool ok, const QString& what) { if (!ok) { std::fprintf(stderr, "file commands: %s\n", qPrintable(what)); failures++; } };
    QTemporaryDir dir;
    const QString png = dir.filePath(QStringLiteral("opened.png"));
    {
        QImage image(40, 30, QImage::Format_ARGB32);
        for (int y = 0; y < 30; y++) for (int x = 0; x < 40; x++) image.setPixel(x, y, qRgba(x * 6, y * 8, 120, 255));
        image.save(png);
    }
    auto pick = [](const QString& path) {
        return [path](QDialog* d) { if (auto* f = qobject_cast<QFileDialog*>(d)) f->selectFile(path); else std::fprintf(stderr, "not a file dialog: %s\n", d->metaObject()->className()); };
    };
    // Qt's own file dialog, whatever the desktop's platform theme offers, so the test can answer it.
    const bool nativeDialogs = !QCoreApplication::testAttribute(Qt::AA_DontUseNativeDialogs);
    QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs, true);
    // Untagged images open in the working space without the question, as a person who turned it off sees.
    const color::Settings colours = color::settings();
    {
        color::Settings quiet = colours;
        quiet.askMissing = quiet.askMismatch = false;
        color::setSettings(quiet);
    }
    // A dialog nobody answers (a question the test does not expect) fails the test instead of waiting for ever.
    QTimer watchdog;
    QPointer<QWidget> waiting;
    int waited = 0;
    QObject::connect(&watchdog, &QTimer::timeout, [&] {
        QWidget* modal = QApplication::activeModalWidget();
        if (!modal || modal != waiting) { waiting = modal; waited = 0; return; }
        if (++waited < 50) return;
        std::fprintf(stderr, "file commands: an unexpected %s (\"%s\") was left open\n", modal->metaObject()->className(), qPrintable(modal->windowTitle()));
        failures++;
        if (auto* d = qobject_cast<QDialog*>(modal)) d->reject(); else modal->close();
    });
    watchdog.start(100);
    auto run = [&](const QStringList& path, std::function<void(QDialog*)> answer) {
        QAction* a = menuItem(w, path);
        if (!a || !a->isEnabled()) { expect(false, path.join(" > ") + " is not there or is greyed"); return; }
        if (answer) answerModal(answer);
        a->trigger();
        QApplication::processEvents();
    };
    ActionLibrary::instance().startRecording(QStringLiteral("file-commands self-test"));
    // New: the dialog's size, in a tab of its own (this one holds a document).
    const int tabs = w.tabCount();
    run({"File", "New…"}, [](QDialog* d) { nth<QSpinBox>(d, 0)->setValue(120); nth<QSpinBox>(d, 1)->setValue(80); });
    expect(w.tabCount() == tabs + 1 && w.session()->hasDocument() && w.session()->document()->width == 120 && w.session()->document()->height == 80, "New did not make a 120 x 80 document in a new tab");
    const QString made = describe(*w.session());
    // Open: the image as a document of its own, in another tab.
    run({"File", "Open…"}, pick(png));
    expect(w.tabCount() == tabs + 2 && w.session()->document()->width == 40, "Open did not open the image in a tab of its own");
    const QString opened = describe(*w.session());
    // Import File: into the document on screen, as a layer.
    const size_t layersBefore = w.session()->document()->layers.size();
    run({"File", "Import File…"}, pick(png));
    expect(w.session()->document()->layers.size() == layersBefore + 1, "Import File did not add a layer");
    const QString imported = describe(*w.session());
    // Save As: a project file the dialog names.
    const QString project = dir.filePath(QStringLiteral("saved.nekophoto"));
    run({"File", "Save As…"}, pick(project));
    expect(QFileInfo::exists(project) && w.session()->projectPath() == project, "Save As did not save " + project);
    // Edit Contents: a smart object's contents, in a tab of their own.
    w.session()->convertToSmartObject(nullptr);
    const int beforeContents = w.tabCount();
    run({"Layer", "Smart Objects", "Edit Contents"}, {});
    expect(w.tabCount() == beforeContents + 1 && w.session()->smartObjectParent(), "Edit Contents did not open the contents");
    const QString contents = describe(*w.session());
    ActionLibrary::instance().stopRecording();
    QStringList recorded;
    if (const RecordedAction* r = ActionLibrary::instance().find(QStringLiteral("file-commands self-test")))
        for (const ActionStep& step : r->steps) recorded << step.method;
    ActionLibrary::instance().remove(QStringLiteral("file-commands self-test"));
    // (The smart object was made by a session call, which records nothing.)
    const QStringList wanted{"document.new", "document.open", "document.import", "document.save", "smartObject.editContents"};
    expect(recorded == wanted, "recorded [" + recorded.join(", ") + "], expected [" + wanted.join(", ") + "]");

    // The same through the window's own calls, as before the command path.
    w.newTab();
    w.session()->createDocument(120, 80, 72, true);
    w.session()->adoptProfile(color::newDocumentProfile());
    expect(describe(*w.session()) == made, "New gives another document than before");
    QString error;
    expect(w.openImageAsDocument(png, &error), "could not open the image: " + error);
    expect(describe(*w.session()) == opened, "Open gives another document than before");
    expect(w.importImageFile(png, std::nullopt, &error), "could not import the image: " + error);
    expect(describe(*w.session()) == imported, "Import File gives another document than before");
    w.session()->convertToSmartObject(nullptr);
    expect(w.editSmartObjectContents(&error), "could not open the contents: " + error);
    expect(describe(*w.session()) == contents, "Edit Contents gives other contents than before");
    watchdog.stop();
    color::setSettings(colours);
    QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs, !nativeDialogs);
    std::printf("file commands: %s\n", failures ? "FAILED" : "ok");
    return failures;
}

} // namespace

/// Edit > Preferences: the autosave interval applies as it is changed (Off stops it, a number starts it again) rather
/// than when the dialog closes, and the settings that apply only at the next launch say so beside their controls.
int preferences(MainWindow& w) {
    int failures = 0;
    auto fail = [&](const char* what) { std::fprintf(stderr, "preferences: %s\n", what); failures++; };
    // This run's own recovery folder only: nothing left by another run is offered back (which would wait on a dialog).
    QDir(Autosave::root()).removeRecursively();
    w.enableAutosave();
    Autosave* autosave = w.autosave();
    if (!autosave) { fail("no autosaver"); return failures; }
    QAction* item = menuItem(w, {"Edit", "Preferences…"});
    if (!item) return 1;
    item->trigger();
    QApplication::processEvents();
    PreferencesDialog* dialog = w.findChild<PreferencesDialog*>();
    if (!dialog) { fail("the dialog did not open"); return failures; }
    auto* minutes = dialog->findChild<QSpinBox*>("autosaveMinutes");
    if (!minutes) { fail("no autosave interval"); dialog->close(); return failures; }
    minutes->setValue(0);
    if (autosave->running()) fail("Off left autosave running while the dialog is open");
    minutes->setValue(7);
    if (!autosave->running()) fail("turning autosave on did not start it while the dialog is open");
    else if (autosave->intervalMs() != 7 * 60 * 1000) fail("the new interval did not apply while the dialog is open");
    minutes->setValue(2);
    if (autosave->intervalMs() != 2 * 60 * 1000) fail("a changed interval did not apply while the dialog is open");
    // Language, CPU power and Automation apply at the next launch, and each says so where it is set.
    const QString nextLaunch = PreferencesDialog::tr("Takes effect the next time NekoPhoto starts.");
    int saying = 0;
    for (QLabel* label : dialog->findChildren<QLabel*>()) if (label->text().contains(nextLaunch)) saying++;
    if (saying < 3) { std::fprintf(stderr, "preferences: %d hints say the setting applies at the next launch, expected 3\n", saying); failures++; }
    dialog->close();
    QApplication::processEvents();
    autosave->finish();
    std::printf("preferences: %s\n", failures ? "FAILED" : "ok");
    return failures;
}

int workCounters(MainWindow& window);   // SelfTestWork.cpp

int runSelfTest(MainWindow& window, const QString& name) {
    if (name == QLatin1String("command-path")) {
        const int first = commandPath(window);
        const int menus = menuCommands(window);
        const int files = fileCommands(window);
        return registry(window) || menus || files || first ? 1 : 0;
    }
    if (name == QLatin1String("guides")) return guides(window);
    if (name == QLatin1String("canvas-menus")) return canvasMenus(window);
    if (name == QLatin1String("search")) return search(window);
    if (name == QLatin1String("held-keys")) return heldKeys(window);
    if (name == QLatin1String("export-as")) return exportAs(window);
    if (name == QLatin1String("preferences")) return preferences(window);
    if (name == QLatin1String("work-counters")) return workCounters(window);
    std::fprintf(stderr, "unknown self-test %s (command-path, guides, canvas-menus, search, held-keys, export-as, preferences, work-counters)\n", qPrintable(name));
    return 2;
}

} // namespace app
