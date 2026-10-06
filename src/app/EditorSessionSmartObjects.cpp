// Smart objects in the editor: the menu commands over the core's operations (smartobject_edit.h), each one undo
// step, and a contents tab sending its document back to the smart object it came from.
#include "compositor/vectorlayer.h"
#include "ColorManagement.h"
#include "EditorSession.h"
#include "ImageConvert.h"
#include "TextLayer.h"
#include "compositor/affinity.h"
#include "compositor/depth.h"
#include "compositor/png.h"
#include "compositor/psd.h"
#include "compositor/render.h"
#include "compositor/smartobject_edit.h"
#include <QBuffer>
#include <QCoreApplication>
#include <QColorSpace>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QImageWriter>

using namespace compositor;

namespace app {

namespace {

/// A file as smart object contents: its bytes, and its image at the file's own depth (a PSD's merged image or our
/// render of it, 16-bit from a 16-bit PSD; a PNG, 16-bit when it is; any image Qt reads, 16-bit when Qt reads it at
/// 16 bits per channel, as a 16-bit TIFF).
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
        if (imported->document.sampleType == SampleType::U16)
            c.image = imported->realComposite && imported->composite16 ? imported->composite16 : Image16Ptr(renderFlattened16(imported->document));
        else c.image = imported->realComposite && imported->composite ? imported->composite : ImagePtr(renderFlattened(imported->document));
        // A CMYK or Lab file's own samples too (the image above is them in sRGB), which a document of its mode places.
        if (imported->document.colorMode != ColorMode::RGB) {
            c.native = imported->realComposite && imported->compositeNative ? imported->compositeNative : renderNative(imported->document);
            c.nativeMode = imported->document.colorMode;
            c.nativeProfile = imported->document.profile;
        } else c.profile = imported->document.profile;
        c.resolution = imported->document.resolution;
        if (data.size() > 5 && data[5] == 2) c.fileType = "8BPB";
    } else if (data.size() >= 4 && data[0] == '\0' && uint8_t(data[1]) == 0xFF && data[2] == 'K' && data[3] == 'A') {
        // An Affinity document: placed as its flattened layers (the bytes stay as they are).
        std::string why;
        auto imported = importAffinityBytes(c.bytes, &why, affinityImportOptions());
        if (!imported) { if (error) *error = QString::fromStdString(why); return std::nullopt; }
        finishPendingText(*imported);
        if (imported->document.sampleType == SampleType::U16) c.image = Image16Ptr(renderFlattened16(imported->document));
        else c.image = ImagePtr(renderFlattened(imported->document));
        c.resolution = imported->document.resolution;
    } else {
        QImage image;
        if (!image.loadFromData(data)) { if (error) *error = QObject::tr("%1 is not an image NekoPhoto can read.").arg(QFileInfo(path).fileName()); return std::nullopt; }
        // A 16-bit PNG (its IHDR's bit depth) goes through the core's reader, which keeps the 16 bits.
        const bool deepPng = data.startsWith("\x89PNG") && data.size() > 24 && uint8_t(data[24]) == 16;
        const bool deep = deepPng || image.depth() == 64;
        if (const BudgetCheck check = Document::canCreate(image.width(), image.height(), deep ? SampleType::U16 : SampleType::U8); !check) {
            if (error) *error = EditorSession::budgetText(check);
            return std::nullopt;
        }
        if (deepPng) c.image = decodeSmartObjectPng(c.bytes);
        if (!c.image) c.image = deep ? AnyImage(Image16Ptr(fromQImage16(image))) : AnyImage(ImagePtr(fromQImage(image)));
        // Its embedded profile: a CMYK or Lab document converts the contents from it.
        if (auto profile = color::embeddedProfile(image.colorSpace().iccProfile())) c.profile = std::move(*profile);
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
    if (refusedAtDepth("edit.smartObject", tr("Smart objects"), error)) return false;
    if (!canEditLayers()) return false;
    std::vector<Uuid> ids(selectedLayerIds_.begin(), selectedLayerIds_.end());
    if (ids.empty() && activeLayerId_) ids.push_back(*activeLayerId_);
    Document next = *document_;
    std::string why;
    auto id = compositor::convertToSmartObject(next, ids, &why, psdExportOptions());
    if (!id) { if (error) *error = QString::fromStdString(why); return false; }
    endOpacityEdit();
    beginEdit(QT_TRANSLATE_NOOP("History", "Convert to Smart Object"));
    *document_ = std::move(next);
    endEdit();
    setActiveLayer(*id);
    notifyDocument();
    return true;
}

bool EditorSession::newSmartObjectViaCopy(QString* error) {
    if (refusedAtDepth("edit.smartObject", tr("Smart objects"), error)) return false;
    const Layer* layer = activeLayer();
    if (!canEditLayers() || !layer || !layer->isLiveSmartObject()) { if (error) *error = tr("Select a smart object."); return false; }
    if (document_->layers.size() >= size_t(Document::maxLayers)) { if (error) *error = tr("The document has too many layers."); return false; }
    const long long pixels = (long long)layer->asset->image.width() * layer->asset->image.height();
    const long long maskPixels = layer->mask && layer->mask->asset.image ? (long long)layer->mask->asset.image.width() * layer->mask->asset.image.height() : 0;
    if (const BudgetCheck check = document_->canAddLayers(1, pixels, maskPixels); !check) { if (error) *error = budgetText(check); return false; }
    Document next = *document_;
    std::string why;
    const std::string name = QCoreApplication::translate("Names", "%1 copy").arg(QString::fromStdString(layer->name)).toStdString();
    auto id = compositor::newSmartObjectViaCopy(next, layer->id, name, &why);
    if (!id) { if (error) *error = QString::fromStdString(why); return false; }
    endOpacityEdit();
    beginEdit(QT_TRANSLATE_NOOP("History", "New Smart Object via Copy"));
    *document_ = std::move(next);
    endEdit();
    setActiveLayer(*id);
    notifyDocument();
    return true;
}

bool EditorSession::placeEmbedded(const QString& path, QString* error) {
    if (refusedAtDepth("edit.smartObject", tr("Smart objects"), error)) return false;
    if (!canEditLayers()) return false;
    auto contents = contentsFromFile(path, error);
    if (!contents) return false;
    auto source = makeSmartObjectSource(std::move(*contents));
    if (!source) { if (error) *error = tr("That file has no image to place."); return false; }
    const Layer* active = activeLayer();
    size_t index = document_->layers.size();
    std::optional<Uuid> parent;
    if (active) { index = size_t(document_->indexOf(active->id)) + 1; parent = active->isGroup ? active->id : active->parentId; }
    // The placed raster and the decoded contents, against the same budgets as any new layer.
    Document next = *document_;
    const Uuid id = placeSmartObject(next, source, index, parent);
    const Layer* placed = next.find(id);
    const long long pixels = placed && placed->asset && placed->asset->image ? (long long)placed->asset->image.width() * placed->asset->image.height() : 0;
    BudgetCheck check = Document::canCreate(std::max(1, source->width), std::max(1, source->height), document_->sampleType);
    if (check) check = document_->canAddLayers(1, pixels);
    if (!check) { if (error) *error = budgetText(check); return false; }
    endOpacityEdit();
    beginEdit(QT_TRANSLATE_NOOP("History", "Place Embedded"));
    *document_ = std::move(next);
    endEdit();
    setActiveLayer(id);
    notifyDocument();
    return true;
}

bool EditorSession::replaceSmartObjectContents(const QString& path, QString* error) {
    if (refusedAtDepth("edit.smartObject", tr("Smart objects"), error)) return false;
    const Layer* layer = activeLayer();
    if (!canEditLayers() || !layer || !layer->isLiveSmartObject()) { if (error) *error = tr("Select a smart object."); return false; }
    std::string why;
    if (!smartObjectContentsEditable(*document_, layer->smartObject->sourceId, &why)) { if (error) *error = QString::fromStdString(why); return false; }
    auto contents = contentsFromFile(path, error);
    if (!contents) return false;
    auto source = makeSmartObjectSource(std::move(*contents));
    if (!source) { if (error) *error = tr("That file has no image."); return false; }
    endOpacityEdit();
    beginEdit(QT_TRANSLATE_NOOP("History", "Replace Contents"));
    replaceSmartObjectSource(*document_, layer->smartObject->sourceId, source);
    endEdit();
    notifyDocument();
    return true;
}

std::shared_ptr<const SmartObjectSource> EditorSession::activeRawSmartObject(CameraRawSettings* settings) const {
    const Layer* layer = activeLayer();
    if (!document_ || !layer || !layer->isLiveSmartObject()) return nullptr;
    auto found = document_->smartObjects.find(layer->smartObject->sourceId);
    if (found == document_->smartObjects.end() || !found->second->isCameraRaw() || !found->second->bytes) return nullptr;
    if (settings && !CameraRawSettings::parse(found->second->rawSettings, *settings)) return nullptr;
    return found->second;
}

bool EditorSession::redevelopRawSmartObject(const CameraRawSettings& settings, const AnyImage& image, QString* error) {
    if (refusedAtDepth("edit.smartObject", tr("Smart objects"), error)) return false;
    auto source = activeRawSmartObject();
    if (!canEditLayers() || !source) { if (error) *error = tr("Select a smart object made from a camera RAW file."); return false; }
    std::string why;
    if (!smartObjectContentsEditable(*document_, source->id, &why)) { if (error) *error = QString::fromStdString(why); return false; }
    endOpacityEdit();
    beginEdit(QT_TRANSLATE_NOOP("History", "Camera Raw"));
    const int changed = compositor::redevelopRawSmartObject(*document_, source->id, settings, image);
    endEdit();
    notifyDocument();
    if (!changed) { if (error) *error = tr("The smart object could not be developed again."); return false; }
    return true;
}

bool EditorSession::warpActiveLayer(const compositor::TextWarp& warp, QString* error) {
    if (refusedAtDepth("edit.distort", tr("Warping a layer"), error)) return false;
    Layer* layer = activeLayerMutable();
    if (!canEditLayers() || !layer) { if (error) *error = tr("Select a layer to warp."); return false; }
    if (isMaskSelected_) { if (error) *error = tr("Warp the layer, not its mask."); return false; }
    if (layer->text && refusedAtDepth("edit.text", tr("Text"), error)) return false;
    endOpacityEdit();
    Layer before = *layer;
    beginEdit(QT_TRANSLATE_NOOP("History", "Warp"));
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

bool EditorSession::beginWarpCage(QString* error) {
    if (refusedAtDepth("edit.distort", tr("Warping a layer"), error)) return false;
    if (warpCage_) { if (error) *error = tr("A warp cage is open: Enter applies it, Esc cancels it."); return false; }
    const Layer* layer = activeLayer();
    if (!canEditLayers() || !layer) { if (error) *error = tr("Select a layer to warp."); return false; }
    if (isMaskSelected_) { if (error) *error = tr("Warp the layer, not its mask."); return false; }
    std::string why;
    auto cage = compositor::layerWarpCage(*document_, *layer, &why);
    if (!cage) { if (error) *error = QString::fromStdString(why); return false; }
    commitTransform();
    warpCage_ = cage;
    warpCageLayer_ = layer->id;
    if (compositor::isVectorShapeLayer(*layer)) {
        // A shape previews exactly: its bent path is applied as the cage moves, in one open undo step.
        endOpacityEdit();
        warpCageShapeBefore_ = *layer;
        beginEdit(QT_TRANSLATE_NOOP("History", "Warp"));
    }
    emit transformChanged();
    return true;
}

void EditorSession::previewWarpCage() {
    if (warpCageShapeBefore_ && warpCage_) {
        Layer* shape = document_ ? document_->find(warpCageLayer_) : nullptr;
        if (!shape) return;
        *shape = *warpCageShapeBefore_;
        std::string why;
        compositor::warpLayerToCage(*document_, *shape, *warpCage_, &why);
        emit documentChanged({});
        emit transformChanged();
        return;
    }
    const Layer* layer = document_ ? document_->find(warpCageLayer_) : nullptr;
    if (!warpCage_ || !layer) return;
    // A reduced draw while dragging; Apply draws at full size.
    auto preview = compositor::previewWarpCageAny(*document_, *layer, *warpCage_, 1024);
    if (preview) setPixelPreview(preview->image, preview->transform, warpCageLayer_);
    emit transformChanged();
}

void EditorSession::moveWarpCagePoint(int index, QPointF p) {
    if (!warpCage_ || index < 0 || index >= int(warpCage_->xs.size())) return;
    warpCage_->xs[size_t(index)] = p.x();
    warpCage_->ys[size_t(index)] = p.y();
    previewWarpCage();
}

void EditorSession::setWarpCage(const compositor::WarpMesh& cage) {
    if (refusedAtDepth("edit.distort", tr("Warping a layer"))) return;
    if (!warpCage_ || cage.xs.size() != warpCage_->xs.size()) return;
    warpCage_ = cage;
    previewWarpCage();
}

bool EditorSession::commitWarpCage(QString* error) {
    if (!warpCage_) return false;
    const compositor::WarpMesh cage = *warpCage_;
    warpCage_.reset();
    if (warpCageShapeBefore_) {
        // Already applied as it moved: the step closes.
        Layer* shape = document_ ? document_->find(warpCageLayer_) : nullptr;
        if (shape) { *shape = *warpCageShapeBefore_; std::string why; compositor::warpLayerToCage(*document_, *shape, cage, &why); }
        warpCageShapeBefore_.reset();
        endEdit();
        notifyDocument();
        emit transformChanged();
        return true;
    }
    clearPixelPreview();
    Layer* layer = document_ ? document_->find(warpCageLayer_) : nullptr;
    if (!layer || !canEditLayers()) { emit transformChanged(); return false; }
    endOpacityEdit();
    const Layer before = *layer;
    beginEdit(QT_TRANSLATE_NOOP("History", "Warp"));
    std::string why;
    if (!compositor::warpLayerToCage(*document_, *layer, cage, &why)) {
        *layer = before;
        endEdit();
        if (error) *error = QString::fromStdString(why);
        emit transformChanged();
        return false;
    }
    endEdit();
    notifyDocument();
    emit transformChanged();
    return true;
}

void EditorSession::cancelWarpCage() {
    if (!warpCage_) return;
    warpCage_.reset();
    if (warpCageShapeBefore_) {
        if (Layer* shape = document_ ? document_->find(warpCageLayer_) : nullptr) *shape = *warpCageShapeBefore_;
        warpCageShapeBefore_.reset();
        endEdit();   // nothing changed: no step
        notifyDocument();
    }
    clearPixelPreview();
    emit transformChanged();
}

bool EditorSession::canAddSmartFilter() const {
    const Layer* layer = activeLayer();
    return canEditLayers() && layer && layer->isLiveSmartObject() && !layer->smartObject->locked() && !isMaskSelected_;
}

bool EditorSession::addSmartFilter(const compositor::SmartFilterEntry& entry, QString* error) {
    if (refusedAtDepth("edit.smartObject", tr("Smart objects"), error)) return false;
    Layer* layer = activeLayerMutable();
    if (!canAddSmartFilter() || !layer) { if (error) *error = tr("Select an editable smart object."); return false; }
    endOpacityEdit();
    const Layer before = *layer;
    const auto carry = document_->psdCarry;
    beginEdit(QT_TRANSLATE_NOOP("History", "Smart Filter"));
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
    if (refusedAtDepth("edit.smartObject", tr("Smart objects"))) return false;
    Layer* layer = activeLayerMutable();
    if (!canEditLayers() || !layer || !layer->smartObject) return false;
    endOpacityEdit();
    beginEdit(QT_TRANSLATE_NOOP("History", "Rasterize Smart Object"));
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
    if (refusedAtDepth("edit.smartObject", tr("Smart objects"), error)) return false;
    if (!document_) return false;
    auto it = document_->smartObjects.find(sourceId);
    if (it == document_->smartObjects.end()) { if (error) *error = tr("The smart object these contents came from is gone."); return false; }
    std::string why;
    if (!smartObjectContentsEditable(*document_, sourceId, &why)) { if (error) *error = QString::fromStdString(why); return false; }
    const SmartObjectSource& original = *it->second;
    SmartObjectContents c;
    c.bytes = encodeSmartObjectContents(contents, original, psdExportOptions());
    // The contents at the depth they were edited at (a 16-bit child document makes a 16-bit source).
    std::shared_ptr<Image16> deep = contents.sampleType == SampleType::U16 ? renderFlattened16(contents) : nullptr;
    std::shared_ptr<Image> flat = deep ? nullptr : renderFlattened(contents);
    if (deep) c.image = Image16Ptr(deep);
    else if (flat) c.image = ImagePtr(flat);
    // CMYK or Lab contents keep their own samples (the image above is them in sRGB).
    if (contents.colorMode != ColorMode::RGB) {
        c.native = renderNative(contents);
        c.nativeMode = contents.colorMode;
        c.nativeProfile = contents.profile;
    } else c.profile = contents.profile;
    if (c.bytes.empty() && c.image) {
        // A type the core does not write (JPEG, TIFF, ...): Qt writes it in the same format (a 16-bit TIFF at 16 bits
        // when Qt's plugin keeps them).
        QByteArray format = QByteArray::fromStdString(original.fileType).trimmed().toLower();
        if (format == "jpeg") format = "jpg";
        if (format == "tiff" || format == "tif") format = "tiff";
        QBuffer buffer;
        buffer.open(QIODevice::WriteOnly);
        QImageWriter writer(&buffer, format);
        const QImage written = deep ? (format == "tiff" && canWriteDeepTiff() ? toQImage16(*deep) : toQImage(*ditherToEightBit(*deep))) : toQImage(*flat);
        if (!writer.write(written)) {
            // Nothing writes it: the contents go back as PNG.
            std::vector<uint8_t> png;
            if (deep ? encodePngImage16(*deep, png) : encodePngImage(*flat, png)) {
                c.bytes = std::move(png); c.fileType = "png ";
                c.fileName = QFileInfo(QString::fromStdString(original.fileName)).completeBaseName().toStdString() + ".png";
            }
        } else c.bytes.assign(buffer.data().begin(), buffer.data().end());
    }
    if (c.bytes.empty() || !c.image) { if (error) *error = tr("The contents could not be written."); return false; }
    if (c.fileName.empty()) c.fileName = original.fileName;
    if (c.fileType.empty()) c.fileType = original.fileType;
    c.resolution = contents.resolution;
    auto source = makeSmartObjectSource(std::move(c));
    endOpacityEdit();
    beginEdit(QT_TRANSLATE_NOOP("History", "Edit Smart Object Contents"));
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
