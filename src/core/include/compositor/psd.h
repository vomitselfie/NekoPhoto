// Photoshop PSD and PSB files as documents: layers with their names, positions, opacity, blend modes,
// visibility, groups, clipping and masks; Levels, Curves, Hue/Saturation, Exposure and Gradient Map
// adjustment layers as ours; solid colour fills as pixels; text and smart objects as the pixels
// Photoshop rendered; 16- and 32-bit files reduced to 8 bits; and the merged image Photoshop saved,
// which stands in when a file has no layers. Whatever cannot be carried over is listed in the notes.
#pragma once
#include "document.h"
#include <optional>
#include <string>
#include <vector>

namespace compositor {

struct PsdImport {
    Document document;
    /// Photoshop's own flattened image, for checking the import against.
    ImagePtr composite;
    /// What the import left behind, one line each (effects, unknown adjustments, rasterised text, ...).
    std::vector<std::string> notes;
};

/// Reads `path`; null with `error` set when the file is not a PSD/PSB or is damaged.
std::optional<PsdImport> importPsd(const std::string& path, std::string* error);

} // namespace compositor
