// EditorSession: The session: construction, the document, history, crop and canvas, tools and the view.
#include "EditorSession.h"
#include "QtGeometry.h"
#include "compositor/project.h"
#include <QFileInfo>
#include <algorithm>
#include <set>

using namespace compositor;

namespace app {

EditorSession::~EditorSession() {
    if (quickSelectThread_.joinable()) quickSelectThread_.join();
}

EditorSession::EditorSession(QObject* parent) : QObject(parent) {
    // Spot healing previews its result once the pointer has paused; the stroke goes on from there.
    healPreview_.setSingleShot(true);
    healPreview_.setInterval(180);
    connect(&healPreview_, &QTimer::timeout, this, [this] {
        if (!stroke_ || strokeMask_) return;
        stroke_->previewHeal();
        emit documentChanged({});
    });
    // Presets with slow position tracking trail the pointer and only move on input; a mouse held still sends
    // none, so the brush would stop short of it. The last input repeats at 60 a second until it has caught up.
    myPaintSettle_.setInterval(16);
    connect(&myPaintSettle_, &QTimer::timeout, this, [this] {
        if (!myPaint_ || !stroke_ || !lastBrushPoint_ || myPaint_->settled()) { myPaintSettle_.stop(); return; }
        myPaintTo(*lastBrushPoint_);
        Rect dirty = stroke_->takeDirtyRect();
        if (!dirty.isEmpty()) {
            strokeRegion_ = strokeRegion_.isEmpty() ? toQRect(dirty) : strokeRegion_.united(toQRect(dirty));
            emit documentChanged(toQRect(dirty));
        }
    });
}

QString EditorSession::title() const {
    if (!document_) return QStringLiteral("NekoPhoto");
    QString name = !projectPath_.isEmpty() ? QFileInfo(projectPath_).completeBaseName() : !importedName_.isEmpty() ? importedName_ : QStringLiteral("Untitled");
    return name + (isModified() ? QStringLiteral(" *") : QString());
}

void EditorSession::notifyDocument(QRectF region) {
    documentRevision_++;
    emit documentChanged(region);
    emit layersChanged();
    emit historyChanged();
    emit titleChanged();
}

const Layer* EditorSession::activeLayer() const {
    if (!document_ || !activeLayerId_) return nullptr;
    return document_->find(*activeLayerId_);
}

Layer* EditorSession::activeLayerMutable() {
    if (!document_ || !activeLayerId_) return nullptr;
    return document_->find(*activeLayerId_);
}

void EditorSession::setActiveLayer(const std::optional<Uuid>& id) {
    if (id != activeLayerId_) isMaskSelected_ = false;
    activeLayerId_ = id;
    selectedLayerIds_.clear();
    if (id) selectedLayerIds_.insert(*id);
}

bool EditorSession::canEditLayers() const {
    return document_ && !stroke_ && !warp_ && !transformEdit_ && !pixelMove_;
}

// ---- Document ----------------------------------------------------------------

void EditorSession::createDocument(int width, int height, double resolution, bool emptyLayer) {
    if (!Document::validDimension(width) || !Document::validDimension(height)) return;
    commitTransform();
    beginEdit("New Canvas");
    Document doc(width, height);
    doc.resolution = resolution;
    std::optional<Uuid> active;
    if (emptyLayer) { doc.layers.emplace_back("Layer 1", doc.size()); active = doc.layers.back().id; }
    document_ = doc;
    setActiveLayer(active);
    projectPath_.clear();
    history_.reset();
    endEdit();
    history_.reset();
    viewport.fit({double(width), double(height)});
    emit viewportChanged();
    emit projectPathChanged();
    notifyDocument();
    emit selectionChanged();
}

std::optional<EditorSession::LoadedProject> EditorSession::readProject(const QString& path, QString* error) {
    ProjectError err;
    auto doc = loadProject(path.toStdString(), err);
    if (!doc) { if (error) *error = QString::fromStdString(err.message); return std::nullopt; }
    std::optional<Uuid> active = loadedActiveLayer(path.toStdString());
    if (active && !doc->find(*active)) active.reset();
    return LoadedProject{std::move(*doc), active, path};
}

bool EditorSession::openProject(const QString& path, QString* error) {
    auto project = readProject(path, error);
    if (!project) return false;
    installProject(std::move(*project));
    return true;
}

void EditorSession::installProject(LoadedProject project) {
    commitTransform();
    document_ = std::move(project.document);
    setActiveLayer(project.activeLayer);
    projectPath_ = project.path;
    history_.reset();
    viewport.fit({double(document_->width), double(document_->height)});
    emit viewportChanged();
    emit projectPathChanged();
    notifyDocument();
    emit selectionChanged();
}

void EditorSession::adoptDocument(const Document& document, const QString& name) {
    commitTransform();
    document_ = document;
    // The topmost visible pixel layer starts active (a hidden top layer, common in exports, would confuse).
    std::optional<Uuid> active;
    std::set<Uuid> visible = effectiveVisibleIds(document_->layers);
    for (auto it = document_->layers.rbegin(); it != document_->layers.rend(); ++it) if (!it->isGroup && visible.count(it->id)) { active = it->id; break; }
    if (!active) for (auto it = document_->layers.rbegin(); it != document_->layers.rend(); ++it) if (!it->isGroup) { active = it->id; break; }
    setActiveLayer(active);
    projectPath_.clear();
    importedName_ = name;
    history_.reset();
    viewport.fit({double(document_->width), double(document_->height)});
    emit viewportChanged();
    emit projectPathChanged();
    emit titleChanged();
    notifyDocument();
    emit selectionChanged();
}

bool EditorSession::saveProject(const QString& path, QString* error) {
    if (!document_) return false;
    commitTransform();
    ProjectError err;
    if (!compositor::saveProject(*document_, activeLayerId_, path.toStdString(), err)) {
        if (error) *error = QString::fromStdString(err.message);
        return false;
    }
    projectPath_ = path;
    history_.markSaved();
    emit projectPathChanged();
    emit titleChanged();
    emit historyChanged();
    return true;
}

void EditorSession::closeDocument() {
    cancelBrush();
    cancelTransform();
    document_.reset();
    setActiveLayer(std::nullopt);
    projectPath_.clear();
    history_.reset();
    emit projectPathChanged();
    notifyDocument();
    emit selectionChanged();
}

void EditorSession::insertImage(std::shared_ptr<const Image> image, const QString& name, std::optional<QPointF> at) {
    if (!image || image->isEmpty()) return;
    cancelBrush();
    commitTransform();
    beginEdit("Import Image");
    if (!document_) {
        document_ = Document(image->width(), image->height());
        viewport.fit({double(image->width()), double(image->height())});
        emit viewportChanged();
        at.reset();
    }
    Point center = at ? toPoint(*at) : Point(document_->width / 2.0, document_->height / 2.0);
    Layer layer(Asset::make(image, name.toStdString()), Point(std::floor(center.x - image->width() / 2.0), std::floor(center.y - image->height() / 2.0)));
    const Layer* active = activeLayer();
    layer.parentId = active && active->isGroup ? activeLayerId_ : (active ? active->parentId : std::nullopt);
    if (layer.parentId) collapsedGroupIds.erase(*layer.parentId);
    // Above the active layer, as on the Mac.
    int index = activeLayerId_ ? document_->indexOf(*activeLayerId_) + 1 : int(document_->layers.size());
    if (active && active->isGroup) {
        auto inside = descendantIds(document_->layers, active->id);
        for (size_t i = 0; i < document_->layers.size(); i++) if (inside.count(document_->layers[i].id)) index = std::max(index, int(i) + 1);
    }
    document_->layers.insert(document_->layers.begin() + std::min(index, int(document_->layers.size())), layer);
    setActiveLayer(layer.id);
    endEdit();
    notifyDocument();
}

std::shared_ptr<Image> EditorSession::flattened() const {
    if (!document_) return nullptr;
    return renderFlattened(*document_);
}

// ---- History -----------------------------------------------------------------

bool EditorSession::canUndo() const { return document_ && !stroke_ && !warp_ && !pixelMove_ && !transformEdit_ && (history_.canUndo() || gradient_); }
bool EditorSession::canRedo() const { return document_ && !stroke_ && !warp_ && !pixelMove_ && !transformEdit_ && !gradient_ && history_.canRedo(); }

void EditorSession::undo() {
    // Like Photoshop, the first Undo discards a pending gradient.
    if (gradient_) { cancelGradient(); return; }
    if (!canUndo()) return;
    auto snapshot = history_.undo();
    if (snapshot) restore(*snapshot);
}

void EditorSession::redo() {
    if (!canRedo()) return;
    auto snapshot = history_.redo();
    if (snapshot) restore(*snapshot);
}

void EditorSession::restore(const DocumentHistory::Snapshot& snapshot) {
    bool changedCanvas = !document_ || !snapshot.document || document_->id != snapshot.document->id || document_->width != snapshot.document->width || document_->height != snapshot.document->height;
    bool keepMask = isMaskSelected_ && activeLayerId_ == snapshot.activeLayerId;
    document_ = snapshot.document;
    setActiveLayer(snapshot.activeLayerId);
    const Layer* active = activeLayer();
    isMaskSelected_ = keepMask && active && active->mask;
    if (changedCanvas && document_) { viewport.fit({double(document_->width), double(document_->height)}); emit viewportChanged(); }
    notifyDocument();
    emit selectionChanged();
}

void EditorSession::beginEdit(const QString& name) { history_.begin(name.toStdString(), document_, activeLayerId_); }
void EditorSession::endEdit() { history_.end(document_, activeLayerId_); }


// ---- Crop and canvas --------------------------------------------------------------

void EditorSession::cropTo(const QRectF& rectF) {
    if (!canEditLayers()) return;
    Rect rect = Rect(rectF.x(), rectF.y(), rectF.width(), rectF.height()).integral().intersection(document_->rect());
    if (rect.isEmpty() || rect == document_->rect()) return;
    beginEdit("Crop");
    Document doc = *document_;
    doc.width = int(rect.width);
    doc.height = int(rect.height);
    for (auto& l : doc.layers) {
        l.transform.origin.x -= rect.x;
        l.transform.origin.y -= rect.y;
        if (l.mask && l.mask->placement) { l.mask->placement->origin.x -= rect.x; l.mask->placement->origin.y -= rect.y; }
    }
    if (doc.selection && doc.selection->coverage) doc.selection->coverage = cropGray(*doc.selection->coverage, int(rect.x), int(rect.y), doc.width, doc.height);
    document_ = doc;
    endEdit();
    viewport.fit({double(doc.width), double(doc.height)});
    emit viewportChanged();
    notifyDocument();
    emit selectionChanged();
}

void EditorSession::resizeCanvas(int width, int height, double anchorX, double anchorY) {
    if (!canEditLayers() || !Document::validDimension(width) || !Document::validDimension(height)) return;
    if (width == document_->width && height == document_->height) return;
    double dx = std::round((width - document_->width) * anchorX), dy = std::round((height - document_->height) * anchorY);
    beginEdit("Canvas Size");
    Document doc = *document_;
    doc.width = width;
    doc.height = height;
    for (auto& l : doc.layers) {
        l.transform.origin.x += dx;
        l.transform.origin.y += dy;
        if (l.mask && l.mask->placement) { l.mask->placement->origin.x += dx; l.mask->placement->origin.y += dy; }
    }
    doc.selection.reset();
    document_ = doc;
    endEdit();
    viewport.fit({double(width), double(height)});
    emit viewportChanged();
    notifyDocument();
    emit selectionChanged();
}

void EditorSession::resizeImage(int width, int height, double resolution, int sampling) {
    if (!canEditLayers() || !Document::validDimension(width) || !Document::validDimension(height)) return;
    Document doc = *document_;
    Sampling mode = sampling == 0 ? Sampling::Nearest : sampling == 1 ? Sampling::Smooth : Sampling::High;
    if (!resizeDocument(doc, width, height, resolution, mode)) { emit error(tr("The resized image would exceed the 100-megapixel limit.")); return; }
    beginEdit("Image Size");
    document_ = doc;
    endEdit();
    viewport.fit({double(width), double(height)});
    emit viewportChanged();
    notifyDocument();
    emit selectionChanged();
}

// ---- Tools and view -----------------------------------------------------------------

void EditorSession::selectTool(Tool tool) {
    if (stroke_ || warp_ || pixelMove_) return;
    if (tool != tool_) { commitTransform(); resolveGradient(); cancelShape(); }
    tool_ = tool;
    emit toolChanged();
}

void EditorSession::fitView() {
    if (!document_) return;
    viewport.fit({double(document_->width), double(document_->height)});
    emit viewportChanged();
}

void EditorSession::zoomTo(double zoom, std::optional<QPointF> anchor) {
    if (!document_) return;
    viewport.setZoom(zoom, anchor.value_or(viewport.center()), {double(document_->width), double(document_->height)});
    emit viewportChanged();
}

Overrides EditorSession::renderOverrides() const {
    Overrides overrides;
    if (transformEdit_ && document_) {
        const TransformEdit& edit = *transformEdit_;
        std::vector<const Layer*> targets;
        if (edit.group) { for (auto& [id, t] : edit.group->originals) if (const Layer* l = document_->find(id)) targets.push_back(l); }
        else if (const Layer* l = document_->find(edit.layerId)) targets.push_back(l);
        for (const Layer* layer : targets) {
            LayerOverride& o = overrides[layer->id];
            if (edit.mask) { o.maskPlacement = std::optional<LayerTransform>(edit.draft); continue; }
            LayerTransform shown = displayedTransform(*layer);
            o.transform = shown;
            if (layer->mask) o.maskPlacement = displayedMaskPlacement(*layer);
            if (edit.corners && layer->asset && layer->asset->image) {
                // The layer warped into the pending distortion, at preview size, cached while nothing changes.
                auto target = distortTarget(*layer, edit);
                if (!target) continue;
                GrayPtr maskImage = layer->mask && layer->mask->enabled ? layer->mask->asset.image : nullptr;
                auto it = distortCache_.find(layer->id);
                bool fresh = it != distortCache_.end() && it->second.corners == target->second && it->second.transform == target->first && it->second.source == layer->asset->image && it->second.mask == maskImage;
                if (!fresh) {
                    DistortCache cache{target->second, target->first, layer->asset->image, maskImage, warpImage(layer->asset->image, target->first, target->second, 2048), nullptr};
                    if (cache.image && maskImage && !layer->mask->placement && layer->mask->linked) {
                        auto wm = warpMask(maskImage, target->first, target->second, 0, 2048);
                        if (wm) cache.warpedMask = wm->image;
                    }
                    it = distortCache_.insert_or_assign(layer->id, std::move(cache)).first;
                }
                const DistortCache& cache = it->second;
                if (cache.image) {
                    o.image = cache.image->image;
                    o.transform = cache.image->transform;
                    if (cache.warpedMask) { o.maskImage = cache.warpedMask; o.maskPlacement = std::optional<LayerTransform>(); }
                    else if (layer->mask) o.maskPlacement = std::optional<LayerTransform>(layer->mask->placement ? *layer->mask->placement : layer->transform);
                }
            }
        }
    }
    if (blendPreview_ && activeLayerId_) overrides[*activeLayerId_].blendMode = *blendPreview_;
    if (previewImage_ && previewLayerId_ && document_ && document_->find(*previewLayerId_)) {
        LayerOverride& o = overrides[*previewLayerId_];
        o.image = previewImage_;
        if (previewTransform_) o.transform = *previewTransform_;
        const Layer* layer = document_->find(*previewLayerId_);
        if (layer && layer->mask && !layer->mask->placement && previewTransform_ && !previewTransform_->samePlacement(layer->transform)) o.maskPlacement = std::optional<LayerTransform>(layer->transform);
    }
    auto strokeOverride = [&](const BrushStroke& stroke, const Uuid& layerId, bool mask) {
        LayerOverride& o = overrides[layerId];
        const Layer* layer = document_ ? document_->find(layerId) : nullptr;
        if (mask) {
            o.maskImage = stroke.previewMask();
            if (layer && layer->mask && layer->mask->placement) o.maskPlacement = std::optional<LayerTransform>(stroke.paintTransform());
        } else {
            o.image = stroke.previewImage();
            o.transform = stroke.paintTransform();
            // A mask covering the old grid stays where it was while the layer grows under the edit.
            if (layer && layer->mask && !layer->mask->placement && layer->asset) o.maskPlacement = std::optional<LayerTransform>(layer->transform);
        }
    };
    if (stroke_) strokeOverride(*stroke_, strokeLayerId_, strokeMask_);
    if (gradient_) strokeOverride(*gradient_->raster, gradient_->layerId, gradient_->mask);
    if (pixelMove_) strokeOverride(*pixelMove_->raster, pixelMove_->layerId, false);
    if (warp_ && document_) {
        LayerOverride& o = overrides[warpLayerId_];
        o.image = warp_->image();
        o.transform = LayerTransform(Point(0, 0), document_->size());
        const Layer* layer = document_->find(warpLayerId_);
        if (layer && layer->mask && !layer->mask->placement) o.maskPlacement = std::optional<LayerTransform>(layer->transform);
    }
    return overrides;
}

} // namespace app
