// Aseprite sprites (.aseprite, .ase): the layers with their visibility, opacity (layer x cel),
// blend modes (Addition as Linear Dodge) and groups, in RGBA, grayscale or indexed colour. A sprite of several frames
// opens with a layer per cel (named "Layer (frame N)" when a layer has more than one) and a timeline (animation.h)
// with the frame durations, linked cels sharing one layer; tilemap layers are left out with a note.
// Ported from Patchy (MIT, src/third_party/patchy_psd/README.md): src/formats/aseprite_document_io.cpp, with
// zlib in place of its vendored miniz.
#pragma once
#include "psd.h"
#include <optional>
#include <string>
#include <vector>

namespace compositor {

bool isAsepriteData(const uint8_t* data, size_t size);
std::optional<PsdImport> importAsepriteBytes(const std::vector<uint8_t>& bytes, std::string* error = nullptr);
std::optional<PsdImport> importAseprite(const std::string& path, std::string* error = nullptr);

} // namespace compositor
