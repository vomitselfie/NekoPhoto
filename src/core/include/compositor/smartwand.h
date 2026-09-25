// The edge-aware Magic Wand: the click samples a small patch as a colour model in OKLab, image edges (at
// three blur scales) are barriers, and the cost of reaching each pixel from the patch is computed once as a
// field. Tolerance then only thresholds the field, so it can change without recomputing anything.
//
// The cost of a step is the pixel's perceptual distance from the patch (less the patch's own spread), its
// alpha difference, and the edge strength there, in the classic wand's units: tolerance 32 means about what
// it meant for the plain wand on flat colour. Two ways to accumulate it along a path:
//   Bottleneck: a pixel costs the worst step on its best path from the seed. Its selection at a tolerance
//               is the region the seed reaches without crossing anything costlier, like a flood fill, but
//               with perceptual distances and edges as walls.
//   Additive:   the steps' costs add up (a geodesic distance), so a long walk through a faint gradient
//               also costs (and, with a toll, every step does).
// See docs/smart-wand.md for the research behind it and how the two compare.
#pragma once
#include "image.h"
#include <cstdint>
#include <memory>
#include <vector>

namespace compositor {

enum class WandAccumulation { Bottleneck, Additive };

struct SmartWandOptions {
    WandAccumulation accumulation = WandAccumulation::Bottleneck;
    double edgeWeight = 0;        // multi-scale gradient barrier; the neighbour step does this better (docs/smart-wand.md)
    double alphaWeight = 1.0;
    double stepToll = 0;          // Additive only: cost per pixel stepped, in tolerance units
    /// How much the step from pixel to pixel counts, beside the distance from the click's colour: shading
    /// changes little per pixel, a boundary a lot. 0 compares with the click only (the classic wand's way).
    double neighbourWeight = 8;
    /// With a textured click, how much a smoother neighbourhood costs (a textured fill should not run into
    /// flat colour of the same mean).
    double textureWeight = 2;
    /// When the neighbour term is on, the share of the distance from the click still counted.
    double seedWeight = 0.7;
};

/// Everything a wand needs from an image that does not depend on where it is clicked, computed once per image.
class SmartWandImage {
public:
    /// `edges`: also prepare the multi-scale edge map (only an edgeWeight above zero reads it).
    explicit SmartWandImage(const Image& pixels, bool edges = false);
    int width() const { return width_; }
    int height() const { return height_; }

    /// The field for a click: each pixel's cost in tolerance units times four (0..65534), 65535 beyond `limit`
    /// tolerance or unreachable. `radius` is the sample patch's half size.
    struct Field {
        int width = 0, height = 0;
        std::vector<uint16_t> cost;
        int limit = 0;   // in tolerance units: thresholds above it are not meaningful
    };
    Field propagate(int seedX, int seedY, int radius, int limit, const SmartWandOptions& options = {}) const;

    /// OKLab of pixel i, scaled: L 0..1 and a, b about -0.4..0.4, times 4096 (for tests and the patch model).
    void oklab(size_t i, float out[3]) const { out[0] = lab_[i * 3] / 4096.0f; out[1] = lab_[i * 3 + 1] / 4096.0f; out[2] = lab_[i * 3 + 2] / 4096.0f; }

private:
    int width_ = 0, height_ = 0;
    std::vector<int16_t> lab_;     // straight-colour OKLab, three per pixel, times 4096
    std::vector<uint8_t> alpha_;
    std::vector<uint16_t> edge_;   // multi-scale gradient magnitude of L, in tolerance units times four
    std::vector<uint16_t> spread_; // local colour spread (5 x 5 window) in tolerance units times four
    std::vector<int16_t> localMean_;   // local mean OKLab (the same window), times 4096: texture averaged out
};

/// Selects where the field is within `tolerance` (0..255): 255 inside, a two-level soft band just past it
/// for antialiasing (`soft`), 0 elsewhere. Returns the count at or above half.
long thresholdWandField(const SmartWandImage::Field& field, int tolerance, bool soft, GrayImage& mask);

} // namespace compositor
