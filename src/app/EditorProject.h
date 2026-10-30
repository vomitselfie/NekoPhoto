// A project read from disk, before any session takes it (EditorSession::readProject, File > Revert).
#pragma once
#include "nekophoto/document.h"
#include <QString>
#include <optional>

namespace app {

/// A project read from disk with its saved active layer, before any session takes it.
struct LoadedProject { nekophoto::Document document; std::optional<nekophoto::Uuid> activeLayer; QString path; };

} // namespace app
