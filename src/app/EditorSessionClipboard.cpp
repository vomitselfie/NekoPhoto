// Copying and pasting whole layers, as Photoshop's Edit > Copy with layers selected and no selection: the
// layers and folders go to an application-wide layer clipboard (with their masks, vector masks, styles, text,
// shapes, smart objects and their sources, adjustments, blending and clipping) and Paste in any open document
// inserts them above the active layer. Other apps get the layers flattened as an image.
#include "EditorSession.h"
#include "ImageConvert.h"
#include "compositor/colormgmt.h"
#include "compositor/depth.h"
#include "compositor/render.h"
#include <QApplication>
#include <QClipboard>
#include <QMimeData>
#include <QUuid>
#include <map>
#include <memory>
#include <set>

namespace app {

using namespace compositor;

namespace {

/// The copied layers as a small document of the source's size, depth and profile (its smart object sources
/// included), and the token the system clipboard carries while it still holds them.
struct LayerClipboard {
    Document layers;
    QByteArray token;
};

std::shared_ptr<const LayerClipboard>& layerClipboard() {
    static std::shared_ptr<const LayerClipboard> clipboard;
    return clipboard;
}

const char* layersMime = "application/x-nekophoto-layers";

/// The layer clipboard while the system clipboard still holds what it was copied with.
std::shared_ptr<const LayerClipboard> currentLayerClipboard() {
    const auto& clipboard = layerClipboard();
    if (!clipboard) return nullptr;
    const QMimeData* mime = QApplication::clipboard()->mimeData();
    if (!mime || mime->data(layersMime) != clipboard->token) return nullptr;
    return clipboard;
}

Rect layersBounds(const Document& doc) {
    Rect bounds;
    bool any = false;
    for (const Layer& l : doc.layers) {
        if (l.isGroup || l.adjustment) continue;
        const Rect b = l.transform.bounds();
        bounds = any ? bounds.unionWith(b) : b;
        any = true;
    }
    return any ? bounds : doc.rect();
}

} // namespace

bool EditorSession::copyLayers() {
    if (!document_ || selectedLayerIds_.empty()) return false;
    const Document& from = *document_;
    // The selected layers and everything in selected folders, in the document's order.
    std::set<Uuid> included;
    for (const Uuid& id : selectedLayerIds_) {
        if (!from.find(id)) continue;
        included.insert(id);
        for (const Uuid& d : descendantIds(from.layers, id)) included.insert(d);
    }
    if (included.empty()) return false;
    auto clipboard = std::make_shared<LayerClipboard>();
    Document& out = clipboard->layers;
    out = Document(from.width, from.height);
    out.resolution = from.resolution;
    out.sampleType = from.sampleType;
    out.profile = from.profile;
    for (const Layer& l : from.layers) {
        if (!included.count(l.id)) continue;
        Layer c = l;
        // A layer whose folder stays behind comes out at the top level; clipping to a layer left behind is baked
        // into the pixels, as dragging a layer to another tab does.
        if (c.parentId && !included.count(*c.parentId)) c.parentId.reset();
        if (c.maskSourceId && !included.count(*c.maskSourceId)) {
            if (!c.adjustment) { if (auto baked = bakeClipping(c.id)) c.asset = *baked; }
            c.maskSourceId.reset();
        }
        if (c.smartObject) {
            auto source = from.smartObjects.find(c.smartObject->sourceId);
            if (source != from.smartObjects.end()) out.smartObjects[source->first] = source->second;
        }
        out.layers.push_back(std::move(c));
    }
    clipboard->token = QUuid::createUuid().toByteArray();
    // Other apps get the layers flattened, at 8 bits.
    auto* mime = new QMimeData;
    QImage image;
    if (out.sampleType == SampleType::U16) { if (auto flat = renderFlattened16(out)) image = toQImage(*narrowImage(*flat)); }
    else if (auto flat = renderFlattened(out)) image = toQImage(*flat);
    if (!image.isNull()) {
        const Rect b = layersBounds(out).intersection(out.rect());
        if (b.width >= 1 && b.height >= 1) image = image.copy(QRectF(b.x, b.y, b.width, b.height).toAlignedRect());
        mime->setImageData(image.convertToFormat(QImage::Format_ARGB32));
    }
    mime->setData(layersMime, clipboard->token);
    layerClipboard() = clipboard;
    pixelClipboard_.reset();
    QApplication::clipboard()->setMimeData(mime);
    return true;
}

bool EditorSession::hasLayerClipboard() { return currentLayerClipboard() != nullptr; }

std::vector<Uuid> EditorSession::pasteLayers(QString* errorText) {
    const auto clipboard = currentLayerClipboard();
    if (!clipboard || !document_ || !canEditLayers()) return {};
    Document copied = clipboard->layers;
    // Another profile: the layers are converted to this document's, as Photoshop's paste does.
    if (!(copied.profile == document_->profile)) {
        std::string why;
        if (!convertDocumentProfile(copied, document_->profile, ConvertOptions{}, &why)) copied.profile = document_->profile;
    }
    long long added = 0, addedMasks = 0;
    for (const Layer& l : copied.layers) {
        if (l.asset && l.asset->image) added += (long long)l.asset->image.width() * l.asset->image.height();
        if (l.mask && l.mask->asset.image) addedMasks += (long long)l.mask->asset.image.width() * l.mask->asset.image.height();
    }
    if (const BudgetCheck check = document_->canAddLayers((long long)copied.layers.size(), added, addedMasks); !check) {
        if (errorText) *errorText = budgetText(check);
        return {};
    }
    // Same-size documents keep the layers where they were; otherwise they are centred on the canvas.
    double dx = 0, dy = 0;
    if (copied.width != document_->width || copied.height != document_->height) {
        const Rect b = layersBounds(copied);
        dx = std::round(document_->width / 2.0 - (b.x + b.width / 2));
        dy = std::round(document_->height / 2.0 - (b.y + b.height / 2));
    }
    std::map<Uuid, Uuid> mapping;
    for (const Layer& l : copied.layers) mapping[l.id] = makeUuid();
    commitTransform();
    resolveGradient();
    endOpacityEdit();
    beginEdit(QT_TRANSLATE_NOOP("History", "Paste Layers"));
    const Layer* active = activeLayer();
    const std::optional<Uuid> parent = active && active->isGroup ? activeLayerId_ : (active ? active->parentId : std::nullopt);
    const int index = activeLayerId_ ? document_->indexOf(*activeLayerId_) + 1 : int(document_->layers.size());
    std::vector<Layer> placed;
    std::vector<Uuid> ids, topLevel;
    for (const Layer& l : copied.layers) {
        Layer c = l;
        c.id = mapping[l.id];
        c.parentId = l.parentId && mapping.count(*l.parentId) ? std::optional(mapping[*l.parentId]) : parent;
        if (c.maskSourceId) c.maskSourceId = mapping.count(*c.maskSourceId) ? std::optional(mapping[*c.maskSourceId]) : std::nullopt;
        c.transform.origin.x += dx;
        c.transform.origin.y += dy;
        if (c.mask && c.mask->placement) { c.mask->placement->origin.x += dx; c.mask->placement->origin.y += dy; }
        ids.push_back(c.id);
        if (!l.parentId || !mapping.count(*l.parentId)) topLevel.push_back(c.id);
        placed.push_back(std::move(c));
    }
    for (const auto& [key, source] : copied.smartObjects) document_->smartObjects.emplace(key, source);
    document_->layers.insert(document_->layers.begin() + std::min(index, int(document_->layers.size())), placed.begin(), placed.end());
    if (parent) collapsedGroupIds.erase(*parent);
    // The pasted layers end up selected, the top one active.
    selectLayers(std::set<Uuid>(topLevel.begin(), topLevel.end()), topLevel.empty() ? std::nullopt : std::optional(topLevel.back()));
    endEdit();   // also brings the pixels to this document's depth
    notifyDocument();
    emit selectionChanged();
    return ids;
}

} // namespace app
