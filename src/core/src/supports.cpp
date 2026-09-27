#include "compositor/supports.h"
#include <string>

namespace compositor {

namespace {
constexpr SampleTypes eightAndSixteen = onlyEightBit | sampleTypeBit(SampleType::U16);

// One line per feature ported beyond 8-bit. What is not listed here stays 8-bit only: its menu entry is greyed and its
// automation method refused on a deeper document ("Not available in 16-bit yet").
//
// P2, the 16-bit core: the renderer, the layer structure and the files. P3a: adjustments, filters, selections and
// pixel edits. Painting (P3b), layer styles, smart objects, text and vector editing follow (docs/bit-depth.md).
constexpr FeatureSupport table[] = {
    {"render.document", eightAndSixteen},
    // Image > Mode > 8 Bits/Channel, 16 Bits/Channel.
    {"document.mode", eightAndSixteen},
    // Saving, and exporting what the renderer draws (PSD and PNG at 16 bits; the 8-bit formats dithered down).
    {"document.save", eightAndSixteen},
    {"export.psd", eightAndSixteen},
    {"export.png", eightAndSixteen},
    {"export.jpeg", eightAndSixteen},
    {"export.webp", eightAndSixteen},
    {"export.tiff", eightAndSixteen},
    {"export.tga", eightAndSixteen},
    {"export.ico", eightAndSixteen},
    {"export.gif", eightAndSixteen},
    // Layers: adding blank layers and folders, deleting, duplicating, ordering, grouping, renaming, visibility,
    // opacity, blend mode, clipping, resampling mode.
    {"layers.structure", eightAndSixteen},
    // Moving, scaling, rotating and flipping whole layers (their transform; no pixels are resampled).
    {"layers.transform", eightAndSixteen},
    // Layer masks: add (reveal or hide all), enable, link, invert, delete; drawn at the document's depth.
    {"layers.mask", eightAndSixteen},
    // Canvas Size and Flip Canvas (they move layers, not pixels).
    {"canvas.size", eightAndSixteen},
    {"canvas.flip", eightAndSixteen},
    // Importing an image as a layer (converted to the document's depth).
    {"document.import", eightAndSixteen},
    // Tools that do not touch pixels.
    {"tool.move", eightAndSixteen},
    {"tool.hand", eightAndSixteen},
    {"tool.zoom", eightAndSixteen},
    {"tool.slice", eightAndSixteen},
    {"view", eightAndSixteen},

    // P3a: adjustments, on pixels (Image > Adjustments, pixels.adjust) and as adjustment layers, every kind.
    {"adjustment.pixels", eightAndSixteen},
    {"adjustment.Levels", eightAndSixteen},
    {"adjustment.Curves", eightAndSixteen},
    {"adjustment.Hue/Saturation", eightAndSixteen},
    {"adjustment.Exposure", eightAndSixteen},
    {"adjustment.Gradient Map", eightAndSixteen},
    {"adjustment.Grain", eightAndSixteen},
    {"adjustment.Invert", eightAndSixteen},
    {"adjustment.Brightness/Contrast", eightAndSixteen},
    {"adjustment.Posterize", eightAndSixteen},
    {"adjustment.Threshold", eightAndSixteen},
    {"adjustment.Black & White", eightAndSixteen},
    {"adjustment.Color Balance", eightAndSixteen},
    {"adjustment.Vibrance", eightAndSixteen},
    {"adjustment.Photo Filter", eightAndSixteen},
    {"adjustment.Channel Mixer", eightAndSixteen},
    {"adjustment.Selective Color", eightAndSixteen},
    {"adjustment.Color Lookup", eightAndSixteen},
    // The Filter menu's built-in filters (pixels.filter); Camera Raw and G'MIC stay 8-bit.
    {"filter.pixels", eightAndSixteen},
    {"filter.Gaussian Blur", eightAndSixteen},
    {"filter.Motion Blur", eightAndSixteen},
    {"filter.Add Noise", eightAndSixteen},
    {"filter.Lens Correction", eightAndSixteen},
    // Selections at the document's depth: the marquee, lasso, Magic Wand and Quick Select tools (they read the
    // canvas as shown, in 8-bit levels, and make 16-bit coverage), the Select menu, Load as Selection, Quick Mask.
    {"edit.selection", eightAndSixteen},
    {"tool.marquee", eightAndSixteen},
    {"tool.lasso", eightAndSixteen},
    {"tool.wand", eightAndSixteen},
    {"tool.quickSelect", eightAndSixteen},
    // Fill and Clear through the selection; Cut, Copy, Copy Merged, Paste and Layer via Copy.
    {"edit.fill", eightAndSixteen},
    {"edit.clipboard", eightAndSixteen},
    // Image Size (16-bit resampling), Crop, the Crop tool, Crop to Selection and Trim.
    {"edit.imageSize", eightAndSixteen},
    {"edit.crop", eightAndSixteen},
    {"tool.crop", eightAndSixteen},
    // Distort and Perspective in Free Transform, Edit > Warp and Warp Cage on whole pixel layers.
    {"edit.distort", eightAndSixteen},
    // Content-Aware Fill, Move, Extend and Scale: decided on the pixels rounded to 8 bits, the 16-bit pixels copied.
    {"edit.contentAware", eightAndSixteen},
};
}   // namespace

const FeatureSupport* featureSupportTable(size_t& count) {
    count = sizeof(table) / sizeof(table[0]);
    return table;
}

SampleTypes supportedSampleTypes(std::string_view feature) {
    for (const FeatureSupport& f : table) if (f.feature == feature) return SampleTypes(f.types | onlyEightBit);
    return onlyEightBit;
}

bool supports(AdjustmentKind kind, SampleType type) {
    return supports(std::string("adjustment.") + adjustmentKindName(kind), type);
}

} // namespace compositor
