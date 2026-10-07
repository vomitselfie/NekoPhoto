#include "compositor/supports.h"
#include "compositor/colormgmt.h"
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
    // The Eyedropper samples the composite at the document's depth (at 32 bits the linear value encoded through the
    // document's curve, in CMYK and Lab the native values through the profile).
    {"tool.eyedropper", allDepths},
    {"view", allDepths},

    // P3a: adjustments, on pixels (Image > Adjustments, pixels.adjust) and as adjustment layers, every kind. P5b:
    // Photoshop's 32-bit set (adjustments_f32.cpp); Brightness/Contrast, Posterize, Threshold, Selective Color and Grain
    // are not in it (photoshopLacksAt32).
    {"adjustment.pixels", allDepths},
    {"adjustment.Levels", allDepths},
    {"adjustment.Curves", allDepths},
    {"adjustment.Hue/Saturation", allDepths},
    {"adjustment.Exposure", allDepths},
    {"adjustment.Gradient Map", allDepths},
    {"adjustment.Grain", eightAndSixteen},
    {"adjustment.Invert", allDepths},
    {"adjustment.Brightness/Contrast", eightAndSixteen},
    {"adjustment.Posterize", eightAndSixteen},
    {"adjustment.Threshold", eightAndSixteen},
    {"adjustment.Black & White", allDepths},
    {"adjustment.Color Balance", allDepths},
    {"adjustment.Vibrance", allDepths},
    {"adjustment.Photo Filter", allDepths},
    {"adjustment.Channel Mixer", allDepths},
    {"adjustment.Selective Color", eightAndSixteen},
    {"adjustment.Color Lookup", allDepths},
    // The Filter menu's built-in filters (pixels.filter); at 32 bits on linear float (filters_f32.cpp).
    {"filter.pixels", allDepths},
    {"filter.Gaussian Blur", allDepths},
    {"filter.Motion Blur", allDepths},
    {"filter.Add Noise", allDepths},
    {"filter.Lens Correction", allDepths},
    // The grid filters (applyGridFilter): the Smart Filter kernels at 8 and 16 bits; the PhotoCraft ports on float
    // straight colour, the geometric ones at every depth.
    {"filter.Box Blur", eightAndSixteen},
    {"filter.Radial Blur", eightAndSixteen},
    {"filter.Surface Blur", eightAndSixteen},
    {"filter.Dust & Scratches", eightAndSixteen},
    {"filter.Median", eightAndSixteen},
    {"filter.Unsharp Mask", eightAndSixteen},
    {"filter.High Pass", eightAndSixteen},
    {"filter.Emboss", eightAndSixteen},
    {"filter.Mosaic", eightAndSixteen},
    {"filter.Clouds", eightAndSixteen},
    {"filter.Difference Clouds", eightAndSixteen},
    {"filter.Find Edges", eightAndSixteen},
    {"filter.Twirl", allDepths},
    {"filter.Pinch", allDepths},
    {"filter.Spherize", allDepths},
    {"filter.Wave", allDepths},
    {"filter.Ripple", allDepths},
    {"filter.Polar Coordinates", allDepths},
    {"filter.ZigZag", allDepths},
    {"filter.Shear", allDepths},
    {"filter.Maximum", allDepths},
    {"filter.Minimum", allDepths},
    {"filter.Offset", allDepths},
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
    // canvas as shown, in 8-bit levels, and make 16-bit coverage; at 32 bits they read decisionImage(), exposure 0,
    // and make float coverage), the Select menu, Load as Selection, Quick Mask.
    {"edit.selection", allDepths},
    // Channels (docs/channels.md): alpha channels at the document's depth, Save and Load Selection, single-channel editing.
    {"edit.channels", allDepths},
    {"tool.marquee", allDepths},
    {"tool.lasso", allDepths},
    {"tool.wand", allDepths},
    {"tool.quickSelect", allDepths},
    // Fill and Clear through the selection; Cut, Copy, Copy Merged, Paste and Layer via Copy.
    {"edit.fill", allDepths},
    {"edit.clipboard", allDepths},
    // Image Size (16-bit resampling), Crop, the Crop tool, Crop to Selection and Trim.
    {"edit.imageSize", allDepths},
    {"edit.crop", allDepths},
    {"tool.crop", allDepths},
    // Distort and Perspective in Free Transform, Edit > Warp and Warp Cage on whole pixel layers.
    {"edit.distort", allDepths},
    // Content-Aware Fill, Move, Extend and Scale: decided on the pixels rounded to 8 bits, the 16-bit pixels copied.
    {"edit.contentAware", eightAndSixteen},

    // P3b: painting and retouching. The brush (round tip, imported tip brushes) and the eraser, on pixels, layer masks
    // and the Quick Mask; brush.stroke. P5c: at 32 bits on linear float (StrokeOps<F32>).
    {"tool.brush", allDepths},
    // The MyPaint presets: at 32 bits through libmypaint's 15 bits (only the samples a dab changes are rewritten).
    {"brush.mypaint", allDepths},
    // Spot Healing and the Healing Brush (the patch search and synthesis decide on the 8-bit rounding; at 32 bits on
    // the area encoded at 15 bits under its brightest value).
    {"tool.spotHealing", allDepths},
    // The Patch tool (pixels.patch): Photoshop has none at 32 bits.
    {"tool.patch", eightAndSixteen},
    {"tool.cloneStamp", allDepths},
    // Blur, Sharpen, Smudge and Liquify.
    {"tool.smudge", allDepths},
    // Dodge, Burn and Sponge (Photoshop has none at 32 bits).
    {"tool.dodge", eightAndSixteen},
    {"tool.gradient", allDepths},
    // The Paint Bucket: what it fills is chosen on the canvas as shown, in 8-bit levels, as the Magic Wand chooses.
    {"tool.paintBucket", eightAndSixteen},
    // Moving, duplicating and nudging selected pixels with the Move tool.
    {"edit.movePixels", allDepths},
    // Merge Down, Merge Layers and Merge Group (rendered at the document's depth), and Layer > Layer Mask > Apply.
    {"layers.merge", allDepths},
    {"layers.applyMask", allDepths},
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
    // Selections are coverage, not colour: the Select menu, Quick Mask, loading a mask or channel, transforming the
    // outline (step E, for moving selected pixels).
    {"edit.selection", allModes},
    // The Magic Wand and Quick Select decide in L*a*b* (decisionImage): a Lab document's own values, a CMYK one's
    // composite through its profile for deciding only; the click-to-select model sees the composite in sRGB.
    {"tool.wand", allModes},
    {"tool.quickSelect", allModes},
    {"edit.timeline", allModes},
    {"view", allModes},
    // Steps C and D (P7): the renderer, Image > Mode, the Channels panel with single-channel fill, PSD (modes 4 and 9).
    {"render.document", allModes},
    {"document.mode", allModes},
    {"edit.channels", allModes},
    {"edit.fill", allModes},
    {"export.psd", allModes},
    // Step E, painting (P7): the brush, eraser and imported tip brushes, the Gradient tool, moving selected pixels,
    // Clone Stamp, the Eyedropper, merging and Apply Layer Mask, in the document's own samples. Retouching: Spot
    // Healing, the Healing Brush, Patch, Blur, Sharpen, Smudge, Liquify, Dodge, Burn, Sponge and the Paint Bucket, on
    // L, a and b in Lab and on the five samples in CMYK (heal.h, toning.h, bucket.h).
    {"tool.brush", allModes},
    {"tool.gradient", allModes},
    {"edit.movePixels", allModes},
    {"tool.cloneStamp", allModes},
    {"tool.eyedropper", allModes},
    {"layers.merge", allModes},
    {"layers.applyMask", allModes},
    {"tool.spotHealing", allModes},
    {"tool.smudge", allModes},
    {"tool.patch", allModes},
    {"tool.dodge", allModes},
    {"tool.paintBucket", allModes},
    // Step E, adjustments and filters (P7): Image > Adjustments and adjustment layers on the document's own samples
    // (modeedit.h), each kind in the modes Photoshop offers it (adjustmentOfferedInMode; what it lacks stays greyed for
    // good). Color Lookup takes an ICC abstract profile or a device link of the document's colour space there (3DLUT
    // files are RGB only, as in Photoshop: such a layer is kept and draws nothing).
    {"adjustment.pixels", allModes},
    {"adjustment.Color Lookup", allModes},
    {"adjustment.Levels", allModes},
    {"adjustment.Curves", allModes},
    {"adjustment.Brightness/Contrast", allModes},
    {"adjustment.Invert", allModes},
    {"adjustment.Posterize", allModes},
    {"adjustment.Threshold", allModes},
    {"adjustment.Gradient Map", allModes},
    {"adjustment.Photo Filter", allModes},
    {"adjustment.Hue/Saturation", colorModeBit(ColorMode::CMYK)},
    {"adjustment.Color Balance", colorModeBit(ColorMode::CMYK)},
    {"adjustment.Selective Color", colorModeBit(ColorMode::CMYK)},
    {"adjustment.Channel Mixer", colorModeBit(ColorMode::CMYK)},
    {"adjustment.Exposure", colorModeBit(ColorMode::Lab)},
    // The Filter menu's built-in filters on every ink, or L, a and b.
    {"filter.pixels", allModes},
    {"filter.Gaussian Blur", allModes},
    {"filter.Motion Blur", allModes},
    {"filter.Add Noise", allModes},
    {"filter.Lens Correction", allModes},
    // The grid filters on every ink, or L, a and b (Find Edges on lightness in Lab); Clouds paints sRGB colours, RGB only.
    {"filter.Box Blur", allModes},
    {"filter.Radial Blur", allModes},
    {"filter.Surface Blur", allModes},
    {"filter.Dust & Scratches", allModes},
    {"filter.Median", allModes},
    {"filter.Unsharp Mask", allModes},
    {"filter.High Pass", allModes},
    {"filter.Emboss", allModes},
    {"filter.Mosaic", allModes},
    {"filter.Twirl", allModes},
    {"filter.Pinch", allModes},
    {"filter.Spherize", allModes},
    {"filter.Wave", allModes},
    {"filter.Ripple", allModes},
    {"filter.Polar Coordinates", allModes},
    {"filter.ZigZag", allModes},
    {"filter.Shear", allModes},
    {"filter.Maximum", allModes},
    {"filter.Minimum", allModes},
    {"filter.Offset", allModes},
    {"filter.Find Edges", allModes},
    // Step E, transforms and moving pixels between documents (P7): Image Size, Crop, Trim and the Crop tool, Distort,
    // Warp and the warp cage, Free Transform of selected pixels, on every sample (CMYK through two 4-sample passes,
    // modetransform.h); Cut, Copy and Paste native within a mode and through the profiles between modes, Import through
    // the profiles; the flat exports drawn through the document's profile to sRGB, as Photoshop's Export As.
    {"edit.imageSize", allModes},
    {"edit.crop", allModes},
    {"tool.crop", allModes},
    {"edit.distort", allModes},
    {"edit.clipboard", allModes},
    {"document.import", allModes},
    {"export.png", allModes},
    {"export.jpeg", allModes},
    {"export.webp", allModes},
    {"export.tiff", allModes},
    {"export.tga", allModes},
    {"export.ico", allModes},
    {"export.gif", allModes},
    // Text, shapes and layer styles (P8): text painted through Qt in RGB at 16 bits, its colours then taken through the
    // document's profile pixel by pixel (the glyph coverage composited in the document's channels); shape layers
    // filled in the document's channels (a CMYK gradient of inks from ink to ink), vector masks, paths, Fill and Stroke
    // Path; every layer effect drawn in the document's channels with its colours through the profile
    // (layerstyle_render.cpp), the Layer Style dialog, copy, paste and presets.
    {"tool.text", allModes},
    {"edit.text", allModes},
    {"tool.shape", allModes},
    {"tool.pen", allModes},
    {"tool.directSelect", allModes},
    {"edit.vector", allModes},
    {"edit.paint", allModes},
    {"edit.style", allModes},
    // Smart objects (P9): the contents placed in the document's layout, their own samples when they are of its mode
    // and profile, else converted through the profiles once (smartObjectSourceImage); warps through every sample, and
    // Smart Filters on the document's samples (Lab's four as they are, CMYK's five in two passes), Plastic Wrap aside as
    // in Photoshop (smartFilterDrawsInMode). Convert to Smart Object makes a child of the document's mode.
    {"edit.smartObject", allModes},
    // Artboards and slices: geometry and a background colour converted as any stored colour; their exports, SVG
    // included, go through the profile to sRGB as Export As does. Deleting a clipping base bakes the clipped layers'
    // coverage into their own samples.
    {"edit.artboard", allModes},
    {"tool.artboard", allModes},
    {"tool.slice", allModes},
    {"export.artboards", allModes},
    {"export.slices", allModes},
    {"export.svg", allModes},
    {"edit.pixels", allModes},
};

// What stays RGB for good (not waiting for a port): refused in CMYK and Lab with "Not available in CMYK mode", where a
// feature not ported yet says "... mode yet". Camera Raw and G'MIC work on RGB, as in Photoshop (Camera Raw Filter
// needs RGB there); the MyPaint engine mixes RGB and has no ink or Lab model, so its presets would only paint through
// RGB and back, which this app does not do.
// Mosh is NekoPhoto's own glitch effect (no Photoshop counterpart) on RGB colour: Photoshop offers no such filter in CMYK
// or Lab, and running it through RGB and back is not done here.
constexpr std::string_view rgbOnly[] = {"filter.Camera Raw", "filter.G'MIC", "brush.mypaint", "filter.Mosh"};
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
    "tool.dodge", "tool.paintBucket", "tool.patch", "edit.contentAware",
    "adjustment.Brightness/Contrast", "adjustment.Posterize", "adjustment.Threshold", "adjustment.Selective Color", "adjustment.Grain",
    "filter.Mosh", "filter.G'MIC",
    // The blend modes outside Photoshop's 32-bit set (blendModeAt32): greyed in the picker, refused by automation.
    "blend.outside32",
    // Image > Mode > CMYK Color and Lab Color: Photoshop has no 32-bit CMYK or Lab.
    "mode.cmykLab",
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

bool photoshopLacksInMode(std::string_view feature, ColorMode mode) {
    if (mode == ColorMode::RGB) return false;
    for (std::string_view f : rgbOnly) if (f == feature) return true;
    // The adjustments Photoshop's Image > Adjustments greys in the mode (Vibrance and Black & White in CMYK and Lab,
    // Exposure in CMYK; Hue/Saturation, Color Balance, Selective Color and Channel Mixer in Lab), and Grain.
    constexpr std::string_view prefix = "adjustment.";
    if (feature.substr(0, prefix.size()) == prefix) {
        AdjustmentKind kind;
        if (parseAdjustmentKind(std::string(feature.substr(prefix.size())), kind)) return !adjustmentOfferedInMode(kind, mode);
    }
    return false;
}

std::string unavailableReason(std::string_view feature, SampleType type, ColorMode mode) {
    if (supports(feature, type, mode)) return {};
    if (!supports(feature, type)) return notAvailableAtDepth(feature, type);
    return photoshopLacksInMode(feature, mode) ? notAvailableInMode(mode) : notAvailableInMode(mode) + " yet";
}

namespace {
// The rows of docs/mode-matrix.md, in order: every feature the tables name, grouped as the menus are.
constexpr FeatureRow rows[] = {
    {"render.document", "Drawing the document (canvas, export render)"},
    {"view", "View, zoom, navigation"},
    {"document.save", "Save (project, PSD)"},
    {"document.mode", "Image > Mode"},
    {"document.profile", "Assign / Convert to Profile"},
    {"document.import", "Import / Place as layer"},
    {"export.psd", "Export PSD"},
    {"export.png", "Export PNG"},
    {"export.jpeg", "Export JPEG"},
    {"export.webp", "Export WebP"},
    {"export.tiff", "Export TIFF"},
    {"export.tga", "Export TGA"},
    {"export.ico", "Export ICO"},
    {"export.gif", "Export GIF"},
    {"export.svg", "Export SVG"},
    {"export.artboards", "Export Artboards"},
    {"export.slices", "Export Slices"},
    {"layers.structure", "Layers: add, delete, order, group, opacity, blend mode"},
    {"layers.transform", "Layers: move, scale, rotate, flip (whole layer)"},
    {"layers.mask", "Layer masks"},
    {"layers.merge", "Merge Down, Merge Layers, Merge Group"},
    {"layers.applyMask", "Layer Mask > Apply"},
    {"canvas.size", "Canvas Size"},
    {"canvas.flip", "Flip Canvas"},
    {"edit.imageSize", "Image Size"},
    {"edit.crop", "Crop, Trim, Crop to Selection"},
    {"tool.crop", "Crop tool"},
    {"edit.distort", "Distort, Warp, Warp Cage"},
    {"tool.move", "Move tool"},
    {"edit.movePixels", "Moving selected pixels (Move tool)"},
    {"tool.hand", "Hand tool"},
    {"tool.zoom", "Zoom tool"},
    {"tool.marquee", "Marquee tools"},
    {"tool.lasso", "Lasso tools"},
    {"tool.wand", "Magic Wand"},
    {"tool.quickSelect", "Quick Selection"},
    {"edit.selection", "Selections: Select menu, Quick Mask, load"},
    {"edit.channels", "Channels panel, alpha channels"},
    {"edit.fill", "Fill, Clear"},
    {"edit.clipboard", "Cut, Copy, Paste"},
    {"tool.brush", "Brush and Eraser (round tip, tip brushes)"},
    {"brush.mypaint", "MyPaint brush presets"},
    {"tool.gradient", "Gradient tool"},
    {"tool.paintBucket", "Paint Bucket"},
    {"tool.cloneStamp", "Clone Stamp"},
    {"tool.spotHealing", "Spot Healing, Healing Brush"},
    {"tool.patch", "Patch tool"},
    {"tool.smudge", "Blur, Sharpen, Smudge, Liquify tools"},
    {"tool.dodge", "Dodge, Burn, Sponge"},
    {"tool.eyedropper", "Eyedropper"},
    {"adjustment.pixels", "Image > Adjustments (menu)"},
    {"adjustment.Levels", "Levels"},
    {"adjustment.Curves", "Curves"},
    {"adjustment.Hue/Saturation", "Hue/Saturation"},
    {"adjustment.Exposure", "Exposure"},
    {"adjustment.Gradient Map", "Gradient Map"},
    {"adjustment.Grain", "Grain"},
    {"adjustment.Invert", "Invert"},
    {"adjustment.Brightness/Contrast", "Brightness/Contrast"},
    {"adjustment.Posterize", "Posterize"},
    {"adjustment.Threshold", "Threshold"},
    {"adjustment.Black & White", "Black & White"},
    {"adjustment.Color Balance", "Color Balance"},
    {"adjustment.Vibrance", "Vibrance"},
    {"adjustment.Photo Filter", "Photo Filter"},
    {"adjustment.Channel Mixer", "Channel Mixer"},
    {"adjustment.Selective Color", "Selective Color"},
    {"adjustment.Color Lookup", "Color Lookup"},
    {"filter.pixels", "Filter menu"},
    {"filter.Gaussian Blur", "Gaussian Blur"},
    {"filter.Motion Blur", "Motion Blur"},
    {"filter.Add Noise", "Add Noise"},
    {"filter.Lens Correction", "Lens Correction"},
    {"filter.Box Blur", "Box Blur"},
    {"filter.Radial Blur", "Radial Blur"},
    {"filter.Surface Blur", "Surface Blur"},
    {"filter.Dust & Scratches", "Dust & Scratches"},
    {"filter.Median", "Median"},
    {"filter.Unsharp Mask", "Unsharp Mask"},
    {"filter.High Pass", "High Pass"},
    {"filter.Emboss", "Emboss"},
    {"filter.Mosaic", "Mosaic"},
    {"filter.Twirl", "Twirl"},
    {"filter.Pinch", "Pinch"},
    {"filter.Spherize", "Spherize"},
    {"filter.Wave", "Wave"},
    {"filter.Ripple", "Ripple"},
    {"filter.Polar Coordinates", "Polar Coordinates"},
    {"filter.ZigZag", "ZigZag"},
    {"filter.Shear", "Shear"},
    {"filter.Maximum", "Maximum"},
    {"filter.Minimum", "Minimum"},
    {"filter.Offset", "Offset"},
    {"filter.Clouds", "Clouds"},
    {"filter.Difference Clouds", "Difference Clouds"},
    {"filter.Find Edges", "Find Edges"},
    {"filter.Camera Raw", "Camera Raw Filter"},
    {"filter.G'MIC", "G'MIC"},
    {"filter.Mosh", "Mosh"},
    {"edit.removeBackground", "Remove Background"},
    {"edit.contentAware", "Content-Aware Fill / Move / Scale"},
    {"tool.text", "Type tool"},
    {"edit.text", "Text editing"},
    {"tool.shape", "Shape tool"},
    {"tool.pen", "Pen tool"},
    {"tool.directSelect", "Direct Selection tool"},
    {"edit.vector", "Paths, vector masks"},
    {"edit.paint", "Shape and path fills and strokes (text/shape colour)"},
    {"edit.style", "Layer styles"},
    {"edit.smartObject", "Smart objects, Smart Filters"},
    {"edit.artboard", "Artboards"},
    {"tool.artboard", "Artboard tool"},
    {"tool.slice", "Slice tool"},
    {"edit.timeline", "Timeline"},
    {"edit.pixels", "Delete clipping base (baked pixels)"},
};
} // namespace

std::string_view throughRgbNote(std::string_view feature, SampleType type, ColorMode mode) {
    if (mode == ColorMode::RGB || !supports(feature, type, mode)) return {};
    // The renderer draws fill layers (solid, gradient, pattern) and vector shapes' paint in sRGB and converts them
    // (render_modes.cpp); merging renders the same way.
    if (feature == "render.document" || feature == "layers.merge") return "fill layers and shape paint are drawn in sRGB, then converted";
    // Text: Qt paints the glyphs in sRGB at 16 bits; each colour then goes through the profile, the coverage kept.
    if (feature == "tool.text" || feature == "edit.text") return "glyphs painted in sRGB, each colour then taken through the profile";
    // Shape fills: inks as they are; RGB colours, RGB gradients and patterns drawn in sRGB and converted.
    if (feature == "edit.paint" || feature == "tool.shape") return "RGB gradient and pattern fills drawn in sRGB, then converted";
    // The flat formats hold RGB: the composite is drawn through the document's profile to sRGB (Photoshop's Export As).
    if (feature.substr(0, 7) == "export." && feature != "export.psd") return "written as sRGB, converted through the document's profile";
    // Smart objects: contents of the document's mode and profile placed as they are; RGB contents (a PNG, an RGB PSD,
    // a camera RAW file) converted through the profiles once.
    if (feature == "edit.smartObject") return "RGB contents converted through the profiles once, as Photoshop places them";
    return {};
}

const FeatureRow* featureRows(size_t& count) {
    count = sizeof(rows) / sizeof(rows[0]);
    return rows;
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
