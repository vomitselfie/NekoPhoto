// The display's precalculated transform for 16-bit CMYK and Lab documents (colormgmt.h, displayTransformBetween;
// docs/color-management.md, "The display grid"). Internal to the core: colormgmt.cpp applies it through
// ColorTransform::apply, colormgmt_display.cpp builds it.
#pragma once
#include "nekophoto/colormgmt.h"
#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace nekophoto {

/// What a precalculated display transform holds. Built once per (document profile, monitor profile, intent, black
/// point compensation, layout) and never changed after, so it is shared by every thread drawing at once.
struct DisplayGrid {
    enum class Kind { Cmyk, Lab };
    Kind kind = Kind::Cmyk;
    /// CMYK: nodes a side, and a stored sample (inverted ink, 0..32768) to its grid coordinate 0..size-1 (the same for
    /// every ink; nodes are closer together towards no ink and full ink).
    int size = 0;
    std::vector<float> coordinate;
    /// CMYK: size^4 nodes, cyan slowest and black fastest, each the monitor's linear light (red, green, blue), not
    /// clipped: Little CMS's float pipeline at the node.
    std::vector<float> nodes;
    /// Lab: the monitor's linear light as an affine function of the cube-law terms of Lab (f⁻¹ of fx, fy and fz, the
    /// white folded in): three rows of four (weights, then offset).
    std::array<float, 12> affine{};
    /// Lab: a = b = 0 maps to equal red, green and blue (the rows agree); such pixels are drawn with one value.
    bool neutralLab = false;
    float neutralWeight = 0, neutralOffset = 0;
    /// Linear light (0..1, clipped) to the monitor's byte as Little CMS rounds it, indexed by sqrt(linear) * 65535
    /// (finer towards black, where the curve is steep).
    std::array<std::vector<uint8_t>, 3> encode;
    /// Per channel: below this linear light the monitor's curve is steep enough to show the grid's error, so the pixel
    /// goes through `exact` instead. Lowest float: never.
    std::array<float, 3> shadow{};
    ColorTransformPtr exact;

    /// `count` premultiplied pixels of the document's layout (CMYKA16 or LabA16) to premultiplied RGBA8.
    void apply(const void* in, void* out, size_t count) const;
};

/// A ColorTransform that applies `grid` (colormgmt.cpp, which owns the transform object).
ColorTransformPtr makeGridTransform(std::shared_ptr<const DisplayGrid> grid, PixelFormat input);

} // namespace nekophoto
