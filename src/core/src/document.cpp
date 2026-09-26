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
    case AdjustmentKind::Invert: return "Invert";
    case AdjustmentKind::BrightnessContrast: return "Brightness/Contrast";
    case AdjustmentKind::Posterize: return "Posterize";
    case AdjustmentKind::Threshold: return "Threshold";
    case AdjustmentKind::BlackWhite: return "Black & White";
    case AdjustmentKind::ColorBalance: return "Color Balance";
    case AdjustmentKind::Vibrance: return "Vibrance";
    case AdjustmentKind::PhotoFilter: return "Photo Filter";
    case AdjustmentKind::ChannelMixer: return "Channel Mixer";
    case AdjustmentKind::SelectiveColor: return "Selective Color";
    case AdjustmentKind::ColorLookup: return "Color Lookup";
    }
    return "Levels";
}

bool parseAdjustmentKind(const std::string& name, AdjustmentKind& out) {
    for (int i = 0; i < adjustmentKindCount; i++)
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
        && parentId == o.parentId && isGroup == o.isGroup && passThrough == o.passThrough && opacity == o.opacity && blendMode == o.blendMode
        && mask == o.mask && maskSourceId == o.maskSourceId && adjustment == o.adjustment
        && shape == o.shape && shapeImage == o.shapeImage && text == o.text && textImage == o.textImage && psdCarry == o.psdCarry
        && smartObject == o.smartObject && smartImage == o.smartImage;
}

int Layer::pixelWidth() const { return asset && asset->image ? asset->image->width() : std::max(1, int(std::lround(transform.size.width))); }
int Layer::pixelHeight() const { return asset && asset->image ? asset->image->height() : std::max(1, int(std::lround(transform.size.height))); }

const PixelBounds& Selection::pixelBounds() const {
    if (coverage.get() != boundsFor_) {
        bounds_ = coverage ? nonzeroBounds(*coverage) : PixelBounds{};
        boundsFor_ = coverage.get();
    }
    return bounds_;
}

bool Selection::isEmpty() const { return !coverage || pixelBounds().isEmpty(); }

Rect Selection::bounds() const {
    if (!coverage) return {};
    const PixelBounds& b = pixelBounds();
    if (b.isEmpty()) return {};
    return {double(b.x0), double(b.y0), double(b.x1 - b.x0), double(b.y1 - b.y0)};
}

Document::Document(int width_, int height_) : id(makeUuid()), width(width_), height(height_) {}

bool Document::operator==(const Document& o) const {
    return id == o.id && width == o.width && height == o.height && resolution == o.resolution && layers == o.layers && selection == o.selection && psdCarry == o.psdCarry && smartObjects == o.smartObjects;
}

long long Document::layerPixels() const {
    long long total = 0;
    for (const Layer& l : layers) if (l.asset && l.asset->image) total += (long long)l.asset->image->width() * l.asset->image->height();
    return total;
}

long long Document::maskPixels() const {
    long long total = 0;
    for (const Layer& l : layers) if (l.mask && l.mask->asset.image) total += (long long)l.mask->asset.image->width() * l.mask->asset.image->height();
    return total;
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

Rect changedArea(const Document& before, const Document& after) {
    const Rect whole = after.rect();
    if (before.width != after.width || before.height != after.height) return whole;
    std::map<Uuid, const Layer*> was, now;
    for (const Layer& l : before.layers) was[l.id] = &l;
    for (const Layer& l : after.layers) now[l.id] = &l;
    // Layers in both, in stacking order: a change of order changes how they cover each other anywhere.
    std::vector<Uuid> orderBefore, orderAfter;
    for (const Layer& l : before.layers) if (now.count(l.id)) orderBefore.push_back(l.id);
    for (const Layer& l : after.layers) if (was.count(l.id)) orderAfter.push_back(l.id);
    if (orderBefore != orderAfter) return whole;
    Rect area;
    auto add = [&](const Layer& l) { const Rect b = l.transform.bounds(); area = area.isEmpty() ? b : area.unionWith(b); };
    // A folder or an adjustment layer changes everything under or inside it: the whole canvas.
    auto reaches = [](const Layer& l) { return l.isGroup || l.adjustment.has_value(); };
    for (const auto& [id, layer] : was) {
        auto other = now.find(id);
        if (other == now.end()) { if (reaches(*layer)) return whole; add(*layer); continue; }
        if (*layer == *other->second) continue;
        if (reaches(*layer) || reaches(*other->second)) return whole;
        add(*layer);
        add(*other->second);
    }
    for (const auto& [id, layer] : now) {
        if (was.count(id)) continue;
        if (reaches(*layer)) return whole;
        add(*layer);
    }
    return area.isEmpty() ? Rect() : area.insetBy(-2, -2).intersection(whole);
}

} // namespace compositor

namespace compositor {

namespace {

std::u16string toUtf16(const std::string& s) {
    std::u16string out;
    for (size_t i = 0; i < s.size();) {
        uint32_t c = uint8_t(s[i]);
        const int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
        if (extra) c &= (0x3Fu >> extra);
        i++;
        for (int k = 0; k < extra && i < s.size(); k++, i++) c = (c << 6) | (uint8_t(s[i]) & 0x3Fu);
        if (c >= 0x10000) { c -= 0x10000; out.push_back(char16_t(0xD800 + (c >> 10))); out.push_back(char16_t(0xDC00 + (c & 0x3FF))); }
        else out.push_back(char16_t(c));
    }
    return out;
}

bool sameStyle(TextRun a, TextRun b) { a.length = b.length = 0; return a == b; }

} // namespace

int utf16Length(const std::string& utf8) { return int(toUtf16(utf8).size()); }

TextRun baseTextRun(const LayerText& t) {
    TextRun r;
    r.fontFamily = t.fontFamily; r.fontSize = t.fontSize; r.bold = t.bold; r.italic = t.italic;
    r.red = t.red; r.green = t.green; r.blue = t.blue; r.letterSpacing = t.letterSpacing;
    return r;
}

std::vector<TextRun> textRuns(const LayerText& text) {
    const int length = utf16Length(text.text);
    std::vector<TextRun> out;
    int covered = 0;
    for (const TextRun& r : text.runs) {
        if (covered >= length) break;
        TextRun k = r;
        k.length = std::clamp(r.length, 0, length - covered);
        if (k.length == 0) continue;
        covered += k.length;
        out.push_back(k);
    }
    if (out.empty()) { TextRun r = baseTextRun(text); r.length = length; out.push_back(r); }
    else if (covered < length) out.back().length += length - covered;
    return out;
}

std::vector<TextRun> adjustTextRuns(const std::vector<TextRun>& runs, const std::string& before, const std::string& after) {
    if (runs.empty()) return {};
    const std::u16string a = toUtf16(before), b = toUtf16(after);
    size_t prefix = 0;
    while (prefix < a.size() && prefix < b.size() && a[prefix] == b[prefix]) prefix++;
    size_t suffix = 0;
    while (suffix < a.size() - prefix && suffix < b.size() - prefix && a[a.size() - 1 - suffix] == b[b.size() - 1 - suffix]) suffix++;
    const int removedFrom = int(prefix), removedTo = int(a.size() - suffix), inserted = int(b.size() - prefix - suffix);
    std::vector<TextRun> out;
    int start = 0;
    bool placed = false;
    for (TextRun r : runs) {
        const int end = start + r.length;
        // The part of the run outside the removed stretch stays.
        const int kept = std::max(0, std::min(end, removedFrom) - start) + std::max(0, end - std::max(start, removedTo));
        int length = kept;
        // What was typed takes the style of the run it starts in (or the last run at the very end).
        if (!placed && removedFrom < end) { length += inserted; placed = true; }
        start = end;
        r.length = length;
        if (r.length > 0) {
            if (!out.empty() && sameStyle(out.back(), r)) out.back().length += r.length;
            else out.push_back(r);
        }
    }
    if (!placed && !out.empty()) out.back().length += inserted;
    return out;
}

void settleTextRuns(LayerText& text) {
    if (text.runs.empty()) return;
    std::vector<TextRun> merged;
    for (const TextRun& r : text.runs) {
        if (r.length <= 0) continue;
        if (!merged.empty() && sameStyle(merged.back(), r)) merged.back().length += r.length;
        else merged.push_back(r);
    }
    text.runs = std::move(merged);
    if (text.runs.empty()) return;
    const TextRun& first = text.runs.front();
    text.fontFamily = first.fontFamily; text.fontSize = first.fontSize; text.bold = first.bold; text.italic = first.italic;
    text.red = first.red; text.green = first.green; text.blue = first.blue; text.letterSpacing = first.letterSpacing;
    // One run that the plain fields say in full is no runs at all.
    if (text.runs.size() == 1 && first.weight == 0 && first.baselineShift == 0 && first.caps == TextRun::Caps::Normal && !first.underline && !first.strikethrough) text.runs.clear();
}

LayerText carryTextEdit(const LayerText& before, LayerText after) {
    if (before.runs.empty() || after.runs != before.runs) return after;
    std::vector<TextRun> runs = adjustTextRuns(before.runs, before.text, after.text);
    const double scale = before.fontSize > 0 ? after.fontSize / before.fontSize : 1;
    for (TextRun& r : runs) {
        if (after.fontSize != before.fontSize) { r.fontSize = std::max(1.0, r.fontSize * scale); r.leading *= scale; }
        if (after.lineSpacing != before.lineSpacing && before.lineSpacing > 0) r.leading *= after.lineSpacing / before.lineSpacing;
        if (after.fontFamily != before.fontFamily) r.fontFamily = after.fontFamily;
        if (after.bold != before.bold) { r.bold = after.bold; r.weight = 0; }
        if (after.italic != before.italic) r.italic = after.italic;
        if (after.red != before.red || after.green != before.green || after.blue != before.blue) { r.red = after.red; r.green = after.green; r.blue = after.blue; }
        if (after.letterSpacing != before.letterSpacing) r.letterSpacing = after.letterSpacing;
    }
    after.runs = std::move(runs);
    settleTextRuns(after);
    return after;
}

} // namespace compositor
