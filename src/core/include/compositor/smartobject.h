// Smart objects: a source (an embedded file, or a linked one) that layers place without owning, each instance
// with its own placement. The model is NekoPhoto's own; PSD is one mapping of it (psd.cpp reads Photoshop's
// placed layers into it, psd_writer.cpp writes them back). See docs/smart-objects.md.
//
// An editable instance's pixels are the source's image and its LayerTransform the placement, so moving or
// scaling it always resamples the full-resolution source (non-destructive). What NekoPhoto cannot redraw
// itself (a warp, a perspective or skewed placement, Smart Filters, a source it cannot read or a linked file)
// is "preview-locked": it shows the preview the file carried and can still be moved and scaled, and its
// Photoshop data is written back with the placement patched in, never rewritten.
#pragma once
#include "colormodes.h"
#include "colorprofile.h"
#include "image.h"
#include "imaget.h"
#include "psd_carry.h"
#include "transform.h"
#include "warpmesh.h"
#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace compositor {

/// A source's contents converted to another depth, made on first use and shared by every instance that places it in
/// a document of that depth. Keyed on the buffer it was made from, so a copied source with new contents never sees a
/// stale one.
struct SmartObjectDepthCache {
    std::mutex mutex;
    const void* from = nullptr;
    AnyImage converted;
    /// The same in a CMYK or Lab document's layout: the contents taken through the profiles once, for the depth, mode
    /// and profile they were made for.
    const void* modeFrom = nullptr;
    AnyImage modeConverted;
    SampleType modeType = SampleType::U8;
    ColorMode mode = ColorMode::RGB;
    ColorProfile modeProfile;
};

struct SmartObjectSource {
    enum class Kind { Embedded, Linked };
    std::string id;                        // Photoshop's 'Idnt' uuid for an imported source
    Kind kind = Kind::Embedded;
    std::string fileName;                  // "Art.psb"
    std::string fileType;                  // four characters: "8BPB", "8BPS", "png ", "JPEG", ...
    std::shared_ptr<const std::vector<uint8_t>> bytes;   // the embedded file, shared by every copy
    std::string linkedPath;                // a linked source's path, as the file gave it
    /// The contents as an image at their own depth (an 8-bit PNG 8-bit, a 16-bit PSB 16-bit), whatever the depth of
    /// the documents placing it; null when they cannot be read here.
    AnyImage image;
    /// The profile `image` is in (empty: sRGB). An RGB document places `image` as it is; a CMYK or Lab one converts it
    /// from this profile to its own.
    ColorProfile profile;
    /// CMYK or Lab contents (a CMYK PSB, a Lab PSD) at their own layout and depth, in `nativeProfile`; `image` is then
    /// the same contents in sRGB. A document of the same mode and profile places these samples as they are; any other
    /// CMYK or Lab document converts them through the profiles. Null for RGB contents.
    AnyImage native;
    ColorMode nativeMode = ColorMode::RGB;
    ColorProfile nativeProfile;
    std::shared_ptr<SmartObjectDepthCache> depthCache = std::make_shared<SmartObjectDepthCache>();
    int width = 0, height = 0;             // the contents' size in pixels
    double resolution = 72;
    /// Where it came from in a PSD: the global block ('lnk2', 'lnkE', ...) and the element as stored, written back
    /// byte for byte while the source is unchanged. A new or edited source has neither.
    std::string psdBlock;
    std::shared_ptr<const std::vector<uint8_t>> psdElement;
    /// A camera RAW source (Camera Raw's Open Object): the develop settings (CameraRawSettings::toJson) its image was
    /// developed with from `bytes`, the RAW file. Empty for every other source.
    std::string rawSettings;
    bool isCameraRaw() const { return !rawSettings.empty(); }
};

struct SmartObjectInstance {
    enum class Lock { None, Warp, Perspective, Filters, Unreadable, Linked, Legacy };
    std::string sourceId;
    /// The placed corners in document pixels, top-left, top-right, bottom-right, bottom-left (Photoshop's 'Trnf').
    std::array<double, 8> quad{};
    Lock lock = Lock::None;
    std::string placedId;                  // Photoshop's per-instance 'placed' uuid
    /// The layer's Photoshop blocks ('SoLd', 'SoLE', 'PlLd') as stored, written back with the placement patched.
    std::vector<PsdBlock> psdBlocks;
    /// The layer's transform when the quad was last true of it: a later transform maps the quad along.
    LayerTransform placedTransform;
    int placedWidth = 0, placedHeight = 0; // the raster size placedTransform applied to
    bool operator==(const SmartObjectInstance&) const = default;
    bool locked() const { return lock != Lock::None; }
};

const char* smartObjectLockDescription(SmartObjectInstance::Lock lock);

/// The source's contents at `type` (U8 or U16): the image itself at its own depth, else a converted copy made once
/// and shared. Null when the contents cannot be read.
AnyImage smartObjectSourceImage(const SmartObjectSource& source, SampleType type);

/// The layout a document places contents in: its depth, and for CMYK and Lab its mode and profile (the profile must
/// outlive the call). Converts from a depth alone, for RGB.
struct SmartObjectTarget {
    SampleType type = SampleType::U8;
    ColorMode mode = ColorMode::RGB;
    const ColorProfile* profile = nullptr;
    SmartObjectTarget() = default;
    SmartObjectTarget(SampleType t) : type(t) {}
    SmartObjectTarget(SampleType t, ColorMode m, const ColorProfile* p) : type(t), mode(m), profile(p) {}
};
/// The contents in `target`'s layout: in RGB as above; in CMYK and Lab the contents' own samples when they are of that
/// mode and profile, else converted through the profiles (Relative Colorimetric with black point compensation, as
/// Photoshop places) once and shared. Null when the contents cannot be read or converted.
AnyImage smartObjectSourceImage(const SmartObjectSource& source, const SmartObjectTarget& target);
/// A PNG file's pixels at its own depth: a 16-bit PNG as 16 bits, any other as 8. Null when it is not one.
AnyImage decodeSmartObjectPng(const std::vector<uint8_t>& bytes);

/// The warp an unlocked instance is drawn through (read from its Photoshop placement); none when it is flat.
std::optional<WarpMesh> smartObjectWarp(const SmartObjectInstance& instance);
/// Whether an unlocked instance's pixels are its source placed as they are (no warp, no Smart Filters), so its
/// layer transform is its placement.
bool smartObjectPixelsArePlacement(const SmartObjectInstance& instance);
/// Whether the instance carries Smart Filters.
bool smartObjectFiltered(const SmartObjectInstance& instance);
/// The instance's pixels when it is warped: `source` drawn through its warp onto `quad`.
std::optional<WarpedRaster> warpedSmartObjectRaster(const SmartObjectInstance& instance, const Image& source, const std::array<double, 8>& quad);
std::optional<WarpedRaster16> warpedSmartObjectRaster(const SmartObjectInstance& instance, const Image16& source, const std::array<double, 8>& quad);

/// Where the raster point (x, y) of a `w` x `h` raster lands in the document under `t`.
Point mapThroughTransform(const LayerTransform& t, int w, int h, double x, double y);
/// The transform that places a `w` x `h` raster on `quad`; none when the quad is skewed or not a rectangle.
std::optional<LayerTransform> transformForQuad(const std::array<double, 8>& quad, int w, int h);
/// `quad` carried from where `before` put a `w0` x `h0` raster to where `after` puts a `w1` x `h1` one.
std::array<double, 8> moveQuad(const std::array<double, 8>& quad, const LayerTransform& before, int w0, int h0,
                               const LayerTransform& after, int w1, int h1);

// ---- Photoshop's structures ------------------------------------------------------------------------------

/// The sources in a global 'lnk2' / 'lnkD' / 'lnk3' / 'lnkE' block (files not decoded yet). Empty on damage.
std::vector<SmartObjectSource> parsePsdLinkBlock(const std::vector<uint8_t>& payload);

struct PsdPlacement {
    std::string sourceId, placedId;
    std::array<double, 8> quad{};
    std::optional<std::array<double, 8>> nonAffine;
    double width = 0, height = 0, resolution = 72;
    std::optional<WarpMesh> warp;          // a warp NekoPhoto draws (a mesh, or a preset style baked to one)
    bool warped = false, filtered = false; // warped: a warp it cannot draw
};
/// A 'SoLd' / 'SoLE' (descriptor) or 'PlLd' (fixed) block's placement; none when it cannot be read.
std::optional<PsdPlacement> parsePsdPlacement(const std::string& key, const std::vector<uint8_t>& payload);
/// The block with the quad replaced (and the perspective quad moved along, and the placed id when given).
std::optional<std::vector<uint8_t>> patchPsdPlacement(const std::string& key, const std::vector<uint8_t>& payload,
                                                      const std::array<double, 8>& quad, const std::string& placedId = {});
/// The same pointing it at another source of another size (Replace and Edit Contents).
std::optional<std::vector<uint8_t>> repointPsdPlacement(const std::string& key, const std::vector<uint8_t>& payload, const std::array<double, 8>& quad,
                                                        const std::string& sourceId, double width, double height);

/// The block warped by `mesh` (contents space, as Photoshop's Custom warp stores it) on `quad` (the mesh's hull
/// placed); a SoLd / SoLE only.
std::optional<std::vector<uint8_t>> warpPsdPlacement(const std::string& key, const std::vector<uint8_t>& payload, const WarpMesh& mesh,
                                                     const std::array<double, 8>& quad);
/// A fresh id in Photoshop's form (lowercase).
std::string newSmartObjectId();
/// A 'SoLd' for a new placement, in Photoshop 2026's field order (Patchy's authoring shape).
std::vector<uint8_t> authorPsdPlacement(const std::string& sourceId, const std::string& placedId, const std::array<double, 8>& quad,
                                        double width, double height, double resolution);
/// A 'lnk2' element (version 7 'liFD') for an embedded source.
std::vector<uint8_t> psdEmbeddedElement(const SmartObjectSource& source);

/// For the project package: a source's record (its image is stored beside it as PNG, 16-bit for a 16-bit source) and
/// an instance's.
std::vector<uint8_t> serializeSmartObjectSource(const SmartObjectSource& source);
std::optional<SmartObjectSource> parseSmartObjectSource(const std::vector<uint8_t>& bytes);
std::vector<uint8_t> serializeSmartObjectInstance(const SmartObjectInstance& instance);
std::optional<SmartObjectInstance> parseSmartObjectInstance(const std::vector<uint8_t>& bytes);

} // namespace compositor
