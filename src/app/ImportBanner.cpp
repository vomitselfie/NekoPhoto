#include "ImportBanner.h"
#include "Style.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QToolButton>

namespace app {

ImportBanner::ImportBanner(QWidget* parent) : QFrame(parent) {
    setObjectName("importBanner");
    setAccessibleName(tr("Import notes"));
    // An info bar in the theme's colours: the highlight faded toward the window, a line under it.
    const QPalette p = palette();
    const QColor hi = p.color(QPalette::Highlight), win = p.color(QPalette::Window);
    const QColor tint((hi.red() + win.red() * 4) / 5, (hi.green() + win.green() * 4) / 5, (hi.blue() + win.blue() * 4) / 5);
    setStyleSheet(QStringLiteral("#importBanner { background: %1; border-bottom: 1px solid %2; }").arg(tint.name(), hi.name()));
    auto* row = new QHBoxLayout(this);
    row->setContentsMargins(10, 4, 4, 4);
    row->setSpacing(6);
    text_ = new QLabel;
    text_->setTextFormat(Qt::PlainText);
    text_->setMinimumWidth(80);
    text_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    row->addWidget(text_, 1);
    auto* details = new QPushButton(tr("Details…"));
    details->setToolTip(tr("Every change the file went through"));
    row->addWidget(details);
    undo_ = new QPushButton(tr("Undo Open"));
    undo_->setToolTip(tr("Close the document again"));
    row->addWidget(undo_);
    auto* close = new QToolButton;
    close->setText(QStringLiteral("×"));
    close->setAutoRaise(true);
    close->setToolTip(tr("Hide this bar"));
    row->addWidget(close);
    connect(details, &QPushButton::clicked, this, &ImportBanner::showDetails);
    connect(undo_, &QPushButton::clicked, this, [this] { hide(); emit undoRequested(); });
    connect(close, &QToolButton::clicked, this, [this] { hide(); emit dismissed(); });
    hide();
}

void ImportBanner::present(const QString& summary, const QString& title, const QString& heading, const QStringList& notes, bool canUndo) {
    summary_ = summary;
    title_ = title;
    heading_ = heading;
    notes_ = notes;
    undo_->setVisible(canUndo);
    show();
    relayText();
}

void ImportBanner::resizeEvent(QResizeEvent* e) {
    QFrame::resizeEvent(e);
    relayText();
}

void ImportBanner::relayText() {
    // The first note shortened to the room there is, so the buttons always stay in view.
    const QString first = notes_.isEmpty() ? QString() : notes_.first().simplified();
    const QFontMetrics fm(text_->font());
    const int room = std::max(40, text_->width() - fm.horizontalAdvance(summary_.arg(QString())) - 4);
    text_->setText(summary_.arg(fm.elidedText(first, Qt::ElideRight, room)));
    text_->setToolTip(notes_.mid(0, 12).join('\n'));
}

void ImportBanner::showDetails() {
    auto* box = new QMessageBox(QMessageBox::Information, title_, heading_, QMessageBox::Ok, window());
    box->setInformativeText(notes_.mid(0, 6).join('\n') + (notes_.size() > 6 ? tr("\n… and %n more (see Details).", nullptr, int(notes_.size()) - 6) : QString()));
    box->setDetailedText(notes_.join('\n'));
    box->setAttribute(Qt::WA_DeleteOnClose);
    box->setModal(false);
    box->show();
}

} // namespace app
