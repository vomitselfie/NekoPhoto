// The document renderer in two halves (docs/high-bit-depth-plan.md, section 4):
//
// - RenderPlan, depth-agnostic: the tree walk (the visible layers in drawing order, artboards among them), the
//   clipping stacks, the folders that open and close around their children (isolation, fading, their styles),
//   the per-layer overrides, and the choice of a cached frame around the one layer being edited.
// - RenderExec<S>, the pixels at one sample type, which draws the plan into an ImageOf<S>. U8 is the renderer's
//   former body, moved, not rewritten (render_exec_u8.cpp); the deep depths follow it once, in
//   render_exec_deep.inc over a DeepOps<S> policy (render_deep_ops.h), and U16 instantiates it (render_exec_u16.cpp).
//
// render() builds the plan and switches on the document's sample type once.
#pragma once
#include "compositor/blendif.h"
#include "compositor/blur.h"
#include "compositor/render.h"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <map>
#include <optional>
#include <set>
#include <type_traits>
#include <vector>

namespace compositor {

struct RenderPlan {
    const Document& document;
    const Overrides* overrides;
    RenderPlan(const Document& d, const Overrides* o) : document(d), overrides(o) {}

    std::map<Uuid, const Layer*> byId;
    std::vector<const Layer*> order;          // visible non-group layers (and artboards), bottom to top
    std::map<Uuid, std::vector<Uuid>> stacks; // clipping base -> children
    std::set<Uuid> stacked;
    // Folders with a layer style: their exterior effects go down before their first child in `order`, the rest
    // after their last (outermost first in, innermost first out).
    std::map<size_t, std::vector<const Layer*>> groupsOpen, groupsClose;
    std::set<Uuid> suppressedGroups;   // the folder a silhouette is being rendered for

    /// The whole document: every layer indexed, the drawing order, artboards, stacks and folders.
    void build();
    /// The layers of `group` from `parent`'s order, for drawing a folder's children alone (its style's source).
    void buildWithin(const RenderPlan& parent, const Uuid& group);

    bool within(const Layer& layer, const Uuid& group) const;
    /// A folder with Blend If isolates even in Pass Through: its result is gated against what is under it.
    static bool isolates(const Layer& g) { return !g.passThrough || hasBlendIf(g); }
    static bool hasBlendIf(const Layer& l) { return l.psdCarry && !l.psdCarry->blendingRanges.empty() && layerBlendIf(l, ColorMode::CMYK); }
    static bool fades(const Layer& g) { return g.passThrough && g.opacity < 1 && !hasBlendIf(g); }
    /// The colour channels the layer or folder leaves out of its blending (blendif.h), 0 for none.
    uint8_t excluded(const Layer& l) const { return layerExcludedChannels(l, document.colorMode); }

    const LayerOverride* over(const Uuid& id) const {
        if (!overrides) return nullptr;
        auto it = overrides->find(id);
        return it == overrides->end() ? nullptr : &it->second;
    }
    LayerTransform transformOf(const Layer& l) const {
        auto* o = over(l.id);
        return o && o->transform ? *o->transform : l.transform;
    }
    BlendMode blendOf(const Layer& l) const {
        auto* o = over(l.id);
        return o && o->blendMode ? *o->blendMode : l.blendMode;
    }
    std::optional<Uuid> sourceOf(const Uuid& id) const {
        auto it = byId.find(id);
        return it == byId.end() ? std::nullopt : it->second->maskSourceId;
    }

    void prepareStacks();
    void prepareGroupStyles();
    /// Visible artboards go into the drawing order just below their first child (or where they stand, when empty).
    void insertArtboards();
    /// The one layer with an override, when a cache can be kept around it: a plain pixel layer that no other
    /// layer clips to or takes its mask from. SIZE_MAX otherwise.
    size_t editedIndex() const;
    /// The adjustment layer being dragged (the one override, marked `adjusting`), when the frame below it can be kept:
    /// a drawn adjustment layer that no layer takes coverage from. SIZE_MAX otherwise.
    size_t adjustingIndex() const;
    /// Where a frame drawn for the adjustment layer at `adjusting` can resume: the first index of `order` whose drawing
    /// depends on that layer's settings. The layer itself; the base of the clipping stack it is in; the opening of a
    /// folder around it with a layer style (its exterior effects are drawn from the folder's contents as it opens).
    /// The 8-bit executor moves it down to the start of a fused run of adjustments the layer is or could be part of.
    size_t resumeIndex(size_t adjusting) const;
    /// A layer above the edited one that composites as plain source-over, so a group of them can be
    /// flattened once and laid over the frame.
    bool plainAbove(const Layer& l) const;
};

/// The document pixel under output pixel (x, y): Dissolve's pattern belongs to the document, so it stays put when
/// the view pans or renders in tiles.
inline int docX(const Rect& region, double scale, int x) { return int(std::floor(region.x + (x + 0.5) / scale)); }
inline int docY(const Rect& region, double scale, int y) { return int(std::floor(region.y + (y + 0.5) / scale)); }

/// A stroked shape's Feather (its vector mask's): Photoshop draws the shape, fill and stroke, then blurs what it drew as
/// a whole (Patchy's notes on photoshop-shape-feather), a gaussian of `sigma` output pixels (vectorFeatherSigma) over
/// every sample of the premultiplied pixels (`channels` of them), nothing past the edges (the shape fades at the canvas
/// edge, as its mask does).
template <class Img>
void featherDrawnShape(Img& image, int channels, double sigma) {
    if (image.isEmpty() || !(sigma > 0)) return;
    const int pad = int(std::ceil(sigma * 4)) + 1, w = image.width(), h = image.height();
    using T = std::remove_cvref_t<decltype(image.row(0)[0])>;
    constexpr float one = std::is_floating_point_v<T> ? 1.0f : std::is_same_v<T, uint8_t> ? 255.0f : 32768.0f;
    GrayF plane(w + 2 * pad, h + 2 * pad, 0.0f);
    for (int c = 0; c < channels; c++) {
        plane.fill(0.0f);
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) plane.row(y + pad)[x + pad] = float(image.row(y)[x * channels + c]) / one;
        gaussianBlur(plane, sigma);
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                const float v = std::max(0.0f, plane.row(y + pad)[x + pad]);
                if constexpr (std::is_floating_point_v<T>) image.row(y)[x * channels + c] = v;
                else image.row(y)[x * channels + c] = T(std::min(one, std::round(v * one)));
            }
    }
    // Colour stays within alpha (premultiplied; float light may exceed it), as each sample was blurred alone.
    if constexpr (!std::is_floating_point_v<T>)
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            T* p = image.row(y) + x * channels;
            for (int c = 0; c < channels - 1; c++) p[c] = std::min(p[c], p[channels - 1]);
        }
}

/// Whether the layer is type to Photoshop: a text layer, or Photoshop text NekoPhoto shows as its pixels (vertical
/// type, say) while those pixels are still the ones Photoshop drew.
bool photoshopType(const Layer& layer);

/// Type over what is under it, as Photoshop draws it in an RGB document: with Color Settings' "Blend Text Colors Using
/// Gamma" (on, at 1.45, by default) a type layer's colours meet the backdrop in a space of gamma 1.45 over linear light,
/// not in the document's encoding. `source` is the layer drawn alone (premultiplied, its opacity and masks in), blended
/// into `target` in Normal mode. Photoshop's merged images of Patchy's photoshop-text-* fixtures match it on every
/// anti-aliased edge pixel.
void blendTextGamma(const Image& source, Image& target);
void blendTextGamma(const Image16& source, Image16& target);

/// The pixel half of the renderer at one sample type: specialised per depth (render_exec_u8.cpp).
template <SampleType S> struct RenderExec;

/// Draws `plan` into `out` (sized, and cleared or not, by the caller), which represents `region` at `scale`; with
/// a cache, around the layer being edited. Defined once per sample type the renderer supports.
template <SampleType S>
void executeRender(const RenderPlan& plan, const Rect& region, double scale, ImageOf<S>& out, RenderCache* cache, uint64_t version);
template <>
void executeRender<SampleType::U8>(const RenderPlan& plan, const Rect& region, double scale, Image& out, RenderCache* cache, uint64_t version);
template <>
void executeRender<SampleType::U16>(const RenderPlan& plan, const Rect& region, double scale, Image16& out, RenderCache* cache, uint64_t version);
/// A 16-bit document's frame for the canvas (render_u16.cpp): rendered at 16 bits into `out`'s size and reduced to 8.
void renderForDisplay16(const RenderPlan& plan, const Rect& region, double scale, Image& out, RenderCache* cache, uint64_t version, bool clear, const ColorTransform* display = nullptr);
template <>
void executeRender<SampleType::F32>(const RenderPlan& plan, const Rect& region, double scale, ImageF& out, RenderCache* cache, uint64_t version);
/// A 32-bit document's frame for the canvas (render_f32.cpp): rendered in float into `out`'s size, then through the
/// view (exposure, gamma or Highlight Compression) and the display transform, or the document's curve, to 8 bits.
void renderForDisplayF(const RenderPlan& plan, const Rect& region, double scale, Image& out, RenderCache* cache, uint64_t version, bool clear,
                       const ColorTransform* display, const View32& view, float peak);
/// A CMYK or Lab document for the canvas (render_modes.cpp): rendered at its layout, then to 8-bit RGBA through
/// `display` when that transform reads the document's layout, else to sRGB; never left unconverted.
void renderForDisplayMode(const Document& document, const RenderPlan& plan, const Rect& region, double scale, Image& out, RenderCache* cache,
                          uint64_t version, bool clear, const ColorTransform* display);
/// A CMYK or Lab document as 16-bit sRGB (render16 hands it over): what 16-bit RGB exports take.
void renderModeAsRgb16(const Document& document, const RenderOptions& options, Image16& out, const Overrides* overrides, RenderCache* cache);
/// resizeDocument for a 16-bit document (render_u16.cpp).
bool resizeDocument16(Document& document, int width, int height, double resolution, Sampling sampling);
/// And for a 32-bit one (render_f32.cpp): the float warps and resamplers.
bool resizeDocumentF(Document& document, int width, int height, double resolution, Sampling sampling);

} // namespace compositor
