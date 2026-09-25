// The renderer's entry into layer styles (layerstyle_render.cpp).
#pragma once
#include "compositor/document.h"
#include "compositor/layerstyle.h"
#include <functional>
#include <optional>

namespace compositor {

struct StyledDraw {
    std::shared_ptr<const LayerStyle> style;
    /// The output `target` represents `region` (document pixels) at `scale`.
    Rect region;
    double scale = 1;
    /// Draws the layer's own pixels (its mask applied, full opacity, Normal) into an image covering `region`.
    std::function<void(Image& into, const Rect& region)> drawSource;
    BlendMode mode = BlendMode::Normal;
    float master = 1, fill = 1;
    /// Per output pixel, what folder masks and clipping allow; null for all.
    const GrayImage* coverage = nullptr;
    std::shared_ptr<const std::map<std::string, PatternTile>> patterns;
    int documentWidth = 0, documentHeight = 0;
    /// Where the layer's pixels can be (document pixels); the effects are worked out only around it. None: anywhere.
    std::optional<Rect> bounds;
    /// A folder's style comes in two parts around its children: the exterior effects before them, the rest after
    /// (over what the children made). A layer's is drawn whole.
    enum class Phase { Whole, Exterior, Interior } phase = Phase::Whole;
};

/// Draws the layer with its effects onto `target`.
void drawStyledLayer(const StyledDraw& draw, Image& target);

} // namespace compositor
