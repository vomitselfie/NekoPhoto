// EditorSession: Adjustment layers and the destructive adjustments and filters on pixels.
#include "EditorSession.h"
#include "Names.h"
#include "compositor/colormgmt.h"
#include "compositor/depth.h"
#include "compositor/filters.h"
#include "compositor/modeedit.h"
#include <random>

using namespace compositor;

namespace app {

// ---- Adjustment layers and pixel adjustments --------------------------------------------

void EditorSession::addAdjustmentLayer(AdjustmentKind kind) {
    if (refusedAtDepth(std::string("adjustment.") + adjustmentKindName(kind), tr("Adjustments"))) return;
    if (!canEditLayers() || document_->layers.size() >= size_t(Document::maxLayers)) return;
    Layer layer(names::adjustmentKind(kind).toStdString(), document_->size());
    AdjustmentSettings settings = AdjustmentSettings::defaults(kind);
    if (kind == AdjustmentKind::GradientMap) {
        settings.gradientMap.shadows = {foregroundColor.redF(), foregroundColor.greenF(), foregroundColor.blueF()};
        settings.gradientMap.highlights = {backgroundColor.redF(), backgroundColor.greenF(), backgroundColor.blueF()};
    }
    if (kind == AdjustmentKind::Grain) settings.grain.seed = uint32_t(std::random_device{}());
    layer.adjustment = settings.toLayerAdjustment();
    const Layer* active = activeLayer();
    layer.parentId = active && active->isGroup ? activeLayerId_ : (active ? active->parentId : std::nullopt);
    int index = activeLayerId_ ? document_->indexOf(*activeLayerId_) + 1 : int(document_->layers.size());
    beginEdit(QStringLiteral(QT_TRANSLATE_NOOP("History", "New %1 Adjustment")).arg(adjustmentKindName(kind)));
    document_->layers.insert(document_->layers.begin() + index, layer);
    if (layer.parentId) collapsedGroupIds.erase(*layer.parentId);
    setActiveLayer(layer.id);
    endEdit();
    notifyDocument();
}

void EditorSession::beginAdjustmentEdit() {
    if (adjustmentEditing_ || !document_) return;
    adjustmentEditing_ = true;
    beginEdit(QT_TRANSLATE_NOOP("History", "Adjustment"));
}

void EditorSession::setAdjustment(const Uuid& id, const AdjustmentSettings& settings) {
    if (refusedAtDepth(std::string("adjustment.") + adjustmentKindName(settings.kind), tr("Adjustments"))) return;
    if (!document_ || !settings.isValid()) return;
    Layer* layer = document_->find(id);
    if (!layer || !layer->adjustment) return;
    bool standalone = !adjustmentEditing_;
    if (standalone) beginEdit(QT_TRANSLATE_NOOP("History", "Adjustment"));
    layer->adjustment = settings.toLayerAdjustment();
    if (standalone) { endEdit(); notifyDocument(); return; }
    // A tick of the drag: the revision stays, so the canvas draws on from the frame it keeps below this layer. A
    // second layer changed in the same edit is a change like any other.
    if (adjustmentEditLayer_ && *adjustmentEditLayer_ != id) documentRevision_++;
    adjustmentEditLayer_ = id;
    adjustmentTick_ = true;
    emit documentChanged({});
    adjustmentTick_ = false;
}

void EditorSession::endAdjustmentEdit() {
    if (!adjustmentEditing_) return;
    adjustmentEditing_ = false;
    adjustmentEditLayer_.reset();
    endEdit();
    notifyDocument();
}

std::optional<AdjustmentSettings> EditorSession::adjustmentSettings(const Uuid& id) const {
    if (!document_) return std::nullopt;
    const Layer* layer = document_->find(id);
    if (!layer || !layer->adjustment) return std::nullopt;
    AdjustmentSettings settings;
    if (!AdjustmentSettings::parse(layer->adjustment->json, settings)) return std::nullopt;
    return settings;
}

bool EditorSession::canAdjustPixels() const {
    if (!canEditLayers()) return false;
    const Layer* layer = activeLayer();
    if (!layer || layer->isGroup || layer->adjustment || !layer->asset || !layer->asset->image || isMaskSelected_) return false;
    if (!effectiveVisibleIds(document_->layers).count(layer->id)) return false;
    if (document_->selection && document_->selection->isEmpty()) return false;
    if (layer->isLiveSmartObject()) return false;   // its contents: Edit Contents or Rasterize first
    return selectedLayerIds_.size() == 1;
}

void EditorSession::setPixelPreview(AnyImage image, std::optional<LayerTransform> transform, std::optional<Uuid> layerId) {
    previewImage_ = std::move(image);
    previewTransform_ = transform;
    previewLayerId_ = layerId ? layerId : activeLayerId_;
    emit documentChanged({});
}

void EditorSession::clearPixelPreview() {
    if (!previewImage_) return;
    previewImage_.reset();
    previewTransform_.reset();
    previewLayerId_.reset();
    emit documentChanged({});
}

std::shared_ptr<const Image> EditorSession::adjustmentSource(int margin, LayerTransform& transform, std::optional<Uuid> layerId) const {
    const Layer* layer = layerId ? (document_ ? document_->find(*layerId) : nullptr) : activeLayer();
    if (!layer || !layer->asset || !layer->asset->image.u8()) return nullptr;
    if (margin <= 0) { transform = layer->transform; return layer->asset->image.u8(); }
    return growImage(*layer->asset->image.u8(), layer->transform, margin, transform);
}

std::shared_ptr<GrayImage> EditorSession::selectionOnGrid(const LayerTransform& transform, int width, int height) const {
    if (!document_ || !document_->selection || !document_->selection->coverage.u8()) return nullptr;
    return selectionInGrid(*document_->selection->coverage.u8(), transform.pixelToDocument(width, height), width, height);
}

std::shared_ptr<const Image16> EditorSession::adjustmentSource16(int margin, LayerTransform& transform, std::optional<Uuid> layerId) const {
    const Layer* layer = layerId ? (document_ ? document_->find(*layerId) : nullptr) : activeLayer();
    if (!layer || !layer->asset || !layer->asset->image.u16()) return nullptr;
    if (margin <= 0) { transform = layer->transform; return layer->asset->image.u16(); }
    return growImage(*layer->asset->image.u16(), layer->transform, margin, transform);
}

std::shared_ptr<Gray16> EditorSession::selectionOnGrid16(const LayerTransform& transform, int width, int height) const {
    if (!document_ || !document_->selection || !document_->selection->coverage.u16()) return nullptr;
    return selectionInGrid(*document_->selection->coverage.u16(), transform.pixelToDocument(width, height), width, height);
}

std::shared_ptr<const ImageF> EditorSession::adjustmentSourceF(int margin, LayerTransform& transform, std::optional<Uuid> layerId) const {
    const Layer* layer = layerId ? (document_ ? document_->find(*layerId) : nullptr) : activeLayer();
    if (!layer || !layer->asset || !layer->asset->image.f32()) return nullptr;
    if (margin <= 0) { transform = layer->transform; return layer->asset->image.f32(); }
    return growImage(*layer->asset->image.f32(), layer->transform, margin, transform);
}

std::shared_ptr<GrayF> EditorSession::selectionOnGridF(const LayerTransform& transform, int width, int height) const {
    if (!document_ || !document_->selection || !document_->selection->coverage.f32()) return nullptr;
    return selectionInGrid(*document_->selection->coverage.f32(), transform.pixelToDocument(width, height), width, height);
}

AnyImage EditorSession::adjustmentSourceAny(int margin, LayerTransform& transform, std::optional<Uuid> layerId) const {
    const Layer* layer = layerId ? (document_ ? document_->find(*layerId) : nullptr) : activeLayer();
    if (!layer || !layer->asset || !layer->asset->image) return {};
    if (margin <= 0) { transform = layer->transform; return layer->asset->image; }
    return growImageAny(layer->asset->image, layer->transform, margin, transform);
}

AnyGray EditorSession::selectionOnGridAny(const LayerTransform& transform, int width, int height) const {
    if (!document_ || !document_->selection || !document_->selection->coverage) return {};
    return selectionInGridAny(document_->selection->coverage, transform.pixelToDocument(width, height), width, height);
}

TransferCurve EditorSession::documentCurve() const { return document_ ? encodedTransfer(*document_) : TransferCurve::srgb(); }

void EditorSession::commitPixels(AnyImage image, const LayerTransform& transform, const QString& name, std::optional<Uuid> layerId) {
    clearPixelPreview();
    Layer* layer = layerId ? (document_ ? document_->find(*layerId) : nullptr) : activeLayerMutable();
    if (!layer || !image) return;
    beginEdit(name);
    // A mask covering the old grid stays where it was when the layer grows.
    if (layer->mask && !layer->mask->placement && !transform.samePlacement(layer->transform)) layer->mask->placement = layer->transform;
    layer->asset = Asset::makeAny(image, layer->name);
    layer->transform = transform;
    layer->shapeImage.reset();
    endEdit();
    notifyDocument();
}

void EditorSession::invertActive() {
    if (refusedAtDepth("adjustment.Invert", tr("Adjustments"))) return;
    if (!canEditLayers()) return;
    Layer* layer = activeLayerMutable();
    if (!layer || layer->isGroup) return;
    if (isMaskSelected_ && layer->mask && layer->mask->asset.image.f32()) {
        const GrayF& before = *layer->mask->asset.image.f32();
        auto out = std::make_shared<GrayF>(before);
        applyInvert(*out);
        if (document_->selection && document_->selection->coverage.f32() && out->width() > 1) {
            auto coverage = selectionInGrid(*document_->selection->coverage.f32(), layer->maskTransform().pixelToDocument(out->width(), out->height()), out->width(), out->height());
            blendThroughCoverage(*out, before, *coverage);
        }
        beginEdit(QT_TRANSLATE_NOOP("History", "Invert"));
        layer->mask->asset = MaskAsset::make(GrayFPtr(out));
        endEdit();
        notifyDocument();
        return;
    }
    if (isMaskSelected_ && layer->mask && layer->mask->asset.image.u16()) {
        const Gray16& before = *layer->mask->asset.image.u16();
        auto out = std::make_shared<Gray16>(before);
        applyInvert(*out);
        if (document_->selection && document_->selection->coverage.u16() && out->width() > 1) {
            auto coverage = selectionInGrid(*document_->selection->coverage.u16(), layer->maskTransform().pixelToDocument(out->width(), out->height()), out->width(), out->height());
            blendThroughCoverage(*out, before, *coverage);
        }
        beginEdit(QT_TRANSLATE_NOOP("History", "Invert"));
        layer->mask->asset = MaskAsset::make(Gray16Ptr(out));
        endEdit();
        notifyDocument();
        return;
    }
    if (isMaskSelected_ && layer->mask) {
        auto out = std::make_shared<GrayImage>(*layer->mask->asset.image.u8());
        applyInvert(*out);
        if (document_->selection && document_->selection->coverage && out->width() > 1) {
            auto coverage = selectionInGrid(*document_->selection->coverage.u8(), layer->maskTransform().pixelToDocument(out->width(), out->height()), out->width(), out->height());
            blendThroughCoverage(*out, *layer->mask->asset.image.u8(), *coverage);
        }
        beginEdit(QT_TRANSLATE_NOOP("History", "Invert"));
        layer->mask->asset = MaskAsset::make(out);
        endEdit();
        notifyDocument();
        return;
    }
    if (layer->asset && layer->asset->image && colorMode() != ColorMode::RGB) {
        // CMYK and Lab: every channel as stored, the inks or L, a and b (modeedit.h).
        const AnyImage before = layer->asset->image;
        AnyImage out = adjustedInMode(AdjustmentSettings::defaults(AdjustmentKind::Invert), before, colorMode(), document_->profile);
        if (!out) return;
        if (AnyGray coverage = selectionOnGridAny(layer->transform, out.width(), out.height())) out = blendThroughCoverageAny(out, before, coverage);
        commitPixels(out, layer->transform, QT_TRANSLATE_NOOP("History", "Invert"));
        return;
    }
    if (layer->asset && layer->asset->image.f32()) {
        const ImageF& before = *layer->asset->image.f32();
        auto out = std::make_shared<ImageF>(before);
        applyInvert(*out, documentCurve());
        if (auto coverage = selectionOnGridF(layer->transform, out->width(), out->height())) blendThroughCoverage(*out, before, *coverage);
        commitPixels(ImageFPtr(out), layer->transform, QT_TRANSLATE_NOOP("History", "Invert"));
        return;
    }
    if (layer->asset && layer->asset->image.u16()) {
        const Image16& before = *layer->asset->image.u16();
        auto out = std::make_shared<Image16>(before);
        applyInvert(*out);
        if (auto coverage = selectionOnGrid16(layer->transform, out->width(), out->height())) blendThroughCoverage(*out, before, *coverage);
        commitPixels(Image16Ptr(out), layer->transform, QT_TRANSLATE_NOOP("History", "Invert"));
        return;
    }
    if (!layer->asset || !layer->asset->image.u8()) return;
    auto out = std::make_shared<Image>(*layer->asset->image.u8());
    applyInvert(*out);
    if (auto coverage = selectionOnGrid(layer->transform, out->width(), out->height())) blendThroughCoverage(*out, *layer->asset->image.u8(), *coverage);
    commitPixels(out, layer->transform, QT_TRANSLATE_NOOP("History", "Invert"));
}

std::array<std::vector<double>, 5> EditorSession::activeHistogramNative() const {
    const Layer* layer = activeLayer();
    if (!layer || !layer->asset || !layer->asset->image || colorMode() == ColorMode::RGB) return {};
    const AnyImage& image = layer->asset->image;
    return levelsHistogramInMode(image, colorMode(), selectionOnGridAny(layer->transform, image.width(), image.height()));
}

std::array<std::vector<double>, 4> EditorSession::activeHistogram() const {
    const Layer* layer = activeLayer();
    if (layer && layer->asset && layer->asset->image.f32()) {
        const ImageF& image = *layer->asset->image.f32();
        auto coverage = selectionOnGridF(layer->transform, image.width(), image.height());
        return levelsHistogram(image, coverage.get(), documentCurve());
    }
    if (layer && layer->asset && layer->asset->image.u16()) {
        const Image16& image = *layer->asset->image.u16();
        auto coverage = selectionOnGrid16(layer->transform, image.width(), image.height());
        return levelsHistogram(image, coverage.get());
    }
    if (!layer || !layer->asset || !layer->asset->image.u8()) return {};
    auto coverage = selectionOnGrid(layer->transform, layer->asset->image.u8()->width(), layer->asset->image.u8()->height());
    return levelsHistogram(*layer->asset->image.u8(), coverage.get());
}

void EditorSession::applySubjectMask(std::shared_ptr<const Gray16> mask, std::shared_ptr<const Image16> pixels, std::optional<Uuid> layerId) {
    if (refusedAtDepth("edit.removeBackground", tr("Remove Background"))) return;
    clearPixelPreview();
    Layer* layer = layerId ? (document_ ? document_->find(*layerId) : nullptr) : activeLayerMutable();
    if (!layer || !mask || !layer->asset || !layer->asset->image.u16()) return;
    const Image16& src = *layer->asset->image.u16();
    if (mask->width() != src.width() || mask->height() != src.height()) return;
    auto out = std::make_shared<Gray16>(*mask);
    // A mask already on the layer (in its own grid) is kept: what either one hides stays hidden.
    std::shared_ptr<const Gray16> existing;
    if (layer->mask && layer->mask->asset.image.u16() && !layer->mask->placement) {
        const Gray16& old = *layer->mask->asset.image.u16();
        if (old.width() == src.width() && old.height() == src.height()) existing = layer->mask->asset.image.u16();
        else if (old.width() == 1 && old.height() == 1) { auto e = std::make_shared<Gray16>(src.width(), src.height(), old.at(0, 0)); existing = e; }
    }
    const size_t count = size_t(src.width()) * size_t(src.height());
    if (existing) for (size_t i = 0; i < count; i++) out->data()[i] = uint16_t(mul15(out->data()[i], existing->data()[i]));
    if (auto coverage = selectionOnGrid16(layer->transform, src.width(), src.height())) {
        Gray16 base = existing ? *existing : Gray16(src.width(), src.height(), uint16_t(one16));
        blendThroughCoverage(*out, base, *coverage);
    }
    beginEdit(QT_TRANSLATE_NOOP("History", "Remove Background"));
    LayerMask m;
    if (layer->mask) { m = *layer->mask; m.placement.reset(); }
    m.asset = MaskAsset::make(Gray16Ptr(out));
    m.enabled = true;
    layer->mask = m;
    isMaskSelected_ = true;
    if (pixels && pixels->width() == out->width() && pixels->height() == out->height()) {
        layer->asset = Asset::makeAny(Image16Ptr(pixels), layer->name);
        layer->shapeImage.reset();
    }
    endEdit();
    notifyDocument();
}

void EditorSession::applySubjectMask(std::shared_ptr<const GrayImage> mask, std::shared_ptr<const Image> pixels, std::optional<Uuid> layerId) {
    if (refusedAtDepth("edit.removeBackground", tr("Remove Background"))) return;
    clearPixelPreview();
    Layer* layer = layerId ? (document_ ? document_->find(*layerId) : nullptr) : activeLayerMutable();
    if (!layer || !mask || !layer->asset || !layer->asset->image.u8()) return;
    const Image& src = *layer->asset->image.u8();
    if (mask->width() != src.width() || mask->height() != src.height()) return;
    auto out = std::make_shared<GrayImage>(*mask);
    // A mask already on the layer (in its own grid) is kept: what either one hides stays hidden.
    std::shared_ptr<const GrayImage> existing;
    if (layer->mask && layer->mask->asset.image.u8() && !layer->mask->placement) {
        const GrayImage& old = *layer->mask->asset.image.u8();
        if (old.width() == src.width() && old.height() == src.height()) existing = layer->mask->asset.image.u8();
        else if (old.width() == 1 && old.height() == 1) { auto e = std::make_shared<GrayImage>(src.width(), src.height(), old.at(0, 0)); existing = e; }
    }
    if (existing) for (size_t i = 0; i < out->byteCount(); i++) out->data()[i] = uint8_t((out->data()[i] * existing->data()[i] + 127) / 255);
    if (auto coverage = selectionOnGrid(layer->transform, src.width(), src.height())) {
        GrayImage base = existing ? *existing : GrayImage(src.width(), src.height(), 255);
        blendThroughCoverage(*out, base, *coverage);
    }
    beginEdit(QT_TRANSLATE_NOOP("History", "Remove Background"));
    LayerMask m;
    if (layer->mask) { m = *layer->mask; m.placement.reset(); }
    m.asset = MaskAsset::make(out);
    m.enabled = true;
    layer->mask = m;
    isMaskSelected_ = true;
    if (pixels && pixels->width() == out->width() && pixels->height() == out->height()) {
        layer->asset = Asset::make(pixels, layer->name);
        layer->shapeImage.reset();
    }
    endEdit();
    notifyDocument();
}

} // namespace app
