// Free Transform's state while it is open (EditorSession::transformEdit): the draft, the corners of a distortion, a
// group of layers moved together, and pixels floated from a selection.
#pragma once
#include "nekophoto/warp.h"
#include "nekophoto/document.h"
#include <map>
#include <optional>

namespace app {

/// Several layers transformed together: the upright box around them when the edit began, and each one's transform then.
struct TransformGroup {
    nekophoto::LayerTransform box;
    std::map<nekophoto::Uuid, nekophoto::LayerTransform> originals;
};

/// Ctrl+T with a selection: the selected pixels float on a temporary layer and merge back on Apply.
struct FloatingTransform {
    nekophoto::Uuid sourceId;
    nekophoto::Document before;
    std::optional<nekophoto::Uuid> beforeActive;
    nekophoto::LayerTransform original;
    int pixelWidth = 0, pixelHeight = 0;
};

struct TransformEdit {
    nekophoto::Uuid layerId;
    nekophoto::LayerTransform draft;
    bool persistent = false;
    bool mask = false;
    /// Set once a handle is Ctrl-dragged: the four corners move freely and Apply resamples the pixels.
    std::optional<nekophoto::Corners> corners;
    std::optional<TransformGroup> group;
    std::optional<FloatingTransform> floating;
};

} // namespace app
