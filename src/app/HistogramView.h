// A histogram graph: the Levels editor's (one channel with its black, gamma and white markers) and the Histogram
// panel's (one channel, or several overlaid in their colours), with the level under the pointer and a dragged range.
#pragma once
#include "compositor/adjustments.h"
#include <QColor>
#include <QWidget>
#include <optional>
#include <vector>

namespace app {

class HistogramView : public QWidget {
    Q_OBJECT
public:
    struct Series {
        std::vector<double> bins;
        QColor color;
    };
    explicit HistogramView(QWidget* parent = nullptr);
    /// One gray channel with the Levels markers under it.
    void setData(const std::vector<double>& bins, const compositor::LevelsRange& range);
    /// One or more channels; `additive` overlays them as light (Photoshop's Colors view of RGB), else as ink.
    void setSeries(std::vector<Series> series, bool additive = true);
    /// Draws the graph dimmed, as data that no longer matches the image.
    void setStale(bool stale);
    /// The level under the pointer, or the range dragged (-1 when neither).
    int levelFrom() const { return from_; }
    int levelTo() const { return to_; }

signals:
    void levelsHovered(int from, int to);

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;

private:
    int levelAt(double x) const;
    std::vector<Series> series_;
    bool additive_ = true;
    std::optional<compositor::LevelsRange> range_;
    bool stale_ = false;
    int from_ = -1, to_ = -1, anchor_ = -1;
};

} // namespace app
