#include "compositor/supports.h"
#include <string>

namespace compositor {

namespace {
constexpr SampleTypes eightAndSixteen = onlyEightBit | sampleTypeBit(SampleType::U16);

// One line per feature ported beyond 8-bit. What is not listed here stays 8-bit only: its menu entry is greyed and its
// automation method refused on a deeper document ("Not available in 16-bit yet").
//
// P2, the 16-bit core: the renderer, the layer structure and the files. P3a: adjustments, filters, selections and
// pixel edits. P3b: painting and retouching. Then text, vectors and layer styles; smart objects follow
// (docs/bit-depth.md).
constexpr FeatureSupport table[] = {
    {"render.document", eightAndSixteen},
    // Image > Mode > 8 Bits/Channel, 16 Bits/Channel.
    {"document.mode", eightAndSixteen},
    // Edit > Assign Profile and Convert to Profile (P4, colormgmt.h).
    {"document.profile", eightAndSixteen},
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
    // Filter > Mosh (compositor/mosh.h, pixels.mosh): straight float colour at either depth.
    {"filter.Mosh", eightAndSixteen},
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

    // P3b: painting and retouching. The brush in every engine (round tip, tip brushes, MyPaint) and the eraser, on
    // pixels, layer masks and the Quick Mask; brush.stroke.
    {"tool.brush", eightAndSixteen},
    // Spot Healing, the Healing Brush and Patch (the patch search and synthesis decide on the 8-bit rounding).
    {"tool.spotHealing", eightAndSixteen},
    {"tool.cloneStamp", eightAndSixteen},
    // Blur, Sharpen, Smudge and Liquify.
    {"tool.smudge", eightAndSixteen},
    // Dodge, Burn and Sponge.
    {"tool.dodge", eightAndSixteen},
    {"tool.gradient", eightAndSixteen},
    // The Paint Bucket: what it fills is chosen on the canvas as shown, in 8-bit levels, as the Magic Wand chooses.
    {"tool.paintBucket", eightAndSixteen},
    // Moving, duplicating and nudging selected pixels with the Move tool.
    {"edit.movePixels", eightAndSixteen},
    // Merge Down, Merge Layers and Merge Group (rendered at 16 bits), and Layer > Layer Mask > Apply.
    {"layers.merge", eightAndSixteen},
    {"layers.applyMask", eightAndSixteen},
    // Deleting a clipping base, its clipped layers keeping their look (baked at 16 bits).
    {"edit.pixels", eightAndSixteen},

    // Text at 16 bits: the Type tool, rich text, Edit Text, the text painted at 16 bits per channel (Qt's 16-bit raster,
    // its glyph coverage 8-bit), Create Work Path.
    {"tool.text", eightAndSixteen},
    {"edit.text", eightAndSixteen},
    // Shapes and paths: the Shape, Pen and Direct Selection tools, live shapes, shape fills (solid, gradient from the
    // ramp's exact colours, pattern) and strokes, vector masks on any layer, path operations, Fill Path and Stroke
    // Path, Convert to Shape; vector coverage rasterised at 15 bits.
    {"tool.shape", eightAndSixteen},
    {"tool.pen", eightAndSixteen},
    {"tool.directSelect", eightAndSixteen},
    {"edit.vector", eightAndSixteen},
    {"edit.paint", eightAndSixteen},
    // Layer styles: all ten effects and folder styles drawn at 16 bits (layerstyle_render.cpp), the Layer Style
    // dialog, copy, paste, clear and style presets.
    {"edit.style", eightAndSixteen},
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
