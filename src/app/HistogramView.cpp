#include "HistogramView.h"
#include <QMouseEvent>
#include <QPainter>
#include <algorithm>
#include <cmath>

using namespace compositor;

namespace app {

HistogramView::HistogramView(QWidget* parent) : QWidget(parent) {
    setMinimumHeight(90);
    setMouseTracking(true);
}

void HistogramView::setData(const std::vector<double>& bins, const LevelsRange& range) {
    series_ = {{bins, QColor(200, 200, 200)}};
    additive_ = true;
    range_ = range;
    update();
}

void HistogramView::setSeries(std::vector<Series> series, bool additive) {
    series_ = std::move(series);
    additive_ = additive;
    range_.reset();
    update();
}

void HistogramView::setStale(bool stale) {
    if (stale_ == stale) return;
    stale_ = stale;
    update();
}

int HistogramView::levelAt(double x) const { return std::clamp(int(x / std::max(1, width()) * 256), 0, 255); }

void HistogramView::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.fillRect(rect(), QColor(40, 40, 40));
    // The scale: the tallest bar, unless a few spikes (often pure black or white) dwarf the rest.
    double peak = 0;
    std::vector<double> interior;
    for (const Series& s : series_) {
        if (s.bins.size() != 256) continue;
        for (size_t i = 1; i < 255; i++) if (s.bins[i] > 0) interior.push_back(s.bins[i]);
        for (double b : s.bins) peak = std::max(peak, b);
    }
    if (!interior.empty()) {
        std::nth_element(interior.begin(), interior.begin() + long((interior.size() - 1) * 0.95), interior.end());
        peak = std::min(peak, interior[size_t((interior.size() - 1) * 0.95)] * 4);
    }
    const double w = width() / 256.0, h = range_ ? height() - 14 : height();
    if (from_ >= 0) {
        const int lo = std::min(from_, to_), hi = std::max(from_, to_);
        p.fillRect(QRectF(lo * w, 0, (hi - lo + 1) * w, h), QColor(70, 70, 70));
    }
    if (peak > 0) {
        p.save();
        p.setPen(Qt::NoPen);
        if (series_.size() > 1 && additive_) p.setCompositionMode(QPainter::CompositionMode_Plus);
        for (const Series& s : series_) {
            if (s.bins.size() != 256) continue;
            QColor color = s.color;
            if (stale_) color = color.darker(160);
            if (series_.size() > 1 && !additive_) color.setAlpha(150);   // inks: each shows through the others
            p.setBrush(color);
            for (int i = 0; i < 256; i++) {
                const double v = std::min(1.0, s.bins[size_t(i)] / peak);
                if (v > 0) p.drawRect(QRectF(i * w, h - v * h, std::max(1.0, w), v * h));
            }
        }
        p.restore();
    }
    if (!range_) return;
    // Black, gamma and white markers.
    LevelsRange r = range_->normalized();
    double mid = r.black + (r.white - r.black) * std::pow(0.5, r.gamma);
    auto marker = [&](double level, QColor color) {
        double x = level / 255.0 * width();
        QPolygonF tri{{x, h}, {x - 5, height() - 1.0}, {x + 5, height() - 1.0}};
        p.setBrush(color); p.setPen(Qt::black); p.drawPolygon(tri);
    };
    marker(r.black, Qt::black); marker(mid, QColor(128, 128, 128)); marker(r.white, Qt::white);
}

void HistogramView::mousePressEvent(QMouseEvent* event) {
    if (range_ || event->button() != Qt::LeftButton) return;
    anchor_ = from_ = to_ = levelAt(event->position().x());
    emit levelsHovered(from_, to_);
    update();
}

void HistogramView::mouseMoveEvent(QMouseEvent* event) {
    if (range_) return;
    const int level = levelAt(event->position().x());
    if (anchor_ >= 0) { from_ = std::min(anchor_, level); to_ = std::max(anchor_, level); }
    else { from_ = to_ = level; }
    emit levelsHovered(from_, to_);
    update();
}

void HistogramView::mouseReleaseEvent(QMouseEvent*) { anchor_ = -1; }

void HistogramView::leaveEvent(QEvent*) {
    if (anchor_ >= 0) return;
    from_ = to_ = -1;
    emit levelsHovered(-1, -1);
    update();
}

} // namespace app
