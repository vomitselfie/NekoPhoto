// Window > Histogram, as Photoshop's panel: Compact View (the graph) and Expanded View (Channel, Source and the
// statistics). Counted on a worker thread from a reduced render (the cache level) after the document changes,
// throttled, so the canvas never waits for it; Uncached Refresh counts every pixel once.
#pragma once
#include "nekophoto/histogram.h"
#include <QPointer>
#include <QTimer>
#include <QWidget>
#include <atomic>
#include <optional>
#include <thread>

class QComboBox;
class QLabel;
class QToolButton;

namespace app {

class EditorSession;
class HistogramView;

class HistogramPanel : public QWidget {
    Q_OBJECT
public:
    /// What the histogram counts: Photoshop's Source menu.
    enum class Source { EntireImage, SelectedLayer, AdjustmentComposite };
    /// The channel shown: 0 the composite, 1 to 4 the channels, then Luminosity and Colors.
    static constexpr int luminosityChannel = 5, colorsChannel = 6;
    /// The most pixels a cached histogram counts; a larger image is reduced a cache level at a time.
    static constexpr double cacheBudget = 1 << 20;

    explicit HistogramPanel(QWidget* parent = nullptr);
    ~HistogramPanel() override;
    void setSession(EditorSession* session);
    bool expanded() const { return expanded_; }
    void setExpanded(bool expanded);
    /// Whether what it shows is counted and drawn: nothing waiting, nothing being counted, no newer change to count.
    bool settled() const { return !busy_ && !pending_ && !stale_ && !throttle_.isActive(); }

    /// What a request counts, taken on the UI thread; computeHistogram runs on any thread.
    struct Request {
        std::optional<nekophoto::Document> document;   // rendered (Entire Image, Adjustment Composite)
        nekophoto::AnyImage layer;                     // counted (Selected Layer)
        nekophoto::ColorMode mode = nekophoto::ColorMode::RGB;
        nekophoto::TransferCurve curve = nekophoto::TransferCurve::srgb();
        bool uncached = false;
    };
    /// Fills a request for `session`'s document and `source`; false when there is nothing to count (no document, no
    /// pixels in the active layer, no adjustment layer active for Adjustment Composite).
    static bool makeRequest(const EditorSession& session, Source source, bool uncached, Request& request);
    static nekophoto::ImageHistogram computeHistogram(const Request& request);
    /// The bins one channel code shows (Colors: the composite, or Lab's lightness).
    static const std::vector<double>& binsFor(const nekophoto::ImageHistogram& histogram, int channel);

protected:
    void showEvent(QShowEvent* event) override;

private:
    void schedule();
    void start(bool uncached);
    void finished(nekophoto::ImageHistogram histogram, uint64_t generation);
    void rebuildChannels();
    void refreshView();
    void refreshStats();
    void refreshWarning();

    QPointer<EditorSession> session_;
    QList<QMetaObject::Connection> connections_;
    HistogramView* view_ = nullptr;
    QComboBox* channel_ = nullptr;
    QComboBox* source_ = nullptr;
    QToolButton* warning_ = nullptr;
    QToolButton* refresh_ = nullptr;
    QToolButton* menu_ = nullptr;
    QWidget* channelRow_ = nullptr;
    QWidget* sourceRow_ = nullptr;
    QWidget* stats_ = nullptr;
    QLabel *mean_ = nullptr, *stdDev_ = nullptr, *median_ = nullptr, *pixels_ = nullptr;
    QLabel *level_ = nullptr, *count_ = nullptr, *percentile_ = nullptr, *cacheLevel_ = nullptr;
    QTimer throttle_;
    std::thread worker_;
    bool busy_ = false, pending_ = false, pendingUncached_ = false, stale_ = true, expanded_ = false, showStats_ = true;
    uint64_t generation_ = 0;
    std::optional<nekophoto::ImageHistogram> data_;
    nekophoto::ColorMode builtMode_ = nekophoto::ColorMode::RGB;
    bool channelsBuilt_ = false;
    int from_ = -1, to_ = -1;
};

} // namespace app
