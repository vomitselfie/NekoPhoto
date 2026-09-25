// Writing documents as Photoshop PSD files: pixel layers with their names (Unicode too), positions,
// visibility, opacity, blend modes, folders (nested), layer and folder masks and clipping; Levels, Curves,
// Exposure and Photoshop-style Hue/Saturation as Photoshop adjustment layers; and the merged image from our
// own renderer. What PSD cannot carry is written the way it looks and reported: transformed layers
// resampled into place, text and shapes as pixels, adjustments Photoshop has no equivalent for baked into a
// pixel layer. psd.h reads the files back; the round trip is tested (tests/psd_writer_tests.cpp).
#pragma once
#include "document.h"
#include <string>
#include <vector>

namespace compositor {

struct PsdExportOptions {
    /// PackBits (RLE) channels, as Photoshop writes; raw when a channel would not shrink.
    bool compress = true;
};

struct PsdExportSummary {
    int layers = 0, folders = 0, masks = 0, clipped = 0, adjustments = 0;
    /// What will not look or behave the same in Photoshop, one line each.
    std::vector<std::string> warnings;
    /// What is written differently but looks the same (resampled layers, text as pixels), one line each.
    std::vector<std::string> notes;
};

/// PSD's size limit per side; larger documents need PSB, which is not written yet.
constexpr int psdMaxSide = 30000;

/// What exporting `document` would write and report, without encoding any pixels (for a dialog).
PsdExportSummary planPsdExport(const Document& document);

/// The file's bytes; empty with `error` set when the document cannot be written as PSD.
std::vector<uint8_t> encodePsd(const Document& document, const PsdExportOptions& options, PsdExportSummary* summary, std::string* error);

/// Writes `path` (through a temporary file, so a failure leaves any old file alone).
bool exportPsd(const Document& document, const std::string& path, const PsdExportOptions& options, PsdExportSummary* summary, std::string* error);

} // namespace compositor
