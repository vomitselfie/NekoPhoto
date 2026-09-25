// Colour conversion through ICC profiles (vendored Little CMS, src/third_party/lcms2): a CMYK file's pixels to
// sRGB through its own embedded profile, the way Photoshop converts them (relative colorimetric, black point
// compensation, no dither; the settings Patchy calibrated against Photoshop's conversion).
#pragma once
#include <cstdint>
#include <memory>
#include <vector>

namespace compositor {

class CmykToSrgb {
public:
    /// None when `profile` is not a usable CMYK ICC profile.
    static std::shared_ptr<const CmykToSrgb> fromProfile(const std::vector<uint8_t>& profile);
    ~CmykToSrgb();
    /// `count` pixels of CMYK as a PSD stores it (four interleaved bytes, 255 = no ink) to RGB triples.
    void convert(const uint8_t* cmyk, uint8_t* rgb, size_t count) const;

private:
    CmykToSrgb() = default;
    void* context_ = nullptr;
    void* transform_ = nullptr;
};

} // namespace compositor
