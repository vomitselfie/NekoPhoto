// Edit > Keyboard Shortcuts… (Photoshop's Alt+Shift+Ctrl+K): the keys of every command in the registry, the tools and
// the tool switches included, changed by the person. The registry keeps the defaults (Photoshop's); the changes are
// kept in the settings as `shortcuts/<command id>` = a list of portable key strings (an empty list: no key), applied
// at startup and when the dialog's OK is pressed. No key is ever the key of two actions.
#pragma once
#include <QDialog>
#include <QKeySequence>
#include <QLineEdit>
#include <QList>
#include <QString>
#include <map>
#include <optional>

class QLabel;
class QPushButton;
class QSettings;
class QTreeWidget;
class QTreeWidgetItem;
class QVBoxLayout;

namespace app {

class CommandRegistry;
struct Command;

namespace shortcuts {

/// Each command's keys by its id.
using KeyMap = std::map<QString, QList<QKeySequence>>;

/// A key as the settings and the .nekokeys file write it ("Ctrl+Shift+N").
QString portable(const QKeySequence& key);
/// The keys a command has by default, without the ones the platform leaves empty (Save As on some Qt versions).
QList<QKeySequence> defaults(const Command& command);
/// Why `key` cannot be a shortcut (a modifier alone, Esc, Enter, Tab, Space, the keys the canvas uses itself), or
/// empty when it can.
QString refusal(const QKeySequence& key);
/// Every command's keys: `overrides` where they say, else the defaults, with no key twice (a changed key wins over a
/// default one, an earlier command over a later one) and none that `refusal` refuses.
KeyMap resolve(const CommandRegistry& registry, const KeyMap& overrides);
/// The keys the actions have now.
KeyMap current(const CommandRegistry& registry);
/// Where `keys` differs from the defaults: what is saved.
KeyMap overridesOf(const CommandRegistry& registry, const KeyMap& keys);
/// Gives every command's action its keys from `keys`.
void apply(const CommandRegistry& registry, const KeyMap& keys);
/// The saved changes (`shortcuts/<id>`), and saving them (replacing what was saved).
KeyMap load(QSettings& settings);
void save(QSettings& settings, const KeyMap& overrides);
/// The .nekokeys file: {"format": "nekophoto-keys", "version": 1, "shortcuts": {"<id>": ["Ctrl+J"], ...}}.
QByteArray toJson(const KeyMap& overrides);
std::optional<KeyMap> fromJson(const QByteArray& json, QString* error);

} // namespace shortcuts

/// A field that takes the next key combination pressed (with its modifiers) instead of text, window shortcuts
/// included: clicking it and pressing Ctrl+J gives Ctrl+J.
class KeyCaptureEdit : public QLineEdit {
    Q_OBJECT
public:
    explicit KeyCaptureEdit(const QKeySequence& key, QWidget* parent = nullptr);
    QKeySequence key() const { return key_; }
    void setKey(const QKeySequence& key);
signals:
    void captured(QKeySequence key);
protected:
    bool event(QEvent* e) override;
    void focusInEvent(QFocusEvent* e) override;
    void focusOutEvent(QFocusEvent* e) override;
private:
    QKeySequence key_;
    bool keyPressed_ = false;   // a key other than a modifier, since the field took the focus
};

class KeyboardShortcutsDialog : public QDialog {
    Q_OBJECT
public:
    /// Starts from the keys the registry's actions have now; nothing changes until OK.
    KeyboardShortcutsDialog(const CommandRegistry& registry, QWidget* parent = nullptr);

    /// The keys as the dialog has them, and where they differ from the defaults.
    const shortcuts::KeyMap& keys() const { return keys_; }
    shortcuts::KeyMap overrides() const;

    /// Gives command `id` key `key` in place of its key at `slot` (past the end: as another key). False when the key
    /// is refused (message() says why) or is another command's (conflict() names it; takeConflicting() moves it,
    /// cancelConflict() drops the change).
    bool assign(const QString& id, int slot, const QKeySequence& key);
    QString conflict() const { return conflictWith_; }
    void takeConflicting();
    void cancelConflict();
    QString message() const;
    void removeKey(const QString& id, int slot);
    /// "Use Default" for one command (its default keys, taken from whichever commands have them now), or for all.
    void resetCommand(const QString& id);
    void resetAll();
    /// Selects command `id` in the tree, for its keys to be edited below.
    void select(const QString& id);
    /// The search field.
    void setFilter(const QString& text);
    /// Import… and Export… without their file dialogs.
    bool importFile(const QString& path, QString* error);
    bool exportFile(const QString& path, QString* error) const;

private:
    struct Pending { QString id; int slot; QKeySequence key; QString owner; };
    void build();
    void refreshTree();
    void refreshEditor();
    void applyFilter();
    void setKeys(const QString& id, QList<QKeySequence> keys);
    QString commandName(const QString& id) const;
    QString ownerOf(const QKeySequence& key, const QString& except) const;
    void showMessage(const QString& text, bool warning);

    const CommandRegistry& registry_;
    shortcuts::KeyMap keys_;
    std::map<QString, QTreeWidgetItem*> items_;
    QString selected_;
    std::optional<Pending> pending_;
    QString conflictWith_;
    QLineEdit* search_ = nullptr;
    QTreeWidget* tree_ = nullptr;
    QLabel* heading_ = nullptr;
    QVBoxLayout* keyRows_ = nullptr;
    QWidget* keyBox_ = nullptr;
    QPushButton* addKey_ = nullptr;
    QPushButton* useDefault_ = nullptr;
    QLabel* message_ = nullptr;
    QWidget* conflictBox_ = nullptr;
};

} // namespace app
