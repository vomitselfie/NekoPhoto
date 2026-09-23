// EditorSession: Free transform, distort and floating selections.
#include "EditorSession.h"
#include "QtGeometry.h"
#include "compositor/blend.h"
#include <algorithm>
#include <cstring>

using namespace compositor;

namespace app {

// ---- Transform -----------------------------------------------------------------

bool EditorSession::transformsAsGroup() const {
    const Layer* active = activeLayer();
    return selectedLayerIds_.size() > 1 || (selectedLayerIds_.size() == 1 && active && active->isGroup);
}

std::vector<const Layer*> EditorSession::groupTransformMembers() const {
    std::vector<const Layer*> result;
    if (!document_ || !transformsAsGroup()) return result;
    auto visible = effectiveVisibleIds(document_->layers);
    for (auto& layer : document_->layers) {
        if (!layer.asset || layer.isGroup || !visible.count(layer.id)) continue;
        std::optional<Uuid> current = layer.id;
        for (int i = 0; i < 64 && current; i++) {
            if (selectedLayerIds_.count(*current)) { result.push_back(&layer); break; }
            const Layer* l = document_->find(*current);
            current = l ? l->parentId : std::nullopt;
        }
    }
    return result;
}

std::optional<LayerTransform> EditorSession::groupTransformBox() const {
    double minX = 1e300, minY = 1e300, maxX = -1e300, maxY = -1e300;
    bool any = false;
    for (const Layer* l : groupTransformMembers()) for (auto& p : l->transform.corners()) { minX = std::min(minX, p.x); minY = std::min(minY, p.y); maxX = std::max(maxX, p.x); maxY = std::max(maxY, p.y); any = true; }
    if (!any) return std::nullopt;
    return LayerTransform(Point(minX, minY), Size(std::max(1.0, maxX - minX), std::max(1.0, maxY - minY)));
}

bool EditorSession::canTransform() const {
    if (!document_ || stroke_ || warp_ || pixelMove_) return false;
    if (transformsAsGroup()) return !groupTransformMembers().empty();
    const Layer* active = activeLayer();
    if (!active || active->isGroup) return false;
    if (isMaskSelected_ && active->mask && !active->mask->linked) return true;
    return active->asset.has_value() && effectiveVisibleIds(document_->layers).count(active->id);
}

bool EditorSession::canTransformSelection() const {
    if (transformEdit_ || !canEditLayers() || isMaskSelected_) return false;
    const Layer* active = activeLayer();
    return active && active->asset && !active->isGroup && document_->selection && document_->selection->coverage && !document_->selection->isEmpty();
}

void EditorSession::transformCommand() {
    if (canTransformSelection()) beginSelectionTransform();
    else { selectTool(Tool::Move); beginTransform(true); }
}

void EditorSession::beginTransform(bool persistent) {
    if (transformEdit_ || !canTransform()) return;
    resolveGradient();
    const Layer* layer = activeLayer();
    if (!layer) return;
    tool_ = Tool::Move;
    if (transformsAsGroup()) {
        auto box = groupTransformBox();
        if (!box) return;
        TransformGroup group;
        group.box = *box;
        for (const Layer* l : groupTransformMembers()) group.originals[l->id] = l->transform;
        TransformEdit edit{.layerId = layer->id, .draft = *box, .persistent = persistent, .mask = false};
        edit.group = group;
        transformEdit_ = edit;
    } else {
        bool maskAlone = isMaskSelected_ && layer->mask && !layer->mask->linked;
        transformEdit_ = TransformEdit{.layerId = layer->id, .draft = maskAlone ? layer->maskTransform() : layer->transform, .persistent = persistent, .mask = maskAlone};
    }
    emit toolChanged();
    emit transformChanged();
}

void EditorSession::beginSelectionTransform() {
    if (!canTransformSelection()) return;
    const Layer* source = activeLayer();
    auto lifted = renderSelectedPixels(false);
    if (!lifted) return;
    Document before = *document_;
    std::optional<Uuid> beforeActive = activeLayerId_;
    // Outer edit: closed by commitTransform (merge) or cancelTransform (restore).
    beginEdit("Transform Selection");
    Layer* src = document_->find(source->id);
    clearSelectedPixelsNow(*src);
    Layer floating(Asset::make(lifted->image, "Floating Selection"), toPoint(lifted->origin));
    floating.name = "Floating Selection";
    floating.parentId = src->parentId;
    floating.opacity = src->opacity;
    floating.blendMode = src->blendMode;
    int index = document_->indexOf(src->id);
    document_->layers.insert(document_->layers.begin() + index + 1, floating);
    setActiveLayer(floating.id);
    tool_ = Tool::Move;
    TransformEdit edit{.layerId = floating.id, .draft = floating.transform, .persistent = true, .mask = false};
    edit.floating = FloatingTransform{source->id, std::move(before), beforeActive, floating.transform, lifted->image->width(), lifted->image->height()};
    transformEdit_ = edit;
    emit toolChanged();
    notifyDocument();
    emit transformChanged();
}

void EditorSession::beginDuplicateTransform() {
    if (transformDuplicate_ || transformsAsGroup() || !activeLayerId_) return;
    Uuid source = *activeLayerId_;
    commitTransform();
    if (!canTransform()) return;
    beginEdit("Duplicate Layer");
    duplicateActiveLayer();
    if (!activeLayerId_ || *activeLayerId_ == source) { endEdit(); return; }
    transformDuplicate_ = std::make_pair(*activeLayerId_, source);
    beginTransform(false);
}

void EditorSession::previewTransform(const LayerTransform& value) {
    if (!transformEdit_ || !value.isValid()) return;
    transformEdit_->draft = value;
    emit documentChanged({});
    emit transformChanged();
}

void EditorSession::beginDistort() {
    if (!transformEdit_ || transformEdit_->corners || !transformEdit_->draft.isValid() || transformEdit_->mask) return;
    transformEdit_->corners = cornersOf(transformEdit_->draft);
    transformEdit_->persistent = true;
    emit transformChanged();
}

void EditorSession::previewCorners(const Corners& corners) {
    if (!transformEdit_ || !transformEdit_->corners || !cornersUsable(corners)) return;
    transformEdit_->corners = corners;
    emit documentChanged({});
    emit transformChanged();
}

Corners EditorSession::editedCorners(const Layer& layer) const {
    if (transformEdit_ && transformEdit_->corners && transformEdit_->layerId == layer.id) return *transformEdit_->corners;
    return cornersOf(editedTransform(layer));
}

std::optional<std::pair<LayerTransform, Corners>> EditorSession::distortTarget(const Layer& layer, const TransformEdit& edit) const {
    if (!edit.corners) return std::nullopt;
    if (!edit.group) return edit.layerId == layer.id ? std::optional(std::make_pair(edit.draft, *edit.corners)) : std::nullopt;
    auto it = edit.group->originals.find(layer.id);
    if (it == edit.group->originals.end()) return std::nullopt;
    LayerTransform transform = it->second.following(edit.group->box, edit.draft);
    Corners carried = carriedCorners(transform, edit.draft, *edit.corners);
    if (!cornersUsable(carried)) return std::nullopt;
    return std::make_pair(transform, carried);
}

void EditorSession::commitMaskTransform(const TransformEdit& edit) {
    Layer* layer = document_->find(edit.layerId);
    if (!layer || !layer->mask || !edit.draft.isValid()) return;
    std::optional<LayerTransform> placement = edit.draft.samePlacement(layer->transform) ? std::nullopt : std::optional<LayerTransform>(edit.draft);
    if (placement == layer->mask->placement) return;
    beginEdit("Transform Layer Mask");
    layer->mask->placement = placement;
    endEdit();
}

void EditorSession::commitDistort(const TransformEdit& edit) {
    distortCache_.clear();
    std::vector<Uuid> ids;
    if (edit.group) for (auto& [id, t] : edit.group->originals) ids.push_back(id); else ids.push_back(edit.layerId);
    beginEdit(edit.group ? "Distort Layers" : "Distort");
    for (auto& id : ids) {
        Layer* layer = document_->find(id);
        if (!layer || !layer->asset || !layer->asset->image) continue;
        auto target = distortTarget(*layer, edit);
        if (!target) continue;
        Rect crop;
        auto warped = warpImageTrimmed(layer->asset->image, target->first, target->second, &crop);
        if (!warped) { emit error(tr("That shape can't be applied.")); continue; }
        if (layer->mask && layer->mask->asset.image) {
            LayerMask& mask = *layer->mask;
            if (!mask.placement && mask.linked) {
                auto wm = warpMask(mask.asset.image, target->first, target->second, 0, 0);
                if (wm) {
                    if (wm->image->width() == 1 && wm->image->height() == 1) {}
                    else mask.asset = MaskAsset::make(cropGray(*wm->image, int(crop.x), int(crop.y), int(crop.width), int(crop.height)));
                }
            } else if (mask.linked && mask.placement) {
                LayerTransform placement = mask.placement->following(layer->transform, target->first);
                Corners carried = carriedCorners(placement, target->first, target->second);
                if (cornersUsable(carried)) {
                    auto wm = warpMask(mask.asset.image, placement, carried, LayerMask::background(*mask.asset.thumbnail), 0);
                    if (wm) { mask.asset = MaskAsset::make(wm->image); mask.placement = wm->transform; }
                }
            } else if (!mask.placement) {
                mask.placement = layer->transform;
            }
        }
        layer->asset = Asset::make(warped->image, layer->name);
        layer->transform = warped->transform;
        layer->shapeImage.reset();
    }
    endEdit();
}

void EditorSession::mergeFloatingTransform(const TransformEdit& edit) {
    const FloatingTransform& floating = *edit.floating;
    Layer* moving = document_->find(edit.layerId);
    Layer* source = document_->find(floating.sourceId);
    if (!moving || !source || !moving->asset || !source->asset || !edit.draft.isValid()) { cancelFloatingTransform(floating); return; }
    std::shared_ptr<const Image> pixels = moving->asset->image;
    LayerTransform placed = edit.draft;
    if (edit.corners) {
        auto warped = warpImageTrimmed(pixels, edit.draft, *edit.corners);
        if (!warped) { cancelFloatingTransform(floating); return; }
        pixels = warped->image;
        placed = warped->transform;
    }
    // Draw the floating pixels onto the source's own grid, growing it where they now extend past it.
    const Image& src = *source->asset->image;
    int w = src.width(), h = src.height();
    Affine toPixels = source->transform.pixelToDocument(w, h).inverted();
    Rect floatBounds = toPixels.mapBounds(placed.pixelToDocument(pixels->width(), pixels->height()).mapBounds(Rect(0, 0, pixels->width(), pixels->height())));
    Rect extent = Rect(0, 0, w, h).unionWith(floatBounds).integral();
    if (extent.width > 30000 || extent.height > 30000 || extent.width * extent.height > double(Document::pixelBudget)) { cancelFloatingTransform(floating); emit error(tr("The merged layer would exceed the size limits.")); return; }
    auto grown = std::make_shared<Image>(int(extent.width), int(extent.height));
    for (int y = 0; y < h; y++) std::memcpy(grown->pixel(int(-extent.x), y + int(-extent.y)), src.row(y), size_t(w) * 4);
    LayerTransform grownTransform = source->transform;
    grownTransform.size = {extent.width * source->transform.size.width / w, extent.height * source->transform.size.height / h};
    Point center = source->transform.pixelToDocument(w, h).apply({extent.midX(), extent.midY()});
    grownTransform.origin = {center.x - grownTransform.size.width / 2, center.y - grownTransform.size.height / 2};
    auto onto = resampleLayer(pixels, placed, grownTransform, grown->width(), grown->height());
    compositeImage(BlendMode::Normal, *onto, 1, *grown);
    if (source->mask && !source->mask->placement && source->mask->asset.image && (extent.width != w || extent.height != h)) {
        const GrayImage& old = *source->mask->asset.image;
        auto mask = std::make_shared<GrayImage>(grown->width(), grown->height(), 255);
        if (old.width() == 1 && old.height() == 1) mask->fill(old.at(0, 0));
        else for (int y = 0; y < h; y++) std::memcpy(mask->row(y + int(-extent.y)) + int(-extent.x), old.row(y), size_t(w));
        source->mask->asset = MaskAsset::make(mask);
    }
    // The selection follows the pixels.
    if (document_->selection && document_->selection->coverage) {
        std::shared_ptr<GrayImage> moved;
        if (edit.corners) moved = warpCoverage(*document_->selection->coverage, floating.original, floating.pixelWidth, floating.pixelHeight, *edit.corners);
        else {
            Affine map = floating.original.pixelToDocument(floating.pixelWidth, floating.pixelHeight).inverted().concatenating(edit.draft.pixelToDocument(floating.pixelWidth, floating.pixelHeight));
            Affine inv = map.inverted();
            const GrayImage& cov = *document_->selection->coverage;
            moved = std::make_shared<GrayImage>(cov.width(), cov.height(), 0);
            for (int y = 0; y < cov.height(); y++) for (int x = 0; x < cov.width(); x++) {
                Point p = inv.apply({x + 0.5, y + 0.5});
                int sx = int(std::floor(p.x)), sy = int(std::floor(p.y));
                if (sx >= 0 && sy >= 0 && sx < cov.width() && sy < cov.height()) moved->at(x, y) = cov.at(sx, sy);
            }
        }
        document_->selection->coverage = moved;
    }
    source->asset = Asset::make(grown, source->name);
    source->transform = grownTransform;
    source->shapeImage.reset();
    Uuid sourceId = source->id;
    document_->layers.erase(document_->layers.begin() + document_->indexOf(edit.layerId));
    setActiveLayer(sourceId);
    endEdit();
}

void EditorSession::cancelFloatingTransform(const FloatingTransform& floating) {
    document_ = floating.before;
    setActiveLayer(floating.beforeActive);
    endEdit();
}

void EditorSession::commitTransform() {
    snapGuidesX.clear();
    snapGuidesY.clear();
    endOpacityEdit();
    if (!transformEdit_) return;
    TransformEdit edit = *transformEdit_;
    transformEdit_.reset();
    distortCache_.clear();
    auto finishDuplicate = [&] { if (transformDuplicate_) { transformDuplicate_.reset(); endEdit(); } };
    if (edit.floating) {
        if (edit.draft == edit.floating->original && !edit.corners) cancelFloatingTransform(*edit.floating);
        else mergeFloatingTransform(edit);
        notifyDocument(); emit selectionChanged(); emit transformChanged();
        return;
    }
    if (edit.mask) { commitMaskTransform(edit); finishDuplicate(); notifyDocument(); emit transformChanged(); return; }
    if (edit.corners) { commitDistort(edit); finishDuplicate(); notifyDocument(); emit transformChanged(); return; }
    if (edit.group) {
        if (edit.draft.isValid()) {
            beginEdit("Transform Layers");
            for (auto& [id, original] : edit.group->originals) {
                Layer* layer = document_->find(id);
                if (!layer) continue;
                LayerTransform moved = original.following(edit.group->box, edit.draft);
                if (!moved.isValid()) continue;
                if (layer->mask) layer->mask->placement = layer->mask->placementMovingLayer(original, moved);
                layer->transform = moved;
                redrawShape(*layer);
            }
            endEdit();
        }
        finishDuplicate(); notifyDocument(); emit transformChanged();
        return;
    }
    Layer* layer = document_ ? document_->find(edit.layerId) : nullptr;
    if (layer && edit.draft.isValid() && !edit.draft.samePlacement(layer->transform)) {
        beginEdit("Transform Layer");
        if (layer->mask) layer->mask->placement = layer->mask->placementMovingLayer(layer->transform, edit.draft);
        layer->transform = edit.draft;
        redrawShape(*layer);
        endEdit();
    }
    finishDuplicate();
    notifyDocument();
    emit transformChanged();
}

void EditorSession::cancelTransform() {
    snapGuidesX.clear();
    snapGuidesY.clear();
    if (!transformEdit_) return;
    TransformEdit edit = *transformEdit_;
    transformEdit_.reset();
    distortCache_.clear();
    if (transformDuplicate_) {
        auto [copy, source] = *transformDuplicate_;
        document_->layers.erase(std::remove_if(document_->layers.begin(), document_->layers.end(), [&](const Layer& l) { return l.id == copy; }), document_->layers.end());
        setActiveLayer(source);
        transformDuplicate_.reset();
        endEdit();
    }
    if (edit.floating) cancelFloatingTransform(*edit.floating);
    notifyDocument();
    emit selectionChanged();
    emit transformChanged();
}

void EditorSession::nudgeLayer(double dx, double dy) {
    bool alreadyEditing = transformEdit_.has_value();
    if (!alreadyEditing) beginTransform(false);
    if (!transformEdit_) return;
    LayerTransform value = transformEdit_->draft;
    value.origin.x += dx;
    value.origin.y += dy;
    if (transformEdit_->corners) { Corners c = *transformEdit_->corners; for (auto& p : c) { p.x += dx; p.y += dy; } transformEdit_->corners = c; }
    previewTransform(value);
    if (!alreadyEditing) commitTransform();
}

LayerTransform EditorSession::displayedTransform(const Layer& layer) const {
    if (transformEdit_ && !transformEdit_->mask) {
        if (transformEdit_->group) { auto it = transformEdit_->group->originals.find(layer.id); if (it != transformEdit_->group->originals.end()) return it->second.following(transformEdit_->group->box, transformEdit_->draft); }
        else if (transformEdit_->layerId == layer.id) return transformEdit_->draft;
    }
    return layer.transform;
}

std::optional<LayerTransform> EditorSession::displayedMaskPlacement(const Layer& layer) const {
    if (!layer.mask) return std::nullopt;
    const LayerMask& mask = *layer.mask;
    if (transformEdit_ && transformEdit_->group) {
        auto it = transformEdit_->group->originals.find(layer.id);
        if (it == transformEdit_->group->originals.end()) return mask.placement;
        return mask.placementMovingLayer(layer.transform, it->second.following(transformEdit_->group->box, transformEdit_->draft));
    }
    if (!transformEdit_ || transformEdit_->layerId != layer.id || transformEdit_->floating) return mask.placement;
    if (transformEdit_->mask) return transformEdit_->draft.samePlacement(layer.transform) ? std::nullopt : std::optional(transformEdit_->draft);
    return mask.placementMovingLayer(layer.transform, transformEdit_->draft);
}

LayerTransform EditorSession::editedTransform(const Layer& layer) const {
    if (transformEdit_ && transformEdit_->layerId == layer.id) return transformEdit_->draft;
    if (!transformEdit_ && layer.id == activeLayerId_ && transformsAsGroup()) { if (auto box = groupTransformBox()) return *box; }
    if (layer.id == activeLayerId_ && isMaskSelected_ && layer.mask && !layer.mask->linked) return layer.maskTransform();
    return layer.transform;
}

void EditorSession::setSnapGuides(std::vector<double> xs, std::vector<double> ys) {
    snapGuidesX = std::move(xs);
    snapGuidesY = std::move(ys);
}

std::optional<Uuid> EditorSession::layerAt(QPointF documentPoint) const {
    if (!document_) return std::nullopt;
    auto layers = renderLayers(document_->layers);
    for (auto it = layers.rbegin(); it != layers.rend(); ++it) {
        const Layer* layer = *it;
        if (!layer->asset || !layer->asset->image) continue;
        LayerTransform t = displayedTransform(*layer);
        if (!t.contains(toPoint(documentPoint))) continue;
        Point p = t.pixelToDocument(layer->asset->image->width(), layer->asset->image->height()).inverted().apply(toPoint(documentPoint));
        int x = int(std::floor(p.x)), y = int(std::floor(p.y));
        if (x < 0 || y < 0 || x >= layer->asset->image->width() || y >= layer->asset->image->height()) continue;
        if (layer->asset->image->pixel(x, y)[3] > 0) return layer->id;
    }
    return std::nullopt;
}

std::optional<Size> EditorSession::transformPixelSize() const {
    if (transformEdit_ && transformEdit_->group) return transformEdit_->group->box.size;
    if (!transformEdit_ && transformsAsGroup()) { auto box = groupTransformBox(); return box ? std::optional(box->size) : std::nullopt; }
    if (transformEdit_ && transformEdit_->mask) return std::nullopt;
    if (transformEdit_ && transformEdit_->floating) return Size(transformEdit_->floating->pixelWidth, transformEdit_->floating->pixelHeight);
    const Layer* active = activeLayer();
    if (!active || !active->asset || !active->asset->image) return std::nullopt;
    return Size(active->asset->image->width(), active->asset->image->height());
}

void EditorSession::redrawShape(Layer& layer) {
    if (!layer.isLiveShape() || !layer.asset) return;
    int w = std::max(1, int(std::lround(layer.transform.size.width))), h = std::max(1, int(std::lround(layer.transform.size.height)));
    if ((w == layer.asset->image->width() && h == layer.asset->image->height()) || (long long)w * h > Document::pixelBudget) return;
    auto image = shapeImage(layer.shape->kind, w, h, layer.shape->red, layer.shape->green, layer.shape->blue, layer.shape->cornerRadius);
    // A mask that follows the layer's pixel grid stays exactly where it is while that grid changes size.
    if (layer.mask && !layer.mask->placement) layer.mask->placement = layer.maskTransform();
    layer.asset = Asset::make(image, layer.name);
    layer.shapeImage = image;
}

} // namespace app
