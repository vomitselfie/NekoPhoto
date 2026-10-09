// The menu commands as one table (CONTRIBUTING.md, "Commands"): each has a stable id ("edit.undo", "file.saveAs",
// "filter.blur.gaussian"), its label, the menus above it, its default keys (Photoshop's), what it needs to run and why
// it cannot now, and how it runs: an automation method with the request it sends, or a function that opens its dialog
// (which commits through the command path). The window builds its menus by adding the commands to them in order;
// Edit > Search shows each one's id and the reason a greyed one gives.
#pragma once
#include <QJsonObject>
#include <QKeySequence>
#include <QList>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

class QAction;
class QMenu;
class QWidget;

namespace app {

struct Command {
    /// Stable across languages and releases: area, then the menu's own words ("layer.mask.revealAll").
    QString id;
    /// As the menu shows it, translated, with its accelerator ("&Undo").
    QString label;
    /// The default keys (Photoshop's), the first one shown in the menu. The action has them unless the person changed
    /// them in Edit > Keyboard Shortcuts (KeyboardShortcuts.h), so what a key runs now is the action's shortcuts().
    QList<QKeySequence> shortcuts;
    /// The automation method the command runs. With `params` the command is that request (nullopt from `params`: there
    /// is nothing to do now); otherwise `run` does it (a dialog, or a choice the menu makes first) and `method` names
    /// what it ends in, for Search and the docs. Empty for what only changes the interface.
    QString method;
    std::function<std::optional<QJsonObject>()> params;
    std::function<void()> run;
    /// The title of an error the request gives (default: the label).
    QString errorTitle;
    /// Whether it needs a document, and the supports() feature it is (compositor/supports.h): the depth and colour
    /// mode then gate it. A document command without a feature is 8-bit RGB only.
    bool needsDocument = false;
    QString feature;
    /// Why it cannot run now (empty when it can), asked after the document and its mode.
    std::function<QString()> unavailable;
    /// Filled in by the registry: the action, and the menus' titles above it (translated, without accelerators).
    QPointer<QAction> action;
    QStringList menuPath;
};

class CommandRegistry {
public:
    /// `runner` sends a command's request (MainWindow::runCommand); `gate` says why a command that needs a document
    /// cannot run in the one on screen (none open, its depth, its colour mode), or nothing.
    using Runner = std::function<void(const QString& method, const QJsonObject& params, const QString& title)>;
    using Gate = std::function<QString(const Command&)>;
    CommandRegistry(QWidget* window, Runner runner, Gate gate);

    /// Adds `command` and makes its action: at the end of `menu`, or with no menu on the window alone (a key with no
    /// menu item, such as Ctrl+3). The id must be new.
    QAction* add(QMenu* menu, Command command);
    /// Registers an action made elsewhere (a panel's show/hide, a view switch, a submenu, a tool) as `command`, adding
    /// it to `menu` when given. Its own signals do the work; `run` and `params` are not used. The command's keys are
    /// given to the action; without any, the action's own keys are the command's defaults.
    QAction* adopt(QMenu* menu, QAction* action, Command command);

    const Command* find(const QString& id) const;
    const Command* forAction(const QAction* action) const;
    /// Every command in the order they were added.
    std::vector<const Command*> all() const;
    /// Why `command` cannot run now: no document, the depth or mode, or its own reason; empty when it can.
    QString disabledReason(const Command& command) const;
    /// Runs `command` as its menu item does.
    void execute(const Command& command) const;

private:
    QWidget* window_;
    Runner runner_;
    Gate gate_;
    std::vector<std::unique_ptr<Command>> commands_;
    std::unordered_map<std::string, Command*> byId_;
    std::unordered_map<const QAction*, Command*> byAction_;
};

} // namespace app
