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

constexpr int projectFormatVersion = 7;

/// Loads a package directory. On failure the error says why, in the Mac app's words.
std::optional<Document> loadProject(const std::string& path, ProjectError& error);
/// Saves atomically: writes a sibling temporary package, then swaps it in.
bool saveProject(const Document& document, const std::optional<Uuid>& activeLayerId, const std::string& path, ProjectError& error);

/// The active layer recorded in the manifest, if any.
std::optional<Uuid> loadedActiveLayer(const std::string& path);

/// The manifest for `document`, as the writer would produce it (pretty-printed, sorted keys).
std::string manifestJson(const Document& document, const std::optional<Uuid>& activeLayerId);
/// Parses a manifest (without assets); images and masks are left empty. For tests and tooling.
std::optional<Document> parseManifest(const std::string& json, ProjectError& error, std::optional<Uuid>* activeLayer = nullptr);

} // namespace compositor
