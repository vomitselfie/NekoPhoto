// Smart Filters: Photoshop's filter stack on a smart object instance (the SoLd 'filterFX' descriptor, plus the
// document-global 'FEid' / 'FXid' cache that holds its shared filter mask). Thirteen filters are drawn here with
// Photoshop's calibrated kernels, ported from Patchy (MIT, src/third_party/patchy_psd/README.md); a stack with any
// other entry keeps the preview the file carried (docs/smart-objects.md).
#pragma once
#include "document.h"
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
/// Spin only (Zoom is not drawn); samples 8 / 16 / 32 for Draft / Good / Best, about the contents' centre.
struct RadialBlur { int32_t amount = 10, samples = 16; bool operator==(const RadialBlur&) const = default; };
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
PlacedRaster smartRadialBlur(const PlacedRaster& in, const PixelRect& canvas, int32_t amount, int32_t samples);
PlacedRaster smartAddNoise(const PlacedRaster& in, double amount, bool gaussian, bool monochromatic, int32_t seed);

/// The whole stack over the unfiltered instance `placed`: each enabled entry composited over the result so far with
/// its blend and opacity (Normal at 100% replaces it), then the shared mask between the unfiltered and filtered
/// pixels. None when the stack is not supported.
std::optional<PlacedRaster> renderSmartFilterStack(const PlacedRaster& placed, const PixelRect& canvas, const SmartFilterStack& stack);

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
/// The FEid / FXid payload with the records named by `replacements` (placed id to new record body) swapped in, the
/// rest byte for byte; none when the block cannot be walked.
std::optional<std::vector<uint8_t>> replaceSmartFilterRecords(const std::vector<uint8_t>& payload,
                                                              const std::vector<std::pair<std::string, std::vector<uint8_t>>>& replacements);

/// The instance's pixels with its Smart Filters: `source` placed on `quad` (through its warp, if any), then the
/// stack over it on the cache's canvas. None when the stack or its cache is not one drawn here.
std::optional<PlacedRaster> filteredSmartObjectRaster(const std::vector<PsdBlock>& globals,
                                                      const SmartObjectInstance& instance, const Image& source, const std::array<double, 8>& quad);
/// The instance placed on `quad` without its filters, on the document's pixel grid.
/// `clip`, when given, bounds it (document pixels).
std::optional<PlacedRaster> placedSmartObjectRaster(const SmartObjectInstance& instance, const Image& source, const std::array<double, 8>& quad,
                                                    const Rect* clip = nullptr);

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

} // namespace compositor

