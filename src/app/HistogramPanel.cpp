#include "HistogramPanel.h"
#include "EditorSession.h"
#include "HistogramView.h"
#include "Names.h"
#include "Style.h"
#include <QComboBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QSettings>
#include <QShowEvent>
#include <QStyle>
#include <QToolButton>
#include <QVBoxLayout>
#include <cmath>

using namespace compositor;

namespace app {

namespace {

/// How long the panel waits after a change before counting again; changes meanwhile wait for that count.
constexpr int throttleMs = 250;

QLabel* statLabel(QGridLayout* grid, int row, int column, const QString& name) {
    auto* label = new QLabel(name);
    label->setStyleSheet(hintStyle());
    grid->addWidget(label, row, column);
    auto* value = new QLabel;
    value->setTextInteractionFlags(Qt::TextSelectableByMouse);
    grid->addWidget(value, row, column + 1);
    return value;
}

} // namespace

HistogramPanel::HistogramPanel(QWidget* parent) : QWidget(parent) {
    QSettings settings;
    expanded_ = settings.value("histogram/expanded", false).toBool();
    showStats_ = settings.value("histogram/statistics", true).toBool();

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->setSpacing(4);

    // Channel above the graph, and beside them the warning, refresh and panel-menu buttons (in Compact View too).
    auto* graph = new QGridLayout;
    graph->setContentsMargins(0, 0, 0, 0);
    graph->setSpacing(2);
    channelRow_ = new QWidget;
    auto* channelLayout = new QHBoxLayout(channelRow_);
    channelLayout->setContentsMargins(0, 0, 0, 0);
    channelLayout->addWidget(new QLabel(tr("Channel:")));
    channel_ = new QComboBox;
    channel_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    channel_->setMinimumContentsLength(6);
    channelLayout->addWidget(channel_, 1);
    graph->addWidget(channelRow_, 0, 0);
    view_ = new HistogramView;
    view_->setMinimumSize(120, 80);
    graph->addWidget(view_, 1, 0);
    auto* buttons = new QVBoxLayout;
    buttons->setContentsMargins(0, 0, 0, 0);
    buttons->setSpacing(0);
    menu_ = new QToolButton;
    menu_->setAutoRaise(true);
    menu_->setText(QStringLiteral("\u2261"));
    menu_->setPopupMode(QToolButton::InstantPopup);
    menu_->setToolTip(tr("Histogram options"));
    buttons->addWidget(menu_);
    refresh_ = new QToolButton;
    refresh_->setAutoRaise(true);
    refresh_->setIcon(style()->standardIcon(QStyle::SP_BrowserReload));
    refresh_->setToolTip(tr("Uncached Refresh"));
    buttons->addWidget(refresh_);
    warning_ = new QToolButton;
    warning_->setAutoRaise(true);
    warning_->setIcon(style()->standardIcon(QStyle::SP_MessageBoxWarning));
    warning_->setToolTip(tr("Click to refresh the histogram from the image's own pixels (uncached)"));
    buttons->addWidget(warning_);
    buttons->addStretch();
    graph->addLayout(buttons, 0, 1, 2, 1);
    graph->setRowStretch(1, 1);
    layout->addLayout(graph, 1);

    sourceRow_ = new QWidget;
    auto* sourceLayout = new QHBoxLayout(sourceRow_);
    sourceLayout->setContentsMargins(0, 0, 0, 0);
    sourceLayout->addWidget(new QLabel(tr("Source:")));
    source_ = new QComboBox;
    source_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    source_->setMinimumContentsLength(8);
    source_->addItem(tr("Entire Image"), int(Source::EntireImage));
    source_->addItem(tr("Selected Layer"), int(Source::SelectedLayer));
    source_->addItem(tr("Adjustment Composite"), int(Source::AdjustmentComposite));
    sourceLayout->addWidget(source_, 1);
    layout->addWidget(sourceRow_);

    stats_ = new QWidget;
    auto* grid = new QGridLayout(stats_);
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setHorizontalSpacing(6);
    grid->setVerticalSpacing(1);
    mean_ = statLabel(grid, 0, 0, tr("Mean:"));
    stdDev_ = statLabel(grid, 1, 0, tr("Std Dev:"));
    median_ = statLabel(grid, 2, 0, tr("Median:"));
    pixels_ = statLabel(grid, 3, 0, tr("Pixels:"));
    level_ = statLabel(grid, 0, 2, tr("Level:"));
    count_ = statLabel(grid, 1, 2, tr("Count:"));
    percentile_ = statLabel(grid, 2, 2, tr("Percentile:"));
    cacheLevel_ = statLabel(grid, 3, 2, tr("Cache Level:"));
    grid->setColumnStretch(1, 1);
    grid->setColumnStretch(3, 1);
    layout->addWidget(stats_);

    auto* menu = new QMenu(menu_);
    auto* compact = menu->addAction(tr("Compact View"), this, [this] { setExpanded(false); });
    auto* expandedAction = menu->addAction(tr("Expanded View"), this, [this] { setExpanded(true); });
    compact->setCheckable(true);
    expandedAction->setCheckable(true);
    menu->addSeparator();
    auto* stats = menu->addAction(tr("Show Statistics"), this, [this](bool on) {
        showStats_ = on;
        QSettings().setValue("histogram/statistics", on);
        setExpanded(expanded_);
    });
    stats->setCheckable(true);
    menu->addSeparator();
    menu->addAction(tr("Uncached Refresh"), this, [this] { start(true); });
    connect(menu, &QMenu::aboutToShow, this, [this, compact, expandedAction, stats] {
        compact->setChecked(!expanded_);
        expandedAction->setChecked(expanded_);
        stats->setChecked(showStats_);
        stats->setEnabled(expanded_);
    });
    menu_->setMenu(menu);

    connect(warning_, &QToolButton::clicked, this, [this] { start(true); });
    connect(refresh_, &QToolButton::clicked, this, [this] { start(true); });
    connect(channel_, QOverload<int>::of(&QComboBox::activated), this, [this] { refreshView(); refreshStats(); });
    connect(source_, QOverload<int>::of(&QComboBox::activated), this, [this] { schedule(); });
    connect(view_, &HistogramView::levelsHovered, this, [this](int from, int to) { from_ = from; to_ = to; refreshStats(); });

    throttle_.setSingleShot(true);
    connect(&throttle_, &QTimer::timeout, this, [this] { start(false); });
    rebuildChannels();
    setExpanded(expanded_);
}

HistogramPanel::~HistogramPanel() {
    if (worker_.joinable()) worker_.join();
}

void HistogramPanel::setExpanded(bool expanded) {
    expanded_ = expanded;
    QSettings().setValue("histogram/expanded", expanded);
    channelRow_->setVisible(expanded);
    sourceRow_->setVisible(expanded);
    refresh_->setVisible(expanded);
    stats_->setVisible(expanded && showStats_);
    // Compact View shows the colours together, as Photoshop's does.
    refreshView();
    refreshStats();
}

void HistogramPanel::setSession(EditorSession* session) {
    if (session_ == session) return;
    for (const auto& c : connections_) disconnect(c);
    connections_.clear();
    session_ = session;
    data_.reset();
    if (session_) {
        connections_ << connect(session_, &EditorSession::documentChanged, this, [this] { schedule(); });
        connections_ << connect(session_, &EditorSession::documentChangedAsShown, this, [this] { schedule(); });
        connections_ << connect(session_, &EditorSession::layersChanged, this, [this] {
            if (source_->currentData().toInt() != int(Source::EntireImage)) schedule();
        });
    }
    schedule();
}

void HistogramPanel::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    if (stale_) schedule();
}

void HistogramPanel::schedule() {
    stale_ = true;
    view_->setStale(bool(data_));
    refreshWarning();
    // Hidden (another tab in front, or closed): nothing is counted until it shows.
    if (!isVisible()) return;
    if (busy_) { pending_ = true; return; }
    if (!throttle_.isActive()) throttle_.start(throttleMs);
}

bool HistogramPanel::makeRequest(const EditorSession& session, Source source, bool uncached, Request& request) {
    const auto& document = session.document();
    if (!document) return false;
    request.mode = document->colorMode;
    request.curve = session.documentCurve();
    request.uncached = uncached;
    const Layer* layer = session.activeLayer();
    switch (source) {
    case Source::EntireImage:
        request.document = *document;
        return true;
    case Source::SelectedLayer:
        if (!layer || !layer->asset || !layer->asset->image) return false;
        request.layer = layer->asset->image;
        return true;
    case Source::AdjustmentComposite: {
        // The adjustment layer and everything beneath it (Photoshop greys this out for other layers).
        if (!layer || !layer->adjustment) return false;
        Document below = *document;
        const int index = below.indexOf(layer->id);
        if (index < 0) return false;
        below.layers.erase(below.layers.begin() + index + 1, below.layers.end());
        request.document = std::move(below);
        return true;
    }
    }
    return false;
}

ImageHistogram HistogramPanel::computeHistogram(const Request& request) {
    if (request.document) {
        const Document& document = *request.document;
        const int level = request.uncached ? 1 : histogramCacheLevel(document.width, document.height, cacheBudget);
        // A cache level renders the document reduced (through the layers' mips) instead of reducing a full render.
        const AnyImage image = renderComposite(document, 1.0 / double(1 << (level - 1)));
        ImageHistogram histogram = imageHistogram(image, request.mode, request.curve, 1);
        histogram.cacheLevel = level;
        return histogram;
    }
    if (!request.layer) return {};
    const int level = request.uncached ? 1 : histogramCacheLevel(request.layer.width(), request.layer.height(), cacheBudget);
    return imageHistogram(request.layer, request.mode, request.curve, level);
}

void HistogramPanel::start(bool uncached) {
    throttle_.stop();
    if (busy_) { pending_ = true; pendingUncached_ = pendingUncached_ || uncached; return; }
    Request request;
    const Source source = Source(source_->currentData().toInt());
    if (!session_ || !makeRequest(*session_, source, uncached, request)) {
        data_.reset();
        stale_ = false;
        refreshView();
        refreshStats();
        refreshWarning();
        return;
    }
    busy_ = true;
    const uint64_t generation = ++generation_;
    if (worker_.joinable()) worker_.join();
    worker_ = std::thread([this, request = std::move(request), generation] {
        ImageHistogram histogram = computeHistogram(request);
        QMetaObject::invokeMethod(this, [this, histogram = std::move(histogram), generation]() mutable { finished(std::move(histogram), generation); }, Qt::QueuedConnection);
    });
}

void HistogramPanel::finished(ImageHistogram histogram, uint64_t generation) {
    if (worker_.joinable()) worker_.join();
    busy_ = false;
    if (generation == generation_) {
        data_ = std::move(histogram);
        stale_ = pending_;
        if (session_ && session_->colorMode() != builtMode_) rebuildChannels();
        refreshView();
        refreshStats();
        refreshWarning();
    }
    if (pending_) {
        const bool uncached = pendingUncached_;
        pending_ = pendingUncached_ = false;
        if (uncached) start(true);
        else if (isVisible()) throttle_.start(throttleMs);
    }
}

void HistogramPanel::rebuildChannels() {
    const ColorMode mode = session_ ? session_->colorMode() : ColorMode::RGB;
    if (channelsBuilt_ && mode == builtMode_) return;
    const int previous = channel_->currentData().isValid() ? channel_->currentData().toInt() : 0;
    builtMode_ = mode;
    channelsBuilt_ = true;
    channel_->clear();
    // Photoshop's choices per mode: the composite and each channel, Luminosity in RGB, and Colors.
    for (int i = 0; i < levelsChannelCount; i++) {
        const QString name = mode == ColorMode::RGB ? (i < 4 ? names::levelsChannel(i) : QString()) : names::levelsChannel(i, mode);
        if (!name.isEmpty()) channel_->addItem(name, i);
    }
    if (mode == ColorMode::RGB) channel_->addItem(tr("Luminosity"), luminosityChannel);
    channel_->addItem(tr("Colors"), colorsChannel);
    const int index = channel_->findData(previous);
    channel_->setCurrentIndex(std::max(0, index));
}

const std::vector<double>& HistogramPanel::binsFor(const ImageHistogram& histogram, int channel) {
    static const std::vector<double> none;
    if (channel == luminosityChannel) return histogram.luminosity;
    if (channel == colorsChannel || channel == 0) {
        if (histogram.mode == ColorMode::Lab) return histogram.channels[1];
        return histogram.channels[0];
    }
    if (channel >= 1 && channel <= 4) return histogram.channels[size_t(channel)];
    return none;
}

void HistogramPanel::refreshView() {
    if (session_ && session_->colorMode() != builtMode_) rebuildChannels();
    if (!data_ || data_->empty()) { view_->setSeries({}); view_->setStale(false); return; }
    const int channel = expanded_ ? channel_->currentData().toInt() : colorsChannel;
    std::vector<HistogramView::Series> series;
    bool additive = true;
    if (channel == colorsChannel) {
        if (data_->mode == ColorMode::RGB) {
            series = {{data_->channels[1], QColor(255, 0, 0)}, {data_->channels[2], QColor(0, 255, 0)}, {data_->channels[3], QColor(0, 0, 255)}};
        } else if (data_->mode == ColorMode::CMYK) {
            additive = false;
            series = {{data_->channels[1], QColor(0, 200, 230)}, {data_->channels[2], QColor(230, 0, 150)}, {data_->channels[3], QColor(240, 220, 0)},
                      {data_->channels[4], QColor(150, 150, 150)}};
        } else {
            additive = false;
            series = {{data_->channels[1], QColor(200, 200, 200)}, {data_->channels[2], QColor(230, 60, 160)}, {data_->channels[3], QColor(230, 200, 40)}};
        }
    } else {
        QColor color(200, 200, 200);
        if (data_->mode == ColorMode::RGB && channel >= 1 && channel <= 3) color = channel == 1 ? QColor(230, 70, 70) : channel == 2 ? QColor(70, 210, 70) : QColor(90, 120, 255);
        if (data_->mode == ColorMode::CMYK && channel >= 1 && channel <= 3) color = channel == 1 ? QColor(0, 200, 230) : channel == 2 ? QColor(230, 0, 150) : QColor(240, 220, 0);
        series = {{binsFor(*data_, channel), color}};
    }
    view_->setSeries(std::move(series), additive);
    view_->setStale(stale_);
}

void HistogramPanel::refreshStats() {
    if (!stats_->isVisible() && !expanded_) return;
    const QString none;
    if (!data_ || data_->empty()) {
        for (QLabel* l : {mean_, stdDev_, median_, pixels_, level_, count_, percentile_, cacheLevel_}) l->setText(none);
        return;
    }
    const int channel = channel_->currentData().toInt();
    const auto& bins = binsFor(*data_, channel);
    const HistogramStats stats = histogramStats(bins);
    const QLocale locale;
    mean_->setText(locale.toString(stats.mean, 'f', 2));
    stdDev_->setText(locale.toString(stats.stdDev, 'f', 2));
    median_->setText(QString::number(stats.median));
    pixels_->setText(locale.toString(qlonglong(std::llround(stats.pixels))));
    cacheLevel_->setText(QString::number(data_->cacheLevel));
    if (from_ < 0) {
        for (QLabel* l : {level_, count_, percentile_}) l->setText(none);
        return;
    }
    level_->setText(from_ == to_ ? QString::number(from_) : QStringLiteral("%1..%2").arg(from_).arg(to_));
    count_->setText(locale.toString(qlonglong(std::llround(histogramCount(bins, from_, to_)))));
    percentile_->setText(locale.toString(histogramPercentile(bins, to_), 'f', 2));
}

void HistogramPanel::refreshWarning() {
    // Photoshop's triangle: the graph is from cached (reduced) data, or no longer matches the image.
    const bool cached = data_ && data_->cacheLevel > 1;
    warning_->setVisible(bool(session_ && session_->document()) && (cached || (stale_ && data_)));
}

} // namespace app
