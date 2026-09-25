// EditorSession: Adjustment layers and the destructive adjustments and filters on pixels.
#include "EditorSession.h"
#include "compositor/filters.h"
#include <random>

using namespace compositor;

namespace app {

// ---- Adjustment layers and pixel adjustments --------------------------------------------

void EditorSession::addAdjustmentLayer(AdjustmentKind kind) {
    if (!canEditLayers() || document_->layers.size() >= size_t(Document::maxLayers)) return;
    Layer layer(adjustmentKindName(kind), document_->size());
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
    beginEdit(QStringLiteral("New %1 Adjustment").arg(adjustmentKindName(kind)));
    document_->layers.insert(document_->layers.begin() + index, layer);
    if (layer.parentId) collapsedGroupIds.erase(*layer.parentId);
    setActiveLayer(layer.id);
    endEdit();
    notifyDocument();
}

void EditorSession::beginAdjustmentEdit() {
    if (adjustmentEditing_ || !document_) return;
    adjustmentEditing_ = true;
    beginEdit("Adjustment");
}

void EditorSession::setAdjustment(const Uuid& id, const AdjustmentSettings& settings) {
    if (!document_ || !settings.isValid()) return;
    Layer* layer = document_->find(id);
    if (!layer || !layer->adjustment) return;
    bool standalone = !adjustmentEditing_;
    if (standalone) beginEdit("Adjustment");
    layer->adjustment = settings.toLayerAdjustment();
    if (standalone) { endEdit(); notifyDocument(); }
    else emit documentChanged({});
}

void EditorSession::endAdjustmentEdit() {
    if (!adjustmentEditing_) return;
    adjustmentEditing_ = false;
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

void EditorSession::setPixelPreview(std::shared_ptr<const Image> image, std::optional<LayerTransform> transform, std::optional<Uuid> layerId) {
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
    if (!layer || !layer->asset || !layer->asset->image) return nullptr;
    if (margin <= 0) { transform = layer->transform; return layer->asset->image; }
    return growImage(*layer->asset->image, layer->transform, margin, transform);
}

std::shared_ptr<GrayImage> EditorSession::selectionOnGrid(const LayerTransform& transform, int width, int height) const {
    if (!document_ || !document_->selection || !document_->selection->coverage) return nullptr;
    return selectionInGrid(*document_->selection->coverage, transform.pixelToDocument(width, height), width, height);
}

void EditorSession::commitPixels(std::shared_ptr<const Image> image, const LayerTransform& transform, const QString& name, std::optional<Uuid> layerId) {
    clearPixelPreview();
    Layer* layer = layerId ? (document_ ? document_->find(*layerId) : nullptr) : activeLayerMutable();
    if (!layer || !image) return;
    beginEdit(name);
    // A mask covering the old grid stays where it was when the layer grows.
    if (layer->mask && !layer->mask->placement && !transform.samePlacement(layer->transform)) layer->mask->placement = layer->transform;
    layer->asset = Asset::make(image, layer->name);
    layer->transform = transform;
    layer->shapeImage.reset();
    endEdit();
    notifyDocument();
}

void EditorSession::invertActive() {
    if (!canEditLayers()) return;
    Layer* layer = activeLayerMutable();
    if (!layer || layer->isGroup) return;
    if (isMaskSelected_ && layer->mask) {
        auto out = std::make_shared<GrayImage>(*layer->mask->asset.image);
        applyInvert(*out);
        if (document_->selection && document_->selection->coverage && out->width() > 1) {
            auto coverage = selectionInGrid(*document_->selection->coverage, layer->maskTransform().pixelToDocument(out->width(), out->height()), out->width(), out->height());
            blendThroughCoverage(*out, *layer->mask->asset.image, *coverage);
        }
        beginEdit("Invert");
        layer->mask->asset = MaskAsset::make(out);
        endEdit();
        notifyDocument();
        return;
    }
    if (!layer->asset || !layer->asset->image) return;
    auto out = std::make_shared<Image>(*layer->asset->image);
    applyInvert(*out);
    if (auto coverage = selectionOnGrid(layer->transform, out->width(), out->height())) blendThroughCoverage(*out, *layer->asset->image, *coverage);
    commitPixels(out, layer->transform, "Invert");
}

std::array<std::vector<double>, 4> EditorSession::activeHistogram() const {
    const Layer* layer = activeLayer();
    if (!layer || !layer->asset || !layer->asset->image) return {};
    auto coverage = selectionOnGrid(layer->transform, layer->asset->image->width(), layer->asset->image->height());
    return levelsHistogram(*layer->asset->image, coverage.get());
}

void EditorSession::applySubjectMask(std::shared_ptr<const GrayImage> mask, std::shared_ptr<const Image> pixels, std::optional<Uuid> layerId) {
    clearPixelPreview();
    Layer* layer = layerId ? (document_ ? document_->find(*layerId) : nullptr) : activeLayerMutable();
    if (!layer || !mask || !layer->asset || !layer->asset->image) return;
    const Image& src = *layer->asset->image;
    if (mask->width() != src.width() || mask->height() != src.height()) return;
    auto out = std::make_shared<GrayImage>(*mask);
    // A mask already on the layer (in its own grid) is kept: what either one hides stays hidden.
    std::shared_ptr<const GrayImage> existing;
    if (layer->mask && layer->mask->asset.image && !layer->mask->placement) {
        const GrayImage& old = *layer->mask->asset.image;
        if (old.width() == src.width() && old.height() == src.height()) existing = layer->mask->asset.image;
        else if (old.width() == 1 && old.height() == 1) { auto e = std::make_shared<GrayImage>(src.width(), src.height(), old.at(0, 0)); existing = e; }
    }
    if (existing) for (size_t i = 0; i < out->byteCount(); i++) out->data()[i] = uint8_t((out->data()[i] * existing->data()[i] + 127) / 255);
    if (auto coverage = selectionOnGrid(layer->transform, src.width(), src.height())) {
        GrayImage base = existing ? *existing : GrayImage(src.width(), src.height(), 255);
        blendThroughCoverage(*out, base, *coverage);
    }
    beginEdit("Remove Background");
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
