// NekoPhoto's own documents (docs/project-format.md): the single-file .nekophoto document, a ZIP container, and the
// .comp project folder it grew from (manifest.json plus images/<UUID>.png assets, as IO/ProjectStore.swift wrote it).
// Both hold the same files under the same names; the path decides which is written.
#pragma once
#include "document.h"
#include <optional>
#include <string>

namespace compositor {

struct ProjectError {
    enum Kind { None, Invalid, Version, MissingImage, TooLarge, Encode, Io };
    Kind kind = None;
    std::string message;
    int version = 0;
    explicit operator bool() const { return kind != None; }
};

/// The newest version this reader opens. Version 9 is a CMYK or Lab document ("colorMode"); an RGB document writes
/// version 8 at most, so older NekoPhoto releases keep opening it.
constexpr int projectFormatVersion = 9;
/// What an RGB document with anything past the Mac app's format writes (16 bits, profiles, channels, artboards...).
constexpr int projectRgbFormatVersion = 8;
/// What a save writes when the document needs nothing past it (the Mac app reads up to 7).
constexpr int projectMacFormatVersion = 7;

/// The single-file document: a ZIP whose first entry, `mimetype`, holds `documentMimeType` (stored, so the type shows
/// at byte 38), then `nekophoto.json`, the package's files and `previews/composite.png`.
inline constexpr const char* documentFileExtension = ".nekophoto";
inline constexpr const char* documentMimeType = "application/vnd.nekophoto.document";
inline constexpr const char* documentFormatId = "org.nekophoto.document";
/// The container's version (nekophoto.json), apart from the manifest's version and the app's.
constexpr int documentContainerVersion = 1;
/// The long side of the preview a .nekophoto file carries, for thumbnailers.
constexpr int documentPreviewSize = 1024;

/// Whether `path` names a .nekophoto document (by its extension, in any case); any other path is a .comp folder.
bool isDocumentFilePath(const std::string& path);

/// What a package may hold beside its layers, in total, however valid each file is on its own: a hostile package
/// could otherwise hold thousands of individually acceptable smart objects and carried blocks.
struct ProjectLoadLimits {
    /// The smart objects' decoded contents together, in bytes at each source's own depth and channel count (a 16-bit
    /// source counts twice its 8-bit size, a CMYK one five samples a pixel): the same 4 GB as all the layers' pixels.
    long long smartObjectBytes = Document::projectPixelBudget * 4;
    /// Every sidecar file read (carried PSD data, smart object sources with their embedded files, instances).
    unsigned long long sidecarBytes = 4ULL << 30;
    int smartObjects = 4096;
    /// The layers' decoded pixels together, in bytes (samples times channels): the document's own budget
    /// (Document::projectPixelBudgetAt) at most, which is also 4 GB in every depth and mode.
    long long layerBytes = Document::projectPixelBudget * 4;
};

/// Loads a .nekophoto file or a .comp package folder (whichever is at `path`). On failure the error says why.
std::optional<Document> loadProject(const std::string& path, ProjectError& error, const ProjectLoadLimits& limits = {});
/// Saves atomically, as a .nekophoto file when the path says so (isDocumentFilePath), else as a .comp folder: writes a
/// sibling temporary file or folder, checks it, then swaps it in. A failed save leaves what was there untouched.
bool saveProject(const Document& document, const std::optional<Uuid>& activeLayerId, const std::string& path, ProjectError& error);

/// The active layer recorded in the manifest, if any.
std::optional<Uuid> loadedActiveLayer(const std::string& path);

/// The manifest for `document`, as the writer would produce it (pretty-printed, sorted keys).
std::string manifestJson(const Document& document, const std::optional<Uuid>& activeLayerId);
/// Parses a manifest (without assets); images and masks are left empty. For tests and tooling.
std::optional<Document> parseManifest(const std::string& json, ProjectError& error, std::optional<Uuid>* activeLayer = nullptr);

} // namespace compositor
