// How a 32-bit document's linear, unbounded values are shown and reduced (docs/bit-depth.md, "32 bits").
//
// View ▸ 32-bit Preview Options and the status bar's exposure slider set a `View32` per view: it changes what the
// canvas shows, never the pixels, and is not an undo step. Image ▸ Mode ▸ 8 or 16 Bits/Channel on a 32-bit document
// goes through HDR Toning with the same settings (Exposure and Gamma, or Highlight Compression), baked into the
// pixels there.
//
// On straight linear colour, per pixel:
// - Exposure and Gamma: colour × 2^exposure, then raised to 1 / gamma; values above 1 clip.
// - Highlight Compression: colour × 2^exposure, then the luminance L (the profile's linear Y) mapped by
//   L (1 + L / W²) / (1 + L) with W the brightest luminance there is (at least 1), colour scaled by the ratio. An
//   image whose luminance never exceeds 1 is left as it is; the brightest pixel lands on 1.
// Then the document's transfer curve (its gamma counterpart's) encodes the result for 8 or 16 bits.
#pragma once
#include <algorithm>
#include <cmath>
#include <optional>
#include <string>

namespace compositor {

enum class ToneMethod { ExposureGamma, HighlightCompression };

/// "exposure-gamma", "highlight-compression" (automation, settings), and back.
inline const char* toneMethodKey(ToneMethod method) { return method == ToneMethod::HighlightCompression ? "highlight-compression" : "exposure-gamma"; }
inline std::optional<ToneMethod> toneMethodFromKey(const std::string& key) {
    if (key == "exposure-gamma") return ToneMethod::ExposureGamma;
    if (key == "highlight-compression") return ToneMethod::HighlightCompression;
    return std::nullopt;
}

struct View32 {
    /// Stops, -20..20 (Photoshop's slider range).
    double exposure = 0;
    /// 0.1..9.99; 1 leaves the values as they are.
    double gamma = 1;
    ToneMethod method = ToneMethod::ExposureGamma;
    static constexpr double minExposure = -20, maxExposure = 20, minGamma = 0.1, maxGamma = 9.99;
    /// Exposure 0, gamma 1, Exposure and Gamma: the pixels' own values (an 8- or 16-bit document converted to 32 bits
    /// and back is then exactly what it was).
    bool isDefault() const { return exposure == 0 && gamma == 1 && method == ToneMethod::ExposureGamma; }
    View32 clamped() const {
        View32 v = *this;
        v.exposure = std::isfinite(exposure) ? std::clamp(exposure, minExposure, maxExposure) : 0.0;
        v.gamma = std::isfinite(gamma) ? std::clamp(gamma, minGamma, maxGamma) : 1.0;
        return v;
    }
    bool operator==(const View32& o) const { return exposure == o.exposure && gamma == o.gamma && method == o.method; }
    bool operator!=(const View32& o) const { return !(*this == o); }
};

/// A View32 ready to apply per pixel: the exposure as a factor, the gamma inverted, the luminance weights and the
/// white point Highlight Compression maps to 1.
struct ToneMap {
    float scale = 1, inverseGamma = 1;
    ToneMethod method = ToneMethod::ExposureGamma;
    float luma[3] = {0.2126f, 0.7152f, 0.0722f};
    float whiteSquared = 1;
    bool identity = true;

    /// `peak` is the brightest luminance of what is shown or converted (Highlight Compression only; before exposure).
    static ToneMap of(const View32& view, const float luma[3], float peak = 1) {
        ToneMap t;
        const View32 v = view.clamped();
        t.scale = float(std::exp2(v.exposure));
        t.inverseGamma = float(1 / v.gamma);
        t.method = v.method;
        for (int k = 0; k < 3; k++) t.luma[k] = luma[k];
        const float white = std::max(1.0f, peak * t.scale);
        t.whiteSquared = white * white;
        t.identity = v.exposure == 0 && (v.method == ToneMethod::HighlightCompression ? white <= 1 : v.gamma == 1);
        return t;
    }

    /// Straight linear colour in place; the result may still exceed 1 (Exposure and Gamma) and is clipped by the
    /// encoding after it.
    void apply(float c[3]) const {
        if (identity) return;
        for (int k = 0; k < 3; k++) c[k] = std::max(0.0f, c[k]) * scale;
        if (method == ToneMethod::ExposureGamma) {
            if (inverseGamma != 1) for (int k = 0; k < 3; k++) c[k] = std::pow(c[k], inverseGamma);
            return;
        }
        const float l = luma[0] * c[0] + luma[1] * c[1] + luma[2] * c[2];
        if (!(l > 0)) return;
        const float mapped = l * (1 + l / whiteSquared) / (1 + l);
        const float k = mapped / l;
        for (int i = 0; i < 3; i++) c[i] *= k;
    }
};

} // namespace compositor
