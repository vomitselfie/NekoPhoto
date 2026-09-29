// Camera RAW files (CR2, CR3, NEF, ARW, RAF, ORF, RW2, DNG, ...) developed through LibRaw (LGPL 2.1 / CDDL 1.0):
// demosaiced with the camera's as-shot white balance into sRGB, turned upright. Optional: builds without LibRaw
// refuse them. Opening one goes through the Camera Raw dialog (the grade of cameraraw.h over the as-shot decode);
// docs/camera-raw.md has the workflow.
#pragma once
#include "cameraraw.h"
#include "image.h"
#include "imaget.h"
#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <array>
#include <string>
#include <vector>

namespace compositor {

/// Whether this build reads RAW files.
bool rawSupported();
/// Whether `path`'s extension is a camera RAW format LibRaw reads.
bool isRawPath(const std::string& path);
/// The developed image at 8 bits (premultiplied RGBA, opaque); null with `error` when the file cannot be read.
std::shared_ptr<Image> decodeRaw(const std::string& path, std::string* error);

struct RawDecodeOptions {
    /// Half the size each way, one output pixel per Bayer quad: the quick decode the Camera Raw dialog previews.
    bool halfSize = false;
    /// Set from another thread to stop the decode; it then fails with "Cancelled.".
    const std::atomic<bool>* cancel = nullptr;
};

/// What the file says about itself, without decoding the pixels.
struct RawInfo {
    std::string make, model;
    int width = 0, height = 0;   // developed and turned upright
    double iso = 0, shutter = 0, aperture = 0, focalLength = 0;
};
bool readRawInfo(const std::vector<uint8_t>& bytes, RawInfo& info, std::string* error);

/// The file's pixels as shot at 16 bits (0..32768, opaque), in sRGB: the camera's white balance, no grade.
std::shared_ptr<Image16> decodeRaw16(const std::vector<uint8_t>& bytes, const RawDecodeOptions& options, std::string* error);

/// Decoded as shot, then graded by `settings` (Camera Raw's panels): what Open in the Camera Raw dialog makes.
/// `seed` fixes the grain. Null with `error` when the file cannot be read or the decode was cancelled.
std::shared_ptr<Image16> developRaw(const std::vector<uint8_t>& bytes, const CameraRawSettings& settings, const RawDecodeOptions& options,
                                    std::string* error, uint32_t seed = 0);

/// White Balance > Auto for a RAW file: the gray-world balance of its quick as-shot decode, as Temperature and Tint
/// relative to the as-shot balance. Empty when the file cannot be read.
std::optional<std::array<double, 2>> rawAutoBalance(const std::vector<uint8_t>& bytes);

/// A file's bytes; empty with `error` when it cannot be read or is larger than `limit`.
std::vector<uint8_t> readRawFileBytes(const std::string& path, std::string* error, size_t limit = size_t(2) << 30);

} // namespace compositor
