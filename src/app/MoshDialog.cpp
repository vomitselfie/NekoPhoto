#include "MoshDialog.h"
#include "ActionLibrary.h"
#include "Names.h"
#include "compositor/depth.h"
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDoubleSpinBox>
#include <QFontMetricsF>
#include <QGridLayout>
#include <QGuiApplication>
#include <QImage>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPushButton>
#include <QSlider>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <random>

using namespace compositor;

namespace app {

namespace {

// OpenMosh's names for the effects, categories, parameters and options (compositor/mosh.cpp), so lupdate lists them in
// the "Names" context; the core keeps the English, which is also the API.
[[maybe_unused]] const char* const kMoshNames[] = {
    // categories
    QT_TRANSLATE_NOOP("Names", "Mosh"), QT_TRANSLATE_NOOP("Names", "Glitch"), QT_TRANSLATE_NOOP("Names", "Distort"),
    QT_TRANSLATE_NOOP("Names", "Retro"), QT_TRANSLATE_NOOP("Names", "Stylize"), QT_TRANSLATE_NOOP("Names", "Color"),
    QT_TRANSLATE_NOOP("Names", "Composite"),
    // effects
    QT_TRANSLATE_NOOP("Names", "Soft Glitch"), QT_TRANSLATE_NOOP("Names", "Hard Glitch"), QT_TRANSLATE_NOOP("Names", "Decimate"),
    QT_TRANSLATE_NOOP("Names", "Data-Mosh"), QT_TRANSLATE_NOOP("Names", "Splitter"), QT_TRANSLATE_NOOP("Names", "Jitter"),
    QT_TRANSLATE_NOOP("Names", "Slices"), QT_TRANSLATE_NOOP("Names", "Shake"), QT_TRANSLATE_NOOP("Names", "Pixel Sort"),
    QT_TRANSLATE_NOOP("Names", "Strobe"), QT_TRANSLATE_NOOP("Names", "Wave"), QT_TRANSLATE_NOOP("Names", "Kaleidoscope"),
    QT_TRANSLATE_NOOP("Names", "Pixelate"), QT_TRANSLATE_NOOP("Names", "Scan Lines"), QT_TRANSLATE_NOOP("Names", "VHS"),
    QT_TRANSLATE_NOOP("Names", "8-Bit CGA"), QT_TRANSLATE_NOOP("Names", "CRT"), QT_TRANSLATE_NOOP("Names", "Dither"),
    QT_TRANSLATE_NOOP("Names", "Dot Screen"), QT_TRANSLATE_NOOP("Names", "Halftone"),
    QT_TRANSLATE_NOOP("Names", "Bulge"), QT_TRANSLATE_NOOP("Names", "Stretch"), QT_TRANSLATE_NOOP("Names", "Push"),
    QT_TRANSLATE_NOOP("Names", "Luma-Mesh"), QT_TRANSLATE_NOOP("Names", "3D Transform"), QT_TRANSLATE_NOOP("Names", "Tile"),
    QT_TRANSLATE_NOOP("Names", "Mirror"), QT_TRANSLATE_NOOP("Names", "Wobble"), QT_TRANSLATE_NOOP("Names", "Smear"),
    QT_TRANSLATE_NOOP("Names", "Twirl"), QT_TRANSLATE_NOOP("Names", "Optical-Flow"), QT_TRANSLATE_NOOP("Names", "Super 8"),
    QT_TRANSLATE_NOOP("Names", "Bad TV"), QT_TRANSLATE_NOOP("Names", "Ascii"), QT_TRANSLATE_NOOP("Names", "Bleach"),
    QT_TRANSLATE_NOOP("Names", "Edges"), QT_TRANSLATE_NOOP("Names", "Emboss"), QT_TRANSLATE_NOOP("Names", "Vignette"),
    QT_TRANSLATE_NOOP("Names", "Noise Displace"), QT_TRANSLATE_NOOP("Names", "Watercolor"), QT_TRANSLATE_NOOP("Names", "Zoom Blur"),
    QT_TRANSLATE_NOOP("Names", "Glow"), QT_TRANSLATE_NOOP("Names", "Light Streak"), QT_TRANSLATE_NOOP("Names", "Feedback"),
    QT_TRANSLATE_NOOP("Names", "Color Correction"), QT_TRANSLATE_NOOP("Names", "Duotone"), QT_TRANSLATE_NOOP("Names", "Solarize"),
    QT_TRANSLATE_NOOP("Names", "Chromatic Warp"), QT_TRANSLATE_NOOP("Names", "Sepia"), QT_TRANSLATE_NOOP("Names", "Overlay"),
    QT_TRANSLATE_NOOP("Names", "Mask"), QT_TRANSLATE_NOOP("Names", "Mask Blocks"), QT_TRANSLATE_NOOP("Names", "ChromaKey"),
    QT_TRANSLATE_NOOP("Names", "Caption"),
    // parameters
    QT_TRANSLATE_NOOP("Names", "Amount"), QT_TRANSLATE_NOOP("Names", "Angle"), QT_TRANSLATE_NOOP("Names", "Blocks"),
    QT_TRANSLATE_NOOP("Names", "Color Shift"), QT_TRANSLATE_NOOP("Names", "Block Size"), QT_TRANSLATE_NOOP("Names", "Drift"),
    QT_TRANSLATE_NOOP("Names", "Stuck Blocks"), QT_TRANSLATE_NOOP("Names", "Strips"), QT_TRANSLATE_NOOP("Names", "Offset"),
    QT_TRANSLATE_NOOP("Names", "Vertical"), QT_TRANSLATE_NOOP("Names", "Band Height"), QT_TRANSLATE_NOOP("Names", "Count"),
    QT_TRANSLATE_NOOP("Names", "Threshold Low"), QT_TRANSLATE_NOOP("Names", "Threshold High"), QT_TRANSLATE_NOOP("Names", "Reverse"),
    QT_TRANSLATE_NOOP("Names", "Phase"), QT_TRANSLATE_NOOP("Names", "Rate"), QT_TRANSLATE_NOOP("Names", "Mode"),
    QT_TRANSLATE_NOOP("Names", "Amplitude"), QT_TRANSLATE_NOOP("Names", "Frequency"), QT_TRANSLATE_NOOP("Names", "Segments"),
    QT_TRANSLATE_NOOP("Names", "Rotation"), QT_TRANSLATE_NOOP("Names", "Density"), QT_TRANSLATE_NOOP("Names", "Opacity"),
    QT_TRANSLATE_NOOP("Names", "Tracking"), QT_TRANSLATE_NOOP("Names", "Color Bleed"), QT_TRANSLATE_NOOP("Names", "Noise"),
    QT_TRANSLATE_NOOP("Names", "Pixel Size"), QT_TRANSLATE_NOOP("Names", "Palette"), QT_TRANSLATE_NOOP("Names", "Curvature"),
    QT_TRANSLATE_NOOP("Names", "Scanlines"), QT_TRANSLATE_NOOP("Names", "Aperture Mask"), QT_TRANSLATE_NOOP("Names", "Scale"),
    QT_TRANSLATE_NOOP("Names", "Levels"), QT_TRANSLATE_NOOP("Names", "Strength"), QT_TRANSLATE_NOOP("Names", "Radius"),
    QT_TRANSLATE_NOOP("Names", "Center X"), QT_TRANSLATE_NOOP("Names", "Center Y"), QT_TRANSLATE_NOOP("Names", "Center"),
    QT_TRANSLATE_NOOP("Names", "Width"), QT_TRANSLATE_NOOP("Names", "Push X"), QT_TRANSLATE_NOOP("Names", "Push Y"),
    QT_TRANSLATE_NOOP("Names", "Wrap"), QT_TRANSLATE_NOOP("Names", "Offset X"), QT_TRANSLATE_NOOP("Names", "Offset Y"),
    QT_TRANSLATE_NOOP("Names", "Tilt X"), QT_TRANSLATE_NOOP("Names", "Tilt Y"), QT_TRANSLATE_NOOP("Names", "Columns"),
    QT_TRANSLATE_NOOP("Names", "Rows"), QT_TRANSLATE_NOOP("Names", "Distance"), QT_TRANSLATE_NOOP("Names", "Swirl"),
    QT_TRANSLATE_NOOP("Names", "Grain"), QT_TRANSLATE_NOOP("Names", "Warmth"), QT_TRANSLATE_NOOP("Names", "Distortion"),
    QT_TRANSLATE_NOOP("Names", "Roll"), QT_TRANSLATE_NOOP("Names", "Cell Size"), QT_TRANSLATE_NOOP("Names", "Threshold"),
    QT_TRANSLATE_NOOP("Names", "Intensity"), QT_TRANSLATE_NOOP("Names", "Length"), QT_TRANSLATE_NOOP("Names", "Zoom"),
    QT_TRANSLATE_NOOP("Names", "Decay"), QT_TRANSLATE_NOOP("Names", "Brightness"), QT_TRANSLATE_NOOP("Names", "Contrast"),
    QT_TRANSLATE_NOOP("Names", "Saturation"), QT_TRANSLATE_NOOP("Names", "Gamma"), QT_TRANSLATE_NOOP("Names", "Shadow Hue"),
    QT_TRANSLATE_NOOP("Names", "Highlight Hue"), QT_TRANSLATE_NOOP("Names", "Mix"), QT_TRANSLATE_NOOP("Names", "Blend"),
    QT_TRANSLATE_NOOP("Names", "Low"), QT_TRANSLATE_NOOP("Names", "High"), QT_TRANSLATE_NOOP("Names", "Key Hue"),
    QT_TRANSLATE_NOOP("Names", "Tolerance"), QT_TRANSLATE_NOOP("Names", "Softness"), QT_TRANSLATE_NOOP("Names", "Position X"),
    QT_TRANSLATE_NOOP("Names", "Position Y"), QT_TRANSLATE_NOOP("Names", "Hue"),
    // options
    QT_TRANSLATE_NOOP("Names", "Blackout"), QT_TRANSLATE_NOOP("Names", "Whiteout"), QT_TRANSLATE_NOOP("Names", "Invert"),
    QT_TRANSLATE_NOOP("Names", "Cyan/Magenta"), QT_TRANSLATE_NOOP("Names", "Green/Red"), QT_TRANSLATE_NOOP("Names", "Grayscale"),
    QT_TRANSLATE_NOOP("Names", "Left → Right"), QT_TRANSLATE_NOOP("Names", "Right → Left"), QT_TRANSLATE_NOOP("Names", "Top → Bottom"),
    QT_TRANSLATE_NOOP("Names", "Bottom → Top"), QT_TRANSLATE_NOOP("Names", "Terminal Green"), QT_TRANSLATE_NOOP("Names", "White"),
    QT_TRANSLATE_NOOP("Names", "Original"), QT_TRANSLATE_NOOP("Names", "Normal"), QT_TRANSLATE_NOOP("Names", "Multiply"),
    QT_TRANSLATE_NOOP("Names", "Screen"), QT_TRANSLATE_NOOP("Names", "Lighten"), QT_TRANSLATE_NOOP("Names", "Darken"),
};

/// Caption's text, white on transparent: one line at 8 pixels times `scale`, the size OpenMosh's 8x8 font takes.
std::shared_ptr<Image> captionImage(const QString& text, float scale, float& logicalWidth, float& logicalHeight) {
    QString line = text;
    line.replace(QLatin1Char('\n'), QLatin1Char(' '));
    if (line.trimmed().isEmpty()) return nullptr;
    const int height = std::max(1, int(std::lround(8.0 * scale)));
    QFont font = QGuiApplication::font();
    font.setBold(true);
    font.setPixelSize(std::max(1, int(std::lround(height * 0.8))));
    const QFontMetricsF metrics(font);
    const int width = std::max(1, int(std::ceil(metrics.horizontalAdvance(line))));
    QImage canvas(width, height, QImage::Format_RGBA8888_Premultiplied);
    canvas.fill(Qt::transparent);
    {
        QPainter painter(&canvas);
        painter.setRenderHint(QPainter::TextAntialiasing);
        painter.setFont(font);
        painter.setPen(Qt::white);
        painter.drawText(QRectF(0, 0, width, height), Qt::AlignCenter, line);
    }
    auto out = std::make_shared<Image>(width, height);
    for (int y = 0; y < height; y++) std::memcpy(out->row(y), canvas.constScanLine(y), size_t(width) * 4);
    logicalWidth = float(width) / scale;   // the effect draws it at this size times Scale: one image pixel a text pixel
    logicalHeight = float(height) / scale;
    return out;
}

/// Decimals for a range: finer for a small span.
int decimalsFor(const mosh::ParamSpec& p) {
    const float span = p.max - p.min;
    return span <= 10 ? 2 : span <= 100 ? 1 : 0;
}

} // namespace

namespace names {
QString mosh(std::string_view english) { return core(std::string(english)); }
} // namespace names

mosh::Sources MoshExtras::sources() const {
    mosh::Sources s;
    s.aux = aux.get();
    s.aux16 = aux16.get();
    s.auxWidth = auxWidth;
    s.auxHeight = auxHeight;
    return s;
}

MoshExtras moshExtras(const mosh::EffectSpec& spec, const mosh::Settings& settings, const Document& document, const std::optional<Uuid>& auxLayer,
                      const QString& text, const LayerTransform& target, int width, int height) {
    MoshExtras e;
    if (spec.text) e.aux = captionImage(text, std::max(settings.normalized().value("scale", 4), 1.0f), e.auxWidth, e.auxHeight);
    if (spec.auxImage && auxLayer) {
        const Layer* layer = document.find(*auxLayer);
        if (layer && layer->asset) {
            const Affine grid = target.pixelToDocument(width, height);
            if (const auto& deep = layer->asset->image.u16())
                e.aux16 = mosh::onGrid(*deep, layer->transform.pixelToDocument(deep->width(), deep->height()), grid, width, height);
            else if (const auto& eight = layer->asset->image.u8())
                e.aux = mosh::onGrid(*eight, layer->transform.pixelToDocument(eight->width(), eight->height()), grid, width, height);
        }
    }
    return e;
}

std::vector<const Layer*> moshSourceLayers(const Document& document, const std::optional<Uuid>& exclude) {
    std::vector<const Layer*> out;
    for (auto it = document.layers.rbegin(); it != document.layers.rend(); ++it) {
        const Layer& l = *it;
        if (l.isGroup || l.adjustment || !l.asset || !l.asset->image || !l.visible || (exclude && l.id == *exclude)) continue;
        out.push_back(&l);
    }
    return out;
}

QJsonObject moshRequest(const mosh::Settings& settings, const std::optional<Uuid>& layer, const QString& text) {
    const mosh::Settings s = settings.normalized();
    QJsonObject params;
    if (const mosh::EffectSpec* spec = mosh::findEffect(s.effect)) {
        for (size_t i = 0; i < spec->params.size() && i < s.values.size(); i++) {
            const mosh::ParamSpec& p = spec->params[i];
            const QString key = QString::fromUtf8(p.key.data(), qsizetype(p.key.size()));
            if (p.kind == mosh::ParamKind::Bool) params[key] = s.values[i] > 0.5f;
            else if (p.kind == mosh::ParamKind::Choice) params[key] = int(s.values[i]);
            else params[key] = double(s.values[i]);
        }
    }
    QJsonObject request{{"effect", QString::fromStdString(s.effect)}, {"params", params}};
    if (const mosh::EffectSpec* spec = mosh::findEffect(s.effect)) {
        if (spec->seeded) request["seed"] = double(s.seed);
        if (spec->auxImage && layer) request["layer"] = QString::fromStdString(*layer);
        if (spec->text) request["text"] = text;
    }
    return request;
}

MoshDialog::MoshDialog(EditorSession* session, const mosh::EffectSpec& spec, QWidget* parent)
    : PixelDialog(session, parent), spec_(spec), settings_(mosh::Settings::defaults(spec)) {
    setWindowTitle(names::mosh(spec.name));
    setMinimumWidth(440);
    previewTimer_ = new QTimer(this);
    previewTimer_->setSingleShot(true);
    previewTimer_->setInterval(0);   // coalesces a burst of slider moves into one preview
    connect(previewTimer_, &QTimer::timeout, this, [this] { refreshPreview(); });

    auto* layout = new QVBoxLayout(this);
    auto* grid = new QGridLayout;   // names, controls and values in columns
    grid->setColumnStretch(1, 1);
    layout->addLayout(grid);
    int row = 0;
    if (spec.auxImage) {
        // Overlay's image, Mask's mask: another layer, read where it sits over this one in the document.
        grid->addWidget(new QLabel(tr("Layer")), row, 0);
        layerCombo_ = new QComboBox;
        layerCombo_->setToolTip(tr("The layer this effect reads, where it lies over this one"));
        if (session && session->document())
            for (const Layer* l : moshSourceLayers(*session->document(), session->activeLayerId()))
                layerCombo_->addItem(QString::fromStdString(l->name), QString::fromStdString(l->id));
        if (layerCombo_->count() == 0) {
            layerCombo_->addItem(tr("(no other layer)"));
            layerCombo_->setEnabled(false);
        }
        connect(layerCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this] { schedulePreview(); });
        grid->addWidget(layerCombo_, row++, 1, 1, 2);
    }
    if (spec.text) {
        grid->addWidget(new QLabel(tr("Text")), row, 0);
        text_ = new QLineEdit(tr("Caption"));
        connect(text_, &QLineEdit::textChanged, this, [this] { schedulePreview(); });
        grid->addWidget(text_, row++, 1, 1, 2);
    }
    for (size_t i = 0; i < spec.params.size(); i++, row++) {
        const mosh::ParamSpec& p = spec.params[i];
        const QString label = names::mosh(p.label);
        if (p.kind == mosh::ParamKind::Bool) {
            auto* box = new QCheckBox(label);
            connect(box, &QCheckBox::toggled, this, [this, i](bool on) { settings_.values[i] = on ? 1.0f : 0.0f; schedulePreview(); });
            syncers_.push_back([this, box, i] { QSignalBlocker b(box); box->setChecked(settings_.values[i] > 0.5f); });
            grid->addWidget(box, row, 0, 1, 3);
            continue;
        }
        grid->addWidget(new QLabel(label), row, 0);
        if (p.kind == mosh::ParamKind::Choice) {
            auto* combo = new QComboBox;
            for (std::string_view option : p.options) combo->addItem(names::mosh(option));
            connect(combo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this, i](int index) { settings_.values[i] = float(index); schedulePreview(); });
            syncers_.push_back([this, combo, i] { QSignalBlocker b(combo); combo->setCurrentIndex(int(settings_.values[i])); });
            grid->addWidget(combo, row, 1, 1, 2);
            continue;
        }
        const int decimals = decimalsFor(p);
        const double scale = std::pow(10.0, decimals);
        auto* slider = new QSlider(Qt::Horizontal);
        slider->setRange(int(std::lround(p.min * scale)), int(std::lround(p.max * scale)));
        grid->addWidget(slider, row, 1);
        auto* spin = new QDoubleSpinBox;
        spin->setRange(p.min, p.max);
        spin->setDecimals(decimals);
        spin->setSingleStep(1 / scale);
        spin->setKeyboardTracking(false);
        spin->setButtonSymbols(QAbstractSpinBox::NoButtons);
        spin->setAlignment(Qt::AlignRight);
        spin->setFixedWidth(64);
        grid->addWidget(spin, row, 2);
        connect(slider, &QSlider::valueChanged, this, [this, spin, i, scale](int v) {
            { QSignalBlocker b(spin); spin->setValue(v / scale); }
            settings_.values[i] = float(v / scale);
            schedulePreview();
        });
        connect(spin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this, slider, i, scale](double v) {
            { QSignalBlocker b(slider); slider->setValue(int(std::lround(v * scale))); }
            settings_.values[i] = float(v);
            schedulePreview();
        });
        syncers_.push_back([this, slider, spin, i, scale] {
            QSignalBlocker a(slider), b(spin);
            slider->setValue(int(std::lround(settings_.values[i] * scale)));
            spin->setValue(settings_.values[i]);
        });
    }
    if (spec.seeded) {
        // OpenMosh's seed: the same number gives the same pattern; Reroll draws another.
        grid->addWidget(new QLabel(tr("Seed")), row, 0);
        auto* seed = new QDoubleSpinBox;
        seed->setRange(0, 99.999);
        seed->setDecimals(3);
        seed->setKeyboardTracking(false);
        seed->setToolTip(tr("The random pattern: the same seed always gives the same result"));
        grid->addWidget(seed, row, 1);
        auto* reroll = new QPushButton(tr("Reroll"));
        reroll->setToolTip(tr("A new random pattern"));
        grid->addWidget(reroll, row, 2);
        connect(seed, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double v) { settings_.seed = float(v); schedulePreview(); });
        connect(reroll, &QPushButton::clicked, this, [seed] { seed->setValue(mosh::seedFrom(uint32_t(std::random_device{}()))); });
        syncers_.push_back([this, seed] { QSignalBlocker b(seed); seed->setValue(settings_.seed); });
    }
    for (auto& sync : syncers_) sync();
    // The effects work in the layer's own pixels (block sizes, scanlines), so the preview runs at full size.
    capture(0, 0);
    connect(addPreviewAndButtons(layout), &QCheckBox::toggled, this, [this] { refreshPreview(); });
    refreshPreview();
}

void MoshDialog::schedulePreview() { previewTimer_->start(); }

std::optional<Uuid> MoshDialog::auxLayer() const {
    if (!layerCombo_ || !layerCombo_->isEnabled() || layerCombo_->currentIndex() < 0) return std::nullopt;
    return layerCombo_->currentData().toString().toStdString();
}

QString MoshDialog::captionText() const { return text_ ? text_->text() : QString(); }

MoshExtras MoshDialog::extras() const {
    const int w = source16() ? source16()->width() : source() ? source()->width() : 0;
    const int h = source16() ? source16()->height() : source() ? source()->height() : 0;
    if (!session() || !session()->document() || (!spec_.auxImage && !spec_.text)) return {};
    return moshExtras(spec_, settings_, *session()->document(), auxLayer(), captionText(), placement(), w, h);
}

void MoshDialog::refreshPreview() {
    if (finished() || !hasPreviewSource()) return;
    if (!previewing()) { clearPreview(); return; }
    const MoshExtras more = extras();   // the preview is at full size (capture(0, 0)), on source()'s grid
    if (previewSource16()) {
        auto out = std::make_shared<Image16>(*previewSource16());
        mosh::apply(settings_, *out, more.sources());
        showPreview(out, placement());
    } else {
        auto out = std::make_shared<Image>(*previewSource());
        mosh::apply(settings_, *out, more.sources());
        showPreview(out, placement());
    }
}

bool MoshDialog::apply() {
    const QString name = QString::fromUtf8(spec_.name.data(), qsizetype(spec_.name.size()));
    const MoshExtras more = extras();
    if (source16()) {
        auto out = std::make_shared<Image16>(*source16());
        mosh::apply(settings_, *out, more.sources());
        throughSelection(*out);
        commit(Image16Ptr(out), placement(), name);
    } else {
        auto out = std::make_shared<Image>(*source());
        mosh::apply(settings_, *out, more.sources());
        throughSelection(*out);
        commit(std::shared_ptr<const Image>(out), placement(), name);
    }
    recordAction("pixels.mosh", moshRequest(settings_, auxLayer(), captionText()));
    return true;
}

} // namespace app
