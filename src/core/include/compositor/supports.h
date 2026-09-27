// Which features work at which sample type (docs/high-bit-depth-plan.md, sections 3 and 9): while ports land,
// a feature not yet ported to a document's depth is refused (greyed out in menus, an error from automation),
// as Photoshop greys out what a mode cannot do. Every feature supports 8-bit.
//
// Features are named by a stable id, "<area>.<name>": "adjustment.Levels" (adjustmentKindName), "filter.<id>",
// "tool.<name>", "export.<format>" and so on. An id the registry does not list is 8-bit only, so a new feature is
// safe by default and a port declares itself by adding a line to the table in supports.cpp.
#pragma once
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

struct FeatureSupport { std::string_view feature; SampleTypes types; };
/// The features the registry lists (P2: the renderer, the layer structure, masks and the files at 16 bits).
const FeatureSupport* featureSupportTable(size_t& count);

} // namespace compositor
