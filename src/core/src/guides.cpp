// Ruler guides and Photoshop's resource 1032 (guides.h).
#include "compositor/guides.h"
#include <algorithm>
#include <cmath>

namespace compositor {

namespace {
uint32_t readU32(const std::vector<uint8_t>& d, size_t at) {
    return uint32_t(d[at]) << 24 | uint32_t(d[at + 1]) << 16 | uint32_t(d[at + 2]) << 8 | uint32_t(d[at + 3]);
}
void writeU32(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back(uint8_t(v >> 24)); out.push_back(uint8_t(v >> 16)); out.push_back(uint8_t(v >> 8)); out.push_back(uint8_t(v));
}
} // namespace

double guidePosition(double position) {
    if (!std::isfinite(position)) return 0;
    return std::round(std::clamp(position, -guideReach, guideReach) * 32) / 32;
}

bool parseGuidesResource(const std::vector<uint8_t>& data, std::vector<Guide>& guides, std::optional<std::pair<uint32_t, uint32_t>>* grid) {
    guides.clear();
    if (data.size() < 16 || readU32(data, 0) != 1) return false;
    const uint32_t count = readU32(data, 12);
    if (count > maxGuides || data.size() < 16 + size_t(count) * 5) return false;
    if (grid) *grid = std::pair{readU32(data, 4), readU32(data, 8)};
    for (uint32_t i = 0; i < count; i++) {
        const size_t at = 16 + size_t(i) * 5;
        const int32_t location = int32_t(readU32(data, at));
        const uint8_t direction = data[at + 4];
        if (direction > 1) { guides.clear(); return false; }
        guides.push_back({direction == 0 ? Guide::Orientation::Vertical : Guide::Orientation::Horizontal, location / 32.0});
    }
    return true;
}

std::vector<uint8_t> guidesResource(const std::vector<Guide>& guides, std::optional<std::pair<uint32_t, uint32_t>> grid) {
    std::vector<uint8_t> out;
    writeU32(out, 1);
    const auto cycle = grid.value_or(std::pair<uint32_t, uint32_t>{576, 576});
    writeU32(out, cycle.first);
    writeU32(out, cycle.second);
    writeU32(out, uint32_t(guides.size()));
    for (const Guide& g : guides) {
        writeU32(out, uint32_t(int32_t(std::lround(guidePosition(g.position) * 32))));
        out.push_back(g.vertical() ? 0 : 1);
    }
    return out;
}

void offsetGuides(std::vector<Guide>& guides, double dx, double dy) {
    for (Guide& g : guides) g.position = guidePosition(g.position + (g.vertical() ? dx : dy));
}

void scaleGuides(std::vector<Guide>& guides, double sx, double sy) {
    for (Guide& g : guides) g.position = guidePosition(g.position * (g.vertical() ? sx : sy));
}

void flipGuides(std::vector<Guide>& guides, bool horizontal, double extent) {
    for (Guide& g : guides) if (g.vertical() == horizontal) g.position = guidePosition(extent - g.position);
}

} // namespace compositor
