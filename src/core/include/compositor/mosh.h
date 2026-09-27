// Mosh: the glitch, distortion and retro effects of OpenMosh (MIT, github.com/vomitselfie/openmosh), ported from
// its WGSL shaders to the CPU (docs/mosh.md). Effects are identified by OpenMosh's ids ("pixel-sort", "vhs") and
// take its parameters, with its keys, labels, ranges and defaults, so an OpenMosh preset's values mean the same
// here. Every effect runs on straight (unpremultiplied) colour through the shader runtime in mosh_runtime.h; the
// image's premultiplied pixels are converted at the boundary, at 8 or 16 bits.
//
// A seeded effect draws its randomness from `seed`, a number in [0, 100) as OpenMosh's Reroll makes; the same seed
// gives the same pixels on any number of threads.
#pragma once
#include "image.h"
#include "imaget.h"
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace compositor::mosh {

enum class Category { Glitch, Distort, Retro, Stylize, Color, Composite };
constexpr int categoryCount = 6;
const char* categoryName(Category category);

enum class ParamKind { Float, Bool, Choice };

/// One control. Every value is a float, as in OpenMosh: a bool is 0 or 1, a choice its option's index.
struct ParamSpec {
    std::string_view key;      // stable: presets and automation
    std::string_view label;    // OpenMosh's label, English
    ParamKind kind = ParamKind::Float;
    float min = 0, max = 1, defaultValue = 0;
    std::vector<std::string_view> options;   // a choice's option labels
};

struct EffectSpec {
    std::string_view id;       // OpenMosh's id, the API name: "pixel-sort"
    std::string_view name;     // "Pixel Sort"
    Category category = Category::Glitch;
    bool seeded = false;       // draws on the seed (the dialog offers Reroll)
    std::vector<ParamSpec> params;
};

/// Every effect NekoPhoto draws, grouped by category in OpenMosh's order.
const std::vector<EffectSpec>& effects();
const EffectSpec* findEffect(std::string_view id);

/// An effect with its values, positional in the spec's parameter order.
struct Settings {
    std::string effect;
    std::vector<float> values;
    float seed = 0;

    static Settings defaults(const EffectSpec& spec);
    /// Values held to their ranges (a bool to 0 or 1, a choice to an option), missing ones at their defaults, the
    /// seed in [0, 100); unchanged for an unknown effect.
    Settings normalized() const;
    /// The value of parameter `key`, or `fallback`.
    float value(std::string_view key, float fallback = 0) const;
    bool set(std::string_view key, float v);
};

/// A new seed in [0, 100) from any 32-bit number (the dialog's Reroll, automation's default).
float seedFrom(uint32_t n);

/// Runs the effect on premultiplied pixels in place. False for an unknown effect (the image is left alone).
bool apply(const Settings& settings, Image& image);
bool apply(const Settings& settings, Image16& image);

} // namespace compositor::mosh
