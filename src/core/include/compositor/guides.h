// Ruler guides, as Photoshop models them: vertical or horizontal lines at a document position, saved with the
// document. In a PSD they are image resource 1032 (GridAndGuidesInfo): a version (1), the grid's horizontal and
// vertical cycle, a count, then per guide its position in 1/32 pixel (signed) and a direction byte (0 vertical,
// 1 horizontal). Positions here are document pixels, kept to the PSD's 1/32 pixel steps.
#pragma once
#include <cstdint>
#include <optional>
#include <vector>

namespace compositor {

struct Guide {
    enum class Orientation : uint8_t { Vertical = 0, Horizontal = 1 };
    Orientation orientation = Orientation::Vertical;
    double position = 0;   // x for a vertical guide, y for a horizontal one, in document pixels
    bool vertical() const { return orientation == Orientation::Vertical; }
    bool operator==(const Guide&) const = default;
};

/// A position on the PSD's 1/32 pixel grid (what a guide can hold).
double guidePosition(double position);
/// Guides farther than this outside the canvas are refused (Photoshop keeps them within the pasteboard).
constexpr double guideReach = 30000;
/// Photoshop's limit is far beyond what anyone draws; this keeps a hostile file from holding millions.
constexpr size_t maxGuides = 10000;

/// The guides in resource 1032; false when the bytes are not one. `grid` receives the two grid cycle words.
bool parseGuidesResource(const std::vector<uint8_t>& data, std::vector<Guide>& guides, std::optional<std::pair<uint32_t, uint32_t>>* grid = nullptr);
/// Resource 1032 for `guides`, with `grid`'s cycle words (Photoshop's default when none: 576 each, 18 points).
std::vector<uint8_t> guidesResource(const std::vector<Guide>& guides, std::optional<std::pair<uint32_t, uint32_t>> grid = std::nullopt);

/// Guides after the canvas changed: moved by (dx, dy) (Canvas Size's anchor, a crop's origin), then scaled
/// (Image Size); a flip mirrors them across the canvas `width` or `height`.
void offsetGuides(std::vector<Guide>& guides, double dx, double dy);
void scaleGuides(std::vector<Guide>& guides, double sx, double sy);
void flipGuides(std::vector<Guide>& guides, bool horizontal, double extent);

} // namespace compositor
