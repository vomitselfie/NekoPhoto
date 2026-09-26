// Aseprite sprites (.aseprite, .ase): the first frame's layers with their visibility, opacity (layer x cel),
// blend modes and groups, in RGBA, grayscale or indexed colour. Blend modes NekoPhoto lacks (Hard Light, Soft
// Light, Exclusion, Addition, Subtract, Divide) open as Normal and the import says so; tilemap layers and the
// frames after the first are left out with a note.
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
