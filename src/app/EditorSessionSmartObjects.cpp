// Smart objects in the editor: the menu commands over the core's operations (smartobject_edit.h), each one undo
// step, and a contents tab sending its document back to the smart object it came from.
#include "EditorSession.h"
#include "ImageConvert.h"
#include "TextLayer.h"
#include "compositor/affinity.h"
#include "compositor/png.h"
#include "compositor/psd.h"
#include "compositor/render.h"
#include <QBuffer>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QImageWriter>

using namespace compositor;

namespace app {

namespace {

/// A file as smart object contents: its bytes, and its image (a PSD's merged image or our render of it; any
/// image Qt reads).
std::optional<SmartObjectContents> contentsFromFile(const QString& path, QString* error) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) { if (error) *error = QObject::tr("Couldn’t read %1.").arg(QFileInfo(path).fileName()); return std::nullopt; }
    const QByteArray data = file.readAll();
    SmartObjectContents c;
    c.bytes.assign(data.begin(), data.end());
    c.fileName = QFileInfo(path).fileName().toStdString();
    c.fileType = smartObjectFileType(c.fileName);
    if (data.startsWith("8BPS")) {
        std::string why;
        auto imported = importPsdBytes(c.bytes, &why);
        if (!imported) { if (error) *error = QString::fromStdString(why); return std::nullopt; }
        c.image = imported->realComposite && imported->composite ? imported->composite : renderFlattened(imported->document);
        c.resolution = imported->document.resolution;
        if (data.size() > 5 && data[5] == 2) c.fileType = "8BPB";
    } else if (data.size() >= 4 && data[0] == '\0' && uint8_t(data[1]) == 0xFF && data[2] == 'K' && data[3] == 'A') {
        // An Affinity document: placed as its flattened layers (the bytes stay as they are).
        std::string why;
        auto imported = importAffinityBytes(c.bytes, &why, affinityImportOptions());
        if (!imported) { if (error) *error = QString::fromStdString(why); return std::nullopt; }
        c.image = renderFlattened(imported->document);
        c.resolution = imported->document.resolution;
    } else {
        QImage image;
        if (!image.loadFromData(data)) { if (error) *error = QObject::tr("%1 is not an image NekoPhoto can read.").arg(QFileInfo(path).fileName()); return std::nullopt; }
        if ((long long)image.width() * image.height() > Document::pixelBudget) { if (error) *error = QObject::tr("That image is larger than a document can hold."); return std::nullopt; }
        c.image = fromQImage(image);
        if (image.dotsPerMeterX() > 0) c.resolution = image.dotsPerMeterX() * 0.0254;
    }
    if (c.fileType.empty()) c.fileType = "    ";
    return c;
}

} // namespace

bool EditorSession::smartObjectBlocksPixels(bool ask) {
    const Layer* layer = activeLayer();
    if (!layer || !layer->isLiveSmartObject() || isMaskSelected_) return false;
    if (ask) emit smartObjectPixelsRequested(layer->id);
    return true;
}

bool EditorSession::convertToSmartObject(QString* error) {
    if (!canEditLayers()) return false;
    std::vector<Uuid> ids(selectedLayerIds_.begin(), selectedLayerIds_.end());
    if (ids.empty() && activeLayerId_) ids.push_back(*activeLayerId_);
    Document next = *document_;
    std::string why;
    auto id = compositor::convertToSmartObject(next, ids, &why, psdExportOptions());
    if (!id) { if (error) *error = QString::fromStdString(why); return false; }
    endOpacityEdit();
    beginEdit("Convert to Smart Object");
    *document_ = std::move(next);
    endEdit();
    setActiveLayer(*id);
    notifyDocument();
    return true;
}

bool EditorSession::placeEmbedded(const QString& path, QString* error) {
    if (!canEditLayers()) return false;
    auto contents = contentsFromFile(path, error);
    if (!contents) return false;
    auto source = makeSmartObjectSource(std::move(*contents));
    if (!source) { if (error) *error = tr("That file has no image to place."); return false; }
    const Layer* active = activeLayer();
    size_t index = document_->layers.size();
    std::optional<Uuid> parent;
    if (active) { index = size_t(document_->indexOf(active->id)) + 1; parent = active->isGroup ? active->id : active->parentId; }
    endOpacityEdit();
    beginEdit("Place Embedded");
    const Uuid id = placeSmartObject(*document_, source, index, parent);
    endEdit();
    setActiveLayer(id);
    notifyDocument();
    return true;
}

bool EditorSession::replaceSmartObjectContents(const QString& path, QString* error) {
    const Layer* layer = activeLayer();
    if (!canEditLayers() || !layer || !layer->isLiveSmartObject()) { if (error) *error = tr("Select a smart object."); return false; }
    std::string why;
    if (!smartObjectContentsEditable(*document_, layer->smartObject->sourceId, &why)) { if (error) *error = QString::fromStdString(why); return false; }
    auto contents = contentsFromFile(path, error);
    if (!contents) return false;
    auto source = makeSmartObjectSource(std::move(*contents));
    if (!source) { if (error) *error = tr("That file has no image."); return false; }
    endOpacityEdit();
    beginEdit("Replace Contents");
    replaceSmartObjectSource(*document_, layer->smartObject->sourceId, source);
    endEdit();
    notifyDocument();
    return true;
}

bool EditorSession::warpActiveLayer(const compositor::TextWarp& warp, QString* error) {
    Layer* layer = activeLayerMutable();
    if (!canEditLayers() || !layer) { if (error) *error = tr("Select a layer to warp."); return false; }
    if (isMaskSelected_) { if (error) *error = tr("Warp the layer, not its mask."); return false; }
    endOpacityEdit();
    Layer before = *layer;
    beginEdit("Warp");
    std::string why;
    bool ok = compositor::warpLayer(*document_, *layer, warp, &why);
    if (ok && layer->text) ok = redrawText(*layer);
    if (!ok) {
        *layer = before;
        endEdit();
        if (error) *error = why.empty() ? tr("The layer could not be warped.") : QString::fromStdString(why);
        return false;
    }
    endEdit();
    notifyDocument();
    return true;
}

bool EditorSession::canAddSmartFilter() const {
    const Layer* layer = activeLayer();
    return canEditLayers() && layer && layer->isLiveSmartObject() && !layer->smartObject->locked() && !isMaskSelected_;
}

bool EditorSession::addSmartFilter(const compositor::SmartFilterEntry& entry, QString* error) {
    Layer* layer = activeLayerMutable();
    if (!canAddSmartFilter() || !layer) { if (error) *error = tr("Select an editable smart object."); return false; }
    endOpacityEdit();
    const Layer before = *layer;
    const auto carry = document_->psdCarry;
    beginEdit("Smart Filter");
    std::string why;
    if (!compositor::addSmartFilter(*document_, *layer, entry, &why)) {
        *layer = before;
        document_->psdCarry = carry;
        endEdit();
        if (error) *error = QString::fromStdString(why);
        return false;
    }
    endEdit();
    notifyDocument();
    return true;
}

bool EditorSession::rasterizeSmartObject() {
    Layer* layer = activeLayerMutable();
    if (!canEditLayers() || !layer || !layer->smartObject) return false;
    endOpacityEdit();
    beginEdit("Rasterize Smart Object");
    compositor::rasterizeSmartObject(*layer);
    endEdit();
    notifyDocument();
    return true;
}

std::optional<std::pair<Document, std::string>> EditorSession::smartObjectContentsForEditing(QString* error) const {
    const Layer* layer = activeLayer();
    if (!document_ || !layer || !layer->isLiveSmartObject()) { if (error) *error = tr("Select a smart object."); return std::nullopt; }
    const std::string sourceId = layer->smartObject->sourceId;
    std::string why;
    if (!smartObjectContentsEditable(*document_, sourceId, &why)) { if (error) *error = QString::fromStdString(why); return std::nullopt; }
    // A PSD's contents open as the file would (text editable, fonts found); other types as one layer.
    const SmartObjectSource& source = *document_->smartObjects.at(sourceId);
    if (source.bytes && source.bytes->size() >= 4 && std::equal(source.bytes->begin(), source.bytes->begin() + 4, "8BPS")) {
        std::string why;
        auto imported = importPsdBytes(*source.bytes, &why, psdImportOptions());
        if (!imported) { if (error) *error = QString::fromStdString(why); return std::nullopt; }
        finishPsdText(*imported);
        return std::make_pair(std::move(imported->document), sourceId);
    }
    auto contents = smartObjectContentsDocument(*document_, sourceId);
    if (!contents) { if (error) *error = tr("Its contents could not be opened."); return std::nullopt; }
    return std::make_pair(std::move(*contents), sourceId);
}

bool EditorSession::commitSmartObjectContents(const std::string& sourceId, const Document& contents, QString* error) {
    if (!document_) return false;
    auto it = document_->smartObjects.find(sourceId);
    if (it == document_->smartObjects.end()) { if (error) *error = tr("The smart object these contents came from is gone."); return false; }
    std::string why;
    if (!smartObjectContentsEditable(*document_, sourceId, &why)) { if (error) *error = QString::fromStdString(why); return false; }
    const SmartObjectSource& original = *it->second;
    SmartObjectContents c;
    c.bytes = encodeSmartObjectContents(contents, original, psdExportOptions());
    c.image = renderFlattened(contents);
    if (c.bytes.empty() && c.image) {
        // A type the core does not write (JPEG, TIFF, ...): Qt writes it in the same format.
        QByteArray format = QByteArray::fromStdString(original.fileType).trimmed().toLower();
        if (format == "jpeg") format = "jpg";
        QBuffer buffer;
        buffer.open(QIODevice::WriteOnly);
        QImageWriter writer(&buffer, format);
        if (!writer.write(toQImage(*c.image))) {
            // Nothing writes it: the contents go back as PNG.
            std::vector<uint8_t> png;
            if (encodePngImage(*c.image, png)) { c.bytes = std::move(png); c.fileType = "png "; c.fileName = QFileInfo(QString::fromStdString(original.fileName)).completeBaseName().toStdString() + ".png"; }
        } else c.bytes.assign(buffer.data().begin(), buffer.data().end());
    }
    if (c.bytes.empty() || !c.image) { if (error) *error = tr("The contents could not be written."); return false; }
    if (c.fileName.empty()) c.fileName = original.fileName;
    if (c.fileType.empty()) c.fileType = original.fileType;
    c.resolution = contents.resolution;
    auto source = makeSmartObjectSource(std::move(c));
    endOpacityEdit();
    beginEdit("Edit Smart Object Contents");
    replaceSmartObjectSource(*document_, sourceId, source);
    endEdit();
    notifyDocument();
    // The contents now belong to the new source.
    return true;
}

bool EditorSession::commitToSmartObjectParent(QString* error) {
    EditorSession* parent = smartObjectParent_.data();
    if (!parent || !document_) { if (error) *error = tr("The document these contents came from is closed."); return false; }
    commitTransform();
    // The parent gives the source a fresh id; follow it so a second save lands on the same smart object.
    std::set<std::string> before;
    for (auto& [id, s] : parent->document()->smartObjects) before.insert(id);
    if (!parent->commitSmartObjectContents(smartObjectSource_, *document_, error)) return false;
    for (auto& [id, s] : parent->document()->smartObjects) if (!before.count(id)) smartObjectSource_ = id;
    history_.markSaved();
    emit titleChanged();
    emit historyChanged();
    return true;
}

} // namespace app
