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
#include "image.h"
#include "psd_carry.h"
#include "transform.h"
#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace compositor {

struct SmartObjectSource {
    enum class Kind { Embedded, Linked };
    std::string id;                        // Photoshop's 'Idnt' uuid for an imported source
    Kind kind = Kind::Embedded;
    std::string fileName;                  // "Art.psb"
    std::string fileType;                  // four characters: "8BPB", "8BPS", "png ", "JPEG", ...
    std::shared_ptr<const std::vector<uint8_t>> bytes;   // the embedded file, shared by every copy
    std::string linkedPath;                // a linked source's path, as the file gave it
    ImagePtr image;                        // the contents as an image; null when they cannot be read here
    int width = 0, height = 0;             // the contents' size in pixels
    double resolution = 72;
    /// Where it came from in a PSD: the global block ('lnk2', 'lnkE', ...) and the element as stored, written back
    /// byte for byte while the source is unchanged. A new or edited source has neither.
    std::string psdBlock;
    std::shared_ptr<const std::vector<uint8_t>> psdElement;
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
    bool warped = false, filtered = false;
};
/// A 'SoLd' / 'SoLE' (descriptor) or 'PlLd' (fixed) block's placement; none when it cannot be read.
std::optional<PsdPlacement> parsePsdPlacement(const std::string& key, const std::vector<uint8_t>& payload);
/// The block with the quad replaced (and the perspective quad moved along, and the placed id when given).
std::optional<std::vector<uint8_t>> patchPsdPlacement(const std::string& key, const std::vector<uint8_t>& payload,
                                                      const std::array<double, 8>& quad, const std::string& placedId = {});
/// The same pointing it at another source of another size (Replace and Edit Contents).
std::optional<std::vector<uint8_t>> repointPsdPlacement(const std::string& key, const std::vector<uint8_t>& payload, const std::array<double, 8>& quad,
                                                        const std::string& sourceId, double width, double height);

/// A fresh id in Photoshop's form (lowercase).
std::string newSmartObjectId();
/// A 'SoLd' for a new placement, in Photoshop 2026's field order (Patchy's authoring shape).
std::vector<uint8_t> authorPsdPlacement(const std::string& sourceId, const std::string& placedId, const std::array<double, 8>& quad,
                                        double width, double height, double resolution);
/// A 'lnk2' element (version 7 'liFD') for an embedded source.
std::vector<uint8_t> psdEmbeddedElement(const SmartObjectSource& source);

/// For the project package: a source's record (its image is stored beside it as PNG) and an instance's.
std::vector<uint8_t> serializeSmartObjectSource(const SmartObjectSource& source);
std::optional<SmartObjectSource> parseSmartObjectSource(const std::vector<uint8_t>& bytes);
std::vector<uint8_t> serializeSmartObjectInstance(const SmartObjectInstance& instance);
std::optional<SmartObjectInstance> parseSmartObjectInstance(const std::vector<uint8_t>& bytes);

} // namespace compositor
