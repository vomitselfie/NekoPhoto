#include "PixelDialog.h"
#include "compositor/filters.h"
#include "compositor/render.h"
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QVBoxLayout>

using namespace compositor;

namespace app {

namespace {

// A copy no larger than `limit` on its longest side, for quick previews; `scale` reports the reduction.
std::shared_ptr<const Image> previewCopy(const std::shared_ptr<const Image>& source, int limit, double& scale) {
    int longest = std::max(source->width(), source->height());
    if (limit <= 0 || longest <= limit) { scale = 1; return source; }
    scale = double(limit) / longest;
    int w = std::max(1, int(source->width() * scale)), h = std::max(1, int(source->height() * scale));
    LayerTransform full(Point(0, 0), Size(source->width(), source->height()));
    return resampleLayer(source, full, full, w, h);
}

std::shared_ptr<GrayImage> coverageCopy(const std::shared_ptr<GrayImage>& coverage, int w, int h) {
    if (!coverage) return nullptr;
    LayerTransform full(Point(0, 0), Size(coverage->width(), coverage->height()));
    return resampleMask(*coverage, full, full, w, h, 0);
}

} // namespace

PixelDialog::PixelDialog(EditorSession* session, QWidget* parent)
    : QDialog(parent), session_(session), layerId_(session->activeLayerId()) {
    setModal(false);
    setAttribute(Qt::WA_DeleteOnClose);
    // Closing the tab takes the layer with it; there is nothing left to preview on or commit to.
    connect(session, &QObject::destroyed, this, [this] { finished_ = true; close(); });
}

PixelDialog::~PixelDialog() {
    if (!finished_ && session_) session_->clearPixelPreview();
}

void PixelDialog::capture(int margin, int previewLimit) {
    if (!session_) return;
    source_ = session_->adjustmentSource(margin, transform_, layerId_);
    previewSource_.reset();
    coverage_.reset();
    previewCoverage_.reset();
    previewScale_ = 1;
    if (!source_) return;
    previewSource_ = previewCopy(source_, previewLimit, previewScale_);
    coverage_ = session_->selectionOnGrid(transform_, source_->width(), source_->height());
    previewCoverage_ = previewSource_ == source_ ? coverage_ : coverageCopy(coverage_, previewSource_->width(), previewSource_->height());
}

QCheckBox* PixelDialog::addPreviewAndButtons(QVBoxLayout* layout) {
    preview_ = new QCheckBox(tr("Preview"));
    preview_->setChecked(true);
    layout->addWidget(preview_);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
    return preview_;
}

bool PixelDialog::previewing() const { return !finished_ && session_ && (!preview_ || preview_->isChecked()); }

void PixelDialog::showPreview(std::shared_ptr<Image> image, std::optional<LayerTransform> placement) {
    if (finished_ || !session_ || !image) return;
    if (previewCoverage_ && previewSource_) blendThroughCoverage(*image, *previewSource_, *previewCoverage_);
    session_->setPixelPreview(std::move(image), placement, layerId_);
}

void PixelDialog::clearPreview() {
    if (session_) session_->clearPixelPreview();
}

void PixelDialog::throughSelection(Image& result) const {
    if (coverage_ && source_) blendThroughCoverage(result, *source_, *coverage_);
}

void PixelDialog::commit(std::shared_ptr<const Image> image, const LayerTransform& placement, const QString& name) {
    finished_ = true;
    if (session_) session_->commitPixels(std::move(image), placement, name, layerId_);
}

void PixelDialog::finish(int result) {
    finished_ = true;
    QDialog::done(result);
}

void PixelDialog::done(int result) {
    if (finished_ || !session_) { finish(result); return; }
    if (result == QDialog::Accepted && source_ && !apply()) return;
    if (!finished_) clearPreview();   // cancelled, or OK with nothing to change
    finish(result);
}

} // namespace app
