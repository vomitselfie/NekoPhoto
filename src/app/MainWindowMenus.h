// What the menu builders share (MainWindowMenus.cpp and Menu<Name>.cpp, one per menu of the menu bar): a command
// written as a row (Spec), the key watcher the tool keys and the field-guarded keys use, and the MenuContext each
// builder is given.
#pragma once
#include "CommandRegistry.h"
#include "MainWindow.h"
#include <QAbstractSpinBox>
#include <QAction>
#include <QApplication>
#include <QKeySequence>
#include <QMenu>
#include <QObject>
#include <QRegularExpression>
#include <QShortcutEvent>
#include <QString>
#include <QWidget>
#include <functional>
#include <optional>

namespace app {

namespace menus {

/// Sees an action's shortcut before the action does, and runs `f` in its place (the action and the keys pressed).
class ShortcutWatch : public QObject {
public:
    ShortcutWatch(QObject* parent, std::function<void(QAction*, const QKeySequence&)> f) : QObject(parent), f_(std::move(f)) {}
protected:
    bool eventFilter(QObject* watched, QEvent* e) override {
        if (e->type() != QEvent::Shortcut) return false;
        auto* shortcut = static_cast<QShortcutEvent*>(e);
        auto* action = qobject_cast<QAction*>(watched);
        if (!action || shortcut->isAmbiguous() || !action->isEnabled()) return false;
        f_(action, shortcut->key());
        return true;
    }
private:
    std::function<void(QAction*, const QKeySequence&)> f_;
};

/// Whether the keyboard is in a text field (a name, a number), where window shortcuts that edit the document or
/// rearrange the window should leave the keys alone.
inline bool typingInField() {
    QWidget* focus = QApplication::focusWidget();
    return focus && (focus->inherits("QLineEdit") || focus->inherits("QAbstractSpinBox") || focus->inherits("QTextEdit") || focus->inherits("QPlainTextEdit"));
}

/// A command for the registry, written as a row: its id, label and keys, then what it needs and how it runs.
struct Spec {
    Command command;
    Spec(const QString& id, const QString& label, QList<QKeySequence> keys = {}) {
        command.id = id;
        command.label = label;
        command.shortcuts = std::move(keys);
    }
    Spec(const char* id, const QString& label, QList<QKeySequence> keys = {}) : Spec(QString::fromLatin1(id), label, std::move(keys)) {}
    /// It needs a document; `feature` is the supports() feature it is (none: 8-bit RGB only).
    Spec& document(const char* feature = nullptr) {
        command.needsDocument = true;
        if (feature) command.feature = QString::fromUtf8(feature);
        return *this;
    }
    /// It runs `run` (a dialog, or a choice made first), which ends in automation method `method` when given.
    Spec& runs(std::function<void()> run, const char* method = nullptr) {
        command.run = std::move(run);
        if (method) command.method = QString::fromLatin1(method);
        return *this;
    }
    /// It is the request `method` with what `params` gives (nothing: nothing to do now); an error is titled `title`.
    Spec& request(const char* method, std::function<std::optional<QJsonObject>()> params, const QString& title = {}) {
        command.method = QString::fromLatin1(method);
        command.params = std::move(params);
        command.errorTitle = title;
        return *this;
    }
    /// Why it cannot run now (empty when it can), beyond the document and its mode.
    Spec& when(std::function<QString()> unavailable) {
        command.unavailable = std::move(unavailable);
        return *this;
    }
};

/// A command id's last part from the interface's English words: "Gaussian Blur" is gaussianBlur, "soft-glitch" softGlitch.
inline QString commandSlug(const QString& words) {
    static const QRegularExpression separators(QStringLiteral("[^A-Za-z0-9]+"));
    QString out;
    for (const QString& word : words.split(separators, Qt::SkipEmptyParts))
        out += out.isEmpty() ? word.toLower() : word.left(1).toUpper() + word.mid(1).toLower();
    return out;
}

} // namespace menus

/// Given to each menu's builder: the registry, adding a command to a menu (`add`), naming an action for the canvas's
/// context menu (`nameAction`: it shows those again, so their shortcuts and enabled state match), the guard that lets
/// F7, F12 and Ctrl+Alt+Z leave the keys alone while a text field has them, and the reason of the commands that need an
/// active layer.
struct MainWindow::MenuContext {
    explicit MenuContext(MainWindow& window);
    MainWindow& w;
    CommandRegistry& reg;
    menus::ShortcutWatch* fieldGuard;
    std::function<QString()> hasLayer;
    QAction* add(QMenu* menu, menus::Spec spec) { return reg.add(menu, std::move(spec.command)); }
    QAction* nameAction(const char* key, QAction* a) { w.named_[QString::fromLatin1(key)] = a; return a; }
};

} // namespace app
