#include "PixelDialog.h"
#include "compositor/filters.h"
#include "compositor/modeedit.h"
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

std::shared_ptr<const Image16> previewCopy(const std::shared_ptr<const Image16>& source, int limit, double& scale) {
    int longest = std::max(source->width(), source->height());
    if (limit <= 0 || longest <= limit) { scale = 1; return source; }
    scale = double(limit) / longest;
    int w = std::max(1, int(source->width() * scale)), h = std::max(1, int(source->height() * scale));
    LayerTransform full(Point(0, 0), Size(source->width(), source->height()));
    return resampleLayer(source, full, full, w, h);
}

std::shared_ptr<Gray16> coverageCopy(const std::shared_ptr<Gray16>& coverage, int w, int h) {
    if (!coverage) return nullptr;
    LayerTransform full(Point(0, 0), Size(coverage->width(), coverage->height()));
    return resampleMask(*coverage, full, full, w, h, 0);
}

std::shared_ptr<const ImageF> previewCopy(const std::shared_ptr<const ImageF>& source, int limit, double& scale) {
    int longest = std::max(source->width(), source->height());
    if (limit <= 0 || longest <= limit) { scale = 1; return source; }
    scale = double(limit) / longest;
    int w = std::max(1, int(source->width() * scale)), h = std::max(1, int(source->height() * scale));
    LayerTransform full(Point(0, 0), Size(source->width(), source->height()));
    return resampleLayer(source, full, full, w, h);
}

std::shared_ptr<GrayF> coverageCopy(const std::shared_ptr<GrayF>& coverage, int w, int h) {
    if (!coverage) return nullptr;
    LayerTransform full(Point(0, 0), Size(coverage->width(), coverage->height()));
    return resampleMask(*coverage, full, full, w, h, 0.0f);
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
    source16_ = session_->adjustmentSource16(margin, transform_, layerId_);
    previewSource16_.reset();
    coverage16_.reset();
    previewCoverage16_.reset();
    sourceF_ = session_->adjustmentSourceF(margin, transform_, layerId_);
    previewSourceF_.reset();
    coverageF_.reset();
    previewCoverageF_.reset();
    curve_ = session_->documentCurve();
    previewScale_ = 1;
    sourceNative_.reset();
    coverageNative_ = {};
    mode_ = session_->colorMode();
    if (mode_ != ColorMode::RGB) {
        // CMYK and Lab: the layer's own samples only (a Lab layer would otherwise read as RGB).
        source_.reset(); source16_.reset(); sourceF_.reset();
        profile_ = session_->document() ? session_->document()->profile : ColorProfile();
        sourceNative_ = session_->adjustmentSourceAny(margin, transform_, layerId_);
        if (sourceNative_) coverageNative_ = session_->selectionOnGridAny(transform_, sourceNative_.width(), sourceNative_.height());
        return;
    }
    if (sourceF_) {
        previewSourceF_ = previewCopy(sourceF_, previewLimit, previewScale_);
        coverageF_ = session_->selectionOnGridF(transform_, sourceF_->width(), sourceF_->height());
        previewCoverageF_ = previewSourceF_ == sourceF_ ? coverageF_ : coverageCopy(coverageF_, previewSourceF_->width(), previewSourceF_->height());
        return;
    }
    if (source16_) {
        previewSource16_ = previewCopy(source16_, previewLimit, previewScale_);
        coverage16_ = session_->selectionOnGrid16(transform_, source16_->width(), source16_->height());
        previewCoverage16_ = previewSource16_ == source16_ ? coverage16_ : coverageCopy(coverage16_, previewSource16_->width(), previewSource16_->height());
        return;
    }
    if (!source_) return;
    previewSource_ = previewCopy(source_, previewLimit, previewScale_);
    coverage_ = session_->selectionOnGrid(transform_, source_->width(), source_->height());
    previewCoverage_ = previewSource_ == source_ ? coverage_ : coverageCopy(coverage_, previewSource_->width(), previewSource_->height());
}

void PixelDialog::captureAny(int margin) {
    if (!session_) return;
    source_.reset(); previewSource_.reset(); coverage_.reset(); previewCoverage_.reset();
    source16_.reset(); previewSource16_.reset(); coverage16_.reset(); previewCoverage16_.reset();
    sourceF_.reset(); previewSourceF_.reset(); coverageF_.reset(); previewCoverageF_.reset();
    curve_ = session_->documentCurve();
    previewScale_ = 1;
    mode_ = session_->colorMode();
    profile_ = session_->document() ? session_->document()->profile : ColorProfile();
    sourceNative_ = session_->adjustmentSourceAny(margin, transform_, layerId_);
    coverageNative_ = sourceNative_ ? session_->selectionOnGridAny(transform_, sourceNative_.width(), sourceNative_.height()) : AnyGray();
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

void PixelDialog::showPreview(std::shared_ptr<Image16> image, std::optional<LayerTransform> placement) {
    if (finished_ || !session_ || !image) return;
    if (previewCoverage16_ && previewSource16_) blendThroughCoverage(*image, *previewSource16_, *previewCoverage16_);
    session_->setPixelPreview(Image16Ptr(std::move(image)), placement, layerId_);
}

void PixelDialog::showPreview(std::shared_ptr<ImageF> image, std::optional<LayerTransform> placement) {
    if (finished_ || !session_ || !image) return;
    if (previewCoverageF_ && previewSourceF_) blendThroughCoverage(*image, *previewSourceF_, *previewCoverageF_);
    session_->setPixelPreview(ImageFPtr(std::move(image)), placement, layerId_);
}

void PixelDialog::showPreviewNative(AnyImage image, std::optional<LayerTransform> placement) {
    if (finished_ || !session_ || !image) return;
    session_->setPixelPreview(throughSelectionNative(image), placement, layerId_);
}

AnyImage PixelDialog::throughSelectionNative(const AnyImage& result) const {
    if (!coverageNative_ || !sourceNative_) return result;
    return blendThroughCoverageAny(result, sourceNative_, coverageNative_);
}

void PixelDialog::clearPreview() {
    if (session_) session_->clearPixelPreview();
}

void PixelDialog::throughSelection(Image& result) const {
    if (coverage_ && source_) blendThroughCoverage(result, *source_, *coverage_);
}

void PixelDialog::throughSelection(Image16& result) const {
    if (coverage16_ && source16_) blendThroughCoverage(result, *source16_, *coverage16_);
}

void PixelDialog::throughSelection(ImageF& result) const {
    if (coverageF_ && sourceF_) blendThroughCoverage(result, *sourceF_, *coverageF_);
}

void PixelDialog::commit(AnyImage image, const LayerTransform& placement, const QString& name) {
    finished_ = true;
    if (session_) session_->commitPixels(std::move(image), placement, name, layerId_);
}

bool PixelDialog::commitAsCommand(const QString& method, const QJsonObject& params) {
    if (!session_ || !session_->commandsRouted() || !layerId_ || !session_->document() || !session_->document()->find(*layerId_)) return false;
    finished_ = true;
    session_->clearPixelPreview();
    // The method acts on the active layer: the pinned one for the moment, as automation's own requests do.
    const auto previous = session_->activeLayerId();
    const bool previousMask = session_->isMaskSelected();
    const bool swap = previous != layerId_ || previousMask;
    if (swap) session_->selectLayer(layerId_, false);
    session_->runCommand(method, params);
    if (swap && previous && session_->document() && session_->document()->find(*previous)) session_->selectLayer(previous, previousMask);
    return true;
}

void PixelDialog::finish(int result) {
    finished_ = true;
    QDialog::done(result);
}

void PixelDialog::done(int result) {
    if (finished_ || !session_) { finish(result); return; }
    if (result == QDialog::Accepted && hasSource() && !apply()) return;
    if (!finished_) clearPreview();   // cancelled, or OK with nothing to change
    finish(result);
}

} // namespace app
