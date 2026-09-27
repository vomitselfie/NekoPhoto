#include "compositor/supports.h"
#include <string>

namespace compositor {

namespace {
constexpr SampleTypes eightAndSixteen = onlyEightBit | sampleTypeBit(SampleType::U16);

// One line per feature ported beyond 8-bit. What is not listed here stays 8-bit only: its menu entry is greyed and its
// automation method refused on a deeper document ("Not available in 16-bit yet").
//
// P2, the 16-bit core: the renderer, the layer structure and the files. Painting, selections, adjustments, filters,
// layer styles, smart objects, text and vector editing follow in P3 and later (docs/bit-depth.md).
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
