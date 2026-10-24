#include "BrushPicker.h"
#include "BrushLibrary.h"
#include <QApplication>
#include <QFrame>
#include <QKeyEvent>
#include <QLineEdit>
#include <QPushButton>
#include <QScreen>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QVBoxLayout>
#include <algorithm>

namespace app {

namespace {
constexpr int idRole = Qt::UserRole + 1;
constexpr int iconSide = 40;
} // namespace

BrushPicker::BrushPicker(QWidget* parent) : QToolButton(parent) {
    setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    setIconSize(QSize(20, 20));
    setToolTip(tr("Brush: the round tip, or one of the MyPaint presets (pencils, inks, charcoal, paint, smudging). "
                  "Presets respond to pen pressure and tilt; Size and Opacity still apply."));
    connect(this, &QToolButton::clicked, this, [this] { openPopup(); });
    setPreset({});
}

void BrushPicker::setPreset(const QString& id) {
    const BrushPreset* preset = id.isEmpty() ? nullptr : BrushLibrary::find(id);
    preset_ = preset ? id : QString();
    setText(preset ? preset->name : tr("Round"));
    setIcon(preset ? preset->icon() : QIcon());
}

void BrushPicker::showPicker() { if (!popup_) openPopup(); }
void BrushPicker::showPickerAt(QPoint globalPosition) { if (!popup_) openPopup(globalPosition); }

void BrushPicker::openPopup(std::optional<QPoint> at) {
    if (popup_) { popup_->close(); return; }
    popup_ = new QFrame(this, Qt::Popup | Qt::FramelessWindowHint);
    popup_->setFrameShape(QFrame::StyledPanel);
    popup_->setAttribute(Qt::WA_DeleteOnClose);
    auto* layout = new QVBoxLayout(popup_);
    layout->setContentsMargins(4, 4, 4, 4);
    auto* list = new BrushList;
    list->setPreset(preset_);
    layout->addWidget(list);
    connect(list, &BrushList::presetChosen, this, [this](const QString& id) { choose(id); });
    connect(list, &BrushList::importRequested, this, [this] { if (popup_) popup_->close(); emit importRequested(); });
    connect(list, &BrushList::dismissed, this, [this] { if (popup_) popup_->close(); });
    connect(popup_, &QObject::destroyed, this, [this] { popup_ = nullptr; });
    // Below the button, within the screen.
    QPoint below = at ? *at : mapToGlobal(QPoint(0, height()));
    QSize size(340, 480);
    if (QScreen* screen = this->screen()) {
        QRect available = screen->availableGeometry();
        if (below.y() + size.height() > available.bottom()) below.setY(std::max(available.top(), (at ? at->y() : mapToGlobal(QPoint(0, 0)).y()) - size.height()));
        if (below.x() + size.width() > available.right()) below.setX(std::max(available.left(), available.right() - size.width()));
    }
    popup_->setGeometry(QRect(below, size));
    popup_->show();
    list->filterField()->setFocus();
}

void BrushPicker::choose(const QString& id) {
    if (popup_) popup_->close();
    if (id == preset_) return;
    setPreset(id);
    emit presetChosen(preset_);
}

// ---- BrushList ----

BrushList::BrushList(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("brushList"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(4);
    filter_ = new QLineEdit;
    filter_->setPlaceholderText(BrushPicker::tr("Filter brushes"));
    filter_->setClearButtonEnabled(true);
    layout->addWidget(filter_);
    tree_ = new QTreeWidget;
    tree_->setHeaderHidden(true);
    tree_->setIconSize(QSize(iconSide * 2, iconSide));   // square MyPaint previews, wide tip-brush strokes
    tree_->setUniformRowHeights(true);
    tree_->setSelectionMode(QAbstractItemView::SingleSelection);
    layout->addWidget(tree_, 1);
    auto* import = new QPushButton(BrushPicker::tr("Import Brushes…"));
    import->setToolTip(BrushPicker::tr("Photoshop .abr, Procreate .brushset and .brush, Clip Studio .sut, or images to use as tips"));
    connect(import, &QPushButton::clicked, this, [this] { emit importRequested(); });
    layout->addWidget(import);
    connect(filter_, &QLineEdit::textChanged, this, [this](const QString& text) { rebuild(text); });
    auto chosen = [this](QTreeWidgetItem* item) {
        if (!item->data(0, idRole).isValid()) return;
        preset_ = item->data(0, idRole).toString();
        emit presetChosen(preset_);
    };
    connect(tree_, &QTreeWidget::itemClicked, this, chosen);
    connect(tree_, &QTreeWidget::itemActivated, this, chosen);
    filter_->installEventFilter(this);
    tree_->installEventFilter(this);
    rebuild({});
}

void BrushList::setPreset(const QString& id) {
    const BrushPreset* preset = id.isEmpty() ? nullptr : BrushLibrary::find(id);
    const QString wanted = preset ? id : QString();
    if (wanted == preset_ && tree_->currentItem()) return;
    preset_ = wanted;
    rebuild(filter_->text());
}

void BrushList::refresh() { rebuild(filter_->text()); }

void BrushList::rebuild(const QString& filter) {
    tree_->clear();
    QTreeWidgetItem* current = nullptr;
    if (filter.isEmpty() || BrushPicker::tr("Round").contains(filter, Qt::CaseInsensitive)) {
        auto* round = new QTreeWidgetItem(tree_, {BrushPicker::tr("Round")});
        round->setToolTip(0, BrushPicker::tr("The classic round tip: Size, Hardness and Opacity"));
        round->setData(0, idRole, QString());
        if (preset_.isEmpty()) current = round;
    }
    for (const QString& group : BrushLibrary::groups()) {
        QTreeWidgetItem* header = nullptr;
        for (const BrushPreset& preset : BrushLibrary::presets()) {
            if (preset.group != group) continue;
            if (!filter.isEmpty() && !preset.name.contains(filter, Qt::CaseInsensitive) && !group.contains(filter, Qt::CaseInsensitive)) continue;
            if (!header) {
                header = new QTreeWidgetItem(tree_, {group});
                header->setFlags(Qt::ItemIsEnabled);
                QFont bold = header->font(0);
                bold.setBold(true);
                header->setFont(0, bold);
            }
            auto* item = new QTreeWidgetItem(header, {preset.name});
            item->setIcon(0, preset.icon());
            item->setData(0, idRole, preset.id);
            item->setToolTip(0, preset.eraser ? BrushPicker::tr("%1 (erases)").arg(preset.name) : preset.name);
            if (preset.id == preset_) current = item;
        }
        // Groups start folded unless filtering or holding the current preset.
        if (header) header->setExpanded(!filter.isEmpty() || (current && current->parent() == header));
    }
    if (current) { tree_->setCurrentItem(current); tree_->scrollToItem(current); }
}

bool BrushList::eventFilter(QObject* watched, QEvent* event) {
    if (event->type() == QEvent::KeyPress) {
        auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Escape) { emit dismissed(); return true; }
        if (watched == filter_ && (key->key() == Qt::Key_Down || key->key() == Qt::Key_Up)) { tree_->setFocus(); QApplication::sendEvent(tree_, event); return true; }
        if (watched == filter_ && (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter)) {
            // The first preset the filter left.
            for (QTreeWidgetItemIterator it(tree_); *it; ++it)
                if ((*it)->data(0, idRole).isValid()) { preset_ = (*it)->data(0, idRole).toString(); emit presetChosen(preset_); return true; }
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

} // namespace app
