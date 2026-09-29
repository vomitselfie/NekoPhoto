// The Camera Raw dialog a camera RAW file opens in (see RawDevelopDialog.h). The develop is compositor/raw.h: LibRaw's
// decode at the white balance's multipliers, graded by the Camera Raw settings of compositor/cameraraw.h.
#include "RawDevelopDialog.h"
#include "CameraRawPanels.h"
#include "compositor/depth.h"
#include "compositor/raw.h"
#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QResizeEvent>
#include <QSettings>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>

using namespace compositor;

namespace app {

namespace {

/// The preview's longest side: enough to judge the grade, small enough to follow a slider.
constexpr int previewLimit = 1600;

/// `image` reduced by a whole factor (a box average) until its longest side fits `limit`.
std::shared_ptr<Image16> reduced(std::shared_ptr<Image16> image, int limit) {
    const int k = (std::max(image->width(), image->height()) + limit - 1) / limit;
    if (k <= 1) return image;
    auto out = std::make_shared<Image16>(std::max(1, image->width() / k), std::max(1, image->height() / k));
    for (int y = 0; y < out->height(); y++)
        for (int x = 0; x < out->width(); x++) {
            uint32_t sum[4] = {0, 0, 0, 0};
            for (int j = 0; j < k; j++) {
                const uint16_t* p = image->pixel(x * k, y * k + j);
                for (int i = 0; i < k; i++, p += 4) for (int c = 0; c < 4; c++) sum[c] += p[c];
            }
            uint16_t* d = out->pixel(x, y);
            for (int c = 0; c < 4; c++) d[c] = uint16_t((sum[c] + uint32_t(k * k) / 2) / uint32_t(k * k));
        }
    return out;
}

QImage toDisplay(const Image16& image) {
    auto eight = narrowImage(image);
    QImage out(eight->width(), eight->height(), QImage::Format_RGBA8888_Premultiplied);
    for (int y = 0; y < eight->height(); y++) std::copy_n(eight->row(y), size_t(eight->width()) * 4, out.scanLine(y));
    return out;
}

} // namespace

int RawDevelopDialog::workflowBits() {
    return QSettings().value("cameraRaw/bitsPerChannel", 16).toInt() == 8 ? 8 : 16;
}

RawDevelopDialog::RawDevelopDialog(std::shared_ptr<const std::vector<uint8_t>> bytes, const QString& fileName, const CameraRawSettings& settings,
                                   Purpose purpose, QWidget* parent)
    : QDialog(parent), bytes_(std::move(bytes)), purpose_(purpose) {
    setObjectName("rawDevelopDialog");
    RawInfo info;
    std::string error;
    const bool known = readRawInfo(*bytes_, info, &error);
    if (known) readRawWhiteBalance(*bytes_, balance_, nullptr);
    baseMultipliers_ = balance_.asShot;
    fullWidth_ = info.width;
    fullHeight_ = info.height;
    const QString camera = QString::fromStdString((info.make + " " + info.model)).trimmed();
    setWindowTitle(camera.isEmpty() ? tr("Camera Raw – %1").arg(fileName) : tr("Camera Raw – %1 (%2)").arg(fileName, camera));
    resize(1180, 760);

    debounce_ = new QTimer(this);
    debounce_->setSingleShot(true);
    debounce_->setInterval(15);
    connect(debounce_, &QTimer::timeout, this, [this] { refreshPreview(); });
    // A white balance change previews at once as a rebalance of the last decode; once it settles, the file is decoded
    // again at the new multipliers so the preview is the develop's.
    settle_ = new QTimer(this);
    settle_->setSingleShot(true);
    settle_->setInterval(350);
    connect(settle_, &QTimer::timeout, this, [this] {
        if (base_ && !developing_ && targetMultipliers() != baseMultipliers_) startPreviewDecode();
    });

    auto* layout = new QVBoxLayout(this);
    auto* body = new QHBoxLayout;
    preview_ = new QLabel(tr("Reading the RAW file…"));
    preview_->setObjectName("rawPreview");
    preview_->setAlignment(Qt::AlignCenter);
    preview_->setMinimumSize(420, 320);
    preview_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    body->addWidget(preview_, 1);
    // Settings saved before kelvin white balance open in kelvin (their relative values as the white point they meant).
    panels_ = new CameraRawPanels(CameraRawPanels::Mode::Raw, balance_.inKelvin(settings), this, &balance_);
    panels_->setMinimumWidth(440);
    panels_->setMaximumWidth(540);
    // White Balance > Auto: the gray-world white point of the quick decode (kelvin and tint), or for a camera without a
    // colour model the gray-world balance relative to as shot.
    panels_->autoBalance = [this]() -> std::optional<std::array<double, 2>> {
        if (!base_) return std::nullopt;
        if (!balance_.kelvin) return CameraRawSettings::autoBalance(*base_);
        const auto solved = rawAutoWhiteBalance(*base_, baseMultipliers_, balance_);
        if (!solved) return std::nullopt;
        return std::array<double, 2>{solved->temperature, solved->tint};
    };
    connect(panels_, &CameraRawPanels::changed, this, [this] { debounce_->start(); settle_->start(); });
    body->addWidget(panels_);
    layout->addLayout(body, 1);

    auto* bottom = new QHBoxLayout;
    status_ = new QLabel(known ? tr("%1 × %2 pixels").arg(fullWidth_).arg(fullHeight_) : QString::fromStdString(error));
    status_->setObjectName("rawStatus");
    bottom->addWidget(status_, 1);
    if (purpose_ == Purpose::Open) {
        // Photoshop's Camera Raw workflow options: the depth the image opens at (sRGB, as LibRaw develops it).
        bottom->addWidget(new QLabel(tr("Depth:")));
        depth_ = new QComboBox;
        depth_->setObjectName("rawDepth");
        depth_->addItems({tr("8 Bits/Channel"), tr("16 Bits/Channel")});
        depth_->setCurrentIndex(workflowBits() == 8 ? 0 : 1);
        depth_->setToolTip(tr("Camera Raw's workflow depth; the colour space is sRGB"));
        bottom->addWidget(depth_);
    }
    cancel_ = new QPushButton(tr("Cancel"));
    connect(cancel_, &QPushButton::clicked, this, &RawDevelopDialog::reject);
    bottom->addWidget(cancel_);
    if (purpose_ == Purpose::Open) {
        openObject_ = new QPushButton(tr("Open Object"));
        openObject_->setToolTip(tr("Open as a smart object that keeps the RAW file and these settings: Edit Contents develops it again"));
        connect(openObject_, &QPushButton::clicked, this, [this] { develop(Choice::OpenObject); });
        bottom->addWidget(openObject_);
    }
    open_ = new QPushButton(purpose_ == Purpose::Open ? tr("Open") : tr("OK"));
    open_->setDefault(true);
    connect(open_, &QPushButton::clicked, this, [this] { develop(Choice::Open); });
    bottom->addWidget(open_);
    layout->addLayout(bottom);

    if (!known) { open_->setEnabled(false); if (openObject_) openObject_->setEnabled(false); return; }
    startPreviewDecode();
}

RawDevelopDialog::~RawDevelopDialog() {
    stopPreview();
    stopWorker();
}

const CameraRawSettings& RawDevelopDialog::settings() const { return panels_->settings(); }

int RawDevelopDialog::bitsPerChannel() const { return depth_ && depth_->currentIndex() == 0 ? 8 : 16; }

void RawDevelopDialog::stopWorker() {
    cancelFlag_ = true;
    if (worker_.joinable()) worker_.join();
    cancelFlag_ = false;
}

std::array<double, 3> RawDevelopDialog::targetMultipliers() const {
    return balance_.multipliersFor(panels_->settings().normalized()).value_or(balance_.asShot);
}

void RawDevelopDialog::startPreviewDecode() {
    // The quick decode (half size) off the UI thread; the dialog shows at once and the preview follows.
    if (previewBusy_) return;   // the one under way starts the next when it is done
    if (previewWorker_.joinable()) previewWorker_.join();
    previewBusy_ = true;
    const int generation = ++previewGeneration_;
    const std::array<double, 3> target = targetMultipliers();
    std::optional<std::array<double, 3>> multipliers;
    if (target != balance_.asShot) multipliers = target;   // as shot is the camera's own balance, exactly
    auto bytes = bytes_;
    previewWorker_ = std::thread([this, bytes, multipliers, target, generation] {
        RawDecodeOptions options;
        options.halfSize = true;
        options.cancel = &previewCancel_;
        options.multipliers = multipliers;
        std::string error;
        auto image = decodeRaw16(*bytes, options, &error);
        if (image) image = reduced(std::move(image), previewLimit);
        QMetaObject::invokeMethod(this, [this, image, error, target, generation] { previewDecoded(image, error, target, generation); },
                                  Qt::QueuedConnection);
    });
}

void RawDevelopDialog::previewDecoded(std::shared_ptr<Image16> image, const std::string& error, const std::array<double, 3>& multipliers,
                                      int generation) {
    if (previewWorker_.joinable()) previewWorker_.join();
    previewBusy_ = false;
    if (generation != previewGeneration_ || developing_) return;   // stopped for a develop
    if (!image) {
        if (base_) return;   // a later decode failed: keep previewing the last one
        preview_->setText(QString::fromStdString(error));
        open_->setEnabled(false);
        if (openObject_) openObject_->setEnabled(false);
        return;
    }
    base_ = std::move(image);
    baseMultipliers_ = multipliers;
    refreshPreview();
    if (targetMultipliers() != baseMultipliers_ && !settle_->isActive()) startPreviewDecode();   // the balance moved on meanwhile
}

void RawDevelopDialog::stopPreview() {
    previewCancel_ = true;
    if (previewWorker_.joinable()) previewWorker_.join();
    previewCancel_ = false;
    previewBusy_ = false;
    previewGeneration_++;   // a result already queued is ignored
}

void RawDevelopDialog::refreshPreview() {
    if (!base_ || developing_) return;
    Image16 graded = *base_;
    if (const auto target = targetMultipliers(); target != baseMultipliers_) rebalanceRawDecode(graded, balance_, baseMultipliers_, target);
    const double scale = fullWidth_ > 0 ? double(base_->width()) / fullWidth_ : 0.5;
    const CameraRawSettings normalized = panels_->settings().normalized();
    const CameraRawPreview& overlays = panels_->preview();
    const bool anyOverlay = overlays.shadowClipIndicator || overlays.highlightClipIndicator || overlays.sharpenMask;
    if (!normalized.isIdentity() || anyOverlay) applyCameraRaw(graded, normalized, scale, 0, overlays);
    shown_ = toDisplay(graded);
    showPreviewImage();
}

void RawDevelopDialog::showPreviewImage() {
    if (shown_.isNull()) return;
    const qreal ratio = devicePixelRatioF();
    QPixmap pixmap = QPixmap::fromImage(shown_.scaled(preview_->size() * ratio, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    pixmap.setDevicePixelRatio(ratio);
    preview_->setPixmap(pixmap);
}

void RawDevelopDialog::resizeEvent(QResizeEvent* event) {
    QDialog::resizeEvent(event);
    showPreviewImage();
}

void RawDevelopDialog::develop(Choice choice) {
    if (developing_ || !base_) return;
    developing_ = true;
    settle_->stop();
    stopPreview();
    if (depth_) QSettings().setValue("cameraRaw/bitsPerChannel", bitsPerChannel());
    panels_->setEnabled(false);
    open_->setEnabled(false);
    if (openObject_) openObject_->setEnabled(false);
    if (depth_) depth_->setEnabled(false);
    status_->setText(tr("Developing the full image…"));
    // The whole file, off the UI thread; Cancel stops it.
    auto bytes = bytes_;
    const CameraRawSettings settings = panels_->settings().normalized();
    worker_ = std::thread([this, bytes, settings, choice] {
        RawDecodeOptions options;
        options.cancel = &cancelFlag_;
        std::string error;
        workerResult_ = developRaw(*bytes, settings, options, &error);
        workerError_ = error;
        QMetaObject::invokeMethod(this, [this, choice] { developed(choice); }, Qt::QueuedConnection);
    });
}

void RawDevelopDialog::developed(Choice choice) {
    if (worker_.joinable()) worker_.join();
    developing_ = false;
    developed_ = std::move(workerResult_);
    if (!developed_) {
        status_->setText(QString::fromStdString(workerError_));
        settle_->start();
        panels_->setEnabled(true);
        open_->setEnabled(true);
        if (openObject_) openObject_->setEnabled(true);
        if (depth_) depth_->setEnabled(true);
        return;
    }
    choice_ = choice;
    accept();
}

void RawDevelopDialog::reject() {
    // Cancel, Escape or closing: a decode under way stops, and nothing opens.
    settle_->stop();
    stopPreview();
    stopWorker();
    developing_ = false;
    choice_ = Choice::Cancelled;
    developed_.reset();
    QDialog::reject();
}

} // namespace app
