// Smart Filters: Photoshop's filter stack on a smart object instance (the SoLd 'filterFX' descriptor, plus the
// document-global 'FEid' / 'FXid' cache that holds its shared filter mask). Thirteen filters are drawn here with
// Photoshop's calibrated kernels, ported from Patchy (MIT, src/third_party/patchy_psd/README.md); a stack with any
// other entry keeps the preview the file carried (docs/smart-objects.md).
#pragma once
#include "document.h"
#include "filters.h"
#include "image.h"
#include "psd_carry.h"
#include "smartobject.h"
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace compositor {

/// Whole pixels in document space.
struct PixelRect {
    int x = 0, y = 0, width = 0, height = 0;
    bool empty() const { return width <= 0 || height <= 0; }
    bool operator==(const PixelRect&) const = default;
};

namespace smartfilter {
struct GaussianBlur { double radius = 0; bool operator==(const GaussianBlur&) const = default; };
struct HighPass { double radius = 0; bool operator==(const HighPass&) const = default; };
struct Median { double radius = 1; bool operator==(const Median&) const = default; };
struct DustAndScratches { int32_t radius = 1, threshold = 0; bool operator==(const DustAndScratches&) const = default; };
struct SurfaceBlur { double radius = 5; int32_t threshold = 15; bool operator==(const SurfaceBlur&) const = default; };
struct UnsharpMask { double amount = 150, radius = 2; int32_t threshold = 8; bool operator==(const UnsharpMask&) const = default; };
struct MotionBlur { int32_t angle = 0, distance = 12; bool operator==(const MotionBlur&) const = default; };
struct PlasticWrap { int32_t highlight = 9, detail = 7, smoothness = 5; bool operator==(const PlasticWrap&) const = default; };
struct Mosaic { int32_t cellSize = 8; bool operator==(const Mosaic&) const = default; };
struct Emboss { int32_t angle = 135, height = 2, amount = 100; bool operator==(const Emboss&) const = default; };
struct BoxBlur { double radius = 1; bool operator==(const BoxBlur&) const = default; };
/// Spin (around the contents' centre) or Zoom (towards it); samples 8 / 16 / 32 for Draft / Good / Best.
struct RadialBlur { int32_t amount = 10, samples = 16; bool zoom = false; bool operator==(const RadialBlur&) const = default; };
struct AddNoise { double amount = 12.5; bool gaussian = false, monochromatic = false; int32_t seed = 1; bool operator==(const AddNoise&) const = default; };
} // namespace smartfilter

using SmartFilterParameters = std::variant<std::monostate, smartfilter::GaussianBlur, smartfilter::HighPass, smartfilter::Median,
                                           smartfilter::DustAndScratches, smartfilter::SurfaceBlur, smartfilter::UnsharpMask,
                                           smartfilter::MotionBlur, smartfilter::PlasticWrap, smartfilter::Mosaic, smartfilter::Emboss,
                                           smartfilter::BoxBlur, smartfilter::RadialBlur, smartfilter::AddNoise>;

struct SmartFilterEntry {
    SmartFilterParameters parameters;      // monostate: a filter NekoPhoto does not draw
    std::string name;                      // Photoshop's entry name ("Gaussian Blur...")
    bool enabled = true;
    double opacity = 1;                    // 0..1
    BlendMode blend = BlendMode::Normal;
    bool operator==(const SmartFilterEntry&) const = default;
};

/// A layer's stack, in the order the filters run (Photoshop's panel lists them the other way up).
struct SmartFilterStack {
    bool enabled = true;
    std::vector<SmartFilterEntry> entries;
    /// The shared filter mask (document space); none: all white. `maskDefault` is its tone past `maskBounds`.
    std::shared_ptr<const GrayImage> mask;
    PixelRect maskBounds;
    uint8_t maskDefault = 255;
    bool maskEnabled = true;
    /// Every entry and the mask are ones NekoPhoto draws exactly as the file means them.
    bool supported = false;
    bool operator==(const SmartFilterStack&) const = default;
};

/// Premultiplied pixels placed in document space.
struct PlacedRaster {
    std::shared_ptr<Image> image;
    int x = 0, y = 0;
    PixelRect bounds() const { return image ? PixelRect{x, y, image->width(), image->height()} : PixelRect{}; }
};

/// The same at 16 bits (0..32768).
struct PlacedRaster16 {
    std::shared_ptr<Image16> image;
    int x = 0, y = 0;
    PixelRect bounds() const { return image ? PixelRect{x, y, image->width(), image->height()} : PixelRect{}; }
};

/// Either depth, as a layer holds it. `transform` is where the raster goes exactly (a warp's may start between
/// pixels); `x`, `y` its origin rounded to the pixel grid.
struct AnyPlacedRaster {
    AnyImage image;
    int x = 0, y = 0;
    LayerTransform transform;
};

// ---- the filters ------------------------------------------------------------------------------------------
// Each takes the current result and `canvas` (Photoshop's filter canvas: the document, or the FEid cache's rect)
// and returns the next result. Filters that grow (the blurs) grow inside the canvas, sampling past it by its
// nearest edge, and are trimmed to their non-transparent pixels; the rest keep the input's bounds.

PlacedRaster smartGaussianBlur(const PlacedRaster& in, const PixelRect& canvas, double radius);
PlacedRaster smartHighPass(const PlacedRaster& in, double radius);
PlacedRaster smartMedian(const PlacedRaster& in, double radius);
PlacedRaster smartDustAndScratches(const PlacedRaster& in, int32_t radius, int32_t threshold);
PlacedRaster smartSurfaceBlur(const PlacedRaster& in, const PixelRect& canvas, double radius, int32_t threshold);
PlacedRaster smartUnsharpMask(const PlacedRaster& in, double amount, double radius, int32_t threshold);
PlacedRaster smartMotionBlur(const PlacedRaster& in, const PixelRect& canvas, int32_t angle, int32_t distance);
PlacedRaster smartPlasticWrap(const PlacedRaster& in, int32_t highlight, int32_t detail, int32_t smoothness);
PlacedRaster smartMosaic(const PlacedRaster& in, int32_t cellSize);
PlacedRaster smartEmboss(const PlacedRaster& in, int32_t angle, int32_t height, int32_t amount);
PlacedRaster smartBoxBlur(const PlacedRaster& in, const PixelRect& canvas, double radius);
PlacedRaster smartRadialBlur(const PlacedRaster& in, const PixelRect& canvas, int32_t amount, int32_t samples, bool zoom = false);
PlacedRaster smartAddNoise(const PlacedRaster& in, double amount, bool gaussian, bool monochromatic, int32_t seed);

/// The whole stack over the unfiltered instance `placed`: each enabled entry composited over the result so far with
/// its blend and opacity (Normal at 100% replaces it), then the shared mask between the unfiltered and filtered
/// pixels. None when the stack is not supported.
std::optional<PlacedRaster> renderSmartFilterStack(const PlacedRaster& placed, const PixelRect& canvas, const SmartFilterStack& stack);
/// The stack at 16 bits (smartfilter_render16.cpp): the same kernels on 15-bit straight colour, each calibrated
/// against the 8-bit one (docs/smart-objects.md gives the figures). None when the stack is not supported, or has an
/// entry not drawn at 16 bits (smartFilterDrawsAt16).
std::optional<PlacedRaster16> renderSmartFilterStack(const PlacedRaster16& placed, const PixelRect& canvas, const SmartFilterStack& stack);
/// The stack over a raster of any depth and layout placed at (`x`, `y`) in a document of `mode`: RGB or Lab (4
/// samples), CMYK (5, run as two 4-sample halves), at 8 or 16 bits. None when a filter is not drawn in that mode
/// (smartFilterDrawsInMode) or the stack is not supported.
std::optional<AnyPlacedRaster> renderSmartFilterStackAny(const AnyImage& placed, int x, int y, const PixelRect& canvas, const SmartFilterStack& stack,
                                                         ColorMode mode);
/// The Smart Filter a Filter menu filter drawn by these kernels is (Box Blur, Radial Blur, Surface Blur, Dust & Scratches,
/// Median, Unsharp Mask, High Pass, Emboss, Mosaic), its settings held in range; none for the others.
std::optional<SmartFilterParameters> smartFilterParametersFor(FilterKind kind, const FilterSettings& settings);
/// Whether a filter is drawn at 16 bits.
bool smartFilterDrawsAt16(const SmartFilterParameters& parameters);

// ---- Photoshop's structures ---------------------------------------------------------------------------------

/// The stack in a SoLd / SoLE block's 'filterFX'; none when it has none. `supported` is left false when any entry
/// or setting is outside what is drawn here (the mask comes from the FEid record, see below).
std::optional<SmartFilterStack> parseSmartFilterStack(const std::string& key, const std::vector<uint8_t>& payload);

/// One instance's record in the document's 'FEid' / 'FXid' cache: the filter canvas and the shared mask.
struct SmartFilterCache {
    PixelRect canvas;
    std::shared_ptr<const GrayImage> mask; // none: the record has no mask (all white)
    PixelRect maskBounds;
};
/// The record for instance `placedId` among the document's global blocks: exactly one, readable (8-bit, version 1);
/// none otherwise.
std::optional<SmartFilterCache> findSmartFilterCache(const std::vector<PsdBlock>& globals, const std::string& placedId);

/// A fresh record for `placedId` (Photoshop 2026's shape): the unfiltered instance over the whole `document` rect
/// as the cache, and the mask (none: all white) over the same rect.
std::vector<uint8_t> authorSmartFilterRecord(const std::string& placedId, const PixelRect& document, const PlacedRaster& unfiltered,
                                             const GrayImage* mask, const PixelRect& maskBounds, uint8_t maskDefault);
/// The same for an 8-bit raster of any layout placed at (`x`, `y`): RGB, CMYK (`ImageC8`, its four inks) or Lab.
std::vector<uint8_t> authorSmartFilterRecord(const std::string& placedId, const PixelRect& document, const AnyImage& unfiltered, int x, int y,
                                             const GrayImage* mask, const PixelRect& maskBounds, uint8_t maskDefault);
/// The FEid / FXid payload with the records named by `replacements` (placed id to new record body) swapped in, the
/// rest byte for byte; an empty body drops that record. None when the block cannot be walked.
std::optional<std::vector<uint8_t>> replaceSmartFilterRecords(const std::vector<uint8_t>& payload,
                                                              const std::vector<std::pair<std::string, std::vector<uint8_t>>>& replacements);

/// A cache record for `instance` on the whole `width` x `height` document with its mask all white, added to `globals`
/// (to the 'FEid' block, a new one when there is none). For a 16-bit PSD, which carries no cache NekoPhoto reads (its
/// 16-bit writer leaves the 8-bit one out): its supported stacks are then drawn and editable here. False when the
/// stack is not one drawn here or the instance cannot be placed.
bool addDefaultSmartFilterCache(std::vector<PsdBlock>& globals, const SmartObjectInstance& instance, const SmartObjectSource& source, int width, int height);

/// A copy of the document's cache record for instance `from` under the id `to` (New Smart Object via Copy: the copy keeps
/// the filter canvas and mask), added to the block holding `from`'s. False when there is no single readable record.
bool copySmartFilterRecord(std::vector<PsdBlock>& globals, const std::string& from, const std::string& to);

/// Whether the instance's Smart Filters (stack and cache) are ones drawn here, without drawing them.
bool smartFiltersDrawable(const std::vector<PsdBlock>& globals, const SmartObjectInstance& instance);
/// The instance's pixels with its Smart Filters: `source` placed on `quad` (through its warp, if any), then the
/// stack over it on the cache's canvas. None when the stack or its cache is not one drawn here.
std::optional<PlacedRaster> filteredSmartObjectRaster(const std::vector<PsdBlock>& globals,
                                                      const SmartObjectInstance& instance, const Image& source, const std::array<double, 8>& quad);
/// The instance placed on `quad` without its filters, on the document's pixel grid.
/// `clip`, when given, bounds it (document pixels).
std::optional<PlacedRaster> placedSmartObjectRaster(const SmartObjectInstance& instance, const Image& source, const std::array<double, 8>& quad,
                                                    const Rect* clip = nullptr);
/// The same at 16 bits.
std::optional<PlacedRaster16> filteredSmartObjectRaster(const std::vector<PsdBlock>& globals,
                                                        const SmartObjectInstance& instance, const Image16& source, const std::array<double, 8>& quad);
std::optional<PlacedRaster16> placedSmartObjectRaster(const SmartObjectInstance& instance, const Image16& source, const std::array<double, 8>& quad,
                                                      const Rect* clip = nullptr);

/// How an instance is drawn: with its Smart Filters (and warp), through its warp only, or placed without filters
/// (through its warp, if any).
enum class SmartObjectDraw { Filtered, Warped, Unfiltered };
/// The instance drawn from `source` at `type` (the document's depth; the source is converted when its own differs):
/// the one entry point that picks the 8- or 16-bit path. None when it cannot be drawn that way.
/// In a CMYK or Lab document (`target` with its mode and profile) the contents are placed in its layout and the filters
/// run on its samples (smartFilterDrawsInMode).
std::optional<AnyPlacedRaster> drawSmartObjectRaster(const std::vector<PsdBlock>& globals, const SmartObjectInstance& instance,
                                                     const SmartObjectSource& source, const std::array<double, 8>& quad, const SmartObjectTarget& target,
                                                     SmartObjectDraw how);
/// The layout `document` places smart object contents in (its depth, mode and profile).
SmartObjectTarget smartObjectTargetOf(const Document& document);
/// Whether a filter is drawn as a Smart Filter in a document of `mode`: in CMYK and Lab every one but Plastic Wrap, a
/// Filter Gallery filter Photoshop offers in RGB only.
bool smartFilterDrawsInMode(const SmartFilterParameters& parameters, ColorMode mode);

/// Warped and filtered instances whose layer was moved, scaled or rotated since they were drawn, drawn again from their
/// contents on the moved quad (their pixels are not the placement, so resampling them would soften them and slide the
/// filter mask). Returns how many were redrawn.
int refreshSmartObjectRasters(Document& document);

/// The placement block with `stack` as its 'filterFX' (Photoshop 2026's shape, Patchy's authoring: its keys and id
/// forms), replacing any stack it had; none for a block that is not a SoLd / SoLE or a stack with an entry not drawn here.
std::optional<std::vector<uint8_t>> setPsdSmartFilterStack(const std::string& key, const std::vector<uint8_t>& payload, const SmartFilterStack& stack);
/// Photoshop's entry name for a filter ("Gaussian Blur..."); empty for none.
std::string smartFilterName(const SmartFilterParameters& parameters);

/// Adds `entry` on top of a smart object's Smart Filters (a first filter starts the stack, its mask all white): the
/// placement's 'filterFX' and the document's 'FEid' cache record for it are written, and the instance is drawn again.
/// False, with `error`, for a layer that is not an editable smart object or a stack not drawn here.
bool addSmartFilter(Document& document, Layer& layer, const SmartFilterEntry& entry, std::string* error);

/// `parameters` held inside the ranges a Photoshop file may carry for that filter (what parseSmartFilterStack reads back).
void clampSmartFilterParameters(SmartFilterParameters& parameters);

/// A smart object's Smart Filters with the shared mask from the document's cache record; none when it has none.
/// `supported` is false when an entry is not drawn here or the cache record cannot be read (the stack is then
/// read-only: setSmartFilters refuses it).
std::optional<SmartFilterStack> smartFilterStackOf(const Document& document, const Layer& layer);

/// Replaces a smart object's Smart Filters with `stack` (its entries in running order, their switches, opacity and
/// blend, the stack's switch and its shared mask, `mask` none meaning all `maskDefault`): the placement's 'filterFX'
/// and the document's 'FEid' record are written anew and the instance is drawn again. No entries removes the stack
/// (the 'filterFX' key and the record go; the instance is drawn unfiltered). False, with `error`, for a layer that is
/// not an editable smart object, a stack that has an entry not drawn here, or one that cannot be written.
bool setSmartFilters(Document& document, Layer& layer, const SmartFilterStack& stack, std::string* error);

} // namespace compositor

