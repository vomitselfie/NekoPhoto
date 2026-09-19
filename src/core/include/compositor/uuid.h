// UUIDs as the Mac app writes them: uppercase 8-4-4-4-12 strings.
#pragma once
#include <string>

namespace compositor {

using Uuid = std::string;

/// A new random (version 4) UUID.
Uuid makeUuid();
/// Whether `text` is a well-formed UUID; the result is uppercased.
bool parseUuid(const std::string& text, Uuid& out);

} // namespace compositor
