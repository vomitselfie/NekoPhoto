// Clip Studio Paint projects (.clip): the layers, folders, masks, opacity, blend modes and clipping, as a
// Document. Needs SQLite at build time (COMPOSITOR_HAVE_SQLITE); without it the import says so.
#pragma once
#include "psd.h"
#include <optional>
#include <string>

namespace compositor {

/// Reads a .clip file. `PsdImport::composite` stays empty; `notes` lists what was approximated or left out.
std::optional<PsdImport> importClip(const std::string& path, std::string* error = nullptr);

/// Whether the file starts like a .clip (the CSFCHUNK signature).
bool isClipFile(const std::string& path);

} // namespace compositor
