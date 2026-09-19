#include "EditorSession.h"
#include "ImageConvert.h"
#include "compositor/blend.h"
#include "compositor/project.h"
#include <QFileInfo>
#include <random>
#include <algorithm>
#include <map>

extern "C" {
#include "ContentFill.h"
#include "WandPixels.h"
}
#include <QApplication>
#include <QClipboard>
#include <cstring>
#include <QMimeData>

using namespace compositor;

namespace app {

namespace {

QRectF toQRect(const Rect& r) { return {r.x, r.y, r.width, r.height}; }
Point toPoint(QPointF p) { return {p.x(), p.y()}; }

} // namespace

EditorSession::EditorSession(QObject* parent) : QObject(parent) {}

QString EditorSession::title() const {
    if (!document_) return QStringLiteral("Compositor");
    QString name = projectPath_.isEmpty() ? QStringLiteral("Untitled") : QFileInfo(projectPath_).completeBaseName();
    return name + (isModified() ? QStringLiteral(" *") : QString());
}

void EditorSession::notifyDocument(QRectF region) {
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

bool EditorSession::openProject(const QString& path, QString* error) {
    ProjectError err;
    auto doc = loadProject(path.toStdString(), err);
    if (!doc) { if (error) *error = QString::fromStdString(err.message); return false; }
    commitTransform();
    document_ = *doc;
    std::optional<Uuid> active = loadedActiveLayer(path.toStdString());
    if (active && !document_->find(*active)) active.reset();
    setActiveLayer(active);
    projectPath_ = path;
    history_.reset();
    viewport.fit({double(document_->width), double(document_->height)});
    emit viewportChanged();
    emit projectPathChanged();
    notifyDocument();
    emit selectionChanged();
    return true;
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

// ---- Layers ------------------------------------------------------------------

void EditorSession::selectLayer(const std::optional<Uuid>& id, bool mask) {
    if (stroke_ || warp_ || pixelMove_) return;
    if (id != activeLayerId_ || (mask != isMaskSelected_)) { commitTransform(); resolveGradient(); }
    setActiveLayer(id);
    const Layer* active = activeLayer();
    isMaskSelected_ = mask && active && active->mask;
    emit layersChanged();
    emit transformChanged();
}

void EditorSession::selectLayers(const std::set<Uuid>& ids, const std::optional<Uuid>& primary) {
    if (stroke_ || warp_ || pixelMove_ || !document_) return;
    std::set<Uuid> valid;
    for (auto& id : ids) if (document_->find(id)) valid.insert(id);
    if (valid != selectedLayerIds_) { commitTransform(); resolveGradient(); }
    activeLayerId_ = primary && valid.count(*primary) ? primary : (valid.empty() ? std::nullopt : std::optional<Uuid>(*valid.begin()));
    selectedLayerIds_ = valid;
    isMaskSelected_ = false;
    emit layersChanged();
    emit transformChanged();
}

void EditorSession::toggleGroupExpansion(const Uuid& id) {
    if (!document_) return;
    const Layer* layer = document_->find(id);
    if (!layer || !layer->isGroup) return;
    if (collapsedGroupIds.count(id)) collapsedGroupIds.erase(id);
    else {
        if (activeLayerId_ && descendantIds(document_->layers, id).count(*activeLayerId_)) selectLayer(id);
        collapsedGroupIds.insert(id);
    }
    emit layersChanged();
}

void EditorSession::addBlankLayer() {
    if (!canEditLayers() || document_->layers.size() >= size_t(Document::maxLayers)) return;
    Layer layer(nextLayerName(document_->layers, "Layer"), document_->size());
    const Layer* active = activeLayer();
    layer.parentId = active && active->isGroup ? activeLayerId_ : (active ? active->parentId : std::nullopt);
    if (layer.parentId) collapsedGroupIds.erase(*layer.parentId);
    int insertion = activeLayerId_ ? document_->indexOf(*activeLayerId_) + 1 : int(document_->layers.size());
    if (active && active->isGroup) {
        auto inside = descendantIds(document_->layers, active->id);
        for (size_t i = 0; i < document_->layers.size(); i++) if (inside.count(document_->layers[i].id)) insertion = std::max(insertion, int(i) + 1);
    }
    beginEdit("New Blank Layer");
    document_->layers.insert(document_->layers.begin() + insertion, layer);
    setActiveLayer(layer.id);
    endEdit();
    notifyDocument();
}

void EditorSession::addGroup() {
    if (!canEditLayers() || document_->layers.size() >= size_t(Document::maxLayers)) return;
    Layer group(nextLayerName(document_->layers, "Folder"), document_->size());
    group.isGroup = true;
    const Layer* active = activeLayer();
    group.parentId = active && active->isGroup ? activeLayerId_ : (active ? active->parentId : std::nullopt);
    int insertion = activeLayerId_ ? document_->indexOf(*activeLayerId_) + 1 : int(document_->layers.size());
    std::vector<Layer> layers = document_->layers;
    layers.insert(layers.begin() + insertion, group);
    if (!validateHierarchy(layers)) return;
    beginEdit("New Folder");
    document_->layers = layers;
    setActiveLayer(group.id);
    if (group.parentId) collapsedGroupIds.erase(*group.parentId);
    endEdit();
    notifyDocument();
}

void EditorSession::groupSelectedLayers() {
    if (!canEditLayers() || document_->layers.size() >= size_t(Document::maxLayers) || selectedLayerIds_.empty()) return;
    std::map<Uuid, const Layer*> byId;
    for (auto& l : document_->layers) byId[l.id] = &l;
    auto ancestors = [&](const Uuid& id) {
        std::vector<std::optional<Uuid>> result;
        std::optional<Uuid> parent = byId[id]->parentId;
        int depth = 0;
        while (parent && depth++ < 64) { result.push_back(parent); parent = byId.count(*parent) ? byId[*parent]->parentId : std::nullopt; }
        result.push_back(std::nullopt);
        return result;
    };
    std::set<Uuid> selected;
    for (auto& id : selectedLayerIds_) if (byId.count(id)) selected.insert(id);
    // A selected folder carries its subtree; selected descendants must not be pulled out of it.
    std::set<Uuid> rootIds;
    for (auto& id : selected) {
        bool inside = false;
        for (auto& a : ancestors(id)) if (a && selected.count(*a)) inside = true;
        if (!inside) rootIds.insert(id);
    }
    std::vector<Uuid> ordered;
    for (auto& e : hierarchyEntries(document_->layers)) if (rootIds.count(e.layer->id)) ordered.push_back(e.layer->id);
    if (ordered.empty()) return;
    std::optional<Uuid> parent;
    for (auto& candidate : ancestors(ordered.front())) {
        bool all = true;
        for (auto& id : ordered) { auto anc = ancestors(id); if (std::find(anc.begin(), anc.end(), candidate) == anc.end()) all = false; }
        if (all) { parent = candidate; break; }
    }
    Layer group(nextLayerName(document_->layers, "Folder"), document_->size());
    group.isGroup = true;
    group.parentId = parent;
    std::vector<Uuid> branches;
    for (auto& id : ordered) {
        Uuid branch = id;
        while (byId[branch]->parentId && byId[branch]->parentId != parent) branch = *byId[branch]->parentId;
        branches.push_back(branch);
    }
    int highest = -1;
    for (size_t i = 0; i < document_->layers.size(); i++) if (std::find(branches.begin(), branches.end(), document_->layers[i].id) != branches.end()) highest = int(i);
    std::vector<Layer> layers;
    int insertion = 0;
    for (size_t i = 0; i < document_->layers.size(); i++) {
        if (rootIds.count(document_->layers[i].id)) continue;
        layers.push_back(document_->layers[i]);
        if (int(i) <= highest) insertion = int(layers.size());
    }
    if (highest < 0) insertion = int(layers.size());
    layers.insert(layers.begin() + std::min(insertion, int(layers.size())), group);
    for (auto& id : ordered) { Layer child = *byId[id]; child.parentId = group.id; layers.push_back(child); }
    if (!validateHierarchy(layers)) return;
    beginEdit("Group Layers");
    document_->layers = layers;
    setActiveLayer(group.id);
    if (parent) collapsedGroupIds.erase(*parent);
    endEdit();
    notifyDocument();
}

std::vector<Uuid> EditorSession::clippingDependents(const std::vector<Uuid>& ids) const {
    std::vector<Uuid> result;
    if (!document_) return result;
    std::set<Uuid> removed;
    for (auto& id : ids) { removed.insert(id); for (auto& d : descendantIds(document_->layers, id)) removed.insert(d); }
    for (auto& l : document_->layers) if (!removed.count(l.id) && l.maskSourceId && removed.count(*l.maskSourceId)) result.push_back(l.id);
    return result;
}

std::optional<Asset> EditorSession::bakeClipping(const Uuid& target) const {
    const Layer* layer = document_->find(target);
    if (!layer || !layer->asset || !layer->asset->image || !layer->maskSourceId) return std::nullopt;
    // The source's coverage (its alpha with its own mask and upstream clipping), ignoring visibility, at document size.
    Document chain(document_->width, document_->height);
    std::set<Uuid> keep;
    std::optional<Uuid> current = layer->maskSourceId;
    for (int i = 0; i < 256 && current; i++) { keep.insert(*current); const Layer* l = document_->find(*current); current = l ? l->maskSourceId : std::nullopt; }
    for (auto& l : document_->layers) if (keep.count(l.id)) { Layer c = l; c.parentId.reset(); c.visible = true; chain.layers.push_back(c); }
    auto flat = renderFlattened(chain);
    GrayImage coverage(document_->width, document_->height);
    for (int y = 0; y < coverage.height(); y++) for (int x = 0; x < coverage.width(); x++) coverage.at(x, y) = flat->pixel(x, y)[3];
    const Image& src = *layer->asset->image;
    auto inGrid = resampleMask(coverage, LayerTransform(Point(0, 0), document_->size()), layer->transform, src.width(), src.height(), 0);
    auto out = std::make_shared<Image>(src);
    for (int y = 0; y < src.height(); y++) for (int x = 0; x < src.width(); x++) { unsigned k = inGrid->at(x, y); uint8_t* p = out->pixel(x, y); for (int c = 0; c < 4; c++) p[c] = uint8_t((p[c] * k + 127) / 255); }
    return Asset::make(out, layer->name);
}

void EditorSession::deleteLayersResolvingClipping(const std::vector<Uuid>& ids, bool bake) {
    if (!canEditLayers()) return;
    std::map<Uuid, Asset> baked;
    if (bake) for (auto& id : clippingDependents(ids)) if (auto asset = bakeClipping(id)) baked[id] = *asset;
    finishDeleting(ids, baked);
}

bool EditorSession::duplicateLayerTo(const Uuid& id, const std::optional<Uuid>& parent, const std::optional<Uuid>& above, bool atBottom) {
    if (!canEditLayers() || !document_->find(id) || document_->find(id)->isGroup) return false;
    beginEdit("Duplicate Layer");
    selectLayer(id);
    duplicateActiveLayer();
    bool ok = activeLayerId_ && *activeLayerId_ != id && placeLayer(*activeLayerId_, parent, above, atBottom);
    endEdit();
    notifyDocument();
    return ok;
}

bool EditorSession::copyMask(const Uuid& source, const Uuid& target) {
    if (!canEditLayers() || source == target) return false;
    const Layer* from = document_->find(source);
    Layer* to = document_->find(target);
    if (!from || !from->mask || !to || to->isGroup) return false;
    commitTransform();
    endOpacityEdit();
    LayerMask mask = *from->mask;
    mask.placement = from->maskTransform();
    beginEdit(to->mask ? "Replace Layer Mask" : "Copy Layer Mask");
    to->mask = mask;
    setActiveLayer(target);
    isMaskSelected_ = true;
    endEdit();
    notifyDocument();
    return true;
}

void EditorSession::finishDeleting(const std::vector<Uuid>& ids) { finishDeleting(ids, {}); }

void EditorSession::finishDeleting(const std::vector<Uuid>& ids, const std::map<Uuid, Asset>& baked) {
    if (!document_) return;
    std::set<Uuid> removed;
    int firstIndex = int(document_->layers.size());
    for (auto& id : ids) {
        int index = document_->indexOf(id);
        if (index < 0) continue;
        firstIndex = std::min(firstIndex, index);
        removed.insert(id);
        for (auto& d : descendantIds(document_->layers, id)) removed.insert(d);
    }
    if (removed.empty()) return;
    beginEdit(ids.size() > 1 ? "Delete Layers" : "Delete Layer");
    std::vector<Layer> kept;
    for (auto& l : document_->layers) if (!removed.count(l.id)) kept.push_back(l);
    for (auto& l : kept) if (l.maskSourceId && removed.count(*l.maskSourceId)) { l.maskSourceId.reset(); auto b = baked.find(l.id); if (b != baked.end()) { l.asset = b->second; l.shapeImage.reset(); } }
    document_->layers = kept;
    if (activeLayerId_ && removed.count(*activeLayerId_)) {
        setActiveLayer(kept.empty() ? std::nullopt : std::optional<Uuid>(kept[size_t(std::min(firstIndex, int(kept.size()) - 1))].id));
    }
    endEdit();
    notifyDocument();
}

void EditorSession::deleteLayer(const Uuid& id) {
    if (!canEditLayers() || !document_->find(id)) return;
    finishDeleting({id});
}

void EditorSession::deleteSelectedLayers() {
    if (!canEditLayers()) return;
    std::vector<Uuid> ids;
    for (auto& l : document_->layers) if (selectedLayerIds_.count(l.id)) ids.push_back(l.id);
    if (ids.empty() && activeLayerId_) ids.push_back(*activeLayerId_);
    finishDeleting(ids);
}

void EditorSession::duplicateActiveLayer() {
    if (!canEditLayers() || !activeLayerId_ || document_->layers.size() >= size_t(Document::maxLayers)) return;
    int index = document_->indexOf(*activeLayerId_);
    if (index < 0) return;
    Layer source = document_->layers[size_t(index)];
    std::vector<Layer> copies;
    std::map<Uuid, Uuid> newIds;
    // A folder duplicates with its contents.
    std::vector<int> indices{index};
    if (source.isGroup) {
        auto inside = descendantIds(document_->layers, source.id);
        for (size_t i = 0; i < document_->layers.size(); i++) if (inside.count(document_->layers[i].id)) indices.push_back(int(i));
        std::sort(indices.begin(), indices.end());
    }
    for (int i : indices) {
        Layer copy = document_->layers[size_t(i)];
        newIds[copy.id] = makeUuid();
        copy.id = newIds[copy.id];
        copies.push_back(copy);
    }
    for (auto& c : copies) {
        if (c.parentId && newIds.count(*c.parentId)) c.parentId = newIds[*c.parentId];
        if (c.maskSourceId && newIds.count(*c.maskSourceId)) c.maskSourceId = newIds[*c.maskSourceId];
    }
    copies.front().name = source.name + " copy";
    int insertion = indices.back() + 1;
    beginEdit("Duplicate Layer");
    document_->layers.insert(document_->layers.begin() + insertion, copies.begin(), copies.end());
    releaseDetachedClipping(document_->layers);
    setActiveLayer(copies.front().id);
    endEdit();
    notifyDocument();
}

void EditorSession::mergeDown() {
    if (!canEditLayers() || !activeLayerId_) return;
    const Layer* top = activeLayer();
    if (!top || top->isGroup) return;
    // The next lower sibling with pixels.
    const Layer* below = nullptr;
    int topIndex = document_->indexOf(top->id);
    for (int i = topIndex - 1; i >= 0; i--) {
        const Layer& l = document_->layers[size_t(i)];
        if (l.parentId == top->parentId && !l.isGroup) { below = &l; break; }
        if (l.parentId == top->parentId && l.isGroup) return;
    }
    if (!below || below->adjustment) return;
    // Render both as displayed, over transparency, on the union of their bounds.
    Rect bounds = below->transform.bounds().unionWith(top->transform.bounds()).intersection(document_->rect()).integral();
    if (bounds.isEmpty()) return;
    Document pair(document_->width, document_->height);
    Layer a = *below, b = *top;
    a.parentId.reset(); b.parentId.reset(); a.visible = b.visible = true;
    pair.layers = {a, b};
    Image out;
    RenderOptions options;
    options.region = bounds;
    render(pair, options, out);
    Layer merged = *below;
    merged.asset = Asset::make(std::make_shared<Image>(std::move(out)), below->name);
    merged.transform = LayerTransform(bounds.origin(), bounds.size());
    merged.mask.reset();
    merged.shape.reset();
    merged.shapeImage.reset();
    merged.opacity = 1;
    merged.blendMode = below->blendMode;
    beginEdit("Merge Down");
    int belowIndex = document_->indexOf(below->id);
    document_->layers[size_t(belowIndex)] = merged;
    document_->layers.erase(document_->layers.begin() + topIndex);
    for (auto& l : document_->layers) if (l.maskSourceId == top->id) l.maskSourceId = merged.id;
    setActiveLayer(merged.id);
    endEdit();
    notifyDocument();
}

void EditorSession::renameLayer(const Uuid& id, const QString& name) {
    QString trimmed = name.trimmed();
    if (!document_ || trimmed.isEmpty()) return;
    Layer* layer = document_->find(id);
    if (!layer || layer->name == trimmed.toStdString()) return;
    beginEdit("Rename Layer");
    layer->name = trimmed.toStdString();
    endEdit();
    notifyDocument();
}

void EditorSession::toggleLayerVisibility(const Uuid& id) {
    if (!canEditLayers()) return;
    Layer* layer = document_->find(id);
    if (!layer) return;
    beginEdit(layer->visible ? "Hide Layer" : "Show Layer");
    layer->visible = !layer->visible;
    endEdit();
    notifyDocument();
}

void EditorSession::beginVisibilitySwipe(const Uuid& id) {
    if (!canEditLayers() || visibilitySwipe_) return;
    Layer* layer = document_->find(id);
    if (!layer) return;
    visibilitySwipe_ = true;
    beginEdit(layer->visible ? "Hide Layer" : "Show Layer");
    setVisibilityInSwipe(id, !layer->visible);
}

void EditorSession::setVisibilityInSwipe(const Uuid& id, bool visible) {
    if (!document_) return;
    Layer* layer = document_->find(id);
    if (!layer || layer->visible == visible) return;
    layer->visible = visible;
    emit documentChanged({});
    emit layersChanged();
}

void EditorSession::endVisibilitySwipe() {
    if (!visibilitySwipe_) return;
    visibilitySwipe_ = false;
    endEdit();
    notifyDocument();
}

bool EditorSession::canMoveActiveLayer(int offset) const {
    const Layer* active = activeLayer();
    if (!canEditLayers() || !active) return false;
    std::vector<const Layer*> siblings;
    for (auto& l : document_->layers) if (l.parentId == active->parentId) siblings.push_back(&l);
    int index = -1;
    for (size_t i = 0; i < siblings.size(); i++) if (siblings[i]->id == active->id) index = int(i);
    return index >= 0 && index + offset >= 0 && index + offset < int(siblings.size());
}

void EditorSession::moveActiveLayer(int offset) {
    if (!canMoveActiveLayer(offset)) return;
    const Layer* active = activeLayer();
    std::vector<const Layer*> siblings;
    for (auto& l : document_->layers) if (l.parentId == active->parentId) siblings.push_back(&l);
    int index = -1;
    for (size_t i = 0; i < siblings.size(); i++) if (siblings[i]->id == active->id) index = int(i);
    const Layer* target = siblings[size_t(index + offset)];
    if (target->isGroup) {
        // Skip over a folder and its contents.
        placeLayer(active->id, active->parentId, offset > 0 ? std::optional<Uuid>(target->id) : (index + offset - 1 >= 0 ? std::optional<Uuid>(siblings[size_t(index + offset - 1)]->id) : std::nullopt), offset < 0 && index + offset - 1 < 0);
        return;
    }
    int a = document_->indexOf(active->id), b = document_->indexOf(target->id);
    beginEdit("Reorder Layers");
    std::swap(document_->layers[size_t(a)], document_->layers[size_t(b)]);
    releaseDetachedClipping(document_->layers);
    endEdit();
    notifyDocument();
}

void EditorSession::adoptClipping(const Uuid& id, std::vector<Layer>& layers) {
    const Layer* layer = nullptr;
    for (auto& l : layers) if (l.id == id) layer = &l;
    if (!layer || layer->isGroup) return;
    std::vector<const Layer*> siblings;
    for (auto& l : layers) if (l.parentId == layer->parentId) siblings.push_back(&l);
    int index = -1;
    for (size_t i = 0; i < siblings.size(); i++) if (siblings[i]->id == id) index = int(i);
    if (index <= 0 || index + 1 >= int(siblings.size())) return;
    const Layer* above = siblings[size_t(index + 1)];
    if (!above->maskSourceId || *above->maskSourceId == id) return;
    const Layer* below = siblings[size_t(index - 1)];
    if (below->id == *above->maskSourceId || below->maskSourceId == above->maskSourceId) {
        for (auto& l : layers) if (l.id == id) l.maskSourceId = above->maskSourceId;
    }
}

void EditorSession::releaseDetachedClipping(std::vector<Layer>& layers) {
    std::map<std::optional<Uuid>, std::vector<Layer*>> siblings;
    for (auto& l : layers) siblings[l.parentId].push_back(&l);
    for (auto& [parent, stack] : siblings) {
        std::optional<Uuid> base;
        for (Layer* layer : stack) {
            if (layer->maskSourceId) {
                if (layer->maskSourceId != base) { layer->maskSourceId.reset(); base = layer->id; }
            } else base = layer->isGroup ? std::nullopt : std::optional<Uuid>(layer->id);
        }
    }
}

bool EditorSession::placeLayer(const Uuid& id, const std::optional<Uuid>& parent, const std::optional<Uuid>& above, bool atBottom) {
    if (!canEditLayers() || !document_->find(id) || above == id) return false;
    if (parent) {
        const Layer* group = document_->find(*parent);
        if (!group || !group->isGroup || *parent == id || descendantIds(document_->layers, id).count(*parent)) return false;
    }
    std::vector<Layer> layers = document_->layers;
    int index = -1;
    for (size_t i = 0; i < layers.size(); i++) if (layers[i].id == id) index = int(i);
    Layer layer = layers[size_t(index)];
    // A folder moves with its contents, keeping their order.
    std::set<Uuid> moving = descendantIds(layers, id);
    std::vector<Layer> subtree;
    for (auto& l : layers) if (moving.count(l.id)) subtree.push_back(l);
    layers.erase(std::remove_if(layers.begin(), layers.end(), [&](const Layer& l) { return l.id == id || moving.count(l.id); }), layers.end());
    layer.parentId = parent;
    int insertion = atBottom ? 0 : int(layers.size());
    if (above) {
        int target = -1;
        for (size_t i = 0; i < layers.size(); i++) if (layers[i].id == *above && layers[i].parentId == parent) target = int(i);
        if (target < 0) return false;
        // Above a folder means above everything inside it.
        auto inside = descendantIds(layers, *above);
        for (size_t i = 0; i < layers.size(); i++) if (inside.count(layers[i].id)) target = std::max(target, int(i));
        insertion = target + 1;
    }
    layers.insert(layers.begin() + insertion, layer);
    layers.insert(layers.begin() + insertion + 1, subtree.begin(), subtree.end());
    adoptClipping(id, layers);
    releaseDetachedClipping(layers);
    if (!validateHierarchy(layers)) return false;
    beginEdit("Move Layer");
    document_->layers = layers;
    setActiveLayer(id);
    if (parent) collapsedGroupIds.erase(*parent);
    endEdit();
    notifyDocument();
    return true;
}

void EditorSession::beginOpacityEdit() {
    if (!canEditLayers() || opacityEditing_ || !activeLayerId_) return;
    beginEdit("Layer Opacity");
    opacityEditing_ = true;
}

void EditorSession::endOpacityEdit() {
    if (!opacityEditing_) return;
    opacityEditing_ = false;
    endEdit();
    notifyDocument();
}

void EditorSession::setLayerOpacity(double opacity) {
    if (!document_ || !std::isfinite(opacity) || (!canEditLayers() && !opacityEditing_)) return;
    double value = std::clamp(opacity, 0.0, 1.0);
    std::vector<Layer*> targets;
    for (auto& l : document_->layers) if (selectedLayerIds_.count(l.id) && !l.isGroup && l.opacity != value) targets.push_back(&l);
    if (targets.empty()) return;
    bool standalone = !opacityEditing_;
    if (standalone) beginEdit("Layer Opacity");
    for (Layer* l : targets) l->opacity = value;
    if (standalone) { endEdit(); notifyDocument(); }
    else { emit documentChanged({}); emit layersChanged(); }
}

void EditorSession::setLayerBlendMode(BlendMode mode) {
    if (!canEditLayers()) return;
    Layer* layer = activeLayerMutable();
    if (!layer || layer->isGroup || layer->blendMode == mode) return;
    endOpacityEdit();
    beginEdit("Layer Blend Mode");
    layer->blendMode = mode;
    endEdit();
    notifyDocument();
}

bool EditorSession::canToggleClippingMask(const Uuid& id) const {
    if (!canEditLayers()) return false;
    const Layer* layer = document_->find(id);
    if (!layer || layer->isGroup) return false;
    if (layer->maskSourceId) return true;
    std::vector<const Layer*> siblings;
    for (auto& l : document_->layers) if (l.parentId == layer->parentId) siblings.push_back(&l);
    int index = -1;
    for (size_t i = 0; i < siblings.size(); i++) if (siblings[i]->id == id) index = int(i);
    if (index <= 0) return false;
    const Layer* below = siblings[size_t(index - 1)];
    if (below->isGroup) return false;
    std::vector<Layer> layers = document_->layers;
    for (auto& l : layers) if (l.id == id) l.maskSourceId = below->maskSourceId ? below->maskSourceId : std::optional<Uuid>(below->id);
    return validateClipping(layers);
}

void EditorSession::toggleClippingMask(const Uuid& id) {
    if (!canToggleClippingMask(id)) return;
    Layer* layer = document_->find(id);
    if (layer->maskSourceId) {
        // Releasing a base releases the clipped layers above it that share that base.
        Uuid source = *layer->maskSourceId;
        std::vector<Layer*> siblings;
        for (auto& l : document_->layers) if (l.parentId == layer->parentId) siblings.push_back(&l);
        int index = -1;
        for (size_t i = 0; i < siblings.size(); i++) if (siblings[i]->id == id) index = int(i);
        beginEdit("Release Clipping Mask");
        for (size_t i = size_t(index); i < siblings.size(); i++) {
            if (siblings[i]->id == id || siblings[i]->maskSourceId == source) siblings[i]->maskSourceId.reset();
            else break;
        }
        endEdit();
    } else {
        std::vector<const Layer*> siblings;
        for (auto& l : document_->layers) if (l.parentId == layer->parentId) siblings.push_back(&l);
        int index = -1;
        for (size_t i = 0; i < siblings.size(); i++) if (siblings[i]->id == id) index = int(i);
        const Layer* below = siblings[size_t(index - 1)];
        beginEdit("Create Clipping Mask");
        layer->maskSourceId = below->maskSourceId ? below->maskSourceId : std::optional<Uuid>(below->id);
        endEdit();
    }
    notifyDocument();
}

void EditorSession::addLayerMask(bool revealing) {
    if (!canEditLayers()) return;
    Layer* layer = activeLayerMutable();
    if (!layer || layer->mask) return;
    endOpacityEdit();
    beginEdit(revealing ? "Add Reveal-All Mask" : "Add Hide-All Mask");
    LayerMask mask;
    mask.asset = MaskAsset::solid(revealing);
    layer->mask = mask;
    isMaskSelected_ = true;
    endEdit();
    notifyDocument();
}

void EditorSession::addMaskFromSelection(bool revealing) {
    if (!document_ || !document_->selection || !document_->selection->coverage) { addLayerMask(revealing); return; }
    if (!canEditLayers()) return;
    Layer* layer = activeLayerMutable();
    if (!layer || layer->mask) return;
    int width = layer->pixelWidth(), height = layer->pixelHeight();
    if ((long long)width * height > Document::pixelBudget) return;
    // The selection resampled into the layer's own pixel grid; the selected area gets the opposite value.
    LayerTransform docTransform(Point(0, 0), document_->size());
    auto selected = resampleMask(*document_->selection->coverage, docTransform, layer->transform, width, height, 0);
    auto mask = std::make_shared<GrayImage>(width, height, revealing ? 255 : 0);
    for (int y = 0; y < height; y++) for (int x = 0; x < width; x++) {
        int s = selected->at(x, y);
        mask->at(x, y) = uint8_t(revealing ? 255 - s : s);
    }
    endOpacityEdit();
    beginEdit("Add Mask from Selection");
    LayerMask m;
    m.asset = MaskAsset::make(mask);
    layer->mask = m;
    document_->selection.reset();
    isMaskSelected_ = true;
    endEdit();
    notifyDocument();
    emit selectionChanged();
}

void EditorSession::toggleLayerMask() {
    if (!canEditLayers()) return;
    Layer* layer = activeLayerMutable();
    if (!layer || !layer->mask) return;
    beginEdit(layer->mask->enabled ? "Disable Layer Mask" : "Enable Layer Mask");
    layer->mask->enabled = !layer->mask->enabled;
    endEdit();
    notifyDocument();
}

void EditorSession::deleteLayerMask() {
    if (!canEditLayers()) return;
    Layer* layer = activeLayerMutable();
    if (!layer || !layer->mask) return;
    beginEdit("Delete Layer Mask");
    layer->mask.reset();
    isMaskSelected_ = false;
    endEdit();
    notifyDocument();
}

void EditorSession::toggleMaskLink(const Uuid& id) {
    if (!canEditLayers()) return;
    Layer* layer = document_->find(id);
    if (!layer || !layer->mask) return;
    beginEdit(layer->mask->linked ? "Unlink Layer Mask" : "Link Layer Mask");
    layer->mask->linked = !layer->mask->linked;
    endEdit();
    notifyDocument();
}

void EditorSession::applyMask() {
    if (!canEditLayers()) return;
    Layer* layer = activeLayerMutable();
    if (!layer || !layer->mask || layer->isGroup || !layer->asset || !layer->asset->image) return;
    const Image& src = *layer->asset->image;
    int w = src.width(), h = src.height();
    std::shared_ptr<const GrayImage> mask = layer->mask->asset.image;
    if (layer->mask->placement) mask = resampleMask(*mask, *layer->mask->placement, layer->transform, w, h, LayerMask::background(*layer->mask->asset.thumbnail));
    auto out = std::make_shared<Image>(w, h);
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
        int m = mask->width() == 1 && mask->height() == 1 ? mask->at(0, 0) : mask->at(std::min(x * mask->width() / w, mask->width() - 1), std::min(y * mask->height() / h, mask->height() - 1));
        const uint8_t* s = src.pixel(x, y);
        uint8_t* d = out->pixel(x, y);
        for (int c = 0; c < 4; c++) d[c] = uint8_t((s[c] * m + 127) / 255);
    }
    beginEdit("Apply Layer Mask");
    layer->asset = Asset::make(out, layer->name);
    layer->mask.reset();
    layer->shapeImage.reset();
    isMaskSelected_ = false;
    endEdit();
    notifyDocument();
}

void EditorSession::invertMask() {
    if (!canEditLayers()) return;
    Layer* layer = activeLayerMutable();
    if (!layer || !layer->mask) return;
    auto out = std::make_shared<GrayImage>(*layer->mask->asset.image);
    for (size_t i = 0; i < out->byteCount(); i++) out->data()[i] = uint8_t(255 - out->data()[i]);
    beginEdit("Invert Mask");
    layer->mask->asset = MaskAsset::make(out);
    endEdit();
    notifyDocument();
}

void EditorSession::flipLayer(bool horizontal) {
    if (!canEditLayers()) return;
    std::vector<Layer*> targets;
    for (auto& l : document_->layers) if (selectedLayerIds_.count(l.id) && !l.isGroup && l.asset) targets.push_back(&l);
    if (targets.empty()) return;
    beginEdit(horizontal ? "Flip Layer Horizontal" : "Flip Layer Vertical");
    for (Layer* l : targets) {
        LayerTransform t = l->transform;
        // Flipping about the layer's own axis: mirror the flag and the rotation.
        if (horizontal) t.flipX = !t.flipX; else t.flipY = !t.flipY;
        t.rotation = -t.rotation;
        if (l->mask) l->mask->placement = l->mask->placementMovingLayer(l->transform, t);
        l->transform = t;
    }
    endEdit();
    notifyDocument();
}

void EditorSession::flipCanvas(bool horizontal) {
    if (!canEditLayers()) return;
    beginEdit(horizontal ? "Flip Canvas Horizontal" : "Flip Canvas Vertical");
    double w = document_->width, h = document_->height;
    auto flip = [&](LayerTransform t) {
        Point c = t.center();
        if (horizontal) { t.flipX = !t.flipX; c.x = w - c.x; } else { t.flipY = !t.flipY; c.y = h - c.y; }
        t.rotation = -t.rotation;
        t.origin = {c.x - t.size.width / 2, c.y - t.size.height / 2};
        return t;
    };
    for (auto& l : document_->layers) {
        l.transform = flip(l.transform);
        if (l.mask && l.mask->placement) l.mask->placement = flip(*l.mask->placement);
    }
    if (document_->selection && document_->selection->coverage) {
        auto out = std::make_shared<GrayImage>(*document_->selection->coverage);
        for (int y = 0; y < out->height(); y++) for (int x = 0; x < out->width(); x++)
            out->at(x, y) = document_->selection->coverage->at(horizontal ? out->width() - 1 - x : x, horizontal ? y : out->height() - 1 - y);
        document_->selection->coverage = out;
    }
    endEdit();
    notifyDocument();
    emit selectionChanged();
}

void EditorSession::setLayerSampling(Sampling sampling) {
    if (!canEditLayers()) return;
    Layer* layer = activeLayerMutable();
    if (!layer || layer->transform.sampling == sampling) return;
    beginEdit("Layer Sampling");
    layer->transform.sampling = sampling;
    endEdit();
    notifyDocument();
}

// ---- Transform -----------------------------------------------------------------

bool EditorSession::transformsAsGroup() const {
    const Layer* active = activeLayer();
    return selectedLayerIds_.size() > 1 || (selectedLayerIds_.size() == 1 && active && active->isGroup);
}

std::vector<const Layer*> EditorSession::groupTransformMembers() const {
    std::vector<const Layer*> result;
    if (!document_ || !transformsAsGroup()) return result;
    auto visible = effectiveVisibleIds(document_->layers);
    for (auto& layer : document_->layers) {
        if (!layer.asset || layer.isGroup || !visible.count(layer.id)) continue;
        std::optional<Uuid> current = layer.id;
        for (int i = 0; i < 64 && current; i++) {
            if (selectedLayerIds_.count(*current)) { result.push_back(&layer); break; }
            const Layer* l = document_->find(*current);
            current = l ? l->parentId : std::nullopt;
        }
    }
    return result;
}

std::optional<LayerTransform> EditorSession::groupTransformBox() const {
    double minX = 1e300, minY = 1e300, maxX = -1e300, maxY = -1e300;
    bool any = false;
    for (const Layer* l : groupTransformMembers()) for (auto& p : l->transform.corners()) { minX = std::min(minX, p.x); minY = std::min(minY, p.y); maxX = std::max(maxX, p.x); maxY = std::max(maxY, p.y); any = true; }
    if (!any) return std::nullopt;
    return LayerTransform(Point(minX, minY), Size(std::max(1.0, maxX - minX), std::max(1.0, maxY - minY)));
}

bool EditorSession::canTransform() const {
    if (!document_ || stroke_ || warp_ || pixelMove_) return false;
    if (transformsAsGroup()) return !groupTransformMembers().empty();
    const Layer* active = activeLayer();
    if (!active || active->isGroup) return false;
    if (isMaskSelected_ && active->mask && !active->mask->linked) return true;
    return active->asset.has_value() && effectiveVisibleIds(document_->layers).count(active->id);
}

bool EditorSession::canTransformSelection() const {
    if (transformEdit_ || !canEditLayers() || isMaskSelected_) return false;
    const Layer* active = activeLayer();
    return active && active->asset && !active->isGroup && document_->selection && document_->selection->coverage && !document_->selection->isEmpty();
}

void EditorSession::transformCommand() {
    if (canTransformSelection()) beginSelectionTransform();
    else { selectTool(Tool::Move); beginTransform(true); }
}

void EditorSession::beginTransform(bool persistent) {
    if (transformEdit_ || !canTransform()) return;
    resolveGradient();
    const Layer* layer = activeLayer();
    if (!layer) return;
    tool_ = Tool::Move;
    if (transformsAsGroup()) {
        auto box = groupTransformBox();
        if (!box) return;
        TransformGroup group;
        group.box = *box;
        for (const Layer* l : groupTransformMembers()) group.originals[l->id] = l->transform;
        TransformEdit edit{layer->id, *box, persistent, false};
        edit.group = group;
        transformEdit_ = edit;
    } else {
        bool maskAlone = isMaskSelected_ && layer->mask && !layer->mask->linked;
        transformEdit_ = TransformEdit{layer->id, maskAlone ? layer->maskTransform() : layer->transform, persistent, maskAlone};
    }
    emit toolChanged();
    emit transformChanged();
}

void EditorSession::beginSelectionTransform() {
    if (!canTransformSelection()) return;
    const Layer* source = activeLayer();
    auto lifted = renderSelectedPixels(false);
    if (!lifted) return;
    Document before = *document_;
    std::optional<Uuid> beforeActive = activeLayerId_;
    // Outer edit: closed by commitTransform (merge) or cancelTransform (restore).
    beginEdit("Transform Selection");
    Layer* src = document_->find(source->id);
    clearSelectedPixelsNow(*src);
    Layer floating(Asset::make(lifted->image, "Floating Selection"), toPoint(lifted->origin));
    floating.name = "Floating Selection";
    floating.parentId = src->parentId;
    floating.opacity = src->opacity;
    floating.blendMode = src->blendMode;
    int index = document_->indexOf(src->id);
    document_->layers.insert(document_->layers.begin() + index + 1, floating);
    setActiveLayer(floating.id);
    tool_ = Tool::Move;
    TransformEdit edit{floating.id, floating.transform, true, false};
    edit.floating = FloatingTransform{source->id, std::move(before), beforeActive, floating.transform, lifted->image->width(), lifted->image->height()};
    transformEdit_ = edit;
    emit toolChanged();
    notifyDocument();
    emit transformChanged();
}

void EditorSession::beginDuplicateTransform() {
    if (transformDuplicate_ || transformsAsGroup() || !activeLayerId_) return;
    Uuid source = *activeLayerId_;
    commitTransform();
    if (!canTransform()) return;
    beginEdit("Duplicate Layer");
    duplicateActiveLayer();
    if (!activeLayerId_ || *activeLayerId_ == source) { endEdit(); return; }
    transformDuplicate_ = std::make_pair(*activeLayerId_, source);
    beginTransform(false);
}

void EditorSession::previewTransform(const LayerTransform& value) {
    if (!transformEdit_ || !value.isValid()) return;
    transformEdit_->draft = value;
    emit documentChanged({});
    emit transformChanged();
}

void EditorSession::beginDistort() {
    if (!transformEdit_ || transformEdit_->corners || !transformEdit_->draft.isValid() || transformEdit_->mask) return;
    transformEdit_->corners = cornersOf(transformEdit_->draft);
    transformEdit_->persistent = true;
    emit transformChanged();
}

void EditorSession::previewCorners(const Corners& corners) {
    if (!transformEdit_ || !transformEdit_->corners || !cornersUsable(corners)) return;
    transformEdit_->corners = corners;
    emit documentChanged({});
    emit transformChanged();
}

Corners EditorSession::editedCorners(const Layer& layer) const {
    if (transformEdit_ && transformEdit_->corners && transformEdit_->layerId == layer.id) return *transformEdit_->corners;
    return cornersOf(editedTransform(layer));
}

std::optional<std::pair<LayerTransform, Corners>> EditorSession::distortTarget(const Layer& layer, const TransformEdit& edit) const {
    if (!edit.corners) return std::nullopt;
    if (!edit.group) return edit.layerId == layer.id ? std::optional(std::make_pair(edit.draft, *edit.corners)) : std::nullopt;
    auto it = edit.group->originals.find(layer.id);
    if (it == edit.group->originals.end()) return std::nullopt;
    LayerTransform transform = it->second.following(edit.group->box, edit.draft);
    Corners carried = carriedCorners(transform, edit.draft, *edit.corners);
    if (!cornersUsable(carried)) return std::nullopt;
    return std::make_pair(transform, carried);
}

void EditorSession::commitMaskTransform(const TransformEdit& edit) {
    Layer* layer = document_->find(edit.layerId);
    if (!layer || !layer->mask || !edit.draft.isValid()) return;
    std::optional<LayerTransform> placement = edit.draft.samePlacement(layer->transform) ? std::nullopt : std::optional<LayerTransform>(edit.draft);
    if (placement == layer->mask->placement) return;
    beginEdit("Transform Layer Mask");
    layer->mask->placement = placement;
    endEdit();
}

void EditorSession::commitDistort(const TransformEdit& edit) {
    distortCache_.clear();
    std::vector<Uuid> ids;
    if (edit.group) for (auto& [id, t] : edit.group->originals) ids.push_back(id); else ids.push_back(edit.layerId);
    beginEdit(edit.group ? "Distort Layers" : "Distort");
    for (auto& id : ids) {
        Layer* layer = document_->find(id);
        if (!layer || !layer->asset || !layer->asset->image) continue;
        auto target = distortTarget(*layer, edit);
        if (!target) continue;
        Rect crop;
        auto warped = warpImageTrimmed(*layer->asset->image, target->first, target->second, &crop);
        if (!warped) { emit error(tr("That shape can't be applied.")); continue; }
        if (layer->mask && layer->mask->asset.image) {
            LayerMask& mask = *layer->mask;
            if (!mask.placement && mask.linked) {
                auto wm = warpMask(*mask.asset.image, target->first, target->second, 0, 0);
                if (wm) {
                    if (wm->image->width() == 1 && wm->image->height() == 1) {}
                    else mask.asset = MaskAsset::make(cropGray(*wm->image, int(crop.x), int(crop.y), int(crop.width), int(crop.height)));
                }
            } else if (mask.linked && mask.placement) {
                LayerTransform placement = mask.placement->following(layer->transform, target->first);
                Corners carried = carriedCorners(placement, target->first, target->second);
                if (cornersUsable(carried)) {
                    auto wm = warpMask(*mask.asset.image, placement, carried, LayerMask::background(*mask.asset.thumbnail), 0);
                    if (wm) { mask.asset = MaskAsset::make(wm->image); mask.placement = wm->transform; }
                }
            } else if (!mask.placement) {
                mask.placement = layer->transform;
            }
        }
        layer->asset = Asset::make(warped->image, layer->name);
        layer->transform = warped->transform;
        layer->shapeImage.reset();
    }
    endEdit();
}

void EditorSession::mergeFloatingTransform(const TransformEdit& edit) {
    const FloatingTransform& floating = *edit.floating;
    Layer* moving = document_->find(edit.layerId);
    Layer* source = document_->find(floating.sourceId);
    if (!moving || !source || !moving->asset || !source->asset || !edit.draft.isValid()) { cancelFloatingTransform(floating); return; }
    std::shared_ptr<const Image> pixels = moving->asset->image;
    LayerTransform placed = edit.draft;
    if (edit.corners) {
        auto warped = warpImageTrimmed(*pixels, edit.draft, *edit.corners);
        if (!warped) { cancelFloatingTransform(floating); return; }
        pixels = warped->image;
        placed = warped->transform;
    }
    // Draw the floating pixels onto the source's own grid, growing it where they now extend past it.
    const Image& src = *source->asset->image;
    int w = src.width(), h = src.height();
    Affine toPixels = source->transform.pixelToDocument(w, h).inverted();
    Rect floatBounds = toPixels.mapBounds(placed.pixelToDocument(pixels->width(), pixels->height()).mapBounds(Rect(0, 0, pixels->width(), pixels->height())));
    Rect extent = Rect(0, 0, w, h).unionWith(floatBounds).integral();
    if (extent.width > 30000 || extent.height > 30000 || extent.width * extent.height > double(Document::pixelBudget)) { cancelFloatingTransform(floating); emit error(tr("The merged layer would exceed the size limits.")); return; }
    auto grown = std::make_shared<Image>(int(extent.width), int(extent.height));
    for (int y = 0; y < h; y++) std::memcpy(grown->pixel(int(-extent.x), y + int(-extent.y)), src.row(y), size_t(w) * 4);
    LayerTransform grownTransform = source->transform;
    grownTransform.size = {extent.width * source->transform.size.width / w, extent.height * source->transform.size.height / h};
    Point center = source->transform.pixelToDocument(w, h).apply({extent.midX(), extent.midY()});
    grownTransform.origin = {center.x - grownTransform.size.width / 2, center.y - grownTransform.size.height / 2};
    auto onto = resampleLayer(*pixels, placed, grownTransform, grown->width(), grown->height());
    compositeImage(BlendMode::Normal, *onto, 1, *grown);
    if (source->mask && !source->mask->placement && source->mask->asset.image && (extent.width != w || extent.height != h)) {
        const GrayImage& old = *source->mask->asset.image;
        auto mask = std::make_shared<GrayImage>(grown->width(), grown->height(), 255);
        if (old.width() == 1 && old.height() == 1) mask->fill(old.at(0, 0));
        else for (int y = 0; y < h; y++) std::memcpy(mask->row(y + int(-extent.y)) + int(-extent.x), old.row(y), size_t(w));
        source->mask->asset = MaskAsset::make(mask);
    }
    // The selection follows the pixels.
    if (document_->selection && document_->selection->coverage) {
        std::shared_ptr<GrayImage> moved;
        if (edit.corners) moved = warpCoverage(*document_->selection->coverage, floating.original, floating.pixelWidth, floating.pixelHeight, *edit.corners);
        else {
            Affine map = floating.original.pixelToDocument(floating.pixelWidth, floating.pixelHeight).inverted().concatenating(edit.draft.pixelToDocument(floating.pixelWidth, floating.pixelHeight));
            Affine inv = map.inverted();
            const GrayImage& cov = *document_->selection->coverage;
            moved = std::make_shared<GrayImage>(cov.width(), cov.height(), 0);
            for (int y = 0; y < cov.height(); y++) for (int x = 0; x < cov.width(); x++) {
                Point p = inv.apply({x + 0.5, y + 0.5});
                int sx = int(std::floor(p.x)), sy = int(std::floor(p.y));
                if (sx >= 0 && sy >= 0 && sx < cov.width() && sy < cov.height()) moved->at(x, y) = cov.at(sx, sy);
            }
        }
        document_->selection->coverage = moved;
    }
    source->asset = Asset::make(grown, source->name);
    source->transform = grownTransform;
    source->shapeImage.reset();
    Uuid sourceId = source->id;
    document_->layers.erase(document_->layers.begin() + document_->indexOf(edit.layerId));
    setActiveLayer(sourceId);
    endEdit();
}

void EditorSession::cancelFloatingTransform(const FloatingTransform& floating) {
    document_ = floating.before;
    setActiveLayer(floating.beforeActive);
    endEdit();
}

void EditorSession::commitTransform() {
    snapGuidesX.clear();
    snapGuidesY.clear();
    endOpacityEdit();
    if (!transformEdit_) return;
    TransformEdit edit = *transformEdit_;
    transformEdit_.reset();
    distortCache_.clear();
    auto finishDuplicate = [&] { if (transformDuplicate_) { transformDuplicate_.reset(); endEdit(); } };
    if (edit.floating) {
        if (edit.draft == edit.floating->original && !edit.corners) cancelFloatingTransform(*edit.floating);
        else mergeFloatingTransform(edit);
        notifyDocument(); emit selectionChanged(); emit transformChanged();
        return;
    }
    if (edit.mask) { commitMaskTransform(edit); finishDuplicate(); notifyDocument(); emit transformChanged(); return; }
    if (edit.corners) { commitDistort(edit); finishDuplicate(); notifyDocument(); emit transformChanged(); return; }
    if (edit.group) {
        if (edit.draft.isValid()) {
            beginEdit("Transform Layers");
            for (auto& [id, original] : edit.group->originals) {
                Layer* layer = document_->find(id);
                if (!layer) continue;
                LayerTransform moved = original.following(edit.group->box, edit.draft);
                if (!moved.isValid()) continue;
                if (layer->mask) layer->mask->placement = layer->mask->placementMovingLayer(original, moved);
                layer->transform = moved;
                redrawShape(*layer);
            }
            endEdit();
        }
        finishDuplicate(); notifyDocument(); emit transformChanged();
        return;
    }
    Layer* layer = document_ ? document_->find(edit.layerId) : nullptr;
    if (layer && edit.draft.isValid() && !edit.draft.samePlacement(layer->transform)) {
        beginEdit("Transform Layer");
        if (layer->mask) layer->mask->placement = layer->mask->placementMovingLayer(layer->transform, edit.draft);
        layer->transform = edit.draft;
        redrawShape(*layer);
        endEdit();
    }
    finishDuplicate();
    notifyDocument();
    emit transformChanged();
}

void EditorSession::cancelTransform() {
    snapGuidesX.clear();
    snapGuidesY.clear();
    if (!transformEdit_) return;
    TransformEdit edit = *transformEdit_;
    transformEdit_.reset();
    distortCache_.clear();
    if (transformDuplicate_) {
        auto [copy, source] = *transformDuplicate_;
        document_->layers.erase(std::remove_if(document_->layers.begin(), document_->layers.end(), [&](const Layer& l) { return l.id == copy; }), document_->layers.end());
        setActiveLayer(source);
        transformDuplicate_.reset();
        endEdit();
    }
    if (edit.floating) cancelFloatingTransform(*edit.floating);
    notifyDocument();
    emit selectionChanged();
    emit transformChanged();
}

void EditorSession::nudgeLayer(double dx, double dy) {
    bool alreadyEditing = transformEdit_.has_value();
    if (!alreadyEditing) beginTransform(false);
    if (!transformEdit_) return;
    LayerTransform value = transformEdit_->draft;
    value.origin.x += dx;
    value.origin.y += dy;
    if (transformEdit_->corners) { Corners c = *transformEdit_->corners; for (auto& p : c) { p.x += dx; p.y += dy; } transformEdit_->corners = c; }
    previewTransform(value);
    if (!alreadyEditing) commitTransform();
}

LayerTransform EditorSession::displayedTransform(const Layer& layer) const {
    if (transformEdit_ && !transformEdit_->mask) {
        if (transformEdit_->group) { auto it = transformEdit_->group->originals.find(layer.id); if (it != transformEdit_->group->originals.end()) return it->second.following(transformEdit_->group->box, transformEdit_->draft); }
        else if (transformEdit_->layerId == layer.id) return transformEdit_->draft;
    }
    return layer.transform;
}

std::optional<LayerTransform> EditorSession::displayedMaskPlacement(const Layer& layer) const {
    if (!layer.mask) return std::nullopt;
    const LayerMask& mask = *layer.mask;
    if (transformEdit_ && transformEdit_->group) {
        auto it = transformEdit_->group->originals.find(layer.id);
        if (it == transformEdit_->group->originals.end()) return mask.placement;
        return mask.placementMovingLayer(layer.transform, it->second.following(transformEdit_->group->box, transformEdit_->draft));
    }
    if (!transformEdit_ || transformEdit_->layerId != layer.id || transformEdit_->floating) return mask.placement;
    if (transformEdit_->mask) return transformEdit_->draft.samePlacement(layer.transform) ? std::nullopt : std::optional(transformEdit_->draft);
    return mask.placementMovingLayer(layer.transform, transformEdit_->draft);
}

LayerTransform EditorSession::editedTransform(const Layer& layer) const {
    if (transformEdit_ && transformEdit_->layerId == layer.id) return transformEdit_->draft;
    if (!transformEdit_ && layer.id == activeLayerId_ && transformsAsGroup()) { if (auto box = groupTransformBox()) return *box; }
    if (layer.id == activeLayerId_ && isMaskSelected_ && layer.mask && !layer.mask->linked) return layer.maskTransform();
    return layer.transform;
}

void EditorSession::setSnapGuides(std::vector<double> xs, std::vector<double> ys) {
    snapGuidesX = std::move(xs);
    snapGuidesY = std::move(ys);
}

std::optional<Uuid> EditorSession::layerAt(QPointF documentPoint) const {
    if (!document_) return std::nullopt;
    auto layers = renderLayers(document_->layers);
    for (auto it = layers.rbegin(); it != layers.rend(); ++it) {
        const Layer* layer = *it;
        if (!layer->asset || !layer->asset->image) continue;
        LayerTransform t = displayedTransform(*layer);
        if (!t.contains(toPoint(documentPoint))) continue;
        Point p = t.pixelToDocument(layer->asset->image->width(), layer->asset->image->height()).inverted().apply(toPoint(documentPoint));
        int x = int(std::floor(p.x)), y = int(std::floor(p.y));
        if (x < 0 || y < 0 || x >= layer->asset->image->width() || y >= layer->asset->image->height()) continue;
        if (layer->asset->image->pixel(x, y)[3] > 0) return layer->id;
    }
    return std::nullopt;
}

std::optional<Size> EditorSession::transformPixelSize() const {
    if (transformEdit_ && transformEdit_->group) return transformEdit_->group->box.size;
    if (!transformEdit_ && transformsAsGroup()) { auto box = groupTransformBox(); return box ? std::optional(box->size) : std::nullopt; }
    if (transformEdit_ && transformEdit_->mask) return std::nullopt;
    if (transformEdit_ && transformEdit_->floating) return Size(transformEdit_->floating->pixelWidth, transformEdit_->floating->pixelHeight);
    const Layer* active = activeLayer();
    if (!active || !active->asset || !active->asset->image) return std::nullopt;
    return Size(active->asset->image->width(), active->asset->image->height());
}

void EditorSession::redrawShape(Layer& layer) {
    if (!layer.isLiveShape() || !layer.asset) return;
    int w = std::max(1, int(std::lround(layer.transform.size.width))), h = std::max(1, int(std::lround(layer.transform.size.height)));
    if ((w == layer.asset->image->width() && h == layer.asset->image->height()) || (long long)w * h > Document::pixelBudget) return;
    auto image = shapeImage(layer.shape->kind, w, h, layer.shape->red, layer.shape->green, layer.shape->blue, layer.shape->cornerRadius);
    // A mask that follows the layer's pixel grid stays exactly where it is while that grid changes size.
    if (layer.mask && !layer.mask->placement) layer.mask->placement = layer.maskTransform();
    layer.asset = Asset::make(image, layer.name);
    layer.shapeImage = image;
}

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
    bool healing = tool_ == Tool::SpotHealing, cloning = tool_ == Tool::CloneStamp;
    // Spot Healing and Clone Stamp rework image pixels; they have nothing to do on a mask.
    if ((healing || cloning) && mask) return false;
    std::optional<CloneSource> clone;
    if (cloning) {
        if (!cloneSource) { emit error(tr("Alt-click where Clone Stamp should copy from first.")); return false; }
        QPointF offset = cloneAligned && cloneOffset ? *cloneOffset : QPointF(std::round(cloneSource->x() - documentPoint.x()), std::round(cloneSource->y() - documentPoint.y()));
        std::shared_ptr<Image> sample;
        if (cloneSampleAll) sample = renderFlattened(*document_);
        else if (layer->asset && layer->asset->image) {
            Document single(document_->width, document_->height);
            Layer copy = *layer;
            copy.parentId.reset(); copy.visible = true; copy.opacity = 1; copy.blendMode = BlendMode::Normal; copy.mask.reset(); copy.maskSourceId.reset();
            single.layers = {copy};
            sample = renderFlattened(single);
        } else sample = std::make_shared<Image>(document_->width, document_->height);
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
    else { settings.red = foregroundColor.redF(); settings.green = foregroundColor.greenF(); settings.blue = foregroundColor.blueF(); }
    const GrayImage* selection = document_->selection && document_->selection->coverage ? document_->selection->coverage.get() : nullptr;
    if (document_->selection && !selection) return false; // an explicit empty selection: touch nothing
    stroke_ = std::make_unique<BrushStroke>(*layer, mask, settings, document_->size(), selection);
    if (!stroke_->isValid()) { emit error(QString::fromStdString(stroke_->error())); stroke_.reset(); return false; }
    strokeLayerId_ = layer->id;
    strokeMask_ = mask;
    if (clone) stroke_->setClone(*clone);
    if (straightFromLast && lastBrushPoint_) stroke_->append(toPoint(*lastBrushPoint_));
    stroke_->append(toPoint(documentPoint));
    lastBrushPoint_ = documentPoint;
    emit documentChanged(toQRect(stroke_->takeDirtyRect()));
    return true;
}

void EditorSession::continueBrush(QPointF documentPoint) {
    if (!stroke_) return;
    stroke_->append(toPoint(documentPoint));
    lastBrushPoint_ = documentPoint;
    Rect dirty = stroke_->takeDirtyRect();
    if (!dirty.isEmpty()) emit documentChanged(toQRect(dirty));
}

std::unique_ptr<BrushStroke> EditorSession::makeRasterEdit(const Layer& layer, bool mask, const BrushSettings& settings) const {
    const GrayImage* selection = document_->selection && document_->selection->coverage ? document_->selection->coverage.get() : nullptr;
    if (document_->selection && !selection) return nullptr; // an explicit empty selection: touch nothing
    auto stroke = std::make_unique<BrushStroke>(layer, mask, settings, document_->size(), selection);
    if (!stroke->isValid()) return nullptr;
    return stroke;
}

void EditorSession::commitRasterEdit(BrushStroke& stroke, const Uuid& layerId, bool mask, const QString& name) {
    Layer* layer = document_->find(layerId);
    if (!layer || !stroke.touched()) { emit documentChanged({}); emit historyChanged(); return; }
    BrushStroke::Commit commit = stroke.commit();
    beginEdit(name);
    if (mask) {
        if (commit.mask && layer->mask) { layer->mask->asset = *commit.mask; layer->mask->placement = commit.maskPlacement; }
    } else if (commit.asset) {
        if (layer->mask && layer->mask->linked && !layer->mask->placement && layer->asset && !commit.transform.samePlacement(layer->transform)) layer->mask->placement = layer->transform;
        layer->asset = commit.asset;
        layer->transform = commit.transform;
        layer->shapeImage.reset();
    }
    endEdit();
    notifyDocument();
}

void EditorSession::endBrush() {
    if (!stroke_) return;
    std::unique_ptr<BrushStroke> stroke = std::move(stroke_);
    stroke->flush();
    QString name = strokeMask_ ? "Paint Mask" : tool_ == Tool::SpotHealing ? "Spot Healing" : tool_ == Tool::CloneStamp ? "Clone Stamp" : tool_ == Tool::Smudge ? "Blur" : (brushErase ? "Eraser" : "Brush Stroke");
    commitRasterEdit(*stroke, strokeLayerId_, strokeMask_, name);
}

void EditorSession::cancelBrush() {
    if (!stroke_) return;
    stroke_.reset();
    emit documentChanged({});
}

// ---- Opacity keys and brush steps ---------------------------------------------------

void EditorSession::typeOpacityDigit(int digit) {
    if (stroke_ || digit < 0 || digit > 9) return;
    bool brushLike = tool_ == Tool::Brush || tool_ == Tool::SpotHealing || tool_ == Tool::CloneStamp || tool_ == Tool::Smudge;
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
        for (auto& l : document_->layers) if (selectedLayerIds_.count(l.id) && !l.isGroup && l.opacity != value) targets.push_back(&l);
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
    if (isMaskSelected_) { emit error(tr("Smudge and Liquify work on a layer's pixels, not its mask.")); return false; }
    if (!effectiveVisibleIds(document_->layers).count(layer->id)) return false;
    // The layer as the canvas shows it, at document size.
    Document single(document_->width, document_->height);
    Layer copy = *layer;
    copy.parentId.reset(); copy.visible = true; copy.opacity = 1; copy.blendMode = BlendMode::Normal; copy.mask.reset(); copy.maskSourceId.reset();
    copy.transform = displayedTransform(*layer);
    single.layers = {copy};
    auto rendered = renderFlattened(single);
    if (blurMode == BlurToolMode::Blur) {
        // Blur paints a softened copy of the layer in place, through the tip.
        double sigma = std::min(30.0, std::max(1.5, brushSettings.diameter / 10));
        gaussianBlur(*rendered, sigma);
        BrushSettings settings = brushSettings;
        stroke_ = makeRasterEdit(*layer, false, settings);
        if (!stroke_) return false;
        stroke_->setClone(CloneSource{rendered, {0, 0}}, false);
        strokeLayerId_ = layer->id;
        strokeMask_ = false;
        stroke_->append(toPoint(documentPoint));
        emit documentChanged({});
        return true;
    }
    warp_ = std::make_unique<WarpStroke>(rendered, blurMode == BlurToolMode::Smudge ? WarpMode::Smudge : WarpMode::Liquify, brushSettings.diameter, brushSettings.hardness, brushSettings.opacity);
    warpLayerId_ = layer->id;
    warpTransform_ = copy.transform;
    warp_->append(toPoint(documentPoint));
    lastBrushPoint_ = documentPoint;
    emit documentChanged({});
    return true;
}

void EditorSession::continueWarp(QPointF documentPoint) {
    if (stroke_ && !warp_) { continueBrush(documentPoint); return; }
    if (!warp_) return;
    warp_->append(toPoint(documentPoint));
    lastBrushPoint_ = documentPoint;
    emit documentChanged({});
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
    stroke->setClone(CloneSource{std::make_shared<Image>(*warp->image()), {0, 0}}, true);
    for (auto& p : warp->points()) stroke->append(p);
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
        float start[4], end[4];
        if (gradient_->mask) {
            float f = fg.lightnessF() >= 0.5 ? 1 : 0, g = bg.lightnessF() >= 0.5 ? 1 : 0;
            start[0] = start[1] = start[2] = f; start[3] = 1;
            end[0] = end[1] = end[2] = gradientSettings.style == GradientStyle::ForegroundToBackground ? g : f;
            end[3] = gradientSettings.style == GradientStyle::ForegroundToBackground ? 1 : 0;
        } else {
            start[0] = fg.redF(); start[1] = fg.greenF(); start[2] = fg.blueF(); start[3] = 1;
            if (gradientSettings.style == GradientStyle::ForegroundToBackground) { end[0] = bg.redF(); end[1] = bg.greenF(); end[2] = bg.blueF(); end[3] = 1; }
            else { end[0] = fg.redF(); end[1] = fg.greenF(); end[2] = fg.blueF(); end[3] = 0; }
        }
        if (gradientSettings.reversed) for (int c = 0; c < 4; c++) std::swap(start[c], end[c]);
        gradient_->raster->fillGradientOver(gradientSettings.shape == GradientShape::Radial ? 1 : 0, toPoint(a), toPoint(b), start, end, gradientSettings.opacity);
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
    int result = content_fill(out->data(), size_t(out->stride()), coverage->data(), size_t(coverage->stride()), out->width(), out->height());
    if (result != 1) { if (errorText) *errorText = tr("Not enough unselected, opaque image pixels to synthesize a fill. Use a smaller selection with some surrounding image."); return false; }
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

void EditorSession::magicWand(QPointF documentPoint, int tolerance, bool contiguous, bool sampleAllLayers, SelectionMode mode) {
    if (!document_ || !canEditLayers()) return;
    int x = int(std::floor(documentPoint.x())), y = int(std::floor(documentPoint.y()));
    if (x < 0 || y < 0 || x >= document_->width || y >= document_->height) return;
    std::shared_ptr<Image> pixels;
    if (sampleAllLayers) pixels = renderFlattened(*document_);
    else {
        const Layer* layer = activeLayer();
        if (!layer || !layer->asset) return;
        Document single(document_->width, document_->height);
        Layer copy = *layer;
        copy.parentId.reset();
        copy.visible = true;
        single.layers = {copy};
        pixels = renderFlattened(single);
    }
    auto mask = std::make_shared<GrayImage>(document_->width, document_->height);
    long count = wand_mask(pixels->data(), size_t(document_->width), size_t(document_->height), size_t(pixels->stride()), size_t(x), size_t(y), 0, tolerance, contiguous ? 1 : 0, mask->data());
    if (count < 0) return;
    applySelectionShape(*mask, mode, "Magic Wand");
}

void EditorSession::fillSelection(const QColor& color) {
    if (!canEditLayers()) return;
    Layer* layer = activeLayerMutable();
    if (!layer || layer->isGroup || layer->adjustment) return;
    bool mask = isMaskSelected_ && layer->mask;
    const GrayImage* selection = document_->selection && document_->selection->coverage ? document_->selection->coverage.get() : nullptr;
    if (document_->selection && !selection) return;
    // A fill is a stroke covering the whole canvas: paint through the selection.
    BrushSettings settings;
    settings.diameter = 1;
    settings.red = color.redF(); settings.green = color.greenF(); settings.blue = color.blueF();
    if (mask) settings.maskValue = color.lightnessF() >= 0.5 ? 1 : 0;
    BrushStroke stroke(*layer, mask, settings, document_->size(), selection);
    if (!stroke.isValid()) return;
    // Direct fill over the working image rather than dabbing.
    auto working = mask ? nullptr : std::const_pointer_cast<Image>(stroke.previewImage());
    auto workingMask = mask ? std::const_pointer_cast<GrayImage>(stroke.previewMask()) : nullptr;
    int w = mask ? workingMask->width() : working->width(), h = mask ? workingMask->height() : working->height();
    Affine toDoc = stroke.paintTransform().pixelToDocument(w, h);
    for (int py = 0; py < h; py++) for (int px = 0; px < w; px++) {
        Point d = toDoc.apply({px + 0.5, py + 0.5});
        if (d.x < 0 || d.y < 0 || d.x >= document_->width || d.y >= document_->height) continue;
        double c = selection ? selection->at(std::min(int(d.x), selection->width() - 1), std::min(int(d.y), selection->height() - 1)) / 255.0 : 1.0;
        if (c <= 0) continue;
        if (mask) {
            uint8_t& v = workingMask->at(px, py);
            v = uint8_t(v * (1 - c) + settings.maskValue * 255 * c + 0.5);
        } else {
            uint8_t* p = working->pixel(px, py);
            p[0] = uint8_t(p[0] * (1 - c) + color.red() * c + 0.5);
            p[1] = uint8_t(p[1] * (1 - c) + color.green() * c + 0.5);
            p[2] = uint8_t(p[2] * (1 - c) + color.blue() * c + 0.5);
            p[3] = uint8_t(p[3] * (1 - c) + 255 * c + 0.5);
        }
    }
    beginEdit("Fill");
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
}

void EditorSession::clearSelectedPixelsNow(Layer& layer) {
    const GrayImage* selection = document_->selection && document_->selection->coverage ? document_->selection->coverage.get() : nullptr;
    if (!selection || !layer.asset || !layer.asset->image) return;
    const Image& src = *layer.asset->image;
    auto out = std::make_shared<Image>(src);
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
    if (!canEditLayers()) return;
    Layer* layer = activeLayerMutable();
    if (!layer || layer->isGroup || !layer->asset || !layer->asset->image) return;
    if (!document_->selection || !document_->selection->coverage) return;
    beginEdit("Clear");
    clearSelectedPixelsNow(*layer);
    endEdit();
    notifyDocument();
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

void EditorSession::selectionContract(int amount) {
    if (!document_ || !document_->selection || !document_->selection->coverage || amount <= 0 || amount > 500) return;
    setSelection(resizeSelection(*document_->selection, -amount), "Contract Selection");
}

// ---- Adjustment layers and pixel adjustments --------------------------------------------

void EditorSession::addAdjustmentLayer(AdjustmentKind kind) {
    if (!canEditLayers() || document_->layers.size() >= size_t(Document::maxLayers)) return;
    Layer layer(adjustmentKindName(kind), document_->size());
    AdjustmentSettings settings = AdjustmentSettings::defaults(kind);
    if (kind == AdjustmentKind::GradientMap) {
        settings.gradientMap.shadows = {foregroundColor.redF(), foregroundColor.greenF(), foregroundColor.blueF()};
        settings.gradientMap.highlights = {backgroundColor.redF(), backgroundColor.greenF(), backgroundColor.blueF()};
    }
    if (kind == AdjustmentKind::Grain) settings.grain.seed = uint32_t(std::random_device{}());
    layer.adjustment = settings.toLayerAdjustment();
    const Layer* active = activeLayer();
    layer.parentId = active && active->isGroup ? activeLayerId_ : (active ? active->parentId : std::nullopt);
    int index = activeLayerId_ ? document_->indexOf(*activeLayerId_) + 1 : int(document_->layers.size());
    beginEdit(QStringLiteral("New %1 Adjustment").arg(adjustmentKindName(kind)));
    document_->layers.insert(document_->layers.begin() + index, layer);
    if (layer.parentId) collapsedGroupIds.erase(*layer.parentId);
    setActiveLayer(layer.id);
    endEdit();
    notifyDocument();
}

void EditorSession::beginAdjustmentEdit() {
    if (adjustmentEditing_ || !document_) return;
    adjustmentEditing_ = true;
    beginEdit("Adjustment");
}

void EditorSession::setAdjustment(const Uuid& id, const AdjustmentSettings& settings) {
    if (!document_ || !settings.isValid()) return;
    Layer* layer = document_->find(id);
    if (!layer || !layer->adjustment) return;
    bool standalone = !adjustmentEditing_;
    if (standalone) beginEdit("Adjustment");
    layer->adjustment = settings.toLayerAdjustment();
    if (standalone) { endEdit(); notifyDocument(); }
    else emit documentChanged({});
}

void EditorSession::endAdjustmentEdit() {
    if (!adjustmentEditing_) return;
    adjustmentEditing_ = false;
    endEdit();
    notifyDocument();
}

std::optional<AdjustmentSettings> EditorSession::adjustmentSettings(const Uuid& id) const {
    if (!document_) return std::nullopt;
    const Layer* layer = document_->find(id);
    if (!layer || !layer->adjustment) return std::nullopt;
    AdjustmentSettings settings;
    if (!AdjustmentSettings::parse(layer->adjustment->json, settings)) return std::nullopt;
    return settings;
}

bool EditorSession::canAdjustPixels() const {
    if (!canEditLayers()) return false;
    const Layer* layer = activeLayer();
    if (!layer || layer->isGroup || layer->adjustment || !layer->asset || !layer->asset->image || isMaskSelected_) return false;
    if (!effectiveVisibleIds(document_->layers).count(layer->id)) return false;
    if (document_->selection && document_->selection->isEmpty()) return false;
    return selectedLayerIds_.size() == 1;
}

void EditorSession::setPixelPreview(std::shared_ptr<const Image> image, std::optional<LayerTransform> transform) {
    previewImage_ = std::move(image);
    previewTransform_ = transform;
    emit documentChanged({});
}

void EditorSession::clearPixelPreview() {
    if (!previewImage_) return;
    previewImage_.reset();
    previewTransform_.reset();
    emit documentChanged({});
}

std::shared_ptr<const Image> EditorSession::adjustmentSource(int margin, LayerTransform& transform) const {
    const Layer* layer = activeLayer();
    if (!layer || !layer->asset || !layer->asset->image) return nullptr;
    if (margin <= 0) { transform = layer->transform; return layer->asset->image; }
    return growImage(*layer->asset->image, layer->transform, margin, transform);
}

std::shared_ptr<GrayImage> EditorSession::selectionOnGrid(const LayerTransform& transform, int width, int height) const {
    if (!document_ || !document_->selection || !document_->selection->coverage) return nullptr;
    return selectionInGrid(*document_->selection->coverage, transform.pixelToDocument(width, height), width, height);
}

void EditorSession::commitPixels(std::shared_ptr<const Image> image, const LayerTransform& transform, const QString& name) {
    clearPixelPreview();
    Layer* layer = activeLayerMutable();
    if (!layer || !image) return;
    beginEdit(name);
    // A mask covering the old grid stays where it was when the layer grows.
    if (layer->mask && !layer->mask->placement && !transform.samePlacement(layer->transform)) layer->mask->placement = layer->transform;
    layer->asset = Asset::make(image, layer->name);
    layer->transform = transform;
    layer->shapeImage.reset();
    endEdit();
    notifyDocument();
}

void EditorSession::invertActive() {
    if (!canEditLayers()) return;
    Layer* layer = activeLayerMutable();
    if (!layer || layer->isGroup) return;
    if (isMaskSelected_ && layer->mask) {
        auto out = std::make_shared<GrayImage>(*layer->mask->asset.image);
        applyInvert(*out);
        if (document_->selection && document_->selection->coverage && out->width() > 1) {
            auto coverage = selectionInGrid(*document_->selection->coverage, layer->maskTransform().pixelToDocument(out->width(), out->height()), out->width(), out->height());
            blendThroughCoverage(*out, *layer->mask->asset.image, *coverage);
        }
        beginEdit("Invert");
        layer->mask->asset = MaskAsset::make(out);
        endEdit();
        notifyDocument();
        return;
    }
    if (!layer->asset || !layer->asset->image) return;
    auto out = std::make_shared<Image>(*layer->asset->image);
    applyInvert(*out);
    if (auto coverage = selectionOnGrid(layer->transform, out->width(), out->height())) blendThroughCoverage(*out, *layer->asset->image, *coverage);
    commitPixels(out, layer->transform, "Invert");
}

std::array<std::vector<double>, 4> EditorSession::activeHistogram() const {
    const Layer* layer = activeLayer();
    if (!layer || !layer->asset || !layer->asset->image) return {};
    auto coverage = selectionOnGrid(layer->transform, layer->asset->image->width(), layer->asset->image->height());
    return levelsHistogram(*layer->asset->image, coverage.get());
}

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

void EditorSession::resizeImage(int width, int height, double resolution) {
    if (!canEditLayers() || !Document::validDimension(width) || !Document::validDimension(height)) return;
    double sx = double(width) / document_->width, sy = double(height) / document_->height;
    beginEdit("Image Size");
    Document doc = *document_;
    doc.width = width;
    doc.height = height;
    doc.resolution = resolution;
    // Layers keep their full-resolution pixels; only their placement scales.
    auto scaleTransform = [&](LayerTransform t) {
        t.origin = {t.origin.x * sx, t.origin.y * sy};
        t.size = {std::max(1.0, t.size.width * sx), std::max(1.0, t.size.height * sy)};
        return t;
    };
    for (auto& l : doc.layers) {
        l.transform = scaleTransform(l.transform);
        if (l.mask && l.mask->placement) l.mask->placement = scaleTransform(*l.mask->placement);
    }
    doc.selection.reset();
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
                    DistortCache cache{target->second, target->first, layer->asset->image, maskImage, warpImage(*layer->asset->image, target->first, target->second, 2048), nullptr};
                    if (cache.image && maskImage && !layer->mask->placement && layer->mask->linked) {
                        auto wm = warpMask(*maskImage, target->first, target->second, 0, 2048);
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
    if (previewImage_ && activeLayerId_) {
        LayerOverride& o = overrides[*activeLayerId_];
        o.image = previewImage_;
        if (previewTransform_) o.transform = *previewTransform_;
        const Layer* layer = document_ ? document_->find(*activeLayerId_) : nullptr;
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
