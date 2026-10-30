// The Layer and Type menus (MainWindow::buildMenus, MainWindowMenus.h).
#include "MainWindowMenus.h"
#include "ExportAs.h"
#include "Automation.h"
#include "CommandPalette.h"
#include "Names.h"
#include "LayerStyleDialog.h"
#include "nekophoto/raw.h"
#include "CanvasFrame.h"
#include "PresetLibrary.h"
#include <QInputDialog>
#include <QMenuBar>
#include <QMessageBox>
#include <QFileDialog>
#include <QSettings>
#include <QJsonObject>
#include <QLineEdit>
#include <cmath>

using namespace nekophoto;

namespace app {

using namespace menus;

void MainWindow::buildLayerMenu(MenuContext& m) {
    QMenu* layer = menuBar()->addMenu(tr("&Layer"));
    // Converted to the command path (CONTRIBUTING.md, "Commands"): the item runs the automation method itself.
    m.nameAction("layer.new", m.add(layer, Spec("layer.new", tr("&New Layer"), {QKeySequence("Ctrl+Shift+N")}).document("layers.structure")
                                           .request("layers.add", [] { return QJsonObject{}; }, tr("New Layer"))))->setObjectName("command.layers.add");
    m.add(layer, Spec("layer.newBelow", tr("New Layer &Below")).document("layers.structure")
                   .request("layers.add", [] { return QJsonObject{{"below", true}}; }, tr("New Layer Below")))->setObjectName("command.layers.addBelow");
    m.add(layer, Spec("layer.newFolder", tr("New &Folder")).document("layers.structure")
                   .request("layers.add", [] { return QJsonObject{{"kind", "group"}}; }, tr("New Folder")))->setObjectName("command.layers.addGroup");
    m.add(layer, Spec("layer.group", tr("&Group Layers"), {QKeySequence("Ctrl+G")}).document("layers.structure")
                   .request("layers.group", [] { return QJsonObject{}; }, tr("Group Layers")))->setObjectName("command.layers.group");
    m.nameAction("layer.viaCopy", m.add(layer, Spec("layer.viaCopy", tr("Layer via &Copy"), {QKeySequence("Ctrl+J")}).document("edit.clipboard").request("layers.viaCopy", [this]() -> std::optional<QJsonObject> {
        const Layer* l = session_->activeLayer();
        if (!l || l->isGroup) return std::nullopt;
        return QJsonObject{};
    }, tr("Layer via Copy"))));
    m.nameAction("layer.duplicate", m.add(layer, Spec("layer.duplicate", tr("&Duplicate Layer")).document("layers.structure")
                                                 .request("layers.duplicate", [] { return QJsonObject{}; }, tr("Duplicate Layer"))))->setObjectName("command.layers.duplicate");
    m.nameAction("layer.delete", m.add(layer, Spec("layer.delete", tr("De&lete Layer")).document("layers.structure").runs([this] { deleteLayersCommand(); }, "layers.delete")))
        ->setObjectName("command.layers.delete");
    mergeAction_ = m.add(layer, Spec("layer.mergeDown", tr("Merge Do&wn"), {QKeySequence("Ctrl+E")}).document("layers.merge")
                                  .runs([this] { runCommand("layers.merge", {}, mergeAction_->text().remove('&')); }, "layers.merge")
                                  .when([this] { return session_->canMergeLayers() ? QString() : tr("Nothing to merge: select the layers to merge, or a layer with a layer below it"); }));
    mergeAction_->setObjectName("command.layers.merge");
    named_["layer.merge"] = mergeAction_;
    mergeVisibleAction_ = m.add(layer, Spec("layer.mergeVisible", tr("Merge &Visible"), {QKeySequence("Ctrl+Shift+E")}).document("layers.merge")
                                         .request("layers.merge", [] { return QJsonObject{{"visible", true}}; }, tr("Merge Visible"))
                                         .when([this] { return session_->canMergeVisible() ? QString() : tr("Nothing to merge: fewer than two layers are visible"); }));
    mergeVisibleAction_->setObjectName("command.layers.mergeVisible");
    editTextAction_ = m.nameAction("layer.editText", m.add(layer, Spec("layer.editText", tr("Edit &Text…")).document("edit.text").runs([this] {
        const Layer* l = session_->activeLayer();
        if (l && l->isLiveText()) session_->requestTextEdit(l->id);
    }).when([this] {
        if (session_->featuresGated()) return session_->unavailableTip();
        const Layer* l = session_->activeLayer();
        return l && l->isLiveText() ? QString() : tr("The active layer is not a text layer");
    })));
    m.nameAction("layer.rename", m.add(layer, Spec("layer.rename", tr("&Rename Layer…")).document("layers.structure").runs([this] {
        const Layer* active = session_->activeLayer();
        if (!active) return;
        bool ok;
        QString name = QInputDialog::getText(this, tr("Rename Layer"), tr("Name"), QLineEdit::Normal, QString::fromStdString(active->name), &ok);
        if (ok) runCommand("layers.set", {{"name", name}}, tr("Rename Layer"));
    }, "layers.set")));
    // Move Out of Folder: layers.move, the layer placed directly above its folder in the folder's own parent.
    m.add(layer, Spec("layer.moveOutOfFolder", tr("Move &Out of Folder"), {QKeySequence("Ctrl+Shift+[")}).document("layers.structure").request("layers.move", [this]() -> std::optional<QJsonObject> {
        const Layer* l = session_->activeLayer();
        const Layer* folder = l && l->parentId ? session_->document()->find(*l->parentId) : nullptr;
        if (!folder) return std::nullopt;
        QJsonObject params{{"id", QString::fromStdString(l->id)}, {"above", QString::fromStdString(folder->id)}};
        if (folder->parentId) params["parent"] = QString::fromStdString(*folder->parentId);
        return params;
    }, tr("Move Out of Folder")))->setObjectName("command.layers.moveOut");
    // The active layer alone, cropped to its visible pixels, as Photoshop's Layer > Quick Export and Export As.
    layer->addSeparator();
    QAction* layerQuickExport = m.add(layer, Spec("layer.quickExport", tr("Quick Export"), {QKeySequence("Ctrl+Shift+'")}).document("export.png").runs([this] { quickExport(true); }, "document.export"));
    layerQuickExport->setObjectName("export.layerQuick");
    layerQuickExport->setText(tr("Quick Export as %1").arg(exportas::formatLabel(exportas::quickExportFormat())));
    m.add(layer, Spec("layer.exportAs", tr("Export As…"), {QKeySequence("Ctrl+Alt+Shift+'")}).document("export.png").runs([this] { exportAs(true); }, "document.export"))->setObjectName("export.layerAs");
    connect(layer, &QMenu::aboutToShow, this, [layerQuickExport] { layerQuickExport->setText(MainWindow::tr("Quick Export as %1").arg(exportas::formatLabel(exportas::quickExportFormat()))); });
    layer->addSeparator();
    QMenu* adjustmentLayers = layer->addMenu(tr("New &Adjustment Layer"));
    for (int i = 0; i < adjustmentKindCount; i++) {
        AdjustmentKind kind = AdjustmentKind(i);
        m.add(adjustmentLayers, Spec(QStringLiteral("layer.newAdjustment.") + commandSlug(QString::fromUtf8(adjustmentKindName(kind))), names::adjustmentKind(kind))
                                  .document((std::string("adjustment.") + adjustmentKindName(kind)).c_str())
                                  .request("layers.add", [kind] { return QJsonObject{{"kind", "adjustment"}, {"adjustmentKind", QString::fromUtf8(adjustmentKindName(kind))}}; }, tr("New Adjustment Layer")));
    }
    QMenu* styles = layer->addMenu(tr("Layer St&yle"));
    const char* const stylePages[] = {QT_TRANSLATE_NOOP("app::MainWindow", "Blending Options…"), QT_TRANSLATE_NOOP("app::MainWindow", "Bevel & Emboss…"), QT_TRANSLATE_NOOP("app::MainWindow", "Stroke…"), QT_TRANSLATE_NOOP("app::MainWindow", "Inner Shadow…"), QT_TRANSLATE_NOOP("app::MainWindow", "Inner Glow…"), QT_TRANSLATE_NOOP("app::MainWindow", "Satin…"), QT_TRANSLATE_NOOP("app::MainWindow", "Color Overlay…"),
                                      QT_TRANSLATE_NOOP("app::MainWindow", "Gradient Overlay…"), QT_TRANSLATE_NOOP("app::MainWindow", "Pattern Overlay…"), QT_TRANSLATE_NOOP("app::MainWindow", "Outer Glow…"), QT_TRANSLATE_NOOP("app::MainWindow", "Drop Shadow…")};
    for (int page = 0; page < int(std::size(stylePages)); page++) {
        // The dialog's OK runs layers.setStyle with the style it shows.
        m.add(styles, Spec(QStringLiteral("layer.style.") + commandSlug(QString::fromLatin1(stylePages[page])), tr(stylePages[page])).document("edit.style").runs([this, page] {
            if (session_->activeLayerId()) LayerStyleDialog(session_, *session_->activeLayerId(), this, page).exec();
        }, "layers.setStyle").when(m.hasLayer));
        if (page == 0) styles->addSeparator();
    }
    styles->addSeparator();
    // Copy Layer Style keeps what layers.style answers; Paste and Clear run layers.setStyle (Photoshop's step names).
    m.add(styles, Spec("layer.style.copy", tr("&Copy Layer Style")).document("edit.style").runs([this] {
        if (!session_->activeLayerHasStyle()) return;
        const auto answer = runCommand("layers.style", {}, tr("Copy Layer Style"));
        if (!answer) return;
        nekophoto::LayerStyle style;
        if (QString error; layerStyleFromRequest(answer->toObject(), session_->document()->colorMode, style, &error)) session_->setStyleClipboard(style);
        else showError(tr("Copy Layer Style"), error);
    }, "layers.style"))->setObjectName("command.layers.copyStyle");
    m.add(styles, Spec("layer.style.paste", tr("&Paste Layer Style")).document("edit.style").request("layers.setStyle", [this]() -> std::optional<QJsonObject> {
        const auto& copied = session_->styleClipboard();
        const auto id = session_->activeLayerId();
        if (!copied || !id || !session_->canStyleLayer(*id)) return std::nullopt;
        return QJsonObject{{"style", layerStyleRequest(*copied, session_->document()->colorMode)}, {"paste", true}};
    }, tr("Paste Layer Style")))->setObjectName("command.layers.pasteStyle");
    m.add(styles, Spec("layer.style.clear", tr("C&lear Layer Style")).document("edit.style").request("layers.setStyle", [this]() -> std::optional<QJsonObject> {
        if (!session_->activeLayerHasStyle() || !session_->canStyleLayer(*session_->activeLayerId())) return std::nullopt;
        return QJsonObject{{"style", QJsonObject{}}};
    }, tr("Clear Layer Style")))->setObjectName("command.layers.clearStyle");
    styles->addSeparator();
    // Imported style presets (.asl): the submenu lists the library as it is when it opens; each runs layers.applyStyle.
    QMenu* applyStyle = styles->addMenu(tr("&Apply Style"));
    applyStyle->setProperty("commandsDynamic", true);
    m.reg.adopt(nullptr, applyStyle->menuAction(), Spec("layer.style.apply", plainText(applyStyle->title())).document("edit.style").command);
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
    m.add(styles, Spec("layer.style.import", tr("&Import Styles…")).runs([this] { importPresetsInteractively(this, session_); }, "presets.import"));
    QMenu* smart = layer->addMenu(tr("Smart Ob&jects"));
    m.nameAction("smart.convert", m.add(smart, Spec("layer.smartObject.convert", tr("&Convert to Smart Object")).document("edit.smartObject")
                                               .request("smartObject.convert", [] { return QJsonObject{}; }, tr("Couldn’t convert to a smart object"))));
    m.nameAction("smart.viaCopy", m.add(smart, Spec("layer.smartObject.viaCopy", tr("New Smart Object via &Copy")).document("edit.smartObject").request("smartObject.viaCopy", [this]() -> std::optional<QJsonObject> {
        const Layer* l = session_->activeLayer();
        if (!l || !l->isLiveSmartObject()) return std::nullopt;
        return QJsonObject{};
    }, tr("Couldn’t copy the smart object"))));
    m.nameAction("smart.edit", m.add(smart, Spec("layer.smartObject.editContents", tr("&Edit Contents")).document("edit.smartObject").runs([this] {
        // A camera RAW smart object reopens in Camera Raw (the dialog is the interface); the rest run
        // smartObject.editContents, the contents in a tab of their own.
        if (session_->activeRawSmartObject()) { editSmartObjectContents(); return; }
        runCommand("smartObject.editContents", {}, tr("Couldn’t open the contents"));
    }, "smartObject.editContents")))->setObjectName("command.smartObject.editContents");
    m.nameAction("smart.replace", m.add(smart, Spec("layer.smartObject.replace", tr("&Replace Contents…")).document("edit.smartObject").runs([this] {
        const QString path = QFileDialog::getOpenFileName(this, tr("Replace Contents"), QSettings().value("lastDir").toString(),
                                                          tr("Images, Photoshop and Affinity documents (*.psd *.psb *.afphoto *.afdesign *.afpub *.af *.png *.jpg *.jpeg *.tif *.tiff *.webp *.bmp *.gif %1)").arg(nekophoto::rawSupported() ? QStringLiteral("*.cr2 *.cr3 *.crw *.nef *.nrw *.arw *.srf *.sr2 *.raf *.orf *.rw2 *.rwl *.pef *.dng *.3fr *.iiq *.erf *.kdc *.dcr *.mrw *.srw *.x3f") : QString()));
        if (path.isEmpty()) return;
        runCommand("smartObject.replace", {{"path", path}}, tr("Couldn’t replace the contents"));
    }, "smartObject.replace")));
    m.nameAction("smart.rasterize", m.add(smart, Spec("layer.smartObject.rasterize", tr("R&asterize")).document("edit.smartObject").request("smartObject.rasterize", [this]() -> std::optional<QJsonObject> {
        const Layer* l = session_->activeLayer();
        if (!l || !l->smartObject) return std::nullopt;
        return QJsonObject{};
    }, tr("Rasterize"))));
    layer->addSeparator();
    QMenu* mask = layer->addMenu(tr("Layer &Mask"));
    // layers.mask, on the active layer (none: nothing to do).
    auto maskItem = [&](QMenu* menu, const char* id, const QString& label, const char* feature, QJsonObject params, bool selection = false) {
        return m.add(menu, Spec(id, label).document(feature).request("layers.mask", [this, params]() -> std::optional<QJsonObject> {
            if (!session_->activeLayerId()) return std::nullopt;
            return params;
        }, tr("Layer Mask")).when([this, hasLayer = m.hasLayer, selection] {
            const QString why = hasLayer();
            if (!why.isEmpty() || !selection) return why;
            return session_->document() && session_->document()->selection ? QString() : tr("No selection");
        }));
    };
    // Photoshop's four: the whole layer shown or hidden, or the selection shown (white inside it) or hidden.
    m.nameAction("mask.add", maskItem(mask, "layer.mask.revealAll", tr("Reveal All"), "layers.mask", {{"action", "add"}, {"mode", "revealAll"}}));
    maskItem(mask, "layer.mask.hideAll", tr("Hide All"), "layers.mask", {{"action", "add"}, {"mode", "hideAll"}});
    maskItem(mask, "layer.mask.fromSelectionReveal", tr("Reveal Selection"), "edit.selection", {{"action", "add"}, {"mode", "revealSelection"}}, true);
    maskItem(mask, "layer.mask.fromSelectionHide", tr("Hide Selection"), "edit.selection", {{"action", "add"}, {"mode", "hideSelection"}}, true);
    mask->addSeparator();
    m.nameAction("mask.toggle", maskItem(mask, "layer.mask.toggle", tr("Enable / Disable"), "layers.mask", {{"action", "toggle"}}));
    m.nameAction("mask.invert", maskItem(mask, "layer.mask.invert", tr("Invert"), "layers.mask", {{"action", "invert"}}));
    m.nameAction("mask.apply", maskItem(mask, "layer.mask.apply", tr("Apply"), "layers.applyMask", {{"action", "apply"}}));
    m.nameAction("mask.delete", maskItem(mask, "layer.mask.delete", tr("Delete"), "layers.mask", {{"action", "delete"}}));
    // Photoshop's Layer > Vector Mask: a path that cuts the layer, edited with the Pen and Direct Selection. The items
    // run vectorMask.set, vectorMask.target and vectorMask.delete on the active layer.
    QMenu* vectorMask = layer->addMenu(tr("&Vector Mask"));
    auto addVector = [this](const char* kind) -> std::optional<QJsonObject> {
        const Layer* l = session_->activeLayer();
        if (l && nekophoto::hasLayerVectorMask(*l)) { QMessageBox::information(this, tr("Vector Mask"), EditorSession::tr("The layer has a vector mask already.")); return std::nullopt; }
        return QJsonObject{{"mode", kind}};
    };
    auto ownVectorMask = [this]() -> std::optional<QJsonObject> {
        const Layer* l = session_->activeLayer();
        if (!l || !nekophoto::hasLayerVectorMask(*l)) return std::nullopt;
        return QJsonObject{};
    };
    m.add(vectorMask, Spec("layer.vectorMask.revealAll", tr("Reveal All")).document("edit.vector").request("vectorMask.set", [addVector] { return addVector("revealAll"); }, tr("Vector Mask")).when(m.hasLayer))
        ->setObjectName("command.vectorMask.revealAll");
    m.add(vectorMask, Spec("layer.vectorMask.hideAll", tr("Hide All")).document("edit.vector").request("vectorMask.set", [addVector] { return addVector("hideAll"); }, tr("Vector Mask")).when(m.hasLayer));
    m.add(vectorMask, Spec("layer.vectorMask.currentPath", tr("Current Path")).document("edit.vector").request("vectorMask.set", [addVector] { return addVector("currentPath"); }, tr("Vector Mask")).when(m.hasLayer));
    vectorMask->addSeparator();
    m.add(vectorMask, Spec("layer.vectorMask.edit", tr("Edit")).document("edit.vector").request("vectorMask.target", ownVectorMask, tr("Vector Mask")).when(m.hasLayer))
        ->setObjectName("command.vectorMask.edit");
    m.add(vectorMask, Spec("layer.vectorMask.delete", tr("Delete")).document("edit.vector").request("vectorMask.delete", ownVectorMask, tr("Vector Mask")).when(m.hasLayer))
        ->setObjectName("command.vectorMask.delete");
    m.nameAction("layer.clipping", m.add(layer, Spec("layer.clippingMask", tr("Create / Release Cl&ipping Mask"), {QKeySequence("Ctrl+Alt+G")}).document("layers.structure").request("layers.set", [this]() -> std::optional<QJsonObject> {
        const Layer* l = session_->activeLayer();
        if (!l || !session_->canToggleClippingMask(l->id)) return std::nullopt;
        return QJsonObject{{"clipping", !l->maskSourceId.has_value()}};
    }, tr("Clipping Mask"))))->setObjectName("command.layers.clipping");
    layer->addSeparator();
    m.add(layer, Spec("layer.bringForward", tr("Bring Forward"), {QKeySequence("Ctrl+]")}).document("layers.structure").request("layers.reorder", [this]() -> std::optional<QJsonObject> {
        if (!session_->canMoveActiveLayer(1)) return std::nullopt;
        return QJsonObject{{"offset", 1}};
    }, tr("Bring Forward")))->setObjectName("command.layers.forward");
    m.add(layer, Spec("layer.sendBackward", tr("Send Backward"), {QKeySequence("Ctrl+[")}).document("layers.structure").request("layers.reorder", [this]() -> std::optional<QJsonObject> {
        if (!session_->canMoveActiveLayer(-1)) return std::nullopt;
        return QJsonObject{{"offset", -1}};
    }, tr("Send Backward")));
    layer->addSeparator();
    m.add(layer, Spec("layer.flipHorizontal", tr("Flip Layer Horizontal")).document("layers.transform")
                   .request("layers.flip", [] { return QJsonObject{}; }, tr("Flip Layer Horizontal")))->setObjectName("command.layers.flip");
    m.add(layer, Spec("layer.flipVertical", tr("Flip Layer Vertical")).document("layers.transform")
                   .request("layers.flip", [] { return QJsonObject{{"vertical", true}}; }, tr("Flip Layer Vertical")));
    // Photoshop's Edit > Transform > Rotate 90°: the active layer (or folder) turned about its centre as one step
    // (layers.setTransform); during Free Transform, the box being edited turns instead, as its context menu does.
    auto rotate = [this](double by) {
        auto turned = [](double degrees) { degrees = std::fmod(degrees, 360.0); if (degrees > 180) degrees -= 360; if (degrees <= -180) degrees += 360; return degrees; };
        if (const auto& edit = session_->transformEdit()) {
            if (edit->corners) return;   // a distortion has no rotation to turn
            LayerTransform t = edit->draft;
            t.rotation = turned(t.rotation + by);
            session_->previewTransform(t);
            return;
        }
        const Layer* l = session_->activeLayer();
        if (!l || !session_->canTransform()) return;
        double rotation = l->transform.rotation;
        if (l->isGroup) { if (auto box = session_->groupTransformBox()) rotation = box->rotation; else return; }
        runCommand("layers.setTransform", {{"rotation", turned(rotation + by)}}, by > 0 ? tr("Rotate 90° Clockwise") : tr("Rotate 90° Counter Clockwise"));
    };
    m.add(layer, Spec("layer.rotate90Clockwise", tr("Rotate Layer 90° Clockwise")).document("layers.transform").when(m.hasLayer).runs([rotate] { rotate(90); }, "layers.setTransform"));
    m.add(layer, Spec("layer.rotate90CounterClockwise", tr("Rotate Layer 90° Counter Clockwise")).document("layers.transform").when(m.hasLayer).runs([rotate] { rotate(-90); }, "layers.setTransform"));
    QMenu* sampling = layer->addMenu(tr("Resampling"));
    auto samplingItem = [&](const char* id, const QString& label, Sampling s) {
        m.add(sampling, Spec(id, label).document("layers.structure").runs([this, s] { samplingCommand(s); }, "layers.set"));
    };
    samplingItem("layer.resampling.high", tr("High Quality"), Sampling::High);
    samplingItem("layer.resampling.smooth", tr("Smooth"), Sampling::Smooth);
    samplingItem("layer.resampling.nearest", tr("Nearest Neighbour"), Sampling::Nearest);
}

void MainWindow::buildTypeMenu(MenuContext& m) {
    // Photoshop's Type menu: the active text layer's outlines as a path (text.toPath) or a shape (text.toShape).
    QMenu* type = menuBar()->addMenu(tr("&Type"));
    auto fromText = [this](bool shape) -> std::optional<QJsonObject> {
        if (!session_->activeLayerId()) { QMessageBox::information(this, shape ? tr("Convert to Shape") : tr("Create Work Path"), tr("Choose a text layer first.")); return std::nullopt; }
        return QJsonObject{};
    };
    m.nameAction("type.workPath", m.add(type, Spec("type.createWorkPath", tr("Create &Work Path")).document("edit.text")
                                              .request("text.toPath", [fromText] { return fromText(false); }, tr("Create Work Path"))))->setObjectName("command.text.toPath");
    m.nameAction("type.shape", m.add(type, Spec("type.convertToShape", tr("Convert to &Shape")).document("edit.paint")
                                           .request("text.toShape", [fromText] { return fromText(true); }, tr("Convert to Shape"))))->setObjectName("command.text.toShape");
}

} // namespace app
