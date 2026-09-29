// Which features work at which sample type (docs/high-bit-depth-plan.md, sections 3 and 9): while ports land,
// a feature not yet ported to a document's depth is refused (greyed out in menus, an error from automation),
// as Photoshop greys out what a mode cannot do. Every feature supports 8-bit.
//
// Features are named by a stable id, "<area>.<name>": "adjustment.Levels" (adjustmentKindName), "filter.<id>",
// "tool.<name>", "export.<format>" and so on. An id the registry does not list is 8-bit only, so a new feature is
// safe by default and a port declares itself by adding a line to the table in supports.cpp.
//
// A second axis is the colour mode (colormodes.h): every feature works in RGB; in CMYK and Lab only the features the
// mode table lists do, as Photoshop greys out what a mode cannot do ("Not available in CMYK mode"). A feature must
// then also support the document's depth.
#pragma once
#include "colormodes.h"
#include "document.h"
#include "sampletype.h"
#include <cstdint>
#include <string_view>

namespace compositor {

/// A set of sample types, one bit each.
using SampleTypes = uint8_t;
constexpr SampleTypes sampleTypeBit(SampleType type) { return SampleTypes(1u << unsigned(type)); }
constexpr SampleTypes onlyEightBit = sampleTypeBit(SampleType::U8);

/// The sample types `feature` works at; 8-bit alone for a feature the registry does not list.
SampleTypes supportedSampleTypes(std::string_view feature);
inline bool supports(std::string_view feature, SampleType type) {
    return type == SampleType::U8 || (supportedSampleTypes(feature) & sampleTypeBit(type)) != 0;   // every feature works at 8 bits
}
/// An adjustment (layer or destructive), by its kind: "adjustment.<name>".
bool supports(AdjustmentKind kind, SampleType type);

/// A set of colour modes, one bit each.
using ColorModes = uint8_t;
constexpr ColorModes colorModeBit(ColorMode mode) { return ColorModes(1u << unsigned(mode)); }
/// The colour modes `feature` works in; RGB alone for a feature the mode table does not list.
ColorModes supportedColorModes(std::string_view feature);
/// Whether `feature` works in a document of `type` and `mode`.
inline bool supports(std::string_view feature, SampleType type, ColorMode mode) {
    return supports(feature, type) && (mode == ColorMode::RGB || (supportedColorModes(feature) & colorModeBit(mode)) != 0);
}
bool supports(AdjustmentKind kind, SampleType type, ColorMode mode);
/// "Not available in CMYK mode", "Not available in Lab mode": the refusal for a feature the mode lacks (empty for RGB).
std::string notAvailableInMode(ColorMode mode);
/// Whether `feature` stays RGB for good in `mode` (Camera Raw, G'MIC, the MyPaint presets), rather than waiting for a
/// port: its refusal is "Not available in CMYK mode"; a feature not ported yet says "Not available in CMYK mode yet".
bool photoshopLacksInMode(std::string_view feature, ColorMode mode);
/// Why `feature` is greyed in a document of `type` and `mode`, empty when it works: the depth's reason first
/// (notAvailableAtDepth), then the mode's ("... in CMYK mode" for good, "... in CMYK mode yet" for a port to come).
std::string unavailableReason(std::string_view feature, SampleType type, ColorMode mode);

/// A row of the capability matrix (docs/mode-matrix.md, tools/mode_matrix): a feature and what the menus call it.
struct FeatureRow { std::string_view feature; std::string_view label; };
/// Every feature the tables name, in the matrix's order.
const FeatureRow* featureRows(size_t& count);
/// Where an enabled feature still makes some of its pixels in RGB and converts them into the document's mode, what
/// does (empty when it works on the native samples throughout); the matrix marks such a cell.
std::string_view throughRgbNote(std::string_view feature, SampleType type, ColorMode mode);
/// Whether Photoshop itself has no `feature` in a 32-bit document (Dodge and Burn, the Paint Bucket, the content-aware
/// tools, Brightness/Contrast, Posterize, Threshold, Selective Color, Grain; Mosh and G'MIC here): such a feature is
/// greyed for good, not waiting for a port.
bool photoshopLacksAt32(std::string_view feature);
/// Why `feature` is greyed at `type`: "Not available in 16-bit yet", "Not available in 32-bit yet", or "Not available
/// in 32-bit mode" for what Photoshop lacks there; empty when it works.
std::string notAvailableAtDepth(std::string_view feature, SampleType type);

struct FeatureModes { std::string_view feature; ColorModes modes; };
/// The features the mode table lists beyond RGB.
const FeatureModes* featureModeTable(size_t& count);

struct FeatureSupport { std::string_view feature; SampleTypes types; };
/// The features the registry lists (P2: the renderer, the layer structure, masks and the files at 16 bits).
const FeatureSupport* featureSupportTable(size_t& count);

} // namespace compositor
