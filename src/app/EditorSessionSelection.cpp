// EditorSession: Selections: setting, combining, the magic wand, fill and clear, and the Select menu.
#include "EditorSession.h"
#include "ColorManagement.h"
#include "compositor/smartwand.h"
#include "compositor/morphology.h"
#include "compositor/heal.h"
#include "compositor/wand.h"
#include "compositor/filters.h"
#include "compositor/depth.h"
#include <algorithm>
#include <cstring>

using namespace compositor;

namespace app {

// ---- Selection --------------------------------------------------------------------

void EditorSession::setSelection(const std::optional<Selection>& selection, const QString& name) {
    if (!document_ || !canEditLayers()) return;
    if (document_->selection == selection) return;
    beginEdit(name);
    document_->selection = selection;
    endEdit();
    emit historyChanged();
    emit titleChanged();
    emit selectionChanged();
}

void EditorSession::applySelectionShape(const GrayImage& shape, SelectionMode mode, const QString& name) {
    if (refusedAtDepth("edit.selection", tr("Selections"))) return;
    if (!document_) return;
    // A 16-bit document's selection stays at 16 bits: the shape is widened and combined there.
    if (document_->sampleType == SampleType::U16) { setSelection(combineSelection(document_->selection, AnyGray(widenGray(shape)), mode, selectionAntialiased, SampleType::U16), name); return; }
    setSelection(combineSelection(document_->selection, shape, mode, selectionAntialiased), name);
}

void EditorSession::applySelectionShape(const Gray16& shape, SelectionMode mode, const QString& name) {
    if (refusedAtDepth("edit.selection", tr("Selections"))) return;
    if (!document_) return;
    setSelection(combineSelection(document_->selection, AnyGray(std::make_shared<Gray16>(shape)), mode, selectionAntialiased, document_->sampleType), name);
}

void EditorSession::selectAll() {
    if (refusedAtDepth("edit.selection", tr("Selections"))) return;
    if (!document_) return;
    Selection s;
    if (document_->sampleType == SampleType::U16) s.coverage = Gray16Ptr(std::make_shared<Gray16>(document_->width, document_->height, uint16_t(one16)));
    else {
        // Already all selected by an earlier Select All: the same buffer again, so nothing changes and no step is added.
        GrayPtr full = selectAllCoverage_.lock();
        if (!full || full->width() != document_->width || full->height() != document_->height || !document_->selection || document_->selection->coverage.identity() != full.get()) {
            full = std::make_shared<GrayImage>(document_->width, document_->height, 255);
            selectAllCoverage_ = full;
        }
        s.coverage = full;
    }
    s.antialiased = selectionAntialiased;
    setSelection(s, QT_TRANSLATE_NOOP("History", "Select All"));
}

void EditorSession::deselect() {
    if (!document_ || !document_->selection) return;
    setSelection(std::nullopt, QT_TRANSLATE_NOOP("History", "Deselect"));
}

void EditorSession::invertSelection() {
    if (refusedAtDepth("edit.selection", tr("Selections"))) return;
    if (!document_ || !document_->selection) return;
    setSelection(compositor::invertSelection(*document_->selection, document_->width, document_->height), QT_TRANSLATE_NOOP("History", "Inverse"));
}

void EditorSession::magicWand(QPointF documentPoint, int tolerance, bool contiguous, bool sampleAllLayers, SelectionMode mode, int sampleRadius, bool edgeAware, std::optional<bool> refineEdge) {
    if (refusedAtDepth("edit.selection", tr("Selections"))) return;
    if (refineEdge) wandRefineEdge = *refineEdge;
    if (!document_ || !canEditLayers()) return;
    int x = int(std::floor(documentPoint.x())), y = int(std::floor(documentPoint.y()));
    if (x < 0 || y < 0 || x >= document_->width || y >= document_->height) return;
    const Layer* layer = sampleAllLayers ? nullptr : activeLayer();
    if (!sampleAllLayers && (!layer || layer->isGroup || layer->adjustment || !layer->asset || !layer->asset->image)) {
        emit notice(tr("The Magic Wand reads the active layer's pixels: select a pixel layer, or turn on Sample All Layers"));
        return;
    }
    // The sampled pixels are kept between clicks on the same document state (repeated wand clicks are common).
    Uuid layerId = layer ? layer->id : Uuid{};
    bool cached = wandSample_ && wandSampleAll_ == sampleAllLayers && wandSampleLayer_ == layerId && wandSampleRevision_ == documentRevision_;
    if (!cached) {
        if (sampleAllLayers) wandSample_ = renderFlattened(*document_);
        else {
            // The active layer's own pixels as placed, without its mask, opacity, blend or clipping (as on the Mac).
            // A 16-bit layer is read as the canvas shows it, reduced to 8 bits: Tolerance counts 8-bit levels.
            Document single(document_->width, document_->height);
            single.sampleType = document_->sampleType;
            Layer copy = *layer;
            copy.parentId.reset();
            copy.visible = true;
            copy.opacity = 1;
            copy.blendMode = BlendMode::Normal;
            copy.mask.reset();
            copy.maskSourceId.reset();
            copy.transform = displayedTransform(*layer);
            single.layers = {copy};
            wandSample_ = renderFlattened(single);
        }
        wandSampleAll_ = sampleAllLayers; wandSampleLayer_ = layerId; wandSampleRevision_ = documentRevision_;
        wandSmart_.reset();
    }
    if (edgeAware) {
        if (!wandSmart_) wandSmart_ = std::make_shared<SmartWandImage>(*wandSample_);
        WandClick click;
        click.x = x; click.y = y;
        click.radius = 1 + 2 * std::clamp(sampleRadius, 0, 2);   // the patch: 3, 7 or 11 pixels across
        // The click's cost field, far enough past this tolerance that the slider can move without recomputing.
        click.anywhere = !contiguous;
        click.tolerance = tolerance;
        click.field = wandSmart_->propagate(x, y, click.radius, wandCost(std::clamp(std::max(tolerance * 2, 64), 0, 255)), {}, click.anywhere);
        if (wandSessionLive() && mode != SelectionMode::Replace && mode != SelectionMode::Intersect) {
            // More evidence for the selection just made: Shift for what belongs, Alt for what does not.
            click.positive = mode == SelectionMode::Add;
            wandSession_->clicks.push_back(std::move(click));
            applyWandSession(tolerance, false);
            return;
        }
        WandSession session;
        session.before = document_->selection;
        session.mode = mode;
        session.clicks.push_back(std::move(click));
        wandSession_ = std::move(session);
        applyWandSession(tolerance, false);
        return;
    }
    wandSession_.reset();
    auto mask = std::make_shared<GrayImage>(document_->width, document_->height);
    long count = wandMask(*wandSample_, x, y, std::clamp(sampleRadius, 0, 2), tolerance, contiguous, *mask);
    if (count < 0) return;
    applySelectionShape(*mask, mode, QT_TRANSLATE_NOOP("History", "Magic Wand"));
}

bool EditorSession::wandSessionLive() const {
    return wandSession_ && document_ && wandSmart_ && documentRevision_ == wandSession_->revisionAfter;
}

void EditorSession::applyWandSession(int tolerance, bool retune) {
    if (refusedAtDepth("edit.selection", tr("Selections"))) return;
    WandSession& session = *wandSession_;
    WandClick& latest = session.clicks.back();
    if (retune) latest.tolerance = tolerance;
    // A keep-out click competes at any cost a positive one can reach, so every field goes as far as the widest tolerance.
    int widest = 0;
    for (const WandClick& click : session.clicks) widest = std::max(widest, click.tolerance);
    std::vector<const SmartWandImage::Field*> positive, negative;
    std::vector<int> tolerances;
    for (WandClick& click : session.clicks) {
        if (wandCost(widest) > click.field.limit)
            click.field = wandSmart_->propagate(click.x, click.y, click.radius, wandCost(std::clamp(std::max(widest * 2, 64), 0, 255)), {}, click.anywhere);
        if (click.positive) { positive.push_back(&click.field); tolerances.push_back(click.tolerance); }
        else negative.push_back(&click.field);
    }
    GrayImage mask(document_->width, document_->height);
    long count = thresholdWandFields(positive, tolerances, negative, selectionAntialiased, mask);
    std::vector<uint32_t> lineColours;
    if (wandRefineEdge) refineWandEdge(*wandSample_, mask, 3, &lineColours);
    if (retune && latest.hasStep) {
        wandRetuning_ = true;
        undo();
        wandRetuning_ = false;
    }
    const size_t steps = undoNames().size();
    if (document_->sampleType == SampleType::U16)
        setSelection(combineSelection(session.before, AnyGray(widenGray(mask)), session.mode, selectionAntialiased, SampleType::U16), QT_TRANSLATE_NOOP("History", "Magic Wand"));
    else setSelection(combineSelection(session.before, mask, session.mode, selectionAntialiased), QT_TRANSLATE_NOOP("History", "Magic Wand"));
    // A selection equal to the one before records no step; the next change then has nothing to take back.
    latest.hasStep = undoNames().size() > steps;
    session.revisionAfter = documentRevision_;
    // The unmixed colours belong to this wand selection only when it replaced the selection outright.
    wandLineColours_ = std::move(lineColours);
    wandLineSelection_ = session.mode == SelectionMode::Replace && document_->selection ? document_->selection->coverage.u8() : nullptr;
    const int next = wandNextTolerance(positive, negative, tolerance);
    emit notice(next < 0 ? tr("Tolerance %1: %L2 pixels").arg(tolerance).arg(count)
                         : tr("Tolerance %1: %L2 pixels; the selection grows next at %3").arg(tolerance).arg(count).arg(next));
}

bool EditorSession::retolerateWand(int tolerance) {
    // Only while the wand's step is the latest thing that happened to the document.
    if (!wandSessionLive()) return false;
    applyWandSession(tolerance, true);
    return true;
}

void EditorSession::fillSelection(const QColor& color) {
    if (refusedAtDepth("edit.fill", tr("Fill"))) return;
    if (!canEditLayers()) return;
    if (document_->colorMode != ColorMode::RGB && !(isMaskSelected_ && activeLayer() && activeLayer()->mask)) {
        fillThroughMode(color, QT_TRANSLATE_NOOP("History", "Fill"));
        return;
    }
    if (document_->sampleType == SampleType::U16) {
        const Gray16* selection = document_->selection ? document_->selection->coverage.u16().get() : nullptr;
        if (document_->selection && !selection) return;
        fillThrough16(color, selection, QT_TRANSLATE_NOOP("History", "Fill"));
        return;
    }
    const GrayImage* selection = document_->selection && document_->selection->coverage.u8() ? document_->selection->coverage.u8().get() : nullptr;
    if (document_->selection && !selection) return;
    fillThrough(color, selection, 1, QT_TRANSLATE_NOOP("History", "Fill"));
}

bool EditorSession::paintBucket(QPointF documentPoint) {
    if (refusedAtDepth("tool.paintBucket", tr("Painting"))) return false;
    if (!canEditLayers()) return false;
    const int x = int(std::floor(documentPoint.x())), y = int(std::floor(documentPoint.y()));
    if (x < 0 || y < 0 || x >= document_->width || y >= document_->height) return false;
    const Layer* layer = activeLayer();
    if (!layer || layer->isGroup || layer->adjustment) return false;
    if (document_->sampleType == SampleType::U16) {
        // What to fill is chosen on the pixels as the canvas shows them (Tolerance counts 8-bit levels, as the Magic
        // Wand's does); the fill itself, its antialiased edge and the selection are 16-bit.
        const Gray16* selection = document_->selection ? document_->selection->coverage.u16().get() : nullptr;
        if (document_->selection && (!selection || selection->at(x, y) == 0)) return false;
        std::shared_ptr<Image16> shown16;
        if (bucket.allLayers) shown16 = renderFlattened16(*document_);
        else {
            Document single(document_->width, document_->height);
            single.sampleType = SampleType::U16;
            Layer copy = *layer;
            copy.parentId.reset(); copy.visible = true; copy.opacity = 1; copy.blendMode = BlendMode::Normal; copy.mask.reset(); copy.maskSourceId.reset();
            copy.transform = displayedTransform(*layer);
            single.layers = {copy};
            shown16 = renderFlattened16(single);
        }
        GrayImage chosen(document_->width, document_->height);
        if (wandMask(*narrowImage(*shown16), x, y, 0, std::clamp(bucket.tolerance, 0, 255), bucket.contiguous, chosen) <= 0) return false;
        auto coverage = widenGray(chosen);
        if (bucket.antialias) gaussianBlur(*coverage, 0.5);
        const uint32_t opacity = uint32_t(std::lround(std::clamp(brushSettings.opacity, 0.0, 1.0) * one16));
        for (int py = 0; py < coverage->height(); py++)
            for (int px = 0; px < coverage->width(); px++) {
                uint32_t c = mul15(coverage->at(px, py), opacity);
                if (selection) c = mul15(c, std::min<uint32_t>(selection->at(px, py), one16));
                coverage->at(px, py) = uint16_t(c);
            }
        return fillThrough16(foregroundColor, coverage.get(), QT_TRANSLATE_NOOP("History", "Paint Bucket"));
    }
    const GrayImage* selection = document_->selection && document_->selection->coverage.u8() ? document_->selection->coverage.u8().get() : nullptr;
    if (document_->selection && (!selection || selection->at(x, y) == 0)) return false;   // a click outside the selection fills nothing
    // What the click is compared with: the document as shown, or the active layer's own pixels as placed (an empty
    // layer is all transparent, so it fills everywhere the fill reaches).
    std::shared_ptr<Image> sample;
    if (bucket.allLayers) sample = renderFlattened(*document_);
    else {
        Document single(document_->width, document_->height);
        Layer copy = *layer;
        copy.parentId.reset(); copy.visible = true; copy.opacity = 1; copy.blendMode = BlendMode::Normal; copy.mask.reset(); copy.maskSourceId.reset();
        copy.transform = displayedTransform(*layer);
        single.layers = {copy};
        sample = renderFlattened(single);
    }
    GrayImage coverage(document_->width, document_->height);
    if (wandMask(*sample, x, y, 0, std::clamp(bucket.tolerance, 0, 255), bucket.contiguous, coverage) <= 0) return false;
    if (bucket.antialias) gaussianBlur(coverage, 0.5);
    if (selection)
        for (int py = 0; py < coverage.height(); py++)
            for (int px = 0; px < coverage.width(); px++) coverage.at(px, py) = uint8_t((coverage.at(px, py) * selection->at(px, py) + 127) / 255);
    return fillThrough(foregroundColor, &coverage, brushSettings.opacity, QT_TRANSLATE_NOOP("History", "Paint Bucket"));
}

bool EditorSession::patchSelection(int dx, int dy) {
    if (refusedAtDepth("tool.spotHealing", tr("Editing pixels"))) return false;
    if (!canEditLayers() || !document_->selection || !document_->selection->coverage || (dx == 0 && dy == 0)) return false;
    const Layer* layer = activeLayer();
    if (!layer || layer->isGroup || layer->adjustment || !layer->asset || !layer->asset->image) return false;
    if (isMaskSelected_) { emit error(tr("Patch works on a layer's pixels, not its mask.")); return false; }
    if (smartObjectBlocksPixels(true)) return false;
    if (document_->sampleType == SampleType::U16) {
        const Gray16* selection = document_->selection->coverage.u16().get();
        if (!selection) return false;
        Document single(document_->width, document_->height);
        single.sampleType = SampleType::U16;
        Layer copy = *layer;
        copy.parentId.reset(); copy.visible = true; copy.opacity = 1; copy.blendMode = BlendMode::Normal; copy.mask.reset(); copy.maskSourceId.reset();
        copy.transform = displayedTransform(*layer);
        single.layers = {copy};
        auto shown = renderFlattened16(single);
        Image16 source(shown->width(), shown->height());
        for (int y = 0; y < source.height(); y++) {
            const int sy = y + dy;
            if (sy < 0 || sy >= shown->height()) continue;
            for (int x = 0; x < source.width(); x++) {
                const int sx = x + dx;
                if (sx >= 0 && sx < shown->width()) std::memcpy(source.pixel(x, y), shown->pixel(sx, sy), 4 * sizeof(uint16_t));
            }
        }
        Image16 healed = *shown;
        healFrom(healed, source, *selection, 1.0f);
        return fillThrough16(foregroundColor, selection, QT_TRANSLATE_NOOP("History", "Patch"), &healed);
    }
    // The layer as the canvas shows it; the source is the same pixels shifted by the drag.
    Document single(document_->width, document_->height);
    Layer copy = *layer;
    copy.parentId.reset(); copy.visible = true; copy.opacity = 1; copy.blendMode = BlendMode::Normal; copy.mask.reset(); copy.maskSourceId.reset();
    copy.transform = displayedTransform(*layer);
    single.layers = {copy};
    auto shown = renderFlattened(single);
    Image source(shown->width(), shown->height());
    for (int y = 0; y < source.height(); y++) {
        const int sy = y + dy;
        if (sy < 0 || sy >= shown->height()) continue;
        for (int x = 0; x < source.width(); x++) {
            const int sx = x + dx;
            if (sx >= 0 && sx < shown->width()) std::memcpy(source.pixel(x, y), shown->pixel(sx, sy), 4);
        }
    }
    const GrayImage& selection = *document_->selection->coverage.u8();
    Image healed = *shown;
    healFrom(healed, source, selection, 1.0f);
    return fillThrough(foregroundColor, &selection, 1, QT_TRANSLATE_NOOP("History", "Patch"), &healed);
}

bool EditorSession::fillThrough(const QColor& color, const GrayImage* selection, double opacity, const char* name, const Image* from) {
    Layer* layer = activeLayerMutable();
    if (!layer || layer->isGroup || layer->adjustment) return false;
    bool mask = isMaskSelected_ && layer->mask;
    if (!mask && smartObjectBlocksPixels(true)) return false;
    opacity = std::clamp(opacity, 0.0, 1.0);
    // A fill is a stroke covering the whole canvas: paint through the selection.
    BrushSettings settings;
    settings.diameter = 1;
    settings.red = color.redF(); settings.green = color.greenF(); settings.blue = color.blueF();
    if (mask) settings.maskValue = color.lightnessF() >= 0.5 ? 1 : 0;
    if (mask && paintsQuickMask()) settings.maskValue = 1 - settings.maskValue;
    BrushStroke stroke(*layer, mask, settings, document_->size(), selection);
    if (!stroke.isValid()) return false;
    // Direct fill over the working image rather than dabbing.
    auto working = mask ? nullptr : std::const_pointer_cast<Image>(stroke.previewImage());
    auto workingMask = mask ? std::const_pointer_cast<GrayImage>(stroke.previewMask()) : nullptr;
    int w = mask ? workingMask->width() : working->width(), h = mask ? workingMask->height() : working->height();
    Affine toDoc = stroke.paintTransform().pixelToDocument(w, h);
    for (int py = 0; py < h; py++) for (int px = 0; px < w; px++) {
        Point d = toDoc.apply({px + 0.5, py + 0.5});
        if (d.x < 0 || d.y < 0 || d.x >= document_->width || d.y >= document_->height) continue;
        double c = opacity * (selection ? selection->at(std::min(int(d.x), selection->width() - 1), std::min(int(d.y), selection->height() - 1)) / 255.0 : 1.0);
        if (c <= 0) continue;
        if (mask) {
            uint8_t& v = workingMask->at(px, py);
            v = uint8_t(v * (1 - c) + settings.maskValue * 255 * c + 0.5);
        } else {
            uint8_t* p = working->pixel(px, py);
            if (from) {
                const uint8_t* f = from->pixel(int(d.x), int(d.y));
                for (int k = 0; k < 4; k++) p[k] = uint8_t(p[k] * (1 - c) + f[k] * c + 0.5);
                continue;
            }
            p[0] = uint8_t(p[0] * (1 - c) + color.red() * c + 0.5);
            p[1] = uint8_t(p[1] * (1 - c) + color.green() * c + 0.5);
            p[2] = uint8_t(p[2] * (1 - c) + color.blue() * c + 0.5);
            p[3] = uint8_t(p[3] * (1 - c) + 255 * c + 0.5);
        }
    }
    beginEdit(name);
    if (mask) { layer->mask->asset = MaskAsset::make(workingMask); }
    else {
        PixelBounds b = alphaBounds(*working);
        Rect crop = b.isEmpty() ? Rect(0, 0, w, h) : Rect(b.x0, b.y0, b.x1 - b.x0, b.y1 - b.y0);
        auto image = cropImage(*working, int(crop.x), int(crop.y), int(crop.width), int(crop.height));
        Point center = toDoc.apply({crop.midX(), crop.midY()});
        LayerTransform t = stroke.paintTransform();
        t.size = {crop.width * t.size.width / w, crop.height * t.size.height / h};
        t.origin = {center.x - t.size.width / 2, center.y - t.size.height / 2};
        if (layer->mask && !layer->mask->placement && layer->asset) layer->mask->placement = layer->transform;
        layer->asset = Asset::make(image, layer->name);
        layer->transform = t;
        layer->shapeImage.reset();
    }
    endEdit();
    notifyDocument();
    return true;
}

bool EditorSession::fillThrough16(const QColor& color, const Gray16* selection, const char* name, const Image16* from) {
    Layer* layer = activeLayerMutable();
    if (!layer || layer->isGroup || layer->adjustment) return false;
    const bool mask = isMaskSelected_ && layer->mask;
    if (!mask && smartObjectBlocksPixels(true)) return false;
    // Coverage at a document point: the selection's there, everything when there is none.
    auto coverageAt = [&](Point d) -> uint32_t {
        if (d.x < 0 || d.y < 0 || d.x >= document_->width || d.y >= document_->height) return 0;
        return selection ? std::min<uint32_t>(selection->at(std::min(int(d.x), selection->width() - 1), std::min(int(d.y), selection->height() - 1)), one16) : one16;
    };
    if (mask) {
        if (!layer->mask->asset.image.u16()) return false;
        auto out = std::make_shared<Gray16>(*layer->mask->asset.image.u16());
        const bool white = color.lightnessF() >= 0.5;
        const uint32_t value = (paintsQuickMask() ? !white : white) ? one16 : 0;
        const Affine toDoc = layer->maskTransform().pixelToDocument(out->width(), out->height());
        for (int py = 0; py < out->height(); py++)
            for (int px = 0; px < out->width(); px++) {
                const uint32_t c = coverageAt(toDoc.apply({px + 0.5, py + 0.5}));
                if (!c) continue;
                uint16_t& v = out->at(px, py);
                v = uint16_t((v * (one16 - c) + value * c + one16 / 2) >> 15);
            }
        beginEdit(name);
        layer->mask->asset = MaskAsset::make(Gray16Ptr(out));
        endEdit();
        notifyDocument();
        return true;
    }
    // The layer's grid grown to take in the canvas (a fill reaches everywhere the selection does), in the layer's
    // own pixel space so a rotated or scaled layer keeps its placement.
    const Image16* src = layer->asset ? layer->asset->image.u16().get() : nullptr;
    const int w = src ? src->width() : document_->width, h = src ? src->height() : document_->height;
    const LayerTransform base = src ? layer->transform : LayerTransform(Point(0, 0), document_->size());
    const Affine toPixels = base.pixelToDocument(w, h).inverted();
    const Rect extent = Rect(0, 0, w, h).unionWith(toPixels.mapBounds(document_->rect())).integral();
    if (extent.width > maxImageSide || extent.height > maxImageSide || extent.width * extent.height > double(Document::imagePixelBudget(SampleType::U16))) {
        emit error(tr("The filled layer would exceed the size limits."));
        return false;
    }
    auto working = std::make_shared<Image16>(int(extent.width), int(extent.height));
    if (src) for (int y = 0; y < h; y++) std::memcpy(working->pixel(int(-extent.x), y + int(-extent.y)), src->row(y), size_t(w) * 4 * sizeof(uint16_t));
    LayerTransform grown = base;
    grown.size = {extent.width * base.size.width / w, extent.height * base.size.height / h};
    const Point center = base.pixelToDocument(w, h).apply({extent.midX(), extent.midY()});
    grown.origin = {center.x - grown.size.width / 2, center.y - grown.size.height / 2};
    const Affine toDoc = grown.pixelToDocument(working->width(), working->height());
    const uint32_t fill[4] = {uint32_t(std::lround(color.redF() * one16)), uint32_t(std::lround(color.greenF() * one16)), uint32_t(std::lround(color.blueF() * one16)), one16};
    for (int py = 0; py < working->height(); py++)
        for (int px = 0; px < working->width(); px++) {
            const Point d = toDoc.apply({px + 0.5, py + 0.5});
            const uint32_t c = coverageAt(d);
            if (!c) continue;
            uint16_t* p = working->pixel(px, py);
            const uint16_t* f = from ? from->pixel(std::min(int(d.x), from->width() - 1), std::min(int(d.y), from->height() - 1)) : nullptr;
            for (int k = 0; k < 4; k++) p[k] = uint16_t((p[k] * (one16 - c) + (f ? uint32_t(f[k]) : fill[k]) * c + one16 / 2) >> 15);
        }
    // Cropped to the pixels it holds, as the 8-bit fill does.
    LayerTransform placed;
    auto image = trimToPixels(*working, grown, placed);
    beginEdit(name);
    if (layer->mask && !layer->mask->placement && layer->asset) layer->mask->placement = layer->transform;
    layer->asset = Asset::make(Image16Ptr(image), layer->name);
    layer->transform = placed;
    layer->shapeImage.reset();
    endEdit();
    notifyDocument();
    return true;
}

bool EditorSession::fillThroughMode(const QColor& color, const char* name) {
    Layer* layer = activeLayerMutable();
    if (!layer || layer->isGroup || layer->adjustment) return false;
    if (smartObjectBlocksPixels(true)) return false;
    const Document& doc = *document_;
    // The colour in the document's mode and profile, opaque, as 8-bit samples.
    const ColorTransformPtr t = transformBetween(ColorProfile(), doc.profile, color::conversionOptions(), PixelFormat::RGBA8, pixelFormatFor(SampleType::U8, doc.colorMode));
    const uint8_t in[4] = {uint8_t(color.red()), uint8_t(color.green()), uint8_t(color.blue()), 255};
    uint8_t ink[5] = {0, 0, 0, 0, 255};
    if (!t) return false;
    t->apply(in, ink, 1);
    const int n = colorModeChannels(doc.colorMode);
    const bool deep = doc.sampleType == SampleType::U16;
    // The layer's own pixels at the document's layout, or a blank canvas-sized raster.
    AnyImage source = layer->asset && layer->asset->image && layer->asset->image.channels() == n ? layer->asset->image : AnyImage();
    const LayerTransform placement = source ? layer->transform : LayerTransform(Point(0, 0), doc.size());
    const int w = source ? source.width() : doc.width, h = source ? source.height() : doc.height;
    const Affine toDoc = placement.pixelToDocument(w, h);
    const AnyGray& selection = doc.selection ? doc.selection->coverage : AnyGray();
    auto coverageAt = [&](Point d) -> float {
        if (d.x < 0 || d.y < 0 || d.x >= doc.width || d.y >= doc.height) return 0.f;
        if (!doc.selection) return 1.f;
        const int x = std::min(int(d.x), selection.width() - 1), y = std::min(int(d.y), selection.height() - 1);
        if (selection.u16()) return std::min(1.f, selection.u16()->at(x, y) / 32768.f);
        if (selection.u8()) return selection.u8()->at(x, y) / 255.f;
        return 0.f;
    };
    auto fill = [&](auto* out) {
        const float one = deep ? 32768.f : 255.f;
        for (int py = 0; py < h; py++)
            for (int px = 0; px < w; px++) {
                const float c = coverageAt(toDoc.apply({px + 0.5, py + 0.5}));
                if (c <= 0) continue;
                auto* p = out->pixel(px, py);
                for (int k = 0; k < n; k++) {
                    const float f = k == n - 1 ? one : float(deep ? widen8(ink[k]) : ink[k]);
                    p[k] = static_cast<std::remove_cvref_t<decltype(p[k])>>(std::lround(p[k] * (1 - c) + f * c));
                }
            }
    };
    AnyImage result;
    if (deep) {
        auto out = source.u16() ? std::make_shared<Image16>(*source.u16()) : std::make_shared<Image16>(w, h, n);
        fill(out.get());
        result = Image16Ptr(out);
    } else if (doc.colorMode == ColorMode::CMYK) {
        auto out = source.c8() ? std::make_shared<ImageC8>(*source.c8()) : std::make_shared<ImageC8>(w, h, n);
        fill(out.get());
        result = ImageC8Ptr(out);
    } else {
        auto out = source.u8() ? std::make_shared<Image>(*source.u8()) : std::make_shared<Image>(w, h);
        fill(out.get());
        result = ImagePtr(out);
    }
    beginEdit(name);
    layer->asset = Asset::makeAny(result, layer->asset ? layer->asset->name : layer->name);
    layer->asset->thumbnail = modeThumbnail(result, doc.colorMode, doc.profile);
    layer->transform = placement;
    layer->shapeImage.reset();
    endEdit();
    notifyDocument();
    return true;
}

void EditorSession::clearSelectedPixelsNow(Layer& layer) {
    if (layer.asset && layer.asset->image.u16()) {
        const Gray16* selection = document_->selection ? document_->selection->coverage.u16().get() : nullptr;
        if (!selection) return;
        const Image16& src = *layer.asset->image.u16();
        auto out = std::make_shared<Image16>(src);
        const Affine toDoc = layer.transform.pixelToDocument(src.width(), src.height());
        for (int y = 0; y < src.height(); y++)
            for (int x = 0; x < src.width(); x++) {
                const Point d = toDoc.apply({x + 0.5, y + 0.5});
                if (d.x < 0 || d.y < 0 || d.x >= document_->width || d.y >= document_->height) continue;
                const uint32_t c = std::min<uint32_t>(selection->at(int(d.x), int(d.y)), one16);
                if (!c) continue;
                uint16_t* p = out->pixel(x, y);
                for (int k = 0; k < 4; k++) p[k] = uint16_t((p[k] * (one16 - c) + one16 / 2) >> 15);
            }
        layer.asset = Asset::make(Image16Ptr(out), layer.name);
        layer.shapeImage.reset();
        return;
    }
    const GrayImage* selection = document_->selection && document_->selection->coverage.u8() ? document_->selection->coverage.u8().get() : nullptr;
    if (!selection || !layer.asset || !layer.asset->image.u8()) return;
    const Image& src = *layer.asset->image.u8();
    auto out = std::make_shared<Image>(src);
    // Right after a refined wand selection, on a layer that covers the canvas pixel for pixel: the cleared edge
    // takes the line's own colour (no rim of the old background).
    const LayerTransform& t = layer.transform;
    if (wandLineSelection_ && document_->selection->coverage == wandLineSelection_ && !wandLineColours_.empty()
        && t.rotation == 0 && !t.flipX && !t.flipY && t.origin.x == 0 && t.origin.y == 0
        && src.width() == document_->width && src.height() == document_->height && t.size.width == src.width() && t.size.height == src.height()) {
        clearDecontaminated(*out, *selection, &wandLineColours_);
        layer.asset = Asset::make(out, layer.name);
        layer.shapeImage.reset();
        return;
    }
    Affine toDoc = layer.transform.pixelToDocument(src.width(), src.height());
    for (int y = 0; y < src.height(); y++) for (int x = 0; x < src.width(); x++) {
        Point d = toDoc.apply({x + 0.5, y + 0.5});
        if (d.x < 0 || d.y < 0 || d.x >= document_->width || d.y >= document_->height) continue;
        double c = selection->at(int(d.x), int(d.y)) / 255.0;
        if (c <= 0) continue;
        uint8_t* p = out->pixel(x, y);
        for (int k = 0; k < 4; k++) p[k] = uint8_t(p[k] * (1 - c) + 0.5);
    }
    layer.asset = Asset::make(out, layer.name);
    layer.shapeImage.reset();
}

void EditorSession::clearSelectionPixels() {
    if (refusedAtDepth("edit.fill", tr("Clear"))) return;
    if (!canEditLayers() || smartObjectBlocksPixels(true)) return;
    Layer* layer = activeLayerMutable();
    if (!layer || layer->isGroup || !layer->asset || !layer->asset->image) return;
    if (!document_->selection || !document_->selection->coverage) return;
    // In some colour channels only, Delete fills them with the background colour, as Photoshop clears a channel.
    if (activeColors_ != allColors() && !isMaskSelected_) { fillSelection(backgroundColor); return; }
    beginEdit(QT_TRANSLATE_NOOP("History", "Clear"));
    clearSelectedPixelsNow(*layer);
    endEdit();
    notifyDocument();
}

void EditorSession::nudgeSelection(double dx, double dy) {
    if (refusedAtDepth("edit.selection", tr("Selections"))) return;
    if (!document_ || !document_->selection || !document_->selection->coverage || !canEditLayers()) return;
    Selection s = offsetSelection(*document_->selection, int(std::lround(dx)), int(std::lround(dy)));
    setSelection(s, QT_TRANSLATE_NOOP("History", "Move Selection"));
}

void EditorSession::loadLayerAsSelection(const Uuid& id, bool mask, SelectionMode mode) {
    if (refusedAtDepth("edit.selection", tr("Selections"))) return;
    if (!document_ || !canEditLayers()) return;
    const Layer* layer = document_->find(id);
    if (!layer) return;
    const QString name = mask ? QT_TRANSLATE_NOOP("History", "Load Mask as Selection") : QT_TRANSLATE_NOOP("History", "Load Layer as Selection");
    if (document_->sampleType == SampleType::U16) {
        std::shared_ptr<Gray16> deep;
        if (mask) {
            if (!layer->mask || !layer->mask->asset.image.u16()) return;
            deep = std::make_shared<Gray16>(document_->width, document_->height, 0);
            sampleMaskCoverage(*layer->mask->asset.image.u16(), layer->maskTransform(), document_->rect(), 1, 0, *deep, false);
        } else {
            if (!layer->asset || !layer->asset->image.u16()) return;
            deep = coverageFromLayer16(*document_, *layer);
        }
        applySelectionShape(*deep, mode, name);
        return;
    }
    std::shared_ptr<GrayImage> shape;
    if (mask) {
        if (!layer->mask || !layer->mask->asset.image.u8()) return;
        shape = std::make_shared<GrayImage>(document_->width, document_->height, 0);
        sampleMaskCoverage(*layer->mask->asset.image.u8(), layer->maskTransform(), document_->rect(), 1, 0, *shape, false);
    } else {
        if (!layer->asset || !layer->asset->image.u8()) return;
        shape = coverageFromLayer(*document_, *layer);
    }
    applySelectionShape(*shape, mode, name);
}

void EditorSession::selectionExpand(int amount) {
    if (refusedAtDepth("edit.selection", tr("Selections"))) return;
    if (!document_ || !document_->selection || !document_->selection->coverage || amount <= 0 || amount > 500) return;
    setSelection(resizeSelection(*document_->selection, amount), QT_TRANSLATE_NOOP("History", "Expand Selection"));
}

void EditorSession::selectionFeather(double radius) {
    if (refusedAtDepth("edit.selection", tr("Selections"))) return;
    if (!document_ || !document_->selection || !document_->selection->coverage || !(radius > 0) || radius > 250) return;
    Selection s = *document_->selection;
    if (s.coverage.u16()) s.coverage = Gray16Ptr(featherSelection(*s.coverage.u16(), radius));
    else s.coverage = featherSelection(*s.coverage.u8(), radius);
    setSelection(s, QT_TRANSLATE_NOOP("History", "Feather Selection"));
}

void EditorSession::selectionSmooth(int radius) {
    if (refusedAtDepth("edit.selection", tr("Selections"))) return;
    if (!document_ || !document_->selection || !document_->selection->coverage || radius <= 0 || radius > 100) return;
    Selection s = *document_->selection;
    if (s.coverage.u16()) s.coverage = Gray16Ptr(smoothSelection(*s.coverage.u16(), radius));
    else s.coverage = smoothSelection(*s.coverage.u8(), radius);
    setSelection(s, QT_TRANSLATE_NOOP("History", "Smooth Selection"));
}

void EditorSession::selectionBorder(int width) {
    if (refusedAtDepth("edit.selection", tr("Selections"))) return;
    if (!document_ || !document_->selection || !document_->selection->coverage || width <= 0 || width > 200) return;
    Selection s = *document_->selection;
    if (s.coverage.u16()) s.coverage = Gray16Ptr(borderSelection(*s.coverage.u16(), width));
    else s.coverage = borderSelection(*s.coverage.u8(), width);
    setSelection(s, QT_TRANSLATE_NOOP("History", "Border Selection"));
}

void EditorSession::selectionContract(int amount) {
    if (refusedAtDepth("edit.selection", tr("Selections"))) return;
    if (!document_ || !document_->selection || !document_->selection->coverage || amount <= 0 || amount > 500) return;
    setSelection(resizeSelection(*document_->selection, -amount), QT_TRANSLATE_NOOP("History", "Contract Selection"));
}

} // namespace app
