// Affinity documents (.afphoto, .afdesign, .afpub, .af) as layered documents. Ported from Patchy (MIT,
// src/third_party/patchy_psd/README.md), src/formats/af_document_io.cpp: Patchy derived the format from files
// authored with a licensed Affinity and the MIT-licensed afread project's notes; the container walk, the tile
// planes, the blend table and the placement rules below are its findings, kept as it pinned them. What differs:
// vector curves and parametric shapes are drawn as pixels, text becomes NekoPhoto text (drawn by the app), and
// adjustments, live filters and effects are left out and listed.
//
// Container (all integers little-endian): u32 magic 0x414BFF00, u16 version, u16 flags, u32 class tag, an "#Inf"
// block (stream-table offset, thumbnail offset, sizes, dates), "Prot" + u32 revision (version > 7). The stream
// table ("#FAT".."#FT4" chain, newest link first) names streams (doc.dat, d/<hex> raster tiles, c/<n> placed
// originals, edc/<n> embedded documents); each sits behind a "#Fil" tag, stored raw, zlib or zstd, with optional
// byte or u16 delta predictors and a CRC32 of the decoded bytes. At the thumbnail offset: FFFFFFFF "Thmb", then a
// PNG of the flattened document.
#include "nekophoto/affinity.h"
#include "affinity_read.h"
#include "affinity_tree.h"
#include "nekophoto/colour.h"
#include "nekophoto/png.h"
#include "nekophoto/render.h"
#include "nekophoto/vectormask.h"
#include "lcms2.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <tuple>
#include <unordered_map>
#include <zlib.h>

#ifdef NEKOPHOTO_HAVE_ZSTD
#include <zstd.h>
#endif

namespace nekophoto {

using namespace affinity_read;

bool affinityZstdSupported() {
#ifdef NEKOPHOTO_HAVE_ZSTD
    return true;
#else
    return false;
#endif
}

bool isAffinityFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    unsigned char magic[4] = {};
    return in.read(reinterpret_cast<char*>(magic), 4) && magic[0] == 0x00 && magic[1] == 0xFF && magic[2] == 0x4B && magic[3] == 0x41;
}

namespace affinity_detail {

void undoByteDelta(std::vector<uint8_t>& bytes) {
    uint8_t sum = 0;
    for (auto& value : bytes) { sum = uint8_t(sum + value); value = sum; }
}

void undoU16Delta(std::vector<uint8_t>& bytes) {
    uint16_t sum = 0;
    for (size_t i = 0; i + 1 < bytes.size(); i += 2) {
        sum = uint16_t(sum + uint16_t(bytes[i] | bytes[i + 1] << 8));
        bytes[i] = uint8_t(sum & 0xFF);
        bytes[i + 1] = uint8_t(sum >> 8);
    }
}

void undoTileInterleave(std::vector<uint8_t>& bytes) {
    if (bytes.size() != 0x10000) return;
    std::vector<uint8_t> out(bytes.size());
    for (size_t i = 0; i < 0x4000; i++) {
        out[4 * i + 1] = bytes[2 * i];
        out[4 * i + 3] = bytes[2 * i + 1];
        out[4 * i + 0] = bytes[0x8000 + 2 * i];
        out[4 * i + 2] = bytes[0x8000 + 2 * i + 1];
    }
    bytes = std::move(out);
}

// Layer Blnd and effect BlnM share one versioned enum: a version-0 base table plus modes added later under a
// version bump; version 6 renumbers the space (Pigment inserted at 1). Pinned by Patchy against a document with
// one layer per blend-menu entry.
std::optional<BlendMode> blendMode(uint16_t id, uint16_t version) {
    if (version == 0) {
        static constexpr BlendMode v0[] = {BlendMode::Normal, BlendMode::Darken, BlendMode::Multiply, BlendMode::ColorBurn,
            BlendMode::Lighten, BlendMode::Screen, BlendMode::ColorDodge, BlendMode::LinearDodge, BlendMode::Overlay,
            BlendMode::SoftLight, BlendMode::HardLight, BlendMode::VividLight, BlendMode::PinLight, BlendMode::HardMix,
            BlendMode::Difference, BlendMode::Exclusion, BlendMode::Subtract, BlendMode::Hue, BlendMode::Saturation,
            BlendMode::Luminosity, BlendMode::Color};
        if (id < std::size(v0)) return v0[id];
        return std::nullopt;   // 21 Average, 22 Negation, 23 Reflect, 24 Glow, 25 Erase
    }
    if (version == 1 && id == 2) return BlendMode::DarkerColor;
    if (version == 1 && id == 6) return BlendMode::LighterColor;
    if (version == 1 && id == 15) return BlendMode::LinearLight;
    if (version == 3 && id == 5) return BlendMode::LinearBurn;
    if (version == 4 && id == 21) return BlendMode::Divide;
    if (version >= 6) {
        switch (id) {
        case 0: return BlendMode::Normal;
        case 2: return BlendMode::Darken;
        case 3: return BlendMode::DarkerColor;
        case 4: return BlendMode::Multiply;
        case 5: return BlendMode::ColorBurn;
        case 6: return BlendMode::LinearBurn;
        case 7: return BlendMode::Lighten;
        case 8: return BlendMode::LighterColor;
        case 9: return BlendMode::Screen;
        case 10: return BlendMode::ColorDodge;
        case 11: return BlendMode::LinearDodge;
        case 12: return BlendMode::Overlay;
        case 13: return BlendMode::SoftLight;
        case 14: return BlendMode::HardLight;
        case 15: return BlendMode::VividLight;
        case 16: return BlendMode::PinLight;
        case 17: return BlendMode::LinearLight;
        case 18: return BlendMode::HardMix;
        case 19: return BlendMode::Difference;
        case 20: return BlendMode::Exclusion;
        case 21: return BlendMode::Subtract;
        case 22: return BlendMode::Divide;
        case 23: return BlendMode::Hue;
        case 24: return BlendMode::Saturation;
        case 25: return BlendMode::Luminosity;
        case 26: return BlendMode::Color;
        default: return std::nullopt;   // 1 Pigment, 27 Average ... 32 Erase
        }
    }
    return std::nullopt;
}

std::vector<double> composeTransforms(const std::vector<double>& p, const std::vector<double>& c) {
    if (p.size() != 6 || c.size() != 6) return p.size() == 6 ? p : c;
    return {p[0] * c[0] + p[1] * c[3], p[0] * c[1] + p[1] * c[4], p[0] * c[2] + p[1] * c[5] + p[2],
            p[3] * c[0] + p[4] * c[3], p[3] * c[1] + p[4] * c[4], p[3] * c[2] + p[4] * c[5] + p[5]};
}

} // namespace affinity_detail

namespace {

using affinity_detail::blendMode;

constexpr uint16_t newestVerifiedVersion = 12;
constexpr int maxLayers = 20000;
constexpr int maxEmbedDepth = 3;
constexpr size_t maxNesting = 128;   // layer nesting; real documents stay within a few dozen

// ------------------------------------------------------------------------------------------------ building

/// A layer as the build makes it, straight alpha, before it becomes a Document entry.
struct Built {
    std::string name;
    bool group = false, passThrough = true, visible = true, clipped = false, erase = false;
    double opacity = 1;
    BlendMode blend = BlendMode::Normal;
    Straight image;
    int x = 0, y = 0;
    Plane mask;
    int maskX = 0, maskY = 0;
    uint8_t maskOutside = 255;   // what the mask is beyond its plane
    std::vector<Built> children;
    /// Text: the layer's text, drawn by the app after the import, and where it goes.
    std::optional<LayerText> text;
    PsdImport::PendingText placement;
};

struct Context {
    Source src;
    const PsdImportOptions& options;
    std::vector<std::string>& notes;
    int width = 0, height = 0;
    int embedDepth = 0;
    int layerCount = 0;
    uint32_t documentVersion = 0;
    Affine6 transform = identity();
    int leftOut = 0;   // visible content not carried
    std::vector<const Class*> open;   // the layers being built, outermost first: a shared class may name its ancestor
};

Document readContainer(std::span<const uint8_t> bytes, std::vector<std::string>& notes, int embedDepth, const PsdImportOptions& options, ImagePtr* preview,
                       std::vector<PsdImport::PendingText>* pending = nullptr);
void buildLayers(Context& ctx, const ClassList& children, std::vector<Built>& out, bool foldErase = true);

std::string layerName(const Class& node) {
    std::string name = node.string(tag("Desc"));
    if (name.empty()) name = node.string(tag("IRFN"));   // placed images show their file name
    return name;
}

/// The node's transform composed under its ancestors'; identity when both are.
Affine6 nodeTransform(const Context& ctx, const Class& node) {
    const auto own = affineOf(node.vector(tag("Xfrm")));
    return own ? compose(ctx.transform, *own) : ctx.transform;
}

std::optional<std::pair<int, int>> translationOf(const Affine6& m) {
    if (std::abs(m[0] - 1) < 1e-6 && std::abs(m[4] - 1) < 1e-6 && std::abs(m[1]) < 1e-6 && std::abs(m[3]) < 1e-6)
        return std::pair{int(std::lround(m[2])), int(std::lround(m[5]))};
    return std::nullopt;
}

void applyBlend(Context& ctx, const Class& node, Built& b, const std::string& name) {
    const affinity::Field* f = node.field(tag("Blnd"));
    const auto* e = f ? std::get_if<affinity::Enum>(&f->value) : nullptr;
    if (!e) return;
    if (b.group) b.passThrough = false;   // an explicit mode isolates a group
    if (auto mode = blendMode(e->id, e->version)) { b.blend = *mode; return; }
    const bool v6 = e->version >= 6;
    const auto approximate = [&](BlendMode mode, double scale, const char* affinityName, const char* ours) {
        b.blend = mode;
        b.opacity *= scale;
        ctx.notes.push_back("Layer \"" + name + "\": Affinity's " + affinityName + " blend mode was approximated as " + ours + ".");
    };
    if ((e->version == 0 && e->id == 21) || (v6 && e->id == 27)) approximate(BlendMode::Normal, 0.5, "Average", "Normal at half opacity");
    else if ((e->version == 0 && e->id == 22) || (v6 && e->id == 28)) approximate(BlendMode::Exclusion, 1, "Negation", "Exclusion");
    else if ((e->version == 0 && e->id == 23) || (v6 && e->id == 29)) approximate(BlendMode::Overlay, 1, "Reflect", "Overlay");
    else if ((e->version == 0 && e->id == 24) || (v6 && e->id == 30)) approximate(BlendMode::LinearLight, 1, "Glow", "Linear Light");
    else if (v6 && e->id == 1) approximate(BlendMode::Overlay, 1, "Pigment", "Overlay");
    else if ((e->version == 0 && e->id == 25) || (v6 && e->id == 32)) b.erase = !b.group;   // folded later; a group carrier stays Normal
    else ctx.notes.push_back("Layer \"" + name + "\": its Affinity blend mode has no counterpart; Normal was used.");
}

void applyCommon(Context& ctx, const Class& node, Built& b, const std::string& name) {
    b.visible = node.boolean(tag("Visi"), true);
    b.opacity = std::clamp(node.number(tag("Opac"), 1.0), 0.0, 1.0);
    // Fill opacity equals opacity for a layer without effects, and effects are not carried.
    if (!b.group) b.opacity *= std::clamp(node.number(tag("FOpc"), 1.0), 0.0, 1.0);
    applyBlend(ctx, node, b, name);
    if (const ClassList* effects = node.list(tag("FiEf"))) {
        int enabled = 0;
        for (const auto& fx : *effects) if (fx && fx->boolean(tag("Enab"), false)) enabled++;
        if (enabled) ctx.notes.push_back("Layer \"" + name + "\": " + std::to_string(enabled) + " layer effect(s) were left out.");
    }
}

/// Every vector outline under `node` (itself, or its children's) in document space, one shape group each.
void appendMaskPaths(const Class& node, const Affine6& base, int depth, VectorPath& into, int& group, int& appended, int& unsupported) {
    if (depth > 16) { unsupported++; return; }
    if (!node.boolean(tag("Visi"), true)) return;
    Affine6 composed = base;
    if (auto own = affineOf(node.vector(tag("Xfrm")))) composed = compose(base, *own);
    auto path = curvePath(node);
    std::string why;
    if (!path && node.type == tag("ShpN")) path = shapePath(node, &why);
    if (path) {
        transformPath(*path, composed);
        for (auto& s : path->subpaths) { s.group = group; s.op = VectorPath::Op::Add; into.subpaths.push_back(std::move(s)); }
        group++;
        appended++;
        return;
    }
    if (const ClassList* kids = node.list(tag("Chld")); kids && !kids->empty()) {
        for (const auto& k : *kids) if (k) appendMaskPaths(*k, composed, depth + 1, into, group, appended, unsupported);
        return;
    }
    unsupported++;
}

/// Whether every coordinate of the path is finite and within a range the rasterizer's integer maths can hold
/// (a damaged file's doubles can be anything).
bool plausiblePath(const VectorPath& path) {
    constexpr double limit = 1e7;
    for (const auto& s : path.subpaths)
        for (const auto& k : s.knots)
            for (double v : {k.x, k.y, k.inX, k.inY, k.outX, k.outY})
                if (!std::isfinite(v) || std::abs(v) > limit) return false;
    return true;
}

/// Coverage of `path` over the canvas (none when the path is implausible).
Plane canvasCoverage(const Context& ctx, const VectorPath& path) {
    if (!plausiblePath(path)) return std::make_shared<GrayImage>(ctx.width, ctx.height, 0);
    return rasterizeVectorMask(path, Rect(0, 0, ctx.width, ctx.height), 1, ctx.width, ctx.height);
}

/// The node's mask: an M8/M16 plane in its AdCh list, or a vector subtree acting as its clip, drawn as a raster.
void applyMasks(Context& ctx, const Class& node, Built& b, const std::string& name) {
    const ClassList* adjuncts = node.list(tag("AdCh"));
    if (!adjuncts) return;
    Affine6 ownerSpace = ctx.transform;
    if (auto own = affineOf(node.vector(tag("Xfrm")))) ownerSpace = compose(ctx.transform, *own);
    for (const auto& adjunct : *adjuncts) {
        if (!adjunct) continue;
        if (isAdjustmentOrFilter(adjunct->type)) {
            ctx.notes.push_back("Layer \"" + name + "\": an adjustment or live filter attached to it was left out.");
            ctx.leftOut++;
            continue;
        }
        if (b.mask) { ctx.notes.push_back("Layer \"" + name + "\": it has more than one mask; only the first was imported."); break; }
        const Class* dybm = adjunct->child(tag("Bitm"));
        if (!dybm) {
            VectorPath path;
            int group = 0, appended = 0, unsupported = 0;
            appendMaskPaths(*adjunct, ownerSpace, 0, path, group, appended, unsupported);
            if (appended == 0 && unsupported == 0) continue;
            if (unsupported > 0 || path.subpaths.empty()) { ctx.notes.push_back("Layer \"" + name + "\": a vector mask could not be read and was left out."); continue; }
            b.mask = canvasCoverage(ctx, path);
            b.maskX = b.maskY = 0;
            b.maskOutside = 0;
            continue;
        }
        std::string why;
        auto decoded = decodeBitmap(ctx.src, *dybm, name, ctx.options, ctx.notes, &why);
        if (!decoded || !decoded->mask) { ctx.notes.push_back("Layer \"" + name + "\": a mask could not be read and was left out."); continue; }
        // The adjunct lives in its owner's space: the owner's transform composes before its own.
        Affine6 m = ownerSpace;
        if (auto own = affineOf(adjunct->vector(tag("Xfrm")))) m = compose(ownerSpace, *own);
        if (auto t = translationOf(m)) {
            b.mask = decoded->mask;
            b.maskX = t->first;
            b.maskY = t->second;
        } else if (auto placed = resampleAffine(*decoded->mask, m)) {
            std::tie(b.mask, b.maskX, b.maskY) = *placed;
        } else {
            ctx.notes.push_back("Layer \"" + name + "\": its mask has a degenerate transform; placed approximately.");
            b.mask = decoded->mask;
            b.maskX = int(std::lround(m[2]));
            b.maskY = int(std::lround(m[5]));
        }
        b.maskOutside = 255;
    }
}

/// Paint of a PCrv or ShpN: solid fill (BFFl, or the oldest files' BFil) and solid stroke (LILn line style, LIFl
/// paint), drawn over the canvas as a straight-alpha layer. None when nothing visible is drawn.
struct Paint {
    std::optional<std::array<float, 4>> fill, stroke;
    VectorStroke line;
    bool gradient = false, texturedStroke = false;
};

std::optional<std::array<float, 4>> solidOf(const Class* descriptor, bool& gradient) {
    if (!descriptor) return std::nullopt;
    const Class* fill = descriptor->child(tag("FDeF"));
    if (!fill && (descriptor->type == tag("FilS") || descriptor->type == tag("FilN") || descriptor->type == tag("FilG"))) fill = descriptor;
    if (!fill || fill->type == tag("FilN")) return std::nullopt;
    if (fill->type == tag("FilG")) { gradient = true; return std::nullopt; }
    return readColor(fill->child(tag("Colr")));
}

bool readPaint(const Class& source, double transformScale, Paint& paint) {
    const Class* fillDescriptor = firstClass(source, tag("BFFl"));
    if (!fillDescriptor) fillDescriptor = firstClass(source, tag("BFil"));
    paint.fill = solidOf(fillDescriptor, paint.gradient);
    const Class* lineDescriptor = firstClass(source, tag("LILn"));
    if (!lineDescriptor || !lineDescriptor->child(tag("LDeL"))) lineDescriptor = firstClass(source, tag("LSty"));
    if (const Class* style = lineDescriptor ? lineDescriptor->child(tag("LDeL")) : nullptr) {
        const double weight = style->number(tag("Wght"), 0.0);
        uint8_t lineStyle = 1;
        if (const affinity::Field* d = style->field(tag("Data")))
            if (const auto* blob = std::get_if<affinity::CurveArray>(&d->value); blob && blob->bytes.size() >= 12) lineStyle = blob->bytes[10];
        if (weight > 0 && lineStyle != 0) {
            bool strokeGradient = false;
            auto color = solidOf(firstClass(source, tag("LIFl")), strokeGradient);
            if (!color) color = solidOf(firstClass(source, tag("PFil")), strokeGradient);
            if (strokeGradient) paint.gradient = true;
            if (color) {
                paint.stroke = color;
                paint.line.enabled = true;
                paint.line.width = lineDescriptor->boolean(tag("LDSc"), false) ? weight * transformScale : weight;
                switch (lineDescriptor->integer(tag("LDSa"), 0)) {
                case 1: paint.line.align = VectorStroke::Align::Inside; break;
                case 2: paint.line.align = VectorStroke::Align::Outside; break;
                default: paint.line.align = VectorStroke::Align::Center; break;
                }
                if (lineStyle == 2) { paint.line.dashes = style->vector(tag("Patn")); paint.line.dashOffset = style->number(tag("Phse"), 0.0); }
                if (lineStyle == 3) paint.texturedStroke = true;
                paint.line.miterLimit = std::max(1.0, source.number(tag("CnML"), paint.line.miterLimit));
            }
        }
    }
    return paint.fill || paint.stroke;
}

/// Draws `path` (document space) with the node's paint; none when nothing shows.
std::optional<Built> drawVector(Context& ctx, const Class& node, const std::string& name, VectorPath path, const Affine6& m, std::string* why) {
    transformPath(path, m);
    const double det = m[0] * m[4] - m[1] * m[3];
    const double scale = std::isfinite(det) && det != 0 ? std::sqrt(std::abs(det)) : 1.0;
    Paint paint;
    bool painted = readPaint(node, scale, paint);
    if (!painted) {
        // Symbol instances may keep their paint on the defining sibling.
        for (const char* ring : {"SLnk", "GLnk", "DLnk", "CLnk"}) {
            const Class* link = node.child(tagOf(ring));
            const ClassList* members = link ? link->list(tag("ILOb")) : nullptr;
            if (!members) continue;
            for (const auto& member : *members)
                if (member && member.get() != &node && readPaint(*member, scale, paint)) { painted = true; break; }
            if (painted) break;
        }
    }
    if (!painted) { *why = paint.gradient ? "is filled with a gradient, which is not read yet" : "has no visible fill or stroke"; return std::nullopt; }
    if (paint.gradient) ctx.notes.push_back("Layer \"" + name + "\": a gradient fill or stroke was left out.");
    if (paint.texturedStroke) ctx.notes.push_back("Layer \"" + name + "\": Affinity's textured brush stroke was drawn as a solid stroke.");

    if (!plausiblePath(path) || !std::isfinite(paint.line.width) || paint.line.width > 1e6) { *why = "has geometry outside any plausible range"; return std::nullopt; }
    // Draw within the canvas: the path's bounds (handles included) grown by the stroke.
    double x0 = 1e18, y0 = 1e18, x1 = -1e18, y1 = -1e18;
    for (const auto& s : path.subpaths)
        for (const auto& k : s.knots)
            for (auto [px, py] : {std::pair{k.x, k.y}, {k.inX, k.inY}, {k.outX, k.outY}}) { x0 = std::min(x0, px); y0 = std::min(y0, py); x1 = std::max(x1, px); y1 = std::max(y1, py); }
    const double grow = (paint.stroke ? paint.line.width * 2 : 0) + 2;
    const int left = std::max(0, int(std::floor(x0 - grow))), top = std::max(0, int(std::floor(y0 - grow)));
    const int right = std::min(ctx.width, int(std::ceil(x1 + grow))), bottom = std::min(ctx.height, int(std::ceil(y1 + grow)));
    if (right <= left || bottom <= top) { *why = "lies outside the canvas"; return std::nullopt; }
    const int w = right - left, h = bottom - top;
    const Rect region(left, top, w, h);
    auto image = std::make_shared<Image>(w, h);
    const auto paintOver = [&](const GrayImage& coverage, const std::array<float, 4>& c) {
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                const double sa = coverage.at(x, y) / 255.0 * c[3];
                if (sa <= 0) continue;
                uint8_t* o = image->pixel(x, y);
                const double da = o[3] / 255.0, oa = sa + da * (1 - sa);
                for (int k = 0; k < 3; k++) o[k] = uint8_t(std::lround((c[size_t(k)] * 255 * sa + o[k] * da * (1 - sa)) / oa));
                o[3] = uint8_t(std::lround(oa * 255));
            }
    };
    if (paint.fill) {
        VectorPath closed = path;
        for (auto& s : closed.subpaths) s.closed = true;
        paintOver(*rasterizeVectorMask(closed, region, 1, w, h), *paint.fill);
    }
    if (paint.stroke) paintOver(*rasterizeVectorStroke(path, paint.line, region, 1, w, h), *paint.stroke);
    if (alphaBounds(*image).isEmpty()) { *why = "draws nothing visible"; return std::nullopt; }
    Built b;
    b.name = name;
    b.image = image;
    b.x = left;
    b.y = top;
    applyCommon(ctx, node, b, name);
    applyMasks(ctx, node, b, name);
    return b;
}

std::optional<Built> buildShapeNode(Context& ctx, const Class& node, const std::string& name, std::string* why) {
    auto path = node.type == tag("ShpN") ? shapePath(node, why) : curvePath(node);
    if (!path) { if (why->empty()) *why = "has no readable outline"; return std::nullopt; }
    return drawVector(ctx, node, name, std::move(*path), nodeTransform(ctx, node), why);
}

// ------------------------------------------------------------------------------------------------ text

/// One code point of UTF-8 at `at` (a malformed byte reads as itself); `used` is its length.
uint32_t utf8At(const std::string& s, size_t at, size_t& used) {
    const auto b = [&](size_t i) { return i < s.size() ? uint8_t(s[i]) : uint8_t(0); };
    const uint8_t c = b(at);
    used = 1;
    if (c < 0x80) return c;
    int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
    if (!extra || at + size_t(extra) >= s.size() + 1) return c;
    uint32_t cp = c & (0x3F >> extra);
    for (int i = 1; i <= extra; i++) {
        if ((b(at + size_t(i)) & 0xC0) != 0x80) return c;
        cp = cp << 6 | (b(at + size_t(i)) & 0x3F);
    }
    used = size_t(extra) + 1;
    return cp;
}

/// A glyph run's caps from its OpenType feature list (OtAt > Setn): Affinity's own "CAP\1" is All Caps, the
/// small-caps features Small Caps; none when the run has no feature list (it keeps the previous run's).
std::optional<TextRun::Caps> runCaps(const Class& item) {
    const ClassList* objects = item.list(tag("Objs"));
    if (!objects) return std::nullopt;
    for (const auto& object : *objects) {
        if (!object || object->type != tag("OtAt")) continue;
        TextRun::Caps caps = TextRun::Caps::Normal;
        if (const ClassList* settings = object->list(tag("Setn")))
            for (const auto& setting : *settings) {
                if (!setting || setting->integer(tag("Valu"), 0) == 0) continue;
                const auto feature = uint32_t(setting->integer(tag("Feat"), 0));
                if (feature == tag("CAP\x01")) return TextRun::Caps::All;
                if (feature == tag("smcp") || feature == tag("c2sc") || feature == tag("pcap") || feature == tag("c2pc")) caps = TextRun::Caps::Small;
            }
        return caps;
    }
    return std::nullopt;
}

/// A glyph-run item's style (DFnt family, weight, italic; Doub[0] size; the first fill's colour) over `run`.
void applyRunItem(const Class& item, TextRun& run, double& alpha) {
    if (const Class* font = item.child(tag("DFnt"))) {
        const std::string family = font->string(tag("Famy"));
        if (!family.empty()) run.fontFamily = family;
        const int64_t weight = font->integer(tag("Wegt"), 400);
        run.bold = weight >= 600;
        run.weight = weight >= 100 && weight <= 900 && weight != 400 && weight != 700 ? int(weight) : 0;
        run.italic = font->boolean(tag("Ital"), false);
    }
    const auto doubles = item.vector(tag("Doub"));
    if (!doubles.empty() && doubles[0] > 0 && std::isfinite(doubles[0])) run.fontSize = doubles[0];
    if (const ClassList* fills = item.list(tag("Objs")); fills && !fills->empty() && fills->front())
        if (const Class* fill = fills->front()->child(tag("FDeF")))
            if (auto c = readColor(fill->child(tag("Colr")))) { run.red = (*c)[0]; run.green = (*c)[1]; run.blue = (*c)[2]; alpha = (*c)[3]; }
}

/// Artistic (TxtA) or frame (TxtF) text as a NekoPhoto text layer: the story's blocks (paragraphs) joined with
/// line breaks, its glyph runs as TextRuns (the run Indx values end each run, in code points of the block), the
/// first paragraph's alignment, and the frame (TxtH > FrmB, node-local) placed through the node's transform.
std::optional<Built> buildText(Context& ctx, const Class& node, const std::string& name, std::string* why) {
    const Class* story = node.child(tag("StSt"));
    const ClassList* blocks = story ? story->list(tag("Blok")) : nullptr;
    const Class* frame = node.child(tag("TxtH"));
    auto box = frame ? frame->vector(tag("FrmB")) : std::vector<double>{};
    if (!blocks || blocks->empty() || box.size() != 4) { *why = "is text laid out in a way that is not read yet (on a path, say)"; return std::nullopt; }

    LayerText text;
    std::vector<TextRun> runs;
    TextRun carry;
    carry.fontFamily = "Arial";
    carry.fontSize = 12;
    TextRun::Caps carryCaps = TextRun::Caps::Normal;
    double firstAlpha = 1;
    bool styled = false, spacing = false, justified = false;
    int64_t align = -1;
    const auto push = [&](TextRun run, int length) {
        if (length <= 0) return;
        run.length = length;
        runs.push_back(run);
    };
    for (const auto& block : *blocks) {
        if (!block) continue;
        std::string raw;
        if (const Class* glyphs = block->child(tag("Glyp"))) raw = glyphs->string(tag("Utf8"));
        if (!text.text.empty()) {
            // The break between blocks belongs to no wire run: the previous run takes it.
            text.text += '\n';
            if (!runs.empty()) runs.back().length++;
            else push(carry, 1);
        }
        // Paragraph and line separators become '\n', NULs drop; unitAt maps each code point boundary of the block
        // (run Indx counts code points, the trailing NUL included) to UTF-16 units of the output.
        std::vector<int> unitAt{0};
        int local = 0;
        for (size_t at = 0; at < raw.size();) {
            size_t used = 1;
            const uint32_t cp = utf8At(raw, at, used);
            if (cp == 0x2029 || cp == 0x2028) { text.text += '\n'; local++; }
            else if (cp != 0) { text.text.append(raw, at, used); local += cp > 0xFFFF ? 2 : 1; }
            unitAt.push_back(local);
            at += used;
        }
        const int count = int(unitAt.size()) - 1;
        int prev = 0;
        if (const Class* glyphAtts = block->child(tag("GAtt")))
            if (const ClassList* wireRuns = glyphAtts->list(tag("Runs")))
                for (const auto& run : *wireRuns) {
                    if (!run) continue;
                    const int end = std::clamp(int(run->integer(tag("Indx"), count)), prev, count);
                    TextRun style = carry;
                    if (const Class* item = run->child(tag("Item"))) {
                        double alpha = 1;
                        applyRunItem(*item, style, alpha);
                        if (!styled) firstAlpha = alpha;
                        styled = true;
                        if (auto caps = runCaps(*item)) carryCaps = *caps;
                    }
                    style.caps = carryCaps;
                    push(style, unitAt[size_t(end)] - unitAt[size_t(prev)]);
                    carry = style;
                    prev = end;
                }
        if (prev < count) { TextRun style = carry; style.caps = carryCaps; push(style, unitAt[size_t(count)] - unitAt[size_t(prev)]); }
        if (const Class* paragraphAtts = block->child(tag("PAtt")))
            if (const ClassList* wireRuns = paragraphAtts->list(tag("Runs")))
                for (const auto& run : *wireRuns) {
                    const Class* item = run ? run->child(tag("Item")) : nullptr;
                    if (!item) continue;
                    const auto ints = item->vector(tag("Ints"));
                    const int64_t a = ints.empty() ? 0 : int64_t(ints[0]);
                    if (align < 0) align = a;
                    if (a == 3) justified = true;
                    const auto d = item->vector(tag("Doub"));
                    for (size_t i = 2; i < d.size() && i <= 6; i++) if (std::abs(d[i]) > 0.01) spacing = true;
                }
    }
    if (text.text.find_first_not_of(" \t\r\n") == std::string::npos || !styled || runs.empty()) { *why = "is empty text"; return std::nullopt; }

    // The frame through the node's transform: axis-aligned, with the scale folded into the sizes. A rotation or
    // shear is approximated (drawn upright).
    const Affine6 m = nodeTransform(ctx, node);
    const double sx = std::hypot(m[0], m[3]), sy = std::hypot(m[1], m[4]), scale = (sx + sy) / 2;
    if (!(scale > 0.01) || !std::isfinite(scale)) { *why = "is text with a degenerate transform"; return std::nullopt; }
    const double angle = std::atan2(m[3], m[0]);
    const bool even = std::abs(sx - sy) <= 0.05 * scale && std::abs(m[0] * m[1] + m[3] * m[4]) <= 0.05 * scale * scale;   // no shear
    const bool upright = std::abs(angle) <= 0.003 && even;
    const bool turned = !upright && even && node.type == tag("TxtA");
    // Turned point text keeps its node-local frame at the uniform scale; the layer is turned about it.
    const Affine6 frameMap = turned ? Affine6{scale, 0, 0, 0, scale, 0} : m;
    double x0 = 1e18, y0 = 1e18, x1 = -1e18, y1 = -1e18;
    for (auto [px, py] : {std::pair{box[0], box[1]}, {box[2], box[1]}, {box[0], box[3]}, {box[2], box[3]}}) {
        const Point p = apply(frameMap, px, py);
        x0 = std::min(x0, p.x); y0 = std::min(y0, p.y); x1 = std::max(x1, p.x); y1 = std::max(y1, p.y);
    }
    for (auto& r : runs) r.fontSize = std::clamp(r.fontSize * scale, 1.0, 4000.0);
    text.runs = runs;
    const TextRun& first = runs.front();
    text.fontFamily = first.fontFamily;
    text.fontSize = first.fontSize;
    text.bold = first.bold;
    text.italic = first.italic;
    text.red = first.red; text.green = first.green; text.blue = first.blue;
    text.alignment = align == 1 ? 1 : align == 2 ? 2 : 0;
    const bool boxed = node.type == tag("TxtF");
    if (boxed) { text.boxWidth = std::max(1.0, x1 - x0); text.boxHeight = std::max(1.0, y1 - y0); }
    settleTextRuns(text);

    Built b;
    b.name = name;
    b.text = text;
    b.placement.left = x0;
    b.placement.top = y0;
    b.placement.width = x1 - x0;
    b.placement.boxed = boxed;
    b.placement.align = text.alignment;
    if (!boxed) {
        const double ascent = frame->number(tag("ArtV"), 0.0) * (turned ? scale : sy);
        if (ascent > 0) b.placement.baseline = y0 + ascent;
    }
    applyCommon(ctx, node, b, name);
    b.opacity *= std::clamp(firstAlpha, 0.0, 1.0);
    applyMasks(ctx, node, b, name);
    if (turned) {
        b.placement.rotation = angle * 180 / M_PI;
        b.placement.originX = m[2];
        b.placement.originY = m[5];
    } else if (!upright) {
        ctx.notes.push_back("Layer \"" + name + "\": the text's rotation, shear or uneven scale was left out (it is drawn upright).");
    }
    if (justified) ctx.notes.push_back("Layer \"" + name + "\": justified text is aligned left.");
    if (spacing) ctx.notes.push_back("Layer \"" + name + "\": paragraph indents and spacing were left out.");
    return b;
}

Built makeGroup(const std::string& name) { Built g; g.name = name; g.group = true; return g; }

/// A canvas mask clipping to the axis-aligned box.
void clipToBox(Context& ctx, Built& group, double x0, double y0, double x1, double y1) {
    const int mx = int(std::lround(x0)), my = int(std::lround(y0)), mw = int(std::lround(x1 - x0)), mh = int(std::lround(y1 - y0));
    if (mw <= 0 || mh <= 0 || !plausibleSize(mw, mh)) return;
    group.mask = std::make_shared<GrayImage>(mw, mh, 255);
    group.maskX = mx;
    group.maskY = my;
    group.maskOutside = 0;
    (void)ctx;
}

Built buildGroup(Context& ctx, const Class& node, const std::string& name) {
    Built group = makeGroup(name.empty() ? "Group" : name);
    applyCommon(ctx, node, group, group.name);
    if (const ClassList* kids = node.list(tag("Chld"))) {
        const Affine6 saved = ctx.transform;
        ctx.transform = nodeTransform(ctx, node);
        buildLayers(ctx, *kids, group.children);
        ctx.transform = saved;
    }
    applyMasks(ctx, node, group, group.name);
    return group;
}

/// An artboard: a group at its layout position, its own rectangle as the bottom layer, clipped to its bounds.
Built buildArtboard(Context& ctx, const Class& node, const std::string& name) {
    Built group = makeGroup(name);
    group.visible = node.boolean(tag("Visi"), true);
    group.opacity = std::clamp(node.number(tag("Opac"), 1.0), 0.0, 1.0);
    std::string why;
    if (auto background = buildShapeNode(ctx, node, name + " Background", &why)) {
        background->visible = true;
        background->opacity = 1;
        background->blend = BlendMode::Normal;
        background->mask = nullptr;
        group.children.push_back(std::move(*background));
    }
    const Affine6 saved = ctx.transform;
    ctx.transform = nodeTransform(ctx, node);
    const Affine6 m = ctx.transform;
    if (const ClassList* kids = node.list(tag("Chld"))) buildLayers(ctx, *kids, group.children);
    ctx.transform = saved;
    const auto box = node.vector(tag("ShpB"));
    if (box.size() == 4 && box[2] > box[0] && box[3] > box[1]) {
        double x0 = 1e18, y0 = 1e18, x1 = -1e18, y1 = -1e18;
        for (auto [sx, sy] : {std::pair{box[0], box[1]}, {box[2], box[1]}, {box[0], box[3]}, {box[2], box[3]}}) {
            const Point p = apply(m, sx, sy);
            x0 = std::min(x0, p.x); y0 = std::min(y0, p.y); x1 = std::max(x1, p.x); y1 = std::max(y1, p.y);
        }
        clipToBox(ctx, group, x0, y0, x1, y1);
    }
    return group;
}

/// A placed embedded document (EmbR > EmCn > EmbC names its edc/<n> stream): read and flattened.
Straight flattenEmbedded(Context& ctx, const Class& embr, const std::string& name) {
    if (ctx.embedDepth >= maxEmbedDepth) { ctx.notes.push_back("Layer \"" + name + "\": embedded documents nest too deeply; left out."); return nullptr; }
    const Class* container = embr.child(tag("EmCn"));
    const affinity::Field* f = container ? container->field(tag("EmbC")) : nullptr;
    const auto* ref = f ? std::get_if<affinity::Embedded>(&f->value) : nullptr;
    if (!ref || ref->data.empty()) return nullptr;
    auto stream = ctx.src.container.streams.find(ref->data);
    if (stream == ctx.src.container.streams.end()) return nullptr;
    try {
        const auto nested = extractStream(ctx.src.bytes, stream->second, ref->data, nullptr);
        std::span<const uint8_t> span(nested);
        if (span.size() > 8 && span[0] == 'E' && span[1] == 'm' && span[2] == 'D' && span[3] == 'c') span = span.subspan(8);   // "EmDc" + u32
        std::vector<std::string> inner;
        const Document doc = readContainer(span, inner, ctx.embedDepth + 1, ctx.options, nullptr);
        ctx.notes.push_back("Layer \"" + name + "\": an embedded document was flattened to pixels.");
        return straightCopy(*renderFlattened(doc));
    } catch (const std::exception& e) {
        ctx.notes.push_back("Layer \"" + name + "\": its embedded document could not be read (" + e.what() + ").");
        return nullptr;
    }
}

/// Erase carriers become a mask on a new isolated group holding the layers beneath them: 255 minus the coverage
/// the carrier would paint. Carriers the construction cannot express stay Normal, with a note.
void foldErase(Context& ctx, std::vector<Built>& out, bool allowed) {
    for (size_t i = 0; i < out.size(); i++) {
        Built& carrier = out[i];
        if (!carrier.erase) continue;
        carrier.erase = false;
        const std::string name = carrier.name;
        const auto decline = [&](const std::string& why) { ctx.notes.push_back("Layer \"" + name + "\": Affinity's Erase blend mode " + why + "; shown as Normal."); };
        if (!carrier.visible || carrier.opacity <= 0) continue;
        if (!allowed) { decline("is not supported inside a clipping group"); continue; }
        if (carrier.clipped) { decline("is not supported on a clipped layer"); continue; }
        if (i + 1 < out.size() && out[i + 1].clipped) { decline("is not supported on the base of a clipping group"); continue; }
        if (i == 0) { decline("has no layers beneath it"); continue; }
        if (!carrier.image) { decline("is empty"); continue; }
        auto keep = std::make_shared<GrayImage>(ctx.width, ctx.height, 255);
        bool erases = false;
        const Image& px = *carrier.image;
        for (int y = std::max(0, carrier.y); y < std::min(ctx.height, carrier.y + px.height()); y++)
            for (int x = std::max(0, carrier.x); x < std::min(ctx.width, carrier.x + px.width()); x++) {
                double coverage = px.pixel(x - carrier.x, y - carrier.y)[3] / 255.0 * carrier.opacity;
                if (carrier.mask) {
                    const int mx = x - carrier.maskX, my = y - carrier.maskY;
                    coverage *= (mx >= 0 && my >= 0 && mx < carrier.mask->width() && my < carrier.mask->height() ? carrier.mask->at(mx, my) : carrier.maskOutside) / 255.0;
                }
                const auto v = uint8_t(std::lround(std::clamp(1 - coverage, 0.0, 1.0) * 255));
                keep->at(x, y) = v;
                erases = erases || v < 255;
            }
        if (!erases) { decline("is empty"); continue; }
        Built wrapper = makeGroup(name + " (Erase)");
        wrapper.passThrough = false;
        for (size_t k = 0; k < i; k++) wrapper.children.push_back(std::move(out[k]));
        wrapper.mask = keep;
        out.erase(out.begin(), out.begin() + std::ptrdiff_t(i) + 1);
        out.insert(out.begin(), std::move(wrapper));
        i = 0;
        ctx.notes.push_back("Layer \"" + name + "\": Affinity's Erase blend mode became a mask on a new group holding the layers beneath it.");
    }
}

void buildLayers(Context& ctx, const ClassList& children, std::vector<Built>& out, bool foldEraseHere) {
    for (const auto& child : children) {
        if (!child) continue;
        if (++ctx.layerCount > maxLayers) fail("The Affinity document has an implausible number of layers.");
        const Class& node = *child;
        // Shared classes are references, so a damaged file can make a layer its own descendant: skip the repeat.
        if (std::find(ctx.open.begin(), ctx.open.end(), &node) != ctx.open.end() || ctx.open.size() >= maxNesting) {
            ctx.notes.push_back("Layers nested in a loop or too deeply were left out; the file may be damaged.");
            continue;
        }
        ctx.open.push_back(&node);
        struct Close { std::vector<const Class*>& open; ~Close() { open.pop_back(); } } close{ctx.open};
        const std::string name = layerName(node);
        const std::string display = name.empty() ? "Layer" : name;
        const uint32_t type = node.type;
        const Class* bitmap = node.child(tag("Bitm"));

        const auto leaveOut = [&](const std::string& why) {
            ctx.notes.push_back("Layer \"" + display + "\" " + why + "; it was left out.");
            if (node.boolean(tag("Visi"), true)) ctx.leftOut++;
        };
        // Children of a content node live in its local space and clip to it (Affinity nests clipped layers inside
        // their base; NekoPhoto keeps them as clipped siblings above it). The 1.x generation (document versions
        // below 10) does not clip children to a base that painted nothing.
        const auto emitClipped = [&](bool baseMissing) {
            const ClassList* kids = node.list(tag("Chld"));
            if (!kids || kids->empty()) return;
            if (baseMissing && ctx.documentVersion >= 10) {
                ctx.notes.push_back("Layer \"" + display + "\": " + std::to_string(kids->size()) + " layer(s) clipped to it were left out with it.");
                return;
            }
            const Affine6 saved = ctx.transform;
            ctx.transform = nodeTransform(ctx, node);
            std::vector<Built> clipped;
            buildLayers(ctx, *kids, clipped, baseMissing);
            ctx.transform = saved;
            for (auto& layer : clipped) {
                if (!baseMissing) {
                    if (layer.group) ctx.notes.push_back("Layer \"" + layer.name + "\": a group clipped to a layer is shown unclipped.");
                    else layer.clipped = true;
                }
                out.push_back(std::move(layer));
            }
        };

        if (isArtboard(node)) { out.push_back(buildArtboard(ctx, node, display)); continue; }
        if (isAdjustmentOrFilter(type)) {
            leaveOut("is an Affinity adjustment or live filter, which is not read yet");
            emitClipped(true);
            continue;
        }
        if (type == tag("Comp")) {
            // A compound shape's boolean result is baked in its own 'crvs'; without it, its operands as a group.
            std::string why;
            if (auto path = curvePath(node))
                if (auto layer = drawVector(ctx, node, display, std::move(*path), nodeTransform(ctx, node), &why)) { out.push_back(std::move(*layer)); continue; }
            out.push_back(buildGroup(ctx, node, name));
            continue;
        }
        if (type == tag("Grup") || (node.field(tag("Chld")) && !bitmap && type != tag("TxtA") && type != tag("TxtF") && type != tag("PCrv") && type != tag("ShpN"))) {
            out.push_back(buildGroup(ctx, node, name));
            continue;
        }
        if (bitmap) {
            Straight pixels;
            bool approximate = false, maskPlane = false;
            std::string why;
            if (bitmap->type == tag("EmbR")) pixels = flattenEmbedded(ctx, *bitmap, display);
            else if (auto decoded = decodeBitmap(ctx.src, *bitmap, display, ctx.options, ctx.notes, &why)) {
                if (decoded->rgba) { pixels = decoded->rgba; approximate = decoded->approximateColor; }
                else maskPlane = true;
            }
            Affine6 m = nodeTransform(ctx, node);
            // A modern embedded document is centre-anchored: the translation places the middle of its canvas.
            if (bitmap->type == tag("EmbR") && pixels && ctx.documentVersion >= 20) {
                const double hw = pixels->width() / 2.0, hh = pixels->height() / 2.0;
                m[2] -= m[0] * hw + m[1] * hh;
                m[5] -= m[3] * hw + m[4] * hh;
            }
            std::optional<std::tuple<Straight, int, int>> placed;
            if (pixels) {
                if (auto t = translationOf(m)) placed = std::tuple{pixels, t->first, t->second};
                else placed = resampleAffine(*pixels, m);
            }
            if (!placed) {
                leaveOut(maskPlane ? "is an Affinity adjustment or live filter, which is not read yet"
                                   : pixels ? "has a degenerate transform" : why.empty() ? "could not be read" : why);
                emitClipped(true);
                continue;
            }
            if (approximate) ctx.notes.push_back("Layer \"" + display + "\": CMYK converted without a colour profile (approximate).");
            Built b;
            b.name = display;
            std::tie(b.image, b.x, b.y) = *placed;
            applyCommon(ctx, node, b, display);
            applyMasks(ctx, node, b, display);
            out.push_back(std::move(b));
            emitClipped(false);
            continue;
        }
        if (type == tag("TxtA") || type == tag("TxtF")) {
            std::string why;
            if (auto text = buildText(ctx, node, display, &why)) {
                out.push_back(std::move(*text));
                emitClipped(false);
                continue;
            }
            leaveOut(why);
            emitClipped(true);
            continue;
        }
        if (type == tag("PCrv") || type == tag("ShpN")) {
            std::string why;
            auto base = buildShapeNode(ctx, node, display, &why);
            if (!base) { leaveOut(why); emitClipped(true); continue; }
            const ClassList* kids = node.list(tag("Chld"));
            if (!kids || kids->empty()) { out.push_back(std::move(*base)); continue; }
            // Children clip to the shape; when a group is among them (groups cannot clip), the shape and its children
            // go in an isolated group masked by the shape's outline instead.
            const Affine6 saved = ctx.transform;
            ctx.transform = nodeTransform(ctx, node);
            std::vector<Built> inner;
            buildLayers(ctx, *kids, inner, false);
            ctx.transform = saved;
            if (std::none_of(inner.begin(), inner.end(), [](const Built& l) { return l.group; })) {
                out.push_back(std::move(*base));
                for (auto& l : inner) { l.clipped = true; out.push_back(std::move(l)); }
                continue;
            }
            Built wrapper = makeGroup(display);
            wrapper.passThrough = false;
            wrapper.visible = base->visible;
            wrapper.opacity = base->opacity;
            wrapper.blend = base->blend;
            wrapper.mask = base->mask;
            wrapper.maskX = base->maskX;
            wrapper.maskY = base->maskY;
            wrapper.maskOutside = base->maskOutside;
            if (!wrapper.mask) {
                auto outline = node.type == tag("ShpN") ? shapePath(node, &why) : curvePath(node);
                if (outline) {
                    transformPath(*outline, nodeTransform(ctx, node));
                    for (auto& s : outline->subpaths) { s.group = 0; s.op = VectorPath::Op::Add; s.closed = true; }
                    wrapper.mask = canvasCoverage(ctx, *outline);
                    wrapper.maskOutside = 0;
                }
            }
            base->visible = true;
            base->opacity = 1;
            base->blend = BlendMode::Normal;
            base->mask = nullptr;
            wrapper.children.push_back(std::move(*base));
            for (auto& l : inner) wrapper.children.push_back(std::move(l));
            out.push_back(std::move(wrapper));
            continue;
        }
        leaveOut("is Affinity content that is not read yet");
    }
    if (std::any_of(out.begin(), out.end(), [](const Built& b) { return b.erase; })) foldErase(ctx, out, foldEraseHere);
}

bool anyPixels(const std::vector<Built>& layers) {
    for (const auto& l : layers) if ((l.image && !l.image->isEmpty()) || l.text || anyPixels(l.children)) return true;
    return false;
}

/// The Built tree into the document's flat, bottom-to-top list.
void emit(std::vector<Built>& layers, std::optional<Uuid> parent, Document& doc, std::vector<std::string>& notes, long long& total,
          std::vector<PsdImport::PendingText>& pending) {
    std::optional<Uuid> base;   // the nearest unclipped pixel layer below, for clipping
    for (auto& b : layers) {
        Layer layer;
        int lx = 0, ly = 0, lw = doc.width, lh = doc.height;
        if (b.group) {
            layer = Layer(b.name, doc.size());
            layer.isGroup = true;
            layer.passThrough = b.passThrough;
        } else if (b.text) {
            // A transparent pixel where the text goes, until the app draws it (as for an empty PSD type layer).
            lx = int(std::floor(b.placement.left));
            ly = int(std::floor(b.placement.top));
            lw = lh = 1;
            layer = Layer(Asset::make(std::make_shared<Image>(1, 1), b.name), Point(lx, ly));
            layer.text = *b.text;
            layer.textImage = layer.asset->image;
            b.placement.layer = layer.id;
            pending.push_back(b.placement);
        } else {
            if (!b.image || b.image->isEmpty()) continue;
            premultiply(*b.image);
            const PixelBounds bounds = alphaBounds(*b.image);
            if (bounds.isEmpty()) { layer = Layer(b.name, doc.size()); }
            else {
                auto image = (bounds.x0 == 0 && bounds.y0 == 0 && bounds.x1 == b.image->width() && bounds.y1 == b.image->height())
                    ? b.image : cropImage(*b.image, bounds.x0, bounds.y0, bounds.x1 - bounds.x0, bounds.y1 - bounds.y0);
                total += (long long)image->width() * image->height();
                if (total > Document::projectPixelBudget) fail("The layers exceed the gigapixel a project may hold.");
                lx = b.x + bounds.x0;
                ly = b.y + bounds.y0;
                lw = image->width();
                lh = image->height();
                layer = Layer(Asset::make(image, b.name), Point(lx, ly));
            }
        }
        layer.name = b.name;
        layer.visible = b.visible;
        layer.opacity = b.opacity;
        layer.blendMode = b.blend;
        layer.parentId = parent;
        if (b.text && b.mask) notes.push_back("Layer \"" + b.name + "\": the mask on this text was left out.");
        if (b.mask && !b.text && plausibleSize(lw, lh)) {
            // Over the layer's own grid (a group's is the canvas); beyond the stored plane, its outside value.
            auto mask = std::make_shared<GrayImage>(lw, lh, b.maskOutside);
            for (int y = 0; y < lh; y++) {
                const int sy = ly + y - b.maskY;
                if (sy < 0 || sy >= b.mask->height()) continue;
                for (int x = 0; x < lw; x++) {
                    const int sx = lx + x - b.maskX;
                    if (sx >= 0 && sx < b.mask->width()) mask->at(x, y) = b.mask->at(sx, sy);
                }
            }
            LayerMask lm;
            lm.asset = MaskAsset::make(mask);
            layer.mask = lm;
        }
        if (b.clipped && !b.group) {
            if (base) layer.maskSourceId = base;
            else notes.push_back("Layer \"" + b.name + "\": clipped to a layer that was left out; shown unclipped.");
        } else if (!b.group) {
            base = layer.id;
        } else {
            base.reset();
        }
        const Uuid id = layer.id;
        doc.layers.push_back(std::move(layer));
        if (b.group) emit(b.children, id, doc, notes, total, pending);
    }
}

/// Old files save a snapshot cache: a DyBm under the root's Snap subtree names a full-size PNG render.
std::shared_ptr<Image> snapshotRender(const Source& src, const affinity::Tree& tree, const PsdImportOptions& options) {
    const affinity::Field* snap = tree.root ? tree.root->field(tag("Snap")) : nullptr;
    if (!snap) return nullptr;
    std::vector<const Class*> pending;
    if (const auto* one = std::get_if<std::shared_ptr<Class>>(&snap->value)) pending.push_back(one->get());
    else if (const auto* list = std::get_if<ClassList>(&snap->value)) for (auto& c : *list) pending.push_back(c.get());
    Straight best;
    for (int guard = 0; !pending.empty() && guard < 4096; guard++) {
        const Class* node = pending.back();
        pending.pop_back();
        if (!node) continue;
        if (node->type == tag("DyBm")) {
            if (Straight s = decodeOriginal(src, *node, options); s && (!best || (long long)s->width() * s->height() > (long long)best->width() * best->height())) best = s;
            continue;
        }
        for (const auto& f : node->fields) {
            if (const auto* c = std::get_if<std::shared_ptr<Class>>(&f.value)) pending.push_back(c->get());
            else if (const auto* l = std::get_if<ClassList>(&f.value)) for (auto& c : *l) pending.push_back(c.get());
        }
    }
    return best;
}

/// A document of one layer: `image` (premultiplied) stretched over a `width` x `height` canvas.
Document singleLayer(const ImagePtr& image, int width, int height, const std::string& name) {
    Document doc(width, height);
    Layer layer(Asset::make(image, name), Point(0, 0));
    layer.transform.size = Size(width, height);
    doc.layers.push_back(std::move(layer));
    return doc;
}

Document buildDocument(const Source& src, const affinity::Tree& tree, std::vector<std::string>& notes, int embedDepth,
                       const PsdImportOptions& options, const ImagePtr& preview, bool* fellBack, std::vector<PsdImport::PendingText>& pending) {
    if (!tree.root) fail("The Affinity document tree is empty.");
    const Class* docNode = tree.root->child(tag("DocR"));
    if (!docNode) fail("The Affinity document has no document node.");
    const auto size = docNode->vector(tag("DfSz"));
    if (size.size() != 2) fail("The Affinity document has no canvas size.");
    int width = int(std::lround(size[0])), height = int(std::lround(size[1]));
    const ClassList* spreads = docNode->list(tag("Chld"));
    if (!spreads || spreads->empty() || !spreads->front()) fail("The Affinity document has no spread.");
    if (spreads->size() > 1) notes.push_back("The document has " + std::to_string(spreads->size()) + " pages or spreads; only the first was imported.");
    const Class& spread = *spreads->front();

    // The canvas: the spread's bounds (SprB) when sane; else the union of its artboards; else the page rectangle
    // (SpMd > PagR[0] > rctp), which follows canvas resizes where DfSz keeps the creation size.
    double originX = 0, originY = 0;
    {
        auto bounds = spread.vector(tag("SprB"));
        if (bounds.size() != 4 || !(bounds[2] > bounds[0]) || !(bounds[3] > bounds[1])) {
            bounds.clear();
            bool any = false;
            std::array<double, 4> box{};
            std::function<void(const Class&, const Affine6&, int)> walk = [&](const Class& n, const Affine6& parent, int depth) {
                const ClassList* kids = depth <= 64 ? n.list(tag("Chld")) : nullptr;
                if (!kids) return;
                for (const auto& k : *kids) {
                    if (!k) continue;
                    Affine6 m = parent;
                    if (auto own = affineOf(k->vector(tag("Xfrm")))) m = compose(parent, *own);
                    if (isArtboard(*k)) {
                        const auto b = k->vector(tag("ShpB"));
                        if (b.size() == 4)
                            for (auto [sx, sy] : {std::pair{b[0], b[1]}, {b[2], b[1]}, {b[0], b[3]}, {b[2], b[3]}}) {
                                const Point p = apply(m, sx, sy);
                                box = any ? std::array<double, 4>{std::min(box[0], p.x), std::min(box[1], p.y), std::max(box[2], p.x), std::max(box[3], p.y)}
                                          : std::array<double, 4>{p.x, p.y, p.x, p.y};
                                any = true;
                            }
                        continue;
                    }
                    walk(*k, m, depth + 1);
                }
            };
            walk(spread, identity(), 0);
            if (any) bounds.assign(box.begin(), box.end());
        }
        if (bounds.size() != 4)
            if (const Class* meta = spread.child(tag("SpMd")))
                if (const ClassList* pages = meta->list(tag("PagR")); pages && !pages->empty() && pages->front()) {
                    auto page = pages->front()->vector(tag("rctp"));
                    if (page.size() == 4 && page[2] > page[0] && page[3] > page[1]) bounds = page;
                }
        if (bounds.size() == 4) {
            const int w = int(std::lround(bounds[2] - bounds[0])), h = int(std::lround(bounds[3] - bounds[1]));
            if (plausibleSize(w, h)) { width = w; height = h; originX = bounds[0]; originY = bounds[1]; }
        }
    }
    if (!plausibleSize(width, height)) fail("The canvas exceeds the 100-megapixel budget.");

    Context ctx{src, options, notes, width, height, embedDepth};
    ctx.documentVersion = tree.documentVersion;
    ctx.transform[2] = -originX;
    ctx.transform[5] = -originY;
    std::vector<Built> built;
    if (const ClassList* layers = spread.list(tag("Chld"))) buildLayers(ctx, *layers, built);

    Document doc(width, height);
    if (const Class* units = tree.root->child(tag("UVCn"))) {
        const double ppi = units->number(tag("UPPI"), 0.0);
        if (ppi >= 1 && ppi <= 10000) doc.resolution = ppi;
    }
    if (!anyPixels(built)) {
        // Nothing decoded: a flat render beats a blank canvas. Old files' saved snapshot first, then the preview.
        if (fellBack) *fellBack = true;
        if (Straight snapshot = snapshotRender(src, tree, options)) {
            premultiply(*snapshot);
            notes.push_back("None of the layers could be read; the document's saved snapshot render was imported as one layer.");
            return singleLayer(snapshot, width, height, "Affinity snapshot");
        }
        if (preview) {
            notes.push_back("None of the layers could be read; the embedded preview (" + std::to_string(preview->width()) + " x " +
                            std::to_string(preview->height()) + ") was imported as one layer.");
            return singleLayer(preview, width, height, "Affinity preview");
        }
        fail("The Affinity document has no layers this reader can use.");
    }
    // Unless the spread is transparent (SprT), Affinity paints its background colour (BgrC, white by default)
    // behind every layer.
    if (!spread.boolean(tag("SprT"), false)) {
        std::array<float, 4> c{1, 1, 1, 1};
        if (auto stored = readColor(spread.child(tag("BgrC")))) c = *stored;
        if (c[3] > 0) {
            auto fill = std::make_shared<Image>(width, height);
            const uint8_t a = uint8_t(std::lround(c[3] * 255));
            fill->fill(uint8_t(std::lround(c[0] * c[3] * 255)), uint8_t(std::lround(c[1] * c[3] * 255)), uint8_t(std::lround(c[2] * c[3] * 255)), a);
            doc.layers.emplace_back(Asset::make(fill, "Background"), Point(0, 0));
        }
    }
    long long total = (long long)width * height;
    emit(built, std::nullopt, doc, notes, total, pending);
    if (ctx.leftOut > 0 && preview && embedDepth == 0) {
        // What was left out still shows in Affinity's own preview: kept hidden on top, for reference.
        Layer ref(Asset::make(preview, "Affinity preview (reference)"), Point(0, 0));
        ref.transform.size = Size(width, height);
        ref.visible = false;
        doc.layers.push_back(std::move(ref));
        notes.push_back("Affinity's own preview of the whole document is the hidden top layer, for comparing what was left out.");
    }
    return doc;
}

Document readContainer(std::span<const uint8_t> bytes, std::vector<std::string>& notes, int embedDepth, const PsdImportOptions& options, ImagePtr* previewOut,
                       std::vector<PsdImport::PendingText>* pendingOut) {
    if (bytes.size() < 4 || bytes[0] != 0x00 || bytes[1] != 0xFF || bytes[2] != 0x4B || bytes[3] != 0x41) fail("Not an Affinity document.");
    const Container container = parseContainer(bytes);
    if (container.version > newestVerifiedVersion)
        notes.push_back("The file was saved by a newer Affinity (container version " + std::to_string(container.version) + "); the import may be incomplete.");
    ImagePtr preview;
    try { preview = extractPreview(bytes, container); } catch (const std::exception&) {}
    if (previewOut) *previewOut = preview;
    const Source src{bytes, container};
    auto docStream = container.streams.find("doc.dat");
    if (docStream != container.streams.end()) {
        std::string why;
        try {
            const auto treeBytes = extractStream(bytes, docStream->second, "doc.dat", &notes);
            const affinity::Tree tree = affinity::parseTree(treeBytes);
            std::vector<PsdImport::PendingText> pending;
            Document doc = buildDocument(src, tree, notes, embedDepth, options, preview, nullptr, pending);
            if (!pending.empty() && !pendingOut) {
                // An embedded document is flattened here, where no fonts draw its text.
                notes.push_back("Text inside an embedded document is not drawn.");
            }
            if (pendingOut) *pendingOut = std::move(pending);
            return doc;
        } catch (const std::exception& e) {
            why = e.what();
        }
        if (!preview || embedDepth > 0) fail(why);
        notes.push_back("The layers could not be read (" + why + "); the embedded preview was imported instead.");
    } else if (!preview) {
        fail("The Affinity document has no document stream.");
    }
    return singleLayer(preview, preview->width(), preview->height(), "Affinity preview");
}

} // namespace

std::optional<PsdImport> importAffinityBytes(const std::vector<uint8_t>& file, std::string* error, const PsdImportOptions& options) {
    try {
        PsdImport result;
        ImagePtr preview;
        result.document = readContainer(file, result.notes, 0, options, &preview, &result.pendingTexts);
        result.composite = preview;
        result.realComposite = false;
        if (result.document.layers.empty()) { if (error) *error = "The file holds no layers this reader can use."; return std::nullopt; }
        return result;
    } catch (const std::bad_alloc&) {
        if (error) *error = "The Affinity document needs more memory than is available.";
    } catch (const std::exception& e) {
        if (error) *error = e.what();
    }
    return std::nullopt;
}

std::optional<PsdImport> importAffinity(const std::string& path, std::string* error, const PsdImportOptions& options) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) { if (error) *error = "The file could not be opened."; return std::nullopt; }
    const std::streamoff size = in.tellg();
    if (size < 0 || uint64_t(size) > (uint64_t(4) << 30)) { if (error) *error = "The file is too large."; return std::nullopt; }
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    in.seekg(0);
    if (!in.read(reinterpret_cast<char*>(bytes.data()), size)) { if (error) *error = "The file could not be read."; return std::nullopt; }
    return importAffinityBytes(bytes, error, options);
}

} // namespace nekophoto
