#include "compositor/adjustments.h"
#include <nlohmann/json.hpp>

namespace compositor {

std::string defaultAdjustmentJson(AdjustmentKind kind) {
    nlohmann::json j;
    j["kind"] = adjustmentKindName(kind);
    j["hue"] = 0; j["saturation"] = 0; j["lightness"] = 0; j["colorize"] = false;
    j["levels"] = {{"channel", "RGB"}, {"ranges", nlohmann::json::array()}};
    for (int i = 0; i < 4; i++) j["levels"]["ranges"].push_back({{"black", 0}, {"gamma", 1}, {"white", 255}, {"outputBlack", 0}, {"outputWhite", 255}});
    j["curves"] = {{"channel", "RGB"}, {"channels", nlohmann::json::array()}};
    for (int i = 0; i < 4; i++) j["curves"]["channels"].push_back({{{"x", 0}, {"y", 0}}, {{"x", 255}, {"y", 255}}});
    return j.dump();
}

// Rendering of adjustment kinds lands with milestone 3; until then an adjustment
// layer leaves the composite unchanged.
bool applyAdjustment(const LayerAdjustment&, Image&, const Rect&, double) { return false; }

} // namespace compositor
