#include "compositor/seamcarve.h"
#include "EditorSession.h"
#include "ImageConvert.h"
#include "QtGeometry.h"
#include "compositor/filters.h"
#include "compositor/inpaint.h"
#include "compositor/contentmove.h"
#include "compositor/seamcarve.h"
#include <QApplication>
#include <QClipboard>
#include <QMimeData>
#include <algorithm>
#include <cstring>
#include <map>
#include <set>

using namespace compositor;

namespace app {

// ---- Moving selected pixels ------------------------------------------------------------------

bool EditorSession::canMovePixels(QPointF documentPoint) const {
    if (!document_ || !document_->selection || !document_->selection->coverage || pixelMove_ || stroke_ || transformEdit_ || isMaskSelected_) return false;
    const Layer* layer = activeLayer();
    if (!layer || !layer->asset || layer->isGroup || layer->adjustment) return false;
    int x = int(std::floor(documentPoint.x())), y = int(std::floor(documentPoint.y()));
    if (x < 0 || y < 0 || x >= document_->width || y >= document_->height) return false;
    return document_->selection->coverage.u8()->at(x, y) > 127;
}

bool EditorSession::beginPixelMove(bool duplicate) {
    if (pixelMove_ || !document_ || !document_->selection || !document_->selection->coverage || document_->selection->isEmpty() || isMaskSelected_) return false;
    const Layer* layer = activeLayer();
    if (!layer || !layer->asset || layer->isGroup || layer->adjustment || stroke_ || transformEdit_) return false;
    resolveGradient();
    auto raster = makeRasterEdit(*layer, false, BrushSettings());
    if (!raster || !raster->liftSelection()) return false;
    endOpacityEdit();
    pixelMove_ = std::make_unique<PixelMove>();
    pixelMove_->raster = std::move(raster);
    pixelMove_->origin = *document_->selection;
    pixelMove_->duplicate = duplicate;
    pixelMove_->layerId = layer->id;
    return true;
}

void EditorSession::movePixels(QPointF offset) {
    if (!pixelMove_) return;
    QPointF rounded(std::round(offset.x()), std::round(offset.y()));
    pixelMove_->raster->moveLifted(toPoint(rounded), pixelMove_->duplicate);
    pixelMove_->offset = rounded;
    emit documentChanged({});
    emit selectionChanged();
}

std::optional<Selection> EditorSession::displayedSelection() const {
    if (!document_ || !document_->selection) return std::nullopt;
    if (pixelMove_ && pixelMove_->origin.coverage) {
        int dx = int(pixelMove_->offset.x()), dy = int(pixelMove_->offset.y());
        if (dx == 0 && dy == 0) return pixelMove_->origin;
        const GrayImage& src = *pixelMove_->origin.coverage.u8();
        auto moved = std::make_shared<GrayImage>(src.width(), src.height(), 0);
        for (int y = 0; y < src.height(); y++) { int sy = y - dy; if (sy < 0 || sy >= src.height()) continue; for (int x = 0; x < src.width(); x++) { int sx = x - dx; if (sx >= 0 && sx < src.width()) moved->at(x, y) = src.at(sx, sy); } }
        Selection s = pixelMove_->origin;
        s.coverage = moved;
        return s;
    }
    if (transformEdit_ && transformEdit_->floating && document_->selection->coverage) {
        const FloatingTransform& f = *transformEdit_->floating;
        const GrayImage& cov = *document_->selection->coverage.u8();
        auto moved = std::make_shared<GrayImage>(cov.width(), cov.height(), 0);
        if (transformEdit_->corners) moved = warpCoverage(cov, f.original, f.pixelWidth, f.pixelHeight, *transformEdit_->corners);
        else {
            Affine map = f.original.pixelToDocument(f.pixelWidth, f.pixelHeight).inverted().concatenating(transformEdit_->draft.pixelToDocument(f.pixelWidth, f.pixelHeight));
            Affine inv = map.inverted();
            for (int y = 0; y < cov.height(); y++) for (int x = 0; x < cov.width(); x++) {
                Point p = inv.apply({x + 0.5, y + 0.5});
                int sx = int(std::floor(p.x)), sy = int(std::floor(p.y));
                if (sx >= 0 && sy >= 0 && sx < cov.width() && sy < cov.height()) moved->at(x, y) = cov.at(sx, sy);
            }
        }
        Selection s = *document_->selection;
        s.coverage = moved;
        return s;
    }
    return document_->selection;
}

void EditorSession::finishPixelMove() {
    if (!pixelMove_) return;
    std::unique_ptr<PixelMove> move = std::move(pixelMove_);
    if (move->offset.isNull()) { emit documentChanged({}); emit selectionChanged(); return; }
    Selection moved = *[&] { pixelMove_ = std::move(move); auto s = displayedSelection(); move = std::move(pixelMove_); return s; }();
    Layer* layer = document_->find(move->layerId);
    if (!layer) return;
    BrushStroke::Commit commit = move->raster->commit();
    beginEdit(move->duplicate ? QT_TRANSLATE_NOOP("History", "Duplicate Pixels") : QT_TRANSLATE_NOOP("History", "Move Pixels"));
    if (commit.asset) {
        if (layer->mask && layer->mask->linked && !layer->mask->placement && !commit.transform.samePlacement(layer->transform)) layer->mask->placement = layer->transform;
        layer->asset = commit.asset;
        layer->transform = commit.transform;
        layer->shapeImage.reset();
    }
    document_->selection = moved;
    endEdit();
    notifyDocument();
    emit selectionChanged();
}

void EditorSession::cancelPixelMove() {
    if (!pixelMove_) return;
    pixelMove_.reset();
    emit documentChanged({});
    emit selectionChanged();
}

void EditorSession::nudgePixels(double dx, double dy) {
    if (!beginPixelMove(false)) return;
    movePixels({dx, dy});
    finishPixelMove();
}

std::optional<QColor> EditorSession::compositeColorAt(QPointF documentPoint) const {
    auto flat = flattened();
    if (!flat) return std::nullopt;
    int x = int(std::floor(documentPoint.x())), y = int(std::floor(documentPoint.y()));
    if (x < 0 || y < 0 || x >= flat->width() || y >= flat->height()) return std::nullopt;
    const uint8_t* p = flat->pixel(x, y);
    if (!p[3]) return std::nullopt;
    return QColor(p[0] * 255 / p[3], p[1] * 255 / p[3], p[2] * 255 / p[3]);
}

// ---- Clipboard and Content-Aware Fill ------------------------------------------------

bool EditorSession::canCopyPixels() const {
    if (!canEditLayers()) return false;
    const Layer* layer = activeLayer();
    if (!layer || (layer->isGroup && !isMaskSelected_)) return false;
    if (document_->selection && document_->selection->isEmpty()) return false;
    return isMaskSelected_ ? layer->mask.has_value() : layer->asset.has_value();
}

std::optional<EditorSession::PixelClipboard> EditorSession::renderSelectedPixels(bool merged) const {
    if (!document_) return std::nullopt;
    Rect region = document_->rect();
    const GrayImage* coverage = nullptr;
    if (document_->selection) {
        if (!document_->selection->coverage || document_->selection->isEmpty()) return std::nullopt;
        coverage = document_->selection->coverage.u8().get();
        region = document_->selection->bounds().intersection(document_->rect());
    }
    if (region.isEmpty()) return std::nullopt;
    Image out(int(region.width), int(region.height));
    RenderOptions options;
    options.region = region;
    if (merged) render(*document_, options, out);
    else {
        const Layer* layer = activeLayer();
        if (!layer) return std::nullopt;
        if (isMaskSelected_ && layer->mask) {
            // The mask as opaque gray, placed as it sits on the document.
            GrayImage gray(out.width(), out.height(), layer->mask->placement ? LayerMask::background(*layer->mask->asset.thumbnail) : 0);
            sampleMaskCoverage(*layer->mask->asset.image.u8(), layer->maskTransform(), region, 1, gray.at(0, 0), gray, false);
            for (int y = 0; y < out.height(); y++) for (int x = 0; x < out.width(); x++) { uint8_t* p = out.pixel(x, y); p[0] = p[1] = p[2] = gray.at(x, y); p[3] = 255; }
        } else if (layer->asset && layer->asset->image.u8()) {
            Document single(document_->width, document_->height);
            Layer copy = *layer;
            copy.parentId.reset(); copy.visible = true; copy.opacity = 1; copy.blendMode = BlendMode::Normal; copy.maskSourceId.reset();
            single.layers = {copy};
            render(single, options, out);
        } else return std::nullopt;
    }
    if (coverage) {
        for (int y = 0; y < out.height(); y++) for (int x = 0; x < out.width(); x++) {
            unsigned k = coverage->at(x + int(region.x), y + int(region.y));
            uint8_t* p = out.pixel(x, y);
            for (int c = 0; c < 4; c++) p[c] = uint8_t((p[c] * k + 127) / 255);
        }
    }
    return PixelClipboard{std::make_shared<Image>(std::move(out)), QPointF(region.x, region.y)};
}

void EditorSession::copySelection() {
    if (!canCopyPixels()) return;
    auto copied = renderSelectedPixels(false);
    if (!copied) return;
    pixelClipboard_ = copied;
    QApplication::clipboard()->setImage(toQImage(*copied->image).convertToFormat(QImage::Format_ARGB32));
}

void EditorSession::copyMerged() {
    if (!canEditLayers() || (document_->selection && document_->selection->isEmpty())) return;
    auto copied = renderSelectedPixels(true);
    if (!copied) return;
    pixelClipboard_ = copied;
    QApplication::clipboard()->setImage(toQImage(*copied->image).convertToFormat(QImage::Format_ARGB32));
}

void EditorSession::cutSelection() {
    if (!document_ || !document_->selection || !canCopyPixels()) return;
    copySelection();
    clearSelectionPixels();
}

bool EditorSession::canPaste() const {
    if (!document_ || !canEditLayers()) return false;
    return pixelClipboard_.has_value() || QApplication::clipboard()->mimeData()->hasImage();
}

void EditorSession::paste() {
    if (!canPaste()) return;
    const QMimeData* mime = QApplication::clipboard()->mimeData();
    QImage external = mime->hasImage() ? qvariant_cast<QImage>(mime->imageData()) : QImage();
    // Pixels copied here go back exactly where they came from unless another app copied since.
    if (pixelClipboard_ && (!mime->hasImage() || (external.width() == pixelClipboard_->image->width() && external.height() == pixelClipboard_->image->height()))) {
        addPixelLayer(pixelClipboard_->image, pixelClipboard_->origin, QT_TRANSLATE_NOOP("History", "Paste"), true);
        return;
    }
    if (external.isNull()) return;
    QPointF origin(std::floor((document_->width - external.width()) / 2.0), std::floor((document_->height - external.height()) / 2.0));
    addPixelLayer(fromQImage(external), origin, QT_TRANSLATE_NOOP("History", "Paste"), true);
}

void EditorSession::layerViaCopy() {
    if (!canEditLayers()) return;
    const Layer* layer = activeLayer();
    if (!layer || layer->isGroup) return;
    if (!document_->selection) { duplicateActiveLayer(); return; }
    auto copied = renderSelectedPixels(false);
    if (!copied) return;
    addPixelLayer(copied->image, copied->origin, QT_TRANSLATE_NOOP("History", "Layer via Copy"), false);
}

void EditorSession::addPixelLayer(std::shared_ptr<const Image> image, QPointF origin, const QString& editName, bool dropsSelection) {
    if (!document_ || !image || document_->layers.size() >= size_t(Document::maxLayers)) return;
    Layer layer(Asset::make(image, nextLayerName(document_->layers, QCoreApplication::translate("Names", "Layer").toStdString())), toPoint(origin));
    const Layer* active = activeLayer();
    layer.parentId = active && active->isGroup ? activeLayerId_ : (active ? active->parentId : std::nullopt);
    int index = activeLayerId_ ? document_->indexOf(*activeLayerId_) + 1 : int(document_->layers.size());
    endOpacityEdit();
    beginEdit(editName);
    document_->layers.insert(document_->layers.begin() + index, layer);
    if (dropsSelection) document_->selection.reset();
    setActiveLayer(layer.id);
    endEdit();
    notifyDocument();
    if (dropsSelection) emit selectionChanged();
}

std::shared_ptr<const Image> EditorSession::contentAwareFillResult(const ContentFillRequest& request, LayerTransform& placed, QString* errorText) const {
    if (!canAdjustPixels() || !document_->selection || !document_->selection->coverage) { if (errorText) *errorText = tr("Select a visible image layer and an area to fill."); return nullptr; }
    const Layer* layer = activeLayer();
    // The layer grows over any of the selection on the canvas past its edge.
    Rect area = document_->selection->bounds().intersection(document_->rect());
    const Image& src = *layer->asset->image.u8();
    Affine toPixels = layer->transform.pixelToDocument(src.width(), src.height()).inverted();
    Rect wanted = toPixels.mapBounds(area).integral().unionWith(Rect(0, 0, src.width(), src.height()));
    int margin = int(std::ceil(std::max({0.0, -wanted.minX(), -wanted.minY(), wanted.maxX() - src.width(), wanted.maxY() - src.height()})));
    LayerTransform grown;
    auto source = adjustmentSource(margin, grown);
    if (!source) { if (errorText) *errorText = tr("The layer is too large to grow."); return nullptr; }
    auto coverage = selectionOnGrid(grown, source->width(), source->height());
    if (!coverage) { if (errorText) *errorText = tr("Select an area to fill."); return nullptr; }
    auto out = std::make_shared<Image>(*source);
    // What the layer's mask hides is not copied from: a fill at the edge of a cut-out takes the subject.
    std::shared_ptr<GrayImage> visible;
    if (layer->mask && layer->mask->enabled && !layer->mask->placement && layer->mask->asset.image.u8() && layer->mask->asset.image.u8()->width() == src.width() && layer->mask->asset.image.u8()->height() == src.height()) {
        visible = std::make_shared<GrayImage>(source->width(), source->height(), 255);
        const GrayImage& m = *layer->mask->asset.image.u8();
        for (int y = 0; y < m.height(); y++) std::memcpy(visible->row(y + margin) + margin, m.row(y), size_t(m.width()));
    }
    InpaintOptions options;
    if (request.sampling != ContentFillRequest::Sampling::Auto) {
        // All of the layer, or the area painted in the dialog, on the layer's grid and inside what its mask shows.
        std::shared_ptr<GrayImage> sample;
        if (request.sampling == ContentFillRequest::Sampling::Custom && request.sampleArea && request.sampleArea->width() == document_->width && request.sampleArea->height() == document_->height)
            sample = selectionInGrid(*request.sampleArea, grown.pixelToDocument(source->width(), source->height()), source->width(), source->height());
        else sample = std::make_shared<GrayImage>(source->width(), source->height(), 255);
        if (visible) for (size_t i = 0; i < sample->byteCount(); i++) sample->data()[i] = uint8_t(sample->data()[i] * visible->data()[i] / 255);
        visible = sample;
        options.sampleWholeVisible = true;
    }
    if (!contentFill(*out, *coverage, options, visible.get())) {
        if (errorText) *errorText = request.sampling == ContentFillRequest::Sampling::Custom ? tr("The sampling area holds no opaque image pixels outside the selection to copy from.")
                                                                                               : tr("Not enough unselected, opaque image pixels to synthesize a fill. Use a smaller selection with some surrounding image.");
        return nullptr;
    }
    if (request.newLayer) {
        // Only the filled pixels, by the selection's coverage, for a layer of their own.
        for (int y = 0; y < out->height(); y++)
            for (int x = 0; x < out->width(); x++) {
                uint8_t* p = out->pixel(x, y);
                const int c = coverage->at(x, y);
                for (int k = 0; k < 4; k++) p[k] = uint8_t((p[k] * c + 127) / 255);
            }
    }
    return trimToPixels(*out, grown, placed);
}

bool EditorSession::contentAwareFill(QString* errorText, const ContentFillRequest& request) {
    LayerTransform placed;
    auto result = contentAwareFillResult(request, placed, errorText);
    if (!result) return false;
    if (!request.newLayer) { commitPixels(result, placed, QT_TRANSLATE_NOOP("History", "Content-Aware Fill")); return true; }
    if (document_->layers.size() >= size_t(Document::maxLayers)) { if (errorText) *errorText = tr("The document has too many layers."); return false; }
    const Layer* active = activeLayer();
    Layer layer(Asset::make(result, nextLayerName(document_->layers, QCoreApplication::translate("Names", "Layer").toStdString())), Point{0, 0});
    layer.transform = placed;
    layer.parentId = active ? active->parentId : std::nullopt;
    const int index = activeLayerId_ ? document_->indexOf(*activeLayerId_) + 1 : int(document_->layers.size());
    clearPixelPreview();
    endOpacityEdit();
    beginEdit(QT_TRANSLATE_NOOP("History", "Content-Aware Fill"));
    document_->layers.insert(document_->layers.begin() + index, layer);
    setActiveLayer(layer.id);
    endEdit();
    notifyDocument();
    return true;
}

bool EditorSession::contentAwareMove(int dx, int dy, QString* errorText) {
    auto failWith = [&](const QString& text) { if (errorText) *errorText = text; return false; };
    if (!canAdjustPixels() || !document_->selection || !document_->selection->coverage) return failWith(tr("Select an area on a visible image layer, then drag it where it should go."));
    if (dx == 0 && dy == 0) return failWith(tr("Drag the selection to where it should go."));
    if (isMaskSelected_) return failWith(tr("Content-Aware Move works on a layer's pixels, not its mask."));
    if (smartObjectBlocksPixels(true)) return false;
    const Layer* layer = activeLayer();
    // The layer grows over the selection and where it lands.
    Rect sel = document_->selection->bounds();
    Rect area = sel.unionWith(Rect(sel.x + dx, sel.y + dy, sel.width, sel.height)).intersection(document_->rect());
    const Image& src = *layer->asset->image.u8();
    Affine toPixels = layer->transform.pixelToDocument(src.width(), src.height()).inverted();
    Rect wanted = toPixels.mapBounds(area).integral().unionWith(Rect(0, 0, src.width(), src.height()));
    int margin = int(std::ceil(std::max({0.0, -wanted.minX(), -wanted.minY(), wanted.maxX() - src.width(), wanted.maxY() - src.height()})));
    LayerTransform grown;
    auto source = adjustmentSource(margin, grown);
    if (!source) return failWith(tr("The layer is too large to grow."));
    auto coverage = selectionOnGrid(grown, source->width(), source->height());
    if (!coverage) return failWith(tr("Select the area to move first."));
    // The drag on the layer's own grid.
    Affine gridFromDoc = grown.pixelToDocument(source->width(), source->height()).inverted();
    const Point o = gridFromDoc.apply({0, 0}), d = gridFromDoc.apply({double(dx), double(dy)});
    const int gdx = int(std::lround(d.x - o.x)), gdy = int(std::lround(d.y - o.y));
    std::shared_ptr<GrayImage> visible;
    if (layer->mask && layer->mask->enabled && !layer->mask->placement && layer->mask->asset.image.u8() && layer->mask->asset.image.u8()->width() == src.width() && layer->mask->asset.image.u8()->height() == src.height()) {
        visible = std::make_shared<GrayImage>(source->width(), source->height(), 255);
        const GrayImage& m = *layer->mask->asset.image.u8();
        for (int y = 0; y < m.height(); y++) std::memcpy(visible->row(y + margin) + margin, m.row(y), size_t(m.width()));
    }
    auto out = std::make_shared<Image>(*source);
    ContentMoveOptions options;
    options.extend = contentMoveExtend;
    options.adaptation = contentMoveAdaptation;
    if (!compositor::contentAwareMove(*out, *coverage, gdx, gdy, options, visible.get()))
        return failWith(tr("Nothing could be moved there: the selection must land on the layer and leave opaque pixels around it to fill from."));
    LayerTransform placed;
    auto trimmed = trimToPixels(*out, grown, placed);
    // The selection follows the patch, as in Photoshop.
    const GrayImage& before = *document_->selection->coverage.u8();
    auto shifted = std::make_shared<GrayImage>(before.width(), before.height());
    for (int y = 0; y < shifted->height(); y++) {
        const int sy = y - dy;
        if (sy < 0 || sy >= before.height()) continue;
        for (int x = std::max(0, dx); x < std::min(shifted->width(), shifted->width() + dx); x++) shifted->at(x, y) = before.at(x - dx, sy);
    }
    const QString name = contentMoveExtend ? QStringLiteral(QT_TRANSLATE_NOOP("History", "Content-Aware Extend")) : QStringLiteral(QT_TRANSLATE_NOOP("History", "Content-Aware Move"));
    beginEdit(name);
    commitPixels(trimmed, placed, name);
    Selection moved = *document_->selection;
    moved.coverage = shifted;
    document_->selection = moved;
    endEdit();
    notifyDocument();
    emit selectionChanged();
    return true;
}

bool EditorSession::contentAwareScale(int width, int height, bool protectSelection, QString* errorText) {
    if (!canAdjustPixels()) { if (errorText) *errorText = tr("Select a visible image layer to scale."); return false; }
    Layer* layer = activeLayerMutable();
    const ImagePtr src = layer->asset->image.u8();
    if (!Document::validDimension(width) || !Document::validDimension(height) || (long long)width * height > Document::pixelBudget) {
        if (errorText) *errorText = tr("The size must be between 1 and %1 pixels a side, 100 megapixels at most.").arg(maxImageSide);
        return false;
    }
    std::shared_ptr<GrayImage> protect;
    if (protectSelection && document_->selection && document_->selection->coverage) protect = selectionOnGrid(layer->transform, src->width(), src->height());
    SeamCarveOptions options;
    options.protect = protect.get();
    auto out = std::make_shared<Image>(seamCarve(*src, width, height, options));
    if (out->isEmpty()) { if (errorText) *errorText = tr("Could not scale the layer."); return false; }
    LayerTransform placed = layer->transform;
    placed.size = {placed.size.width * width / src->width(), placed.size.height * height / src->height()};
    commitPixels(out, placed, QT_TRANSLATE_NOOP("History", "Content-Aware Scale"));
    return true;
}

bool EditorSession::copyLayerFrom(const EditorSession& source, const Uuid& id, std::optional<QPointF> at, QString* errorText) {
    if (!source.document_ || (document_ && !canEditLayers())) return false;
    const Document& from = *source.document_;
    if (!from.find(id)) return false;
    std::set<Uuid> included = descendantIds(from.layers, id);
    included.insert(id);
    std::vector<Layer> copied;
    for (auto& l : from.layers) if (included.count(l.id)) copied.push_back(l);
    const long long used = document_ ? document_->layerPixels() : 0;
    long long added = 0;
    for (auto& l : copied) if (l.asset && l.asset->image.u8()) added += (long long)l.asset->image.u8()->width() * l.asset->image.u8()->height();
    if (used + added > Document::projectPixelBudget) { if (errorText) *errorText = tr("The copied layers would take this project past its 1-gigapixel limit for all layers together."); return false; }
    // Clipping to a layer that stays behind is baked into the pixels.
    for (auto& l : copied) {
        if (l.maskSourceId && !included.count(*l.maskSourceId)) {
            if (!l.adjustment) { if (auto baked = source.bakeClipping(l.id)) l.asset = *baked; }
            l.maskSourceId.reset();
        }
    }
    std::map<Uuid, Uuid> mapping;
    for (auto& l : copied) mapping[l.id] = makeUuid();
    commitTransform();
    resolveGradient();
    beginEdit(QT_TRANSLATE_NOOP("History", "Copy Layer"));
    if (!document_) {
        document_ = Document(from.width, from.height);
        document_->resolution = from.resolution;
        viewport.fit({double(from.width), double(from.height)});
        emit viewportChanged();
        emit projectPathChanged();
    }
    const Layer* dragged = from.find(id);
    Point anchor = dragged->transform.center();
    Point center = at ? toPoint(*at) : Point(document_->width / 2.0, document_->height / 2.0);
    double dx = center.x - anchor.x, dy = center.y - anchor.y;
    const Layer* active = activeLayer();
    std::optional<Uuid> parent = active && active->isGroup ? activeLayerId_ : (active ? active->parentId : std::nullopt);
    int index = activeLayerId_ ? document_->indexOf(*activeLayerId_) + 1 : int(document_->layers.size());
    std::vector<Layer> placed;
    for (auto& l : copied) {
        Layer c = l;
        c.id = mapping[l.id];
        c.parentId = l.parentId && mapping.count(*l.parentId) ? std::optional(mapping[*l.parentId]) : parent;
        if (c.maskSourceId) c.maskSourceId = mapping.count(*c.maskSourceId) ? std::optional(mapping[*c.maskSourceId]) : std::nullopt;
        c.transform.origin.x += dx; c.transform.origin.y += dy;
        if (c.mask && c.mask->placement) { c.mask->placement->origin.x += dx; c.mask->placement->origin.y += dy; }
        placed.push_back(c);
    }
    document_->layers.insert(document_->layers.begin() + std::min(index, int(document_->layers.size())), placed.begin(), placed.end());
    setActiveLayer(mapping[id]);
    if (parent) collapsedGroupIds.erase(*parent);
    endEdit();
    notifyDocument();
    emit selectionChanged();
    return true;
}

} // namespace app
