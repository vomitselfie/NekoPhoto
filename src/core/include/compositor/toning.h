// Photoshop's toning tools as images: what Dodge, Burn, Sponge and Sharpen make of a layer at full strength. The
// editor paints this through the brush tip with the tool's Exposure (Dodge, Burn), Flow (Sponge) or Strength
// (Sharpen) as opacity, so a stroke never goes past one full application, as in Photoshop. The curves are shaped
// after Photoshop's behaviour, not calibrated against it (docs/features.md).
#pragma once
#include "image.h"

namespace compositor {

enum class ToningKind { Dodge, Burn, Sponge };
/// Which tones Dodge and Burn work on most.
enum class ToneRange { Shadows, Midtones, Highlights };

struct ToningSettings {
    ToningKind kind = ToningKind::Dodge;
    ToneRange range = ToneRange::Midtones;
    /// Dodge and Burn: move the lightness and keep the colour (Photoshop's Protect Tones), rather than each
    /// channel on its own.
    bool protectTones = true;
    /// Sponge: saturate (true) or desaturate.
    bool saturate = false;
};

/// A tone 0..1 dodged (lightened) or burned (darkened) at full exposure in `range`.
double dodgeTone(double v, ToneRange range);
double burnTone(double v, ToneRange range);

/// `image` (premultiplied) toned at full strength, in place; transparent pixels stay as they are.
void toneImage(Image& image, const ToningSettings& settings);
/// `image` sharpened at full strength (the Sharpen tool): an unsharp mask of 100% at `radius` pixels.
void sharpenImage(Image& image, double radius = 1.0);

} // namespace compositor
