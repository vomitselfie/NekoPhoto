// Photoshop PSD and PSB files as documents: layers with their names, positions, opacity, blend modes,
// visibility, groups, clipping and masks; Levels, Curves, Hue/Saturation, Exposure and Gradient Map
// adjustment layers as ours; solid colour fills as pixels; text and smart objects as the pixels
// Photoshop rendered; 16- and 32-bit files reduced to 8 bits; and the merged image Photoshop saved,
// which stands in when a file has no layers. Whatever cannot be carried over is listed in the notes.
#pragma once
#include "document.h"
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace compositor {

/// A Photoshop type layer opened as NekoPhoto text. Its pixels stay Photoshop's until the text is edited;
/// the font and the line spacing need a font engine, so the app finishes them (the core has none).
struct PsdImportedText {
    Uuid layer;
    std::string postScriptName;        // the face, e.g. "Georgia-BoldItalic"
    std::vector<std::string> runPostScriptNames;   // each run's face, when the text has runs
    double leading = 0;                // baseline to baseline in pixels; 0: automatic
    double autoLeading = 1.2;          // with automatic leading, the fraction of the size
};

struct PsdImport {
    Document document;
    /// Photoshop's own flattened image, for checking the import against.
    ImagePtr composite;
    /// What the import left behind, one line each (effects, unknown adjustments, rasterised text, ...).
    std::vector<std::string> notes;
    /// Type layers opened as text (see PsdImportedText).
    std::vector<PsdImportedText> texts;
    /// Whether the merged image is Photoshop's own ("Maximize Compatibility"); without it the file stores a
    /// blank stand-in.
    bool realComposite = true;
};

struct PsdImportOptions {
    /// How deep inside smart objects this file is (their embedded PSDs are read through here too); sources
    /// past the limit stay unread (preview-locked).
    int depth = 0;
    /// Decodes an embedded file NekoPhoto's core cannot (JPEG, TIFF, ...) to an image; the app supplies it.
    std::function<ImagePtr(const std::vector<uint8_t>& bytes, const std::string& fileType, const std::string& fileName)> decodeImage;
};

/// Smart objects nest at most this deep (a guard against cycles and runaway files).
constexpr int psdSmartObjectDepthLimit = 4;

/// What a 'TySh' block says, when it is text NekoPhoto can hold: one style, one alignment, horizontal point
/// text without a warp, unrotated. `scale` is the block's uniform scale (the size is already multiplied).
struct PsdTypeLayer {
    LayerText text;
    std::string postScriptName;
    std::vector<std::string> runPostScriptNames;
    double leading = 0, autoLeading = 1.2;
    double anchorX = 0, anchorY = 0;   // the first baseline's anchor in the document (by alignment)
    double rotation = 0;               // degrees clockwise the text is turned
};
std::optional<PsdTypeLayer> readPhotoshopType(const uint8_t* data, size_t size, std::string* why = nullptr);

/// Reads `path`; null with `error` set when the file is not a PSD/PSB or is damaged.
std::optional<PsdImport> importPsd(const std::string& path, std::string* error, const PsdImportOptions& options = {});
/// The same from the file's bytes.
std::optional<PsdImport> importPsdBytes(const std::vector<uint8_t>& file, std::string* error, const PsdImportOptions& options = {});

} // namespace compositor
