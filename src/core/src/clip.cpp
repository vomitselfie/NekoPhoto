// Clip Studio Paint projects (.clip), read by csp_clip_import.c (the CSFCHUNK container, its SQLite
// database and the layers' zlib tiles) and built into a Document the way the PSD import builds one. Checked
// against twelve real .clip files and the PSDs Clip Studio exported from them: every layer's pixels and
// placement match exactly.
//
// Clip Studio keeps each layer as a canvas-sized (or larger) bitmap, so every layer is cropped to what it
// paints. Blend modes Clip Studio has and the editor lacks take the nearest one, and the import says so.
#include "compositor/clip.h"
#include "compositor/parallel.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>

#ifdef COMPOSITOR_HAVE_SQLITE
#include "csp_clip_import.h"
#endif

namespace compositor {

bool isClipFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    char magic[8] = {};
    return in.read(magic, 8) && std::memcmp(magic, "CSFCHUNK", 8) == 0;
}

#ifdef COMPOSITOR_HAVE_SQLITE

namespace {

/// Clip Studio's blend mode numbers; `lossy` when the editor has no such mode and the nearest stands in.
BlendMode blendFor(int mode, bool* lossy) {
    *lossy = false;
    switch (mode) {
    case 0: case 30: return BlendMode::Normal;   // 30 is a folder's pass-through
    case 1: return BlendMode::Darken;
    case 2: return BlendMode::Multiply;
    case 3: return BlendMode::ColorBurn;
    case 7: return BlendMode::Lighten;
    case 8: return BlendMode::Screen;
    case 9: return BlendMode::ColorDodge;
    case 14: return BlendMode::Overlay;
    case 21: return BlendMode::Difference;
    case 23: return BlendMode::Hue;
    case 24: return BlendMode::Saturation;
    case 25: return BlendMode::Color;
    case 26: return BlendMode::Luminosity;
    default: break;
    }
    switch (mode) {
    case 4: return BlendMode::LinearBurn;
    case 5: return BlendMode::Subtract;
    case 6: return BlendMode::DarkerColor;
    case 11: return BlendMode::LinearDodge;           // add
    case 13: return BlendMode::LighterColor;
    case 15: return BlendMode::SoftLight;
    case 16: return BlendMode::HardLight;
    case 17: return BlendMode::VividLight;
    case 18: return BlendMode::LinearLight;
    case 19: return BlendMode::PinLight;
    case 20: return BlendMode::HardMix;
    case 22: return BlendMode::Exclusion;
    case 27: return BlendMode::Divide;
    default: break;
    }
    // Clip Studio's own: glow dodge and add (glow) have no Photoshop counterpart.
    *lossy = true;
    switch (mode) {
    case 10: return BlendMode::ColorDodge;            // glow dodge
    case 12: return BlendMode::LinearDodge;           // add (glow)
    default: return BlendMode::Normal;
    }
}

struct Closer { void operator()(csp_clip_document* d) const { csp_clip_close(d); } };
struct Freer { void operator()(uint8_t* p) const { csp_clip_free(p); } };

} // namespace

std::optional<PsdImport> importClip(const std::string& path, std::string* error) {
    char message[512] = {};
    csp_clip_document* opened = nullptr;
    csp_clip_options options;
    csp_clip_default_options(&options);
    options.max_pixels = uint64_t(Document::pixelBudget);   // any one bitmap, as for a PSD layer
    if (csp_clip_open(path.c_str(), &options, &opened, message, sizeof message) != CSP_CLIP_OK) {
        if (error) *error = message[0] ? message : "The file could not be read as a Clip Studio project.";
        return std::nullopt;
    }
    std::unique_ptr<csp_clip_document, Closer> doc(opened);
    const csp_clip_document_info& info = *csp_clip_get_info(doc.get());
    if (info.width <= 0 || info.height <= 0 || info.width > maxImageSide || info.height > maxImageSide
        || (long long)info.width * info.height > Document::pixelBudget) {
        if (error) *error = "The canvas exceeds the 100-megapixel budget.";
        return std::nullopt;
    }
    PsdImport result;
    Document& document = result.document;
    document = Document(info.width, info.height);
    if (info.dpi > 0) document.resolution = info.dpi;
    std::vector<std::string>& notes = result.notes;
    if (info.channel_bytes > 1) notes.push_back("The file keeps more than 8 bits per channel; its layers could not be read.");

    const size_t count = csp_clip_get_layer_count(doc.get());
    std::vector<std::optional<Uuid>> ids(count);   // each reader layer's id in the document, for parents
    std::map<int, int> skipped;                    // layer kinds with no pixels, by kind
    long long total = 0;
    int converted = 0;
    for (size_t i = 0; i < count; i++) {
        const csp_clip_layer& l = *csp_clip_get_layer(doc.get(), i);
        const std::string name = l.name && *l.name ? l.name : (l.is_group ? "Folder" : "Layer");
        std::optional<Uuid> parent;
        if (l.parent_index >= 0 && size_t(l.parent_index) < i) parent = ids[size_t(l.parent_index)];
        if (l.parent_index >= 0 && !parent) continue;   // inside a folder that was skipped
        bool lossy = false;
        const BlendMode blend = blendFor(l.raw_composite, &lossy);

        Layer layer;
        if (l.is_group) {
            layer = Layer(name, document.size());
            layer.isGroup = true;
        } else {
            uint8_t* raw = nullptr;
            int w = 0, h = 0, x = 0, y = 0;
            size_t stride = 0;
            const int rc = csp_clip_decode_layer_rgba(doc.get(), i, &raw, &w, &h, &stride, &x, &y, message, sizeof message);
            std::unique_ptr<uint8_t, Freer> pixels(raw);
            if (rc != CSP_CLIP_OK) {
                if (rc == CSP_CLIP_ERR_NOT_FOUND && l.kind != CSP_CLIP_LAYER_RASTER) { skipped[l.kind]++; continue; }
                notes.push_back("Layer \"" + name + "\": its pixels could not be read (" + message + ").");
                continue;
            }
            if (w <= 0 || h <= 0 || w > maxImageSide || h > maxImageSide) {
                notes.push_back("Layer \"" + name + "\": its bitmap is larger than a layer may be; skipped.");
                continue;
            }
            // Straight alpha in, premultiplied out, on every core; each row notes where it paints, for the crop.
            Image full(w, h);
            std::vector<int> first(size_t(h), w), last(size_t(h), -1);
            parallelFor(0, h, 16, [&](int y0, int y1) {
                for (int row = y0; row < y1; row++) {
                    const uint8_t* s = raw + size_t(row) * stride;
                    uint8_t* d = full.row(row);
                    for (int col = 0; col < w; col++, s += 4, d += 4) {
                        const unsigned a = s[3];
                        d[0] = uint8_t((s[0] * a + 127) / 255); d[1] = uint8_t((s[1] * a + 127) / 255); d[2] = uint8_t((s[2] * a + 127) / 255); d[3] = uint8_t(a);
                        if (a) { if (first[size_t(row)] == w) first[size_t(row)] = col; last[size_t(row)] = col; }
                    }
                }
            });
            pixels.reset();
            PixelBounds b;
            for (int row = 0, x0 = w, x1 = -1, y0 = -1, y1 = -1; row < h; row++) {
                if (last[size_t(row)] < 0) continue;
                x0 = std::min(x0, first[size_t(row)]); x1 = std::max(x1, last[size_t(row)]);
                if (y0 < 0) y0 = row;
                y1 = row;
                b = {x0, y0, x1 + 1, y1 + 1};
            }
            if (b.isEmpty()) {
                layer = Layer(name, document.size());   // an empty layer (a divider) stays, as in the PSD import
            } else {
                auto image = cropImage(full, b.x0, b.y0, b.x1 - b.x0, b.y1 - b.y0);
                total += (long long)image->width() * image->height();
                if (total > Document::projectPixelBudget) {
                    if (error) *error = "The layers exceed the gigapixel a project may hold.";
                    return std::nullopt;
                }
                layer = Layer(Asset::make(image, name), Point(x + b.x0, y + b.y0));
            }
            if (l.kind != CSP_CLIP_LAYER_RASTER && l.kind != CSP_CLIP_LAYER_UNKNOWN) converted++;
            // The mask, over the layer's own pixels; beyond its bitmap, whatever its edge is.
            if (l.mask_mipmap_id != 0) {
                uint8_t* maskRaw = nullptr;
                int mw = 0, mh = 0, mx = 0, my = 0;
                size_t maskStride = 0;
                if (csp_clip_decode_layer_mask_gray8(doc.get(), i, &maskRaw, &mw, &mh, &maskStride, &mx, &my, message, sizeof message) == CSP_CLIP_OK) {
                    std::unique_ptr<uint8_t, Freer> maskPixels(maskRaw);
                    const bool placed = layer.asset.has_value();
                    const int lw = placed ? layer.asset->image->width() : document.width, lh = placed ? layer.asset->image->height() : document.height;
                    const int lx = placed ? int(layer.transform.origin.x) : 0, ly = placed ? int(layer.transform.origin.y) : 0;
                    auto mask = std::make_shared<GrayImage>(lw, lh, maskRaw[0]);
                    for (int row = 0; row < lh; row++) {
                        const int sy = ly + row - my;
                        if (sy < 0 || sy >= mh) continue;
                        for (int col = 0; col < lw; col++) { const int sx = lx + col - mx; if (sx >= 0 && sx < mw) mask->at(col, row) = maskRaw[size_t(sy) * maskStride + size_t(sx)]; }
                    }
                    LayerMask lm;
                    lm.asset = MaskAsset::make(mask);
                    lm.enabled = l.mask_enabled != 0;
                    layer.mask = lm;
                } else {
                    notes.push_back("Layer \"" + name + "\": its mask could not be read (" + message + ").");
                }
            }
        }
        layer.name = name;
        layer.visible = l.visible != 0;
        layer.opacity = l.opacity;
        layer.blendMode = blend;
        layer.parentId = parent;
        if (lossy) notes.push_back("Layer \"" + name + "\": Clip Studio's " + csp_clip_blend_mode_name(l.raw_composite) + " blend mode has no counterpart; " + blendModeName(blend) + " was used.");
        if (l.clipped && !l.is_group) {
            // Clipped to the nearest unclipped layer below it in the same folder.
            for (size_t k = document.layers.size(); k-- > 0;) {
                const Layer& below = document.layers[k];
                if (below.parentId != parent) { if (below.isGroup && below.id == parent) break; continue; }
                if (!below.maskSourceId && !below.isGroup) { layer.maskSourceId = below.id; break; }
                if (below.isGroup) break;
            }
        }
        ids[i] = layer.id;
        document.layers.push_back(std::move(layer));
    }
    static const std::map<int, const char*> kindNames{{CSP_CLIP_LAYER_VECTOR, "vector"}, {CSP_CLIP_LAYER_TEXT, "text"}, {CSP_CLIP_LAYER_FILL, "fill or gradient"},
        {CSP_CLIP_LAYER_CORRECTION, "correction"}, {CSP_CLIP_LAYER_PAPER, "paper"}, {CSP_CLIP_LAYER_OTHER, "special"}};
    for (auto& [kind, n] : skipped) {
        auto it = kindNames.find(kind);
        notes.push_back(std::to_string(n) + " " + (it != kindNames.end() ? it->second : "unknown") + " layer(s) had no pixels stored with them and were left out.");
    }
    if (converted) notes.push_back(std::to_string(converted) + " vector, text or other special layer(s) were imported as their pixels.");
    if (document.layers.empty()) { if (error) *error = "The file holds no layers this reader can use."; return std::nullopt; }
    return result;
}

#else

std::optional<PsdImport> importClip(const std::string&, std::string* error) {
    if (error) *error = "This build reads no Clip Studio projects (it was made without SQLite).";
    return std::nullopt;
}

#endif

} // namespace compositor
