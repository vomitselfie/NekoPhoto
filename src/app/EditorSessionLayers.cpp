// EditorSession: Layers: adding, grouping, clipping, masks, merging, ordering and their properties.
#include "EditorSession.h"
#include "compositor/filters.h"
#include <algorithm>
#include <map>
#include <set>

using namespace compositor;

namespace app {

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

void EditorSession::addBlankLayer(bool below) {
    if (!canEditLayers() || document_->layers.size() >= size_t(Document::maxLayers)) return;
    Layer layer(nextLayerName(document_->layers, "Layer"), document_->size());
    const Layer* active = activeLayer();
    bool intoGroup = active && active->isGroup && !below;
    layer.parentId = intoGroup ? activeLayerId_ : (active ? active->parentId : std::nullopt);
    if (layer.parentId) collapsedGroupIds.erase(*layer.parentId);
    // Layers are stored bottom to top with a folder's contents after it, so "below" is the active index itself.
    int insertion = activeLayerId_ ? document_->indexOf(*activeLayerId_) + (below ? 0 : 1) : int(document_->layers.size());
    if (intoGroup) {
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

std::optional<EditorSession::MergePlan> EditorSession::mergePlan() const {
    if (!canEditLayers()) return std::nullopt;
    const Layer* active = activeLayer();
    if (!active) return std::nullopt;
    const auto& layers = document_->layers;
    MergePlan plan;
    if (selectedLayerIds_.size() > 1) {
        std::set<Uuid> picked = selectedLayerIds_;
        for (auto& id : selectedLayerIds_) for (auto& d : descendantIds(layers, id)) picked.insert(d);
        const Layer* top = nullptr;
        bool anyPixels = false;
        for (auto& l : layers) if (picked.count(l.id)) { plan.ids.push_back(l.id); if (!l.isGroup) anyPixels = true; if (selectedLayerIds_.count(l.id)) top = &l; }
        if (!anyPixels || !top) return std::nullopt;
        plan.removed = picked; plan.name = top->name; plan.parent = top->parentId; plan.anchor = top->id; plan.action = "Merge Layers";
        return plan;
    }
    if (active->isGroup) {
        std::set<Uuid> inside = descendantIds(layers, active->id);
        bool anyPixels = false;
        for (auto& l : layers) if (inside.count(l.id) && !l.isGroup) anyPixels = true;
        if (!anyPixels) return std::nullopt;
        for (auto& l : layers) if (inside.count(l.id) || l.id == active->id) { plan.ids.push_back(l.id); plan.removed.insert(l.id); }
        plan.name = active->name; plan.parent = active->parentId; plan.anchor = active->id; plan.action = "Merge Group";
        return plan;
    }
    int index = document_->indexOf(active->id);
    const Layer* below = nullptr;
    for (int i = index - 1; i >= 0; i--) if (layers[size_t(i)].parentId == active->parentId) { below = &layers[size_t(i)]; break; }
    if (!below || below->isGroup) return std::nullopt;
    plan.ids = {below->id, active->id}; plan.removed = {below->id, active->id};
    plan.name = below->name; plan.parent = active->parentId; plan.anchor = active->id; plan.action = "Merge Down";
    return plan;
}

bool EditorSession::canMergeLayers() const { return mergePlan().has_value(); }
QString EditorSession::mergeTitle() const { auto plan = mergePlan(); return plan ? plan->action : QStringLiteral("Merge Down"); }

void EditorSession::mergeLayers() {
    commitTransform();
    auto plan = mergePlan();
    if (!plan) return;
    const auto& layers = document_->layers;
    std::set<Uuid> kept(plan->ids.begin(), plan->ids.end());
    // Only the merged layers, cut loose from anything outside the merge, composited as the canvas shows them.
    Document flat(document_->width, document_->height);
    for (auto& l : layers) {
        if (!kept.count(l.id)) continue;
        Layer copy = l;
        if (copy.parentId && !kept.count(*copy.parentId)) copy.parentId.reset();
        if (copy.maskSourceId && !kept.count(*copy.maskSourceId)) copy.maskSourceId.reset();
        flat.layers.push_back(copy);
    }
    auto full = renderFlattened(flat);
    LayerTransform canvas(Point(0, 0), document_->size());
    LayerTransform placed;
    auto trimmed = trimToPixels(*full, canvas, placed);
    if (alphaBounds(*trimmed).isEmpty()) { emit error(tr("Nothing to merge: the layers have no visible pixels.")); return; }
    Layer merged(Asset::make(trimmed, plan->name), placed.origin);
    merged.transform = placed;
    merged.name = plan->name;
    merged.parentId = plan->parent;
    std::vector<Layer> next;
    for (auto& l : layers) if (!plan->removed.count(l.id)) next.push_back(l);
    // Layers clipped to anything that was merged now clip to the result.
    for (auto& l : next) if (l.maskSourceId && plan->removed.count(*l.maskSourceId)) l.maskSourceId = merged.id;
    int slot = document_->indexOf(plan->anchor);
    int removedBefore = 0;
    for (int i = 0; i < slot; i++) if (plan->removed.count(layers[size_t(i)].id)) removedBefore++;
    int insertion = std::clamp(slot - removedBefore, 0, int(next.size()));
    next.insert(next.begin() + insertion, merged);
    if (!validateHierarchy(next)) return;
    endOpacityEdit();
    beginEdit(plan->action);
    document_->layers = next;
    setActiveLayer(merged.id);
    endEdit();
    notifyDocument();
}

void EditorSession::mergeDown() { mergeLayers(); }

void EditorSession::moveActiveLayerOutOfGroup() {
    const Layer* layer = activeLayer();
    if (!layer || !layer->parentId) return;
    const Layer* group = document_->find(*layer->parentId);
    if (!group) return;
    placeLayer(layer->id, group->parentId, group->id, false);
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

void EditorSession::previewBlendMode(std::optional<BlendMode> mode) {
    if (blendPreview_ == mode) return;
    blendPreview_ = mode;
    emit documentChanged({});
}

void EditorSession::setLayerBlendMode(BlendMode mode) {
    blendPreview_.reset();
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

} // namespace app
