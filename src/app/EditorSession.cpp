#include "EditorSession.h"
#include "ImageConvert.h"
#include "compositor/project.h"
#include <QFileInfo>
#include <algorithm>
#include <map>

extern "C" {
#include "WandPixels.h"
}

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
    return document_ && !stroke_ && !transformEdit_;
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

bool EditorSession::canUndo() const { return document_ && !stroke_ && !transformEdit_ && history_.canUndo(); }
bool EditorSession::canRedo() const { return document_ && !stroke_ && !transformEdit_ && history_.canRedo(); }

void EditorSession::undo() {
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
    if (stroke_) return;
    if (id != activeLayerId_) commitTransform();
    setActiveLayer(id);
    const Layer* active = activeLayer();
    isMaskSelected_ = mask && active && active->mask;
    emit layersChanged();
    emit transformChanged();
}

void EditorSession::selectLayers(const std::set<Uuid>& ids, const std::optional<Uuid>& primary) {
    if (stroke_ || !document_) return;
    std::set<Uuid> valid;
    for (auto& id : ids) if (document_->find(id)) valid.insert(id);
    if (valid != selectedLayerIds_) commitTransform();
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

void EditorSession::finishDeleting(const std::vector<Uuid>& ids) {
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
    for (auto& l : kept) if (l.maskSourceId && removed.count(*l.maskSourceId)) l.maskSourceId.reset();
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

bool EditorSession::canTransform() const {
    if (!document_ || stroke_) return false;
    const Layer* active = activeLayer();
    if (!active || active->isGroup) return false;
    if (isMaskSelected_ && active->mask && !active->mask->linked) return true;
    return active->asset.has_value() && effectiveVisibleIds(document_->layers).count(active->id);
}

void EditorSession::beginTransform(bool persistent) {
    if (transformEdit_ || !canTransform()) return;
    const Layer* layer = activeLayer();
    tool_ = Tool::Move;
    bool maskAlone = isMaskSelected_ && layer->mask && !layer->mask->linked;
    transformEdit_ = TransformEdit{layer->id, maskAlone ? layer->maskTransform() : layer->transform, persistent, maskAlone};
    emit toolChanged();
    emit transformChanged();
}

void EditorSession::previewTransform(const LayerTransform& value) {
    if (!transformEdit_ || !value.isValid()) return;
    transformEdit_->draft = value;
    emit documentChanged({});
    emit transformChanged();
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

void EditorSession::commitTransform() {
    snapGuidesX.clear();
    snapGuidesY.clear();
    endOpacityEdit();
    if (!transformEdit_) return;
    TransformEdit edit = *transformEdit_;
    transformEdit_.reset();
    if (edit.mask) { commitMaskTransform(edit); notifyDocument(); emit transformChanged(); return; }
    Layer* layer = document_ ? document_->find(edit.layerId) : nullptr;
    if (layer && edit.draft.isValid() && !edit.draft.samePlacement(layer->transform)) {
        beginEdit("Transform Layer");
        if (layer->mask) layer->mask->placement = layer->mask->placementMovingLayer(layer->transform, edit.draft);
        layer->transform = edit.draft;
        endEdit();
    }
    notifyDocument();
    emit transformChanged();
}

void EditorSession::cancelTransform() {
    snapGuidesX.clear();
    snapGuidesY.clear();
    if (!transformEdit_) return;
    transformEdit_.reset();
    emit documentChanged({});
    emit transformChanged();
}

void EditorSession::nudgeLayer(double dx, double dy) {
    bool alreadyEditing = transformEdit_.has_value();
    if (!alreadyEditing) beginTransform(false);
    if (!transformEdit_) return;
    LayerTransform value = transformEdit_->draft;
    value.origin.x += dx;
    value.origin.y += dy;
    previewTransform(value);
    if (!alreadyEditing) commitTransform();
}

LayerTransform EditorSession::displayedTransform(const Layer& layer) const {
    if (transformEdit_ && !transformEdit_->mask && transformEdit_->layerId == layer.id) return transformEdit_->draft;
    return layer.transform;
}

LayerTransform EditorSession::editedTransform(const Layer& layer) const {
    if (transformEdit_ && transformEdit_->layerId == layer.id) return transformEdit_->draft;
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
    const Layer* active = activeLayer();
    if (!active) return std::nullopt;
    if (transformEdit_ && transformEdit_->mask) return std::nullopt;
    if (!active->asset || !active->asset->image) return std::nullopt;
    return Size(active->asset->image->width(), active->asset->image->height());
}

// ---- Brush ---------------------------------------------------------------------

bool EditorSession::beginBrush(QPointF documentPoint, bool straightFromLast) {
    if (!document_ || stroke_ || transformEdit_) return false;
    const Layer* layer = activeLayer();
    if (!layer || layer->isGroup || layer->adjustment) return false;
    bool mask = isMaskSelected_ && layer->mask;
    if (!mask && !effectiveVisibleIds(document_->layers).count(layer->id)) { emit error(tr("The active layer is hidden.")); return false; }
    BrushSettings settings = brushSettings;
    settings.erasing = brushErase && !mask;
    if (mask) settings.maskValue = (maskPaintWhite != brushErase) ? 1 : 0;
    else { settings.red = foregroundColor.redF(); settings.green = foregroundColor.greenF(); settings.blue = foregroundColor.blueF(); }
    const GrayImage* selection = document_->selection && document_->selection->coverage ? document_->selection->coverage.get() : nullptr;
    if (document_->selection && !selection) return false; // an explicit empty selection: touch nothing
    stroke_ = std::make_unique<BrushStroke>(*layer, mask, settings, document_->size(), selection);
    if (!stroke_->isValid()) { emit error(QString::fromStdString(stroke_->error())); stroke_.reset(); return false; }
    strokeLayerId_ = layer->id;
    strokeMask_ = mask;
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

void EditorSession::endBrush() {
    if (!stroke_) return;
    std::unique_ptr<BrushStroke> stroke = std::move(stroke_);
    stroke->flush();
    Layer* layer = document_->find(strokeLayerId_);
    if (!layer || !stroke->touched()) { emit documentChanged({}); emit historyChanged(); return; }
    BrushStroke::Commit commit = stroke->commit();
    beginEdit(strokeMask_ ? "Paint Mask" : (brushErase ? "Eraser" : "Brush Stroke"));
    if (strokeMask_) {
        if (commit.mask) { layer->mask->asset = *commit.mask; layer->mask->placement = commit.maskPlacement; }
    } else if (commit.asset) {
        if (layer->mask && layer->mask->linked && !layer->mask->placement) {
            // The mask kept covering the old pixel grid; a grown layer leaves it placed where it was.
            if (!commit.transform.samePlacement(layer->transform) && layer->asset) layer->mask->placement = layer->transform;
        }
        layer->asset = commit.asset;
        layer->transform = commit.transform;
        layer->shapeImage.reset();
    }
    endEdit();
    notifyDocument();
}

void EditorSession::cancelBrush() {
    if (!stroke_) return;
    stroke_.reset();
    emit documentChanged({});
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

void EditorSession::clearSelectionPixels() {
    if (!canEditLayers()) return;
    Layer* layer = activeLayerMutable();
    if (!layer || layer->isGroup || !layer->asset || !layer->asset->image) return;
    const GrayImage* selection = document_->selection && document_->selection->coverage ? document_->selection->coverage.get() : nullptr;
    if (!selection) return;
    const Image& src = *layer->asset->image;
    auto out = std::make_shared<Image>(src);
    Affine toDoc = layer->transform.pixelToDocument(src.width(), src.height());
    for (int y = 0; y < src.height(); y++) for (int x = 0; x < src.width(); x++) {
        Point d = toDoc.apply({x + 0.5, y + 0.5});
        if (d.x < 0 || d.y < 0 || d.x >= document_->width || d.y >= document_->height) continue;
        double c = selection->at(int(d.x), int(d.y)) / 255.0;
        if (c <= 0) continue;
        uint8_t* p = out->pixel(x, y);
        for (int k = 0; k < 4; k++) p[k] = uint8_t(p[k] * (1 - c) + 0.5);
    }
    beginEdit("Clear");
    layer->asset = Asset::make(out, layer->name);
    layer->shapeImage.reset();
    endEdit();
    notifyDocument();
}

void EditorSession::selectionExpand(int amount) {
    if (!document_ || !document_->selection || !document_->selection->coverage || amount <= 0 || amount > 500) return;
    setSelection(resizeSelection(*document_->selection, amount), "Expand Selection");
}

void EditorSession::selectionContract(int amount) {
    if (!document_ || !document_->selection || !document_->selection->coverage || amount <= 0 || amount > 500) return;
    setSelection(resizeSelection(*document_->selection, -amount), "Contract Selection");
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
    if (stroke_) return;
    if (tool != tool_) commitTransform();
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
    if (transformEdit_) {
        LayerOverride& o = overrides[transformEdit_->layerId];
        const Layer* layer = document_ ? document_->find(transformEdit_->layerId) : nullptr;
        if (transformEdit_->mask) {
            o.maskPlacement = std::optional<LayerTransform>(transformEdit_->draft);
        } else {
            o.transform = transformEdit_->draft;
            if (layer && layer->mask) o.maskPlacement = layer->mask->placementMovingLayer(layer->transform, transformEdit_->draft);
        }
    }
    if (stroke_) {
        LayerOverride& o = overrides[strokeLayerId_];
        if (strokeMask_) {
            o.maskImage = stroke_->previewMask();
            const Layer* layer = document_ ? document_->find(strokeLayerId_) : nullptr;
            if (layer && layer->mask && layer->mask->placement) o.maskPlacement = std::optional<LayerTransform>(stroke_->paintTransform());
        } else {
            o.image = stroke_->previewImage();
            o.transform = stroke_->paintTransform();
            // A mask covering the old grid stays where it was while the layer grows under the stroke.
            const Layer* layer = document_ ? document_->find(strokeLayerId_) : nullptr;
            if (layer && layer->mask && !layer->mask->placement && layer->asset) o.maskPlacement = std::optional<LayerTransform>(layer->transform);
        }
    }
    return overrides;
}

} // namespace app
