// Animated GIF: every frame opens as its own layer, as the frame shows once the ones before it are drawn and
// disposed (so each layer is a whole picture), named "Frame N (D ms)". Frames stack in order, frame 1 at the
// bottom; only frame 1 is visible, so the canvas shows what a viewer shows first. Still GIFs keep opening as
// plain images (through Qt) and never come here. Written for NekoPhoto from the GIF89a specification (Patchy
// only writes GIFs).
#pragma once
#include "psd.h"
#include <optional>
#include <string>
#include <vector>

namespace compositor {

/// How many frames (image descriptors) the GIF holds; 0 when it is not a GIF or is damaged before the first.
int gifFrameCount(const std::vector<uint8_t>& bytes);
int gifFrameCount(const std::string& path);

std::optional<PsdImport> importGifBytes(const std::vector<uint8_t>& bytes, std::string* error = nullptr);
std::optional<PsdImport> importGif(const std::string& path, std::string* error = nullptr);

} // namespace compositor
