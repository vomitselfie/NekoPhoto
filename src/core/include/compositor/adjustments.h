// Adjustment layers and the destructive filters that share their math.
#pragma once
#include "document.h"

namespace compositor {

/// Applies `adjustment` to `image` in place. `region` is the document rectangle the image covers
/// and `scale` its output pixels per document pixel (Grain keeps its pattern fixed on the document).
/// Returns false for a kind this build cannot render yet; the image is then left alone.
bool applyAdjustment(const LayerAdjustment& adjustment, Image& image, const Rect& region, double scale);

/// The default settings JSON for a new adjustment layer of `kind`.
std::string defaultAdjustmentJson(AdjustmentKind kind);

} // namespace compositor
