// command-path, continued (SelfTestCommandPath.cpp has the first six commands): the menu items, dialogs and panel
// buttons converted after them, each through the menu bar (answering its dialog), through the requests the
// recording holds, and through the session calls the interface made before, compared as documents and history
// names, with each recorded once as the method it names.
#include "nekophoto/trim.h"
#include "SelfTestSupport.h"
#include "ActionLibrary.h"
#include "AdjustmentEditor.h"
#include "Automation.h"
#include "CameraRawDialog.h"
#include "CameraRawPanels.h"
#include "CanvasWidget.h"
#include "ChannelDialogs.h"
#include "ChannelsPanel.h"
#include "CommandRegistry.h"
#include "ContentAwareScaleDialog.h"
#include "EditorSession.h"
#include "FilterDialog.h"
#include "Gmic.h"
#include "GmicDialog.h"
#include "LayerStyleDialog.h"
#include "LayersPanel.h"
#include "MainWindow.h"
#include "ModelStore.h"
#include "MoshDialog.h"
#include "Names.h"
#include "PathsPanel.h"
#include "PresetLibrary.h"
#include "WarpDialog.h"
#include "nekophoto/warpmesh.h"
#include <QApplication>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QInputDialog>
#include <QJsonArray>
#include <QKeyEvent>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPushButton>
#include <QSlider>
#include <QSpinBox>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidgetItemIterator>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <utility>
#include <vector>

using namespace nekophoto;

namespace app {

using namespace selftest;

namespace {

/// The style preset Layer > Layer Style > Apply Style applies (imported for the test, removed after it).
constexpr const char* kStylePreset = "Self-test Shadow";

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

} // namespace

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
        {"Panel Mask", "Background", backgroundUnderRed, layersButton(LayersPanel::tr("Add layer mask (it reveals the selection; Alt-click: hides it)")),
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
             auto* dialog = new MoshDialog(&s, *nekophoto::mosh::findEffect("wave"), &w);
             dialog->accept();
             QApplication::processEvents();
             return true;
         }, [window](EditorSession& s, auto&) { (new MoshDialog(&s, *nekophoto::mosh::findEffect("wave"), window))->accept(); }, {"pixels.mosh"}},
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

} // namespace app
