// Camera RAW files (CR2, CR3, NEF, ARW, RAF, ORF, RW2, DNG, ...) developed through LibRaw (LGPL 2.1 / CDDL 1.0):
// demosaiced with the camera's as-shot white balance, or the multipliers of a Temperature and Tint (whitebalance.h), into
// sRGB, turned upright. Optional: builds without LibRaw refuse them. Opening one goes through the Camera Raw dialog (the
// grade of cameraraw.h over the decode);
// docs/camera-raw.md has the workflow.
#pragma once
#include "cameraraw.h"
#include "image.h"
#include "whitebalance.h"
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
    /// White balance multipliers (red, green, blue) to develop with instead of the camera's as-shot balance.
    std::optional<std::array<double, 3>> multipliers;
};

/// A RAW file's white balance: the camera's exact as-shot multipliers, the presets the file records, and the colour
/// model that turns multipliers into Temperature and Tint (compositor/whitebalance.h). Multipliers are red, green,
/// blue with green 1.
struct RawWhiteBalance {
    struct Preset {
        CameraRawWhiteBalance mode = CameraRawWhiteBalance::Daylight;
        std::array<double, 3> multipliers{1, 1, 1};
        TemperatureTint value;
    };
    /// Whether the camera's matrix is known, so the balance reads as Temperature and Tint. Without it (an unknown or
    /// four-colour camera) White Balance stays relative to the as-shot balance.
    bool kelvin = false;
    std::array<double, 3> asShot{1, 1, 1};
    TemperatureTint asShotValue;
    /// Daylight, Cloudy, Shade, Tungsten, Fluorescent and Flash, the ones the file records (LibRaw's WB_Coeffs).
    std::vector<Preset> presets;
    CameraWhiteModel model;
    /// LibRaw's camera (white-balanced) -> linear sRGB matrix.
    Matrix3 cameraToSrgb{};

    const Preset* preset(CameraRawWhiteBalance mode) const;
    /// Multipliers for a white point of `value` (clamped to Camera Raw's ranges); empty without a colour model.
    std::optional<std::array<double, 3>> multipliersFor(TemperatureTint value) const;
    /// The white point `multipliers` balance for, as Temperature and Tint (unclamped).
    std::optional<TemperatureTint> valueOf(const std::array<double, 3>& multipliers) const;
    /// The multipliers `settings` develops with; empty for the camera's own (As Shot, or no kelvin balance set).
    std::optional<std::array<double, 3>> multipliersFor(const CameraRawSettings& settings) const;
    /// Settings saved before kelvin white balance (relative Temperature and Tint over the as-shot decode) in kelvin, for
    /// the Camera Raw dialog: relative 0, 0 is As Shot exactly; other values become the white point their gains bring to
    /// grey. Unchanged without a colour model or when the settings already hold a kelvin balance.
    CameraRawSettings inKelvin(const CameraRawSettings& settings) const;
};
bool readRawWhiteBalance(const std::vector<uint8_t>& bytes, RawWhiteBalance& out, std::string* error);

/// White Balance > Auto in kelvin: the gray-world white point of `decoded`, a decode made with `multipliers`.
std::optional<TemperatureTint> rawAutoWhiteBalance(const Image16& decoded, const std::array<double, 3>& multipliers, const RawWhiteBalance& balance);
/// A decode made with multipliers `from` as if made with `to` (a 3x3 in linear light): the dialog's quick preview
/// while the exact decode follows. Clipped highlights and LibRaw's automatic brightness can differ slightly.
void rebalanceRawDecode(Image16& image, const RawWhiteBalance& balance, const std::array<double, 3>& from, const std::array<double, 3>& to);
/// Automation's white balance for a RAW file, settled against the file: a `temperature` above 100 is kelvin (with
/// `tint` then absolute), As Shot and the presets fill in the file's values, Auto without numbers solves them, and a
/// settings object that says nothing about white balance opens As Shot. A temperature within -100..100 is the older
/// relative form and stays relative. Empty on success, else why the settings do not fit this file.
std::string resolveRawWhiteBalance(const std::vector<uint8_t>& bytes, CameraRawSettings& settings, bool temperatureGiven, bool tintGiven);

/// What the file says about itself, without decoding the pixels.
struct RawInfo {
    std::string make, model;
    int width = 0, height = 0;   // developed and turned upright
    double iso = 0, shutter = 0, aperture = 0, focalLength = 0;
};
bool readRawInfo(const std::vector<uint8_t>& bytes, RawInfo& info, std::string* error);

/// The file's pixels at 16 bits (0..32768, opaque), in sRGB: the camera's white balance (or options.multipliers), no grade.
std::shared_ptr<Image16> decodeRaw16(const std::vector<uint8_t>& bytes, const RawDecodeOptions& options, std::string* error);

/// Decoded as shot, then graded by `settings` (Camera Raw's panels): what Open in the Camera Raw dialog makes.
/// `seed` fixes the grain. Null with `error` when the file cannot be read or the decode was cancelled.
std::shared_ptr<Image16> developRaw(const std::vector<uint8_t>& bytes, const CameraRawSettings& settings, const RawDecodeOptions& options,
                                    std::string* error, uint32_t seed = 0);

/// White Balance > Auto for a RAW file without a kelvin balance: the gray-world balance of its quick as-shot decode, as
/// relative Temperature and Tint. Empty when the file cannot be read.
std::optional<std::array<double, 2>> rawAutoBalance(const std::vector<uint8_t>& bytes);

/// A file's bytes; empty with `error` when it cannot be read or is larger than `limit`.
std::vector<uint8_t> readRawFileBytes(const std::string& path, std::string* error, size_t limit = size_t(2) << 30);

} // namespace compositor
