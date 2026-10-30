// The Select menu (MainWindow::buildMenus, MainWindowMenus.h).
#include "MainWindowMenus.h"
#include "ChannelDialogs.h"
#include "CanvasFrame.h"
#include "PreferencesDialog.h"
#include <QInputDialog>
#include <QMenuBar>
#include <QJsonObject>

using namespace nekophoto;

namespace app {

using namespace menus;

void MainWindow::buildSelectMenu(MenuContext& m) {
    // Everything about the selection in one place, as Photoshop's Select menu: the whole-canvas commands,
    // then Modify, then loading a layer's pixels or mask as the selection.
    QMenu* select = menuBar()->addMenu(tr("&Select"));
    m.nameAction("select.all", m.add(select, Spec("select.all", tr("&All"), {QKeySequence::SelectAll}).document("edit.selection")
                                             .request("selection.all", [] { return QJsonObject{}; }, tr("Select All"))));
    m.nameAction("select.deselect", m.add(select, Spec("select.deselect", tr("&Deselect"), {QKeySequence("Ctrl+D")}).document("edit.selection")
                                                  .request("selection.none", [] { return QJsonObject{}; }, tr("Deselect"))));
    m.nameAction("select.reselect", m.add(select, Spec("select.reselect", tr("&Reselect"), {QKeySequence("Shift+Ctrl+D")}).document("edit.selection").request("selection.reselect", [this]() -> std::optional<QJsonObject> {
        if (!session_->canReselect()) return std::nullopt;
        return QJsonObject{};
    }, tr("Reselect"))));
    m.nameAction("select.inverse", m.add(select, Spec("select.inverse", tr("&Inverse"), {QKeySequence("Ctrl+Shift+I")}).document("edit.selection")
                                                 .request("selection.invert", [] { return QJsonObject{}; }, tr("Inverse"))));
    // Quick Mask: selection.quickMask, on or off.
    m.add(select, Spec("select.quickMask", tr("Edit in &Quick Mask Mode"), {QKeySequence("Q")}).document("edit.selection")
                    .request("selection.quickMask", [] { return QJsonObject{}; }, tr("Quick Mask")))->setObjectName("command.selection.quickMask");
    // Photoshop's Select > Subject, through selection.subject; greyed, saying why, without the click-to-select model.
    select->addSeparator();
    m.nameAction("select.subject", m.add(select, Spec("select.subject", tr("Subject")).document("tool.quickSelect").when([]() -> QString {
        if (!ModelStore::supported()) return MainWindow::tr("Unavailable: this build has no OpenCV");
        if (!ModelStore::promptReady())
            return MainWindow::tr("The click-to-select model isn’t downloaded: choose the Click engine in the Quick Selection tool’s options and download it");
        return {};
    }).runs([this] { selectSubject(); }, "selection.subject")));
    select->addSeparator();
    QMenu* modify = select->addMenu(tr("&Modify"));
    m.add(modify, Spec("select.modify.expand", tr("&Expand…")).document("edit.selection").runs([this] { bool ok; int n = QInputDialog::getInt(this, tr("Expand Selection"), tr("Pixels"), 1, 1, 500, 1, &ok); if (ok) runCommand("selection.grow", {{"amount", n}}, tr("Expand Selection")); }, "selection.grow"));
    m.add(modify, Spec("select.modify.contract", tr("&Contract…")).document("edit.selection").runs([this] { bool ok; int n = QInputDialog::getInt(this, tr("Contract Selection"), tr("Pixels"), 1, 1, 500, 1, &ok); if (ok) runCommand("selection.grow", {{"amount", -n}}, tr("Contract Selection")); }, "selection.grow"));
    m.nameAction("select.feather", m.add(modify, Spec("select.modify.feather", tr("&Feather…"), {QKeySequence("Shift+F6")}).document("edit.selection").runs([this] { bool ok; double r = QInputDialog::getDouble(this, tr("Feather Selection"), tr("Radius (pixels)"), 5, 0.1, 250, 1, &ok); if (ok) runCommand("selection.feather", {{"radius", r}}, tr("Feather Selection")); }, "selection.feather")));
    m.add(modify, Spec("select.modify.smooth", tr("&Smooth…")).document("edit.selection").runs([this] { bool ok; int n = QInputDialog::getInt(this, tr("Smooth Selection"), tr("Sample radius (pixels)"), 3, 1, 100, 1, &ok); if (ok) runCommand("selection.smooth", {{"radius", n}}, tr("Smooth Selection")); }, "selection.smooth"));
    m.add(modify, Spec("select.modify.border", tr("&Border…")).document("edit.selection").runs([this] { bool ok; int n = QInputDialog::getInt(this, tr("Border Selection"), tr("Width (pixels)"), 4, 1, 200, 1, &ok); if (ok) runCommand("selection.border", {{"width", n}}, tr("Border Selection")); }, "selection.border"));
    select->addSeparator();
    QMenu* load = select->addMenu(tr("&Load as Selection"));
    auto loadItem = [&](const char* id, const QString& label, QJsonObject params) {
        m.add(load, Spec(id, label).document("edit.selection").request("selection.fromLayer", [this, params]() -> std::optional<QJsonObject> {
            if (!session_->activeLayerId()) return std::nullopt;
            return params;
        }, tr("Load as Selection")).when(m.hasLayer));
    };
    loadItem("select.load.layerPixels", tr("Layer Pixels"), {});
    loadItem("select.load.layerMask", tr("Layer Mask"), {{"mask", true}});
    load->addSeparator();
    loadItem("select.load.addLayerPixels", tr("Add Layer Pixels"), {{"mode", "add"}});
    loadItem("select.load.subtractLayerPixels", tr("Subtract Layer Pixels"), {{"mode", "subtract"}});
    loadItem("select.load.intersectLayerPixels", tr("Intersect with Layer Pixels"), {{"mode", "intersect"}});
    // Selections kept as alpha channels (the Channels panel; docs/channels.md): the dialogs' OK runs
    // channels.loadSelection and channels.saveSelection.
    m.add(select, Spec("select.loadSelection", tr("Load Selection…")).document("edit.channels").runs([this] { (new LoadSelectionDialog(session_, this))->open(); }, "channels.loadSelection"));
    m.nameAction("select.save", m.add(select, Spec("select.saveSelection", tr("Save Selection…")).document("edit.channels").runs([this] {
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
            m.add(nullptr, Spec(id, loadKey ? tr("Load Channel %1 as Selection").arg(n - 1) : tr("Select Channel %1").arg(n - 1),
                              {QKeySequence(QString::fromLatin1(loadKey ? "Ctrl+Alt+%1" : "Ctrl+%1").arg(n))})
                             .document("edit.channels")
                             .request(loadKey ? "channels.loadSelection" : "channels.select", [channelOfKey, n]() -> std::optional<QJsonObject> {
                                 const auto channel = channelOfKey(n);
                                 if (!channel) return std::nullopt;
                                 return QJsonObject{{"channel", *channel}};
                             }));
        }
}

} // namespace app
