// The Edit menu: history, the clipboard, transforms and fills, colour management, Search, Keyboard Shortcuts
// and Preferences (MainWindow::buildMenus, MainWindowMenus.h).
#include "MainWindowMenus.h"
#include "Theme.h"
#include "ContentFillDialog.h"
#include "ContentAwareScaleDialog.h"
#include "WarpDialog.h"
#include "CanvasFrame.h"
#include "ColorDialogs.h"
#include <QStatusBar>
#include <QMenuBar>
#include <QApplication>
#include <QJsonObject>

using namespace nekophoto;

namespace app {

using namespace menus;

void MainWindow::buildEditMenu(MenuContext& m) {
    QMenu* edit = menuBar()->addMenu(tr("&Edit"));
    // Undo and Redo run history.undo and history.redo (history steps are not recorded in actions).
    undoAction_ = m.add(edit, Spec("edit.undo", tr("&Undo"), {QKeySequence::Undo}).request("history.undo", [] { return QJsonObject{}; }, tr("Undo"))
                                .when([this] { return session_->canUndo() ? QString() : tr("Nothing to undo"); }));
    undoAction_->setObjectName("command.history.undo");
    redoAction_ = m.add(edit, Spec("edit.redo", tr("&Redo"), {QKeySequence("Ctrl+Shift+Z")}).request("history.redo", [] { return QJsonObject{}; }, tr("Redo"))
                                .when([this] { return session_->canRedo() ? QString() : tr("Nothing to redo"); }));
    redoAction_->setObjectName("command.history.redo");
    // Photoshop CC's Toggle Last State: undoes the last step, or redoes the one just undone.
    toggleStateAction_ = m.add(edit, Spec("edit.toggleLastState", tr("Toggle &Last State"), {QKeySequence("Ctrl+Alt+Z")}).runs([this] { session_->toggleLastState(); })
                                       .when([this] { return session_->canUndo() || session_->canRedo() ? QString() : tr("Nothing to undo or redo"); }));
    toggleStateAction_->setObjectName("edit.toggleLastState");
    toggleStateAction_->installEventFilter(m.fieldGuard);
    edit->addSeparator();
    // The pixel clipboard runs pixels.cut, pixels.copy, pixels.copyMerged and pixels.paste (layers.copy and
    // layers.paste for whole layers); with nothing to copy or paste the items do nothing, as before.
    m.nameAction("edit.cut", m.add(edit, Spec("edit.cut", tr("Cu&t"), {QKeySequence::Cut}).document("edit.clipboard").runs([this] {
        if (!session_->document()->selection || !session_->canCopyPixels()) return;
        // A smart object: copied, then the question about rasterizing it (pixels.cut refuses one).
        if (session_->smartObjectBlocksPixels()) { session_->cutSelection(); return; }
        runCommand("pixels.cut", {}, tr("Cut"));
    }, "pixels.cut")))->setObjectName("command.pixels.cut");
    m.nameAction("edit.copy", m.add(edit, Spec("edit.copy", tr("&Copy"), {QKeySequence::Copy}).document("edit.clipboard").runs([this] {
        // As Photoshop: with no selection, the selected layers themselves (pasted whole in any document).
        if (!session_->document()->selection && !session_->selectedLayerIds().empty()) { runCommand("layers.copy", {}, tr("Copy")); return; }
        if (session_->canCopyPixels()) runCommand("pixels.copy", {}, tr("Copy"));
    }, "pixels.copy")))->setObjectName("command.pixels.copy");
    m.add(edit, Spec("edit.copyMerged", tr("Copy &Merged"), {QKeySequence("Ctrl+Shift+C")}).document("edit.clipboard").runs([this] {
        if (session_->canEditLayers() && !(session_->document()->selection && session_->document()->selection->isEmpty())) runCommand("pixels.copyMerged", {}, tr("Copy Merged"));
    }, "pixels.copyMerged"))->setObjectName("command.pixels.copyMerged");
    m.nameAction("edit.paste", m.add(edit, Spec("edit.paste", tr("&Paste"), {QKeySequence::Paste}).document("edit.clipboard").runs([this] {
        if (EditorSession::hasLayerClipboard()) { if (session_->canPaste()) runCommand("layers.paste", {}, tr("Paste")); return; }
        if (session_->hasPixelsToPaste()) runCommand("pixels.paste", {}, tr("Paste"));
    }, "pixels.paste")))->setObjectName("command.pixels.paste");
    edit->addSeparator();
    // Free Transform is interactive: Ctrl+T invokes it, the canvas updates it, and Enter or Apply commits it through
    // the command layers.setTransform (EditorSession::commitTransformCommand).
    m.nameAction("edit.freeTransform", m.add(edit, Spec("edit.freeTransform", tr("&Free Transform"), {QKeySequence("Ctrl+T")}).document("layers.transform")
                                                   .runs([this] { session_->transformCommand(); }, "layers.setTransform")))->setObjectName("command.transform");
    // Warp's OK runs layers.warp; the Warp Cage's Enter runs layers.setCage (EditorSession::commitWarpCageCommand).
    m.nameAction("edit.warp", m.add(edit, Spec("edit.warp", tr("&Warp…")).document("edit.distort").runs([this] { WarpDialog(session_, this).exec(); }, "layers.warp").when(m.hasLayer)));
    m.add(edit, Spec("edit.warpCage", tr("Warp Ca&ge")).document("edit.distort").runs([this] {
        QString error;
        if (!session_->beginWarpCage(&error)) showError(tr("Warp Cage"), error);
        else statusBar()->showMessage(tr("Drag the cage's points; Enter applies, Esc cancels."), 8000);
    }, "layers.setCage"));
    m.nameAction("edit.fillForeground", m.add(edit, Spec("edit.fillForeground", tr("Fill with Foreground"), {QKeySequence("Alt+Backspace")}).document("edit.fill")
                                                    .runs([this] { fillWith(session_->foregroundColor); }, "pixels.fill")))->setObjectName("command.pixels.fill");
    m.nameAction("edit.fillBackground", m.add(edit, Spec("edit.fillBackground", tr("Fill with Background"), {QKeySequence("Ctrl+Backspace")}).document("edit.fill")
                                                    .runs([this] { fillWith(session_->backgroundColor); }, "pixels.fill")));
    QAction* clear = m.add(edit, Spec("edit.clear", tr("Clear"), {QKeySequence(Qt::Key_Delete), QKeySequence(Qt::Key_Backspace)}).document("layers.structure").runs([this] {
        if (session_->document() && session_->document()->selection) runCommand("pixels.clear", {}, tr("Clear"));
        else deleteLayersCommand();
    }, "pixels.clear"));
    clear->setObjectName("command.pixels.clear");
    m.nameAction("edit.contentAwareFill", m.add(edit, Spec("edit.contentAwareFill", tr("Content-Aware Fill…"), {QKeySequence("Shift+F5")}).document("edit.fill").runs([this] {
        if (!session_->canAdjustPixels() || !session_->document()->selection || !session_->document()->selection->coverage) { showError(tr("Content-Aware Fill"), tr("Select a visible image layer and an area to fill.")); return; }
        (new ContentFillDialog(session_, this))->show();
    }, "pixels.contentAwareFill")));
    m.add(edit, Spec("edit.contentAwareScale", tr("Content-Aware Scale…"), {QKeySequence("Ctrl+Alt+Shift+C")}).document("edit.contentAware")
                  .runs([this] { (new ContentAwareScaleDialog(session_, this))->show(); }, "pixels.contentAwareScale"));

    // Colour management (docs/color-management.md), as in Photoshop's Edit menu.
    edit->addSeparator();
    m.add(edit, Spec("edit.colorSettings", tr("Color Settings…"), {QKeySequence("Ctrl+Shift+K")}).runs([this] { color::showColorSettings(this); }, "color.settings"));
    auto profileKey = [](const nekophoto::ColorProfile& p) -> QString {
        if (p.empty()) return QStringLiteral("none");
        auto s = nekophoto::matchingWorkingSpace(p);
        return s && p == nekophoto::builtinProfile(*s) ? QString::fromLatin1(nekophoto::workingSpaceKey(*s)) : QString();
    };
    m.add(edit, Spec("edit.assignProfile", tr("Assign Profile…")).document("document.profile").runs([this, profileKey] {
        auto profile = color::askAssignProfile(this, session_->document()->profile);
        if (!profile) return;
        // A profile the method names runs as the command; an ICC file chosen from disk is assigned directly (and
        // not recorded: the method takes a key or a path, and the dialog keeps no path).
        if (const QString key = profileKey(*profile); !key.isEmpty()) runCommand("document.profile", {{"action", "assign"}, {"profile", key}}, tr("Assign Profile"));
        else session_->assignProfile(*profile);
    }, "document.profile"));
    m.add(edit, Spec("edit.convertToProfile", tr("Convert to Profile…")).document("document.profile").runs([this, profileKey] {
        auto choice = color::askConvertProfile(this, session_->document()->profile);
        if (!choice) return;
        const QString key = profileKey(choice->profile);
        QApplication::setOverrideCursor(themedCursor(Qt::WaitCursor));
        if (!key.isEmpty()) {
            runCommand("document.profile", {{"action", "convert"}, {"profile", key}, {"intent", QString::fromLatin1(nekophoto::renderingIntentKey(choice->options.intent))},
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
    searchAction_ = m.add(edit, Spec("edit.search", tr("&Search…"), {QKeySequence("Ctrl+F")}).runs([this] { showCommandPalette(); }));
    searchAction_->setObjectName("edit.search");
    // Photoshop's Edit > Keyboard Shortcuts… (Alt+Shift+Ctrl+K): every command's, tool's and panel's keys.
    m.add(edit, Spec("edit.keyboardShortcuts", tr("&Keyboard Shortcuts…"), {QKeySequence("Ctrl+Alt+Shift+K")}).runs([this] { showKeyboardShortcuts(); }))
        ->setObjectName("edit.keyboardShortcuts");
    // Photoshop's Ctrl+K, and the platform's own Preferences key where it has one.
    QList<QKeySequence> preferenceKeys{QKeySequence("Ctrl+K")};
    for (const QKeySequence& key : QKeySequence::keyBindings(QKeySequence::Preferences)) if (!preferenceKeys.contains(key)) preferenceKeys << key;
    m.add(edit, Spec("edit.preferences", tr("Prefere&nces…"), preferenceKeys).runs([this] { showPreferences(); }));
}

} // namespace app
