// Window > History: see HistoryPanel.h.
#include "HistoryPanel.h"
#include "EditorSession.h"
#include <QListWidget>
#include <QVBoxLayout>

namespace app {

HistoryPanel::HistoryPanel(std::function<void(int)> step, QWidget* parent) : QWidget(parent), step_(std::move(step)) {
    setObjectName(QStringLiteral("historyPanel"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    list_ = new QListWidget;
    list_->setObjectName(QStringLiteral("historyList"));
    list_->setSelectionMode(QAbstractItemView::SingleSelection);
    list_->setUniformItemSizes(true);
    layout->addWidget(list_);
    connect(list_, &QListWidget::itemClicked, this, [this](QListWidgetItem* item) {
        const int row = list_->row(item);
        if (row != undoCount_ && step_) step_(row - undoCount_);
        refresh();   // the click's own selection gives way to the state on screen
    });
}

void HistoryPanel::setSession(EditorSession* session) {
    if (session_ == session) return;
    disconnect(watch_);
    session_ = session;
    if (session) watch_ = connect(session, &EditorSession::historyChanged, this, &HistoryPanel::refresh);
    refresh();
}

void HistoryPanel::refresh() {
    list_->clear();
    undoCount_ = 0;
    if (!session_ || !session_->hasDocument()) return;
    const std::vector<std::string> undo = session_->undoNames(), redo = session_->redoNames();
    undoCount_ = int(undo.size());
    list_->addItem(tr("Opened"));
    for (const std::string& name : undo) list_->addItem(QString::fromStdString(name));
    // Undone steps, greyed, the next to redo first.
    for (const std::string& name : redo) {
        auto* item = new QListWidgetItem(QString::fromStdString(name));
        item->setForeground(palette().color(QPalette::Disabled, QPalette::Text));
        QFont f = item->font();
        f.setItalic(true);
        item->setFont(f);
        list_->addItem(item);
    }
    list_->setCurrentRow(undoCount_);
    list_->scrollToItem(list_->currentItem());
}

} // namespace app
