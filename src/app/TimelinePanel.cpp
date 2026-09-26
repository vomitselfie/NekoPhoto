#include "TimelinePanel.h"
#include "EditorSession.h"
#include "ImageConvert.h"
#include "compositor/animation.h"
#include "compositor/render.h"
#include <QComboBox>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QButtonGroup>
#include <QScrollArea>
#include <QPainter>
#include <QPushButton>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStyle>
#include <QToolButton>
#include <QVBoxLayout>

using namespace compositor;

namespace app {

namespace {

constexpr int thumbSize = 72;

QToolButton* tool(const QIcon& icon, const QString& text, const QString& tip) {
    auto* b = new QToolButton;
    if (icon.isNull()) b->setText(text); else b->setIcon(icon);
    b->setToolTip(tip);
    b->setAutoRaise(true);
    return b;
}

QString delayText(int ms) { return ms % 1000 == 0 ? QStringLiteral("%1 s").arg(ms / 1000) : QStringLiteral("%1 s").arg(ms / 1000.0, 0, 'g', 3); }

} // namespace

TimelinePanel::TimelinePanel(QWidget* parent) : QWidget(parent) {
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(4, 4, 4, 4);
    stack_ = new QStackedWidget;
    outer->addWidget(stack_);

    // Without frames: Photoshop's "Create Frame Animation" button.
    auto* empty = new QWidget;
    auto* emptyLayout = new QHBoxLayout(empty);
    auto* create = new QPushButton(tr("Create Frame Animation"));
    auto* fromLayers = new QPushButton(tr("Make Frames From Layers"));
    emptyLayout->addStretch(1);
    emptyLayout->addWidget(create);
    emptyLayout->addWidget(fromLayers);
    emptyLayout->addStretch(1);
    stack_->addWidget(empty);

    auto* page = new QWidget;
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* strip = new QWidget;
    strip_ = new QHBoxLayout(strip);
    strip_->setContentsMargins(2, 2, 2, 2);
    strip_->setSpacing(4);
    strip_->addStretch(1);
    scroll->setWidget(strip);
    scroll->setFixedHeight(thumbSize + 72);
    scroll->setToolTip(tr("Click a frame to show it; changes to layer visibility, position and opacity go into the selected frame."));
    cells_ = new QButtonGroup(this);
    cells_->setExclusive(true);
    layout->addWidget(scroll);
    auto* row = new QHBoxLayout;
    row->setSpacing(2);
    auto* first = tool(style()->standardIcon(QStyle::SP_MediaSkipBackward), tr("First"), tr("Select the first frame"));
    auto* previous = tool(style()->standardIcon(QStyle::SP_MediaSeekBackward), tr("Previous"), tr("Select the previous frame"));
    playButton_ = tool(style()->standardIcon(QStyle::SP_MediaPlay), tr("Play"), tr("Play the animation"));
    playButton_->setCheckable(true);
    auto* next = tool(style()->standardIcon(QStyle::SP_MediaSeekForward), tr("Next"), tr("Select the next frame"));
    loops_ = new QComboBox;
    loops_->addItem(tr("Forever"), 0);
    loops_->addItem(tr("Once"), 1);
    loops_->addItem(tr("3 times"), 3);
    loops_->addItem(tr("Other…"), -1);
    loops_->setToolTip(tr("How many times the animation plays (saved in the project and the GIF)"));
    delay_ = new QSpinBox;
    delay_->setRange(0, maxFrameDelayMs);
    delay_->setSingleStep(10);
    delay_->setSuffix(tr(" ms"));
    delay_->setToolTip(tr("How long the selected frame shows"));
    auto* allDelays = tool({}, tr("All"), tr("Give every frame this delay"));
    auto* left = tool(style()->standardIcon(QStyle::SP_ArrowLeft), tr("Left"), tr("Move the frame earlier"));
    auto* right = tool(style()->standardIcon(QStyle::SP_ArrowRight), tr("Right"), tr("Move the frame later"));
    auto* duplicate = tool({}, QStringLiteral("+"), tr("New frame: a copy of the selected one"));
    auto* remove = tool(style()->standardIcon(QStyle::SP_TrashIcon), tr("Delete"), tr("Delete the selected frame"));
    auto* layersButton = tool({}, tr("From Layers"), tr("Make Frames From Layers: a frame per top-level layer"));
    info_ = new QLabel;
    for (QWidget* w : {static_cast<QWidget*>(loops_), static_cast<QWidget*>(first), static_cast<QWidget*>(previous), static_cast<QWidget*>(playButton_), static_cast<QWidget*>(next)}) row->addWidget(w);
    row->addSpacing(12);
    row->addWidget(new QLabel(tr("Delay")));
    row->addWidget(delay_);
    row->addWidget(allDelays);
    row->addSpacing(12);
    for (QToolButton* b : {left, right, duplicate, remove, layersButton}) row->addWidget(b);
    row->addStretch(1);
    row->addWidget(info_);
    layout->addLayout(row);
    stack_->addWidget(page);

    connect(create, &QPushButton::clicked, this, [this] { if (session_) session_->timelineCreate(); });
    connect(fromLayers, &QPushButton::clicked, this, [this] { if (session_) session_->timelineFramesFromLayers(); });
    connect(layersButton, &QToolButton::clicked, this, [this] { if (session_) session_->timelineFramesFromLayers(); });
    connect(cells_, &QButtonGroup::idClicked, this, [this](int row) {
        if (refreshing_ || !session_ || row < 0) return;
        play(false);
        session_->timelineSelectFrame(row);
    });
    auto go = [this](int to) {
        if (!session_ || !session_->document() || session_->document()->animation.empty()) return;
        play(false);
        const int n = int(session_->document()->animation.frames.size());
        session_->timelineSelectFrame(std::clamp(to, 0, n - 1));
    };
    connect(first, &QToolButton::clicked, this, [go] { go(0); });
    connect(previous, &QToolButton::clicked, this, [this, go] { go(session_ && session_->document() ? session_->document()->animation.current - 1 : 0); });
    connect(next, &QToolButton::clicked, this, [this, go] { go(session_ && session_->document() ? session_->document()->animation.current + 1 : 0); });
    connect(playButton_, &QToolButton::toggled, this, [this](bool on) { play(on); });
    connect(delay_, &QSpinBox::editingFinished, this, [this] {
        if (refreshing_ || !session_ || !session_->document() || session_->document()->animation.empty()) return;
        session_->timelineSetDelay(session_->document()->animation.current, delay_->value());
    });
    connect(allDelays, &QToolButton::clicked, this, [this] { if (session_) session_->timelineSetDelay(-1, delay_->value()); });
    connect(left, &QToolButton::clicked, this, [this] {
        if (!session_ || !session_->document()) return;
        const int c = session_->document()->animation.current;
        if (c > 0) session_->timelineMoveFrame(c, c - 1);
    });
    connect(right, &QToolButton::clicked, this, [this] {
        if (!session_ || !session_->document()) return;
        const int c = session_->document()->animation.current;
        if (c + 1 < int(session_->document()->animation.frames.size())) session_->timelineMoveFrame(c, c + 1);
    });
    connect(duplicate, &QToolButton::clicked, this, [this] { if (session_) { play(false); session_->timelineDuplicateFrame(); } });
    connect(remove, &QToolButton::clicked, this, [this] {
        if (!session_ || !session_->document() || session_->document()->animation.empty()) return;
        play(false);
        session_->timelineDeleteFrame(session_->document()->animation.current);
    });
    connect(loops_, QOverload<int>::of(&QComboBox::activated), this, [this](int index) {
        if (!session_) return;
        int loops = loops_->itemData(index).toInt();
        if (loops < 0) {
            bool ok = false;
            const int current = session_->document() ? session_->document()->animation.loopCount : 0;
            loops = QInputDialog::getInt(this, tr("Looping Options"), tr("Play this many times"), std::max(1, current), 1, 65535, 1, &ok);
            if (!ok) { refresh(); return; }
        }
        session_->timelineSetLoopCount(loops);
    });

    playTimer_.setSingleShot(true);
    connect(&playTimer_, &QTimer::timeout, this, [this] { step(); });
    thumbTimer_.setSingleShot(true);
    thumbTimer_.setInterval(150);
    connect(&thumbTimer_, &QTimer::timeout, this, [this] { refreshThumbnails(); });
    refresh();
}

void TimelinePanel::setSession(EditorSession* session) {
    if (session == session_) return;
    play(false);
    for (auto& c : connections_) disconnect(c);
    connections_.clear();
    session_ = session;
    if (session_) {
        // Hidden, the panel catches up when it shows (showEvent).
        connections_ << connect(session_, &EditorSession::documentChanged, this, [this] { if (!playing() && isVisible()) { refresh(); thumbTimer_.start(); } });
        connections_ << connect(session_, &EditorSession::documentChangedAsShown, this, [this] { if (isVisible()) thumbTimer_.start(); });
        connections_ << connect(session_, &EditorSession::historyChanged, this, [this] { if (!playing() && isVisible()) refresh(); });
    }
    refresh();
    refreshThumbnails();
}

void TimelinePanel::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    refresh();
    thumbTimer_.start();
}

void TimelinePanel::selectCell(int index) {
    if (QAbstractButton* b = cells_->button(index)) b->setChecked(true);
}

void TimelinePanel::refresh() {
    refreshing_ = true;
    const Document* doc = session_ && session_->document() ? &*session_->document() : nullptr;
    const bool animated = doc && !doc->animation.empty();
    stack_->setCurrentIndex(animated ? 1 : 0);
    stack_->setEnabled(doc != nullptr);
    if (animated) {
        const Animation& a = doc->animation;
        while (int(cells_->buttons().size()) > int(a.frames.size())) {
            QAbstractButton* last = cells_->button(int(cells_->buttons().size()) - 1);
            cells_->removeButton(last);
            delete last;
        }
        while (int(cells_->buttons().size()) < int(a.frames.size())) {
            auto* cell = new QToolButton;
            cell->setCheckable(true);
            cell->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
            cell->setIconSize(QSize(thumbSize, thumbSize));
            cell->setAutoRaise(true);
            const int index = int(cells_->buttons().size());
            cells_->addButton(cell, index);
            strip_->insertWidget(index, cell);
            thumbTimer_.start();
        }
        for (int i = 0; i < int(a.frames.size()); i++) cells_->button(i)->setText(QStringLiteral("%1  %2").arg(i + 1).arg(delayText(a.frames[size_t(i)].delayMs)));
        selectCell(std::clamp(a.current, 0, int(a.frames.size()) - 1));
        if (!delay_->hasFocus()) delay_->setValue(a.frames[size_t(std::clamp(a.current, 0, int(a.frames.size()) - 1))].delayMs);
        int index = loops_->findData(a.loopCount);
        if (index < 0) {
            loops_->setItemText(loops_->count() - 1, tr("%1 times").arg(a.loopCount));
            index = loops_->count() - 1;
        } else {
            loops_->setItemText(loops_->count() - 1, tr("Other…"));
        }
        loops_->setCurrentIndex(index);
        int total = 0;
        for (const AnimationFrame& f : a.frames) total += f.delayMs;
        info_->setText(tr("%n frame(s), %1", nullptr, int(a.frames.size())).arg(delayText(total)));
    } else {
        for (QAbstractButton* b : cells_->buttons()) { cells_->removeButton(b); delete b; }
    }
    refreshing_ = false;
}

void TimelinePanel::refreshThumbnails() {
    if (!session_ || !session_->document() || session_->document()->animation.empty() || !isVisible()) return;
    const Document& doc = *session_->document();
    const double scale = double(thumbSize) / std::max(doc.width, doc.height);
    const int w = std::max(1, int(doc.width * scale)), h = std::max(1, int(doc.height * scale));
    Document copy = doc;
    for (int i = 0; i < int(cells_->buttons().size()) && i < int(doc.animation.frames.size()); i++) {
        applyFrame(copy, doc.animation.frames[size_t(i)]);
        Image out(w, h);
        RenderOptions options;
        options.scale = scale;
        compositor::render(copy, options, out);
        QPixmap pixmap(thumbSize, thumbSize);
        pixmap.fill(Qt::transparent);
        QPainter p(&pixmap);
        const QRect target((thumbSize - w) / 2, (thumbSize - h) / 2, w, h);
        // A checkerboard behind transparency, as the canvas shows it.
        for (int y = 0; y < h; y += 6) for (int x = 0; x < w; x += 6) p.fillRect(QRect(target.x() + x, target.y() + y, 6, 6), ((x + y) / 6) % 2 ? QColor(204, 204, 204) : QColor(255, 255, 255));
        p.drawImage(target, wrapImage(out));
        p.end();
        cells_->button(i)->setIcon(QIcon(pixmap));
    }
}

void TimelinePanel::play(bool on) {
    if (on == playing() && on == playButton_->isChecked()) return;
    if (!on) {
        playTimer_.stop();
        { QSignalBlocker block(playButton_); playButton_->setChecked(false); }
        playButton_->setIcon(style()->standardIcon(QStyle::SP_MediaPlay));
        if (session_) session_->endFramePreview();
        refresh();
        return;
    }
    if (!session_ || !session_->document() || session_->document()->animation.frames.size() < 2) { QSignalBlocker block(playButton_); playButton_->setChecked(false); return; }
    { QSignalBlocker block(playButton_); playButton_->setChecked(true); }
    playButton_->setIcon(style()->standardIcon(QStyle::SP_MediaStop));
    const Animation& a = session_->document()->animation;
    playFrame_ = a.current;
    loopsLeft_ = a.loopCount;
    session_->previewFrame(playFrame_);
    playTimer_.start(std::max(20, a.frames[size_t(playFrame_)].delayMs));
}

void TimelinePanel::step() {
    if (!session_ || !session_->document() || session_->document()->animation.frames.size() < 2 || !session_->previewingFrames()) { play(false); return; }
    const Animation& a = session_->document()->animation;
    playFrame_++;
    if (playFrame_ >= int(a.frames.size())) {
        // One pass done: stop after loopCount of them (0 plays on).
        if (loopsLeft_ > 0 && --loopsLeft_ == 0) { play(false); return; }
        playFrame_ = 0;
    }
    session_->previewFrame(playFrame_);
    selectCell(playFrame_);
    playTimer_.start(std::max(20, a.frames[size_t(playFrame_)].delayMs));
}

} // namespace app
