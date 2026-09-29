// A PSD's alpha and spot channels: the planes after the merged image's colour (and transparency), named by
// resources 1045 (Unicode) and 1006 (Pascal), shown as resource 1077 (or the older 1007) says, identified by 1053.
// Shared by psd.cpp (reading) and psd_writer.cpp (writing); see docs/channels.md.
#pragma once
#include "compositor/document.h"
#include <cstdint>
#include <vector>

namespace compositor {

/// The channel resources as the file stored them (empty when it had none).
struct PsdChannelResources {
    std::vector<uint8_t> names;          // 1006
    std::vector<uint8_t> unicodeNames;   // 1045
    std::vector<uint8_t> displayInfo;    // 1077
    std::vector<uint8_t> oldDisplayInfo; // 1007
    std::vector<uint8_t> identifiers;    // 1053
    bool any() const { return !names.empty() || !unicodeNames.empty() || !displayInfo.empty() || !oldDisplayInfo.empty(); }
};

/// One extra plane of the merged image: at 8 bits, and for a 16-bit document at 0..32768 with the file's own
/// big-endian 0..65535 samples beside it.
struct PsdExtraPlane {
    std::vector<uint8_t> eight;
    std::vector<uint16_t> wide;
    std::vector<uint8_t> raw16;
};

/// The document's channels from the extra planes. `transparencyFirst`: the file's first extra plane is the merged
/// image's transparency (already taken out of `planes`), which some writers name as a channel too.
std::vector<Channel> psdChannels(const PsdChannelResources& resources, std::vector<PsdExtraPlane>& planes, bool transparencyFirst,
                                 int width, int height, bool deep);

/// The resources (1006, 1045, 1077, 1053) that name and describe `document`'s channels, in that order; none
/// without channels.
std::vector<std::pair<uint16_t, std::vector<uint8_t>>> psdChannelResourceBlocks(const Document& document);
/// A channel's plane as the merged image stores it: bytes at 8 bits; at 16 bits big-endian 0..65535, the carried
/// samples while the channel is the one read from them.
std::vector<uint8_t> psdChannelPlane(const Channel& channel, bool deep, int width, int height);

} // namespace compositor
