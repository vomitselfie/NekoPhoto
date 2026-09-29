// The 32-bit sheets (docs/bit-depth.md, "32 bits"): View ▸ 32-bit Preview Options and Image ▸ Mode's HDR Toning
// when a 32-bit document goes to 16 or 8 bits. Both set a View32 (Exposure and Gamma, or Highlight Compression);
// while they are open the canvas shows it (`preview`), which for HDR Toning is the conversion's result, since the view
// and the conversion share the same maths.
#pragma once
#include "compositor/view32.h"
#include <QString>
#include <functional>
#include <optional>

class QWidget;

namespace app {

/// View ▸ 32-bit Preview Options. Null when cancelled (the caller puts the view back).
std::optional<compositor::View32> askPreviewOptions(QWidget* parent, const compositor::View32& initial, const std::function<void(const compositor::View32&)>& preview);
/// HDR Toning, going to `bits` (8 or 16) bits per channel. Null when cancelled.
std::optional<compositor::View32> askHdrToning(QWidget* parent, int bits, const compositor::View32& initial, const std::function<void(const compositor::View32&)>& preview);

} // namespace app
