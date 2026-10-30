// What the parts of the PSD reader share (psd.cpp: the file's sections and the import; psd_parse.cpp: channel data,
// colour and layer records; psd_settings.cpp: Photoshop's settings as NekoPhoto's, and what is carried for export).
// Internal to the core.
#pragma once
#include "nekophoto/adjustments.h"
#include "nekophoto/colormgmt.h"
#include "nekophoto/colour.h"
#include "nekophoto/document.h"
#include "photoshop.h"
#include <algorithm>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace nekophoto::psd_read {

// ---- Channel data ------------------------------------------------------------------------------------

struct RecordChannel { int id = 0; uint64_t length = 0; const uint8_t* data = nullptr; };

/// Decodes one plane of `width` x `height` samples to 8 bits from `compression` (0 raw, 1 RLE, 2 zip,
/// 3 zip with prediction) and `depth` bits. False when the data does not add up.
bool decodePlane(const uint8_t* data, size_t size, int compression, int width, int height, int depth, bool psb, std::vector<uint8_t>& out, std::string* why,
                 std::vector<uint16_t>* wide = nullptr, std::vector<uint8_t>* raw16 = nullptr, std::vector<float>* floats = nullptr);

// ---- Colour ---------------------------------------------------------------------------------------

enum ColourMode { Bitmap = 0, Grayscale = 1, Indexed = 2, RGB = 3, CMYK = 4, Multichannel = 7, Duotone = 8, Lab = 9 };

/// Premultiplied RGBA from the decoded planes of a layer or the composite.
std::shared_ptr<Image> assemble(int mode, int width, int height, const std::map<int, std::vector<uint8_t>>& planes, const std::vector<uint8_t>& palette,
                                const CmykToSrgb* cmykProfile = nullptr);
/// An RGB or grayscale 16-bit document's planes (0..32768, straight colour) as a premultiplied 16-bit image.
std::shared_ptr<Image16> assemble16(int mode, int width, int height, const std::map<int, std::vector<uint16_t>>& planes);
/// A 32-bit file's planes (straight linear colour, alpha) as premultiplied linear float, NaN and infinities cleaned.
std::shared_ptr<ImageF> assembleF(int mode, int width, int height, const std::map<int, std::vector<float>>& planes);
/// A CMYK or Lab file's planes premultiplied at the document's layout, at 8 and at 16 bits.
AnyImage assembleMode8(int mode, int width, int height, const std::map<int, std::vector<uint8_t>>& planes);
AnyImage assembleMode16(int mode, int width, int height, const std::map<int, std::vector<uint16_t>>& planes);

// ---- Layer records --------------------------------------------------------------------------------

/// One side of a rectangle whose corners came from the file. The subtraction is 64-bit because `right -
/// left` on two raw i32 overflows, and the result is clamped to what a buffer can hold, so a rectangle can
/// never size an allocation or a loop beyond the pixel format's limit.
inline int extentOf(int low, int high) { return int(std::clamp<int64_t>(int64_t(high) - int64_t(low), 0, maxImageSide)); }

struct MaskRecord {
    bool present = false; int top = 0, left = 0, bottom = 0, right = 0; uint8_t defaultColour = 0; uint8_t flags = 0;
    // With a vector mask too, the painted mask is the "real" one (channel -3) with its own rectangle, and
    // the -2 plane is the vector mask rendered (flag bit 3).
    bool real = false; int realTop = 0, realLeft = 0, realBottom = 0, realRight = 0; uint8_t realDefault = 0, realFlags = 0;
    std::vector<uint8_t> section;   // the whole mask section, as stored
    int width() const { return extentOf(left, right); }
    int height() const { return extentOf(top, bottom); }
    int realWidth() const { return extentOf(realLeft, realRight); }
    int realHeight() const { return extentOf(realTop, realBottom); }
};

struct Record {
    int top = 0, left = 0, bottom = 0, right = 0;
    std::vector<RecordChannel> channels;
    std::string blend = "norm";
    uint8_t opacity = 255, clipping = 0, flags = 0, fillOpacity = 255;
    MaskRecord mask;
    std::string name;
    int section = 0;                                   // lsct: 1 open folder, 2 closed folder, 3 the folder's end marker
    std::map<std::string, std::pair<const uint8_t*, size_t>> blocks;   // tagged blocks by key
    std::vector<std::pair<std::string, std::pair<const uint8_t*, size_t>>> ordered;   // and in file order
    std::vector<uint8_t> blendingRanges;
    int width() const { return extentOf(left, right); }
    int height() const { return extentOf(top, bottom); }
};

/// Whether a PSB stores the block's length in 8 bytes.
bool psbLongKey(const std::string& key);
/// Tagged blocks (`8BIM`/`8B64`, key, length, data) up to `end`; `pad` rounds the data length.
void readTaggedBlocks(photoshop::Reader& r, size_t end, bool psb, size_t pad, std::map<std::string, std::pair<const uint8_t*, size_t>>& blocks,
                      std::vector<std::pair<std::string, std::pair<const uint8_t*, size_t>>>* ordered = nullptr);
Record readRecord(photoshop::Reader& r, bool psb);

// ---- Photoshop settings as ours (psd_settings.cpp) ------------------------------------------------

BlendMode blendFor(const std::string& key, bool* lossy);
const char* blendDescription(const std::string& key);

/// Image resources written back on PSD export (psd_carry.h).
bool carriedResource(uint16_t id, int mode);
/// Global tagged blocks written back.
bool carriedGlobalBlock(const std::string& key);
/// Per-layer blocks written back.
bool carriedLayerBlock(const std::string& key, bool modelledAdjustment, bool smartObject = false);

std::optional<AdjustmentSettings> levelsFrom(const uint8_t* data, size_t size, bool grey);
std::optional<AdjustmentSettings> curvesFrom(const uint8_t* data, size_t size, bool grey);
std::optional<AdjustmentSettings> hueSaturationFrom(const uint8_t* data, size_t size);
std::optional<AdjustmentSettings> exposureFrom(const uint8_t* data, size_t size, bool grey);
/// Brightness/Contrast: Photoshop 2026's descriptor ('CgEd') when it reads, else the legacy 'brit'.
std::optional<AdjustmentSettings> brightnessContrastFrom(const std::pair<const uint8_t*, size_t>* cged, const std::pair<const uint8_t*, size_t>* brit);
/// The other adjustment layers by their block's key (adjustments_more.cpp); none when the block does not read.
std::optional<AdjustmentSettings> simpleAdjustmentFrom(const std::string& key, const uint8_t* data, size_t size);
std::optional<AdjustmentSettings> gradientMapFrom(const uint8_t* data, size_t size, int mode, const ColorProfile& profile);
/// A solid colour fill layer's colour, from its descriptor.
std::optional<AdjustmentColor> solidColourFrom(const uint8_t* data, size_t size);
/// The text of a type layer, from its descriptor.
std::optional<std::string> textFrom(const uint8_t* data, size_t size);
std::string jsonEscape(const std::string& s);

} // namespace nekophoto::psd_read
