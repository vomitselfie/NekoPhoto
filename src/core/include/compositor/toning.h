// Photoshop's toning tools as images: what Dodge, Burn, Sponge and Sharpen make of a layer at full strength. The
// editor paints this through the brush tip with the tool's Exposure (Dodge, Burn), Flow (Sponge) or Strength
// (Sharpen) as opacity, so a stroke never goes past one full application, as in Photoshop. The curves are shaped
// after Photoshop's behaviour, not calibrated against it (docs/features.md).
#pragma once
#include "image.h"
#include "colormodes.h"
#include "imaget.h"

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
/// Both on a 16-bit image (0..32768): the same curves on the exact straight colour, stored at 15 bits.
void toneImage(ImageT<SampleType::U16>& image, const ToningSettings& settings);
void sharpenImage(ImageT<SampleType::U16>& image, double radius = 1.0);
/// The Sharpen tool's source at 32 bits: the same unsharp step on straight linear colour, not clamped above 1 (light
/// stays light), only below 0.
void sharpenImage(ImageT<SampleType::F32>& image, double radius = 1.0);

// ---- CMYK and Lab (docs/color-modes.md, "Painting"): in the document's own samples, nothing through RGB ----
// Lab: Dodge and Burn move L along the same curves (a and b stay: Lab keeps colour apart from lightness, so Protect
// Tones changes nothing there); Sponge scales a and b (twice the chroma, or none), L kept. CMYK: Dodge and Burn take
// each plate's brightness (the ink inverted, as stored) along the curves, so Dodge removes ink and Burn adds it, K
// included; Sponge works on C, M and Y as the RGB Sponge does on their complements, K left alone.

/// Dodge, Burn or Sponge at full strength on a Lab (`mode` Lab) or RGB image of 8 or 16 bits.
void toneImage(Image& image, const ToningSettings& settings, ColorMode mode);
void toneImage(ImageT<SampleType::U16>& image, const ToningSettings& settings, ColorMode mode);
/// The same on 8-bit CMYK.
void toneImage(ImageT<SampleType::U8>& image, const ToningSettings& settings);
/// Gaussian blur and the Sharpen tool's source on an image of four or five samples (8-bit CMYK, 16-bit CMYK or not):
/// every sample, alpha last, blurred as it is stored.
void gaussianBlurSamples(ImageT<SampleType::U8>& image, double sigma);
void gaussianBlurSamples(ImageT<SampleType::U16>& image, double sigma);
void sharpenSamples(ImageT<SampleType::U8>& image, double radius = 1.0);
void sharpenSamples(ImageT<SampleType::U16>& image, double radius = 1.0);

} // namespace compositor
