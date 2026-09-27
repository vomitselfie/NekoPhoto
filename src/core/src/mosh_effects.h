// The Mosh effects' entry points (mosh_effects.cpp), looked up by OpenMosh id.
#pragma once
#include "mosh_runtime.h"
#include <functional>
#include <string_view>

namespace compositor::mosh {

/// Receives each finished output row, straight colour.
using RowSink = std::function<void(int y, const rt::vec4* row)>;
/// An effect: its passes over `in` with the uniforms `u`, the last one's rows handed to `sink`.
using EffectFn = void (*)(const rt::Frame& in, const rt::Uniforms& u, const RowSink& sink);

/// The effect with OpenMosh id `id`; null when it is not ported.
EffectFn effectFunction(std::string_view id);

} // namespace compositor::mosh
