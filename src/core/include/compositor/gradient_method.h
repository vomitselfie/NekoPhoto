// Photoshop's gradient Method (Classic, Perceptual, Linear): the space a gradient's colours blend in. Shared by layer
// styles, fill layers, the Gradient tool and Gradient Map (docs/layer-styles.md, "Gradient methods").
#pragma once
#include <string>

namespace compositor {

/// Classic blends the stored sRGB values, Linear blends in linear light, Perceptual in Oklab.
enum class GradientMethod { Classic, Perceptual, Linear };

/// "classic", "perceptual" or "linear" (projects and automation).
const char* gradientMethodKey(GradientMethod method);
/// The method a key names (any case); false for none.
bool parseGradientMethod(const std::string& key, GradientMethod& out);

/// One run of a gradient in `method`'s space: from stop `l` to stop `r` at `u` (0..1, after the midpoint remap), with
/// their neighbours `p` and `n` easing it by `smoothness` (0..1) as Photoshop's smoothness does. Colours are straight
/// sRGB, 0..1, in and out. Classic applies the smoothness as given (callers decide when Classic smooths).
void gradientMethodRun(GradientMethod method, double smoothness, const double p[3], const double l[3], const double r[3], const double n[3], double u,
                       double out[3]);

} // namespace compositor
