// Importing brushes from other applications into tip brushes (tipbrush.h): Photoshop .abr, Procreate
// .brushset and .brush, Clip Studio .sut, and plain images used as tips. Each reader is written from the
// format's public documentation and its observable structure; what a format holds that tip brushes cannot
// express is listed in the result's notes rather than dropped silently.
#pragma once
#include "tipbrush.h"
#include <optional>
#include <string>
#include <vector>

namespace compositor {

struct BrushImport {
    std::string set;                 // the collection's name, from the file
    std::vector<TipPreset> brushes;
    std::vector<std::string> notes;  // what was approximated or left out
};

/// A tip from any image: its alpha when the image has transparency, otherwise its darkness (black paints,
/// as in Photoshop), cropped to the part that paints. Null when nothing in it would paint.
std::shared_ptr<GrayImage> tipFromImage(const Image& image);
/// A plain image as a brush: its tip at 25% spacing, sized to the image (at most 300 pixels).
std::optional<TipPreset> presetFromImage(const Image& image, const std::string& name);

/// Reads a brush file of any supported kind, recognised by its content. Nullopt with `error` when the file
/// is not a brush file this reader knows, or holds no brush it can use.
std::optional<BrushImport> importBrushFile(const std::string& path, std::string* error = nullptr);

/// Writes every brush of `import` as a preset folder (with a preview) under `root`/<set>/, making names
/// unique; `written` receives the folders. False with `error` when the folder cannot be written.
bool saveBrushImport(const BrushImport& import, const std::string& root, std::vector<std::string>* written = nullptr,
                     std::string* error = nullptr);

} // namespace compositor
