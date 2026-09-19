#include "Style.h"
#include "AdjustmentEditor.h"
#include "EditorSession.h"
#include <QApplication>
#include <QButtonGroup>
#include <QCheckBox>
#include <QToolButton>
#include <QColorDialog>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QSlider>
#include <QVBoxLayout>
#include <cmath>

using namespace compositor;

namespace app {

// ---- Histogram --------------------------------------------------------------------------

class HistogramWidget : public QWidget {
public:
    explicit HistogramWidget(QWidget* parent = nullptr) : QWidget(parent) { setMinimumHeight(90); }
    void setData(const std::vector<double>& bins, const LevelsRange& range) { bins_ = bins; range_ = range; update(); }
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.fillRect(rect(), QColor(40, 40, 40));
        if (bins_.size() != 256) return;
        double peak = 0;
        std::vector<double> interior;
        for (size_t i = 1; i < 255; i++) if (bins_[i] > 0) interior.push_back(bins_[i]);
        for (double b : bins_) peak = std::max(peak, b);
        if (!interior.empty()) { std::sort(interior.begin(), interior.end()); peak = std::min(peak, interior[size_t((interior.size() - 1) * 0.95)] * 4); }
        if (peak <= 0) return;
        double w = width() / 256.0, h = height() - 14;
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(200, 200, 200));
        for (int i = 0; i < 256; i++) {
            double v = std::min(1.0, bins_[size_t(i)] / peak);
            p.drawRect(QRectF(i * w, h - v * h, std::max(1.0, w), v * h));
        }
        // Black, gamma and white markers.
        LevelsRange r = range_.normalized();
        double mid = r.black + (r.white - r.black) * std::pow(0.5, r.gamma);
        auto marker = [&](double level, QColor color) {
            double x = level / 255.0 * width();
            QPolygonF tri{{x, h}, {x - 5, height() - 1.0}, {x + 5, height() - 1.0}};
            p.setBrush(color); p.setPen(Qt::black); p.drawPolygon(tri);
        };
        marker(r.black, Qt::black); marker(mid, QColor(128, 128, 128)); marker(r.white, Qt::white);
    }
private:
    std::vector<double> bins_;
    LevelsRange range_;
};

// ---- Curve editor -------------------------------------------------------------------------

class CurveWidget : public QWidget {
public:
    std::function<void()> started, changed, finished;
    explicit CurveWidget(QWidget* parent = nullptr) : QWidget(parent) { setMinimumSize(200, 200); setCursor(Qt::CrossCursor); }
    void setSettings(const CurvesSettings& s) { settings_ = s; update(); }
    const CurvesSettings& settings() const { return settings_; }
protected:
    QRectF area() const { return QRectF(8, 8, width() - 16, height() - 16); }
    QPointF toView(double x, double y) const { QRectF a = area(); return {a.left() + x / 255 * a.width(), a.bottom() - y / 255 * a.height()}; }
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        QRectF a = area();
        p.fillRect(a, QColor(40, 40, 40));
        p.setPen(QColor(80, 80, 80));
        for (int i = 1; i < 4; i++) { p.drawLine(QPointF(a.left() + a.width() * i / 4, a.top()), QPointF(a.left() + a.width() * i / 4, a.bottom())); p.drawLine(QPointF(a.left(), a.top() + a.height() * i / 4), QPointF(a.right(), a.top() + a.height() * i / 4)); }
        p.setPen(QColor(120, 120, 120)); p.drawLine(toView(0, 0), toView(255, 255));
        static const QColor colors[4] = {QColor(230, 230, 230), QColor(230, 80, 80), QColor(80, 220, 80), QColor(90, 120, 255)};
        for (int c = 3; c >= 0; c--) {
            if (c != settings_.channel && settings_.channels[size_t(c)].size() == 2 && settings_.channels[size_t(c)][0].y == 0 && settings_.channels[size_t(c)][1].y == 255) continue;
            QPolygonF poly;
            for (int x = 0; x <= 255; x++) poly << toView(x, settings_.value(x, c));
            p.setPen(QPen(colors[c], c == settings_.channel ? 2 : 1));
            p.drawPolyline(poly);
        }
        p.setBrush(Qt::white); p.setPen(Qt::black);
        for (auto& pt : settings_.channels[size_t(settings_.channel)]) p.drawEllipse(toView(pt.x, pt.y), 4, 4);
    }
    int hit(QPointF v) const {
        auto& pts = settings_.channels[size_t(settings_.channel)];
        for (size_t i = 0; i < pts.size(); i++) { QPointF q = toView(pts[i].x, pts[i].y); if (std::hypot(q.x() - v.x(), q.y() - v.y()) <= 7) return int(i); }
        return -1;
    }
    void mousePressEvent(QMouseEvent* e) override {
        auto& pts = settings_.channels[size_t(settings_.channel)];
        dragging_ = hit(e->position());
        if (dragging_ < 0 && pts.size() < 32) {
            QRectF a = area();
            double x = std::clamp((e->position().x() - a.left()) / a.width() * 255, 1.0, 254.0);
            double y = settings_.value(x, settings_.channel);
            size_t insert = 0;
            for (size_t i = 0; i < pts.size(); i++) if (pts[i].x < x) insert = i + 1;
            if (insert == 0 || insert >= pts.size() || pts[insert - 1].x >= x - 0.5 || pts[insert].x <= x + 0.5) return;
            pts.insert(pts.begin() + long(insert), {x, y});
            dragging_ = int(insert);
        }
        if (dragging_ >= 0 && started) started();
        update();
    }
    void mouseMoveEvent(QMouseEvent* e) override {
        if (dragging_ < 0) return;
        auto& pts = settings_.channels[size_t(settings_.channel)];
        QRectF a = area();
        double y = std::clamp((a.bottom() - e->position().y()) / a.height() * 255, 0.0, 255.0);
        double x = std::clamp((e->position().x() - a.left()) / a.width() * 255, 0.0, 255.0);
        size_t i = size_t(dragging_);
        if (i == 0) x = 0; else if (i == pts.size() - 1) x = 255;
        else x = std::clamp(x, pts[i - 1].x + 1, pts[i + 1].x - 1);
        pts[i] = {x, y};
        if (changed) changed();
        update();
    }
    void mouseReleaseEvent(QMouseEvent*) override { if (dragging_ >= 0 && finished) finished(); dragging_ = -1; }
    void mouseDoubleClickEvent(QMouseEvent* e) override {
        int i = hit(e->position());
        auto& pts = settings_.channels[size_t(settings_.channel)];
        if (i > 0 && size_t(i) < pts.size() - 1) { if (started) started(); pts.erase(pts.begin() + i); if (changed) changed(); if (finished) finished(); update(); }
    }
private:
    CurvesSettings settings_;
    int dragging_ = -1;
};

// ---- Editor ----------------------------------------------------------------------------------

AdjustmentEditor::AdjustmentEditor(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 6, 6, 6);
    rebuild();
}

void AdjustmentEditor::setSettings(const AdjustmentSettings& settings) {
    bool sameKind = settings.kind == settings_.kind && body_;
    settings_ = settings;
    if (!sameKind) rebuild(); else sync();
}

void AdjustmentEditor::setHistogram(const std::array<std::vector<double>, 4>& histogram) {
    histogramData_ = histogram;
    sync();
}

void AdjustmentEditor::changed() {
    if (syncing_) return;
    emit settingsChanged(settings_);
    sync();
}

QWidget* AdjustmentEditor::sliderRow(const QString& label, double min, double max, int decimals, double scale, std::function<double()> get, std::function<void(double)> apply) {
    auto* row = new QWidget;
    auto* h = new QHBoxLayout(row);
    h->setContentsMargins(0, 0, 0, 0);
    auto* name = new QLabel(label);
    name->setMinimumWidth(80);
    h->addWidget(name);
    auto* slider = new QSlider(Qt::Horizontal);
    slider->setRange(int(std::round(min * scale)), int(std::round(max * scale)));
    h->addWidget(slider, 1);
    auto* spin = new QDoubleSpinBox;
    spin->setRange(min, max);
    spin->setDecimals(decimals);
    spin->setKeyboardTracking(false);
    spin->setFixedWidth(84);
    h->addWidget(spin);
    connect(slider, &QSlider::sliderPressed, this, [this] { emit editStarted(); });
    connect(slider, &QSlider::sliderReleased, this, [this] { emit editFinished(); });
    connect(slider, &QSlider::valueChanged, this, [this, spin, apply, scale](int v) {
        if (syncing_) return;
        double value = v / scale;
        { QSignalBlocker b(spin); spin->setValue(value); }
        apply(value);
        changed();
    });
    connect(spin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this, slider, apply, scale](double v) {
        if (syncing_) return;
        { QSignalBlocker b(slider); slider->setValue(int(std::round(v * scale))); }
        emit editStarted();
        apply(v);
        changed();
        emit editFinished();
    });
    syncers_.push_back([slider, spin, get, scale] {
        QSignalBlocker a(slider), b(spin);
        slider->setValue(int(std::round(get() * scale)));
        spin->setValue(get());
    });
    return row;
}

void AdjustmentEditor::rebuild() {
    syncers_.clear();
    histogram_ = nullptr;
    curve_ = nullptr;
    if (body_) { layout()->removeWidget(body_); body_->deleteLater(); body_ = nullptr; }
    switch (settings_.kind) {
    case AdjustmentKind::Levels: body_ = buildLevels(); break;
    case AdjustmentKind::Curves: body_ = buildCurves(); break;
    case AdjustmentKind::HueSaturation: body_ = buildHsv(); break;
    case AdjustmentKind::Exposure: body_ = buildExposure(); break;
    case AdjustmentKind::GradientMap: body_ = buildGradientMap(); break;
    case AdjustmentKind::Grain: body_ = buildGrain(); break;
    }
    layout()->addWidget(body_);
    sync();
}

void AdjustmentEditor::sync() {
    syncing_ = true;
    for (auto& s : syncers_) s();
    if (histogram_) histogram_->setData(histogramData_[size_t(settings_.levels.channel)], settings_.levels.ranges[size_t(settings_.levels.channel)]);
    if (curve_) curve_->setSettings(settings_.curves);
    syncing_ = false;
}

QWidget* AdjustmentEditor::buildLevels() {
    auto* w = new QWidget;
    auto* v = new QVBoxLayout(w);
    v->setContentsMargins(0, 0, 0, 0);
    auto* channelRow = new QHBoxLayout;
    channelRow->addWidget(new QLabel(tr("Channel")));
    auto* channel = new QComboBox;
    for (int i = 0; i < 4; i++) channel->addItem(QString::fromUtf8(levelsChannelName(i)));
    connect(channel, QOverload<int>::of(&QComboBox::activated), this, [this](int i) { settings_.levels.channel = i; changed(); });
    syncers_.push_back([this, channel] { channel->setCurrentIndex(settings_.levels.channel); });
    channelRow->addWidget(channel, 1);
    v->addLayout(channelRow);
    histogram_ = new HistogramWidget;
    v->addWidget(histogram_);
    auto range = [this]() -> LevelsRange& { return settings_.levels.ranges[size_t(settings_.levels.channel)]; };
    auto normalize = [this] { auto& r = settings_.levels.ranges[size_t(settings_.levels.channel)]; r = r.normalized(); };
    v->addWidget(sliderRow(tr("Input black"), 0, 254, 0, 1, [range] { return range().black; }, [range, normalize](double x) { range().black = x; normalize(); }));
    v->addWidget(sliderRow(tr("Gamma"), 0.1, 9.99, 2, 100, [range] { return range().gamma; }, [range, normalize](double x) { range().gamma = x; normalize(); }));
    v->addWidget(sliderRow(tr("Input white"), 1, 255, 0, 1, [range] { return range().white; }, [range, normalize](double x) { range().white = x; normalize(); }));
    v->addWidget(sliderRow(tr("Output black"), 0, 255, 0, 1, [range] { return range().outputBlack; }, [range](double x) { range().outputBlack = x; }));
    v->addWidget(sliderRow(tr("Output white"), 0, 255, 0, 1, [range] { return range().outputWhite; }, [range](double x) { range().outputWhite = x; }));
    auto* buttons = new QHBoxLayout;
    auto autoButton = [&](const QString& label, LevelsAuto mode) {
        auto* b = new QPushButton(label);
        connect(b, &QPushButton::clicked, this, [this, mode] { emit editStarted(); settings_.levels = autoLevels(mode, histogramData_); changed(); emit editFinished(); });
        buttons->addWidget(b);
    };
    autoButton(tr("Auto Contrast"), LevelsAuto::Contrast);
    autoButton(tr("Auto Color"), LevelsAuto::Color);
    autoButton(tr("Auto + Neutral"), LevelsAuto::Neutral);
    auto* reset = new QPushButton(tr("Reset"));
    connect(reset, &QPushButton::clicked, this, [this] { emit editStarted(); settings_.levels = LevelsSettings(); changed(); emit editFinished(); });
    buttons->addWidget(reset);
    v->addLayout(buttons);
    // The samplers: click the image to set the black, gray or white point from that colour.
    auto* samplers = new QHBoxLayout;
    samplers->addWidget(new QLabel(tr("Sample")));
    auto* group = new QButtonGroup(w);
    group->setExclusive(false);
    auto sampler = [&](const QString& text, LevelsSample mode) {
        auto* b = new QToolButton;
        b->setText(text);
        b->setCheckable(true);
        b->setToolTip(tr("Click the image to set the %1 point").arg(text.toLower()));
        connect(b, &QToolButton::toggled, this, [this, b, group, mode](bool on) {
            if (on) for (auto* other : group->buttons()) if (other != b) other->setChecked(false);
            levelsSample_ = on ? std::optional(mode) : std::nullopt;
            if (on) installLevelsHook(); else if (session_) session_->canvasPressHook = nullptr;
        });
        group->addButton(b);
        samplers->addWidget(b);
    };
    sampler(tr("Black"), LevelsSample::Black);
    sampler(tr("Gray"), LevelsSample::Gray);
    sampler(tr("White"), LevelsSample::White);
    samplers->addStretch();
    v->addLayout(samplers);
    return w;
}

QWidget* AdjustmentEditor::buildCurves() {
    auto* w = new QWidget;
    auto* v = new QVBoxLayout(w);
    v->setContentsMargins(0, 0, 0, 0);
    auto* channelRow = new QHBoxLayout;
    channelRow->addWidget(new QLabel(tr("Channel")));
    auto* channel = new QComboBox;
    for (int i = 0; i < 4; i++) channel->addItem(QString::fromUtf8(levelsChannelName(i)));
    connect(channel, QOverload<int>::of(&QComboBox::activated), this, [this](int i) { settings_.curves.channel = i; changed(); });
    syncers_.push_back([this, channel] { channel->setCurrentIndex(settings_.curves.channel); });
    channelRow->addWidget(channel, 1);
    v->addLayout(channelRow);
    curve_ = new CurveWidget;
    curve_->started = [this] { emit editStarted(); };
    curve_->changed = [this] { settings_.curves = curve_->settings(); if (!syncing_) emit settingsChanged(settings_); };
    curve_->finished = [this] { emit editFinished(); };
    v->addWidget(curve_, 1);
    auto* hint = new QLabel(tr("Click the curve to add a point, drag to move, double-click to remove."));
    hint->setWordWrap(true);
    hint->setStyleSheet(hintStyle());
    v->addWidget(hint);
    auto* reset = new QPushButton(tr("Reset"));
    connect(reset, &QPushButton::clicked, this, [this] { emit editStarted(); settings_.curves = CurvesSettings(); changed(); emit editFinished(); });
    v->addWidget(reset);
    return w;
}

QWidget* AdjustmentEditor::buildHsv() {
    auto* w = new QWidget;
    auto* v = new QVBoxLayout(w);
    v->setContentsMargins(0, 0, 0, 0);
    auto* rangeRow = new QHBoxLayout;
    rangeRow->addWidget(new QLabel(tr("Range")));
    auto* range = new QComboBox;
    for (int i = 0; i < 7; i++) range->addItem(QString::fromUtf8(colorRangeName(i)));
    connect(range, QOverload<int>::of(&QComboBox::activated), this, [this](int i) { settings_.hsv.range = i; changed(); });
    syncers_.push_back([this, range] { range->setCurrentIndex(settings_.hsv.range); range->setEnabled(!settings_.hsv.colorize); });
    rangeRow->addWidget(range, 1);
    v->addLayout(rangeRow);
    auto current = [this]() -> RangeAdjustment& { return settings_.hsv.current(); };
    v->addWidget(sliderRow(tr("Hue"), -180, 360, 0, 1, [current] { return current().hue; }, [current](double x) { current().hue = x; }));
    v->addWidget(sliderRow(tr("Saturation"), -100, 100, 0, 1, [current] { return current().saturation; }, [current](double x) { current().saturation = x; }));
    v->addWidget(sliderRow(tr("Lightness"), -100, 100, 0, 1, [current] { return current().lightness; }, [current](double x) { current().lightness = x; }));
    auto* colorize = new QCheckBox(tr("Colorize"));
    connect(colorize, &QCheckBox::toggled, this, [this](bool on) {
        if (syncing_) return;
        emit editStarted();
        if (on) { HueSaturationSettings s = HueSaturationSettings::colorizeStart(); s.bands = settings_.hsv.bands; settings_.hsv = s; }
        else { settings_.hsv.colorize = false; settings_.hsv.adjustments.clear(); }
        changed();
        emit editFinished();
    });
    syncers_.push_back([this, colorize] { colorize->setChecked(settings_.hsv.colorize); });
    v->addWidget(colorize);
    auto* invert = new QCheckBox(tr("Invert range"));
    connect(invert, &QCheckBox::toggled, this, [this](bool on) { if (syncing_) return; emit editStarted(); settings_.hsv.invertRange = on; changed(); emit editFinished(); });
    syncers_.push_back([this, invert] { invert->setChecked(settings_.hsv.invertRange); invert->setEnabled(settings_.hsv.range != 0 && !settings_.hsv.colorize); });
    v->addWidget(invert);
    // Eyedroppers and the targeted-adjustment tool: they take the next canvas click(s).
    auto* tools = new QHBoxLayout;
    auto* group = new QButtonGroup(w);
    group->setExclusive(false);
    auto toolButton = [&](const QString& text, const QString& tip, int mode) {
        auto* b = new QToolButton;
        b->setText(text);
        b->setToolTip(tip);
        b->setCheckable(true);
        connect(b, &QToolButton::toggled, this, [this, mode, b, group](bool on) {
            if (on) { for (auto* other : group->buttons()) if (other != b) other->setChecked(false); }
            if (mode < 0) hueTargeting_ = on; else hueSampleMode_ = on ? mode : (hueSampleMode_ == mode ? 0 : hueSampleMode_);
            if (on) installHueHooks(); else if (!hueTargeting_ && hueSampleMode_ == 0) clearHueHooks();
        });
        group->addButton(b);
        tools->addWidget(b);
        return b;
    };
    toolButton(tr("Sample"), tr("Click the image to centre this range on that colour"), 1);
    toolButton(tr("+"), tr("Click the image to widen this range to include that colour"), 2);
    toolButton(tr("−"), tr("Click the image to narrow this range to exclude that colour"), 3);
    toolButton(tr("Targeted"), tr("Drag on the image: right raises saturation of the colour under the pointer, left lowers it (Ctrl: hue)"), -1);
    tools->addStretch();
    v->addLayout(tools);
    syncers_.push_back([this, group] { bool enabled = settings_.hsv.range != 0 && !settings_.hsv.colorize; for (auto* b : group->buttons()) if (b->text() != tr("Targeted")) b->setEnabled(enabled); });
    auto* reset = new QPushButton(tr("Reset"));
    connect(reset, &QPushButton::clicked, this, [this] { emit editStarted(); settings_.hsv = HueSaturationSettings(); changed(); emit editFinished(); });
    v->addWidget(reset);
    return w;
}

AdjustmentEditor::~AdjustmentEditor() { clearHueHooks(); }

void AdjustmentEditor::setSession(EditorSession* session) { session_ = session; }

void AdjustmentEditor::clearHueHooks() {
    if (!session_) return;
    session_->canvasPressHook = nullptr;
    session_->canvasDragHook = nullptr;
    session_->canvasReleaseHook = nullptr;
}

void AdjustmentEditor::installLevelsHook() {
    if (!session_) return;
    session_->canvasPressHook = [this](QPointF p) {
        if (settings_.kind != AdjustmentKind::Levels || !levelsSample_) return false;
        auto color = session_->compositeColorAt(p);
        if (!color) return false;
        emit editStarted();
        settings_.levels = sampleLevels(settings_.levels, color->redF(), color->greenF(), color->blueF(), *levelsSample_);
        changed();
        emit editFinished();
        return true;
    };
}

void AdjustmentEditor::installHueHooks() {
    if (!session_) return;
    auto sampledHue = [this](QPointF p) -> std::optional<double> {
        auto color = session_->compositeColorAt(p);
        if (!color || color->hsvSaturationF() <= 0.02) return std::nullopt;
        return color->hueF() * 360;
    };
    session_->canvasPressHook = [this, sampledHue](QPointF p) {
        if (settings_.kind != AdjustmentKind::HueSaturation) return false;
        auto hue = sampledHue(p);
        if (!hue) return false;
        if (hueTargeting_ && !settings_.hsv.colorize) {
            int best = 1;
            for (int r = 1; r <= 6; r++) if (settings_.hsv.weight(r, *hue) > settings_.hsv.weight(best, *hue)) best = r;
            settings_.hsv.range = best;
            RangeAdjustment a = settings_.hsv.currentValue();
            hueDrag_ = HueDrag{best, a.hue, a.saturation};
            emit editStarted();
            changed();
            return true;
        }
        if (hueSampleMode_ == 0 || settings_.hsv.range == 0 || settings_.hsv.colorize) return false;
        emit editStarted();
        HueBand band = settings_.hsv.band();
        if (hueSampleMode_ == 1) band = band.centered(*hue);
        else if (hueSampleMode_ == 2) band.include(*hue);
        else band.exclude(*hue);
        settings_.hsv.bands[settings_.hsv.range] = band;
        changed();
        emit editFinished();
        return true;
    };
    session_->canvasDragHook = [this](QPointF start, QPointF now) {
        if (!hueDrag_) return;
        double delta = (now.x() - start.x()) * (session_ ? session_->viewport.pointsPerPixel() : 1);
        RangeAdjustment& a = settings_.hsv.adjustments[hueDrag_->range];
        if (QApplication::keyboardModifiers() & Qt::ControlModifier) a.hue = std::min(180.0, std::max(-180.0, hueDrag_->hue + delta / 2));
        else a.saturation = std::min(100.0, std::max(-100.0, hueDrag_->saturation + delta / 2));
        changed();
    };
    session_->canvasReleaseHook = [this] { if (hueDrag_) { hueDrag_.reset(); emit editFinished(); } };
}

QWidget* AdjustmentEditor::buildExposure() {
    auto* w = new QWidget;
    auto* v = new QVBoxLayout(w);
    v->setContentsMargins(0, 0, 0, 0);
    v->addWidget(sliderRow(tr("Exposure"), -20, 20, 2, 100, [this] { return settings_.exposure.exposure; }, [this](double x) { settings_.exposure.exposure = x; }));
    v->addWidget(sliderRow(tr("Offset"), -0.5, 0.5, 4, 10000, [this] { return settings_.exposure.offset; }, [this](double x) { settings_.exposure.offset = x; }));
    v->addWidget(sliderRow(tr("Gamma"), 0.01, 9.99, 2, 100, [this] { return settings_.exposure.gamma; }, [this](double x) { settings_.exposure.gamma = x; }));
    auto* reset = new QPushButton(tr("Reset"));
    connect(reset, &QPushButton::clicked, this, [this] { emit editStarted(); settings_.exposure = ExposureSettings(); changed(); emit editFinished(); });
    v->addWidget(reset);
    return w;
}

QWidget* AdjustmentEditor::buildGradientMap() {
    auto* w = new QWidget;
    auto* v = new QVBoxLayout(w);
    v->setContentsMargins(0, 0, 0, 0);
    auto colorButton = [&](const QString& label, std::function<AdjustmentColor&()> color) {
        auto* row = new QHBoxLayout;
        row->addWidget(new QLabel(label));
        auto* b = new QPushButton;
        connect(b, &QPushButton::clicked, this, [this, color, b] {
            AdjustmentColor& c = color();
            QColor chosen = QColorDialog::getColor(QColor::fromRgbF(c.red, c.green, c.blue), this);
            if (!chosen.isValid()) return;
            emit editStarted();
            c = {chosen.redF(), chosen.greenF(), chosen.blueF()};
            changed();
            emit editFinished();
        });
        syncers_.push_back([color, b] { AdjustmentColor& c = color(); QColor q = QColor::fromRgbF(c.red, c.green, c.blue); b->setText(q.name()); b->setStyleSheet(QStringLiteral("background: %1; color: %2;").arg(q.name(), q.lightnessF() > 0.5 ? "black" : "white")); });
        row->addWidget(b, 1);
        v->addLayout(row);
    };
    colorButton(tr("Shadows"), [this]() -> AdjustmentColor& { return settings_.gradientMap.shadows; });
    colorButton(tr("Highlights"), [this]() -> AdjustmentColor& { return settings_.gradientMap.highlights; });
    auto* reversed = new QCheckBox(tr("Reverse"));
    connect(reversed, &QCheckBox::toggled, this, [this](bool on) { if (syncing_) return; emit editStarted(); settings_.gradientMap.reversed = on; changed(); emit editFinished(); });
    syncers_.push_back([this, reversed] { reversed->setChecked(settings_.gradientMap.reversed); });
    v->addWidget(reversed);
    return w;
}

QWidget* AdjustmentEditor::buildGrain() {
    auto* w = new QWidget;
    auto* v = new QVBoxLayout(w);
    v->setContentsMargins(0, 0, 0, 0);
    v->addWidget(sliderRow(tr("Amount"), 0, 100, 0, 1, [this] { return settings_.grain.amount; }, [this](double x) { settings_.grain.amount = x; }));
    v->addWidget(sliderRow(tr("Size"), 0.5, 20, 1, 10, [this] { return settings_.grain.size; }, [this](double x) { settings_.grain.size = x; }));
    v->addWidget(sliderRow(tr("Roughness"), 0, 100, 0, 1, [this] { return settings_.grain.roughness; }, [this](double x) { settings_.grain.roughness = x; }));
    auto* reseed = new QPushButton(tr("New Pattern"));
    connect(reseed, &QPushButton::clicked, this, [this] { emit editStarted(); settings_.grain.seed = uint32_t(std::rand()) ^ uint32_t(std::rand() << 16); changed(); emit editFinished(); });
    v->addWidget(reseed);
    return w;
}

} // namespace app
