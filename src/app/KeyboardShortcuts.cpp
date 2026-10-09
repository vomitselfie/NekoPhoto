// Edit > Keyboard Shortcuts…: see KeyboardShortcuts.h.
#include "KeyboardShortcuts.h"
#include "CommandPalette.h"
#include "CommandRegistry.h"
#include <QAction>
#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileDialog>
#include <QFocusEvent>
#include <QFont>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QPushButton>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QSignalBlocker>
#include <algorithm>
#include <functional>
#include <set>

namespace app {

namespace shortcuts {

namespace {

/// Commands that take keys: every one with an action, but the submenus (Image > Mode) themselves.
bool keyable(const Command& c) { return c.action && !c.action->menu(); }

QList<QKeySequence> withoutEmpty(const QList<QKeySequence>& keys) {
    QList<QKeySequence> out;
    for (const QKeySequence& k : keys) if (!k.isEmpty()) out << k;
    return out;
}

bool isModifier(int key) {
    switch (key) {
    case Qt::Key_Shift: case Qt::Key_Control: case Qt::Key_Alt: case Qt::Key_Meta: case Qt::Key_AltGr: case Qt::Key_Super_L:
    case Qt::Key_Super_R: case Qt::Key_Hyper_L: case Qt::Key_Hyper_R: case Qt::Key_CapsLock: case Qt::Key_NumLock:
    case Qt::Key_ScrollLock: case Qt::Key_Mode_switch:
        return true;
    default: return false;
    }
}

} // namespace

QString portable(const QKeySequence& key) { return key.toString(QKeySequence::PortableText); }

QList<QKeySequence> defaults(const Command& command) { return withoutEmpty(command.shortcuts); }

QString refusal(const QKeySequence& key) {
    if (key.isEmpty()) return KeyboardShortcutsDialog::tr("Press a key with its modifiers.");
    if (key.count() != 1) return KeyboardShortcutsDialog::tr("A shortcut is one key with its modifiers, not a sequence of keys.");
    const QKeyCombination combination = key[0];
    const int k = int(combination.key());
    const Qt::KeyboardModifiers m = combination.keyboardModifiers();
    const bool ctrl = m & (Qt::ControlModifier | Qt::MetaModifier), alt = m & Qt::AltModifier;
    if (k == 0 || k == Qt::Key_unknown || isModifier(k)) return KeyboardShortcutsDialog::tr("A modifier alone cannot be a shortcut: hold it with another key.");
    if (k == Qt::Key_Escape) return KeyboardShortcutsDialog::tr("Esc cancels what is in progress (a transform, a crop, typing) and closes dialogs, so it cannot be a shortcut.");
    if (k == Qt::Key_Return || k == Qt::Key_Enter) return KeyboardShortcutsDialog::tr("Enter commits what is in progress (a transform, a crop, a path, typing), so it cannot be a shortcut.");
    if ((k == Qt::Key_Tab || k == Qt::Key_Backtab) && !ctrl) return KeyboardShortcutsDialog::tr("Tab moves between fields; use it with Ctrl.");
    if (k == Qt::Key_Space) return KeyboardShortcutsDialog::tr("Space held pans the view (with Ctrl or Alt, zooms), so it cannot be a shortcut.");
    if ((k == Qt::Key_Left || k == Qt::Key_Right || k == Qt::Key_Up || k == Qt::Key_Down) && !alt)
        return KeyboardShortcutsDialog::tr("The arrow keys nudge the selection, the layer or the selected pixels (with Shift, Ctrl or both); use them with Alt.");
    if ((k == Qt::Key_BracketLeft || k == Qt::Key_BracketRight || k == Qt::Key_BraceLeft || k == Qt::Key_BraceRight) && !ctrl && !alt)
        return KeyboardShortcutsDialog::tr("[ and ] change the brush's size (with Shift, its hardness); use them with Ctrl or Alt.");
    if (k >= Qt::Key_0 && k <= Qt::Key_9 && !ctrl && !alt) return KeyboardShortcutsDialog::tr("The digits set the tool's opacity; use them with Ctrl or Alt.");
    if ((k == Qt::Key_Less || k == Qt::Key_Greater || k == Qt::Key_Comma || k == Qt::Key_Period) && (m & Qt::ControlModifier) && (m & Qt::ShiftModifier))
        return KeyboardShortcutsDialog::tr("Ctrl+Shift+< and > step the type size.");
    return {};
}

KeyMap resolve(const CommandRegistry& registry, const KeyMap& overrides) {
    KeyMap out;
    std::set<QString> taken;
    const auto commands = registry.all();
    // The changed keys first, then the defaults of the commands left as they were.
    for (const Command* c : commands) {
        if (!keyable(*c)) continue;
        auto it = overrides.find(c->id);
        if (it == overrides.end()) continue;
        QList<QKeySequence>& keys = out[c->id];
        for (const QKeySequence& key : it->second)
            if (!key.isEmpty() && refusal(key).isEmpty() && taken.insert(portable(key)).second) keys << key;
    }
    for (const Command* c : commands) {
        if (!keyable(*c) || overrides.count(c->id)) continue;
        QList<QKeySequence>& keys = out[c->id];
        for (const QKeySequence& key : defaults(*c))
            if (taken.insert(portable(key)).second) keys << key;
    }
    return out;
}

KeyMap current(const CommandRegistry& registry) {
    KeyMap out;
    for (const Command* c : registry.all())
        if (keyable(*c)) out[c->id] = withoutEmpty(c->action->shortcuts());
    return out;
}

KeyMap overridesOf(const CommandRegistry& registry, const KeyMap& keys) {
    KeyMap out;
    for (const Command* c : registry.all()) {
        if (!keyable(*c)) continue;
        auto it = keys.find(c->id);
        if (it == keys.end()) continue;
        QStringList now, before;
        for (const QKeySequence& k : withoutEmpty(it->second)) now << portable(k);
        for (const QKeySequence& k : defaults(*c)) before << portable(k);
        if (now != before) out[c->id] = withoutEmpty(it->second);
    }
    return out;
}

void apply(const CommandRegistry& registry, const KeyMap& keys) {
    for (const Command* c : registry.all()) {
        if (!keyable(*c)) continue;
        auto it = keys.find(c->id);
        const QList<QKeySequence> wanted = it == keys.end() ? QList<QKeySequence>{} : it->second;
        if (c->action->shortcuts() != wanted) c->action->setShortcuts(wanted);
    }
}

KeyMap load(QSettings& settings) {
    KeyMap out;
    settings.beginGroup(QStringLiteral("shortcuts"));
    for (const QString& id : settings.childKeys()) {
        QList<QKeySequence> keys;
        // An empty list (no key) reads back as nothing at all, or as an empty string on some back ends.
        for (const QString& text : settings.value(id).toStringList())
            if (const QKeySequence key = QKeySequence::fromString(text, QKeySequence::PortableText); !key.isEmpty()) keys << key;
        out[id] = keys;
    }
    settings.endGroup();
    return out;
}

void save(QSettings& settings, const KeyMap& overrides) {
    settings.remove(QStringLiteral("shortcuts"));
    for (const auto& [id, keys] : overrides) {
        QStringList texts;
        for (const QKeySequence& key : keys) texts << portable(key);
        settings.setValue(QStringLiteral("shortcuts/") + id, texts);
    }
}

QByteArray toJson(const KeyMap& overrides) {
    QJsonObject keys;
    for (const auto& [id, list] : overrides) {
        QJsonArray texts;
        for (const QKeySequence& key : list) texts.append(portable(key));
        keys[id] = texts;
    }
    return QJsonDocument(QJsonObject{{"format", "nekophoto-keys"}, {"version", 1}, {"shortcuts", keys}}).toJson(QJsonDocument::Indented);
}

std::optional<KeyMap> fromJson(const QByteArray& json, QString* error) {
    QJsonParseError parse;
    const QJsonDocument doc = QJsonDocument::fromJson(json, &parse);
    const QJsonObject root = doc.object();
    if (parse.error != QJsonParseError::NoError || root.value("format").toString() != QLatin1String("nekophoto-keys") || !root.value("shortcuts").isObject()) {
        if (error) *error = KeyboardShortcutsDialog::tr("This is not a NekoPhoto keyboard shortcuts file.");
        return std::nullopt;
    }
    if (root.value("version").toInt() > 1) {
        if (error) *error = KeyboardShortcutsDialog::tr("This keyboard shortcuts file is from a newer version of NekoPhoto.");
        return std::nullopt;
    }
    KeyMap out;
    const QJsonObject keys = root.value("shortcuts").toObject();
    for (auto it = keys.begin(); it != keys.end(); ++it) {
        QList<QKeySequence> list;
        for (const QJsonValue& v : it.value().toArray())
            if (const QKeySequence key = QKeySequence::fromString(v.toString(), QKeySequence::PortableText); !key.isEmpty()) list << key;
        out[it.key()] = list;
    }
    return out;
}

} // namespace shortcuts

// ---- The key field ----

KeyCaptureEdit::KeyCaptureEdit(const QKeySequence& key, QWidget* parent) : QLineEdit(parent) {
    setReadOnly(true);
    setPlaceholderText(tr("Press a key"));
    setKey(key);
}

void KeyCaptureEdit::setKey(const QKeySequence& key) {
    key_ = key;
    setText(key.toString(QKeySequence::NativeText));
}

bool KeyCaptureEdit::event(QEvent* e) {
    // Every key is the shortcut being set: none runs a window shortcut, moves the focus or closes the dialog.
    if (e->type() == QEvent::ShortcutOverride) { e->accept(); return true; }
    if (e->type() == QEvent::KeyPress || e->type() == QEvent::KeyRelease) {
        auto* k = static_cast<QKeyEvent*>(e);
        int key = k->key();
        Qt::KeyboardModifiers mods = k->modifiers() & (Qt::ShiftModifier | Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier | Qt::KeypadModifier);
        const bool modifierKey = key == Qt::Key_Shift || key == Qt::Key_Control || key == Qt::Key_Alt || key == Qt::Key_Meta || key == Qt::Key_AltGr;
        if (e->type() == QEvent::KeyRelease) {
            // A modifier pressed and let go alone: said to be refused, as a key would be.
            if (modifierKey && !k->isAutoRepeat() && !keyPressed_ && hasFocus())
                emit captured(QKeySequence(QKeyCombination(Qt::NoModifier, Qt::Key(key))));
            return true;
        }
        if (modifierKey) {
            keyPressed_ = false;
            setText(QKeySequence(QKeyCombination(mods, Qt::Key(0))).toString(QKeySequence::NativeText) + QStringLiteral("…"));
            return true;
        }
        keyPressed_ = true;
        if (key == 0 || key == Qt::Key_unknown) return true;
        if (key == Qt::Key_Backtab) { key = Qt::Key_Tab; mods |= Qt::ShiftModifier; }
        emit captured(QKeySequence(QKeyCombination(mods, Qt::Key(key))));
        return true;
    }
    return QLineEdit::event(e);
}

void KeyCaptureEdit::focusInEvent(QFocusEvent* e) {
    QLineEdit::focusInEvent(e);
    keyPressed_ = false;
    clear();   // the placeholder asks for the key
}

void KeyCaptureEdit::focusOutEvent(QFocusEvent* e) {
    QLineEdit::focusOutEvent(e);
    setKey(key_);
}

// ---- The dialog ----

namespace {

QString keysText(const QList<QKeySequence>& keys) {
    QStringList texts;
    for (const QKeySequence& k : keys) texts << k.toString(QKeySequence::NativeText);
    return texts.join(QStringLiteral(", "));
}

} // namespace

KeyboardShortcutsDialog::KeyboardShortcutsDialog(const CommandRegistry& registry, QWidget* parent)
    : QDialog(parent), registry_(registry), keys_(shortcuts::current(registry)) {
    setWindowTitle(tr("Keyboard Shortcuts"));
    build();
    refreshTree();
    refreshEditor();
}

void KeyboardShortcutsDialog::build() {
    auto* layout = new QVBoxLayout(this);
    auto* intro = new QLabel(tr("Click a command, then click its shortcut and press the keys. Shortcuts for the tools are "
                                "single letters; menu commands usually use Ctrl."));
    intro->setWordWrap(true);
    layout->addWidget(intro);
    search_ = new QLineEdit;
    search_->setPlaceholderText(tr("Search commands and keys"));
    search_->setClearButtonEnabled(true);
    search_->setObjectName("shortcutSearch");
    connect(search_, &QLineEdit::textChanged, this, [this] { applyFilter(); });
    layout->addWidget(search_);

    tree_ = new QTreeWidget;
    tree_->setObjectName("shortcutTree");
    tree_->setColumnCount(2);
    tree_->setHeaderLabels({tr("Command"), tr("Shortcut")});
    tree_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    tree_->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    tree_->header()->setStretchLastSection(false);
    tree_->setUniformRowHeights(true);
    tree_->setMinimumHeight(220);
    connect(tree_, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem* item) {
        const QString id = item ? item->data(0, Qt::UserRole).toString() : QString();
        if (id == selected_) return;
        cancelConflict();
        selected_ = id;
        message_->clear();
        refreshEditor();
    });
    // A double-click (or Enter) on a command starts on its first key.
    connect(tree_, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem* item) {
        if (item && !item->data(0, Qt::UserRole).toString().isEmpty())
            if (auto* first = keyBox_->findChild<KeyCaptureEdit*>()) first->setFocus();
    });
    layout->addWidget(tree_, 1);

    heading_ = new QLabel;
    heading_->setWordWrap(true);
    QFont bold = heading_->font();
    bold.setBold(true);
    heading_->setFont(bold);
    layout->addWidget(heading_);
    keyBox_ = new QWidget;
    keyRows_ = new QVBoxLayout(keyBox_);
    keyRows_->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(keyBox_);
    auto* commandButtons = new QHBoxLayout;
    addKey_ = new QPushButton(tr("Add Shortcut"));
    addKey_->setAutoDefault(false);
    connect(addKey_, &QPushButton::clicked, this, [this] {
        if (selected_.isEmpty()) return;
        // An empty field for the next key; nothing is kept until a key is pressed in it.
        auto* row = new QWidget;
        auto* h = new QHBoxLayout(row);
        h->setContentsMargins(0, 0, 0, 0);
        auto* edit = new KeyCaptureEdit(QKeySequence());
        const int slot = int(keys_[selected_].size());
        connect(edit, &KeyCaptureEdit::captured, this, [this, slot](const QKeySequence& key) { assign(selected_, slot, key); });
        h->addWidget(edit, 1);
        keyRows_->addWidget(row);
        edit->setFocus();
    });
    useDefault_ = new QPushButton(tr("Use Default"));
    useDefault_->setAutoDefault(false);
    useDefault_->setToolTip(tr("This command's default keys, taken from the commands that have them now"));
    connect(useDefault_, &QPushButton::clicked, this, [this] { if (!selected_.isEmpty()) resetCommand(selected_); });
    commandButtons->addWidget(addKey_);
    commandButtons->addWidget(useDefault_);
    commandButtons->addStretch(1);
    layout->addLayout(commandButtons);
    message_ = new QLabel;
    message_->setWordWrap(true);
    message_->setObjectName("shortcutMessage");
    layout->addWidget(message_);
    conflictBox_ = new QWidget;
    auto* conflictRow = new QHBoxLayout(conflictBox_);
    conflictRow->setContentsMargins(0, 0, 0, 0);
    auto* take = new QPushButton(tr("Take It"));
    take->setAutoDefault(false);
    take->setToolTip(tr("Move the key to this command; the other command loses it"));
    connect(take, &QPushButton::clicked, this, [this] { takeConflicting(); });
    auto* keep = new QPushButton(tr("Cancel Change"));
    keep->setAutoDefault(false);
    connect(keep, &QPushButton::clicked, this, [this] { cancelConflict(); message_->clear(); });
    conflictRow->addStretch(1);
    conflictRow->addWidget(take);
    conflictRow->addWidget(keep);
    conflictBox_->hide();
    layout->addWidget(conflictBox_);

    auto* bottom = new QHBoxLayout;
    auto* all = new QPushButton(tr("Use Default for All"));
    all->setAutoDefault(false);
    connect(all, &QPushButton::clicked, this, [this] { resetAll(); });
    auto* importButton = new QPushButton(tr("Import…"));
    importButton->setAutoDefault(false);
    connect(importButton, &QPushButton::clicked, this, [this] {
        const QString path = QFileDialog::getOpenFileName(this, tr("Import Keyboard Shortcuts"), QSettings().value("lastDir").toString(), tr("Keyboard shortcuts (*.nekokeys)"));
        if (path.isEmpty()) return;
        QString error;
        if (!importFile(path, &error)) showMessage(error, true);
    });
    auto* exportButton = new QPushButton(tr("Export…"));
    exportButton->setAutoDefault(false);
    connect(exportButton, &QPushButton::clicked, this, [this] {
        const QString start = QSettings().value("lastDir", QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)).toString();
        QString path = QFileDialog::getSaveFileName(this, tr("Export Keyboard Shortcuts"), start + QStringLiteral("/shortcuts.nekokeys"), tr("Keyboard shortcuts (*.nekokeys)"));
        if (path.isEmpty()) return;
        if (!path.endsWith(QStringLiteral(".nekokeys"))) path += QStringLiteral(".nekokeys");
        QString error;
        if (!exportFile(path, &error)) showMessage(error, true);
        else showMessage(tr("Exported the changed shortcuts to %1.").arg(path), false);
    });
    bottom->addWidget(all);
    bottom->addWidget(importButton);
    bottom->addWidget(exportButton);
    bottom->addStretch(1);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    bottom->addWidget(buttons);
    layout->addLayout(bottom);
    resize(760, 720);
}

void KeyboardShortcutsDialog::refreshTree() {
    // The menus as the menu bar has them, then the tools, then the keys without a menu item.
    const QString current = selected_;
    tree_->clear();
    items_.clear();
    std::map<QString, QTreeWidgetItem*> groups;
    auto groupFor = [&](const QStringList& path) {
        QTreeWidgetItem* parent = nullptr;
        QString key;
        for (const QString& part : path) {
            key += QStringLiteral("\x1f") + part;
            auto it = groups.find(key);
            if (it == groups.end()) {
                auto* item = parent ? new QTreeWidgetItem(parent, {part}) : new QTreeWidgetItem(tree_, {part});
                item->setFlags(Qt::ItemIsEnabled);
                it = groups.emplace(key, item).first;
            }
            parent = it->second;
        }
        return parent;
    };
    const QString tools = tr("Tools"), others = tr("Other Keys");
    // Groups in order: the menus first (as added), the tools and the others after them.
    std::vector<const Command*> menus, rest;
    for (const Command* c : registry_.all()) {
        if (!keys_.count(c->id)) continue;
        (c->menuPath.isEmpty() ? rest : menus).push_back(c);
    }
    std::stable_partition(rest.begin(), rest.end(), [](const Command* c) { return c->id.startsWith(QStringLiteral("tool.")); });
    for (const auto* list : {&menus, &rest})
        for (const Command* c : *list) {
            const QStringList path = !c->menuPath.isEmpty() ? c->menuPath : QStringList{c->id.startsWith(QStringLiteral("tool.")) ? tools : others};
            auto* item = new QTreeWidgetItem(groupFor(path), {plainText(c->label), keysText(keys_.at(c->id))});
            item->setData(0, Qt::UserRole, c->id);
            items_[c->id] = item;
        }
    for (auto& [id, item] : items_) {
        const Command* c = registry_.find(id);
        QStringList now, before;
        for (const QKeySequence& k : keys_.at(id)) now << shortcuts::portable(k);
        for (const QKeySequence& k : shortcuts::defaults(*c)) before << shortcuts::portable(k);
        QFont f = item->font(0);
        f.setBold(now != before);   // changed from the default
        item->setFont(0, f);
        item->setFont(1, f);
    }
    tree_->expandAll();
    applyFilter();
    if (auto it = items_.find(current); it != items_.end()) {
        QSignalBlocker block(tree_);
        tree_->setCurrentItem(it->second);
        tree_->scrollToItem(it->second);
    }
}

void KeyboardShortcutsDialog::applyFilter() {
    const QString text = search_->text().trimmed();
    // A group shows while any command under it does.
    std::function<bool(QTreeWidgetItem*, const QString&)> show = [&](QTreeWidgetItem* item, const QString& path) -> bool {
        const QString id = item->data(0, Qt::UserRole).toString();
        const QString here = path.isEmpty() ? item->text(0) : path + QStringLiteral(" > ") + item->text(0);
        bool visible = false;
        if (!id.isEmpty()) {
            if (text.isEmpty()) visible = true;
            else {
                QStringList haystack{here, englishText(item->text(0)), id, item->text(1)};
                for (const QKeySequence& k : keys_.at(id)) haystack << shortcuts::portable(k);
                for (const QString& s : haystack) if (s.contains(text, Qt::CaseInsensitive)) { visible = true; break; }
            }
        }
        for (int i = 0; i < item->childCount(); i++) visible = show(item->child(i), here) || visible;
        item->setHidden(!visible);
        return visible;
    };
    for (int i = 0; i < tree_->topLevelItemCount(); i++) show(tree_->topLevelItem(i), QString());
}

void KeyboardShortcutsDialog::setFilter(const QString& text) { search_->setText(text); }

void KeyboardShortcutsDialog::select(const QString& id) {
    if (auto it = items_.find(id); it != items_.end()) {
        tree_->setCurrentItem(it->second);
        tree_->scrollToItem(it->second, QAbstractItemView::PositionAtCenter);
        // Again once the dialog is laid out (it may not be shown yet).
        QTimer::singleShot(0, tree_, [this] { if (QTreeWidgetItem* item = tree_->currentItem()) tree_->scrollToItem(item, QAbstractItemView::PositionAtCenter); });
    }
}

QString KeyboardShortcutsDialog::commandName(const QString& id) const {
    const Command* c = registry_.find(id);
    if (!c) return id;
    QStringList path = c->menuPath;
    if (path.isEmpty()) path << (id.startsWith(QStringLiteral("tool.")) ? tr("Tools") : tr("Other Keys"));
    return (path << plainText(c->label)).join(QStringLiteral(" > "));
}

void KeyboardShortcutsDialog::refreshEditor() {
    // The selected command's keys, one field each, with a button to remove it.
    while (QLayoutItem* row = keyRows_->takeAt(0)) {
        if (QWidget* w = row->widget()) { w->hide(); w->deleteLater(); }
        delete row;
    }
    const bool has = !selected_.isEmpty() && keys_.count(selected_);
    heading_->setText(has ? commandName(selected_) : tr("Choose a command to change its shortcuts."));
    addKey_->setEnabled(has);
    useDefault_->setEnabled(has);
    if (!has) return;
    const QList<QKeySequence>& keys = keys_.at(selected_);
    for (int slot = 0; slot < keys.size(); slot++) {
        auto* row = new QWidget;
        auto* h = new QHBoxLayout(row);
        h->setContentsMargins(0, 0, 0, 0);
        auto* edit = new KeyCaptureEdit(keys[slot]);
        connect(edit, &KeyCaptureEdit::captured, this, [this, slot](const QKeySequence& key) { assign(selected_, slot, key); });
        auto* remove = new QToolButton;
        remove->setText(tr("Remove"));
        remove->setToolTip(tr("Remove this shortcut"));
        connect(remove, &QToolButton::clicked, this, [this, slot] { removeKey(selected_, slot); });
        h->addWidget(edit, 1);
        h->addWidget(remove);
        keyRows_->addWidget(row);
    }
    if (keys.isEmpty()) {
        auto* none = new QLabel(tr("No shortcut. Add Shortcut gives it one."));
        keyRows_->addWidget(none);
    }
}

void KeyboardShortcutsDialog::showMessage(const QString& text, bool warning) {
    message_->setText(text);
    message_->setStyleSheet(warning ? QStringLiteral("color: palette(bright-text); background: #b3261e; padding: 4px; border-radius: 3px;") : QString());
}

QString KeyboardShortcutsDialog::message() const { return message_->text(); }

QString KeyboardShortcutsDialog::ownerOf(const QKeySequence& key, const QString& except) const {
    const QString text = shortcuts::portable(key);
    for (const auto& [id, keys] : keys_) {
        if (id == except) continue;
        for (const QKeySequence& k : keys) if (shortcuts::portable(k) == text) return id;
    }
    return {};
}

void KeyboardShortcutsDialog::setKeys(const QString& id, QList<QKeySequence> keys) {
    keys_[id] = std::move(keys);
}

bool KeyboardShortcutsDialog::assign(const QString& id, int slot, const QKeySequence& key) {
    cancelConflict();
    if (!keys_.count(id)) return false;
    if (const QString why = shortcuts::refusal(key); !why.isEmpty()) {
        showMessage(why, true);
        refreshEditor();
        tree_->setFocus();   // a second Esc closes the dialog
        return false;
    }
    QList<QKeySequence> keys = keys_.at(id);
    for (int i = 0; i < keys.size(); i++)
        if (shortcuts::portable(keys[i]) == shortcuts::portable(key)) {
            showMessage(i == slot ? QString() : tr("%1 is already a shortcut of this command.").arg(key.toString(QKeySequence::NativeText)), false);
            refreshEditor();
            return true;
        }
    if (const QString owner = ownerOf(key, id); !owner.isEmpty()) {
        // Photoshop's conflict: say whose key it is, and take it only when asked.
        pending_ = Pending{id, slot, key, owner};
        conflictWith_ = owner;
        showMessage(tr("%1 is already the shortcut of %2. Take it from that command, or cancel the change?").arg(key.toString(QKeySequence::NativeText), commandName(owner)), true);
        conflictBox_->show();
        return false;
    }
    if (slot >= 0 && slot < keys.size()) keys[slot] = key;
    else keys << key;
    setKeys(id, keys);
    message_->clear();
    message_->setStyleSheet(QString());
    refreshTree();
    refreshEditor();
    return true;
}

void KeyboardShortcutsDialog::takeConflicting() {
    if (!pending_) return;
    const Pending p = *pending_;
    QList<QKeySequence> theirs = keys_.at(p.owner);
    theirs.erase(std::remove_if(theirs.begin(), theirs.end(), [&](const QKeySequence& k) { return shortcuts::portable(k) == shortcuts::portable(p.key); }), theirs.end());
    setKeys(p.owner, theirs);
    pending_.reset();
    conflictWith_.clear();
    conflictBox_->hide();
    assign(p.id, p.slot, p.key);
    showMessage(tr("%1 moved from %2.").arg(p.key.toString(QKeySequence::NativeText), commandName(p.owner)), false);
}

void KeyboardShortcutsDialog::cancelConflict() {
    if (!pending_) return;
    pending_.reset();
    conflictWith_.clear();
    conflictBox_->hide();
    refreshEditor();
}

void KeyboardShortcutsDialog::removeKey(const QString& id, int slot) {
    cancelConflict();
    if (!keys_.count(id)) return;
    QList<QKeySequence> keys = keys_.at(id);
    if (slot < 0 || slot >= keys.size()) return;
    keys.removeAt(slot);
    setKeys(id, keys);
    message_->clear();
    refreshTree();
    refreshEditor();
}

void KeyboardShortcutsDialog::resetCommand(const QString& id) {
    cancelConflict();
    const Command* c = registry_.find(id);
    if (!c || !keys_.count(id)) return;
    const QList<QKeySequence> wanted = shortcuts::defaults(*c);
    QStringList moved;
    for (const QKeySequence& key : wanted)
        if (const QString owner = ownerOf(key, id); !owner.isEmpty()) {
            QList<QKeySequence> theirs = keys_.at(owner);
            theirs.erase(std::remove_if(theirs.begin(), theirs.end(), [&](const QKeySequence& k) { return shortcuts::portable(k) == shortcuts::portable(key); }), theirs.end());
            setKeys(owner, theirs);
            moved << tr("%1 from %2").arg(key.toString(QKeySequence::NativeText), commandName(owner));
        }
    setKeys(id, wanted);
    if (moved.isEmpty()) message_->clear();
    else showMessage(tr("Taken back: %1.").arg(moved.join(QStringLiteral("; "))), false);
    refreshTree();
    refreshEditor();
}

void KeyboardShortcutsDialog::resetAll() {
    cancelConflict();
    keys_ = shortcuts::resolve(registry_, {});
    message_->clear();
    refreshTree();
    refreshEditor();
}

shortcuts::KeyMap KeyboardShortcutsDialog::overrides() const { return shortcuts::overridesOf(registry_, keys_); }

bool KeyboardShortcutsDialog::importFile(const QString& path, QString* error) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) { if (error) *error = tr("Couldn’t read %1.").arg(path); return false; }
    const auto imported = shortcuts::fromJson(file.read(4 << 20), error);
    if (!imported) return false;
    // The file's set in place of the dialog's: its changes over the defaults; ids this version lacks are left out.
    int unknown = 0;
    for (const auto& entry : *imported) if (!registry_.find(entry.first)) unknown++;
    cancelConflict();
    keys_ = shortcuts::resolve(registry_, *imported);
    refreshTree();
    refreshEditor();
    showMessage(unknown ? tr("Imported the shortcuts; %n command(s) in the file are not in this version.", nullptr, unknown) : tr("Imported the shortcuts."), false);
    return true;
}

bool KeyboardShortcutsDialog::exportFile(const QString& path, QString* error) const {
    QFile file(path);
    const QByteArray json = shortcuts::toJson(overrides());
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) || file.write(json) != json.size() || !file.flush()) {
        if (error) *error = tr("Couldn’t write %1.").arg(path);
        return false;
    }
    return true;
}

} // namespace app
