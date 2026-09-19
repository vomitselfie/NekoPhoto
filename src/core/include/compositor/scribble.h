// Quick Select by scribble: the subject from a few foreground and background strokes, by GrabCut (Rother,
// Kolmogorov & Blake 2004) on a reduced copy, for a selection that needs no model and works in every build
// with OpenCV (docs/background-removal-review.md item 9). The refinement to the image's edges is the matte
// panel's, applied by the caller.
#pragma once
#include "image.h"
#include <memory>
#include <string>

namespace compositor {

/// Whether this build can run GrabCut (it needs OpenCV's imgproc).
bool scribbleSelectionSupported();

/// The subject as coverage (white) from `labels` at the image's size: 1 where the user marked foreground, 2
/// background, 0 elsewhere. GrabCut runs on a copy no larger than `limit` on its longest side for `iterations`
/// rounds, everything inside the foreground strokes' surroundings starting as probable foreground and the rest
/// as probable background; the coverage comes back at full size with a soft edge. Null with `error` when there
/// is no foreground stroke or the build has no OpenCV.
std::shared_ptr<GrayImage> scribbleSelection(const Image& image, const GrayImage& labels, int limit, int iterations, std::string* error);

} // namespace compositor
