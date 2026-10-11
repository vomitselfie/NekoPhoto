// Tab and Shift+Tab in the Standard Screen Mode: see HidePanels.h.
#include "HidePanels.h"
#include "CanvasWidget.h"
#include "FullScreen.h"
#include "MainWindow.h"
#include "ToolOptionsBar.h"
#include <QAction>
#include <QApplication>
#include <QDockWidget>
#include <QKeyEvent>
#include <QPointer>
#include <QToolBar>

namespace app {

bool takesTypedText(const QWidget* w) {
    return w && (w->inherits("QLineEdit") || w->inherits("QAbstractSpinBox") || w->inherits("QTextEdit") || w->inherits("QPlainTextEdit"));
}

HidePanels::HidePanels(MainWindow* window) : QObject(window), window_(window) {
    // F7 and the Window menu's panels while they are hidden: the layout comes back, with that panel showing.
    for (QDockWidget* dock : window->findChildren<QDockWidget*>(Qt::FindDirectChildrenOnly))
        connect(dock->toggleViewAction(), &QAction::triggered, this, [this, dock = QPointer<QDockWidget>(dock)](bool checked) {
            if (!dock || !checked || hidden_ == None || (window_->fullScreen_ && window_->fullScreen_->active())) return;
            set(None);
            dock->show();
            dock->raise();
        });
    // Tab and Shift+Tab move between fields while one has the keys, rather than hiding the panels (the canvas's
    // typing keeps its keys itself).
    qApp->installEventFilter(this);
}

HidePanels::~HidePanels() { qApp->removeEventFilter(this); }

void HidePanels::set(Hidden h) {
    if (h == hidden_) return;
    if (hidden_ != None) restore();
    hidden_ = h;
    if (h != None) {
        saved_ = window_->saveState();
        hideNow();
    }
}

bool HidePanels::hidesOptions() const {
    return hidden_ == All && !(window_->fullScreen_ && window_->fullScreen_->active());
}

QByteArray HidePanels::normalState() const { return hidden_ != None ? saved_ : window_->saveState(); }

void HidePanels::beforeFullScreen() {
    if (hidden_ != None) restore();
}

void HidePanels::afterFullScreen() {
    if (hidden_ == None) return;
    saved_ = window_->saveState();
    hideNow();
}

void HidePanels::restore() {
    MainWindow* w = window_;
    w->restoreState(saved_);
    // The saved state names every tab's options bar alike; the tab on screen owns the bar.
    for (size_t i = 0; i < w->tabs_.size(); i++) w->tabs_[i].options->setVisible(int(i) == w->current_);
}

void HidePanels::hideNow() {
    MainWindow* w = window_;
    QWidget* focus = QApplication::focusWidget();
    std::vector<QWidget*> hiding;
    for (QDockWidget* d : w->findChildren<QDockWidget*>(Qt::FindDirectChildrenOnly)) {
        if (d->isHidden()) continue;
        if (hidden_ == All || (!d->isFloating() && w->dockWidgetArea(d) == Qt::RightDockWidgetArea)) hiding.push_back(d);
    }
    if (hidden_ == All) {
        if (w->toolRail_ && !w->toolRail_->isHidden()) hiding.push_back(w->toolRail_);
        for (const auto& tab : w->tabs_) if (!tab.options->isHidden()) hiding.push_back(tab.options);
    }
    for (QWidget* x : hiding) {
        if (focus && (x == focus || x->isAncestorOf(focus)) && w->canvas_) w->canvas_->setFocus();
        x->hide();
    }
}

bool HidePanels::eventFilter(QObject*, QEvent* event) {
    if (event->type() != QEvent::ShortcutOverride) return false;
    auto* ke = static_cast<QKeyEvent*>(event);
    const bool plain = !(ke->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier));   // Ctrl+Tab still switches documents
    if ((ke->key() == Qt::Key_Tab || ke->key() == Qt::Key_Backtab) && plain && takesTypedText(QApplication::focusWidget())) {
        ke->accept();
        return true;
    }
    return false;
}

} // namespace app
