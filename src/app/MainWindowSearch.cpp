// Edit > Search… (CommandPalette): what it searches, and Filter > G'MIC opened on a chosen filter.
#include "CommandPalette.h"
#include "GmicDialog.h"
#include "MainWindow.h"
#include <QAction>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QSet>
#include <algorithm>
#include <functional>

namespace app {

std::vector<PaletteEntry> MainWindow::paletteEntries() {
    std::vector<PaletteEntry> out;
    QSet<QAction*> seen;
    // Every command reachable from the menu bar: submenus are walked (a menu that fills itself when shown is asked
    // to first), separators and the submenus' own items skipped.
    std::function<void(QMenu*, const QStringList&, const QStringList&)> walk = [&](QMenu* menu, const QStringList& path, const QStringList& englishPath) {
        emit menu->aboutToShow();
        for (QAction* a : menu->actions()) {
            if (a->isSeparator() || !a->isVisible() || a == searchAction_) continue;
            const QString label = plainText(a->text());
            if (label.isEmpty()) continue;
            if (QMenu* sub = a->menu()) { walk(sub, path + QStringList{label}, englishPath + QStringList{englishText(label)}); continue; }
            if (seen.contains(a)) continue;
            seen.insert(a);
            PaletteEntry e;
            e.label = label;
            e.englishLabel = englishText(label);
            e.path = path.join(QStringLiteral(" > "));
            e.englishPath = englishPath.join(QStringLiteral(" > "));
            e.shortcut = a->shortcut().toString(QKeySequence::NativeText);
            e.id = QStringLiteral("menu:") + e.englishPath + QStringLiteral(" > ") + e.englishLabel;
            e.action = a;
            out.push_back(std::move(e));
        }
    };
    for (QAction* top : menuBar()->actions())
        if (QMenu* menu = top->menu(); menu && top->isVisible()) {
            const QString title = plainText(top->text());
            walk(menu, {title}, {englishText(title)});
        }

    // The tools, with their keys.
    const QString tools = tr("Tools");
    QList<QAction*> toolActions = toolActions_.values();
    if (eraserAction_) toolActions << eraserAction_;
    for (QAction* a : toolActions) {
        PaletteEntry e;
        e.label = plainText(a->text()).section(QStringLiteral(" ("), 0, 0);
        e.englishLabel = englishText(plainText(a->text())).section(QStringLiteral(" ("), 0, 0);
        e.path = tools;
        e.englishPath = QStringLiteral("Tools");
        e.shortcut = a->shortcut().toString(QKeySequence::NativeText);
        e.id = QStringLiteral("tool:") + e.englishLabel;
        e.action = a;
        out.push_back(std::move(e));
    }

    // G'MIC's filters: picking one opens Filter > G'MIC on it.
    const bool gmicEnabled = gmicAction_ && gmicAction_->isEnabled();
    for (const auto& [name, folder] : GmicDialog::listedFilters()) {
        PaletteEntry e;
        e.label = name;
        e.englishLabel = name;
        e.path = QStringLiteral("G'MIC > ") + folder;
        e.englishPath = QStringLiteral("G'MIC > ") + englishText(folder);
        e.id = QStringLiteral("gmic:") + name;
        e.enabled = gmicEnabled;
        e.run = [this, name = name] { openGmic(name); };
        out.push_back(std::move(e));
    }
    return out;
}

CommandPalette* MainWindow::showCommandPalette(const QString& query) {
    auto* palette = new CommandPalette(paletteEntries(), this);
    const int width = std::min(680, std::max(360, this->width() - 80));
    palette->resize(width, std::min(460, std::max(240, height() - 120)));
    palette->move(mapToGlobal(QPoint((this->width() - width) / 2, 56)));
    palette->setQuery(query);
    palette->show();
    palette->activateWindow();
    if (auto* field = palette->findChild<QLineEdit*>(QStringLiteral("commandPaletteField"))) field->setFocus(Qt::PopupFocusReason);
    return palette;
}

void MainWindow::openGmic(const QString& filter) {
    if (session_->smartObjectBlocksPixels(true)) return;
    if (!session_->canAdjustPixels()) { showError(tr("G'MIC"), tr("Select a layer with pixels first.")); return; }
    auto* dialog = new GmicDialog(session_, this);
    if (!filter.isEmpty()) dialog->showFilter(filter);
    dialog->show();
}

} // namespace app
