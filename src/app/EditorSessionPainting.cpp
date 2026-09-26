// EditorSession: Painting: brushes, opacity keys, blur/smudge/liquify, gradients, shapes and text.
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include "EditorSession.h"
#include "BrushLibrary.h"
#include "PresetLibrary.h"
#include "QtGeometry.h"
#include "TextLayer.h"
#include <algorithm>
#include <random>

using namespace compositor;

namespace app {

// ---- Brush ---------------------------------------------------------------------

std::optional<QPointF> EditorSession::cloneSamplePoint(QPointF point) const {
    if (!cloneSource) return std::nullopt;
    if (!cloneOffset || !(cloneAligned || stroke_)) return cloneSource;
    return QPointF(point.x() + cloneOffset->x(), point.y() + cloneOffset->y());
}

bool EditorSession::beginBrush(QPointF documentPoint, bool straightFromLast) {
    if (!document_ || stroke_ || transformEdit_) return false;
    const Layer* layer = activeLayer();
    if (!layer || layer->isGroup || layer->adjustment) return false;
    bool mask = isMaskSelected_ && layer->mask;
    if (!mask && smartObjectBlocksPixels(true)) return false;
    bool healing = tool_ == Tool::SpotHealing, cloning = tool_ == Tool::CloneStamp || (healing && spotHealingMode == 3);
    if (healing && spotHealingMode == 4) return false;   // Patch drags the selection instead
    // Spot Healing and Clone Stamp rework image pixels; they have nothing to do on a mask.
    if ((healing || cloning) && mask) return false;
    std::optional<CloneSource> clone;
    if (cloning) {
        if (!cloneSource) { emit error(healing ? tr("Alt-click where the Healing Brush should copy from first.") : tr("Alt-click where Clone Stamp should copy from first.")); return false; }
        QPointF offset = cloneAligned && cloneOffset ? *cloneOffset : QPointF(std::round(cloneSource->x() - documentPoint.x()), std::round(cloneSource->y() - documentPoint.y()));
        // The sample is the document (or the layer alone) at document size; keep it between strokes until
        // something changes, since a stroke start is where latency shows.
        std::shared_ptr<const Image> sample;
        bool cached = cloneSample_ && cloneSampleAll_ == cloneSampleAll && cloneSampleLayer_ == layer->id && cloneSampleRevision_ == documentRevision_;
        if (cached) sample = cloneSample_;
        else if (cloneSampleAll) sample = renderFlattened(*document_);
        else if (layer->asset && layer->asset->image) {
            Document single(document_->width, document_->height);
            Layer copy = *layer;
            copy.parentId.reset(); copy.visible = true; copy.opacity = 1; copy.blendMode = BlendMode::Normal; copy.mask.reset(); copy.maskSourceId.reset();
            single.layers = {copy};
            sample = renderFlattened(single);
        } else sample = std::make_shared<Image>(document_->width, document_->height);
        cloneSample_ = sample; cloneSampleAll_ = cloneSampleAll; cloneSampleLayer_ = layer->id; cloneSampleRevision_ = documentRevision_;
        cloneOffset = offset;
        clone = CloneSource{sample, {offset.x(), offset.y()}};
    }
    if (!mask && !effectiveVisibleIds(document_->layers).count(layer->id)) { emit error(tr("The active layer is hidden.")); return false; }
    BrushSettings settings = brushSettings;
    settings.erasing = tool_ == Tool::Brush && brushErase && !mask;
    settings.healing = healing;
    settings.healingMode = spotHealingMode;
    settings.healingSeed = uint32_t(std::random_device{}());
    if (mask) settings.maskValue = (maskPaintWhite != brushErase) ? 1 : 0;
    if (mask && paintsQuickMask()) settings.maskValue = 1 - settings.maskValue;   // white selects: the Quick Mask holds the inverse
    else { settings.red = foregroundColor.redF(); settings.green = foregroundColor.greenF(); settings.blue = foregroundColor.blueF(); }
    const GrayImage* selection = document_->selection && document_->selection->coverage ? document_->selection->coverage.get() : nullptr;
    if (document_->selection && !selection) return false; // an explicit empty selection: touch nothing
    stroke_ = std::make_unique<BrushStroke>(*layer, mask, settings, document_->size(), selection);
    if (!stroke_->isValid()) { emit error(QString::fromStdString(stroke_->error())); stroke_.reset(); return false; }
    strokeLayerId_ = layer->id;
    strokeMask_ = mask;
    if (clone) stroke_->setClone(*clone);
    const BrushPreset* preset = tool_ == Tool::Brush && !brushPreset.isEmpty() ? BrushLibrary::find(brushPreset) : nullptr;
    if (preset && preset->engine == BrushPreset::Engine::Tip) {
        auto tip = preset->tip();
        if (!tip) { emit error(tr("The brush %1 could not be read.").arg(preset->name)); stroke_.reset(); return false; }
        tipStroke_ = std::make_unique<TipStroke>(*stroke_, tip->tip, settings.diameter, uint32_t(std::random_device{}()));
        if (!tipStroke_->isValid()) { emit error(tr("The brush %1 has no usable tip.").arg(preset->name)); tipStroke_.reset(); stroke_.reset(); return false; }
    } else if (preset && !mask) {
        myPaint_ = std::make_unique<MyPaintStroke>(*stroke_, preset->json, settings);
        if (!myPaint_->isValid()) { emit error(QString::fromStdString(myPaint_->error())); myPaint_.reset(); stroke_.reset(); return false; }
    }
    if (tipStroke_) {
        if (straightFromLast && lastBrushPoint_) tipTo(*lastBrushPoint_);
        tipTo(documentPoint);
    } else if (myPaint_) {
        lastPenTime_ = pen.timeMs;
        if (straightFromLast && lastBrushPoint_) myPaintTo(*lastBrushPoint_);
        myPaintTo(documentPoint);
    } else {
        if (straightFromLast && lastBrushPoint_) stroke_->append(toPoint(*lastBrushPoint_));
        stroke_->append(toPoint(documentPoint));
    }
    lastBrushPoint_ = documentPoint;
    // Only what the press painted: a MyPaint press paints nothing until the pen moves, and an empty region
    // would make the canvas render the whole view again.
    strokeRegion_ = toQRect(stroke_->takeDirtyRect());
    if (!strokeRegion_.isEmpty()) emit documentChanged(strokeRegion_);
    if (myPaint_) myPaintSettle_.start();
    if (settings.healing) healPreview_.start();
    return true;
}

void EditorSession::myPaintTo(QPointF documentPoint) {
    MyPaintInput input;
    input.document = toPoint(documentPoint);
    input.pressure = pen.pressure;
    input.xtilt = pen.xtilt;
    input.ytilt = pen.ytilt;
    // Seconds since the last event; events without a timestamp count as 120 a second.
    input.seconds = pen.timeMs > lastPenTime_ ? (pen.timeMs - lastPenTime_) / 1000.0 : 1.0 / 120;
    lastPenTime_ = pen.timeMs;
    myPaint_->strokeTo(input);
}

void EditorSession::tipTo(QPointF documentPoint) {
    tipStroke_->strokeTo({toPoint(documentPoint), pen.tablet ? pen.pressure : 1.0});
}

void EditorSession::continueBrush(QPointF documentPoint) {
    if (!stroke_) return;
    if (tipStroke_) tipTo(documentPoint);
    else if (myPaint_) { myPaintTo(documentPoint); myPaintSettle_.start(); }   // restarts: ticks only once input pauses
    else stroke_->append(toPoint(documentPoint));
    lastBrushPoint_ = documentPoint;
    Rect dirty = stroke_->takeDirtyRect();
    if (!dirty.isEmpty()) {
        strokeRegion_ = strokeRegion_.isEmpty() ? toQRect(dirty) : strokeRegion_.united(toQRect(dirty));
        emit documentChanged(toQRect(dirty));
    }
    if (healPreview_.isActive() || tool_ == Tool::SpotHealing) healPreview_.start();
}

std::unique_ptr<BrushStroke> EditorSession::makeRasterEdit(const Layer& layer, bool mask, const BrushSettings& settings) const {
    const GrayImage* selection = document_->selection && document_->selection->coverage ? document_->selection->coverage.get() : nullptr;
    if (document_->selection && !selection) return nullptr; // an explicit empty selection: touch nothing
    auto stroke = std::make_unique<BrushStroke>(layer, mask, settings, document_->size(), selection);
    if (!stroke->isValid()) return nullptr;
    return stroke;
}

void EditorSession::commitRasterEdit(BrushStroke& stroke, const Uuid& layerId, bool mask, const QString& name, QRectF region, bool previewExact) {
    Layer* layer = document_->find(layerId);
    // `region`: what the edit painted, when known. The committed layer looks as the live preview did, so only
    // that part of the canvas is rendered again; empty means the whole view.
    if (!layer || !stroke.touched()) { if (!region.isEmpty()) emit documentChanged(region); emit historyChanged(); return; }
    // The preview matches the result exactly when the layer sits on whole pixels at its own size: the
    // committed pixels land where the preview drew them, so the canvas has nothing to render again.
    const LayerTransform& placed = layer->transform;
    const bool onPixelGrid = placed.rotation == 0 && !placed.flipX && !placed.flipY
        && placed.origin.x == std::round(placed.origin.x) && placed.origin.y == std::round(placed.origin.y)
        && placed.size.width == layer->pixelWidth() && placed.size.height == layer->pixelHeight();
    const bool asShown = previewExact && onPixelGrid;
    BrushStroke::Commit commit = stroke.commit();
    beginEdit(name);
    // What the stroke painted is all that differs between before and after: undo renders only that.
    if (!region.isEmpty()) history_.noteRegion(Rect(region.x(), region.y(), region.width(), region.height()));
    if (mask) {
        if (commit.mask && layer->mask) { layer->mask->asset = *commit.mask; layer->mask->placement = commit.maskPlacement; }
    } else if (commit.asset) {
        if (layer->mask && layer->mask->linked && !layer->mask->placement && layer->asset && !commit.transform.samePlacement(layer->transform)) layer->mask->placement = layer->transform;
        layer->asset = commit.asset;
        layer->transform = commit.transform;
        layer->shapeImage.reset();
    }
    endEdit();
    if (asShown) {
        documentRevision_++;
        emit documentChangedAsShown();
        emit layersChanged();
        emit historyChanged();
        emit titleChanged();
    } else {
        notifyDocument(region);
    }
}

void EditorSession::endBrush() {
    if (!stroke_) return;
    healPreview_.stop();
    myPaintSettle_.stop();
    if (myPaint_) { myPaint_->finish(); myPaint_.reset(); }
    tipStroke_.reset();
    std::unique_ptr<BrushStroke> stroke = std::move(stroke_);
    stroke->flush();
    Rect tail = stroke->takeDirtyRect();
    if (!tail.isEmpty()) strokeRegion_ = strokeRegion_.isEmpty() ? toQRect(tail) : strokeRegion_.united(toQRect(tail));
    QString name = strokeMask_ ? "Paint Mask" : tool_ == Tool::SpotHealing ? (spotHealingMode == 3 ? "Healing Brush" : "Spot Healing") : tool_ == Tool::CloneStamp ? "Clone Stamp" : tool_ == Tool::Smudge ? (blurMode == BlurToolMode::Sharpen ? "Sharpen" : "Blur")
                 : tool_ == Tool::Dodge ? (toning.kind == ToningKind::Dodge ? "Dodge" : toning.kind == ToningKind::Burn ? "Burn" : "Sponge") : (brushErase ? "Eraser" : "Brush Stroke");
    // Spot healing changes the pixels as it commits; every other brush commits what its preview showed.
    commitRasterEdit(*stroke, strokeLayerId_, strokeMask_, name, strokeRegion_, tool_ != Tool::SpotHealing);
    strokeRegion_ = {};
}

void EditorSession::cancelBrush() {
    if (!stroke_) return;
    healPreview_.stop();
    myPaintSettle_.stop();
    myPaint_.reset();
    tipStroke_.reset();
    stroke_.reset();
    emit documentChanged({});
}

// ---- Opacity keys and brush steps ---------------------------------------------------

void EditorSession::typeOpacityDigit(int digit) {
    if (stroke_ || digit < 0 || digit > 9) return;
    bool brushLike = tool_ == Tool::Brush || tool_ == Tool::SpotHealing || tool_ == Tool::CloneStamp || tool_ == Tool::Smudge || tool_ == Tool::Dodge || tool_ == Tool::PaintBucket;
    if (!brushLike && tool_ != Tool::Gradient && tool_ != Tool::Move) return;
    qint64 now = opacityTimer_.isValid() ? opacityTimer_.elapsed() : 0;
    if (!opacityTimer_.isValid()) opacityTimer_.start();
    int percent = digit == 0 ? 100 : digit * 10;
    if (pendingOpacityDigit_ && now - pendingOpacityDigit_->second < 600) {
        percent = std::max(1, pendingOpacityDigit_->first * 10 + digit);
        pendingOpacityDigit_.reset();
    } else pendingOpacityDigit_ = std::make_pair(digit, now);
    double value = percent / 100.0;
    if (brushLike) { brushSettings.opacity = value; emit toolChanged(); }
    else if (tool_ == Tool::Gradient) { gradientSettings.opacity = value; refreshGradient(); emit toolChanged(); }
    else {
        std::vector<Layer*> targets;
        for (auto& l : document_->layers) if (selectedLayerIds_.count(l.id) && l.opacity != value) targets.push_back(&l);
        if (targets.empty()) return;
        endOpacityEdit();
        beginEdit("Layer Opacity");
        for (Layer* l : targets) l->opacity = value;
        endEdit();
        notifyDocument();
    }
}

void EditorSession::changeBrushHardness(bool increase) {
    if (stroke_) return;
    double quarter = brushSettings.hardness * 4;
    double step = increase ? std::floor(quarter + 0.001) + 1 : std::ceil(quarter - 0.001) - 1;
    brushSettings.hardness = std::min(4.0, std::max(0.0, step)) / 4;
    emit toolChanged();
}

void EditorSession::changeBrushSize(bool increase) {
    if (stroke_) return;
    double current = brushSettings.diameter;
    double stepped = increase ? std::max(current + 1, std::round(current * 1.2)) : std::min(current - 1, std::round(current / 1.2));
    brushSettings.diameter = std::min(2000.0, std::max(1.0, stepped));
    emit toolChanged();
}

// ---- Blur / Smudge / Liquify ---------------------------------------------------------

bool EditorSession::beginWarp(QPointF documentPoint) {
    if (!document_ || stroke_ || warp_ || transformEdit_) return false;
    const Layer* layer = activeLayer();
    if (!layer || layer->isGroup || layer->adjustment || !layer->asset || !layer->asset->image) return false;
    if (smartObjectBlocksPixels(true)) return false;
    if (isMaskSelected_ && blurMode == BlurToolMode::Blur && layer->mask && layer->mask->asset.image) {
        // Blur on a mask: the mask as it sits on the document, its edge tone beyond its pixels, softened.
        const GrayImage& own = *layer->mask->asset.image;
        uint8_t background = LayerMask::background(*layer->mask->asset.thumbnail);
        auto sample = std::make_shared<GrayImage>(document_->width, document_->height, background);
        sampleMaskCoverage(own, layer->maskTransform(), document_->rect(), 1, background, *sample, false);
        Image temp(sample->width(), sample->height());
        for (int y = 0; y < temp.height(); y++) for (int x = 0; x < temp.width(); x++) { uint8_t v = sample->at(x, y); uint8_t* p = temp.pixel(x, y); p[0] = p[1] = p[2] = v; p[3] = 255; }
        gaussianBlur(temp, std::min(30.0, std::max(1.5, brushSettings.diameter / 10)));
        for (int y = 0; y < temp.height(); y++) for (int x = 0; x < temp.width(); x++) sample->at(x, y) = temp.pixel(x, y)[0];
        stroke_ = makeRasterEdit(*layer, true, brushSettings);
        if (!stroke_) return false;
        stroke_->setMaskClone(sample);
        strokeLayerId_ = layer->id;
        strokeMask_ = true;
        stroke_->append(toPoint(documentPoint));
        emit documentChanged({});
        return true;
    }
    if (blurMode == BlurToolMode::Blur || blurMode == BlurToolMode::Sharpen) {
        // Blur paints a softened copy of the layer in place, through the tip; Sharpen a sharpened one.
        const double diameter = brushSettings.diameter;
        if (blurMode == BlurToolMode::Sharpen) return beginProcessedStroke(documentPoint, [](Image& image) { sharpenImage(image); });
        return beginProcessedStroke(documentPoint, [diameter](Image& image) { gaussianBlur(image, std::min(30.0, std::max(1.5, diameter / 10))); });
    }
    if (isMaskSelected_) { emit error(tr("Smudge and Liquify work on a layer's pixels, not its mask.")); return false; }
    if (!effectiveVisibleIds(document_->layers).count(layer->id)) return false;
    // The layer as the canvas shows it, at document size.
    Document single(document_->width, document_->height);
    Layer copy = *layer;
    copy.parentId.reset(); copy.visible = true; copy.opacity = 1; copy.blendMode = BlendMode::Normal; copy.mask.reset(); copy.maskSourceId.reset();
    copy.transform = displayedTransform(*layer);
    single.layers = {copy};
    auto rendered = renderFlattened(single);
    warp_ = std::make_unique<WarpStroke>(rendered, blurMode == BlurToolMode::Smudge ? WarpMode::Smudge : WarpMode::Liquify, brushSettings.diameter, brushSettings.hardness, brushSettings.opacity);
    warpLayerId_ = layer->id;
    warpTransform_ = copy.transform;
    warp_->append(toPoint(documentPoint));
    lastBrushPoint_ = documentPoint;
    emit documentChanged({});
    return true;
}

bool EditorSession::beginProcessedStroke(QPointF documentPoint, const std::function<void(Image&)>& process) {
    const Layer* layer = activeLayer();
    if (!document_ || !layer || stroke_ || warp_) return false;
    if (isMaskSelected_) { emit error(tr("This tool works on a layer's pixels, not its mask.")); return false; }
    if (!effectiveVisibleIds(document_->layers).count(layer->id)) return false;
    // The layer as the canvas shows it, at document size, processed, then painted back through the tip.
    Document single(document_->width, document_->height);
    Layer copy = *layer;
    copy.parentId.reset(); copy.visible = true; copy.opacity = 1; copy.blendMode = BlendMode::Normal; copy.mask.reset(); copy.maskSourceId.reset();
    copy.transform = displayedTransform(*layer);
    single.layers = {copy};
    auto rendered = renderFlattened(single);
    process(*rendered);
    stroke_ = makeRasterEdit(*layer, false, brushSettings);
    if (!stroke_) return false;
    stroke_->setClone(CloneSource{rendered, {0, 0}}, false);
    strokeLayerId_ = layer->id;
    strokeMask_ = false;
    stroke_->append(toPoint(documentPoint));
    emit documentChanged({});
    return true;
}

bool EditorSession::beginToning(QPointF documentPoint) {
    if (!document_ || stroke_ || warp_ || transformEdit_) return false;
    const Layer* layer = activeLayer();
    if (!layer || layer->isGroup || layer->adjustment || !layer->asset || !layer->asset->image) return false;
    if (smartObjectBlocksPixels(true)) return false;
    const ToningSettings settings = toning;
    return beginProcessedStroke(documentPoint, [settings](Image& image) { toneImage(image, settings); });
}

void EditorSession::continueWarp(QPointF documentPoint) {
    if (stroke_ && !warp_) { continueBrush(documentPoint); return; }
    if (!warp_) return;
    warp_->append(toPoint(documentPoint));
    lastBrushPoint_ = documentPoint;
    // The warp image is at document size, so its changed pixels are the document area to render again.
    const Rect dirty = warp_->takeDirtyRect();
    if (!dirty.isEmpty()) emit documentChanged(toQRect(dirty));
}

void EditorSession::endWarp() {
    if (stroke_ && !warp_) { endBrush(); return; }
    if (!warp_) return;
    std::unique_ptr<WarpStroke> warp = std::move(warp_);
    Layer* layer = document_->find(warpLayerId_);
    if (!layer || warp->points().empty()) { emit documentChanged({}); return; }
    // Paint the result into the layer's pixels along the stroke: a hard tip a little wider than the brush.
    BrushSettings settings = brushSettings;
    settings.diameter = std::min(2000.0, warp->diameter() + 4);
    settings.hardness = 1;
    settings.opacity = 1;
    auto stroke = makeRasterEdit(*layer, false, settings);
    if (!stroke) { emit documentChanged({}); return; }
    stroke->setClone(CloneSource{warp->image(), {0, 0}}, true);
    stroke->appendAll(warp->points());
    stroke->flush();
    commitRasterEdit(*stroke, layer->id, false, blurMode == BlurToolMode::Smudge ? "Smudge" : "Liquify");
}

void EditorSession::cancelWarp() {
    if (stroke_ && !warp_) { cancelBrush(); return; }
    if (!warp_) return;
    warp_.reset();
    emit documentChanged({});
}

// ---- Gradient ---------------------------------------------------------------------------

std::optional<std::pair<QPointF, QPointF>> EditorSession::gradientLine() const {
    if (!gradient_) return std::nullopt;
    return std::make_pair(gradient_->start, gradient_->end);
}

void EditorSession::beginGradient(QPointF documentPoint) {
    if (!document_ || stroke_ || transformEdit_) return;
    const Layer* layer = activeLayer();
    if (!layer) return;
    if (smartObjectBlocksPixels(true)) return;
    bool mask = isMaskSelected_ && layer->mask;
    if (gradient_ && gradient_->layerId == layer->id && gradient_->mask == mask) {
        gradient_->start = gradient_->end = documentPoint;
        refreshGradient();
        return;
    }
    if (layer->isGroup && !mask) return;
    if (layer->adjustment && !mask) return;
    if (!mask && !effectiveVisibleIds(document_->layers).count(layer->id)) return;
    resolveGradient();
    endOpacityEdit();
    auto raster = makeRasterEdit(*layer, mask, BrushSettings());
    if (!raster) return;
    gradient_ = std::make_unique<GradientEdit>();
    gradient_->raster = std::move(raster);
    gradient_->layerId = layer->id;
    gradient_->mask = mask;
    gradient_->start = gradient_->end = documentPoint;
    emit documentChanged({});
}

void EditorSession::moveGradient(QPointF end) {
    if (!gradient_) return;
    gradient_->end = end;
    refreshGradient();
}

void EditorSession::refreshGradient() {
    if (!gradient_) return;
    QPointF a = gradient_->start, b = gradient_->end;
    if (std::hypot(b.x() - a.x(), b.y() - a.y()) >= 0.5) {
        QColor fg = foregroundColor, bg = backgroundColor;
        const int shape = gradientSettings.shape == GradientShape::Radial ? 1 : 0;
        if (const GradientPreset* preset = gradientSettings.preset.isEmpty() ? nullptr : PresetLibrary::instance().findGradient(gradientSettings.preset)) {
            const float f[3] = {float(fg.redF()), float(fg.greenF()), float(fg.blueF())}, g[3] = {float(bg.redF()), float(bg.greenF()), float(bg.blueF())};
            GradientStops stops = preset->stops(f, g);
            if (gradient_->mask)   // a mask takes each stop's lightness
                for (auto& s : stops.colors) {
                    float v = 0.299f * s.rgb[0] + 0.587f * s.rgb[1] + 0.114f * s.rgb[2];
                    if (paintsQuickMask()) v = 1 - v;
                    s.rgb[0] = s.rgb[1] = s.rgb[2] = v;
                }
            if (gradientSettings.reversed) stops.reverse();
            gradient_->raster->fillGradientOver(shape, toPoint(a), toPoint(b), stops, gradientSettings.opacity);
            emit documentChanged({});
            return;
        }
        float start[4], end[4];
        if (gradient_->mask) {
            float f = fg.lightnessF() >= 0.5 ? 1 : 0, g = bg.lightnessF() >= 0.5 ? 1 : 0;
            if (paintsQuickMask()) { f = 1 - f; g = 1 - g; }
            start[0] = start[1] = start[2] = f; start[3] = 1;
            end[0] = end[1] = end[2] = gradientSettings.style == GradientStyle::ForegroundToBackground ? g : f;
            end[3] = gradientSettings.style == GradientStyle::ForegroundToBackground ? 1 : 0;
        } else {
            start[0] = fg.redF(); start[1] = fg.greenF(); start[2] = fg.blueF(); start[3] = 1;
            if (gradientSettings.style == GradientStyle::ForegroundToBackground) { end[0] = bg.redF(); end[1] = bg.greenF(); end[2] = bg.blueF(); end[3] = 1; }
            else { end[0] = fg.redF(); end[1] = fg.greenF(); end[2] = fg.blueF(); end[3] = 0; }
        }
        if (gradientSettings.reversed) for (int c = 0; c < 4; c++) std::swap(start[c], end[c]);
        gradient_->raster->fillGradientOver(shape, toPoint(a), toPoint(b), start, end, gradientSettings.opacity);
    }
    emit documentChanged({});
}

void EditorSession::endGradientDrag() {
    if (!gradient_) return;
    QPointF a = gradient_->start, b = gradient_->end;
    if (std::hypot(b.x() - a.x(), b.y() - a.y()) < 0.5) cancelGradient();
}

void EditorSession::cancelGradient() {
    if (!gradient_) return;
    gradient_.reset();
    emit documentChanged({});
    emit historyChanged();
}

void EditorSession::commitGradient() {
    if (!gradient_) return;
    std::unique_ptr<GradientEdit> edit = std::move(gradient_);
    QPointF a = edit->start, b = edit->end;
    if (std::hypot(b.x() - a.x(), b.y() - a.y()) < 0.5) { emit documentChanged({}); return; }
    commitRasterEdit(*edit->raster, edit->layerId, edit->mask, edit->mask ? "Gradient Mask" : "Gradient");
}

void EditorSession::resolveGradient() { if (gradient_) commitGradient(); }

// ---- Shape ------------------------------------------------------------------------------

void EditorSession::beginShape(QPointF documentPoint) {
    if (!canEditLayers()) return;
    QPointF anchor(std::round(documentPoint.x()), std::round(documentPoint.y()));
    shapeDraft_ = ShapeDraft{shapeKind, anchor, QRectF(anchor, QSizeF(0, 0)), shapeKind == ShapeKind::Rectangle ? shapeCornerRadius : 0};
    emit transformChanged();
}

void EditorSession::dragShape(QPointF point, bool square, bool fromCenter) {
    if (!shapeDraft_) return;
    QPointF anchor = shapeDraft_->anchor;
    double dx = std::round(point.x()) - anchor.x(), dy = std::round(point.y()) - anchor.y();
    if (square) { double side = std::max(std::fabs(dx), std::fabs(dy)); dx = dx < 0 ? -side : side; dy = dy < 0 ? -side : side; }
    shapeDraft_->rect = fromCenter ? QRectF(anchor.x() - std::fabs(dx), anchor.y() - std::fabs(dy), std::fabs(dx) * 2, std::fabs(dy) * 2)
                                   : QRectF(std::min(anchor.x(), anchor.x() + dx), std::min(anchor.y(), anchor.y() + dy), std::fabs(dx), std::fabs(dy));
    emit transformChanged();
}

void EditorSession::cancelShape() { if (shapeDraft_) { shapeDraft_.reset(); emit transformChanged(); } }

void EditorSession::toggleShapeKind() {
    cancelShape();
    shapeKind = shapeKind == ShapeKind::Rectangle ? ShapeKind::Ellipse : ShapeKind::Rectangle;
    emit toolChanged();
}

void EditorSession::finishShape() {
    if (!shapeDraft_) return;
    ShapeDraft draft = *shapeDraft_;
    shapeDraft_.reset();
    emit transformChanged();
    if (!canEditLayers() || draft.rect.width() < 1 || draft.rect.height() < 1) return;
    int w = int(draft.rect.width()), h = int(draft.rect.height());
    if ((long long)w * h > Document::pixelBudget) { emit error(tr("That shape is too large. A shape can cover up to 100 megapixels.")); return; }
    QColor c = foregroundColor;
    auto image = shapeImage(draft.kind, w, h, c.redF(), c.greenF(), c.blueF(), draft.cornerRadius);
    std::string prefix = draft.kind == ShapeKind::Ellipse ? "Ellipse" : "Rectangle";
    Layer layer(Asset::make(image, nextLayerName(document_->layers, prefix)), toPoint(draft.rect.topLeft()));
    layer.name = layer.asset->name;
    layer.shape = LayerShapeStyle{draft.kind, c.redF(), c.greenF(), c.blueF(), draft.cornerRadius};
    layer.shapeImage = image;
    const Layer* active = activeLayer();
    layer.parentId = active && active->isGroup ? activeLayerId_ : (active ? active->parentId : std::nullopt);
    int index = activeLayerId_ ? document_->indexOf(*activeLayerId_) + 1 : int(document_->layers.size());
    endOpacityEdit();
    beginEdit(QString::fromStdString(prefix));
    document_->layers.insert(document_->layers.begin() + index, layer);
    setActiveLayer(layer.id);
    endEdit();
    notifyDocument();
}

// ---- Text ----------------------------------------------------------------------------------

bool EditorSession::redrawText(Layer& layer) {
    if (!layer.text) return false;
    // Warped text: where the bent raster sits from the upright one, now and at the last redraw (so the upright text
    // stays where it was and only the bend moves the pixels).
    QPointF warpOffset;
    auto image = renderTextLayer(*layer.text, &warpOffset);
    QPointF oldWarpOffset;
    if (layer.extraJson.find("textWarpOffset") != std::string::npos) {
        const QJsonArray o = QJsonDocument::fromJson(QByteArray::fromStdString(layer.extraJson)).object().value("textWarpOffset").toArray();
        if (o.size() == 2) oldWarpOffset = QPointF(o[0].toDouble(), o[1].toDouble());
    }
    if (!image) { emit error(tr("That text is too large to render. Text can cover up to 100 megapixels.")); return false; }
    // A layer scaled on the canvas keeps its scale; the box follows the new raster.
    double scaleX = 1, scaleY = 1;
    if (layer.asset && layer.asset->image && layer.asset->image->width() > 0 && layer.asset->image->height() > 0) {
        scaleX = layer.transform.size.width / layer.asset->image->width();
        scaleY = layer.transform.size.height / layer.asset->image->height();
    }
    if (layer.mask && !layer.mask->placement) layer.mask->placement = layer.maskTransform();
    // A turned layer turns about its centre, so a new size would slide it: its top-left corner stays put instead.
    std::optional<Point> corner;
    if (layer.transform.rotation != 0 && !layer.transform.flipX && !layer.transform.flipY && layer.asset && layer.asset->image)
        corner = mapThroughTransform(layer.transform, layer.asset->image->width(), layer.asset->image->height(), 0, 0);
    layer.asset = Asset::make(image, layer.name);
    layer.textImage = image;
    layer.transform.size = Size(image->width() * scaleX, image->height() * scaleY);
    if (corner) {
        const double a = layer.transform.radians(), c = std::cos(a), sn = std::sin(a);
        const double hw = layer.transform.size.width / 2, hh = layer.transform.size.height / 2;
        const Point centre(corner->x + hw * c - hh * sn, corner->y + hw * sn + hh * c);
        layer.transform.origin = Point(centre.x - hw, centre.y - hh);
    }
    if (warpOffset != oldWarpOffset) {
        // The bend's own shift, turned with the layer.
        const double a = layer.transform.radians(), c = std::cos(a), sn = std::sin(a);
        const double dx = (warpOffset.x() - oldWarpOffset.x()) * scaleX, dy = (warpOffset.y() - oldWarpOffset.y()) * scaleY;
        layer.transform.origin = Point(layer.transform.origin.x + dx * c - dy * sn, layer.transform.origin.y + dx * sn + dy * c);
    }
    // Text opened from a PSD shows Photoshop's pixels until this first redraw: put our first baseline where
    // Photoshop anchored its own, then forget the anchor.
    if (layer.extraJson.find("psdTextAnchor") != std::string::npos) {
        QJsonObject extra = QJsonDocument::fromJson(QByteArray::fromStdString(layer.extraJson)).object();
        const QJsonArray anchor = extra.value("psdTextAnchor").toArray();
        // Text Photoshop turned: drawn upright, the layer turned by its angle about the anchor.
        const double turned = extra.value("psdTextRotation").toDouble(0);
        if (anchor.size() == 2 && (layer.transform.rotation == 0 || turned != 0)) {
            if (auto m = psdTextMetrics(*layer.text)) {
                // Box text is anchored at its frame's top-left, point text at its first baseline.
                const bool boxed = layer.text->boxWidth > 0 && layer.text->boxHeight > 0;
                // (In the bent raster's pixels when warped: the upright anchor, less the bend's shift.)
                const double x = m->blockLeft + (boxed ? 0 : layer.text->alignment == 1 ? m->blockWidth / 2 : layer.text->alignment == 2 ? m->blockWidth : 0) - warpOffset.x();
                const double y = m->blockTop + (boxed ? 0 : m->ascent) - warpOffset.y();
                if (turned == 0) layer.transform.origin = Point(anchor[0].toDouble() - x * scaleX, anchor[1].toDouble() - y * scaleY);
                else {
                    // The layer turns about its centre: put the centre where the turned anchor offset says.
                    layer.transform.size = Size(image->width(), image->height());
                    layer.transform.rotation = turned;
                    const double a = turned * M_PI / 180, c = std::cos(a), sn = std::sin(a);
                    const double dx = x - image->width() / 2.0, dy = y - image->height() / 2.0;
                    const Point centre(anchor[0].toDouble() - (dx * c - dy * sn), anchor[1].toDouble() - (dx * sn + dy * c));
                    layer.transform.origin = Point(centre.x - image->width() / 2.0, centre.y - image->height() / 2.0);
                }
            }
        }
        extra.remove("psdTextAnchor");
        extra.remove("psdTextRotation");
        layer.extraJson = extra.isEmpty() ? std::string() : QJsonDocument(extra).toJson(QJsonDocument::Compact).toStdString();
    }
    // Remember the bend's shift for the next redraw.
    if (warpOffset != oldWarpOffset || layer.extraJson.find("textWarpOffset") != std::string::npos) {
        QJsonObject extra = QJsonDocument::fromJson(QByteArray::fromStdString(layer.extraJson.empty() ? std::string("{}") : layer.extraJson)).object();
        if (warpOffset.isNull()) extra.remove("textWarpOffset");
        else extra.insert("textWarpOffset", QJsonArray{warpOffset.x(), warpOffset.y()});
        layer.extraJson = extra.isEmpty() ? std::string() : QJsonDocument(extra).toJson(QJsonDocument::Compact).toStdString();
    }
    return true;
}

std::optional<Uuid> EditorSession::addTextLayer(QPointF documentPoint, const LayerText& text, bool openEditor) {
    if (!canEditLayers()) return std::nullopt;
    auto image = renderTextLayer(text);
    if (!image) { emit error(tr("That text is too large to render. Text can cover up to 100 megapixels.")); return std::nullopt; }
    Layer layer(Asset::make(image, nextLayerName(document_->layers, "Text")), Point(documentPoint.x() - textPadding, documentPoint.y() - textPadding));
    layer.name = layer.asset->name;
    layer.text = text;
    layer.textImage = image;
    const Layer* active = activeLayer();
    layer.parentId = active && active->isGroup ? activeLayerId_ : (active ? active->parentId : std::nullopt);
    int index = activeLayerId_ ? document_->indexOf(*activeLayerId_) + 1 : int(document_->layers.size());
    endOpacityEdit();
    beginEdit("Add Text");
    document_->layers.insert(document_->layers.begin() + index, layer);
    setActiveLayer(layer.id);
    endEdit();
    notifyDocument();
    if (openEditor) emit textEditRequested(layer.id);
    return layer.id;
}

std::optional<LayerText> EditorSession::layerText(const Uuid& id) const {
    if (!document_) return std::nullopt;
    const Layer* layer = document_->find(id);
    if (!layer || !layer->isLiveText()) return std::nullopt;
    return layer->text;
}

void EditorSession::beginTextEdit(const Uuid& id) {
    if (textEditing_ || !document_) return;
    const Layer* layer = document_->find(id);
    if (!layer || !layer->text) return;
    textEditing_ = true;
    textEditOriginal_ = *layer;
    beginEdit("Edit Text");
}

void EditorSession::setLayerText(const Uuid& id, const LayerText& text) {
    if (!document_) return;
    Layer* layer = document_->find(id);
    if (!layer || !layer->text) return;
    // Text in several styles: an edit through the plain fields carries into its runs.
    // While an edit session previews, each change is carried from the text as it was when editing began.
    const bool fromOrigin = textEditing_ && textEditOriginal_ && textEditOriginal_->id == id && textEditOriginal_->text;
    const LayerText edited = carryTextEdit(fromOrigin ? *textEditOriginal_->text : *layer->text, text);
    if (*layer->text == edited && layer->isLiveText()) return;
    const bool standalone = !textEditing_;
    if (standalone) beginEdit("Edit Text");
    layer->text = edited;
    redrawText(*layer);
    if (standalone) { endEdit(); notifyDocument(); }
    else { emit documentChanged({}); emit layersChanged(); }
}

void EditorSession::endTextEdit(bool keep) {
    if (!textEditing_) return;
    textEditing_ = false;
    if (!keep && textEditOriginal_ && document_) {
        Layer* layer = document_->find(textEditOriginal_->id);
        if (layer) *layer = *textEditOriginal_;
    }
    textEditOriginal_.reset();
    endEdit();
    notifyDocument();
}

} // namespace app
