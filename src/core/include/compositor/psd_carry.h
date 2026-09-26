// What a PSD held that NekoPhoto does not model, kept so exporting the document as PSD again gives it back:
// a layer's styles, editable text, smart object, vector mask, Blend If ranges and other tagged blocks, and
// the file's image resources (colour profile, guides, paths, metadata) and global blocks (linked smart
// object data, patterns). Everything is kept as the file's bytes and written back unchanged.
//
// A block that describes the layer's content (text, a smart object, a fill) is only written back while the
// layer's pixels and placement are still the ones imported; a vector mask, while the layer has not moved;
// the mask section as it was, while the mask is unchanged too. Otherwise the block would contradict what
// the layer now is, so it is left out and the pixels stand. Styles, Blend If and the like are written back
// whatever happens. See docs/psd-roundtrip.md.
#pragma once
#include "image.h"
#include "transform.h"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace compositor {

struct PsdBlock {
    std::string key;                 // four characters
    std::vector<uint8_t> data;
    bool operator==(const PsdBlock& o) const { return key == o.key && data == o.data; }
};

struct PsdLayerCarry {
    /// The record's tagged blocks NekoPhoto does not model, in file order.
    std::vector<PsdBlock> blocks;
    /// The record's blending ranges (Blend If), as stored; empty when the file had none.
    std::vector<uint8_t> blendingRanges;
    /// Opacity and Fill as Photoshop had them (NekoPhoto folds them into one opacity).
    uint8_t opacity = 255, fill = 255;
    /// The blend mode's key as stored, and the mode NekoPhoto read it as: while the layer keeps that mode,
    /// the key is written back (Linear Burn stays Linear Burn, a Normal folder stays isolated).
    std::string blendKey;
    int blendAs = -1;
    /// The record's flags (locks, "pixel data irrelevant") and, for a folder, whether it was closed.
    uint8_t flags = 0;
    bool closedFolder = false;
    /// The layer id Photoshop gave the layer ('lyid'), 0 for none.
    uint32_t layerId = 0;
    /// What the bound blocks describe: the pixels (psdContentHash) and their placement.
    uint64_t contentHash = 0;
    LayerTransform placement;
    /// The record's mask section and its mask channels (-2, -3) as stored, from an 8-bit PSD; with the
    /// fingerprint of the mask NekoPhoto made from them (psdMaskHash).
    std::vector<uint8_t> maskData;
    std::vector<std::pair<int, std::vector<uint8_t>>> maskChannels;
    uint64_t maskHash = 0;
    /// A folder's end-marker record: its blocks and blending ranges (colour label, locks and the like).
    std::vector<PsdBlock> endBlocks;
    std::vector<uint8_t> endRanges;
    /// An adjustment layer's settings as read (their JSON): while they are still these, the file's own adjustment
    /// block is written back instead of one made anew. Empty for anything else.
    std::string adjustmentJson;

    enum class Binding { Always, Placement, Content };
    /// What `key` describes: nothing about the pixels (Always), where they are (a vector mask: Placement),
    /// or what they are (text, a smart object, a fill: Content).
    static Binding binding(const std::string& key);
};

struct PsdDocumentCarry {
    struct Resource {
        uint16_t id = 0;
        std::string name;            // the resource's Pascal name, usually empty
        std::vector<uint8_t> data;
    };
    std::vector<Resource> resources;
    /// Global tagged blocks after the layer information, in file order.
    std::vector<PsdBlock> globals;
    /// The canvas the resources describe (guides and slices hold only while it is unchanged).
    int width = 0, height = 0;
};

/// A fingerprint of a layer's pixels (null: no pixels), stable across saving and reopening a project.
uint64_t psdContentHash(const Image* image);
/// The same for a layer mask (null: none), its pixels and whether it is on.
uint64_t psdMaskHash(const GrayImage* mask, bool enabled);

/// For the project package: the carry as one file's bytes, and back (null when the bytes are not one).
std::vector<uint8_t> serializePsdCarry(const PsdLayerCarry& carry);
std::shared_ptr<const PsdLayerCarry> parsePsdLayerCarry(const std::vector<uint8_t>& bytes);
std::vector<uint8_t> serializePsdCarry(const PsdDocumentCarry& carry);
std::shared_ptr<const PsdDocumentCarry> parsePsdDocumentCarry(const std::vector<uint8_t>& bytes);

} // namespace compositor
