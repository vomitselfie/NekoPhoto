// The readers behind importBrushFile (brushimport.h), one per format. Internal to the core.
#pragma once
#include "compositor/brushimport.h"
#include <cstdint>
#include <optional>
#include <string>

namespace compositor {

/// A round tip as an image: an ellipse `diameter` wide (at most 1024), `roundness` 0..1 as tall, with
/// `hardness` 0..1 of its radius solid and a smooth rim beyond.
std::shared_ptr<GrayImage> roundTipImage(double diameter, double hardness, double roundness);

/// Photoshop brushes: .abr versions 1 and 2 (computed and sampled brushes) and 6 to 10 (sampled tips in the
/// samp section, presets and their dynamics in the desc descriptor).
std::optional<BrushImport> readAbr(const uint8_t* data, size_t size, const std::string& name, std::string* error);

/// Procreate brushes: a .brushset (a ZIP of brush folders listed in brushset.plist) or a single .brush.
std::optional<BrushImport> readProcreate(const uint8_t* data, size_t size, const std::string& name, std::string* error);

/// Clip Studio Paint brushes (.sut, an SQLite database): each tool node's size, spacing, hardness, angle,
/// thickness and spray. The tip images are kept in a format that is not publicly documented, so a round
/// tip stands in. Needs SQLite at build time (COMPOSITOR_HAVE_SQLITE).
std::optional<BrushImport> readClipStudio(const std::string& path, const std::string& name, std::string* error);

} // namespace compositor
