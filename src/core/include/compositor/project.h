// The .comp project package: manifest.json plus images/<UUID>.png assets,
// exactly as IO/ProjectStore.swift reads and writes it (see docs/project-format.md).
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

/// Loads a package directory. On failure the error says why, in the Mac app's words.
std::optional<Document> loadProject(const std::string& path, ProjectError& error, const ProjectLoadLimits& limits = {});
/// Saves atomically: writes a sibling temporary package, then swaps it in.
bool saveProject(const Document& document, const std::optional<Uuid>& activeLayerId, const std::string& path, ProjectError& error);

/// The active layer recorded in the manifest, if any.
std::optional<Uuid> loadedActiveLayer(const std::string& path);

/// The manifest for `document`, as the writer would produce it (pretty-printed, sorted keys).
std::string manifestJson(const Document& document, const std::optional<Uuid>& activeLayerId);
/// Parses a manifest (without assets); images and masks are left empty. For tests and tooling.
std::optional<Document> parseManifest(const std::string& json, ProjectError& error, std::optional<Uuid>* activeLayer = nullptr);

} // namespace compositor
