// Typing on the canvas, as Photoshop's Type tool: one editing session per layer, one undo step for all of it.
// The canvas (CanvasWidgetText.cpp) keeps the caret and the selection; the session holds the undo step open,
// sets the text exactly as typed and keeps the text anchored while it grows.
#include "EditorSession.h"
#include "TextLayer.h"
#include <QCoreApplication>
#include <QFontMetricsF>

namespace app {

using namespace compositor;

namespace {

/// The raster point that stays put while the text changes: the first baseline at the alignment's side for point
/// text, the box's top-left for paragraph text.
QPointF typeAnchor(const LayerText& text, const TextCaretGeometry& geometry, int rasterWidth) {
    const bool boxed = text.boxWidth > 0 && text.boxHeight > 0;
    if (boxed) return QPointF(textPadding, textPadding);
    const double baseline = geometry.lines.empty() ? textPadding : geometry.lines.front().baseline;
    const double inner = rasterWidth - 2.0 * textPadding;
    const double x = textPadding + (text.alignment == 1 ? inner / 2 : text.alignment == 2 ? inner : 0);
    return QPointF(x, baseline);
}

} // namespace

bool EditorSession::beginTypeEdit(const Uuid& id) {
    if (refusedAtDepth("edit.text", tr("Text"))) return false;
    if (typeEdit_ || !canEditLayers()) return false;
    const Layer* layer = document_->find(id);
    if (!layer || !layer->isLiveText() || layer->text->warp.active()) return false;
    typeEdit_ = TypeEdit{id, false, *document_, activeLayerId_};
    beginEdit(QT_TRANSLATE_NOOP("History", "Edit Type Layer"));
    return true;
}

std::optional<Uuid> EditorSession::beginNewTypeEdit(QPointF point, const LayerText& style, std::optional<QSizeF> box) {
    if (refusedAtDepth("edit.text", tr("Text"))) return std::nullopt;
    if (typeEdit_ || !canEditLayers()) return std::nullopt;
    LayerText text = style;
    text.text.clear();
    text.runs.clear();
    if (box) { text.boxWidth = std::max(1.0, box->width()); text.boxHeight = std::max(1.0, box->height()); }
    const AnyImage image = renderTextLayerAt(text, sampleType());
    if (!image) return std::nullopt;
    // Point text: the click is the first baseline's start (Photoshop's); paragraph text: the box's corner.
    QPointF topLeft = point - QPointF(textPadding, textPadding);
    if (!box) {
        const TextCaretGeometry geometry = textCaretGeometry(text);
        const QPointF anchor = typeAnchor(text, geometry, image.width());
        topLeft = point - anchor;
    }
    Layer layer(Asset::makeAny(image, nextLayerName(document_->layers, QCoreApplication::translate("Names", "Text").toStdString())),
                Point(std::round(topLeft.x()), std::round(topLeft.y())));
    layer.name = layer.asset->name;
    layer.text = text;
    layer.textImage = image;
    typeEdit_ = TypeEdit{layer.id, true, *document_, activeLayerId_};
    endOpacityEdit();
    beginEdit(QT_TRANSLATE_NOOP("History", "Type Tool"));
    const Layer* active = activeLayer();
    layer.parentId = active && active->isGroup ? activeLayerId_ : (active ? active->parentId : std::nullopt);
    const int index = activeLayerId_ ? document_->indexOf(*activeLayerId_) + 1 : int(document_->layers.size());
    document_->layers.insert(document_->layers.begin() + index, layer);
    setActiveLayer(layer.id);
    emit documentChanged({});
    emit layersChanged();
    return layer.id;
}

bool EditorSession::setTypeEditText(const LayerText& text, QPointF rasterShift) {
    if (!typeEdit_ || !document_) return false;
    Layer* layer = document_->find(typeEdit_->layer);
    if (!layer || !layer->text) return false;
    // Where the anchor sits on the document now (moved by `rasterShift` when a box edge was dragged).
    const int oldWidth = layer->asset && layer->asset->image ? layer->asset->image.width() : 1;
    const int oldHeight = layer->asset && layer->asset->image ? layer->asset->image.height() : 1;
    const Affine oldMap = layer->transform.pixelToDocument(oldWidth, oldHeight);
    const QPointF oldAnchor = typeAnchor(*layer->text, textCaretGeometry(*layer->text), oldWidth) + rasterShift;
    const Point target = oldMap.apply(Point(oldAnchor.x(), oldAnchor.y()));
    layer->text = text;
    if (!redrawText(*layer)) return false;
    const int width = layer->asset->image.width(), height = layer->asset->image.height();
    const QPointF anchor = typeAnchor(text, textCaretGeometry(text), width);
    const Point now = layer->transform.pixelToDocument(width, height).apply(Point(anchor.x(), anchor.y()));
    layer->transform.origin.x += target.x - now.x;
    layer->transform.origin.y += target.y - now.y;
    emit documentChanged({});
    emit layersChanged();
    return true;
}

void EditorSession::endTypeEdit(bool keep) {
    if (!typeEdit_) return;
    TypeEdit edit = std::move(*typeEdit_);
    typeEdit_.reset();
    if (document_) {
        // Cancelled, or new type left empty (which goes, as in Photoshop): the document as it was, so the step
        // records nothing.
        const Layer* layer = document_->find(edit.layer);
        if (!keep || (edit.created && (!layer || !layer->text || layer->text->text.empty()))) {
            *document_ = edit.before;
            setActiveLayer(edit.previousActive);
        }
    }
    endEdit();
    notifyDocument();
}

} // namespace app
