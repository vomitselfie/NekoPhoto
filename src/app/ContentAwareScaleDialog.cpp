#include "ContentAwareScaleDialog.h"
#include "EditorSession.h"
#include "ImageConvert.h"
#include "compositor/seamcarve.h"
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPixmap>
#include <QPushButton>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <cstring>

using namespace compositor;

namespace app {

namespace { constexpr int previewSide = 360; }

ContentAwareScaleDialog::ContentAwareScaleDialog(EditorSession* session, QWidget* parent) : QDialog(parent), session_(session) {
    setWindowTitle(tr("Content-Aware Scale"));
    setAttribute(Qt::WA_DeleteOnClose);
    auto* layout = new QVBoxLayout(this);
    preview_ = new QLabel(this);
    preview_->setAlignment(Qt::AlignCenter);
    preview_->setMinimumSize(previewSide, previewSide);
    layout->addWidget(preview_);
    auto* form = new QFormLayout;
    auto percent = [this](const QString& name) {
        auto* box = new QDoubleSpinBox(this);
        box->setObjectName(name);
        box->setRange(10, 300); box->setDecimals(1); box->setSuffix(QStringLiteral(" %")); box->setValue(100);
        return box;
    };
    width_ = percent("casWidth");
    height_ = percent("casHeight");
    protect_ = new QCheckBox(tr("Protect the selection"), this);
    size_ = new QLabel(this);
    form->addRow(tr("Width:"), width_);
    form->addRow(tr("Height:"), height_);
    form->addRow(QString(), protect_);
    form->addRow(tr("Result:"), size_);
    layout->addLayout(form);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, &ContentAwareScaleDialog::apply);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    // A reduced copy of the layer (and of the selection over it) for the preview.
    const Document* doc = session_->document() ? &*session_->document() : nullptr;
    const Layer* layer = session_->activeLayer();
    if (doc && layer && !layer->isGroup && layer->asset && layer->asset->image.u8()) {
        const Image& src = *layer->asset->image.u8();
        pixelWidth_ = src.width(); pixelHeight_ = src.height();
        const double f = std::min(1.0, double(previewSide) / std::max(pixelWidth_, pixelHeight_));
        const int tw = std::max(1, int(std::lround(pixelWidth_ * f))), th = std::max(1, int(std::lround(pixelHeight_ * f)));
        thumb_ = std::make_shared<Image>(tw, th);
        for (int y = 0; y < th; y++) for (int x = 0; x < tw; x++) {
            int sx = std::min(pixelWidth_ - 1, int((x + 0.5) / f)), sy = std::min(pixelHeight_ - 1, int((y + 0.5) / f));
            std::memcpy(thumb_->pixel(x, y), src.pixel(sx, sy), 4);
        }
        if (doc->selection && doc->selection->coverage) {
            const GrayImage& cov = *doc->selection->coverage.u8();
            const Affine toDoc = layer->transform.pixelToDocument(pixelWidth_, pixelHeight_);
            thumbProtect_ = std::make_shared<GrayImage>(tw, th, 0);
            for (int y = 0; y < th; y++) for (int x = 0; x < tw; x++) {
                Point p = toDoc.apply({(x + 0.5) / f, (y + 0.5) / f});
                int dx = int(std::floor(p.x)), dy = int(std::floor(p.y));
                if (dx >= 0 && dy >= 0 && dx < cov.width() && dy < cov.height()) thumbProtect_->at(x, y) = cov.at(dx, dy) >= 128 ? 255 : 0;
            }
            protect_->setChecked(true);
        } else protect_->setEnabled(false);
    } else {
        preview_->setText(tr("Select an image layer."));
        buttons->button(QDialogButtonBox::Ok)->setEnabled(false);
    }
    timer_.setSingleShot(true);
    timer_.setInterval(60);
    connect(&timer_, &QTimer::timeout, this, &ContentAwareScaleDialog::updatePreview);
    auto changed = [this] { timer_.start(); };
    connect(width_, &QDoubleSpinBox::valueChanged, this, changed);
    connect(height_, &QDoubleSpinBox::valueChanged, this, changed);
    connect(protect_, &QCheckBox::toggled, this, changed);
    updatePreview();
}

ContentAwareScaleDialog::~ContentAwareScaleDialog() = default;

void ContentAwareScaleDialog::updatePreview() {
    if (!thumb_) return;
    const double sx = width_->value() / 100, sy = height_->value() / 100;
    size_->setText(tr("%1 x %2 px").arg(std::max(1, int(std::lround(pixelWidth_ * sx)))).arg(std::max(1, int(std::lround(pixelHeight_ * sy)))));
    SeamCarveOptions options;
    if (protect_->isChecked() && thumbProtect_) options.protect = thumbProtect_.get();
    Image out = seamCarve(*thumb_, std::max(1, int(std::lround(thumb_->width() * sx))), std::max(1, int(std::lround(thumb_->height() * sy))), options);
    if (out.isEmpty()) return;
    QImage shown = toQImage(out);
    const int side = std::max(shown.width(), shown.height());
    if (side > previewSide) shown = shown.scaled(previewSide, previewSide, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    preview_->setPixmap(QPixmap::fromImage(shown));
}

void ContentAwareScaleDialog::apply() {
    const int w = std::max(1, int(std::lround(pixelWidth_ * width_->value() / 100)));
    const int h = std::max(1, int(std::lround(pixelHeight_ * height_->value() / 100)));
    QString error;
    if (!session_->contentAwareScale(w, h, protect_->isChecked() && protect_->isEnabled(), &error)) {
        QMessageBox::warning(this, windowTitle(), error);
        return;
    }
    accept();
}

} // namespace app
