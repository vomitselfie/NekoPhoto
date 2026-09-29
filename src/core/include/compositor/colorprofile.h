// A document's colour profile (docs/color-management.md): the ICC bytes exactly as they came (a PSD's resource 1039,
// a PNG's iCCP, a project's profile.icc), so an unchanged profile is written back byte for byte, with the description
// and colour model read from them. An empty profile is an untagged document, which is treated as sRGB.
#pragma once
#include "colormodes.h"
#include <cstdint>
#include <string>
#include <vector>

namespace compositor {

enum class ColorModel { RGB, Gray, CMYK, Lab, Other };

/// The profile model a document of `mode` carries.
constexpr ColorModel colorModelOf(ColorMode mode) { return mode == ColorMode::CMYK ? ColorModel::CMYK : mode == ColorMode::Lab ? ColorModel::Lab : ColorModel::RGB; }

struct ColorProfile {
    std::vector<uint8_t> icc;          // verbatim; empty: untagged
    std::string description;           // the profile's 'desc', e.g. "Adobe RGB (1998)"
    ColorModel model = ColorModel::RGB;

    bool empty() const { return icc.empty(); }
    bool operator==(const ColorProfile& o) const { return icc == o.icc; }
    bool operator!=(const ColorProfile& o) const { return !(*this == o); }
};

} // namespace compositor
