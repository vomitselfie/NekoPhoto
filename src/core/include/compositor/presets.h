// Photoshop preset libraries: layer styles (.asl), patterns (.pat) and gradients (.grd), read into NekoPhoto's own
// models and written back in the same formats (the app keeps its imported patterns and gradients as a .pat and a .grd
// file). The layouts are ported from Patchy (MIT, src/third_party/patchy_psd/README.md): src/psd/asl_io, pat_reader,
// grd_io and psd_patterns there, pinned against Photoshop 2026. See docs/presets.md.
#pragma once
#include "layerstyle.h"
#include "shape.h"
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace compositor {

struct Document;

// ---- Patterns --------------------------------------------------------------------------------------------------

/// One pattern: its id (what styles and fill layers reference), its name, its size, and its record in the shape of a
/// PSD 'Patt' block (u32 length, the record, padding to four bytes), so it can join a document's block unchanged.
struct PatternPreset {
    std::string id, name;
    int width = 0, height = 0;
    std::vector<uint8_t> record;
};

/// A .pat file's patterns ('8BPT', version 1). Records that cannot be used are left out with a line in `notes`; none
/// (with `error`) when the file is not one or holds nothing usable.
std::optional<std::vector<PatternPreset>> readPat(const std::vector<uint8_t>& bytes, std::string* error, std::vector<std::string>* notes = nullptr);
/// The patterns as a .pat file.
std::vector<uint8_t> writePat(const std::vector<PatternPreset>& patterns);
/// The records of a 'Patt'-shaped payload (a PSD's pattern block, an .asl's patterns section).
std::vector<PatternPreset> patternRecords(const std::vector<uint8_t>& payload);
/// A pattern from straight RGBA pixels: an 8-bit RGB record, with a transparency plane when some pixel is not opaque.
PatternPreset makePattern(const std::string& id, const std::string& name, int width, int height, const std::vector<uint8_t>& rgba);
/// The pattern's pixels (straight RGBA); none when its record cannot be decoded.
std::optional<PatternTile> decodePattern(const PatternPreset& pattern);
/// Gives the document the patterns it does not have yet, in its 'Patt' global block (with a new PSD carry when it has
/// none), where pattern overlays, bevel textures and PSD export find them. Returns how many were added.
int addDocumentPatterns(Document& document, const std::vector<PatternPreset>& patterns);
/// The pattern ids a style uses (pattern overlays and bevel textures).
std::vector<std::string> stylePatternIds(const LayerStyle& style);

// ---- Layer styles --------------------------------------------------------------------------------------------------

struct StylePreset {
    std::string id, name;
    LayerStyle style;
};

struct StyleLibrary {
    std::vector<StylePreset> styles;
    /// The file's patterns section: the patterns its styles use.
    std::vector<PatternPreset> patterns;
};

/// An .asl file ('8BSL', version 2): each style's effects ('Lefx') and the patterns it carries. A style's blending
/// options are not read (NekoPhoto's styles hold effects only); a note says so. None (with `error`) when the file is
/// not one or holds no usable style.
std::optional<StyleLibrary> readAsl(const std::vector<uint8_t>& bytes, std::string* error, std::vector<std::string>* notes = nullptr);
/// The styles and patterns as an .asl file.
std::vector<uint8_t> writeAsl(const StyleLibrary& library);

// ---- Gradients -----------------------------------------------------------------------------------------------------

/// A named gradient: colour stops (some may stand for the foreground or background colour) and opacity stops.
struct GradientPreset {
    enum class Source { User, Foreground, Background };
    struct Color { float location = 0; StyleColor color; float midpoint = 0.5f; Source source = Source::User; };
    std::string name;
    std::vector<Color> colors;
    std::vector<StyleGradient::AlphaStop> alphas;
    float smoothness = 1;   // Photoshop's 'Intr' / 4096
    /// The stops for a fill, with the foreground and background colours (straight RGB, 0..1) put in.
    GradientStops stops(const float foreground[3], const float background[3]) const;
    /// The same as a layer style's gradient (a Gradient Overlay's or a Stroke's).
    StyleGradient styleGradient(StyleColor foreground, StyleColor background) const;
};

/// A .grd file ('8BGR', version 5): its solid gradients. Noise gradients are left out with a note; version 3 files
/// (Photoshop 5 and older) are refused.
std::optional<std::vector<GradientPreset>> readGrd(const std::vector<uint8_t>& bytes, std::string* error, std::vector<std::string>* notes = nullptr);
/// The gradients as a .grd file.
std::vector<uint8_t> writeGrd(const std::vector<GradientPreset>& gradients);

} // namespace compositor
