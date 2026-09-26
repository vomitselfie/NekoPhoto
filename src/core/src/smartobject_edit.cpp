// Making and changing smart objects. The semantics follow Photoshop 2026 as Patchy captured them (MIT; its
// docs/smart-object-editing.md): Convert makes a child document of the selected layers (canvas = their bounds)
// and places it where they were, named after the topmost; Place is 1:1 centred, scaled to fit; Replace (and an
// Edit Contents commit) gives the source a fresh id and rebuilds every instance about its own centre, keeping the
// instance's scale; Rasterize keeps the pixels.
#include "compositor/smartobject_edit.h"
#include "compositor/smartfilter.h"
#include "compositor/png.h"
#include "compositor/psd.h"
#include "compositor/psd_writer.h"
#include "compositor/render.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <set>

namespace compositor {

namespace {

std::string lower(std::string s) { for (auto& c : s) c = char(std::tolower(static_cast<unsigned char>(c))); return s; }

std::array<double, 8> quadOf(const LayerTransform& t, int w, int h) {
    std::array<double, 8> q{};
    const double corners[4][2] = {{0, 0}, {double(w), 0}, {double(w), double(h)}, {0, double(h)}};
    for (int i = 0; i < 4; i++) { const Point p = mapThroughTransform(t, w, h, corners[i][0], corners[i][1]); q[size_t(i * 2)] = p.x; q[size_t(i * 2 + 1)] = p.y; }
    return q;
}

} // namespace

std::string smartObjectFileType(const std::string& fileName) {
    const std::string name = lower(fileName);
    auto ends = [&](const char* e) { const std::string x(e); return name.size() >= x.size() && name.compare(name.size() - x.size(), x.size(), x) == 0; };
    if (ends(".psd")) return "8BPS";
    if (ends(".psb")) return "8BPB";
    if (ends(".png")) return "png ";
    if (ends(".jpg") || ends(".jpeg")) return "JPEG";
    if (ends(".tif") || ends(".tiff")) return "TIFF";
    if (ends(".gif")) return "GIFf";
    if (ends(".bmp")) return "BMP ";
    if (ends(".webp")) return "WEBP";
    return {};
}

std::shared_ptr<const SmartObjectSource> makeSmartObjectSource(SmartObjectContents contents) {
    if (!contents.image || contents.image->isEmpty()) return nullptr;
    auto s = std::make_shared<SmartObjectSource>();
    s->id = newSmartObjectId();
    s->fileName = contents.fileName;
    s->fileType = contents.fileType.empty() ? smartObjectFileType(contents.fileName) : contents.fileType;
    s->bytes = std::make_shared<const std::vector<uint8_t>>(std::move(contents.bytes));
    s->image = contents.image;
    s->width = contents.image->width();
    s->height = contents.image->height();
    s->resolution = contents.resolution;
    return s;
}

Layer smartObjectLayer(const std::shared_ptr<const SmartObjectSource>& source, const std::array<double, 8>& quad, const std::string& name) {
    Layer layer(Asset::make(source->image, name), Point(quad[0], quad[1]));
    layer.name = name;
    if (auto t = transformForQuad(quad, source->width, source->height)) layer.transform = *t;
    SmartObjectInstance instance;
    instance.sourceId = source->id;
    instance.quad = quad;
    instance.placedId = newSmartObjectId();
    instance.psdBlocks.push_back({"SoLd", authorPsdPlacement(source->id, instance.placedId, quad, source->width, source->height, source->resolution)});
    instance.placedTransform = layer.transform;
    instance.placedWidth = source->width;
    instance.placedHeight = source->height;
    layer.smartObject = std::move(instance);
    layer.smartImage = source->image;
    return layer;
}

std::array<double, 8> placementQuad(const Document& document, int width, int height) {
    double scale = 1;
    if (width > document.width || height > document.height) scale = std::min(double(document.width) / width, double(document.height) / height);
    const double w = width * scale, h = height * scale;
    const double x = (document.width - w) / 2, y = (document.height - h) / 2;
    return {x, y, x + w, y, x + w, y + h, x, y + h};
}

Uuid placeSmartObject(Document& document, const std::shared_ptr<const SmartObjectSource>& source, size_t index, std::optional<Uuid> parent) {
    document.smartObjects[source->id] = source;
    std::string name = source->fileName;
    if (auto dot = name.rfind('.'); dot != std::string::npos && dot > 0) name.resize(dot);
    Layer layer = smartObjectLayer(source, placementQuad(document, source->width, source->height), name.empty() ? "Smart Object" : name);
    layer.parentId = parent;
    const Uuid id = layer.id;
    document.layers.insert(document.layers.begin() + long(std::min(index, document.layers.size())), std::move(layer));
    return id;
}

std::optional<Uuid> convertToSmartObject(Document& document, const std::vector<Uuid>& ids, std::string* error, const PsdExportOptions& options) {
    auto fail = [&](const char* why) -> std::optional<Uuid> { if (error) *error = why; return std::nullopt; };
    // The selection with everything inside its folders, in stack order.
    std::set<Uuid> chosen(ids.begin(), ids.end());
    auto within = [&](const Layer& l) {
        int depth = 0;
        for (const Layer* p = &l; p && depth < 64; depth++) {
            if (chosen.count(p->id)) return true;
            p = p->parentId ? document.find(*p->parentId) : nullptr;
        }
        return false;
    };
    std::vector<size_t> members;
    for (size_t i = 0; i < document.layers.size(); i++) if (within(document.layers[i])) members.push_back(i);
    if (members.empty()) return fail("Select the layers to convert.");
    // The topmost selected root: its place, parent and name go to the smart object.
    const Layer* top = nullptr;
    for (size_t i : members) if (chosen.count(document.layers[i].id) && (!document.layers[i].parentId || !within(*document.find(*document.layers[i].parentId)))) top = &document.layers[i];
    if (!top) return fail("Select the layers to convert.");
    // The canvas: the members' bounds.
    double x0 = 1e300, y0 = 1e300, x1 = -1e300, y1 = -1e300;
    for (size_t i : members) {
        const Layer& l = document.layers[i];
        if (l.isGroup || l.adjustment || !l.asset || !l.asset->image) continue;
        const Rect b = l.transform.bounds();
        x0 = std::min(x0, b.x); y0 = std::min(y0, b.y); x1 = std::max(x1, b.x + b.width); y1 = std::max(y1, b.y + b.height);
    }
    if (x1 <= x0 || y1 <= y0) return fail("There are no pixels to convert.");
    const double left = std::floor(x0), topY = std::floor(y0);
    const int w = int(std::ceil(x1) - left), h = int(std::ceil(y1) - topY);
    if (w > psbMaxSide || h > psbMaxSide) return fail("The layers are too large for a smart object (300,000 pixels a side).");
    Document child(w, h);
    child.resolution = document.resolution;
    child.psdCarry = document.psdCarry;   // the global light and patterns the members' styles use
    for (size_t i : members) {
        Layer l = document.layers[i];
        if (chosen.count(l.id) && l.parentId && !within(*document.find(*l.parentId))) l.parentId.reset();
        else if (l.parentId && !within(*document.find(*l.parentId))) l.parentId.reset();
        l.transform.origin.x -= left; l.transform.origin.y -= topY;
        if (l.mask && l.mask->placement) { l.mask->placement->origin.x -= left; l.mask->placement->origin.y -= topY; }
        if (l.isGroup) l.transform = LayerTransform(Point(0, 0), Size(w, h));
        if (l.smartObject) {
            l.smartObject->placedTransform.origin.x -= left; l.smartObject->placedTransform.origin.y -= topY;
            for (size_t k = 0; k < 8; k += 2) { l.smartObject->quad[k] -= left; l.smartObject->quad[k + 1] -= topY; }
            if (auto s = document.smartObjects.find(l.smartObject->sourceId); s != document.smartObjects.end()) child.smartObjects[s->first] = s->second;
        }
        child.layers.push_back(std::move(l));
    }
    for (Layer& l : child.layers) if (l.maskSourceId && !child.find(*l.maskSourceId)) l.maskSourceId.reset();
    PsdExportSummary summary;
    std::string encodeError;
    // As Photoshop stores converted layers: a PSB.
    PsdExportOptions large = options;
    large.large = true;
    std::vector<uint8_t> bytes = encodePsd(child, large, &summary, &encodeError);
    if (bytes.empty()) return fail("The layers could not be written as a smart object.");
    SmartObjectContents contents;
    contents.bytes = std::move(bytes);
    contents.fileName = top->name + ".psb";
    contents.fileType = "8BPB";
    contents.image = renderFlattened(child);
    contents.resolution = document.resolution;
    auto source = makeSmartObjectSource(std::move(contents));
    if (!source) return fail("The layers could not be drawn.");
    Layer layer = smartObjectLayer(source, {left, topY, left + w, topY, left + w, topY + h, left, topY + h}, top->name);
    layer.parentId = top->parentId;
    const Uuid topId = top->id;
    // Out go the members; in at the topmost one's place goes the smart object.
    size_t at = 0;
    for (size_t i = 0; i < document.layers.size(); i++) if (document.layers[i].id == topId) at = i;
    size_t removedBelow = 0;
    for (size_t i : members) if (i < at) removedBelow++;
    std::set<Uuid> gone;
    for (size_t i : members) gone.insert(document.layers[i].id);
    document.layers.erase(std::remove_if(document.layers.begin(), document.layers.end(), [&](const Layer& l) { return gone.count(l.id) > 0; }), document.layers.end());
    // Clipping and live masks that pointed at a member point nowhere now.
    for (Layer& l : document.layers) if (l.maskSourceId && gone.count(*l.maskSourceId)) l.maskSourceId.reset();
    document.smartObjects[source->id] = source;
    const Uuid id = layer.id;
    document.layers.insert(document.layers.begin() + long(at - removedBelow), std::move(layer));
    return id;
}

bool smartObjectContentsEditable(const Document& document, const std::string& sourceId, std::string* why) {
    auto source = document.smartObjects.find(sourceId);
    if (source == document.smartObjects.end() || source->second->kind != SmartObjectSource::Kind::Embedded || !source->second->image) {
        if (why) *why = "Its contents are not embedded in a form NekoPhoto can open.";
        return false;
    }
    for (const Layer& l : document.layers)
        if (l.isLiveSmartObject() && l.smartObject->sourceId == sourceId && l.smartObject->locked()) {
            if (why) *why = std::string("Layer \"") + l.name + "\" places it " + smartObjectLockDescription(l.smartObject->lock) + ", which NekoPhoto cannot redraw yet.";
            return false;
        }
    return true;
}

int replaceSmartObjectSource(Document& document, const std::string& from, const std::shared_ptr<const SmartObjectSource>& replacement) {
    auto old = document.smartObjects.find(from);
    if (old == document.smartObjects.end() || !replacement || !replacement->image) return 0;
    const std::string oldStem = [&] { std::string n = old->second->fileName; if (auto d = n.rfind('.'); d != std::string::npos) n.resize(d); return n; }();
    const std::string newStem = [&] { std::string n = replacement->fileName; if (auto d = n.rfind('.'); d != std::string::npos) n.resize(d); return n; }();
    document.smartObjects[replacement->id] = replacement;
    int changed = 0;
    for (Layer& l : document.layers) {
        if (!l.isLiveSmartObject() || l.smartObject->sourceId != from || l.smartObject->locked()) continue;
        SmartObjectInstance& so = *l.smartObject;
        if (!smartObjectPixelsArePlacement(so)) {
            // Warped or filtered: the quad (the warp cage) stays where it is and the new contents are drawn through
            // the same warp and filters.
            const std::array<double, 8> quad = moveQuad(so.quad, so.placedTransform, so.placedWidth, so.placedHeight,
                                                        l.transform, l.asset->image->width(), l.asset->image->height());
            SmartObjectInstance next = so;
            for (PsdBlock& b : next.psdBlocks)
                if (auto patched = repointPsdPlacement(b.key, b.data, quad, replacement->id, replacement->width, replacement->height)) b.data = std::move(*patched);
            std::optional<WarpedRaster> raster;
            if (smartObjectFiltered(next)) {
                static const std::vector<PsdBlock> none;
                if (auto f = filteredSmartObjectRaster(document.psdCarry ? document.psdCarry->globals : none, next, *replacement->image, quad))
                    raster = WarpedRaster{f->image, LayerTransform(Point(f->x, f->y), Size(f->image->width(), f->image->height()))};
            } else raster = warpedSmartObjectRaster(next, *replacement->image, quad);
            if (!raster) continue;
            next.sourceId = replacement->id;
            next.quad = quad;
            const Sampling sampling = l.transform.sampling;
            l.asset = Asset::make(raster->image, l.name);
            l.transform = raster->transform;
            l.transform.sampling = sampling;
            l.smartImage = raster->image;
            next.placedTransform = l.transform;
            next.placedWidth = raster->image->width();
            next.placedHeight = raster->image->height();
            so = std::move(next);
            if (!oldStem.empty() && l.name.compare(0, oldStem.size(), oldStem) == 0) l.name = newStem + l.name.substr(oldStem.size());
            changed++;
            continue;
        }
        // About its own centre, at its own scale.
        const int w0 = l.asset->image->width(), h0 = l.asset->image->height();
        LayerTransform t = l.transform;
        const Point centre = t.center();
        const double sx = t.size.width / std::max(1, w0), sy = t.size.height / std::max(1, h0);
        t.size = Size(replacement->width * sx, replacement->height * sy);
        t.origin = Point(centre.x - t.size.width / 2, centre.y - t.size.height / 2);
        l.asset = Asset::make(replacement->image, l.name);
        l.transform = t;
        l.smartImage = replacement->image;
        so.sourceId = replacement->id;
        so.quad = quadOf(t, replacement->width, replacement->height);
        for (PsdBlock& b : so.psdBlocks)
            if (auto patched = repointPsdPlacement(b.key, b.data, so.quad, replacement->id, replacement->width, replacement->height)) b.data = std::move(*patched);
        so.placedTransform = t;
        so.placedWidth = replacement->width;
        so.placedHeight = replacement->height;
        // "A" becomes "B", "A copy" becomes "B copy" (Photoshop's rename).
        if (!oldStem.empty() && l.name.compare(0, oldStem.size(), oldStem) == 0) l.name = newStem + l.name.substr(oldStem.size());
        changed++;
    }
    // The old contents go once nothing places them (an instance that could not be redrawn keeps them).
    bool stillPlaced = false;
    for (const Layer& l : document.layers) stillPlaced |= l.smartObject && l.smartObject->sourceId == from;
    if (changed && !stillPlaced) document.smartObjects.erase(from);
    return changed;
}

std::optional<Document> smartObjectContentsDocument(const Document& document, const std::string& sourceId) {
    auto it = document.smartObjects.find(sourceId);
    if (it == document.smartObjects.end() || !it->second->bytes) return std::nullopt;
    const SmartObjectSource& s = *it->second;
    const auto& bytes = *s.bytes;
    if (bytes.size() >= 4 && std::equal(bytes.begin(), bytes.begin() + 4, "8BPS")) {
        std::string error;
        if (auto imported = importPsdBytes(bytes, &error)) return std::move(imported->document);
        return std::nullopt;
    }
    if (!s.image) return std::nullopt;
    Document d(s.image->width(), s.image->height());
    d.resolution = s.resolution;
    std::string name = s.fileName;
    if (auto dot = name.rfind('.'); dot != std::string::npos && dot > 0) name.resize(dot);
    Layer layer(Asset::make(s.image, name.empty() ? "Contents" : name), Point(0, 0));
    layer.name = name.empty() ? "Contents" : name;
    d.layers.push_back(layer);
    return d;
}

std::vector<uint8_t> encodeSmartObjectContents(const Document& contents, const SmartObjectSource& source, const PsdExportOptions& options) {
    if (source.fileType == "8BPS" || source.fileType == "8BPB") {
        PsdExportSummary summary;
        std::string error;
        PsdExportOptions o = options;
        o.large = source.fileType == "8BPB";
        return encodePsd(contents, o, &summary, &error);
    }
    if (source.fileType == "png ") {
        std::vector<uint8_t> out;
        if (auto flat = renderFlattened(contents); flat && encodePngImage(*flat, out, contents.resolution)) return out;
    }
    return {};
}

void rasterizeSmartObject(Layer& layer) {
    layer.smartObject.reset();
    layer.smartImage.reset();
}

} // namespace compositor
