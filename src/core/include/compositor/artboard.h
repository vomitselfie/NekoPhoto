// Artboards and slices, as Photoshop models them.
//
// An artboard is a folder with a rectangle and a background: its children are clipped to the rectangle and
// the background fills it under them. In a PSD it is the folder's 'artb' block (also read as 'artd' or
// 'abdd'), a version-16 descriptor of class "artboard": artboardRect (classFloatRect Top/Left/Btom/Rght),
// guideIndeces, artboardPresetName, 'Clr ' (RGBC) and artboardBackgroundType (1 white, 2 black,
// 3 transparent, 4 other). Descriptors are read and written with Patchy's psd_descriptor (MIT).
//
// Slices are named document rectangles for export (Save for Web's slices); in a PSD they are image resource
// 1050, version 6 (binary records) or 7/8 (a descriptor). See docs/artboards-slices.md.
#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace compositor {

struct Artboard {
    int x = 0, y = 0, width = 0, height = 0;     // document pixels
    enum Background { White = 1, Black = 2, Transparent = 3, Other = 4 };
    int background = White;
    double red = 1, green = 1, blue = 1;          // 0..1, used when background is Other
    std::string presetName;                      // Photoshop's preset ("Custom", "iPhone 8/7/6", ...)
    bool operator==(const Artboard&) const = default;
    /// The fill colour (0..255 each, alpha 0 for a transparent background).
    void fill(uint8_t rgba[4]) const;
};

struct Slice {
    uint32_t id = 1;
    std::string name;
    int x = 0, y = 0, width = 0, height = 0;
    std::string url, target, message, altTag;
    bool operator==(const Slice&) const = default;
};

/// The artboard in a folder's 'artb' / 'artd' / 'abdd' block, or none when the bytes are not one.
std::optional<Artboard> parseArtboardBlock(const std::vector<uint8_t>& data);
/// The block Photoshop writes for `artboard` (key 'artb').
std::vector<uint8_t> artboardBlock(const Artboard& artboard);

/// The user and layer slices in resource 1050 (the automatic ones Photoshop makes to fill the canvas are
/// left out: they follow from the others). False when the bytes are not a slices resource.
bool parseSlicesResource(const std::vector<uint8_t>& data, std::vector<Slice>& slices, std::string* groupName = nullptr);
/// Resource 1050, version 6, for a `width` x `height` canvas: the automatic whole-canvas slice, then `slices`.
std::vector<uint8_t> slicesResource(const std::vector<Slice>& slices, int width, int height, const std::string& groupName = "");

/// A slice id not used yet (one above the largest).
uint32_t nextSliceId(const std::vector<Slice>& slices);

} // namespace compositor
