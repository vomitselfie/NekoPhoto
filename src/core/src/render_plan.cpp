// The depth-agnostic half of the document renderer (render_plan.h): what is drawn, in which order, and how the
// layers group, clip and cache. No pixels here.
#include "render_plan.h"
#include "compositor/layerstyle.h"
#include <algorithm>
#include <cstdint>

namespace compositor {

bool RenderPlan::within(const Layer& layer, const Uuid& group) const {
    int depth = 0;
    for (auto p = layer.parentId; p && depth < 64; depth++) {
        if (*p == group) return true;
        auto it = byId.find(*p);
        if (it == byId.end()) break;
        p = it->second->parentId;
    }
    return false;
}

void RenderPlan::prepareGroupStyles() {
    for (const Layer& g : document.layers) {
        // A folder needs handling around its children when it has a style, isolates them (anything but Pass
        // Through), or fades them (Pass Through below full opacity).
        if (!g.isGroup || !g.visible || suppressedGroups.count(g.id)) continue;
        if (!layerStyleOf(g, document) && !isolates(g) && !fades(g)) continue;
        // An artboard's own entry (its background) belongs inside: it takes the folder's mode and opacity with its
        // layers instead of painting beneath the folder at full strength.
        size_t first = SIZE_MAX, last = 0;
        for (size_t i = 0; i < order.size(); i++) if (order[i] == &g || within(*order[i], g.id)) { first = std::min(first, i); last = i; }
        if (first == SIZE_MAX) continue;
        groupsOpen[first].push_back(&g);
        groupsClose[last].insert(groupsClose[last].begin(), &g);
    }
    // Outer folders open first: sort each opening list by depth.
    auto depth = [&](const Layer* l) { int d = 0; for (auto p = l->parentId; p && d < 64; d++) { auto it = byId.find(*p); if (it == byId.end()) break; p = it->second->parentId; } return d; };
    for (auto& [i, list] : groupsOpen) std::stable_sort(list.begin(), list.end(), [&](auto a, auto b) { return depth(a) < depth(b); });
    for (auto& [i, list] : groupsClose) std::stable_sort(list.begin(), list.end(), [&](auto a, auto b) { return depth(a) > depth(b); });
}

void RenderPlan::insertArtboards() {
    const std::set<Uuid> visible = effectiveVisibleIds(document.layers);
    for (size_t li = 0; li < document.layers.size(); li++) {
        const Layer& g = document.layers[li];
        if (!g.isGroup || !g.artboard || !visible.count(g.id)) continue;
        size_t at = SIZE_MAX;
        for (size_t i = 0; i < order.size(); i++) if (within(*order[i], g.id)) { at = i; break; }
        if (at == SIZE_MAX) {
            // Empty: after the layers that come before it in the document.
            at = 0;
            for (size_t i = 0; i < order.size(); i++)
                if (!order[i]->isGroup && document.indexOf(order[i]->id) < int(li)) at = i + 1;
        }
        order.insert(order.begin() + std::ptrdiff_t(at), &g);
    }
}

void RenderPlan::prepareStacks() {
    for (size_t index = 0; index < order.size(); index++) {
        const Layer* base = order[index];
        if (base->maskSourceId || base->adjustment) continue;
        std::vector<Uuid> children;
        for (size_t j = index + 1; j < order.size(); j++) {
            const Layer* child = order[j];
            if (!(child->maskSourceId && *child->maskSourceId == base->id && child->parentId == base->parentId)) break;
            children.push_back(child->id);
        }
        if (children.empty()) continue;
        stacks[base->id] = children;
        for (auto& c : children) stacked.insert(c);
    }
}

size_t RenderPlan::editedIndex() const {
    if (!overrides || overrides->size() != 1 || !groupsOpen.empty()) return SIZE_MAX;
    const Uuid& id = overrides->begin()->first;
    size_t index = SIZE_MAX;
    for (size_t i = 0; i < order.size(); i++) if (order[i]->id == id) index = i;
    if (index == SIZE_MAX) return SIZE_MAX;
    const Layer& l = *order[index];
    if (l.adjustment || l.maskSourceId || stacks.count(l.id) || stacked.count(l.id)) return SIZE_MAX;
    for (auto& other : document.layers) if (other.maskSourceId && *other.maskSourceId == id) return SIZE_MAX;
    return index;
}

size_t RenderPlan::adjustingIndex() const {
    if (!overrides || overrides->size() != 1 || !overrides->begin()->second.adjusting) return SIZE_MAX;
    const Uuid& id = overrides->begin()->first;
    size_t index = SIZE_MAX;
    for (size_t i = 0; i < order.size(); i++) if (order[i]->id == id) index = i;
    if (index == SIZE_MAX || !order[index]->adjustment || order[index]->isGroup) return SIZE_MAX;
    // A layer clipped to it, or taking its mask from it, would read its coverage wherever it is drawn.
    for (auto& other : document.layers) if (other.maskSourceId && *other.maskSourceId == id) return SIZE_MAX;
    return index;
}

size_t RenderPlan::resumeIndex(size_t adjusting) const {
    const Layer& layer = *order[adjusting];
    size_t at = adjusting;
    // Clipped: the stack is drawn as one, from its base.
    if (stacked.count(layer.id))
        for (size_t i = adjusting; i-- > 0;) {
            auto stack = stacks.find(order[i]->id);
            if (stack == stacks.end()) continue;
            if (std::find(stack->second.begin(), stack->second.end(), layer.id) != stack->second.end()) at = i;
            break;
        }
    // A styled folder around it draws its exterior effects from its contents (the layer among them) as it opens. Those
    // effects follow the contents' shape, which an adjustment does not change; the folder is drawn whole all the same.
    for (auto& [i, list] : groupsOpen) {
        if (i >= at) break;
        for (const Layer* g : list) if (layerStyleOf(*g, document) && within(layer, g->id)) { at = i; break; }
    }
    return at;
}

bool RenderPlan::plainAbove(const Layer& l) const {
    return !l.adjustment && !l.maskSourceId && !stacks.count(l.id) && !stacked.count(l.id) && blendOf(l) == BlendMode::Normal
        && !layerStyleOf(l, document)   // effects blend in their own modes
        && !hasBlendIf(l)                // gated by what is under it
        && !photoshopType(l);            // type blends with its own gamma
}

void RenderPlan::build() {
    for (auto& l : document.layers) byId[l.id] = &l;
    order = renderLayers(document.layers);
    insertArtboards();
    prepareStacks();
    prepareGroupStyles();
}

void RenderPlan::buildWithin(const RenderPlan& parent, const Uuid& group) {
    suppressedGroups = parent.suppressedGroups;
    suppressedGroups.insert(group);
    for (auto& l : document.layers) byId[l.id] = &l;
    for (const Layer* l : parent.order) if (parent.within(*l, group)) order.push_back(l);
    prepareStacks();
    prepareGroupStyles();
}

} // namespace compositor
