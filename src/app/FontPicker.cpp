#include "FontPicker.h"
#include <QApplication>
#include <QEvent>
#include <QFont>
#include <QFontDatabase>
#include <QFrame>
#include <QKeyEvent>
#include <QLineEdit>
#include <QScreen>
#include <QSettings>
#include <QStandardItemModel>
#include <QStyledItemDelegate>
#include <QTreeView>
#include <QVBoxLayout>
#include <functional>

namespace app {

namespace {

constexpr int recentLimit = 6;
constexpr int familyRole = Qt::UserRole + 1;

/// Rows draw in their own family but keep the list's row height, so tall scripts do not open gaps.
class RowHeightDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        QSize hint = QStyledItemDelegate::sizeHint(option, index);
        hint.setHeight(QFontMetrics(QApplication::font()).height() + 10);
        return hint;
    }
};

QStringList installedFamilies() {
    QStringList families;
    for (const QString& f : QFontDatabase::families()) if (!QFontDatabase::isPrivateFamily(f)) families << f;
    return families;
}

} // namespace

FontPicker::FontPicker(QWidget* parent) : QToolButton(parent) {
    setToolButtonStyle(Qt::ToolButtonTextOnly);
    setPopupMode(QToolButton::InstantPopup);
    setArrowType(Qt::NoArrow);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setToolTip(tr("Font family. Families sharing a name are folded into one row; expand it for the whole set, or type to filter."));
    recent_ = QSettings().value("text/recentFonts").toStringList();
    connect(this, &QToolButton::clicked, this, &FontPicker::openPopup);
    setFamily(QFontDatabase::systemFont(QFontDatabase::GeneralFont).family());
}

void FontPicker::setFamily(const QString& family) {
    family_ = family;
    QFontMetrics metrics(font());
    setText(metrics.elidedText(family, Qt::ElideRight, std::max(60, width() - 24)));
}

void FontPicker::showPicker() { if (!popup_) openPopup(); }

void FontPicker::openPopup() {
    if (popup_) { closePopup(); return; }
    if (nodes_.empty()) nodes_ = groupFonts(installedFamilies());
    popup_ = new QFrame(this, Qt::Popup | Qt::FramelessWindowHint);
    popup_->setFrameShape(QFrame::StyledPanel);
    popup_->setAttribute(Qt::WA_DeleteOnClose);
    auto* layout = new QVBoxLayout(popup_);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->setSpacing(4);
    filter_ = new QLineEdit;
    filter_->setPlaceholderText(tr("Filter fonts"));
    filter_->setClearButtonEnabled(true);
    layout->addWidget(filter_);
    tree_ = new QTreeView;
    tree_->setHeaderHidden(true);
    tree_->setRootIsDecorated(true);
    tree_->setUniformRowHeights(true);
    tree_->setItemDelegate(new RowHeightDelegate(tree_));
    tree_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    tree_->setSelectionMode(QAbstractItemView::SingleSelection);
    model_ = new QStandardItemModel(tree_);
    tree_->setModel(model_);
    layout->addWidget(tree_, 1);
    connect(filter_, &QLineEdit::textChanged, this, [this](const QString& text) { rebuild(text); });
    connect(tree_, &QTreeView::clicked, this, [this](const QModelIndex& index) {
        // A group row chooses its representative; the arrow still expands it.
        QString family = index.data(familyRole).toString();
        if (!family.isEmpty()) choose(family);
    });
    connect(tree_, &QTreeView::activated, this, [this](const QModelIndex& index) { QString f = index.data(familyRole).toString(); if (!f.isEmpty()) choose(f); });
    filter_->installEventFilter(this);
    tree_->installEventFilter(this);
    popup_->installEventFilter(this);
    connect(popup_, &QObject::destroyed, this, [this] { popup_ = nullptr; filter_ = nullptr; tree_ = nullptr; model_ = nullptr; });
    rebuild({});
    // Below the button, within the screen.
    QPoint below = mapToGlobal(QPoint(0, height()));
    QSize size(std::max(320, width()), 420);
    if (QScreen* screen = this->screen()) {
        QRect available = screen->availableGeometry();
        if (below.y() + size.height() > available.bottom()) below.setY(std::max(available.top(), mapToGlobal(QPoint(0, 0)).y() - size.height()));
        if (below.x() + size.width() > available.right()) below.setX(std::max(available.left(), available.right() - size.width()));
    }
    popup_->setGeometry(QRect(below, size));
    popup_->show();
    filter_->setFocus();
}

void FontPicker::closePopup() {
    if (popup_) popup_->close();
}

QStandardItem* FontPicker::itemFor(const FontGroup& node, const QString& filter, bool& anyMatch) {
    const bool group = node.isGroup();
    bool matches = filter.isEmpty() || node.label.contains(filter, Qt::CaseInsensitive);
    auto* item = new QStandardItem;
    item->setEditable(false);
    if (group) {
        bool childMatch = false;
        for (const FontGroup& child : node.children) {
            bool m = false;
            QStandardItem* c = itemFor(child, filter, m);
            if (m) { item->appendRow(c); childMatch = true; } else delete c;
        }
        if (!matches && !childMatch) { delete item; anyMatch = false; return nullptr; }
        if (matches && !childMatch) {
            // The group name matched but no child did: show every member after all.
            for (const FontGroup& child : node.children) { bool m = false; item->appendRow(itemFor(child, {}, m)); }
        }
        item->setText(fontGroupLabel(node.label, int(node.members.size())));
        item->setData(node.family, familyRole);
        QFont f(node.family); f.setPointSizeF(font().pointSizeF() + 1); item->setFont(f);
        item->setToolTip(tr("%1 families; this row is %2").arg(node.members.size()).arg(node.family));
    } else {
        if (!matches) { delete item; anyMatch = false; return nullptr; }
        item->setText(node.label);
        item->setData(node.family, familyRole);
        QFont f(node.family); f.setPointSizeF(font().pointSizeF() + 1); item->setFont(f);
    }
    anyMatch = true;
    return item;
}

void FontPicker::rebuild(const QString& filter) {
    if (!model_) return;
    model_->clear();
    const QString needle = filter.trimmed();
    if (needle.isEmpty() && !recent_.isEmpty()) {
        auto* recent = new QStandardItem(tr("Recent"));
        recent->setEditable(false);
        recent->setSelectable(false);
        QFont bold = font(); bold.setBold(true); recent->setFont(bold);
        for (const QString& f : recent_) {
            auto* item = new QStandardItem(f);
            item->setEditable(false);
            item->setData(f, familyRole);
            QFont ff(f); ff.setPointSizeF(font().pointSizeF() + 1); item->setFont(ff);
            recent->appendRow(item);
        }
        model_->appendRow(recent);
    }
    QStandardItem* current = nullptr;
    for (const FontGroup& node : nodes_) {
        bool m = false;
        QStandardItem* item = itemFor(node, needle, m);
        if (!item) continue;
        model_->appendRow(item);
        if (!current) {
            // Find the current family's row to select and reveal it.
            std::function<QStandardItem*(QStandardItem*)> find = [&](QStandardItem* it) -> QStandardItem* {
                if (it->data(familyRole).toString() == family_ && it->rowCount() == 0) return it;
                for (int r = 0; r < it->rowCount(); r++) if (QStandardItem* c = find(it->child(r))) return c;
                return nullptr;
            };
            current = find(item);
        }
    }
    if (!needle.isEmpty()) tree_->expandAll();
    else tree_->collapseAll();
    if (model_->rowCount() && model_->item(0)->text() == tr("Recent")) tree_->expand(model_->item(0)->index());
    if (current) {
        tree_->setCurrentIndex(current->index());
        tree_->scrollTo(current->index(), QAbstractItemView::PositionAtCenter);
    }
}

void FontPicker::choose(const QString& family) {
    recent_.removeAll(family);
    recent_.prepend(family);
    while (recent_.size() > recentLimit) recent_.removeLast();
    QSettings().setValue("text/recentFonts", recent_);
    closePopup();
    if (family == family_) return;
    setFamily(family);
    emit familyChanged(family);
}

bool FontPicker::eventFilter(QObject* watched, QEvent* event) {
    if (event->type() == QEvent::KeyPress) {
        auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Escape) { closePopup(); return true; }
        if (watched == filter_ && (key->key() == Qt::Key_Down || key->key() == Qt::Key_Up)) { tree_->setFocus(); QApplication::sendEvent(tree_, event); return true; }
        if (watched == filter_ && (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter)) {
            // The first family in the list (a group row stands for its representative).
            QModelIndex index = tree_->currentIndex().isValid() ? tree_->currentIndex() : model_->index(0, 0);
            if (index.isValid() && index.data(familyRole).toString().isEmpty() && model_->rowCount(index)) index = model_->index(0, 0, index);
            QString family = index.data(familyRole).toString();
            if (!family.isEmpty()) choose(family);
            return true;
        }
        if (watched == tree_ && (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter)) {
            QString family = tree_->currentIndex().data(familyRole).toString();
            if (!family.isEmpty()) { choose(family); return true; }
        }
    }
    return QToolButton::eventFilter(watched, event);
}

} // namespace app
