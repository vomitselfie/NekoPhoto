// File > Export > Export As… and Layer > Export As…: the dialog (ExportAsDialog.h).
#include "ExportAsDialog.h"
#include "EditorSession.h"
#include "ImageConvert.h"
#include "Style.h"
#include "compositor/tga.h"
#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSlider>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>
#include <cmath>

using namespace compositor;

namespace app {

namespace {
/// Past this many output pixels the estimate encodes a smaller copy and scales its size up.
constexpr double exactPixels = 4'000'000, proxyPixels = 2'000'000;

/// One tile of the checkerboard drawn behind transparency.
QPixmap checkered(QSize size) {
    QPixmap out(size);
    QPainter p(&out);
    const int cell = size.width() / 2;
    for (int y = 0; y < size.height(); y += cell)
        for (int x = 0; x < size.width(); x += cell)
            p.fillRect(x, y, cell, cell, ((x / cell + y / cell) % 2) ? QColor(204, 204, 204) : QColor(255, 255, 255));
    return out;
}
}

ExportAsDialog::ExportAsDialog(EditorSession* session, bool layer, const QString& format, QWidget* parent) : QDialog(parent), session_(session), layer_(layer) {
    setWindowTitle(layer ? tr("Export Layer As") : tr("Export As"));
    // What is exported: the document, or the active layer alone (cropped to its visible pixels).
    if (session_ && session_->hasDocument()) {
        session_->endTemporaryLayers();
        const Document& doc = *session_->document();
        if (layer) {
            const Layer* active = session_->activeLayer();
            std::string error;
            if (active) solo_ = exportas::layerDocument(doc, active->id, &error);
            if (solo_) source_ = &*solo_;
            else why_ = tr("Select a layer to export.");
        } else source_ = &doc;
    } else why_ = tr("Open a document to export.");

    format_ = exportas::formats().contains(format) ? format : exportas::lastFormat();
    if (source_) {
        const exportas::Flat& first = flat();
        sourceSize_ = QSize(first.width(), first.height());
        if (first.empty()) why_ = tr("The layer has no visible pixels to export.");
    }
    ready_ = source_ && !sourceSize_.isEmpty();

    auto* layout = new QHBoxLayout(this);
    preview_ = new QLabel;
    preview_->setObjectName("preview");
    preview_->setMinimumSize(480, 380);
    preview_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);   // the pixmap follows the label, not the other way
    preview_->setAlignment(Qt::AlignCenter);
    preview_->setStyleSheet(QStringLiteral("background: %1;").arg(QColor(46, 46, 46).name()));
    layout->addWidget(preview_, 1);

    auto* side = new QVBoxLayout;
    layout->addLayout(side);

    // File settings.
    auto* fileBox = new QGroupBox(tr("File Settings"));
    auto* fileForm = new QFormLayout(fileBox);
    formatBox_ = new QComboBox;
    formatBox_->setObjectName("format");
    for (const QString& f : exportas::formats()) formatBox_->addItem(exportas::formatLabel(f), f);
    fileForm->addRow(tr("Format"), formatBox_);
    quality_ = new QSlider(Qt::Horizontal);
    quality_->setObjectName("quality");
    quality_->setRange(1, 100);
    quality_->setMinimumWidth(160);   // the form would squeeze it to its handle
    qualitySpin_ = new QSpinBox;
    qualitySpin_->setRange(1, 100);
    qualitySpin_->setButtonSymbols(QAbstractSpinBox::NoButtons);
    qualitySpin_->setFixedWidth(48);
    qualitySpin_->setAlignment(Qt::AlignRight);
    qualityRow_ = new QWidget;
    auto* qualityLayout = new QHBoxLayout(qualityRow_);
    qualityLayout->setContentsMargins(0, 0, 0, 0);
    qualityLayout->addWidget(quality_, 1);
    qualityLayout->addWidget(qualitySpin_);
    qualityLabel_ = new QLabel(tr("Quality"));
    fileForm->addRow(qualityLabel_, qualityRow_);
    transparency_ = new QCheckBox(tr("Transparency"));
    transparency_->setObjectName("transparency");
    fileForm->addRow(QString(), transparency_);
    matteButton_ = new QPushButton;
    matteButton_->setObjectName("matte");
    matteLabel_ = new QLabel(tr("Matte"));
    fileForm->addRow(matteLabel_, matteButton_);
    formatNote_ = new QLabel;
    formatNote_->setWordWrap(true);
    formatNote_->setStyleSheet(hintStyle());
    fileForm->addRow(formatNote_);
    side->addWidget(fileBox);

    // Image size, in pixels or as a percentage of the image's own size.
    auto* sizeBox = new QGroupBox(tr("Image Size"));
    auto* sizeForm = new QFormLayout(sizeBox);
    width_ = new QSpinBox;
    width_->setObjectName("width");
    height_ = new QSpinBox;
    height_->setObjectName("height");
    for (QSpinBox* s : {width_, height_}) { s->setRange(1, maxImageSide); s->setSuffix(tr(" px")); }
    scale_ = new QDoubleSpinBox;
    scale_->setObjectName("scale");
    scale_->setRange(0.1, 1000);
    scale_->setDecimals(1);
    scale_->setSuffix(QStringLiteral(" %"));
    keepRatio_ = new QCheckBox(tr("Keep proportions"));
    keepRatio_->setObjectName("keepRatio");
    keepRatio_->setChecked(true);
    resample_ = new QComboBox;
    resample_->setObjectName("resample");
    for (exportas::Resample r : {exportas::Resample::Bicubic, exportas::Resample::Bilinear, exportas::Resample::Nearest, exportas::Resample::Lanczos})
        resample_->addItem(exportas::resampleLabel(r), exportas::resampleKey(r));
    sizeForm->addRow(tr("Width"), width_);
    sizeForm->addRow(tr("Height"), height_);
    sizeForm->addRow(tr("Scale"), scale_);
    sizeForm->addRow(QString(), keepRatio_);
    sizeForm->addRow(tr("Resample"), resample_);
    side->addWidget(sizeBox);

    // Colour: converted to sRGB for the web, the profile embedded where the format carries one.
    auto* colorBox = new QGroupBox(tr("Color Space"));
    auto* colorLayout = new QVBoxLayout(colorBox);
    convert_ = new QCheckBox(tr("Convert to sRGB"));
    convert_->setObjectName("convertToSrgb");
    embed_ = new QCheckBox(tr("Embed Color Profile"));
    embed_->setObjectName("embedProfile");
    colorNote_ = new QLabel;
    colorNote_->setWordWrap(true);
    colorNote_->setStyleSheet(hintStyle());
    colorLayout->addWidget(convert_);
    colorLayout->addWidget(embed_);
    colorLayout->addWidget(colorNote_);
    side->addWidget(colorBox);

    estimateLabel_ = new QLabel;
    estimateLabel_->setObjectName("estimate");
    side->addWidget(estimateLabel_);
    side->addStretch(1);
    auto* buttons = new QDialogButtonBox;
    QPushButton* exportButton = buttons->addButton(tr("Export…"), QDialogButtonBox::AcceptRole);
    exportButton->setDefault(true);
    exportButton->setEnabled(ready_);
    buttons->addButton(QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    side->addWidget(buttons);

    timer_ = new QTimer(this);
    timer_->setSingleShot(true);
    timer_->setInterval(150);
    connect(timer_, &QTimer::timeout, this, &ExportAsDialog::refresh);

    {
        QSignalBlocker b1(width_), b2(height_), b3(scale_);
        width_->setValue(std::max(1, sourceSize_.width()));
        height_->setValue(std::max(1, sourceSize_.height()));
        scale_->setValue(100);
    }
    formatBox_->setCurrentIndex(std::max(0, formatBox_->findData(format_)));
    loadSettings(format_);

    connect(formatBox_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) {
        storeSettings();
        format_ = formatBox_->currentData().toString();
        loadSettings(format_);
        schedule();
    });
    connect(quality_, &QSlider::valueChanged, this, [this](int v) { { QSignalBlocker b(qualitySpin_); qualitySpin_->setValue(v); } schedule(); });
    connect(qualitySpin_, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int v) { quality_->setValue(v); });
    connect(transparency_, &QCheckBox::toggled, this, [this] { syncEnabled(); schedule(); });
    connect(matteButton_, &QPushButton::clicked, this, [this] {
        const QColor c = QColorDialog::getColor(matte_, this, tr("Matte"));
        if (!c.isValid()) return;
        matte_ = c;
        syncEnabled();
        schedule();
    });
    connect(width_, QOverload<int>::of(&QSpinBox::valueChanged), this, [this] { syncSize(width_); });
    connect(height_, QOverload<int>::of(&QSpinBox::valueChanged), this, [this] { syncSize(height_); });
    connect(scale_, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this] { syncSize(scale_); });
    connect(keepRatio_, &QCheckBox::toggled, this, [this](bool on) { if (on) syncSize(width_); });
    connect(resample_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this] { schedule(); });
    connect(convert_, &QCheckBox::toggled, this, [this] { syncEnabled(); schedule(); });
    connect(embed_, &QCheckBox::toggled, this, [this] { schedule(); });
    refresh();
}

QString ExportAsDialog::format() const { return format_; }

exportas::Settings ExportAsDialog::settings() const {
    exportas::Settings s;
    s.quality = quality_->value();
    s.transparency = transparency_->isChecked();
    s.matte = matte_;
    s.resample = exportas::resampleNamed(resample_->currentData().toString()).value_or(exportas::Resample::Bicubic);
    s.convertToSrgb = convert_->isChecked();
    s.embedProfile = embed_->isChecked();
    return s;
}

int ExportAsDialog::outputWidth() const { return width_->value(); }
int ExportAsDialog::outputHeight() const { return height_->value(); }

QJsonObject ExportAsDialog::commandParams() const {
    const bool full = outputWidth() == sourceSize_.width() && outputHeight() == sourceSize_.height();
    QJsonObject p = exportas::commandParams(format_, settings(), full ? 0 : outputWidth(), full ? 0 : outputHeight(), layer_ ? QStringLiteral("active") : QString());
    if (!full && !keepRatio_->isChecked()) return p;
    // In proportion: the width alone, so the height rounds the way the method rounds it.
    if (!full) p.remove("height");
    return p;
}

void ExportAsDialog::rememberSettings() {
    exportas::remember(format_, settings());
    exportas::setLastFormat(format_);
}

void ExportAsDialog::loadSettings(const QString& format) {
    const exportas::Settings s = settings_.contains(format) ? settings_.value(format) : exportas::remembered(format);
    QSignalBlocker b1(quality_), b2(qualitySpin_), b3(transparency_), b4(resample_), b5(convert_), b6(embed_);
    quality_->setValue(s.quality);
    qualitySpin_->setValue(s.quality);
    transparency_->setChecked(s.transparency);
    matte_ = s.matte;
    resample_->setCurrentIndex(std::max(0, resample_->findData(exportas::resampleKey(s.resample))));
    convert_->setChecked(s.convertToSrgb);
    embed_->setChecked(s.embedProfile);
    syncEnabled();
}

void ExportAsDialog::storeSettings() { settings_[format_] = settings(); }

void ExportAsDialog::syncEnabled() {
    const bool quality = exportas::hasQuality(format_), transparent = exportas::hasTransparency(format_);
    qualityLabel_->setVisible(quality);
    qualityRow_->setVisible(quality);
    transparency_->setVisible(transparent);
    const bool matteUsed = !transparent || !transparency_->isChecked();
    matteButton_->setEnabled(matteUsed);
    matteLabel_->setEnabled(matteUsed);
    matteButton_->setText(matte_ == Qt::white ? tr("White") : matte_ == Qt::black ? tr("Black") : matte_.name());
    matteButton_->setIcon([this] { QPixmap swatch(16, 16); swatch.fill(matte_); return QIcon(swatch); }());
    QString note;
    if (format_ == "gif" && source_ && !source_->animation.frames.empty() && !layer_)
        note = tr("A still image of the composite; File > Export > Animated GIF writes the timeline's frames.");
    else if (format_ == "gif") note = tr("At most 256 colours; pixels less than half opaque become transparent.");
    else if (format_ == "webp") note = tr("Quality 100 is lossless.");
    else if (source_ && source_->sampleType != SampleType::U8)
        note = exportas::keepsSixteenBits(format_, *source_) ? tr("Written at 16 bits per channel.") : tr("Written at 8 bits per channel, dithered.");
    formatNote_->setText(note);
    formatNote_->setVisible(!note.isEmpty());
    // Colour: only an RGB document tagged with another profile than sRGB has anything to convert.
    const bool rgb = source_ && source_->colorMode == ColorMode::RGB;
    const bool other = source_ && rgb && color::hasNonSrgbProfile(*source_);
    convert_->setEnabled(other);
    const bool converting = other && convert_->isChecked();
    const bool carries = format_ == "png" || format_ == "jpg" || format_ == "webp" || format_ == "tif";
    embed_->setEnabled(rgb && carries && !converting);
    if (!rgb && source_) colorNote_->setText(tr("%1 documents are written in sRGB.").arg(QString::fromLatin1(colorModeName(source_->colorMode))));
    else if (converting) colorNote_->setText(tr("The pixels are converted from %1 to sRGB, which needs no profile.").arg(QString::fromStdString(source_->profile.description)));
    else if (!carries) colorNote_->setText(tr("%1 files carry no colour profile.").arg(exportas::formatLabel(format_)));
    else if (!other) colorNote_->setText(tr("The document is in sRGB."));
    else colorNote_->setText(tr("The pixels keep the document’s profile; viewers that ignore profiles show them as sRGB."));
}

void ExportAsDialog::syncSize(QObject* from) {
    if (syncing_ || sourceSize_.isEmpty()) return;
    syncing_ = true;
    const double sw = sourceSize_.width(), sh = sourceSize_.height();
    QSignalBlocker b1(width_), b2(height_), b3(scale_);
    if (from == scale_) {
        const double k = scale_->value() / 100;
        width_->setValue(std::max(1, int(std::lround(sw * k))));
        height_->setValue(std::max(1, int(std::lround(sh * k))));
    } else if (from == width_) {
        if (keepRatio_->isChecked()) height_->setValue(std::max(1, int(std::lround(width_->value() * sh / sw))));
        scale_->setValue(width_->value() / sw * 100);
    } else {
        if (keepRatio_->isChecked()) width_->setValue(std::max(1, int(std::lround(height_->value() * sw / sh))));
        scale_->setValue(height_->value() / sh * 100);
    }
    syncing_ = false;
    schedule();
}

void ExportAsDialog::setFormat(const QString& format) { formatBox_->setCurrentIndex(std::max(0, formatBox_->findData(format))); }
void ExportAsDialog::setScalePercent(double percent) { scale_->setValue(percent); }
void ExportAsDialog::setOutputWidth(int width) { width_->setValue(width); }

void ExportAsDialog::schedule() { timer_->start(); }

void ExportAsDialog::resizeEvent(QResizeEvent* event) {
    QDialog::resizeEvent(event);
    if (timer_) schedule();   // the preview fits the new size
}

void ExportAsDialog::refreshNow() {
    timer_->stop();
    refresh();
}

const exportas::Flat& ExportAsDialog::flat() {
    const bool convert = convert_ ? convert_->isChecked() : exportas::remembered(format_).convertToSrgb;
    const bool embed = embed_ ? embed_->isChecked() : true;
    const bool sixteen = exportas::keepsSixteenBits(format_, *source_);
    const color::ExportPlan plan = color::exportPlan(*source_, convert, embed);
    const auto key = std::make_tuple(sixteen, plan.convert, embed);
    auto it = flats_.find(key);
    if (it == flats_.end()) it = flats_.emplace(key, exportas::flatten(*source_, format_, plan, layer_)).first;
    return it->second;
}

void ExportAsDialog::refresh() {
    if (!ready_) {
        estimateLabel_->setText(why_);
        preview_->setText(why_);
        estimate_ = 0;
        return;
    }
    const exportas::Settings s = settings();
    const color::ExportPlan plan = color::exportPlan(*source_, s.convertToSrgb, s.embedProfile);
    const exportas::Flat& full = flat();
    const int w = outputWidth(), h = outputHeight();
    exportas::Options options;
    options.format = format_;
    options.quality = s.quality;
    options.transparency = s.transparency;
    options.matte = s.matte;
    options.resample = s.resample;
    options.resolution = source_->resolution;
    // At most a few megapixels are encoded: past that, a smaller copy, and its size scaled by the area.
    const double pixels = double(w) * h;
    exact_ = pixels <= exactPixels;
    const double f = exact_ ? 1.0 : std::sqrt(proxyPixels / pixels);
    options.width = std::max(1, int(std::lround(w * f)));
    options.height = std::max(1, int(std::lround(h * f)));
    const exportas::Encoded encoded = exportas::encode(full, options, plan.icc, source_);
    if (!encoded.ok()) {
        estimate_ = 0;
        estimateLabel_->setText(tr("Can’t encode: %1").arg(encoded.error));
        return;
    }
    estimate_ = exact_ ? encoded.bytes.size() : qint64(double(encoded.bytes.size()) * pixels / (double(options.width) * options.height));
    estimateLabel_->setText(tr("%1 × %2 px, %3").arg(w).arg(h).arg(exportas::sizeText(estimate_, !exact_)));
    // The preview is the file decoded, so JPEG's and WebP's artefacts and GIF's palette show.
    QImage shown = QImage::fromData(encoded.bytes);
    if (shown.isNull() && format_ == "tga") {
        if (auto decoded = decodeTgaImage(reinterpret_cast<const uint8_t*>(encoded.bytes.constData()), size_t(encoded.bytes.size()))) shown = toQImage(*decoded);
    }
    if (shown.isNull()) { preview_->setText(tr("No preview for this format.")); return; }
    // Small images at their own size, larger ones fitted; transparency over a checkerboard.
    QSize fitted = shown.size();
    if (fitted.width() > preview_->width() || fitted.height() > preview_->height()) fitted = fitted.scaled(preview_->size(), Qt::KeepAspectRatio);
    QPixmap canvas(preview_->size());
    canvas.fill(QColor(46, 46, 46));
    QPainter p(&canvas);
    const QRect at(QPoint((canvas.width() - fitted.width()) / 2, (canvas.height() - fitted.height()) / 2), fitted);
    p.fillRect(at, QBrush(checkered(QSize(16, 16))));
    p.setRenderHint(QPainter::SmoothPixmapTransform, s.resample != exportas::Resample::Nearest);
    p.drawImage(at, shown);
    p.end();
    preview_->setPixmap(canvas);
}

} // namespace app
