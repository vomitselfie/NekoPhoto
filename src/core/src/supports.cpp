#include "compositor/supports.h"
#include <string>

namespace compositor {

namespace {
constexpr SampleTypes eightAndSixteen = onlyEightBit | sampleTypeBit(SampleType::U16);
/// P5a, the 32-bit core (docs/bit-depth.md, "32 bits"): what works in a 32-bit document too.
constexpr SampleTypes allDepths = eightAndSixteen | sampleTypeBit(SampleType::F32);

// One line per feature ported beyond 8-bit. What is not listed here stays 8-bit only: its menu entry is greyed and its
// automation method refused on a deeper document ("Not available in 16-bit yet"; at 32 bits "Not available in 32-bit
// yet", or "... in 32-bit mode" for what Photoshop itself lacks there, photoshopLacksAt32).
//
// P2, the 16-bit core: the renderer, the layer structure and the files. P3a: adjustments, filters, selections and
// pixel edits. P3b: painting and retouching. Then text, vectors and layer styles, and smart objects with their
// Smart Filters (docs/bit-depth.md).
constexpr FeatureSupport table[] = {
    {"render.document", allDepths},
    // Image > Mode > 8 Bits/Channel, 16 Bits/Channel.
    {"document.mode", allDepths},
    // Edit > Assign Profile and Convert to Profile (P4, colormgmt.h).
    {"document.profile", eightAndSixteen},
    // Saving, and exporting what the renderer draws (PSD and PNG at 16 bits; the 8-bit formats dithered down).
    {"document.save", allDepths},
    {"export.psd", allDepths},
    {"export.png", allDepths},
    {"export.jpeg", allDepths},
    {"export.webp", allDepths},
    {"export.tiff", allDepths},
    {"export.tga", allDepths},
    {"export.ico", allDepths},
    {"export.gif", allDepths},
    // Layers: adding blank layers and folders, deleting, duplicating, ordering, grouping, renaming, visibility,
    // opacity, blend mode, clipping, resampling mode.
    {"layers.structure", allDepths},
    // Moving, scaling, rotating and flipping whole layers (their transform; no pixels are resampled).
    {"layers.transform", allDepths},
    // Layer masks: add (reveal or hide all), enable, link, invert, delete; drawn at the document's depth.
    {"layers.mask", allDepths},
    // Canvas Size and Flip Canvas (they move layers, not pixels).
    {"canvas.size", allDepths},
    {"canvas.flip", allDepths},
    // Importing an image as a layer (converted to the document's depth).
    {"document.import", allDepths},
    // Tools that do not touch pixels.
    {"tool.move", allDepths},
    {"tool.hand", allDepths},
    {"tool.zoom", allDepths},
    {"tool.slice", eightAndSixteen},
    // The Eyedropper samples the 16-bit composite.
    {"tool.eyedropper", eightAndSixteen},
    {"view", allDepths},

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
    // The Filter menu's built-in filters (pixels.filter).
    {"filter.pixels", eightAndSixteen},
    {"filter.Gaussian Blur", eightAndSixteen},
    {"filter.Motion Blur", eightAndSixteen},
    {"filter.Add Noise", eightAndSixteen},
    {"filter.Lens Correction", eightAndSixteen},
    // Filter > Camera Raw Filter: its kernels on float colour, rounded to 16 bits once (cameraraw.h).
    {"filter.Camera Raw", eightAndSixteen},
    // Filter > G'MIC: 16-bit pixels go to G'MIC as float on its 0..255 scale and come back at 16 bits (app/Gmic.h).
    {"filter.G'MIC", eightAndSixteen},
    // Filter > Remove Background: the model and the refinement's guide see the layer reduced to 8 bits, the matte
    // stays float (AlphaPlane) and is committed as a 16-bit mask, and Clean edge colours works at 16 bits.
    {"edit.removeBackground", eightAndSixteen},
    // Filter > Mosh (compositor/mosh.h, pixels.mosh): straight float colour at either depth.
    {"filter.Mosh", eightAndSixteen},
    // Selections at the document's depth: the marquee, lasso, Magic Wand and Quick Select tools (they read the
    // canvas as shown, in 8-bit levels, and make 16-bit coverage), the Select menu, Load as Selection, Quick Mask.
    {"edit.selection", eightAndSixteen},
    // Channels (docs/channels.md): alpha channels at the document's depth, Save and Load Selection, single-channel editing.
    {"edit.channels", eightAndSixteen},
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

    // Artboards (their backgrounds and clipping drawn by the 16-bit renderer) and the Artboard tool; the timeline's
    // frames and playback (layer states, no pixels); exporting artboards and slices (16-bit PNG, or JPEG dithered
    // down to 8 bits) and SVG (images and folder masks embedded as 16-bit PNGs).
    {"edit.artboard", eightAndSixteen},
    {"tool.artboard", eightAndSixteen},
    {"edit.timeline", eightAndSixteen},
    {"export.artboards", eightAndSixteen},
    {"export.slices", eightAndSixteen},
    {"export.svg", eightAndSixteen},

    // Smart objects: sources at their own depth, placed at the document's; Place Embedded, Convert (a 16-bit
    // child PSB), Edit Contents, Replace, Rasterize, warps, and Smart Filters with their 16-bit kernels
    // (smartfilter_render16.cpp).
    {"edit.smartObject", eightAndSixteen},
};

constexpr ColorModes allModes = colorModeBit(ColorMode::RGB) | colorModeBit(ColorMode::CMYK) | colorModeBit(ColorMode::Lab);

// The colour-mode axis (docs/high-bit-depth-plan.md, "P7 plan"): what works in CMYK and Lab documents, which exist at
// 8 and 16 bits. What is not listed works in RGB only, and is refused in CMYK and Lab with "Not available in CMYK mode".
// Step A lists what never reads colour values: the layer structure, masks, the canvas, navigation and saving projects.
// Rendering, the channels, Image > Mode, PSD and the editing tools join as steps C to F port them. Photoshop's own
// exclusions stay out for good: Camera Raw, G'MIC and the MyPaint brushes are RGB only.
constexpr FeatureModes modeTable[] = {
    {"document.save", allModes},
    {"document.profile", allModes},
    {"layers.structure", allModes},
    {"layers.transform", allModes},
    {"layers.mask", allModes},
    {"canvas.size", allModes},
    {"canvas.flip", allModes},
    {"tool.move", allModes},
    {"tool.hand", allModes},
    {"tool.zoom", allModes},
    {"tool.marquee", allModes},
    {"tool.lasso", allModes},
    {"edit.timeline", allModes},
    {"view", allModes},
};
}   // namespace

const FeatureModes* featureModeTable(size_t& count) {
    count = sizeof(modeTable) / sizeof(modeTable[0]);
    return modeTable;
}

ColorModes supportedColorModes(std::string_view feature) {
    for (const FeatureModes& f : modeTable) if (f.feature == feature) return ColorModes(f.modes | colorModeBit(ColorMode::RGB));
    return colorModeBit(ColorMode::RGB);
}

bool supports(AdjustmentKind kind, SampleType type, ColorMode mode) {
    return supports(std::string("adjustment.") + adjustmentKindName(kind), type, mode);
}

namespace {
// What Photoshop itself greys in a 32-bit document (docs/high-bit-depth-plan.md, "P5 plan"): refused here with "Not
// available in 32-bit mode", for good, where features not ported yet say "yet".
constexpr std::string_view lackedAt32[] = {
    "tool.dodge", "tool.paintBucket", "edit.contentAware",
    "adjustment.Brightness/Contrast", "adjustment.Posterize", "adjustment.Threshold", "adjustment.Selective Color", "adjustment.Grain",
    "filter.Mosh", "filter.G'MIC",
    // The blend modes outside Photoshop's 32-bit set (blendModeAt32): greyed in the picker, refused by automation.
    "blend.outside32",
};
} // namespace

bool photoshopLacksAt32(std::string_view feature) {
    for (std::string_view f : lackedAt32) if (f == feature) return true;
    return false;
}

std::string notAvailableAtDepth(std::string_view feature, SampleType type) {
    if (supports(feature, type)) return {};
    if (type == SampleType::F32) return photoshopLacksAt32(feature) ? "Not available in 32-bit mode" : "Not available in 32-bit yet";
    return std::string("Not available in ") + sampleTypeName(type) + "-bit yet";
}

std::string notAvailableInMode(ColorMode mode) {
    if (mode == ColorMode::RGB) return {};
    return std::string("Not available in ") + colorModeName(mode) + " mode";
}

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
