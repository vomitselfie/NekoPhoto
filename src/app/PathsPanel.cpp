#include "PathsPanel.h"
#include <QHBoxLayout>
#include <QInputDialog>
#include <QListWidget>
#include <QMenu>
#include <QToolButton>
#include <QVBoxLayout>

using namespace compositor;

namespace app {

PathsPanel::PathsPanel(EditorSession* session, QWidget* parent) : QWidget(parent), session_(session) {
    auto* box = new QVBoxLayout(this);
    box->setContentsMargins(4, 4, 4, 4);
    list_ = new QListWidget(this);
    list_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    list_->setContextMenuPolicy(Qt::CustomContextMenu);
    box->addWidget(list_, 1);
    auto* buttons = new QHBoxLayout;
    auto button = [&](const QString& text, const QString& tip, auto action) {
        auto* b = new QToolButton(this);
        b->setText(text);
        b->setToolTip(tip);
        connect(b, &QToolButton::clicked, this, action);
        buttons->addWidget(b);
        return b;
    };
    button(tr("Fill"), tr("Fill the path with the foreground colour"), [this] { if (auto id = chosen()) session_->fillPath(*id); });
    button(tr("Stroke"), tr("Stroke the path with the brush's size in the foreground colour"), [this] { if (auto id = chosen()) session_->strokePath(*id); });
    button(tr("Select"), tr("Load the path as a selection"), [this] { if (auto id = chosen()) session_->pathToSelection(*id, SelectionMode::Replace); });
    button(tr("From Sel."), tr("Make a work path from the selection"), [this] { session_->selectionToWorkPath(); });
    button(tr("Shape"), tr("Make a shape layer from the path"), [this] { if (auto id = chosen()) session_->pathToShapeLayer(*id); });
    button(tr("New"), tr("Create a new path"), [this] { session_->newPath(tr("Path %1").arg(list_->count() + 1)); });
    button(tr("Delete"), tr("Delete the path"), [this] { if (auto id = chosen()) session_->deletePath(*id); });
    buttons->addStretch();
    box->addLayout(buttons);

    connect(list_, &QListWidget::currentRowChanged, this, [this] { if (!refreshing_) session_->selectPath(chosen()); });
    connect(list_, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem* item) {
        const uint16_t id = uint16_t(item->data(Qt::UserRole).toUInt());
        // Double-clicking the Work Path saves it, as in Photoshop; a saved path is renamed.
        bool ok = false;
        const QString name = QInputDialog::getText(this, id == kWorkPathId ? tr("Save Path") : tr("Rename Path"), tr("Name"), QLineEdit::Normal,
                                                   id == kWorkPathId ? tr("Path %1").arg(list_->count()) : item->text(), &ok);
        if (!ok) return;
        if (id == kWorkPathId) session_->savePath(id, name); else session_->renamePath(id, name);
    });
    connect(list_, &QListWidget::customContextMenuRequested, this, [this](const QPoint& at) {
        auto id = chosen();
        if (!id) return;
        QMenu menu(this);
        menu.addAction(tr("Make Selection"), this, [this, id] { session_->pathToSelection(*id, SelectionMode::Replace); });
        menu.addAction(tr("Add to Selection"), this, [this, id] { session_->pathToSelection(*id, SelectionMode::Add); });
        menu.addAction(tr("Fill Path"), this, [this, id] { session_->fillPath(*id); });
        menu.addAction(tr("Stroke Path"), this, [this, id] { session_->strokePath(*id); });
        menu.addAction(tr("Make Shape Layer"), this, [this, id] { session_->pathToShapeLayer(*id); });
        menu.addSeparator();
        menu.addAction(tr("Deselect Path"), this, [this] { session_->selectPath(std::nullopt); });
        menu.addAction(tr("Delete Path"), this, [this, id] { session_->deletePath(*id); });
        menu.exec(list_->viewport()->mapToGlobal(at));
    });
    connect(session_, &EditorSession::pathsChanged, this, &PathsPanel::refresh);
    connect(session_, &EditorSession::layersChanged, this, &PathsPanel::refresh);
    connect(session_, &EditorSession::historyChanged, this, &PathsPanel::refresh);
    refresh();
}

std::optional<uint16_t> PathsPanel::chosen() const {
    auto* item = list_->currentItem();
    return item ? std::optional<uint16_t>(uint16_t(item->data(Qt::UserRole).toUInt())) : std::nullopt;
}

void PathsPanel::refresh() {
    if (!session_) return;
    refreshing_ = true;
    list_->clear();
    for (const auto& p : session_->paths()) {
        auto* item = new QListWidgetItem(QString::fromStdString(p.name), list_);
        item->setData(Qt::UserRole, unsigned(p.id));
        if (p.id == kWorkPathId) { QFont f = item->font(); f.setItalic(true); item->setFont(f); }
        if (session_->activePathId() == p.id) list_->setCurrentItem(item);
    }
    refreshing_ = false;
}

} // namespace app
