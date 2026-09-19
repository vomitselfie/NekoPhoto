#include "compositor/document.h"
#include <algorithm>
#include <cmath>
#include <functional>
#include <map>

namespace compositor {

const char* blendModeName(BlendMode mode) {
    switch (mode) {
    case BlendMode::Normal: return "Normal";
    case BlendMode::Multiply: return "Multiply";
    case BlendMode::Screen: return "Screen";
    case BlendMode::Overlay: return "Overlay";
    case BlendMode::Darken: return "Darken";
    case BlendMode::Lighten: return "Lighten";
    case BlendMode::Difference: return "Difference";
    case BlendMode::ColorDodge: return "Color Dodge";
    case BlendMode::ColorBurn: return "Color Burn";
    case BlendMode::Hue: return "Hue";
    case BlendMode::Saturation: return "Saturation";
    case BlendMode::Color: return "Color";
    case BlendMode::Luminosity: return "Luminosity";
    }
    return "Normal";
}

bool parseBlendMode(const std::string& name, BlendMode& out) {
    for (int i = 0; i < blendModeCount; i++)
        if (name == blendModeName(BlendMode(i))) { out = BlendMode(i); return true; }
    return false;
}

const char* adjustmentKindName(AdjustmentKind kind) {
    switch (kind) {
    case AdjustmentKind::HueSaturation: return "Hue/Saturation";
    case AdjustmentKind::Levels: return "Levels";
    case AdjustmentKind::Curves: return "Curves";
    case AdjustmentKind::Exposure: return "Exposure";
    case AdjustmentKind::GradientMap: return "Gradient Map";
    case AdjustmentKind::Grain: return "Grain";
    }
    return "Levels";
}

bool parseAdjustmentKind(const std::string& name, AdjustmentKind& out) {
    for (int i = 0; i < 6; i++)
        if (name == adjustmentKindName(AdjustmentKind(i))) { out = AdjustmentKind(i); return true; }
    return false;
}

Asset Asset::make(ImagePtr image, std::string name) {
    Asset asset;
    asset.thumbnail = image ? makeThumbnail(*image) : nullptr;
    asset.image = std::move(image);
    asset.name = std::move(name);
    return asset;
}

MaskAsset MaskAsset::make(GrayPtr image) {
    MaskAsset asset;
    asset.thumbnail = image ? makeGrayThumbnail(*image) : nullptr;
    asset.image = std::move(image);
    return asset;
}

MaskAsset MaskAsset::solid(bool revealing) {
    return make(std::make_shared<GrayImage>(1, 1, revealing ? 255 : 0));
}

uint8_t LayerMask::background(const GrayImage& thumbnail) {
    int w = thumbnail.width(), h = thumbnail.height();
    if (w <= 0 || h <= 0) return 255;
    long total = 0, count = 0;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            if (y == 0 || y == h - 1 || x == 0 || x == w - 1) { total += thumbnail.at(x, y); count++; }
    return total * 2 >= count * 255 ? 255 : 0;
}

std::optional<LayerTransform> LayerMask::placementMovingLayer(const LayerTransform& from, const LayerTransform& to) const {
    if (!asset.image || (asset.image->width() <= 1 && asset.image->height() <= 1)) return std::nullopt;
    std::optional<LayerTransform> moved;
    if (linked) { if (placement) moved = placement->following(from, to); }
    else moved = placement ? *placement : from;
    if (!moved) return std::nullopt;
    return moved->samePlacement(to) ? std::nullopt : moved;
}

Layer::Layer(Asset asset_, Point origin) : id(makeUuid()), asset(std::move(asset_)) {
    transform = LayerTransform(origin, Size(double(asset->image->width()), double(asset->image->height())));
    name = asset->name;
}

Layer::Layer(std::string name_, Size blankSize) : id(makeUuid()), transform(Point(0, 0), blankSize), name(std::move(name_)) {}

bool Layer::operator==(const Layer& o) const {
    return id == o.id && name == o.name && visible == o.visible && transform == o.transform
        && (asset ? asset->image : nullptr) == (o.asset ? o.asset->image : nullptr)
        && parentId == o.parentId && isGroup == o.isGroup && opacity == o.opacity && blendMode == o.blendMode
        && mask == o.mask && maskSourceId == o.maskSourceId && adjustment == o.adjustment
        && shape == o.shape && shapeImage == o.shapeImage;
}

int Layer::pixelWidth() const { return asset && asset->image ? asset->image->width() : std::max(1, int(std::lround(transform.size.width))); }
int Layer::pixelHeight() const { return asset && asset->image ? asset->image->height() : std::max(1, int(std::lround(transform.size.height))); }

bool Selection::isEmpty() const {
    if (!coverage) return true;
    return nonzeroBounds(*coverage).isEmpty();
}

Rect Selection::bounds() const {
    if (!coverage) return {};
    auto b = nonzeroBounds(*coverage);
    if (b.isEmpty()) return {};
    return {double(b.x0), double(b.y0), double(b.x1 - b.x0), double(b.y1 - b.y0)};
}

Document::Document(int width_, int height_) : id(makeUuid()), width(width_), height(height_) {}

bool Document::operator==(const Document& o) const {
    return id == o.id && width == o.width && height == o.height && resolution == o.resolution && layers == o.layers && selection == o.selection;
}

const Layer* Document::find(const Uuid& lid) const {
    for (auto& l : layers) if (l.id == lid) return &l;
    return nullptr;
}

Layer* Document::find(const Uuid& lid) {
    for (auto& l : layers) if (l.id == lid) return &l;
    return nullptr;
}

int Document::indexOf(const Uuid& lid) const {
    for (size_t i = 0; i < layers.size(); i++) if (layers[i].id == lid) return int(i);
    return -1;
}

std::vector<HierarchyEntry> hierarchyEntries(const std::vector<Layer>& layers, bool topFirst, const std::set<Uuid>* collapsed) {
    std::map<std::optional<Uuid>, std::vector<const Layer*>> children;
    for (auto& l : layers) children[l.parentId].push_back(&l);
    std::vector<HierarchyEntry> result;
    std::function<void(const std::optional<Uuid>&, int, bool)> visit = [&](const std::optional<Uuid>& parent, int depth, bool visible) {
        if (depth > 64) return;
        auto it = children.find(parent);
        if (it == children.end()) return;
        std::vector<const Layer*> siblings = it->second;
        if (topFirst) std::reverse(siblings.begin(), siblings.end());
        for (const Layer* layer : siblings) {
            bool effective = visible && layer->visible;
            result.push_back({layer, depth, effective});
            if (layer->isGroup && !(collapsed && collapsed->count(layer->id)))
                visit(layer->id, depth + 1, effective);
        }
    };
    visit(std::nullopt, 0, true);
    return result;
}

std::vector<const Layer*> renderLayers(const std::vector<Layer>& layers) {
    std::vector<const Layer*> result;
    for (auto& e : hierarchyEntries(layers)) if (e.visible && !e.layer->isGroup) result.push_back(e.layer);
    return result;
}

std::set<Uuid> effectiveVisibleIds(const std::vector<Layer>& layers) {
    std::set<Uuid> result;
    for (auto& e : hierarchyEntries(layers)) if (e.visible) result.insert(e.layer->id);
    return result;
}

std::set<Uuid> descendantIds(const std::vector<Layer>& layers, const Uuid& id) {
    std::map<std::optional<Uuid>, std::vector<const Layer*>> children;
    for (auto& l : layers) children[l.parentId].push_back(&l);
    std::set<Uuid> result;
    std::vector<Uuid> pending{id};
    while (!pending.empty()) {
        Uuid parent = pending.back();
        pending.pop_back();
        for (const Layer* child : children[parent]) if (result.insert(child->id).second) pending.push_back(child->id);
    }
    return result;
}

bool validateHierarchy(const std::vector<Layer>& layers, std::string* error) {
    std::map<Uuid, const Layer*> byId;
    for (auto& l : layers) {
        if (!byId.emplace(l.id, &l).second) { if (error) *error = "duplicate layer id"; return false; }
        if (l.isGroup && l.asset) { if (error) *error = "a group carries an image"; return false; }
    }
    for (auto& l : layers) {
        std::set<Uuid> seen{l.id};
        std::optional<Uuid> parent = l.parentId;
        while (parent) {
            if (seen.size() > 64 || !seen.insert(*parent).second) { if (error) *error = "hierarchy cycle or too deep"; return false; }
            auto it = byId.find(*parent);
            if (it == byId.end() || !it->second->isGroup) { if (error) *error = "missing or non-group parent"; return false; }
            parent = it->second->parentId;
        }
        if (l.isGroup && seen.size() > 64) { if (error) *error = "hierarchy too deep"; return false; }
    }
    return true;
}

bool validateClipping(const std::vector<Layer>& layers, std::string* error) {
    std::map<Uuid, const Layer*> byId;
    for (auto& l : layers) if (!byId.emplace(l.id, &l).second) { if (error) *error = "duplicate layer id"; return false; }
    for (auto& l : layers) {
        std::set<Uuid> path;
        std::optional<Uuid> current = l.id;
        while (current) {
            if (path.size() >= 256 || !path.insert(*current).second) { if (error) *error = "clipping cycle"; return false; }
            auto it = byId.find(*current);
            if (it == byId.end()) { if (error) *error = "missing clipping source"; return false; }
            const Layer* record = it->second;
            if (record->maskSourceId) {
                auto src = byId.find(*record->maskSourceId);
                if (record->isGroup || src == byId.end() || src->second->isGroup || src->second->adjustment) {
                    if (error) *error = "invalid clipping source";
                    return false;
                }
            }
            current = record->maskSourceId;
        }
    }
    return true;
}

std::string nextLayerName(const std::vector<Layer>& layers, const std::string& prefix) {
    std::set<std::string> names;
    for (auto& l : layers) names.insert(l.name);
    for (int n = 1;; n++) {
        std::string candidate = prefix + " " + std::to_string(n);
        if (!names.count(candidate)) return candidate;
    }
}

} // namespace compositor
