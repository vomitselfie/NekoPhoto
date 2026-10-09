// Blend If and Advanced Blending's Channels: the models, their bytes in a PSD layer record, and the gates the renderer
// applies (blendif.h).
#include "compositor/blendif.h"
#include "compositor/document.h"
#include "compositor/psd_carry.h"
#include <algorithm>
#include <memory>

namespace compositor {

namespace {

int colours(ColorMode mode) { return colorModeChannels(mode) - 1; }

} // namespace

float BlendIfRange::factor(int v) const {
    if (v < blackLow || v > whiteHigh) return 0;
    // A split handle's ramp includes both of its ends: the first value kept is 1 / (span + 1), and joined handles
    // cut hard.
    if (v < blackHigh) return float(v - blackLow + 1) / float(blackHigh - blackLow + 1);
    if (v > whiteLow) return float(whiteHigh - v + 1) / float(whiteHigh - whiteLow + 1);
    return 1;
}

bool BlendIf::identity() const {
    for (auto& c : channels) if (!c.thisLayer.identity() || !c.underlying.identity()) return false;
    return true;
}

std::vector<int> blendIfChannels(ColorMode mode) {
    switch (mode) {
    case ColorMode::CMYK: return {0, 1, 2, 3, 4};
    case ColorMode::Lab: return {1, 2, 3};
    case ColorMode::RGB: break;
    }
    return {0, 1, 2, 3};
}

const char* blendIfChannelName(ColorMode mode, int channel) {
    static const char* const rgb[] = {"Gray", "Red", "Green", "Blue", ""};
    static const char* const cmyk[] = {"Gray", "Cyan", "Magenta", "Yellow", "Black"};
    static const char* const lab[] = {"Lightness", "Lightness", "a", "b", ""};
    if (channel < 0 || channel >= BlendIf::maxChannels) return "";
    return mode == ColorMode::CMYK ? cmyk[channel] : mode == ColorMode::Lab ? lab[channel] : rgb[channel];
}

std::optional<BlendIf> parseBlendIf(const std::vector<uint8_t>& bytes, ColorMode mode) {
    if (bytes.empty() || bytes.size() % 8) return std::nullopt;
    BlendIf b;
    const size_t pairs = std::min(bytes.size() / 8, size_t(1 + colours(mode)));
    auto range = [&](size_t at) { return BlendIfRange{bytes[at], bytes[at + 1], bytes[at + 2], bytes[at + 3]}; };
    for (size_t i = 0; i < pairs; i++) {
        b.channels[i].thisLayer = range(i * 8);
        b.channels[i].underlying = range(i * 8 + 4);
    }
    return b;
}

std::vector<uint8_t> encodeBlendIf(const BlendIf& blendIf, const std::vector<uint8_t>& original, ColorMode mode) {
    const size_t pairs = size_t(1 + colours(mode));
    if (original.empty() && blendIf.identity()) return {};
    std::vector<uint8_t> out = original;
    if (out.size() < pairs * 8 || out.size() % 8) {
        // A fresh block: every channel, then an identity transparency pair (as Photoshop writes it).
        out.assign((pairs + 1) * 8, 0);
        for (size_t i = 0; i < pairs + 1; i++) { out[i * 8 + 2] = out[i * 8 + 3] = out[i * 8 + 6] = out[i * 8 + 7] = 255; }
    }
    auto put = [&](size_t at, const BlendIfRange& r) { out[at] = r.blackLow; out[at + 1] = r.blackHigh; out[at + 2] = r.whiteLow; out[at + 3] = r.whiteHigh; };
    for (size_t i = 0; i < pairs; i++) {
        put(i * 8, blendIf.channels[i].thisLayer);
        put(i * 8 + 4, blendIf.channels[i].underlying);
    }
    return out;
}

std::optional<BlendIf> layerBlendIf(const Layer& layer, ColorMode mode) {
    if (!layer.psdCarry || layer.psdCarry->blendingRanges.empty()) return std::nullopt;
    auto b = parseBlendIf(layer.psdCarry->blendingRanges, mode);
    if (!b || b->identity()) return std::nullopt;
    return b;
}

BlendIf editableBlendIf(const Layer& layer, ColorMode mode) {
    if (layer.psdCarry) if (auto b = parseBlendIf(layer.psdCarry->blendingRanges, mode)) return *b;
    return BlendIf{};
}

bool setLayerBlendIf(Layer& layer, const BlendIf& blendIf, ColorMode mode) {
    BlendIf wanted = blendIf;
    // Channels the mode does not have stay identity.
    for (int i = 1 + colours(mode); i < BlendIf::maxChannels; i++) wanted.channels[size_t(i)] = BlendIfChannel{};
    if (editableBlendIf(layer, mode) == wanted) return false;
    auto carry = layer.psdCarry ? std::make_shared<PsdLayerCarry>(*layer.psdCarry) : std::make_shared<PsdLayerCarry>();
    carry->blendingRanges = encodeBlendIf(wanted, carry->blendingRanges, mode);
    layer.psdCarry = std::move(carry);
    return true;
}

uint8_t allBlendChannels(ColorMode mode) { return uint8_t((1u << colours(mode)) - 1); }

std::optional<uint8_t> parseBlendChannels(const std::vector<uint8_t>& bytes, ColorMode mode) {
    if (bytes.size() % 4) return std::nullopt;
    uint8_t excluded = 0;
    for (size_t at = 0; at < bytes.size(); at += 4) {
        const uint32_t index = uint32_t(bytes[at]) << 24 | uint32_t(bytes[at + 1]) << 16 | uint32_t(bytes[at + 2]) << 8 | bytes[at + 3];
        if (index < uint32_t(colours(mode))) excluded |= uint8_t(1u << index);
    }
    return excluded;
}

std::vector<uint8_t> encodeBlendChannels(uint8_t excluded, ColorMode mode) {
    std::vector<uint8_t> out;
    for (int k = 0; k < colours(mode); k++)
        if (excluded & (1u << k)) out.insert(out.end(), {0, 0, 0, uint8_t(k)});
    return out;
}

uint8_t layerExcludedChannels(const Layer& layer, ColorMode mode) {
    if (!layer.psdCarry) return 0;
    for (const PsdBlock& b : layer.psdCarry->blocks)
        if (b.key == "brst") return parseBlendChannels(b.data, mode).value_or(0);
    return 0;
}

bool setLayerExcludedChannels(Layer& layer, uint8_t excluded, ColorMode mode) {
    excluded &= allBlendChannels(mode);
    if (layerExcludedChannels(layer, mode) == excluded) return false;
    auto carry = layer.psdCarry ? std::make_shared<PsdLayerCarry>(*layer.psdCarry) : std::make_shared<PsdLayerCarry>();
    auto& blocks = carry->blocks;
    blocks.erase(std::remove_if(blocks.begin(), blocks.end(), [](const PsdBlock& b) { return b.key == "brst"; }), blocks.end());
    if (excluded) {
        // Where Photoshop writes it: after the layer's protection flags ('lspf'), else last.
        auto at = std::find_if(blocks.begin(), blocks.end(), [](const PsdBlock& b) { return b.key == "lspf"; });
        blocks.insert(at == blocks.end() ? at : at + 1, PsdBlock{"brst", encodeBlendChannels(excluded, mode)});
    }
    layer.psdCarry = std::move(carry);
    return true;
}

BlendIfGate::BlendIfGate(const BlendIf& b, ColorMode m) : mode(m), count(1 + colours(m)) {
    for (int i = 0; i < count; i++) {
        const auto& c = b.channels[size_t(i)];
        sourceOn[size_t(i)] = !c.thisLayer.identity();
        underOn[size_t(i)] = !c.underlying.identity();
        anySource = anySource || sourceOn[size_t(i)];
        anyUnder = anyUnder || underOn[size_t(i)];
        for (int v = 0; v < 256; v++) {
            source[size_t(i)][size_t(v)] = c.thisLayer.factor(v);
            under[size_t(i)][size_t(v)] = c.underlying.factor(v);
        }
    }
}

int BlendIfGate::gray(const int* c) const {
    auto luma = [](int r, int g, int b) { return (299 * r + 590 * g + 111 * b + 500) / 1000; };
    switch (mode) {
    case ColorMode::Lab: return c[0];
    // CMYK is stored as Photoshop stores it (255 is no ink): the complements of C, M and Y stand for R, G and B,
    // darkened by black.
    case ColorMode::CMYK: return (luma(c[0], c[1], c[2]) * c[3] + 127) / 255;
    case ColorMode::RGB: break;
    }
    return luma(c[0], c[1], c[2]);
}

float BlendIfGate::factor(const std::array<std::array<float, 256>, BlendIf::maxChannels>& t, const std::array<bool, BlendIf::maxChannels>& on,
                          const int* c) const {
    float f = 1;
    if (on[0]) f *= t[0][size_t(std::clamp(gray(c), 0, 255))];
    for (int i = 1; i < count && f > 0; i++) if (on[size_t(i)]) f *= t[size_t(i)][size_t(std::clamp(c[i - 1], 0, 255))];
    return f;
}

} // namespace compositor
