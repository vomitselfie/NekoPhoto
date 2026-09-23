// The readers behind importBrushFile (brushimport.h), one per format. Internal to the core.
#pragma once
#include "compositor/brushimport.h"
#include <cstdint>
#include <optional>
#include <string>

namespace compositor {

/// Photoshop brushes: .abr versions 1 and 2 (computed and sampled brushes) and 6 to 10 (sampled tips in the
/// samp section, presets and their dynamics in the desc descriptor).
std::optional<BrushImport> readAbr(const uint8_t* data, size_t size, const std::string& name, std::string* error);

/// Procreate brushes: a .brushset (a ZIP of brush folders listed in brushset.plist) or a single .brush.
std::optional<BrushImport> readProcreate(const uint8_t* data, size_t size, const std::string& name, std::string* error);

} // namespace compositor
