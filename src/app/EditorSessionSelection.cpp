// EditorSession: Selections: setting, combining, the magic wand, fill and clear, and the Select menu.
#include "EditorSession.h"
#include "compositor/smartwand.h"
#include "compositor/morphology.h"
#include "compositor/heal.h"
#include "compositor/wand.h"
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
    if (!document_) return;
    setSelection(combineSelection(document_->selection, shape, mode, selectionAntialiased), name);
}

void EditorSession::selectAll() {
    if (!document_) return;
    Selection s;
    s.coverage = std::make_shared<GrayImage>(document_->width, document_->height, 255);
    s.antialiased = selectionAntialiased;
    setSelection(s, "Select All");
}

void EditorSession::deselect() {
    if (!document_ || !document_->selection) return;
    setSelection(std::nullopt, "Deselect");
}

void EditorSession::invertSelection() {
    if (!document_ || !document_->selection) return;
    setSelection(compositor::invertSelection(*document_->selection, document_->width, document_->height), "Inverse");
}

void EditorSession::magicWand(QPointF documentPoint, int tolerance, bool contiguous, bool sampleAllLayers, SelectionMode mode, int sampleRadius, bool edgeAware, std::optional<bool> refineEdge) {
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
            Document single(document_->width, document_->height);
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
        click.field = wandSmart_->propagate(x, y, click.radius, wandCost(std::clamp(std::max(tolerance * 2, 64), 0, 255)), {}, click.anywhere);
        if (wandSessionLive() && mode != SelectionMode::Replace && mode != SelectionMode::Intersect) {
            // More evidence for the selection just made: Shift for what belongs, Alt for what does not.
            click.positive = mode == SelectionMode::Add;
            wandSession_->clicks.push_back(std::move(click));
            applyWandSession(tolerance, true);
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
    applySelectionShape(*mask, mode, "Magic Wand");
}

bool EditorSession::wandSessionLive() const {
    return wandSession_ && document_ && wandSmart_ && documentRevision_ == wandSession_->revisionAfter;
}

void EditorSession::applyWandSession(int tolerance, bool replaceStep) {
    WandSession& session = *wandSession_;
    std::vector<const SmartWandImage::Field*> positive, negative;
    for (WandClick& click : session.clicks) {
        if (wandCost(tolerance) > click.field.limit)
            click.field = wandSmart_->propagate(click.x, click.y, click.radius, wandCost(std::clamp(std::max(tolerance * 2, 64), 0, 255)), {}, click.anywhere);
        // A keep-out click competes at any cost it can reach, so its field goes as far as the positive ones do.
        (click.positive ? positive : negative).push_back(&click.field);
    }
    GrayImage mask(document_->width, document_->height);
    long count = thresholdWandFields(positive, negative, tolerance, selectionAntialiased, mask);
    std::vector<uint32_t> lineColours;
    if (wandRefineEdge) refineWandEdge(*wandSample_, mask, 3, &lineColours);
    if (replaceStep && session.hasStep) undo();
    const size_t steps = undoNames().size();
    setSelection(combineSelection(session.before, mask, session.mode, selectionAntialiased), "Magic Wand");
    // A selection equal to the one before records no step; the next change then has nothing to take back.
    session.hasStep = undoNames().size() > steps;
    session.revisionAfter = documentRevision_;
    // The unmixed colours belong to this wand selection only when it replaced the selection outright.
    wandLineColours_ = std::move(lineColours);
    wandLineSelection_ = session.mode == SelectionMode::Replace && document_->selection ? document_->selection->coverage : nullptr;
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
    if (!canEditLayers()) return;
    const GrayImage* selection = document_->selection && document_->selection->coverage ? document_->selection->coverage.get() : nullptr;
    if (document_->selection && !selection) return;
    fillThrough(color, selection, 1, "Fill");
}

bool EditorSession::paintBucket(QPointF documentPoint) {
    if (!canEditLayers()) return false;
    const int x = int(std::floor(documentPoint.x())), y = int(std::floor(documentPoint.y()));
    if (x < 0 || y < 0 || x >= document_->width || y >= document_->height) return false;
    const Layer* layer = activeLayer();
    if (!layer || layer->isGroup || layer->adjustment) return false;
    const GrayImage* selection = document_->selection && document_->selection->coverage ? document_->selection->coverage.get() : nullptr;
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
    return fillThrough(foregroundColor, &coverage, brushSettings.opacity, "Paint Bucket");
}

bool EditorSession::patchSelection(int dx, int dy) {
    if (!canEditLayers() || !document_->selection || !document_->selection->coverage || (dx == 0 && dy == 0)) return false;
    const Layer* layer = activeLayer();
    if (!layer || layer->isGroup || layer->adjustment || !layer->asset || !layer->asset->image) return false;
    if (isMaskSelected_) { emit error(tr("Patch works on a layer's pixels, not its mask.")); return false; }
    if (smartObjectBlocksPixels(true)) return false;
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
    const GrayImage& selection = *document_->selection->coverage;
    Image healed = *shown;
    healFrom(healed, source, selection, 1.0f);
    return fillThrough(foregroundColor, &selection, 1, "Patch", &healed);
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

void EditorSession::clearSelectedPixelsNow(Layer& layer) {
    const GrayImage* selection = document_->selection && document_->selection->coverage ? document_->selection->coverage.get() : nullptr;
    if (!selection || !layer.asset || !layer.asset->image) return;
    const Image& src = *layer.asset->image;
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
    if (!canEditLayers() || smartObjectBlocksPixels(true)) return;
    Layer* layer = activeLayerMutable();
    if (!layer || layer->isGroup || !layer->asset || !layer->asset->image) return;
    if (!document_->selection || !document_->selection->coverage) return;
    beginEdit("Clear");
    clearSelectedPixelsNow(*layer);
    endEdit();
    notifyDocument();
}

void EditorSession::nudgeSelection(double dx, double dy) {
    if (!document_ || !document_->selection || !document_->selection->coverage || !canEditLayers()) return;
    const GrayImage& src = *document_->selection->coverage;
    int ix = int(std::lround(dx)), iy = int(std::lround(dy));
    auto moved = std::make_shared<GrayImage>(src.width(), src.height(), 0);
    for (int y = 0; y < src.height(); y++) { int sy = y - iy; if (sy < 0 || sy >= src.height()) continue; for (int x = 0; x < src.width(); x++) { int sx = x - ix; if (sx >= 0 && sx < src.width()) moved->at(x, y) = src.at(sx, sy); } }
    Selection s = *document_->selection;
    s.coverage = moved;
    setSelection(s, "Move Selection");
}

void EditorSession::loadLayerAsSelection(const Uuid& id, bool mask, SelectionMode mode) {
    if (!document_ || !canEditLayers()) return;
    const Layer* layer = document_->find(id);
    if (!layer) return;
    std::shared_ptr<GrayImage> shape;
    if (mask) {
        if (!layer->mask || !layer->mask->asset.image) return;
        shape = std::make_shared<GrayImage>(document_->width, document_->height, 0);
        sampleMaskCoverage(*layer->mask->asset.image, layer->maskTransform(), document_->rect(), 1, 0, *shape, false);
    } else {
        if (!layer->asset || !layer->asset->image) return;
        shape = coverageFromLayer(*document_, *layer);
    }
    applySelectionShape(*shape, mode, mask ? "Load Mask as Selection" : "Load Layer as Selection");
}

void EditorSession::selectionExpand(int amount) {
    if (!document_ || !document_->selection || !document_->selection->coverage || amount <= 0 || amount > 500) return;
    setSelection(resizeSelection(*document_->selection, amount), "Expand Selection");
}

void EditorSession::selectionFeather(double radius) {
    if (!document_ || !document_->selection || !document_->selection->coverage || !(radius > 0) || radius > 250) return;
    Selection s = *document_->selection;
    s.coverage = featherSelection(*s.coverage, radius);
    setSelection(s, "Feather Selection");
}

void EditorSession::selectionSmooth(int radius) {
    if (!document_ || !document_->selection || !document_->selection->coverage || radius <= 0 || radius > 100) return;
    Selection s = *document_->selection;
    s.coverage = smoothSelection(*s.coverage, radius);
    setSelection(s, "Smooth Selection");
}

void EditorSession::selectionBorder(int width) {
    if (!document_ || !document_->selection || !document_->selection->coverage || width <= 0 || width > 200) return;
    Selection s = *document_->selection;
    s.coverage = borderSelection(*s.coverage, width);
    setSelection(s, "Border Selection");
}

void EditorSession::selectionContract(int amount) {
    if (!document_ || !document_->selection || !document_->selection->coverage || amount <= 0 || amount > 500) return;
    setSelection(resizeSelection(*document_->selection, -amount), "Contract Selection");
}

} // namespace app
