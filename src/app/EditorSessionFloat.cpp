// EditorSession at 32 bits (docs/bit-depth.md, "32 bits"): the float counterparts of the 16-bit pixel edits. Fill and
// Clear through a float selection, the pixels under the selection for the clipboard and Free Transform, Distort of
// layers and its preview, and merging a transformed selection back. Colour is premultiplied linear light and may exceed
// 1; coverage stays 0..1. The shared session files branch here once per entry point.
#include "EditorSession.h"
#include "compositor/depth.h"
#include "compositor/colormgmt.h"
#include <algorithm>
#include <cmath>
#include <cstring>

using namespace compositor;

namespace app {

bool EditorSession::fillThroughF(const QColor& color, const GrayF* selection, const char* name) {
    Layer* layer = activeLayerMutable();
    if (!layer || layer->isGroup || layer->adjustment) return false;
    const bool mask = isMaskSelected_ && layer->mask;
    if (!mask && smartObjectBlocksPixels(true)) return false;
    auto coverageAt = [&](Point d) -> float {
        if (d.x < 0 || d.y < 0 || d.x >= document_->width || d.y >= document_->height) return 0.0f;
        return selection ? cleanCoverage(selection->at(std::min(int(d.x), selection->width() - 1), std::min(int(d.y), selection->height() - 1))) : 1.0f;
    };
    if (mask) {
        if (!layer->mask->asset.image.f32()) return false;
        auto out = std::make_shared<GrayF>(*layer->mask->asset.image.f32());
        const bool white = color.lightnessF() >= 0.5;
        const float value = (paintsQuickMask() ? !white : white) ? 1.0f : 0.0f;
        const Affine toDoc = layer->maskTransform().pixelToDocument(out->width(), out->height());
        for (int py = 0; py < out->height(); py++)
            for (int px = 0; px < out->width(); px++) {
                const float c = coverageAt(toDoc.apply({px + 0.5, py + 0.5}));
                if (!(c > 0)) continue;
                float& v = out->at(px, py);
                v = v + (value - v) * c;
            }
        beginEdit(name);
        layer->mask->asset = MaskAsset::make(GrayFPtr(out));
        endEdit();
        notifyDocument();
        return true;
    }
    // The layer's grid grown to take in the canvas, in its own pixel space (fillThrough16's steps).
    const ImageF* src = layer->asset ? layer->asset->image.f32().get() : nullptr;
    const int w = src ? src->width() : document_->width, h = src ? src->height() : document_->height;
    const LayerTransform base = src ? layer->transform : LayerTransform(Point(0, 0), document_->size());
    const Affine toPixels = base.pixelToDocument(w, h).inverted();
    const Rect extent = Rect(0, 0, w, h).unionWith(toPixels.mapBounds(document_->rect())).integral();
    if (extent.width > maxImageSide || extent.height > maxImageSide || extent.width * extent.height > double(Document::imagePixelBudget(SampleType::F32))) {
        emit error(tr("The filled layer would exceed the size limits."));
        return false;
    }
    auto working = std::make_shared<ImageF>(int(extent.width), int(extent.height));
    if (src) for (int y = 0; y < h; y++) std::memcpy(working->pixel(int(-extent.x), y + int(-extent.y)), src->row(y), size_t(w) * 4 * sizeof(float));
    LayerTransform grown = base;
    grown.size = {extent.width * base.size.width / w, extent.height * base.size.height / h};
    const Point center = base.pixelToDocument(w, h).apply({extent.midX(), extent.midY()});
    grown.origin = {center.x - grown.size.width / 2, center.y - grown.size.height / 2};
    const Affine toDoc = grown.pixelToDocument(working->width(), working->height());
    // The colour picked (encoded, as the colour pickers show it) linearised through the document's curve.
    const TransferCurve curve = documentCurve();
    const float fill[4] = {curve.toLinear(float(color.redF())), curve.toLinear(float(color.greenF())), curve.toLinear(float(color.blueF())), 1.0f};
    for (int py = 0; py < working->height(); py++)
        for (int px = 0; px < working->width(); px++) {
            const float c = coverageAt(toDoc.apply({px + 0.5, py + 0.5}));
            if (!(c > 0)) continue;
            float* p = working->pixel(px, py);
            for (int k = 0; k < 4; k++) p[k] = p[k] + (fill[k] - p[k]) * c;
        }
    LayerTransform placed;
    auto image = trimToPixels(*working, grown, placed);
    beginEdit(name);
    if (layer->mask && !layer->mask->placement && layer->asset) layer->mask->placement = layer->transform;
    layer->asset = Asset::make(ImageFPtr(image), layer->name);
    layer->transform = placed;
    layer->shapeImage.reset();
    endEdit();
    notifyDocument();
    return true;
}

void EditorSession::clearSelectedPixelsF(Layer& layer) {
    const GrayF* selection = document_->selection ? document_->selection->coverage.f32().get() : nullptr;
    if (!selection || !layer.asset || !layer.asset->image.f32()) return;
    const ImageF& src = *layer.asset->image.f32();
    auto out = std::make_shared<ImageF>(src);
    const Affine toDoc = layer.transform.pixelToDocument(src.width(), src.height());
    for (int y = 0; y < src.height(); y++)
        for (int x = 0; x < src.width(); x++) {
            const Point d = toDoc.apply({x + 0.5, y + 0.5});
            if (d.x < 0 || d.y < 0 || d.x >= document_->width || d.y >= document_->height) continue;
            const float c = cleanCoverage(selection->at(int(d.x), int(d.y)));
            if (!(c > 0)) continue;
            float* p = out->pixel(x, y);
            for (int k = 0; k < 4; k++) p[k] *= 1.0f - c;
        }
    layer.asset = Asset::make(ImageFPtr(out), layer.name);
    layer.shapeImage.reset();
}

std::optional<EditorSession::PixelClipboard> EditorSession::renderSelectedPixelsF(bool merged, const Rect& region) const {
    RenderOptions options;
    options.region = region;
    const Layer* layer = activeLayer();
    auto out = std::make_shared<ImageF>(int(region.width), int(region.height));
    if (merged) renderF(*document_, options, *out);
    else if (isMaskSelected_ && layer->mask) {
        // The mask as opaque gray, placed as it sits on the document.
        if (!layer->mask->asset.image.f32()) return std::nullopt;
        const float background = layer->mask->placement ? LayerMask::background(*layer->mask->asset.thumbnail) / 255.0f : 0.0f;
        GrayF gray(out->width(), out->height(), background);
        sampleMaskCoverage(*layer->mask->asset.image.f32(), layer->maskTransform(), region, 1, background, gray, false);
        // A mask's gray is coverage; shown as colour it is the encoded level, linearised.
        const TransferCurve curve = documentCurve();
        for (int y = 0; y < out->height(); y++)
            for (int x = 0; x < out->width(); x++) {
                float* p = out->pixel(x, y);
                p[0] = p[1] = p[2] = curve.toLinear(cleanCoverage(gray.at(x, y)));
                p[3] = 1.0f;
            }
    } else if (layer && layer->asset && layer->asset->image) {
        Document single(document_->width, document_->height);
        single.sampleType = document_->sampleType;
        single.profile = document_->profile;
        single.encodedProfile = document_->encodedProfile;
        Layer copy = *layer;
        copy.parentId.reset(); copy.visible = true; copy.opacity = 1; copy.blendMode = BlendMode::Normal; copy.maskSourceId.reset();
        single.layers = {copy};
        renderF(single, options, *out);
    } else return std::nullopt;
    if (document_->selection) {
        const GrayF* coverage = document_->selection->coverage.f32().get();
        if (!coverage) return std::nullopt;
        for (int y = 0; y < out->height(); y++)
            for (int x = 0; x < out->width(); x++) {
                const float k = cleanCoverage(coverage->at(x + int(region.x), y + int(region.y)));
                float* p = out->pixel(x, y);
                for (int c = 0; c < 4; c++) p[c] *= k;
            }
    }
    return PixelClipboard{ImageFPtr(out), QPointF(region.x, region.y)};
}

void EditorSession::distortLayerF(Layer& layer, const TransformEdit& edit) {
    // distortLayer16's steps at 32 bits.
    auto target = distortTarget(layer, edit);
    if (!target) return;
    Rect crop;
    auto warped = warpImageTrimmed(layer.asset->image.f32(), target->first, target->second, &crop);
    if (!warped) { emit error(tr("That shape can't be applied.")); return; }
    if (layer.mask && layer.mask->asset.image.f32()) {
        LayerMask& mask = *layer.mask;
        const GrayF& image = *mask.asset.image.f32();
        if (!mask.placement && mask.linked) {
            auto wm = warpMask(image, target->first, target->second, 0.0f, 0);
            if (wm && (wm->image->width() > 1 || wm->image->height() > 1))
                mask.asset = MaskAsset::make(GrayFPtr(cropGray(*wm->image, int(crop.x), int(crop.y), int(crop.width), int(crop.height))));
        } else if (mask.linked && mask.placement) {
            const LayerTransform placement = mask.placement->following(layer.transform, target->first);
            const Corners carried = carriedCorners(placement, target->first, target->second);
            if (cornersUsable(carried)) {
                auto wm = warpMask(image, placement, carried, LayerMask::background(*mask.asset.thumbnail) / 255.0f, 0);
                if (wm) { mask.asset = MaskAsset::make(GrayFPtr(wm->image)); mask.placement = wm->transform; }
            }
        } else if (!mask.placement) {
            mask.placement = layer.transform;
        }
    }
    layer.asset = Asset::make(ImageFPtr(warped->image), layer.name);
    layer.transform = warped->transform;
    layer.shapeImage.reset();
}

void EditorSession::mergeFloatingTransformF(const TransformEdit& edit) {
    // mergeFloatingTransform16's steps at 32 bits: the floating pixels drawn Normal onto the source layer's grid, grown
    // where they reach past it; its mask grows with it and the selection follows.
    const FloatingTransform& floating = *edit.floating;
    Layer* moving = document_->find(edit.layerId);
    Layer* source = document_->find(floating.sourceId);
    ImageFPtr pixels = moving->asset->image.f32();
    LayerTransform placed = edit.draft;
    if (edit.corners) {
        auto warped = warpImageTrimmed(pixels, edit.draft, *edit.corners);
        if (!warped) { cancelFloatingTransform(floating); return; }
        pixels = warped->image;
        placed = warped->transform;
    }
    const ImageF& src = *source->asset->image.f32();
    const int w = src.width(), h = src.height();
    const Affine toPixels = source->transform.pixelToDocument(w, h).inverted();
    const Rect floatBounds = toPixels.mapBounds(placed.pixelToDocument(pixels->width(), pixels->height()).mapBounds(Rect(0, 0, pixels->width(), pixels->height())));
    const Rect extent = Rect(0, 0, w, h).unionWith(floatBounds).integral();
    if (extent.width > 30000 || extent.height > 30000 || extent.width * extent.height > double(document_->imagePixelBudget())) { cancelFloatingTransform(floating); emit error(tr("The merged layer would exceed the size limits.")); return; }
    auto grown = std::make_shared<ImageF>(int(extent.width), int(extent.height));
    for (int y = 0; y < h; y++) std::memcpy(grown->pixel(int(-extent.x), y + int(-extent.y)), src.row(y), size_t(w) * 4 * sizeof(float));
    LayerTransform grownTransform = source->transform;
    grownTransform.size = {extent.width * source->transform.size.width / w, extent.height * source->transform.size.height / h};
    const Point center = source->transform.pixelToDocument(w, h).apply({extent.midX(), extent.midY()});
    grownTransform.origin = {center.x - grownTransform.size.width / 2, center.y - grownTransform.size.height / 2};
    auto onto = resampleLayer(pixels, placed, grownTransform, grown->width(), grown->height());
    for (int y = 0; y < grown->height(); y++) {
        const float* f = onto->row(y);
        float* d = grown->row(y);
        for (int x = 0; x < grown->width() * 4; x += 4) {
            const float keep = 1.0f - cleanCoverage(f[x + 3]);
            for (int c = 0; c < 3; c++) d[x + c] = f[x + c] + d[x + c] * keep;
            d[x + 3] = std::min(1.0f, f[x + 3] + d[x + 3] * keep);
        }
    }
    if (source->mask && !source->mask->placement && source->mask->asset.image.f32() && (extent.width != w || extent.height != h)) {
        const GrayF& old = *source->mask->asset.image.f32();
        auto mask = std::make_shared<GrayF>(grown->width(), grown->height(), 1.0f);
        if (old.width() == 1 && old.height() == 1) mask->fill(old.at(0, 0));
        else for (int y = 0; y < h; y++) std::memcpy(mask->row(y + int(-extent.y)) + int(-extent.x), old.row(y), size_t(w) * sizeof(float));
        source->mask->asset = MaskAsset::make(GrayFPtr(mask));
    }
    if (document_->selection && document_->selection->coverage.f32()) {
        if (auto moved = floatingSelectionF(edit)) document_->selection->coverage = GrayFPtr(moved);
    }
    source->asset = Asset::make(ImageFPtr(grown), source->name);
    source->transform = grownTransform;
    source->shapeImage.reset();
    const Uuid sourceId = source->id;
    document_->layers.erase(document_->layers.begin() + document_->indexOf(edit.layerId));
    setActiveLayer(sourceId);
    endEdit();
}

std::shared_ptr<GrayF> EditorSession::floatingSelectionF(const TransformEdit& edit) const {
    // The selection carried by a floating transform: through the distortion, or the affine move.
    if (!edit.floating || !document_ || !document_->selection || !document_->selection->coverage.f32()) return nullptr;
    const FloatingTransform& f = *edit.floating;
    const GrayF& cov = *document_->selection->coverage.f32();
    if (edit.corners) return warpCoverage(cov, f.original, f.pixelWidth, f.pixelHeight, *edit.corners);
    auto moved = std::make_shared<GrayF>(cov.width(), cov.height(), 0.0f);
    const Affine map = f.original.pixelToDocument(f.pixelWidth, f.pixelHeight).inverted().concatenating(edit.draft.pixelToDocument(f.pixelWidth, f.pixelHeight));
    const Affine inv = map.inverted();
    for (int y = 0; y < cov.height(); y++)
        for (int x = 0; x < cov.width(); x++) {
            const Point p = inv.apply({x + 0.5, y + 0.5});
            const int sx = int(std::floor(p.x)), sy = int(std::floor(p.y));
            if (sx >= 0 && sy >= 0 && sx < cov.width() && sy < cov.height()) moved->at(x, y) = cov.at(sx, sy);
        }
    return moved;
}

bool EditorSession::distortOverrideF(const Layer& layer, const TransformEdit& edit, LayerOverride& o) const {
    // The layer warped into the pending distortion at preview size, cached while nothing changes (as at 8 and 16 bits).
    if (!edit.corners || !layer.asset || !layer.asset->image.f32()) return false;
    auto target = distortTarget(layer, edit);
    if (!target) return true;
    const GrayFPtr maskImage = layer.mask && layer.mask->enabled ? layer.mask->asset.image.f32() : nullptr;
    auto it = distortCacheF_.find(layer.id);
    const bool fresh = it != distortCacheF_.end() && it->second.corners == target->second && it->second.transform == target->first
        && it->second.source == layer.asset->image.f32() && it->second.mask == maskImage;
    if (!fresh) {
        DistortCacheF cache;
        cache.corners = target->second;
        cache.transform = target->first;
        cache.source = layer.asset->image.f32();
        cache.mask = maskImage;
        cache.image = warpImage(layer.asset->image.f32(), target->first, target->second, 2048);
        if (cache.image && maskImage && !layer.mask->placement && layer.mask->linked) {
            auto wm = warpMask(*maskImage, target->first, target->second, 0.0f, 2048);
            if (wm) cache.warpedMask = wm->image;
        }
        it = distortCacheF_.insert_or_assign(layer.id, std::move(cache)).first;
    }
    const DistortCacheF& cache = it->second;
    if (cache.image) {
        o.imageF = ImageFPtr(cache.image->image);
        o.transform = cache.image->transform;
        if (cache.warpedMask) { o.maskImageF = GrayFPtr(cache.warpedMask); o.maskPlacement = std::optional<LayerTransform>(); }
        else if (layer.mask) o.maskPlacement = std::optional<LayerTransform>(layer.mask->placement ? *layer.mask->placement : layer.transform);
    }
    return true;
}

} // namespace app
