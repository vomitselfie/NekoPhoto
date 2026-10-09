// The menu commands' table: see CommandRegistry.h.
#include "CommandRegistry.h"
#include "CommandPalette.h"
#include <QAction>
#include <QMenu>
#include <QWidget>
#include <stdexcept>

namespace app {

namespace {

/// The titles of the menus above `menu`, outermost first (translated, without accelerators).
QStringList menuPathOf(QMenu* menu) {
    QStringList path;
    for (QMenu* m = menu; m; m = qobject_cast<QMenu*>(m->parentWidget())) path.prepend(plainText(m->title()));
    return path;
}

} // namespace

CommandRegistry::CommandRegistry(QWidget* window, Runner runner, Gate gate) : window_(window), runner_(std::move(runner)), gate_(std::move(gate)) {}

QAction* CommandRegistry::adopt(QMenu* menu, QAction* action, Command command) {
    const std::string key = command.id.toStdString();
    if (command.id.isEmpty() || byId_.count(key)) throw std::logic_error("command id missing or used twice: " + key);
    if (menu) menu->addAction(action);
    // The command's keys are its defaults: given, they are the action's; not given, the action's own are.
    if (command.shortcuts.isEmpty()) command.shortcuts = action->shortcuts();
    else action->setShortcuts(command.shortcuts);
    command.action = action;
    command.menuPath = menuPathOf(menu);
    action->setProperty("commandId", command.id);
    auto owned = std::make_unique<Command>(std::move(command));
    byId_[key] = owned.get();
    byAction_[action] = owned.get();
    commands_.push_back(std::move(owned));
    return action;
}

QAction* CommandRegistry::add(QMenu* menu, Command command) {
    auto* action = new QAction(command.label, menu ? static_cast<QObject*>(menu) : static_cast<QObject*>(window_));
    if (!command.shortcuts.isEmpty()) action->setShortcuts(command.shortcuts);
    if (!menu) window_->addAction(action);
    QAction* added = adopt(menu, action, std::move(command));
    const Command* entry = byAction_.at(added);
    QObject::connect(added, &QAction::triggered, window_, [this, entry] { execute(*entry); });
    return added;
}

const Command* CommandRegistry::find(const QString& id) const {
    auto it = byId_.find(id.toStdString());
    return it == byId_.end() ? nullptr : it->second;
}

const Command* CommandRegistry::forAction(const QAction* action) const {
    auto it = byAction_.find(action);
    return it == byAction_.end() ? nullptr : it->second;
}

std::vector<const Command*> CommandRegistry::all() const {
    std::vector<const Command*> out;
    out.reserve(commands_.size());
    for (const auto& c : commands_) out.push_back(c.get());
    return out;
}

QString CommandRegistry::disabledReason(const Command& command) const {
    if (command.needsDocument && gate_) {
        if (const QString why = gate_(command); !why.isEmpty()) return why;
    }
    return command.unavailable ? command.unavailable() : QString();
}

void CommandRegistry::execute(const Command& command) const {
    if (command.params) {
        const std::optional<QJsonObject> params = command.params();
        if (params && runner_) runner_(command.method, *params, command.errorTitle.isEmpty() ? plainText(command.label) : command.errorTitle);
        return;
    }
    if (command.run) command.run();
}

} // namespace app
