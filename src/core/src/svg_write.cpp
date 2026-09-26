// SVG export (svg.h): vector shape layers as <path> elements, folders as <g>, and every layer SVG cannot draw
// element by element (pixels, text, smart objects, styled or masked layers, clipping runs) as a PNG <image> rendered
// by the compositor, so the file always looks like the document. Adjustment layers and blend modes CSS lacks merge
// everything below them in their folder into one image. Ported from Patchy (MIT,
// src/third_party/patchy_psd/README.md): formats/svg_document_write.cpp and formats/vector_export_plan.cpp.
#include "compositor/layerstyle.h"
#include "compositor/png.h"
#include "compositor/render.h"
#include "compositor/svg.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

namespace compositor {

namespace {

std::string number(double v) {
    if (!std::isfinite(v)) v = 0;
    v = std::round(v * 1e4) / 1e4;   // below a ten-thousandth of a pixel is the path storage's rounding
    if (v == 0) v = 0;              // no "-0"
    std::ostringstream s;
    s.imbue(std::locale::classic());
    s.precision(10);
    s << v;
    return s.str();
}

std::string opacityText(double v) { return number(std::round(v * 1e6) / 1e6); }

std::string hex(uint8_t r, uint8_t g, uint8_t b) {
    static const char digits[] = "0123456789abcdef";
    std::string out = "#";
    for (uint8_t c : {r, g, b}) { out += digits[c >> 4]; out += digits[c & 15]; }
    return out;
}

std::string escape(std::string_view text) {
    std::string out;
    for (char c : text) {
        switch (c) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '"': out += "&quot;"; break;
        default: out += c;
        }
    }
    return out;
}

std::string base64(const std::vector<uint8_t>& bytes) {
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((bytes.size() + 2) / 3 * 4);
    for (size_t i = 0; i < bytes.size(); i += 3) {
        const uint32_t v = (uint32_t(bytes[i]) << 16) | (i + 1 < bytes.size() ? uint32_t(bytes[i + 1]) << 8 : 0) | (i + 2 < bytes.size() ? uint32_t(bytes[i + 2]) : 0);
        out += alphabet[(v >> 18) & 63];
        out += alphabet[(v >> 12) & 63];
        out += i + 1 < bytes.size() ? alphabet[(v >> 6) & 63] : '=';
        out += i + 2 < bytes.size() ? alphabet[v & 63] : '=';
    }
    return out;
}

/// CSS's name for a blend mode; empty when CSS has none (Normal is "normal").
std::string cssBlend(BlendMode mode) {
    switch (mode) {
    case BlendMode::Normal: return "normal";
    case BlendMode::Multiply: return "multiply";
    case BlendMode::Screen: return "screen";
    case BlendMode::Overlay: return "overlay";
    case BlendMode::Darken: return "darken";
    case BlendMode::Lighten: return "lighten";
    case BlendMode::ColorDodge: return "color-dodge";
    case BlendMode::ColorBurn: return "color-burn";
    case BlendMode::HardLight: return "hard-light";
    case BlendMode::SoftLight: return "soft-light";
    case BlendMode::Difference: return "difference";
    case BlendMode::Exclusion: return "exclusion";
    case BlendMode::Hue: return "hue";
    case BlendMode::Saturation: return "saturation";
    case BlendMode::Color: return "color";
    case BlendMode::Luminosity: return "luminosity";
    case BlendMode::LinearDodge: return "plus-lighter";
    default: return {};
    }
}

// ---- The combine structure (Patchy's vector_export_plan) --------------------------------------------------------

enum class Combine { SinglePath, SeparatePaths, Unsupported };

/// Shape groups as the renderer draws them: runs of consecutive subpaths sharing a group index.
std::vector<VectorPath> shapeGroups(const VectorPath& path) {
    std::vector<VectorPath> groups;
    for (size_t i = 0; i < path.subpaths.size(); i++) {
        if (i == 0 || path.subpaths[i].group != path.subpaths[i - 1].group) groups.emplace_back();
        groups.back().subpaths.push_back(path.subpaths[i]);
    }
    return groups;
}

bool disjoint(const VectorPath& a, const VectorPath& b) {
    const Rect ra = pathBounds(a), rb = pathBounds(b);
    if (ra.isEmpty() || rb.isEmpty()) return true;
    return ra.maxX() <= rb.x || rb.maxX() <= ra.x || ra.maxY() <= rb.y || rb.maxY() <= ra.y;
}

Combine classify(const VectorPath& path) {
    const auto groups = shapeGroups(path);
    if (groups.size() <= 1) return Combine::SinglePath;   // one group fills even-odd, as SVG's evenodd does
    // Adds then subtracts, holes inside their outlines, the outlines apart: exactly one even-odd path.
    size_t firstSubtract = groups.size();
    bool ordered = true;
    for (size_t i = 0; i < groups.size(); i++) {
        const auto op = groups[i].subpaths.front().op;
        if (op == VectorPath::Op::Intersect || (op == VectorPath::Op::Xor && i > 0)) return Combine::Unsupported;
        if (op == VectorPath::Op::Subtract) { if (i == 0) return Combine::Unsupported; firstSubtract = std::min(firstSubtract, i); }
        else if (i > firstSubtract) ordered = false;
    }
    auto addsDisjoint = [&] {
        for (size_t i = 0; i < firstSubtract; i++)
            for (size_t j = i + 1; j < firstSubtract; j++) if (!disjoint(groups[i], groups[j])) return false;
        return true;
    };
    if (firstSubtract < groups.size()) {
        if (!ordered || !addsDisjoint()) return Combine::Unsupported;
        for (size_t i = firstSubtract; i < groups.size(); i++) {
            const auto& knots = groups[i].subpaths.front().knots;
            if (knots.empty()) continue;
            bool inside = false;
            for (size_t j = 0; j < firstSubtract && !inside; j++) inside = pathBounds(groups[j]).contains({knots.front().x, knots.front().y});
            if (!inside) return Combine::Unsupported;
        }
        return Combine::SinglePath;
    }
    return addsDisjoint() ? Combine::SinglePath : Combine::SeparatePaths;
}

struct Writer {
    const Document& document;
    SvgExportSummary& summary;
    std::string defs, body;
    std::set<std::string> usedIds;
    int clips = 0, masks = 0;
    std::map<Uuid, std::vector<const Layer*>> children;
    std::map<Uuid, const Layer*> byId;

    Writer(const Document& d, SvgExportSummary& s) : document(d), summary(s) {
        for (const Layer& l : document.layers) { children[l.parentId.value_or(std::string())].push_back(&l); byId[l.id] = &l; }
    }

    void note(const std::string& text) {
        if (std::find(summary.notes.begin(), summary.notes.end(), text) == summary.notes.end()) summary.notes.push_back(text);
    }

    std::string uniqueId(const std::string& name) {
        std::string id;
        for (char c : name) id += (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' ? c : '_';
        if (id.empty() || (id[0] >= '0' && id[0] <= '9') || id[0] == '-') id = "layer-" + id;
        const std::string base = id;
        for (int n = 2; !usedIds.insert(id).second; n++) id = base + "_" + std::to_string(n);
        return id;
    }

    static void indent(std::string& out, int n) { out.append(size_t(n), ' '); }

    std::string css(const Layer& layer, bool withOpacity = true) const {
        std::string out;
        if (withOpacity && layer.opacity < 0.99995) out += "opacity:" + opacityText(layer.opacity) + ";";
        if (layer.blendMode != BlendMode::Normal && !cssBlend(layer.blendMode).empty()) out += "mix-blend-mode:" + cssBlend(layer.blendMode) + ";";
        return out;
    }

    static bool expressible(BlendMode mode) { return !cssBlend(mode).empty(); }

    // ---- Rasterised units ----------------------------------------------------------------------------------------

    void descendants(const Layer& layer, std::set<Uuid>& out) const {
        out.insert(layer.id);
        if (auto it = children.find(layer.id); it != children.end()) for (const Layer* c : it->second) descendants(*c, out);
    }

    /// The document with only `showing` (and what they hold) drawn, their folders neutral (the <g> elements carry
    /// those), and `neutral` at full opacity in Normal: the pixels one <image> stands for.
    std::shared_ptr<Image> renderOnly(const std::vector<const Layer*>& showing, const Layer* neutral) const {
        std::set<Uuid> keep, ancestors;
        for (const Layer* l : showing) {
            descendants(*l, keep);
            for (auto p = l->parentId; p; ) { ancestors.insert(*p); auto it = byId.find(*p); p = it == byId.end() ? std::nullopt : it->second->parentId; }
        }
        Document copy = document;
        for (Layer& l : copy.layers) {
            if (ancestors.count(l.id)) { l.visible = true; l.opacity = 1; l.blendMode = BlendMode::Normal; l.passThrough = true; l.mask.reset(); l.psdCarry.reset(); continue; }
            if (!keep.count(l.id)) { l.visible = false; continue; }
            if (neutral && l.id == neutral->id) { l.visible = true; l.opacity = 1; l.blendMode = BlendMode::Normal; }
        }
        return renderFlattened(copy);
    }

    void emitImage(const Image& pixels, const std::string& name, const std::string& style, int depth) {
        int x0 = pixels.width(), y0 = pixels.height(), x1 = -1, y1 = -1;
        for (int y = 0; y < pixels.height(); y++) {
            const uint8_t* row = pixels.row(y);
            for (int x = 0; x < pixels.width(); x++)
                if (row[x * 4 + 3]) { x0 = std::min(x0, x); x1 = std::max(x1, x); y0 = std::min(y0, y); y1 = y; }
        }
        if (x1 < 0) return;   // nothing shows
        Image crop(x1 - x0 + 1, y1 - y0 + 1);
        for (int y = 0; y < crop.height(); y++) std::copy_n(pixels.row(y0 + y) + x0 * 4, crop.width() * 4, crop.row(y));
        std::vector<uint8_t> png;
        if (!encodePngImage(crop, png)) { note("An image could not be encoded and was left out."); return; }
        const std::string data = "data:image/png;base64," + base64(png);
        indent(body, depth);
        body += "<image id=\"" + escape(uniqueId(name)) + "\" x=\"" + std::to_string(x0) + "\" y=\"" + std::to_string(y0) + "\" width=\"" + std::to_string(crop.width())
              + "\" height=\"" + std::to_string(crop.height()) + "\" href=\"" + data + "\" xlink:href=\"" + data + "\"";
        if (!style.empty()) body += " style=\"" + style + "\"";
        body += "/>\n";
        summary.images++;
    }

    void emitRasterUnit(const std::vector<const Layer*>& run, int depth) {
        const Layer& base = *run.front();
        if (run.size() > 1) note("Clipped layers over \"" + base.name + "\" were written as one image.");
        else if (base.isGroup) note("Folder \"" + base.name + "\" was written as one image (SVG cannot draw its style or blending).");
        else if (isVectorShapeLayer(base)) note("Shape layer \"" + base.name + "\" was written as an image (its style, mask or path combination has no SVG form).");
        if (auto pixels = renderOnly(run, &base)) emitImage(*pixels, base.name, css(base), depth);
    }

    // ---- Shapes --------------------------------------------------------------------------------------------------

    std::optional<VectorShape> exportableShape(const Layer& layer) const {
        if (!isVectorShapeLayer(layer) || layer.maskSourceId) return std::nullopt;
        if (layer.mask && layer.mask->enabled) return std::nullopt;
        if (auto style = layerStyleOf(layer, document); style && hasAnyEffect(*style)) return std::nullopt;
        auto shape = vectorShapeOf(layer, document);
        if (!shape || shape->path.inverted || shape->path.subpaths.empty() || classify(shape->path) == Combine::Unsupported) return std::nullopt;
        return shape;
    }

    std::string clipOf(const VectorPath& path, bool evenOdd) {
        const std::string id = "clip" + std::to_string(++clips);
        defs += "<clipPath id=\"" + id + "\">";
        if (evenOdd) defs += "<path clip-rule=\"evenodd\" d=\"" + svgPathData(path) + "\"/>";
        else for (const auto& group : shapeGroups(path)) defs += "<path clip-rule=\"evenodd\" d=\"" + svgPathData(group) + "\"/>";
        defs += "</clipPath>";
        return id;
    }

    std::string outsideMaskOf(const VectorPath& path, bool evenOdd) {
        const std::string id = "mask" + std::to_string(++masks);
        defs += "<mask id=\"" + id + "\" maskUnits=\"userSpaceOnUse\" x=\"-100000\" y=\"-100000\" width=\"200000\" height=\"200000\">"
                "<rect x=\"-100000\" y=\"-100000\" width=\"200000\" height=\"200000\" fill=\"#ffffff\"/>";
        if (evenOdd) defs += "<path fill-rule=\"evenodd\" fill=\"#000000\" d=\"" + svgPathData(path) + "\"/>";
        else for (const auto& group : shapeGroups(path)) defs += "<path fill-rule=\"evenodd\" fill=\"#000000\" d=\"" + svgPathData(group) + "\"/>";
        defs += "</mask>";
        return id;
    }

    std::string strokeAttributes(const VectorStroke& s) const {
        const bool doubled = s.align != VectorStroke::Align::Center;
        std::string out = " stroke=\"" + hex(s.r, s.g, s.b) + "\" stroke-width=\"" + number(s.width * (doubled ? 2 : 1)) + "\"";
        out += std::string(" stroke-linecap=\"") + (s.cap == VectorStroke::Cap::Round ? "round" : s.cap == VectorStroke::Cap::Square ? "square" : "butt") + "\"";
        out += std::string(" stroke-linejoin=\"") + (s.join == VectorStroke::Join::Round ? "round" : s.join == VectorStroke::Join::Bevel ? "bevel" : "miter") + "\"";
        if (s.join == VectorStroke::Join::Miter) out += " stroke-miterlimit=\"" + number(std::max(1.0, s.miterLimit)) + "\"";
        if (s.opacity < 0.99995f) out += " stroke-opacity=\"" + opacityText(s.opacity) + "\"";
        if (!s.dashes.empty()) {
            out += " stroke-dasharray=\"";
            for (size_t i = 0; i < s.dashes.size(); i++) out += (i ? " " : "") + number(s.dashes[i] * s.width);
            out += "\"";
            if (std::abs(s.dashOffset) > 1e-9) out += " stroke-dashoffset=\"" + number(s.dashOffset * s.width) + "\"";
        }
        if (doubled) {
            // Drawn at twice the width and cut to one side; the hint lets NekoPhoto (and Patchy) read the true stroke back.
            out += std::string(" data-nekophoto-stroke-align=\"") + (s.align == VectorStroke::Align::Inside ? "inside" : "outside") + "\" data-nekophoto-stroke-width=\"" + number(s.width) + "\"";
        }
        return out;
    }

    void emitShape(const Layer& layer, const VectorShape& shape, int depth) {
        const Combine combine = classify(shape.path);
        const bool single = combine == Combine::SinglePath;
        const std::string fill = shape.fill ? hex(shape.r, shape.g, shape.b) : "none";
        const VectorStroke& stroke = shape.stroke;
        std::string style = css(layer);
        std::string cut;   // what cuts a doubled stroke to one side
        if (stroke.enabled && stroke.align == VectorStroke::Align::Inside) cut = " clip-path=\"url(#" + clipOf(shape.path, single) + ")\"";
        else if (stroke.enabled && stroke.align == VectorStroke::Align::Outside) {
            if (shape.fill) cut = " paint-order=\"stroke\"";   // the stroke under an opaque fill: its outer half shows
            else cut = " mask=\"url(#" + outsideMaskOf(shape.path, single) + ")\"";
        }
        const std::string id = escape(uniqueId(layer.name));
        indent(body, depth);
        if (single) {
            body += "<path id=\"" + id + "\" d=\"" + svgPathData(shape.path) + "\" fill-rule=\"evenodd\" fill=\"" + fill + "\"" + (stroke.enabled ? strokeAttributes(stroke) + cut : std::string());
            if (!style.empty()) body += " style=\"" + style + "\"";
            body += "/>\n";
        } else {
            // Overlapping outlines that add: one path each (the union), then the stroke over them all.
            body += "<g id=\"" + id + "\"" + (style.empty() ? std::string() : " style=\"" + style + "\"") + ">\n";
            if (shape.fill)
                for (const auto& group : shapeGroups(shape.path)) { indent(body, depth + 2); body += "<path d=\"" + svgPathData(group) + "\" fill-rule=\"evenodd\" fill=\"" + fill + "\"/>\n"; }
            if (stroke.enabled) {
                std::string strokeCut = cut;
                if (stroke.align == VectorStroke::Align::Outside && shape.fill) strokeCut = " mask=\"url(#" + outsideMaskOf(shape.path, false) + ")\"";
                indent(body, depth + 2);
                body += "<path d=\"" + svgPathData(shape.path) + "\" fill=\"none\"" + strokeAttributes(stroke) + strokeCut + "/>\n";
            }
            indent(body, depth);
            body += "</g>\n";
        }
        summary.shapes++;
    }

    // ---- Folders -------------------------------------------------------------------------------------------------

    bool folderExportable(const Layer& folder) const {
        if (auto style = layerStyleOf(folder, document); style && hasAnyEffect(*style)) return false;
        return true;
    }

    std::string folderMask(const Layer& folder) {
        std::string out;
        if (folder.mask && folder.mask->enabled && folder.mask->asset.image) {
            GrayImage coverage(document.width, document.height, 0);
            const uint8_t outside = folder.mask->asset.thumbnail ? LayerMask::background(*folder.mask->asset.thumbnail) : 255;
            sampleMaskCoverage(folder.mask->asset.image, folder.maskTransform(), document.rect(), 1, outside, coverage, false);
            Image grey(document.width, document.height);
            for (int y = 0; y < document.height; y++) {
                const uint8_t* s = coverage.row(y);
                uint8_t* d = grey.row(y);
                for (int x = 0; x < document.width; x++) { d[x * 4] = d[x * 4 + 1] = d[x * 4 + 2] = s[x]; d[x * 4 + 3] = 255; }
            }
            std::vector<uint8_t> png;
            if (encodePngImage(grey, png)) {
                const std::string id = "mask" + std::to_string(++masks), data = "data:image/png;base64," + base64(png);
                defs += "<mask id=\"" + id + "\" maskUnits=\"userSpaceOnUse\" x=\"0\" y=\"0\" width=\"" + std::to_string(document.width) + "\" height=\"" + std::to_string(document.height)
                      + "\"><image x=\"0\" y=\"0\" width=\"" + std::to_string(document.width) + "\" height=\"" + std::to_string(document.height) + "\" href=\"" + data + "\" xlink:href=\"" + data + "\"/></mask>";
                out += " mask=\"url(#" + id + ")\"";
            }
        }
        if (auto path = layerVectorMask(folder, document); path && !path->subpaths.empty()) {
            if (path->inverted) {
                VectorPath whole = rectanglePath(Rect(-100000, -100000, 200000, 200000));
                VectorPath complement = *path;
                complement.subpaths.insert(complement.subpaths.begin(), whole.subpaths.begin(), whole.subpaths.end());
                out += " clip-path=\"url(#" + clipOf(complement, true) + ")\"";
            } else out += " clip-path=\"url(#" + clipOf(*path, classify(*path) != Combine::SeparatePaths) + ")\"";
        }
        return out;
    }

    void emitFolder(const Layer& folder, int depth) {
        if (!folderExportable(folder)) { emitRasterUnit({&folder}, depth); return; }
        std::string style = css(folder);
        if (!folder.passThrough) style += "isolation:isolate;";
        else if (folder.opacity < 0.99995) note("A pass-through folder's opacity was written as SVG group opacity, which isolates the folder.");
        indent(body, depth);
        body += "<g id=\"" + escape(uniqueId(folder.name)) + "\"" + folderMask(folder) + (style.empty() ? std::string() : " style=\"" + style + "\"") + ">\n";
        emitChildren(folder.id, depth + 2);
        indent(body, depth);
        body += "</g>\n";
        summary.groups++;
    }

    // ---- A folder's contents ---------------------------------------------------------------------------------------

    /// Whether drawing this unit needs what is below it in a way SVG cannot say (an adjustment, a blend mode CSS
    /// lacks), looking into pass-through folders, whose contents blend with what is below them.
    bool barrier(const Layer& layer) const {
        if (!layer.visible) return false;
        if (layer.adjustment) return true;
        if (!expressible(layer.blendMode)) return true;
        if (layer.isGroup && layer.passThrough)
            if (auto it = children.find(layer.id); it != children.end())
                for (const Layer* c : it->second) if (barrier(*c)) return true;
        return false;
    }

    void emitChildren(const std::optional<Uuid>& parent, int depth) {
        auto it = children.find(parent.value_or(std::string()));
        if (it == children.end()) return;
        const auto& siblings = it->second;
        // Units: a layer, or a clipping base with the layers clipped to it.
        std::vector<std::vector<const Layer*>> units;
        for (const Layer* l : siblings) {
            if (l->maskSourceId && !units.empty() && units.back().front()->id == *l->maskSourceId) units.back().push_back(l);
            else if (l->maskSourceId && !units.empty() && std::any_of(units.back().begin(), units.back().end(), [&](const Layer* m) { return m->id == *l->maskSourceId; })) units.back().push_back(l);
            else units.push_back({l});
        }
        size_t start = 0;
        for (size_t i = 0; i < units.size(); i++)
            for (const Layer* l : units[i]) if (barrier(*l)) start = i + 1;
        if (start > 0) {
            std::vector<const Layer*> merged;
            std::string names;
            for (size_t i = 0; i < start; i++) for (const Layer* l : units[i]) { merged.push_back(l); names += (names.empty() ? "" : ", ") + l->name; }
            note("Merged into one image, as SVG has no adjustment layers or these blend modes: " + names + ".");
            if (auto pixels = renderOnly(merged, nullptr)) emitImage(*pixels, "Merged", "", depth);
        }
        for (size_t i = start; i < units.size(); i++) {
            const auto& unit = units[i];
            const Layer& base = *unit.front();
            if (!base.visible) continue;   // hidden layers are left out
            if (unit.size() > 1) { emitRasterUnit(unit, depth); continue; }
            if (base.isGroup) { emitFolder(base, depth); continue; }
            if (auto shape = exportableShape(base)) { emitShape(base, *shape, depth); continue; }
            emitRasterUnit(unit, depth);
        }
    }

    std::string run() {
        emitChildren(std::nullopt, 2);
        std::string out = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<svg xmlns=\"http://www.w3.org/2000/svg\" xmlns:xlink=\"http://www.w3.org/1999/xlink\" version=\"1.1\" width=\""
            + std::to_string(document.width) + "\" height=\"" + std::to_string(document.height) + "\" viewBox=\"0 0 " + std::to_string(document.width) + " " + std::to_string(document.height) + "\">\n";
        if (!defs.empty()) out += "  <defs>" + defs + "</defs>\n";
        out += body;
        out += "</svg>\n";
        return out;
    }
};

} // namespace

std::string writeSvg(const Document& document, SvgExportSummary* summary) {
    SvgExportSummary local;
    Writer writer(document, summary ? *summary : local);
    return writer.run();
}

bool exportSvg(const Document& document, const std::string& path, SvgExportSummary* summary, std::string* error) {
    const std::string text = writeSvg(document, summary);
    // Written beside the target and renamed over it, so a failed write never truncates an existing file.
    const std::string temporary = path + ".part";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out) { if (error) *error = "could not create the file"; return false; }
        out.write(text.data(), std::streamsize(text.size()));
        if (!out) { if (error) *error = "could not write the file"; return false; }
    }
    if (std::rename(temporary.c_str(), path.c_str()) != 0) { std::remove(temporary.c_str()); if (error) *error = "could not replace the file"; return false; }
    return true;
}

} // namespace compositor
