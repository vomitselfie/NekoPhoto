#include "Style.h"
#include "FilterDialog.h"
#include "ModelStore.h"
#include <QCheckBox>
#include <QComboBox>
#include <QMessageBox>
#include <thread>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QSlider>
#include <QVBoxLayout>
#include <cmath>
#include <random>

using namespace compositor;

namespace app {

namespace {

constexpr int previewLimit = 2048;

} // namespace

// ---- Pixel adjustments ----------------------------------------------------------------------

PixelAdjustmentDialog::PixelAdjustmentDialog(EditorSession* session, AdjustmentKind kind, QWidget* parent)
    : PixelDialog(session, parent) {
    setWindowTitle(QString::fromUtf8(adjustmentKindName(kind)));
    auto* layout = new QVBoxLayout(this);
    editor_ = new AdjustmentEditor;
    editor_->setSession(session);
    AdjustmentSettings settings = AdjustmentSettings::defaults(kind);
    if (kind == AdjustmentKind::GradientMap) {
        settings.gradientMap.shadows = {session->foregroundColor.redF(), session->foregroundColor.greenF(), session->foregroundColor.blueF()};
        settings.gradientMap.highlights = {session->backgroundColor.redF(), session->backgroundColor.greenF(), session->backgroundColor.blueF()};
    }
    if (kind == AdjustmentKind::Grain) settings.grain.seed = uint32_t(std::random_device{}());
    editor_->setSettings(settings);
    layout->addWidget(editor_);
    QCheckBox* preview = addPreviewAndButtons(layout);

    // Levels and Hue/Saturation preview at full size (a lookup per pixel), Grain too (its texture is per pixel);
    // the rest from a reduced copy.
    capture(0, kind == AdjustmentKind::Grain ? 0 : (kind == AdjustmentKind::Levels || kind == AdjustmentKind::HueSaturation) ? 8000 : previewLimit);
    if (source() && kind == AdjustmentKind::Levels) editor_->setHistogram(levelsHistogram(*source(), coverage()));
    connect(editor_, &AdjustmentEditor::settingsChanged, this, [this] { refreshPreview(); });
    connect(preview, &QCheckBox::toggled, this, [this] { refreshPreview(); });
    refreshPreview();
}

std::shared_ptr<Image> PixelAdjustmentDialog::run(const Image& source, double scale) const {
    auto out = std::make_shared<Image>(source);
    AdjustmentSettings settings = editor_->settings();
    Rect region(0, 0, source.width(), source.height());
    applyAdjustment(settings, *out, region, scale);
    return out;
}

void PixelAdjustmentDialog::refreshPreview() {
    if (!previewSource()) return;
    if (!previewing() || editor_->settings().isIdentity()) { clearPreview(); return; }
    showPreview(run(*previewSource(), previewScale()));
}

bool PixelAdjustmentDialog::apply() {
    if (editor_->settings().isIdentity()) return true;
    auto out = run(*source(), 1);
    throughSelection(*out);
    commit(out, placement(), QString::fromUtf8(adjustmentKindName(editor_->settings().kind)));
    return true;
}

// ---- Filters ----------------------------------------------------------------------------------

FilterDialog::FilterDialog(EditorSession* session, FilterKind kind, QWidget* parent)
    : PixelDialog(session, parent), kind_(kind), seed_(uint32_t(std::random_device{}())) {
    setWindowTitle(QString::fromUtf8(filterKindName(kind)));
    setMinimumWidth(420);
    auto* layout = new QVBoxLayout(this);
    auto slider = [&](const QString& label, double min, double max, int decimals, double scale, std::function<double()> get, std::function<void(double)> apply) {
        auto* row = new QHBoxLayout;
        auto* name = new QLabel(label);
        name->setMinimumWidth(80);
        row->addWidget(name);
        auto* s = new QSlider(Qt::Horizontal);
        s->setRange(int(min * scale), int(max * scale));
        row->addWidget(s, 1);
        auto* spin = new QDoubleSpinBox;
        spin->setRange(min, max);
        spin->setDecimals(decimals);
        spin->setKeyboardTracking(false);
        spin->setButtonSymbols(QAbstractSpinBox::NoButtons);
        spin->setAlignment(Qt::AlignRight);
        spin->setFixedWidth(64);
        row->addWidget(spin);
        connect(s, &QSlider::valueChanged, this, [this, spin, apply, scale](int v) { { QSignalBlocker b(spin); spin->setValue(v / scale); } apply(v / scale); refreshPreview(); });
        connect(spin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this, s, apply, scale](double v) { { QSignalBlocker b(s); s->setValue(int(std::round(v * scale))); } apply(v); refreshPreview(); });
        syncers_.push_back([s, spin, get, scale] { QSignalBlocker a(s), b(spin); s->setValue(int(std::round(get() * scale))); spin->setValue(get()); });
        layout->addLayout(row);
    };
    auto check = [&](const QString& label, std::function<bool()> get, std::function<void(bool)> apply) {
        auto* c = new QCheckBox(label);
        c->setChecked(get());
        connect(c, &QCheckBox::toggled, this, [this, apply](bool on) { apply(on); refreshPreview(); });
        layout->addWidget(c);
    };
    switch (kind) {
    case FilterKind::GaussianBlur:
        slider(tr("Radius"), 0.1, 250, 1, 10, [this] { return settings_.radius; }, [this](double v) { settings_.radius = v; });
        break;
    case FilterKind::MotionBlur:
        slider(tr("Angle"), -90, 90, 0, 1, [this] { return settings_.angle; }, [this](double v) { settings_.angle = v; });
        slider(tr("Distance"), 1, 2000, 0, 1, [this] { return settings_.distance; }, [this](double v) { settings_.distance = v; });
        break;
    case FilterKind::AddNoise:
        slider(tr("Amount"), 0.1, 400, 1, 10, [this] { return settings_.amount; }, [this](double v) { settings_.amount = v; });
        check(tr("Gaussian"), [this] { return settings_.gaussian; }, [this](bool on) { settings_.gaussian = on; });
        check(tr("Monochromatic"), [this] { return settings_.monochromatic; }, [this](bool on) { settings_.monochromatic = on; });
        break;
    case FilterKind::LensCorrection:
        slider(tr("Remove Distortion"), -100, 100, 0, 1, [this] { return settings_.distortion; }, [this](double v) { settings_.distortion = v; });
        check(tr("Bicubic"), [this] { return settings_.bicubic; }, [this](bool on) { settings_.bicubic = on; });
        break;
    }
    for (auto& s : syncers_) s();
    connect(addPreviewAndButtons(layout), &QCheckBox::toggled, this, [this] { refreshPreview(); });
    refreshPreview();
}

void FilterDialog::prepareSource() {
    // A blur grows the layer by its reach; only ever grows, so easing the amount off rebuilds nothing.
    int margin = int(std::ceil(blurMargin(kind_, settings_)));
    if (source() && margin <= margin_) return;
    margin_ = std::max(margin_, margin);
    capture(margin_, kind_ == FilterKind::AddNoise ? 0 : previewLimit);
}

bool FilterDialog::identity() const { return kind_ == FilterKind::LensCorrection && settings_.normalized().distortion == 0; }

std::shared_ptr<Image> FilterDialog::run(const Image& source, double scale) const {
    auto out = std::make_shared<Image>(source);
    applyFilter(kind_, *out, settings_, scale, seed_);
    return out;
}

void FilterDialog::refreshPreview() {
    if (finished()) return;
    prepareSource();
    if (!previewSource()) return;
    if (!previewing() || identity()) { clearPreview(); return; }
    showPreview(run(*previewSource(), previewScale()), placement());
}

bool FilterDialog::apply() {
    if (identity()) return true;
    auto out = run(*source(), 1);
    throughSelection(*out);
    LayerTransform placed = placement();
    std::shared_ptr<const Image> image = out;
    if (kind_ == FilterKind::GaussianBlur || kind_ == FilterKind::MotionBlur) image = trimToPixels(*out, placement(), placed);
    commit(image, placed, QString::fromUtf8(filterKindName(kind_)));
    return true;
}

// ---- Remove Background -----------------------------------------------------------------------

BackgroundDialog::BackgroundDialog(EditorSession* session, QString modelPath, QString quickModelPath, QWidget* parent)
    : PixelDialog(session, parent), modelPath_(std::move(modelPath)) {
    setWindowTitle(tr("Remove Background"));
    capture(0, 0);
    // The matting band is worth a few percent of the short side on a big photo.
    const int mattingMax = source() ? std::max(40, int(std::lround(std::min(source()->width(), source()->height()) * 0.025))) : 40;
    auto* layout = new QVBoxLayout(this);
    auto* qualityRow = new QHBoxLayout;
    qualityRow->addWidget(new QLabel(tr("Quality")));
    auto* quality = new QComboBox;
    quality->addItems({tr("Basic"), tr("Advanced")});
    quality->setToolTip(tr("Basic is the model's mask as it comes; Advanced refines it against the image's own edges"));
    qualityRow->addWidget(quality, 1);
    layout->addLayout(qualityRow);
    advanced_ = new QWidget;
    auto* av = new QVBoxLayout(advanced_);
    av->setContentsMargins(0, 0, 0, 0);
    auto slider = [&](const QString& label, const QString& tip, double min, double max, double scale, std::function<double()> get, std::function<void(double)> apply) {
        auto* row = new QHBoxLayout;
        auto* name = new QLabel(label);
        name->setMinimumWidth(90);
        name->setToolTip(tip);
        row->addWidget(name);
        auto* s = new QSlider(Qt::Horizontal);
        s->setRange(int(min * scale), int(max * scale));
        s->setValue(int(std::round(get() * scale)));
        row->addWidget(s, 1);
        auto* spin = new QDoubleSpinBox;
        spin->setRange(min, max);
        spin->setDecimals(scale >= 10 ? 1 : 0);
        spin->setValue(get());
        spin->setKeyboardTracking(false);
        row->addWidget(spin);
        connect(s, &QSlider::valueChanged, this, [this, spin, apply, scale](int v) { { QSignalBlocker b(spin); spin->setValue(v / scale); } apply(v / scale); refreshPreview(); });
        connect(spin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this, s, apply, scale](double v) { { QSignalBlocker b(s); s->setValue(int(std::round(v * scale))); } apply(v); refreshPreview(); });
        av->addLayout(row);
    };
    slider(tr("Refine Edges"), tr("Pulls the mask onto the image's own edges, recovering hair and fur (layer pixels)"), 0, 40, 1, [this] { return settings_.refineEdges; }, [this](double v) { settings_.refineEdges = v; });
    slider(tr("Contrast"), tr("Pushes the mask's grays toward black and white, clearing haze"), 0, 100, 1, [this] { return settings_.contrast; }, [this](double v) { settings_.contrast = v; });
    slider(tr("Matting"), tr("Solves the true opacity of hair and fur in a band this wide around the edge from foreground and background colours (slower)"), 0, mattingMax, 1, [this] { return settings_.matting; }, [this](double v) { settings_.matting = v; });
    slider(tr("Shift Edge"), tr("Contracts (negative) or expands the edge, dropping the rim of background colour"), -10, 10, 1, [this] { return settings_.shiftEdge; }, [this](double v) { settings_.shiftEdge = v; });
    auto check = [&](const QString& label, const QString& tip, bool& value) {
        auto* box = new QCheckBox(label);
        box->setToolTip(tip);
        box->setChecked(value);
        connect(box, &QCheckBox::toggled, this, [this, &value](bool on) { value = on; refreshPreview(); });
        av->addWidget(box);
    };
    auto* detail = new QCheckBox(tr("Detail pass (native resolution, slower)"));
    detail->setToolTip(tr("Runs the model again on full-resolution windows along the edge, where the whole-image pass blurred away hair and thin structures; a few seconds more on a large photo"));
    connect(detail, &QCheckBox::toggled, this, [this](bool on) {
        detail_ = on;
        if (on) { if (detailed_) { raw_ = detailed_; refreshPreview(); } else startDetail(); }
        else if (coarse_) { raw_ = coarse_; refreshPreview(); }
    });
    av->addWidget(detail);
    check(tr("Clean up speckle"), tr("Half-transparent specks that touch no edge go: inside the subject they become opaque, out in the background transparent"), settings_.cleanup);
    check(tr("Clean edge colours"), tr("The edge pixels take the subject's own colour, so no rim of the old background shows over a new one (those pixels of the layer change)"), settings_.decontaminate);
    advanced_->setVisible(false);
    layout->addWidget(advanced_);
    connect(quality, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int i) { advancedMode_ = i == 1; advanced_->setVisible(advancedMode_); adjustSize(); refreshPreview(); });
    auto* note = new QLabel(tr("The background is hidden by a layer mask, not erased: paint the mask, disable it or delete it to bring it back. Clean edge colours changes the edge pixels themselves."));
    note->setWordWrap(true);
    note->setStyleSheet(hintStyle());
    layout->addWidget(note);
    connect(addPreviewAndButtons(layout), &QCheckBox::toggled, this, [this] { refreshPreview(); });

    if (!source()) return;
    // The model runs once, off the UI thread; the sliders only redo the refinement. A quick coarse model,
    // when there is one, gives a preview within a few milliseconds while the chosen model works.
    computing_ = true;
    setCursor(Qt::BusyCursor);
    std::shared_ptr<const Image> image = source();
    std::string path = modelPath_.toStdString(), quick = quickModelPath == modelPath_ ? std::string() : quickModelPath.toStdString();
    const bool mirror = ModelStore::mirrorAverage();
    worker_ = std::thread([this, image, path, quick, mirror] {
        if (!quick.empty()) {
            std::string ignored;
            if (auto coarse = subjectMask(*image, quick, &ignored))
                QMetaObject::invokeMethod(this, [this, coarse] { if (computing_) { raw_ = coarse; refreshPreview(); } }, Qt::QueuedConnection);
        }
        std::string error;
        auto mask = subjectMask(*image, path, &error, mirror);
        QMetaObject::invokeMethod(this, [this, mask, error] {
            computing_ = false;
            unsetCursor();
            if (!mask) { error_ = QString::fromStdString(error); QMessageBox::warning(this, tr("Remove Background"), error_.isEmpty() ? tr("No subject mask could be made.") : error_); reject(); return; }
            raw_ = coarse_ = mask;
            refreshPreview();
            if (detail_) startDetail();
        }, Qt::QueuedConnection);
    });
}

void BackgroundDialog::startDetail() {
    if (!coarse_ || detailed_ || computing_ || !source()) return;
    computing_ = true;
    setCursor(Qt::BusyCursor);
    if (worker_.joinable()) worker_.join();
    std::shared_ptr<const Image> image = source();
    std::shared_ptr<const GrayImage> coarse = coarse_;
    std::string path = modelPath_.toStdString();
    worker_ = std::thread([this, image, coarse, path] {
        std::string error;
        auto detailed = subjectMaskDetailed(*image, path, coarse.get(), 12, &error);
        QMetaObject::invokeMethod(this, [this, detailed] {
            computing_ = false;
            unsetCursor();
            if (!detailed) return;   // the coarse mask stays
            detailed_ = detailed;
            if (detail_) { raw_ = detailed_; refreshPreview(); }
        }, Qt::QueuedConnection);
    });
}

BackgroundDialog::~BackgroundDialog() {
    if (worker_.joinable()) worker_.join();
}

std::shared_ptr<GrayImage> BackgroundDialog::refined(int limit) const {
    if (!raw_) return nullptr;
    if (!advancedMode_) return raw_;
    return refineMatte(*raw_, *source(), settings_, limit);
}

void BackgroundDialog::refreshPreview() {
    if (!source() || !raw_) return;
    if (!previewing()) { clearPreview(); return; }
    auto mask = refined(1400);
    // The layer with its background made transparent by the same mask the commit lays down, with the edge
    // colours it will have.
    std::shared_ptr<const Image> base = source();
    if (advancedMode_ && settings_.decontaminate) base = estimateForeground(*source(), *mask);
    auto out = std::make_shared<Image>(*base);
    for (int y = 0; y < out->height(); y++) for (int x = 0; x < out->width(); x++) {
        unsigned k = mask->at(x, y);
        uint8_t* p = out->pixel(x, y);
        for (int c = 0; c < 4; c++) p[c] = uint8_t((p[c] * k + 127) / 255);
    }
    showPreview(out);
}

bool BackgroundDialog::apply() {
    if (computing_) return false;   // OK waits for the mask
    if (!raw_) return true;
    auto mask = refined(0);
    std::shared_ptr<const Image> pixels;
    if (advancedMode_ && settings_.decontaminate) pixels = estimateForeground(*source(), *mask);
    session()->applySubjectMask(mask, pixels, layerId());
    return true;
}

} // namespace app
