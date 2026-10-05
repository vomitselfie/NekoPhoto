// Blend If (Layer Style ▸ Blending Options): a layer, or a folder, shows only where its own colours ("This Layer")
// and the colours under it ("Underlying Layer") fall within ranges per channel. Each range has a black and a white
// point, each split in two (Alt-drag in Photoshop), with a linear ramp between the halves.
//
// The ranges live in the PSD layer record's blending ranges, which the layer's carry keeps as the file's bytes
// (psd_carry.h): each range is four bytes (black low, black high, white low, white high), a channel is the source
// range then the destination range, and the channels are the composite (Gray) then each colour channel of the
// document (R, G, B; C, M, Y, K; L, a, b), then one for transparency. An edit patches the channels and keeps the
// rest; an unedited layer keeps its bytes.
//
// Drawing (Photoshop 2026, calibrated on Patchy's photoshop-blend-if-4b fixture): the channels' gates multiply, and
// This Layer's gate multiplies Underlying Layer's. Byte v in a split black range [a, b] keeps (v - a + 1) / (b - a + 1)
// for a <= v < b, nothing below a; the white side mirrors it. Gray is (299 R + 590 G + 111 B) / 1000, rounded. A
// transparent backdrop always passes: the underlying gate is (1 - alpha) + alpha * gate.
#pragma once
#include "colormodes.h"
#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace compositor {

struct Layer;

struct BlendIfRange {
    uint8_t blackLow = 0, blackHigh = 0, whiteLow = 255, whiteHigh = 255;
    bool identity() const { return blackLow == 0 && blackHigh == 0 && whiteLow == 255 && whiteHigh == 255; }
    /// How much of a pixel whose channel reads `v` (0..255) the range keeps, 0..1.
    float factor(int v) const;
    bool operator==(const BlendIfRange&) const = default;
};

struct BlendIfChannel {
    BlendIfRange thisLayer, underlying;
    bool operator==(const BlendIfChannel&) const = default;
};

struct BlendIf {
    static constexpr int maxChannels = 5;   // Gray and CMYK's four inks
    /// [0] the composite (Gray; Lab's is not offered and reads Lightness), then the document's colour channels.
    std::array<BlendIfChannel, maxChannels> channels{};
    bool identity() const;
    bool operator==(const BlendIf&) const = default;
};

/// The channels Blend If offers in a colour mode, in Photoshop's menu order, as indices into BlendIf::channels:
/// Gray, Red, Green, Blue; Gray, Cyan, Magenta, Yellow, Black; Lightness, a, b.
std::vector<int> blendIfChannels(ColorMode mode);
/// Their names (English, untranslated keys for the dialog).
const char* blendIfChannelName(ColorMode mode, int channel);

/// The ranges in a record's blending-ranges bytes; none when they are missing or malformed.
std::optional<BlendIf> parseBlendIf(const std::vector<uint8_t>& bytes, ColorMode mode);
/// The bytes for `blendIf`: `original` with the composite and colour channels patched (its tail kept), or a fresh
/// block with an identity transparency pair; empty when both are identity and there was nothing.
std::vector<uint8_t> encodeBlendIf(const BlendIf& blendIf, const std::vector<uint8_t>& original, ColorMode mode);

/// The layer's (or folder's) Blend If when it does anything; none otherwise.
std::optional<BlendIf> layerBlendIf(const Layer& layer, ColorMode mode);
/// The layer's ranges for editing (identity when it has none).
BlendIf editableBlendIf(const Layer& layer, ColorMode mode);
/// Gives the layer `blendIf`, through a copy of its carry; a layer whose ranges already read so keeps its bytes.
/// Returns whether anything changed.
bool setLayerBlendIf(Layer& layer, const BlendIf& blendIf, ColorMode mode);

/// The gates as tables, made once per layer per render.
struct BlendIfGate {
    ColorMode mode = ColorMode::RGB;
    int count = 0;                                     // composite + colour channels
    std::array<std::array<float, 256>, BlendIf::maxChannels> source{}, under{};
    std::array<bool, BlendIf::maxChannels> sourceOn{}, underOn{};
    bool anySource = false, anyUnder = false;
    explicit BlendIfGate(const BlendIf& b, ColorMode m);
    /// The composite channel's value for straight 0..255 colour samples in the document's layout.
    int gray(const int* c) const;
    /// This Layer's gate for straight samples `c` (C of them).
    float sourceFactor(const int* c) const { return anySource ? factor(source, sourceOn, c) : 1.0f; }
    /// Underlying Layer's gate for straight samples `c` at alpha `alpha` (0..1); transparency passes.
    float underFactor(const int* c, float alpha) const {
        if (!anyUnder || alpha <= 0) return 1.0f;
        return (1 - alpha) + alpha * factor(under, underOn, c);
    }

private:
    float factor(const std::array<std::array<float, 256>, BlendIf::maxChannels>& t, const std::array<bool, BlendIf::maxChannels>& on, const int* c) const;
};

} // namespace compositor
