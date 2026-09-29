// EditorSession in CMYK and Lab documents (docs/color-modes.md, "Transforms, the clipboard and Place"): the pixels
// under the selection at the document's own layout for the clipboard and Free Transform, Distort of CMYK layers and its
// preview, merging a transformed selection back, and the conversions through the profiles when pixels move between
// documents of different modes. Lab layers are 4-sample buffers the RGB paths resample as they are (modetransform.h);
// what differs is where they come from and go to.
#include "EditorSession.h"
#include "ColorManagement.h"
#include "ImageConvert.h"
#include "compositor/depth.h"
#include "compositor/modetransform.h"
#include <algorithm>
#include <cmath>
#include <cstring>

using namespace compositor;

namespace app {

namespace {

/// Every sample of `image` (any layout, 8 or 16 bits) times the coverage at (x + dx, y + dy), rounded as the RGB
/// clipboard rounds.
AnyImage timesCoverage(const AnyImage& image, const AnyGray& coverage, int dx, int dy) {
    auto apply = [&](const auto& src, const auto& cov, uint32_t full) {
        using Img = std::remove_cvref_t<decltype(src)>;
        using Sample = std::remove_cv_t<std::remove_pointer_t<decltype(src.row(0))>>;
        auto out = std::make_shared<Img>(src);
        const int n = src.channels();
        for (int y = 0; y < out->height(); y++)
            for (int x = 0; x < out->width(); x++) {
                const uint32_t k = std::min<uint32_t>(cov.at(x + dx, y + dy), full);
                Sample* p = out->pixel(x, y);
                for (int c = 0; c < n; c++) p[c] = Sample((p[c] * k + full / 2) / full);
            }
        return out;
    };
    if (image.c8() && coverage.u8()) return ImageC8Ptr(apply(*image.c8(), *coverage.u8(), 255u));
    if (image.u16() && coverage.u16()) return Image16Ptr(apply(*image.u16(), *coverage.u16(), one16));
    if (image.u8() && coverage.u8()) {
        // Image is not ImageT: its four samples the same way.
        const Image& src = *image.u8();
        const GrayImage& cov = *coverage.u8();
        auto out = std::make_shared<Image>(src);
        for (int y = 0; y < out->height(); y++)
            for (int x = 0; x < out->width(); x++) {
                const uint32_t k = cov.at(x + dx, y + dy);
                uint8_t* p = out->pixel(x, y);
                for (int c = 0; c < 4; c++) p[c] = uint8_t((p[c] * k + 127) / 255);
            }
        return ImagePtr(out);
    }
    return {};
}

} // namespace

Asset EditorSession::modeAsset(const AnyImage& image, const std::string& name) const {
    Asset asset = Asset::makeAny(image, name);
    if (document_ && document_->colorMode != ColorMode::RGB) asset.thumbnail = modeThumbnail(image, document_->colorMode, document_->profile);
    return asset;
}

std::optional<EditorSession::PixelClipboard> EditorSession::renderSelectedPixelsNative(bool merged, const Rect& region) const {
    const Document& doc = *document_;
    const Layer* layer = activeLayer();
    RenderOptions options;
    options.region = region;
    AnyImage out;
    if (merged) out = renderNative(doc, options);
    else if (layer && isMaskSelected_ && layer->mask) {
        // The mask as opaque gray, placed as it sits on the document: a gray in sRGB, then into the mode through the
        // profile, as Photoshop pastes gray into a colour document.
        const int w = int(region.width), h = int(region.height);
        AnyImage gray;
        const uint8_t background8 = layer->mask->placement ? LayerMask::background(*layer->mask->asset.thumbnail) : 0;
        if (layer->mask->asset.image.u16()) {
            Gray16 g(w, h, widen8(background8));
            sampleMaskCoverage(*layer->mask->asset.image.u16(), layer->maskTransform(), region, 1, g.at(0, 0), g, false);
            auto rgb = std::make_shared<Image16>(w, h);
            for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) { uint16_t* p = rgb->pixel(x, y); p[0] = p[1] = p[2] = g.at(x, y); p[3] = uint16_t(one16); }
            gray = Image16Ptr(rgb);
        } else if (layer->mask->asset.image.u8()) {
            GrayImage g(w, h, background8);
            sampleMaskCoverage(*layer->mask->asset.image.u8(), layer->maskTransform(), region, 1, g.at(0, 0), g, false);
            auto rgb = std::make_shared<Image>(w, h);
            for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) { uint8_t* p = rgb->pixel(x, y); p[0] = p[1] = p[2] = g.at(x, y); p[3] = 255; }
            gray = ImagePtr(rgb);
        } else return std::nullopt;
        out = convertImage(gray, ColorMode::RGB, ColorProfile(), doc.colorMode, doc.profile, color::conversionOptions());
    } else if (layer && layer->asset && layer->asset->image) {
        Document single(doc.width, doc.height);
        single.sampleType = doc.sampleType;
        single.colorMode = doc.colorMode;
        single.profile = doc.profile;
        Layer copy = *layer;
        copy.parentId.reset(); copy.visible = true; copy.opacity = 1; copy.blendMode = BlendMode::Normal; copy.maskSourceId.reset();
        single.layers = {copy};
        out = renderNative(single, options);
    } else return std::nullopt;
    if (!out) return std::nullopt;
    if (doc.selection) {
        out = timesCoverage(out, doc.selection->coverage, int(region.x), int(region.y));
        if (!out) return std::nullopt;
    }
    return PixelClipboard{out, QPointF(region.x, region.y), doc.colorMode, doc.profile};
}

AnyImage EditorSession::pixelsForDocument(const AnyImage& image, ColorMode mode, const ColorProfile& profile) const {
    if (!document_ || !image) return image;
    const Document& doc = *document_;
    if (mode == ColorMode::RGB && doc.colorMode == ColorMode::RGB) return image;   // as it always was
    const ConvertOptions options = color::conversionOptions();
    // A 32-bit source is RGB: encoded to 16 bits through its own curve first (there is no 32-bit CMYK or Lab).
    AnyImage source = image;
    ColorProfile from = profile;
    if (source.f32()) {
        from = gammaCounterpart(profile);
        const TransferCurve curve = TransferCurve::ofProfile(from);
        source = imageAtDepth(source, SampleType::U16, &curve);
    }
    // To the depth the conversion runs at (the document's; 16 bits for a 32-bit one), in the source's own mode, so an
    // 8-bit file converted into a 16-bit document keeps the 16-bit result's precision.
    const SampleType depth = doc.sampleType == SampleType::F32 ? SampleType::U16 : doc.sampleType;
    source = imageAtFormat(source, depth, mode);
    if (!source) return {};
    if (doc.colorMode == ColorMode::RGB) {
        // CMYK or Lab into RGB: through the profiles into the document's (its encoded profile at 32 bits; addPixelLayer
        // and insertImage then linearise it).
        const ColorProfile target = doc.sampleType == SampleType::F32 ? encodedProfileOf(doc) : doc.profile;
        return convertImage(source, mode, from, ColorMode::RGB, target, options);
    }
    if (mode == doc.colorMode && equivalentProfiles(from, doc.profile)) return source;
    return convertImage(source, mode, from, doc.colorMode, doc.profile, options);
}

QImage EditorSession::clipboardImageFor(const AnyImage& image) const {
    if (!document_ || !image) return {};
    // CMYK and Lab: the pixels through the document's profile to sRGB at their depth, then 8 bits.
    AnyImage rgb = convertImage(image, document_->colorMode, document_->profile, ColorMode::RGB, ColorProfile(), color::conversionOptions());
    if (rgb.u16()) return toQImage(*narrowImage(*rgb.u16())).convertToFormat(QImage::Format_ARGB32);
    return rgb.u8() ? toQImage(*rgb.u8()).convertToFormat(QImage::Format_ARGB32) : QImage();
}

void EditorSession::distortLayerAny(Layer& layer, const TransformEdit& edit) {
    // distortLayer16's steps over every sample of a CMYK layer, its mask at the document's depth.
    auto target = distortTarget(layer, edit);
    if (!target) return;
    Rect crop;
    auto warped = warpImageTrimmedAny(layer.asset->image, target->first, target->second, &crop);
    if (!warped) { emit error(tr("That shape can't be applied.")); return; }
    if (layer.mask) {
        LayerMask& mask = *layer.mask;
        if (!mask.placement && mask.linked) {
            if (const Gray16Ptr& m = mask.asset.image.u16()) {
                auto wm = warpMask(*m, target->first, target->second, 0, 0);
                if (wm && (wm->image->width() > 1 || wm->image->height() > 1))
                    mask.asset = MaskAsset::make(Gray16Ptr(cropGray(*wm->image, int(crop.x), int(crop.y), int(crop.width), int(crop.height))));
            } else if (const GrayPtr& m8 = mask.asset.image.u8()) {
                auto wm = warpMask(m8, target->first, target->second, 0, 0);
                if (wm && (wm->image->width() > 1 || wm->image->height() > 1))
                    mask.asset = MaskAsset::make(GrayPtr(cropGray(*wm->image, int(crop.x), int(crop.y), int(crop.width), int(crop.height))));
            }
        } else if (mask.linked && mask.placement) {
            const LayerTransform placement = mask.placement->following(layer.transform, target->first);
            const Corners carried = carriedCorners(placement, target->first, target->second);
            if (cornersUsable(carried)) {
                const uint8_t background = LayerMask::background(*mask.asset.thumbnail);
                if (const Gray16Ptr& m = mask.asset.image.u16()) {
                    auto wm = warpMask(*m, placement, carried, widen8(background), 0);
                    if (wm) { mask.asset = MaskAsset::make(Gray16Ptr(wm->image)); mask.placement = wm->transform; }
                } else if (const GrayPtr& m8 = mask.asset.image.u8()) {
                    auto wm = warpMask(m8, placement, carried, background, 0);
                    if (wm) { mask.asset = MaskAsset::make(wm->image); mask.placement = wm->transform; }
                }
            }
        } else if (!mask.placement) {
            mask.placement = layer.transform;
        }
    }
    layer.asset = modeAsset(warped->image, layer.name);
    layer.transform = warped->transform;
    layer.shapeImage.reset();
}

bool EditorSession::distortOverrideAny(const Layer& layer, const TransformEdit& edit, LayerOverride& o) const {
    // A CMYK layer warped into the pending distortion at preview size, cached while nothing changes.
    if (!edit.corners || !layer.asset || !isFiveSample(layer.asset->image)) return false;
    auto target = distortTarget(layer, edit);
    if (!target) return true;
    const AnyGray maskImage = layer.mask && layer.mask->enabled ? layer.mask->asset.image : AnyGray();
    auto it = distortCacheAny_.find(layer.id);
    const bool fresh = it != distortCacheAny_.end() && it->second.corners == target->second && it->second.transform == target->first
        && it->second.source == layer.asset->image && it->second.mask == maskImage;
    if (!fresh) {
        DistortCacheAny cache;
        cache.corners = target->second;
        cache.transform = target->first;
        cache.source = layer.asset->image;
        cache.mask = maskImage;
        cache.image = warpImageAny(layer.asset->image, target->first, target->second, 2048);
        if (cache.image && maskImage && !layer.mask->placement && layer.mask->linked) {
            if (maskImage.u16()) { if (auto wm = warpMask(*maskImage.u16(), target->first, target->second, 0, 2048)) cache.warpedMask = Gray16Ptr(wm->image); }
            else if (maskImage.u8()) { if (auto wm = warpMask(maskImage.u8(), target->first, target->second, 0, 2048)) cache.warpedMask = GrayPtr(wm->image); }
        }
        it = distortCacheAny_.insert_or_assign(layer.id, std::move(cache)).first;
    }
    const DistortCacheAny& cache = it->second;
    if (cache.image) {
        if (cache.image->image.c8()) o.imageC8 = cache.image->image.c8();
        else o.image16 = cache.image->image.u16();
        o.transform = cache.image->transform;
        if (cache.warpedMask) {
            if (cache.warpedMask.u16()) o.maskImage16 = cache.warpedMask.u16(); else o.maskImage = cache.warpedMask.u8();
            o.maskPlacement = std::optional<LayerTransform>();
        } else if (layer.mask) o.maskPlacement = std::optional<LayerTransform>(layer.mask->placement ? *layer.mask->placement : layer.transform);
    }
    return true;
}

void EditorSession::mergeFloatingTransformAny(const TransformEdit& edit) {
    // mergeFloatingTransform16's steps over a CMYK layer's five samples: the floating pixels (warped when distorted)
    // drawn Normal onto the source layer's grid, grown where they reach past it; its mask grows with it and the selection
    // follows.
    const FloatingTransform& floating = *edit.floating;
    Layer* moving = document_->find(edit.layerId);
    Layer* source = document_->find(floating.sourceId);
    AnyImage pixels = moving->asset->image;
    LayerTransform placed = edit.draft;
    if (edit.corners) {
        auto warped = warpImageTrimmedAny(pixels, edit.draft, *edit.corners);
        if (!warped) { cancelFloatingTransform(floating); return; }
        pixels = warped->image;
        placed = warped->transform;
    }
    const AnyImage& src = source->asset->image;
    const int w = src.width(), h = src.height();
    const Affine toPixels = source->transform.pixelToDocument(w, h).inverted();
    const Rect floatBounds = toPixels.mapBounds(placed.pixelToDocument(pixels.width(), pixels.height()).mapBounds(Rect(0, 0, pixels.width(), pixels.height())));
    const Rect extent = Rect(0, 0, w, h).unionWith(floatBounds).integral();
    if (extent.width > 30000 || extent.height > 30000 || extent.width * extent.height > double(document_->imagePixelBudget())) { cancelFloatingTransform(floating); emit error(tr("The merged layer would exceed the size limits.")); return; }
    const AnyImage grown = placeInAny(src, int(extent.width), int(extent.height), int(-extent.x), int(-extent.y));
    LayerTransform grownTransform = source->transform;
    grownTransform.size = {extent.width * source->transform.size.width / w, extent.height * source->transform.size.height / h};
    const Point center = source->transform.pixelToDocument(w, h).apply({extent.midX(), extent.midY()});
    grownTransform.origin = {center.x - grownTransform.size.width / 2, center.y - grownTransform.size.height / 2};
    const AnyImage onto = resampleLayerAny(pixels, placed, grownTransform, grown.width(), grown.height());
    const AnyImage merged = compositeOverAny(onto, grown);
    if (source->mask && !source->mask->placement && (extent.width != w || extent.height != h)) {
        if (const Gray16Ptr& old = source->mask->asset.image.u16()) {
            auto mask = std::make_shared<Gray16>(grown.width(), grown.height(), uint16_t(one16));
            if (old->width() == 1 && old->height() == 1) mask->fill(old->at(0, 0));
            else for (int y = 0; y < h; y++) std::memcpy(mask->row(y + int(-extent.y)) + int(-extent.x), old->row(y), size_t(w) * sizeof(uint16_t));
            source->mask->asset = MaskAsset::make(Gray16Ptr(mask));
        } else if (const GrayPtr& old8 = source->mask->asset.image.u8()) {
            auto mask = std::make_shared<GrayImage>(grown.width(), grown.height(), 255);
            if (old8->width() == 1 && old8->height() == 1) mask->fill(old8->at(0, 0));
            else for (int y = 0; y < h; y++) std::memcpy(mask->row(y + int(-extent.y)) + int(-extent.x), old8->row(y), size_t(w));
            source->mask->asset = MaskAsset::make(GrayPtr(mask));
        }
    }
    // The selection follows the pixels, as the canvas showed it during the transform.
    if (document_->selection && document_->selection->coverage) {
        transformEdit_ = edit;
        const std::optional<Selection> moved = displayedSelection();
        transformEdit_.reset();
        if (moved) document_->selection = moved;
    }
    source->asset = modeAsset(merged, source->name);
    source->transform = grownTransform;
    source->shapeImage.reset();
    const Uuid sourceId = source->id;
    document_->layers.erase(document_->layers.begin() + document_->indexOf(edit.layerId));
    setActiveLayer(sourceId);
    endEdit();
}

} // namespace app
