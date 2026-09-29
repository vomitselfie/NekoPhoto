// Colour management (docs/color-management.md, docs/high-bit-depth-plan.md section 5), following Photoshop's model:
// a document is in one profile; colours are converted only at the edges (Assign and Convert to Profile, the display,
// soft proofing, export, import policy). Little CMS (vendored, src/third_party/lcms2) does the work; this file is the
// only one that builds transforms for documents, and keeps them in a small thread-safe LRU cache.
//
// Pixels are premultiplied, as everywhere in the core: a transform unpremultiplies, converts and premultiplies again
// (Little CMS's premultiplied-alpha layouts), and alpha passes through. 16-bit pixels are the document's 0..32768; they
// are widened to 0..65535 for Little CMS and narrowed back, which is exact for every 15-bit level.
//
// CMYK and Lab pixels (colormodes.h) have their own layouts: CMYK is Little CMS's reversed ("_REV") CMYK, as PSD
// stores inverted ink, with alpha after it; Lab's a and b are offset (128 at 8 bits, 16384 at 16), which the transform
// maps to and from Little CMS's own Lab encodings. Every layout is made straight around the transform, as RGB is.
#pragma once
#include "colormodes.h"
#include "colorprofile.h"
#include "image.h"
#include "imaget.h"
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace compositor {

struct Document;

// ---- Profiles ------------------------------------------------------------------------------------------------------

/// The RGB working spaces Edit > Color Settings offers (Photoshop's usual four).
enum class WorkingSpace { SRGB, AdobeRGB, DisplayP3, ProPhoto };
/// "srgb", "adobe-rgb", "display-p3", "prophoto" (automation and settings), and back.
const char* workingSpaceKey(WorkingSpace space);
std::optional<WorkingSpace> workingSpaceFromKey(const std::string& key);
/// The profile's name as the menus show it: "sRGB IEC61966-2.1", "Adobe RGB (1998)", "Display P3", "ProPhoto RGB".
const char* workingSpaceName(WorkingSpace space);

/// A built-in working-space profile, made by Little CMS from the published primaries, white point and tone curve (no
/// ICC file is bundled): the same bytes every time.
const ColorProfile& builtinProfile(WorkingSpace space);
/// sRGB: what an untagged document is treated as.
inline const ColorProfile& srgbProfile() { return builtinProfile(WorkingSpace::SRGB); }
/// Lab D50, made by Little CMS (a v4 Lab identity profile, D50 white): what a Lab document's values are in, and the
/// same bytes every time.
const ColorProfile& labProfile();
/// The default Working CMYK: ISO Coated v2 300% (basICColor), FOGRA39, compiled in (src/third_party/icc). An untagged
/// CMYK document is treated as this profile, as Photoshop treats one as its Working CMYK.
const ColorProfile& defaultCmykProfile();

/// A profile from ICC bytes, with its description and model; nullopt when Little CMS cannot read them.
std::optional<ColorProfile> profileFromIcc(const std::vector<uint8_t>& icc);
std::optional<ColorProfile> profileFromIcc(const uint8_t* data, size_t size);
/// The profile an untagged document stands for (sRGB), else the profile itself.
const ColorProfile& effectiveProfile(const ColorProfile& profile);
/// The same for values of `model`: sRGB for RGB, the default CMYK for CMYK, Lab D50 for Lab.
const ColorProfile& effectiveProfile(const ColorProfile& profile, ColorModel model);
/// Whether two profiles give the same colours: the same bytes, or matrix-shaper profiles whose conversion between
/// them is the identity at 16 bits (sRGB by another vendor, say). Untagged counts as sRGB. Cached.
bool equivalentProfiles(const ColorProfile& a, const ColorProfile& b);
/// The working space the profile is, when it is equivalent to one.
std::optional<WorkingSpace> matchingWorkingSpace(const ColorProfile& profile);

// ---- Transforms ----------------------------------------------------------------------------------------------------

enum class RenderingIntent { Perceptual = 0, RelativeColorimetric = 1, Saturation = 2, AbsoluteColorimetric = 3 };
const char* renderingIntentKey(RenderingIntent intent);   // "perceptual", "relative", "saturation", "absolute"
std::optional<RenderingIntent> renderingIntentFromKey(const std::string& key);

struct ConvertOptions {
    RenderingIntent intent = RenderingIntent::RelativeColorimetric;
    bool blackPointCompensation = true;
};

/// The pixel layouts a transform reads and writes.
enum class PixelFormat {
    RGBA8,      // premultiplied bytes (Image)
    RGBA16,     // premultiplied 0..32768 (Image16)
    RGBFloat,   // three straight floats, 0..1 (single colours)
    CMYKA8,     // premultiplied bytes, inverted ink, then alpha: 5 samples (ImageC8)
    CMYKA16,    // premultiplied 0..32768, inverted ink, then alpha: 5 samples (Image16 with 5 channels)
    LabA8,      // premultiplied bytes, a and b offset by 128 (Image in a Lab document)
    LabA16,     // premultiplied 0..32768, a and b offset by 16384 (Image16 in a Lab document)
    CMYKFloat,  // four straight floats, ink 0..100 as Little CMS counts it (single colours)
    LabFloat,   // three floats: L 0..100, a and b signed (single colours)
};
/// The layout of a document's pixels at `type` in `mode` (U8 or U16).
PixelFormat pixelFormatFor(SampleType type, ColorMode mode);
/// The colour model a layout's values are in, its samples per pixel, and whether it is a single straight colour.
ColorModel pixelFormatModel(PixelFormat format);
int pixelFormatChannels(PixelFormat format);
bool isColorFormat(PixelFormat format);

/// A Little CMS transform with fixed layouts. Thread-safe to apply from several threads at once.
class ColorTransform {
public:
    ~ColorTransform();
    ColorTransform(const ColorTransform&) = delete;
    ColorTransform& operator=(const ColorTransform&) = delete;
    PixelFormat input() const { return input_; }
    PixelFormat output() const { return output_; }
    /// `count` pixels from `in` to `out` (which may be `in` when the layouts have the same size).
    void apply(const void* in, void* out, size_t count) const;

private:
    friend struct TransformFactory;
    ColorTransform() = default;
    void applyStaged(const void* in, void* out, size_t count) const;
    void* context_ = nullptr;
    void* transform_ = nullptr;
    PixelFormat input_ = PixelFormat::RGBA8, output_ = PixelFormat::RGBA8;
    bool staged_ = false;   // a CMYK or Lab layout on either side: made straight here, colours only for Little CMS
};
using ColorTransformPtr = std::shared_ptr<const ColorTransform>;

/// `from` to `to` (untagged = sRGB for RGB layouts, the default CMYK for CMYK ones, Lab D50 for Lab); null when the
/// profiles are equivalent and the layouts the same (nothing to do), or either cannot be used (a profile of another
/// model than its layout, say). Transforms come from a cache shared by every thread (the last 24 kept).
ColorTransformPtr transformBetween(const ColorProfile& from, const ColorProfile& to, const ConvertOptions& options,
                                   PixelFormat input, PixelFormat output);

/// A soft proof (View > Proof Colors): `document` through `proof`, as `display` shows it. `proofIntent` is how the
/// document goes to the proofing profile; the proof goes to the display relative colorimetric, as Photoshop does
/// with "Preserve Numbers" off. With `gamutWarning`, colours outside the proofing profile's gamut are shown as
/// `warning` (RGB bytes). Never null for usable profiles (a proof always converts).
struct ProofSettings {
    ColorProfile profile;
    RenderingIntent intent = RenderingIntent::RelativeColorimetric;
    bool blackPointCompensation = true;
    bool gamutWarning = false;
    uint8_t warning[3] = {128, 128, 128};
};
ColorTransformPtr proofTransform(const ColorProfile& document, const ColorProfile& display, const ProofSettings& proof,
                                 PixelFormat input, PixelFormat output);

/// Drops every cached transform (tests; the cache otherwise keeps the most recently used).
void clearTransformCache();
size_t transformCacheSize();

// ---- Converting pixels and colours ---------------------------------------------------------------------------------

/// Converts in place through `transform` (made for RGBA8 to RGBA8, or RGBA16 to RGBA16), rows in parallel. A null
/// transform leaves the pixels as they are. The ImageC8 form takes CMYKA8 to CMYKA8; the Image16 one also CMYKA16 to
/// CMYKA16 and LabA16 to LabA16 (the layouts must have the image's channel count), the Image one LabA8 to LabA8.
void convertImage(Image& image, const ColorTransform* transform);
void convertImage(Image16& image, const ColorTransform* transform);
void convertImage(ImageC8& image, const ColorTransform* transform);
/// Any layout to any other at the same depth: `image`'s values in `fromMode` and `from` to `toMode` and `to` (untagged
/// profiles as transformBetween takes them), in a new buffer (5 channels for CMYK). The same buffer when there is nothing
/// to do; null when the depth is 32-bit, the buffer's channels are not `fromMode`'s, or a profile cannot be used.
AnyImage convertImage(const AnyImage& image, ColorMode fromMode, const ColorProfile& from, ColorMode toMode, const ColorProfile& to,
                      const ConvertOptions& options = {});
/// The same from `from` to `to`; false when either profile cannot be used (the pixels are then unchanged).
bool convertImage(Image& image, const ColorProfile& from, const ColorProfile& to, const ConvertOptions& options = {});
bool convertImage(Image16& image, const ColorProfile& from, const ColorProfile& to, const ConvertOptions& options = {});
/// A 16-bit image to 8 bits through `transform` (RGBA16 to RGBA8) in one pass: the canvas's display reduction fused
/// with the display transform.
void convertImage16To8(const Image16& in, Image& out, const ColorTransform& transform);
/// Any layout to 8-bit RGBA through `transform` (made from the image's layout to RGBA8) in one pass: the display of an
/// RGB, CMYK or Lab document at either depth. False, leaving `out` as it was, when the transform's input is not the
/// buffer's layout (its depth and channel count).
bool convertImageTo8(const AnyImage& in, Image& out, const ColorTransform& transform);

/// One straight colour (0..1 each) from `from` to `to`.
void convertColor(const ColorProfile& from, const ColorProfile& to, double rgb[3], const ConvertOptions& options = {});
void convertColor(const ColorProfile& from, const ColorProfile& to, uint8_t rgb[3], const ConvertOptions& options = {});

/// Convert to Profile over a whole document (one undo step for the caller): every raster layer's pixels (masks are
/// not colour), and the colours stored as values: text and shape colours, vector shape fills and strokes, artboard
/// backgrounds, layer style colours and gradients, Photo Filter and Gradient Map colours. Smart objects keep their
/// source; their placed pixels are converted. The document then carries `to`. False, with `why`, when a profile
/// cannot be used (nothing is changed).
bool convertDocumentProfile(Document& document, const ColorProfile& to, const ConvertOptions& options, std::string* why = nullptr);

// ---- Transfer curves (for code that works in linear light) -------------------------------------------------------

/// A profile's tone curve (its red TRC; the working spaces use one curve for all three), tabulated both ways so
/// callers can linearise and re-encode without Little CMS: sRGB's piecewise curve for sRGB and Display P3,
/// gamma 2.2 (563/256) for Adobe RGB, 1.8 for ProPhoto, a table for anything else. An untagged document, and a
/// profile without an RGB tone curve (a LUT profile), get sRGB's.
class TransferCurve {
public:
    enum class Kind { SRGB, Gamma, Table };
    Kind kind() const { return kind_; }
    /// The gamma for Kind::Gamma, else Little CMS's estimate of the curve's overall gamma.
    double gamma() const { return gamma_; }
    /// 0..1 encoded to 0..1 linear light, and back.
    float toLinear(float encoded) const;
    float fromLinear(float linear) const;

    static TransferCurve srgb();
    static TransferCurve ofProfile(const ColorProfile& profile);

private:
    Kind kind_ = Kind::SRGB;
    double gamma_ = 2.2;
    std::shared_ptr<const std::vector<float>> forward_, inverse_;   // Kind::Table: 4096 + 1 samples each way
};
/// The document's transfer curve (its profile's, sRGB when untagged).
TransferCurve documentTransfer(const Document& document);

} // namespace compositor
