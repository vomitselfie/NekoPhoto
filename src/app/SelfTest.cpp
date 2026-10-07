// In-app checks that need the whole window (run by ctest through `nekophoto --self-test <name>`).
//
// command-path: the converted commands (New Layer, Duplicate Layer, Merge Down, Gaussian Blur's OK, Levels' OK and
// Free Transform's commit) give the same document and the same history whether they come from the interface
// (menu actions, the dialogs, the canvas's Enter) or from automation requests, and an action recording the
// interface path holds the requests automation would send. Then the same for the menu items, dialogs and panel
// buttons converted after them (menuCommands): each through the menu bar (answering its dialog), through the
// requests the recording holds, and through the session calls the interface made before, compared as documents and
// history names, with each recorded once as the method it names.
//
// held-keys: Photoshop's held tools (Alt, Ctrl, Ctrl+Space), spring-loaded tool letters, F7, F12, Ctrl+Alt+Z and the
// type size keys, through synthesised key and mouse events.
#include "SelfTest.h"
#include "ActionLibrary.h"
#include "AdjustmentEditor.h"
#include "Automation.h"
#include "CanvasWidget.h"
#include "ChannelsPanel.h"
#include "CommandPalette.h"
#include "FilterDialog.h"
#include "LayersPanel.h"
#include "Names.h"
#include "PathsPanel.h"
#include "MainWindow.h"
#include "Ruler.h"
#include <QApplication>
#include <QDoubleSpinBox>
#include <QComboBox>
#include <QInputDialog>
#include <QLineEdit>
#include <QSlider>
#include <QSpinBox>
#include <QTreeWidgetItemIterator>
#include <QTimer>
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
};

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

    const std::vector<Converted> steps = {
        {"New Layer Below", "Paint", {}, trigger({"Layer", "New Layer Below"}), [](EditorSession& s, auto&) { s.addBlankLayer(true); }, {"layers.add"}},
        {"New Folder", "Paint", {}, trigger({"Layer", "New Folder"}), [](EditorSession& s, auto&) { s.addGroup(); }, {"layers.add"}},
        {"New Adjustment Layer", "Paint", {}, trigger({"Layer", "New Adjustment Layer", names::adjustmentKind(AdjustmentKind::Curves)}),
         [](EditorSession& s, auto&) { s.addAdjustmentLayer(AdjustmentKind::Curves); }, {"layers.add"}},
        {"Rename Layer", nullptr, {}, trigger({"Layer", "Rename Layer…"}, inputValue(QStringLiteral("Renamed"))),
         [](EditorSession& s, auto&) { s.renameLayer(*s.activeLayerId(), "Renamed"); }, {"layers.set"}},
        {"Delete Layer", "Renamed", {}, trigger({"Layer", "Delete Layer"}), [](EditorSession& s, auto&) { s.deleteLayersResolvingClipping({*s.activeLayerId()}, false); }, {"layers.delete"}},
        {"Group Layers", "Paint", {}, trigger({"Layer", "Group Layers"}), [](EditorSession& s, auto&) { s.groupSelectedLayers(); }, {"layers.group"}},
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
        // Last: it merges most of the document. Red hidden first, so a hidden layer stays out of the merge.
        {"Merge Visible", "Paint", [](EditorSession& s) { const Layer* red = s.document()->find(layerId(s, "Red")); if (red && red->visible) s.toggleLayerVisibility(red->id); },
         trigger({"Layer", "Merge Visible"}), [](EditorSession& s, auto&) { s.mergeVisible(); }, {"layers.merge"}},
    };

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

    // The interface, recorded.
    w.newTab();
    EditorSession& a = *w.session();
    prepare(a);
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
    for (const Converted& step : steps) {
        current = step.label;
        before(a, step);
        namesAt.emplace_back();
        for (const Layer& l : a.document()->layers) namesAt.back()[l.id] = l.name;
        recordedBefore.push_back(int(recorded().size()));
        const auto history = a.undoNames();   // (the list is capped: compare it, not its length)
        if (!step.ui(w, a)) { std::fprintf(stderr, "%s: could not be done through the interface\n", qPrintable(step.label)); failures++; }
        if ((a.undoNames() == history) == step.edits) { std::fprintf(stderr, step.edits ? "%s left no history step\n" : "%s left a history step\n", qPrintable(step.label)); failures++; }
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
            for (const char* key : {"id", "parent", "above"}) {
                if (!sent.value(key).isString()) continue;
                auto name = namesAt[i].find(sent.value(key).toString().toStdString());
                if (name == namesAt[i].end()) continue;
                for (const Layer& l : b.document()->layers) if (l.name == name->second) sent[key] = QString::fromStdString(l.id);
            }
            const QJsonObject reply = engine->handle(QJsonObject{{"jsonrpc", "2.0"}, {"id", 1}, {"method", steps_[k].method}, {"params", sent}});
            if (reply.contains("error")) { std::fprintf(stderr, "%s: %s\n", qPrintable(steps_[k].method), qPrintable(reply.value("error").toObject().value("message").toString())); failures++; }
        }
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
    }
    const QString direct = describe(c);

    if (viaInterface != viaAutomation) {
        std::fprintf(stderr, "menu commands: the interface and automation differ:\n--- interface\n%s\n--- automation\n%s\n", qPrintable(viaInterface), qPrintable(viaAutomation));
        failures++;
    }
    if (viaInterface != direct) {
        std::fprintf(stderr, "menu commands: the command path changed what the interface does:\n--- now\n%s\n--- before\n%s\n", qPrintable(viaInterface), qPrintable(direct));
        failures++;
    }
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
/// Ctrl+Alt+Z toggles the last state, F12 reverts (asking first), and Ctrl+Shift+> and < size the type being typed.
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

    // F12: File > Revert, asking first with unsaved changes (answered Revert here); the saved layers come back.
    {
        const QString path = QDir(QDir::tempPath()).filePath(QStringLiteral("nekophoto-held-keys-%1.nekophoto").arg(QCoreApplication::applicationPid()));
        QString error;
        if (!s.saveProject(path, &error)) { std::fprintf(stderr, "held keys: could not save %s: %s\n", qPrintable(path), qPrintable(error)); return 1; }
        QApplication::processEvents();
        const size_t saved = s.document()->layers.size();
        s.addBlankLayer();
        QApplication::processEvents();
        bool asked = false;
        QTimer::singleShot(0, [&asked] {
            if (auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget()))
                for (QAbstractButton* b : box->buttons())
                    if (b->text() == QLatin1String("Revert")) { asked = true; b->click(); return; }
        });
        expect(press(canvas, Qt::Key_F12, Qt::NoModifier), "F12 was not taken by File > Revert");
        expect(asked, "Revert did not ask about the unsaved changes");
        expect(s.document()->layers.size() == saved && !s.isModified(), "F12 did not revert to the saved project");
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

} // namespace

int runSelfTest(MainWindow& window, const QString& name) {
    if (name == QLatin1String("command-path")) {
        const int first = commandPath(window);
        return menuCommands(window) || first ? 1 : 0;
    }
    if (name == QLatin1String("guides")) return guides(window);
    if (name == QLatin1String("canvas-menus")) return canvasMenus(window);
    if (name == QLatin1String("search")) return search(window);
    if (name == QLatin1String("held-keys")) return heldKeys(window);
    std::fprintf(stderr, "unknown self-test %s (command-path, guides, canvas-menus, search, held-keys)\n", qPrintable(name));
    return 2;
}

} // namespace app
