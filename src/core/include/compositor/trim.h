// Image ▸ Trim (Photoshop's): the canvas cut down to its content, judged by transparency or by a corner pixel's
// colour, on the sides asked for. A port of upstream Compositor's ImageTrim.swift (MIT, LICENSES/MIT-Compositor.txt).
#pragma once
#include "geometry.h"
#include "image.h"
#include <cstdint>
#include <optional>

namespace compositor {

struct TrimOptions {
    enum class BasedOn { TransparentPixels, TopLeftColor, BottomRightColor } basedOn = BasedOn::TransparentPixels;
    bool top = true, bottom = true, left = true, right = true;
    uint8_t tolerance = 0;             // per channel, for the colour modes
    bool any() const { return top || bottom || left || right; }
};

/// The part of `flat` (the flattened document) to keep; none when nothing would remain (all transparent, or all the
/// corner's colour) or no side is asked for.
std::optional<Rect> trimRect(const Image& flat, const TrimOptions& options);

} // namespace compositor
