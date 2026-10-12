// Camera Raw and Lightroom develop settings as XMP (the crs: attributes of a preset or a sidecar) onto CameraRawSettings:
// Photoshop's Camera Raw loads .xmp presets, and so does NekoPhoto's Camera Raw dialog. A preset is a sparse patch: the
// settings it names replace the current ones and the rest stay. Settings this engine has no control for are listed, not
// applied. docs/camera-raw.md#xmp-presets has the mapping.
#pragma once
#include "cameraraw.h"
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace nekophoto {

struct CameraRawXmp {
    CameraRawSettings settings;
    /// The preset's name (crs:Name), when it has one.
    std::string name;
    /// The develop settings taken, by attribute name.
    std::vector<std::string> applied;
    /// The develop settings left out because NekoPhoto has no control for them (or not at that value), as "Name=value".
    std::vector<std::string> unsupported;
};

/// How a preset's white balance is read: on a RAW file (`raw`) crs:Temperature and Tint are kelvin and the white point's
/// tint, as in Camera Raw; on pixels (the filter) only the relative crs:IncrementalTemperature and IncrementalTint apply.
/// `attributes` are the crs: settings without their prefix (text values as XMP writes them, "+0.50", "True");
/// `sequences` the ones written as lists (the point curves, "x, y" per point on 0..255).
CameraRawXmp cameraRawFromXmpAttributes(const std::map<std::string, std::string>& attributes,
                                        const std::map<std::string, std::vector<std::string>>& sequences, const CameraRawSettings& base,
                                        bool raw);

/// Reads an XMP file (a Lightroom or Camera Raw preset, or a sidecar) over `base`; empty with `error` when it is not XML
/// or holds no crs: settings.
std::optional<CameraRawXmp> readCameraRawXmp(const std::vector<uint8_t>& bytes, const CameraRawSettings& base, bool raw, std::string* error);

} // namespace nekophoto
