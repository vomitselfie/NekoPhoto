// EditorSession: Moving selected pixels, the clipboard and Content-Aware Fill.
#include "EditorSession.h"
#include "ImageConvert.h"
#include "QtGeometry.h"
#include "compositor/filters.h"
#include "compositor/inpaint.h"
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
    return document_->selection->coverage->at(x, y) > 127;
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
        const GrayImage& src = *pixelMove_->origin.coverage;
        auto moved = std::make_shared<GrayImage>(src.width(), src.height(), 0);
        for (int y = 0; y < src.height(); y++) { int sy = y - dy; if (sy < 0 || sy >= src.height()) continue; for (int x = 0; x < src.width(); x++) { int sx = x - dx; if (sx >= 0 && sx < src.width()) moved->at(x, y) = src.at(sx, sy); } }
        Selection s = pixelMove_->origin;
        s.coverage = moved;
        return s;
    }
    if (transformEdit_ && transformEdit_->floating && document_->selection->coverage) {
        const FloatingTransform& f = *transformEdit_->floating;
        const GrayImage& cov = *document_->selection->coverage;
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
    beginEdit(move->duplicate ? "Duplicate Pixels" : "Move Pixels");
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
        coverage = document_->selection->coverage.get();
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
            sampleMaskCoverage(*layer->mask->asset.image, layer->maskTransform(), region, 1, gray.at(0, 0), gray, false);
            for (int y = 0; y < out.height(); y++) for (int x = 0; x < out.width(); x++) { uint8_t* p = out.pixel(x, y); p[0] = p[1] = p[2] = gray.at(x, y); p[3] = 255; }
        } else if (layer->asset && layer->asset->image) {
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
        addPixelLayer(pixelClipboard_->image, pixelClipboard_->origin, "Paste", true);
        return;
    }
    if (external.isNull()) return;
    QPointF origin(std::floor((document_->width - external.width()) / 2.0), std::floor((document_->height - external.height()) / 2.0));
    addPixelLayer(fromQImage(external), origin, "Paste", true);
}

void EditorSession::layerViaCopy() {
    if (!canEditLayers()) return;
    const Layer* layer = activeLayer();
    if (!layer || layer->isGroup) return;
    if (!document_->selection) { duplicateActiveLayer(); return; }
    auto copied = renderSelectedPixels(false);
    if (!copied) return;
    addPixelLayer(copied->image, copied->origin, "Layer via Copy", false);
}

void EditorSession::addPixelLayer(std::shared_ptr<const Image> image, QPointF origin, const QString& editName, bool dropsSelection) {
    if (!document_ || !image || document_->layers.size() >= size_t(Document::maxLayers)) return;
    Layer layer(Asset::make(image, nextLayerName(document_->layers, "Layer")), toPoint(origin));
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

bool EditorSession::contentAwareFill(QString* errorText) {
    if (!canAdjustPixels() || !document_->selection || !document_->selection->coverage) { if (errorText) *errorText = tr("Select a visible image layer and an area to fill."); return false; }
    Layer* layer = activeLayerMutable();
    // The layer grows over any of the selection on the canvas past its edge.
    Rect area = document_->selection->bounds().intersection(document_->rect());
    const Image& src = *layer->asset->image;
    Affine toPixels = layer->transform.pixelToDocument(src.width(), src.height()).inverted();
    Rect wanted = toPixels.mapBounds(area).integral().unionWith(Rect(0, 0, src.width(), src.height()));
    int margin = int(std::ceil(std::max({0.0, -wanted.minX(), -wanted.minY(), wanted.maxX() - src.width(), wanted.maxY() - src.height()})));
    LayerTransform grown;
    auto source = adjustmentSource(margin, grown);
    if (!source) { if (errorText) *errorText = tr("The layer is too large to grow."); return false; }
    auto coverage = selectionOnGrid(grown, source->width(), source->height());
    if (!coverage) { if (errorText) *errorText = tr("Select an area to fill."); return false; }
    auto out = std::make_shared<Image>(*source);
    // What the layer's mask hides is not copied from: a fill at the edge of a cut-out takes the subject.
    std::shared_ptr<GrayImage> visible;
    if (layer->mask && layer->mask->enabled && !layer->mask->placement && layer->mask->asset.image && layer->mask->asset.image->width() == src.width() && layer->mask->asset.image->height() == src.height()) {
        visible = std::make_shared<GrayImage>(source->width(), source->height(), 255);
        const GrayImage& m = *layer->mask->asset.image;
        for (int y = 0; y < m.height(); y++) std::memcpy(visible->row(y + margin) + margin, m.row(y), size_t(m.width()));
    }
    if (!contentFill(*out, *coverage, {}, visible.get())) { if (errorText) *errorText = tr("Not enough unselected, opaque image pixels to synthesize a fill. Use a smaller selection with some surrounding image."); return false; }
    LayerTransform placed;
    auto trimmed = trimToPixels(*out, grown, placed);
    commitPixels(trimmed, placed, "Content-Aware Fill");
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
    long long used = 0, added = 0;
    if (document_) for (auto& l : document_->layers) if (l.asset && l.asset->image) used += (long long)l.asset->image->width() * l.asset->image->height();
    for (auto& l : copied) if (l.asset && l.asset->image) added += (long long)l.asset->image->width() * l.asset->image->height();
    if (used + added > Document::pixelBudget) { if (errorText) *errorText = tr("The copied layers exceed this project’s 100-megapixel limit."); return false; }
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
    beginEdit("Copy Layer");
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
