// SVG files: opened as a layered document of editable vector shape layers (vectorlayer.h), and written from one.
// Each path, rect, circle, ellipse, line, polyline and polygon with solid paint becomes a shape layer, groups become
// folders; what the shape model cannot hold (gradients, patterns, text, images, filters, clip paths, masks, markers)
// is left to the app to draw as pixels, element by element, from the small standalone SVGs `rasterParts` carries.
// Export writes shape layers as <path> elements and every other layer as an embedded PNG. Ported from Patchy (MIT,
// src/third_party/patchy_psd/README.md): formats/svg_document_read.cpp, svg_document_write.cpp, vector_fill_rule.cpp
// and vector_export_plan.cpp. See docs/svg-pdf.md.
#pragma once
#include "document.h"
#include "vectorlayer.h"
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace compositor {

/// A layer the app fills by rendering `svg` (a standalone SVG the document's size, in document pixels).
struct SvgRasterPart {
    Uuid layer;
    std::string svg;
};

struct SvgImport {
    Document document;
    /// What was approximated or left to pixels, in plain words, each once.
    std::vector<std::string> notes;
    /// Placeholder layers (blank pixel layers in the stack) to draw.
    std::vector<SvgRasterPart> rasterParts;
    int shapeLayers = 0;
};

/// Reads an SVG (or gzip-compressed SVGZ) file's bytes; none, with `error` set, when it is not an SVG this reads
/// (the app then draws the whole file as one layer).
std::optional<SvgImport> importSvg(const std::vector<uint8_t>& bytes, std::string* error = nullptr);
std::optional<SvgImport> importSvgFile(const std::string& path, std::string* error = nullptr);
/// The bytes of an .svg or .svgz file, inflated; none when unreadable.
std::optional<std::vector<uint8_t>> readSvgBytes(const std::string& path, std::string* error = nullptr);

/// SVG path data ("M0 0L10 0...") as a path; none when malformed. Subpaths keep their own groups (non-zero is
/// resolved later by `applySvgFillRule`).
std::optional<VectorPath> parseSvgPathData(const std::string& data, std::string* error = nullptr);
/// Regroups a path's subpaths so the shape model's combine rules draw it as SVG's fill rule does: even-odd is one
/// group; non-zero makes each subpath its own group, a hole (wound against the outline it sits in) subtracting.
void applySvgFillRule(VectorPath& path, bool evenOdd);
/// A path as SVG path data (M, L, C, Z).
std::string svgPathData(const VectorPath& path);

struct SvgExportSummary {
    int shapes = 0, images = 0, groups = 0;
    std::vector<std::string> notes;
};
/// The document as SVG text: visible shape layers as paths, folders as groups, other layers as PNG images.
std::string writeSvg(const Document& document, SvgExportSummary* summary = nullptr);
bool exportSvg(const Document& document, const std::string& path, SvgExportSummary* summary = nullptr, std::string* error = nullptr);

} // namespace compositor
