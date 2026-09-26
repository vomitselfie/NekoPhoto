// Filter > Camera Raw Filter. The panels follow upstream Compositor's Compositor/UI/CameraRawControls.swift,
// CameraRawColorControls.swift, CameraRawDetailOpticsControls.swift, CameraRawGeometryCalibrationControls.swift
// and RawDevelopSheet.swift (MIT, see LICENSES/MIT-Compositor.txt); the grade itself is compositor/cameraraw.h.
#include "CameraRawDialog.h"
#include "Style.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSlider>
#include <QStackedWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <cmath>
#include <random>

using namespace compositor;

namespace app {

namespace {

/// Big enough for the radii to read as they will, small enough to follow a slider on a large photo.
constexpr int previewLimit = 1600;

/// The grade the last OK left, offered again on the next open as upstream does.
CameraRawSettings& remembered() {
    static CameraRawSettings settings;
    return settings;
}

QFormLayout* newForm(QWidget* parent) {
    auto* form = new QFormLayout(parent);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form->setRowWrapPolicy(QFormLayout::DontWrapRows);
    form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    return form;
}

/// A titled section of a page: a bold header row in the page's one form, so every slider lines up.
QFormLayout* section(QFormLayout* form, const QString& title) {
    auto* header = new QLabel(title);
    QFont font = header->font();
    font.setBold(true);
    header->setFont(font);
    if (form->rowCount() > 0) form->addItem(new QSpacerItem(0, 8));
    form->addRow(header);
    return form;
}

/// Wraps a page in a scroll area so the dialog keeps a modest height.
QWidget* scrolled(QWidget* page) {
    auto* scroll = new QScrollArea;
    scroll->setWidget(page);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    return scroll;
}

} // namespace

CameraRawDialog::CameraRawDialog(EditorSession* session, QWidget* parent)
    : PixelDialog(session, parent), settings_(remembered()), seed_(uint32_t(std::random_device{}())) {
    setWindowTitle(tr("Camera Raw Filter"));
    setMinimumWidth(440);
    resize(540, 640);
    debounce_ = new QTimer(this);
    debounce_->setSingleShot(true);
    debounce_->setInterval(15);
    connect(debounce_, &QTimer::timeout, this, [this] { refreshPreview(); });

    auto* layout = new QVBoxLayout(this);
    // Upstream's panel column: a list of the panels beside the one shown, which stays narrow where eight tabs would not.
    auto* body = new QHBoxLayout;
    auto* panels = new QListWidget;
    panels->setObjectName("cameraRawPanels");
    auto* stack = new QStackedWidget;
    const std::pair<QString, QWidget*> pages[] = {
        {tr("Basic"), basicPage()}, {tr("Curve"), curvePage()}, {tr("Detail"), detailPage()}, {tr("Color"), colorPage()},
        {tr("Optics"), opticsPage()}, {tr("Geometry"), geometryPage()}, {tr("Effects"), effectsPage()}, {tr("Calibration"), calibrationPage()},
    };
    for (const auto& [name, page] : pages) {
        panels->addItem(name);
        stack->addWidget(scrolled(page));
    }
    panels->item(3)->setToolTip(tr("Color Mixer and Color Grading"));
    panels->setFixedWidth(panels->sizeHintForColumn(0) + 2 * panels->frameWidth() + 16);
    panels->setCurrentRow(0);
    connect(panels, &QListWidget::currentRowChanged, stack, &QStackedWidget::setCurrentIndex);
    body->addWidget(panels);
    body->addWidget(stack, 1);
    layout->addLayout(body, 1);

    auto* resetRow = new QHBoxLayout;
    auto* reset = new QPushButton(tr("Reset All"));
    reset->setToolTip(tr("Put every panel back to its defaults"));
    connect(reset, &QPushButton::clicked, this, [this] { settings_ = CameraRawSettings(); sync(); settingsChanged(); });
    resetRow->addWidget(reset);
    resetRow->addStretch(1);
    layout->addLayout(resetRow);
    connect(addPreviewAndButtons(layout), &QCheckBox::toggled, this, [this] { refreshPreview(); });

    capture(0, previewLimit);
    sync();
    refreshPreview();
}

// ---- controls ---------------------------------------------------------------------------------------

void CameraRawDialog::slider(QFormLayout* form, const QString& label, double min, double max, double step, std::function<double&()> value,
                             std::function<void()> changed) {
    auto* row = new QHBoxLayout;
    auto* s = new QSlider(Qt::Horizontal);
    s->setRange(int(std::lround(min / step)), int(std::lround(max / step)));
    s->setMinimumWidth(120);
    auto* spin = new QDoubleSpinBox;
    spin->setRange(min, max);
    spin->setSingleStep(step);
    spin->setDecimals(step < 1 ? (step < 0.1 ? 2 : 1) : 0);
    spin->setKeyboardTracking(false);
    spin->setButtonSymbols(QAbstractSpinBox::NoButtons);
    spin->setAlignment(Qt::AlignRight);
    spin->setFixedWidth(56);
    row->addWidget(s, 1);
    row->addWidget(spin);
    auto set = [this, value, changed](double v) {
        if (syncing_) return;
        value() = v;
        if (changed) changed();
        settingsChanged();
    };
    connect(s, &QSlider::valueChanged, this, [spin, set, step](int v) { { QSignalBlocker b(spin); spin->setValue(v * step); } set(v * step); });
    connect(spin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [s, set, step](double v) { { QSignalBlocker b(s); s->setValue(int(std::lround(v / step))); } set(v); });
    syncers_.push_back([s, spin, value, step] { QSignalBlocker a(s), b(spin); s->setValue(int(std::lround(value() / step))); spin->setValue(value()); });
    form->addRow(label, row);
}

QCheckBox* CameraRawDialog::check(QFormLayout* form, const QString& label, std::function<bool&()> value) {
    auto* c = new QCheckBox(label);
    connect(c, &QCheckBox::toggled, this, [this, value](bool on) { if (syncing_) return; value() = on; settingsChanged(); });
    syncers_.push_back([c, value] { QSignalBlocker b(c); c->setChecked(value()); });
    form->addRow(QString(), c);
    return c;
}

QComboBox* CameraRawDialog::choice(QFormLayout* form, const QString& label, const QStringList& items, std::function<int()> get, std::function<void(int)> set) {
    auto* combo = new QComboBox;
    combo->addItems(items);
    connect(combo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this, set](int index) { if (!syncing_ && index >= 0) set(index); });
    syncers_.push_back([combo, get] { QSignalBlocker b(combo); combo->setCurrentIndex(get()); });
    form->addRow(label, combo);
    return combo;
}

void CameraRawDialog::sync() {
    syncing_ = true;
    for (auto& s : syncers_) s();
    syncing_ = false;
}

void CameraRawDialog::settingsChanged() {
    if (!syncing_) debounce_->start();
}

// ---- panels -----------------------------------------------------------------------------------------

QWidget* CameraRawDialog::basicPage() {
    auto* page = new QWidget;
    QFormLayout* v = newForm(page);
    CameraRawSettings& s = settings_;

    QFormLayout* white = section(v, tr("White Balance"));
    whiteBalance_ = choice(white, tr("Mode"), {tr("Custom"), tr("Auto")},
        [&s] { return int(s.whiteBalance); },
        [this, &s](int index) {
            s.whiteBalance = CameraRawWhiteBalance(index);
            // Auto: the gray-world balance of the layer's covered pixels fills Temperature and Tint.
            if (s.whiteBalance == CameraRawWhiteBalance::Auto && source())
                if (auto solved = CameraRawSettings::autoBalance(*source())) {
                    s.temperature = std::clamp((*solved)[0], -100.0, 100.0);
                    s.tint = std::clamp((*solved)[1], -100.0, 100.0);
                }
            sync();
            settingsChanged();
        });
    auto toCustom = [&s, this] { s.whiteBalance = CameraRawWhiteBalance::Custom; if (whiteBalance_) { QSignalBlocker b(whiteBalance_); whiteBalance_->setCurrentIndex(0); } };
    slider(white, tr("Temperature"), -100, 100, 1, [&s]() -> double& { return s.temperature; }, toCustom);
    slider(white, tr("Tint"), -100, 100, 1, [&s]() -> double& { return s.tint; }, toCustom);

    QFormLayout* light = section(v, tr("Light"));
    slider(light, tr("Exposure"), -5, 5, 0.05, [&s]() -> double& { return s.exposure; });
    slider(light, tr("Contrast"), -100, 100, 1, [&s]() -> double& { return s.contrast; });
    slider(light, tr("Highlights"), -100, 100, 1, [&s]() -> double& { return s.highlights; });
    slider(light, tr("Shadows"), -100, 100, 1, [&s]() -> double& { return s.shadows; });
    slider(light, tr("Whites"), -100, 100, 1, [&s]() -> double& { return s.whites; });
    slider(light, tr("Blacks"), -100, 100, 1, [&s]() -> double& { return s.blacks; });
    check(light, tr("Show shadow clipping (blue)"), [this]() -> bool& { return preview_.shadowClipIndicator; });
    check(light, tr("Show highlight clipping (red)"), [this]() -> bool& { return preview_.highlightClipIndicator; });

    QFormLayout* presence = section(v, tr("Presence"));
    slider(presence, tr("Texture"), -100, 100, 1, [&s]() -> double& { return s.texture; });
    slider(presence, tr("Clarity"), -100, 100, 1, [&s]() -> double& { return s.clarity; });
    slider(presence, tr("Dehaze"), -100, 100, 1, [&s]() -> double& { return s.dehaze; });
    slider(presence, tr("Vibrance"), -100, 100, 1, [&s]() -> double& { return s.vibrance; });
    slider(presence, tr("Saturation"), -100, 100, 1, [&s]() -> double& { return s.saturation; });
    return page;
}

QWidget* CameraRawDialog::curvePage() {
    auto* page = new QWidget;
    QFormLayout* v = newForm(page);
    CameraRawCurveSettings& c = settings_.curve;
    QFormLayout* parametric = section(v, tr("Parametric Curve"));
    slider(parametric, tr("Highlights"), -100, 100, 1, [&c]() -> double& { return c.highlights; });
    slider(parametric, tr("Lights"), -100, 100, 1, [&c]() -> double& { return c.lights; });
    slider(parametric, tr("Darks"), -100, 100, 1, [&c]() -> double& { return c.darks; });
    slider(parametric, tr("Shadows"), -100, 100, 1, [&c]() -> double& { return c.shadows; });
    // The dividers stay in order: each is clamped against its neighbours as upstream's normalization does.
    auto ordered = [this, &c] { c = c.normalized(); sync(); };
    slider(parametric, tr("Shadow split"), 5, 90, 1, [&c]() -> double& { return c.shadowSplit; }, ordered);
    slider(parametric, tr("Dark split"), 7, 95, 1, [&c]() -> double& { return c.darkSplit; }, ordered);
    slider(parametric, tr("Light split"), 9, 98, 1, [&c]() -> double& { return c.lightSplit; }, ordered);

    QFormLayout* point = section(v, tr("Point Curve"));
    choice(point, tr("RGB curve"), {tr("Linear"), tr("Medium Contrast"), tr("Strong Contrast"), tr("Custom")},
        [&c] {
            if (CameraRawCurveSettings::isLinear(c.rgb)) return 0;
            if (c.rgb == CameraRawCurveSettings::mediumContrast()) return 1;
            if (c.rgb == CameraRawCurveSettings::strongContrast()) return 2;
            return 3;
        },
        [this, &c](int index) {
            if (index == 0) c.rgb = CameraRawCurveSettings::linear();
            else if (index == 1) c.rgb = CameraRawCurveSettings::mediumContrast();
            else if (index == 2) c.rgb = CameraRawCurveSettings::strongContrast();
            else { sync(); return; }   // Custom curves come from automation (pixels.cameraRaw)
            settingsChanged();
        });
    slider(point, tr("Refine saturation"), -100, 100, 1, [&c]() -> double& { return c.refineSaturation; });
    auto* note = new QLabel(tr("Red, green and blue point curves are set through automation (pixels.cameraRaw)."));
    note->setWordWrap(true);
    note->setStyleSheet(hintStyle());
    point->addRow(note);
    return page;
}

QWidget* CameraRawDialog::detailPage() {
    auto* page = new QWidget;
    QFormLayout* v = newForm(page);
    CameraRawDetailSettings& d = settings_.detail;
    QFormLayout* sharpen = section(v, tr("Sharpening"));
    slider(sharpen, tr("Amount"), 0, 150, 1, [&d]() -> double& { return d.sharpenAmount; });
    slider(sharpen, tr("Radius"), 0, 100, 1, [&d]() -> double& { return d.sharpenRadius; });
    slider(sharpen, tr("Detail"), 0, 100, 1, [&d]() -> double& { return d.sharpenDetail; });
    slider(sharpen, tr("Masking"), 0, 100, 1, [&d]() -> double& { return d.sharpenMasking; });
    check(sharpen, tr("Show the sharpening mask"), [this]() -> bool& { return preview_.sharpenMask; });
    QFormLayout* noise = section(v, tr("Noise Reduction"));
    slider(noise, tr("Luminance"), 0, 100, 1, [&d]() -> double& { return d.noiseLuminance; });
    slider(noise, tr("Detail"), 0, 100, 1, [&d]() -> double& { return d.noiseLuminanceDetail; });
    slider(noise, tr("Contrast"), 0, 100, 1, [&d]() -> double& { return d.noiseLuminanceContrast; });
    slider(noise, tr("Color"), 0, 100, 1, [&d]() -> double& { return d.noiseColor; });
    slider(noise, tr("Color detail"), 0, 100, 1, [&d]() -> double& { return d.noiseColorDetail; });
    slider(noise, tr("Smoothness"), 0, 100, 1, [&d]() -> double& { return d.noiseColorSmoothness; });
    return page;
}

QWidget* CameraRawDialog::colorPage() {
    auto* page = new QWidget;
    QFormLayout* v = newForm(page);
    CameraRawMixerSettings& m = settings_.mixer;
    QFormLayout* mixer = section(v, tr("Color Mixer"));
    choice(mixer, tr("Adjust"), {tr("Hue"), tr("Saturation"), tr("Luminance")}, [this] { return mixerChannel_; },
           [this](int index) { mixerChannel_ = index; sync(); });
    for (size_t i = 0; i < 8; i++)
        slider(mixer, tr(CameraRawMixerSettings::names[i]), -100, 100, 1, [this, &m, i]() -> double& {
            return mixerChannel_ == 0 ? m.hue[i] : mixerChannel_ == 1 ? m.saturation[i] : m.luminance[i];
        });

    CameraRawGradingSettings& g = settings_.grading;
    auto wheel = [this, &g]() -> CameraRawGradeWheel& {
        return gradeWheel_ == 0 ? g.shadows : gradeWheel_ == 1 ? g.midtones : gradeWheel_ == 2 ? g.highlights : g.global;
    };
    QFormLayout* grading = section(v, tr("Color Grading"));
    choice(grading, tr("Wheel"), {tr("Shadows"), tr("Midtones"), tr("Highlights"), tr("Global")}, [this] { return gradeWheel_; },
           [this](int index) { gradeWheel_ = index; sync(); });
    slider(grading, tr("Hue"), 0, 360, 1, [wheel]() -> double& { return wheel().hue; });
    slider(grading, tr("Saturation"), 0, 100, 1, [wheel]() -> double& { return wheel().saturation; });
    slider(grading, tr("Luminance"), -100, 100, 1, [wheel]() -> double& { return wheel().luminance; });
    slider(grading, tr("Blending"), 0, 100, 1, [&g]() -> double& { return g.blending; });
    slider(grading, tr("Balance"), -100, 100, 1, [&g]() -> double& { return g.balance; });
    return page;
}

QWidget* CameraRawDialog::opticsPage() {
    auto* page = new QWidget;
    QFormLayout* v = newForm(page);
    CameraRawOpticsSettings& o = settings_.optics;
    QFormLayout* lens = section(v, tr("Lens"));
    check(lens, tr("Remove chromatic aberration"), [&o]() -> bool& { return o.removeChromaticAberration; });
    check(lens, tr("Use profile corrections (generic)"), [&o]() -> bool& { return o.enableLensProfile; });
    slider(lens, tr("Profile distortion"), 0, 100, 1, [&o]() -> double& { return o.profileDistortion; });
    slider(lens, tr("Profile vignetting"), 0, 100, 1, [&o]() -> double& { return o.profileVignetting; });
    slider(lens, tr("Distortion"), -100, 100, 1, [&o]() -> double& { return o.distortion; });
    slider(lens, tr("Vignetting"), -100, 100, 1, [&o]() -> double& { return o.vignetteAmount; });
    slider(lens, tr("Midpoint"), 0, 100, 1, [&o]() -> double& { return o.vignetteMidpoint; });

    QFormLayout* defringe = section(v, tr("Defringe"));
    auto ordered = [this, &o] { o = o.normalized(); sync(); };
    slider(defringe, tr("Purple amount"), 0, 100, 1, [&o]() -> double& { return o.purpleAmount; });
    slider(defringe, tr("Purple hue from"), 0, 360, 1, [&o]() -> double& { return o.purpleHueLow; }, ordered);
    slider(defringe, tr("Purple hue to"), 0, 360, 1, [&o]() -> double& { return o.purpleHueHigh; }, ordered);
    slider(defringe, tr("Green amount"), 0, 100, 1, [&o]() -> double& { return o.greenAmount; });
    slider(defringe, tr("Green hue from"), 0, 360, 1, [&o]() -> double& { return o.greenHueLow; }, ordered);
    slider(defringe, tr("Green hue to"), 0, 360, 1, [&o]() -> double& { return o.greenHueHigh; }, ordered);
    return page;
}

QWidget* CameraRawDialog::geometryPage() {
    auto* page = new QWidget;
    QFormLayout* v = newForm(page);
    CameraRawGeometrySettings& g = settings_.geometry;
    QFormLayout* transform = section(v, tr("Transform"));
    choice(transform, tr("Projection"), {tr("Perspective"), tr("Rectilinear")}, [&g] { return int(g.projection); },
           [this, &g](int index) { g.projection = CameraRawProjection(index); settingsChanged(); });
    slider(transform, tr("Vertical"), -100, 100, 1, [&g]() -> double& { return g.vertical; });
    slider(transform, tr("Horizontal"), -100, 100, 1, [&g]() -> double& { return g.horizontal; });
    slider(transform, tr("Rotate"), -45, 45, 0.1, [&g]() -> double& { return g.rotate; });
    slider(transform, tr("Aspect"), -100, 100, 1, [&g]() -> double& { return g.aspect; });
    slider(transform, tr("Scale"), -100, 100, 1, [&g]() -> double& { return g.scale; });
    slider(transform, tr("Offset X"), -100, 100, 1, [&g]() -> double& { return g.offsetX; });
    slider(transform, tr("Offset Y"), -100, 100, 1, [&g]() -> double& { return g.offsetY; });
    check(transform, tr("Constrain crop"), [&g]() -> bool& { return g.constrainCrop; });
    auto* note = new QLabel(tr("Upright > Guided reads guide lines, which are set through automation (pixels.cameraRaw)."));
    note->setWordWrap(true);
    note->setStyleSheet(hintStyle());
    transform->addRow(note);
    return page;
}

QWidget* CameraRawDialog::effectsPage() {
    auto* page = new QWidget;
    QFormLayout* v = newForm(page);
    CameraRawSettings& s = settings_;
    QFormLayout* glow = section(v, tr("Glow"));
    slider(glow, tr("Amount"), 0, 100, 1, [&s]() -> double& { return s.glow; });
    choice(glow, tr("Style"), {tr("Diffusion"), tr("Bloom"), tr("Halation")}, [&s] { return int(s.glowStyle); },
           [this, &s](int index) { s.glowStyle = CameraRawGlowStyle(index); settingsChanged(); });
    slider(glow, tr("Range"), -100, 100, 1, [&s]() -> double& { return s.glowRange; });
    slider(glow, tr("Spread"), -100, 100, 1, [&s]() -> double& { return s.glowSpread; });
    slider(glow, tr("Warmth"), -100, 100, 1, [&s]() -> double& { return s.glowWarmth; });

    QFormLayout* vignette = section(v, tr("Vignette"));
    slider(vignette, tr("Amount"), -100, 100, 1, [&s]() -> double& { return s.vignetteAmount; });
    choice(vignette, tr("Style"), {tr("Highlight Priority"), tr("Color Priority"), tr("Paint Overlay")}, [&s] { return int(s.vignetteStyle); },
           [this, &s](int index) { s.vignetteStyle = CameraRawVignetteStyle(index); settingsChanged(); });
    slider(vignette, tr("Midpoint"), 0, 100, 1, [&s]() -> double& { return s.vignetteMidpoint; });
    slider(vignette, tr("Roundness"), -100, 100, 1, [&s]() -> double& { return s.vignetteRoundness; });
    slider(vignette, tr("Feather"), 0, 100, 1, [&s]() -> double& { return s.vignetteFeather; });
    slider(vignette, tr("Highlights"), 0, 100, 1, [&s]() -> double& { return s.vignetteHighlights; });

    QFormLayout* grain = section(v, tr("Grain"));
    slider(grain, tr("Amount"), 0, 100, 1, [&s]() -> double& { return s.grainAmount; });
    slider(grain, tr("Size"), 0, 100, 1, [&s]() -> double& { return s.grainSize; });
    slider(grain, tr("Roughness"), 0, 100, 1, [&s]() -> double& { return s.grainRoughness; });
    return page;
}

QWidget* CameraRawDialog::calibrationPage() {
    auto* page = new QWidget;
    QFormLayout* v = newForm(page);
    CameraRawCalibrationSettings& c = settings_.calibration;
    QFormLayout* process = section(v, tr("Process"));
    QStringList versions;
    for (int i = 1; i <= 6; i++) versions << tr("Version %1").arg(i);
    choice(process, tr("Process"), versions, [&c] { return c.process - 1; },
           [this, &c](int index) { c.process = index + 1; settingsChanged(); });
    slider(process, tr("Shadows tint"), -100, 100, 1, [&c]() -> double& { return c.shadowTint; });
    QFormLayout* primaries = section(v, tr("Primaries"));
    slider(primaries, tr("Red hue"), -100, 100, 1, [&c]() -> double& { return c.redHue; });
    slider(primaries, tr("Red saturation"), -100, 100, 1, [&c]() -> double& { return c.redSaturation; });
    slider(primaries, tr("Green hue"), -100, 100, 1, [&c]() -> double& { return c.greenHue; });
    slider(primaries, tr("Green saturation"), -100, 100, 1, [&c]() -> double& { return c.greenSaturation; });
    slider(primaries, tr("Blue hue"), -100, 100, 1, [&c]() -> double& { return c.blueHue; });
    slider(primaries, tr("Blue saturation"), -100, 100, 1, [&c]() -> double& { return c.blueSaturation; });
    return page;
}

// ---- preview and commit -----------------------------------------------------------------------------

std::shared_ptr<Image> CameraRawDialog::run(const Image& source, double scale, const CameraRawPreview& preview) const {
    auto out = std::make_shared<Image>(source);
    applyCameraRaw(*out, settings_, scale, seed_, preview);
    return out;
}

void CameraRawDialog::refreshPreview() {
    if (finished() || !previewSource()) return;
    const bool overlays = preview_.shadowClipIndicator || preview_.highlightClipIndicator || preview_.sharpenMask;
    if (!previewing() || (settings_.normalized().isIdentity() && !overlays)) { clearPreview(); return; }
    showPreview(run(*previewSource(), previewScale(), preview_), placement());
}

bool CameraRawDialog::apply() {
    remembered() = settings_;
    if (settings_.normalized().isIdentity()) return true;   // an unchanged grade is not an edit
    auto out = run(*source(), 1, {});
    throughSelection(*out);
    commit(out, placement(), tr("Camera Raw Filter"));
    return true;
}

} // namespace app
