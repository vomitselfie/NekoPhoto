// Vector shape layers made and edited here: a layer whose shape is a path, filled with a colour and optionally
// stroked, stored the way Photoshop stores a shape layer, as the 'vsms' path, 'vstk' stroke and 'SoCo' fill
// blocks in the layer's PSD carry (psd_carry.h). The renderer draws them from those blocks (vectormask.h), PSD
// export writes them back, and the project package keeps them, so they stay editable everywhere. The layer's
// pixels are the fill colour over the path's bounds; the path cuts them. Block layouts follow Patchy (MIT,
// src/third_party/patchy_psd/README.md), which pinned them against Photoshop 2026. See docs/vector-tools.md.
#pragma once
#include "document.h"
#include "vectormask.h"
#include <optional>
#include <string>
#include <vector>

namespace compositor {

struct VectorShape {
    VectorPath path;                       // document pixels
    bool fill = true;
    uint8_t r = 0, g = 0, b = 0;           // the fill colour
    VectorStroke stroke;                   // stroke.enabled false: no stroke
};

/// The blocks, as Photoshop writes them. The path is stored against a `canvasWidth` x `canvasHeight` canvas.
std::vector<uint8_t> authorVectorMask(const VectorPath& path, int canvasWidth, int canvasHeight);
std::vector<uint8_t> authorVectorStroke(const VectorStroke& stroke, bool fillEnabled);
std::vector<uint8_t> authorSolidColour(uint8_t r, uint8_t g, uint8_t b);

/// The canvas a document's paths are stored against (a PSD's own, as read; else the document's).
void pathCanvas(const Document& document, int& width, int& height);

/// Whether the layer is a vector shape layer (a path with a solid fill, its pixels still that fill: painting on it makes
/// it pixels cut by a vector mask), and its shape as it now stands.
bool isVectorShapeLayer(const Layer& layer);
std::optional<VectorShape> vectorShapeOf(const Layer& layer, const Document& document);
/// Makes `layer` show `shape`: its pixels become the fill colour over the path's bounds (grown by the stroke), and
/// its carry takes the path, stroke and fill blocks, pinned to those pixels so they are kept. Other carried blocks
/// (styles, Blend If, ...) stay.
void setVectorShape(Layer& layer, const Document& document, const VectorShape& shape);

/// Vector shape layers moved, scaled or rotated since they were drawn, drawn again on their moved path (a shape
/// scales as a path, not as pixels). Returns how many.
int refreshVectorShapes(Document& document);

/// The control-point bounds of a path (document pixels); empty for none.
Rect pathBounds(const VectorPath& path);

// ---- Paths the Shape tool draws (one closed subpath each, clockwise, document pixels) ---------------------------

VectorPath rectanglePath(const Rect& box, double cornerRadius = 0);
VectorPath ellipsePath(const Rect& box);
/// A regular polygon, or a star when `starInset` (0..0.99, how far the inner points come in) is above zero, fitting
/// `box`, its first point at the top.
VectorPath polygonPath(const Rect& box, int sides, double starInset = 0);
/// A line from `a` to `b`, `weight` pixels thick (a closed band, as Photoshop's Line tool makes).
VectorPath linePath(Point a, Point b, double weight);
/// The built-in custom shapes (Photoshop's defaults' spirit: heart, star, arrow, speech bubble, check mark,
/// lightning bolt), fitting `box`; their names in `customShapeNames`.
const std::vector<std::string>& customShapeNames();
VectorPath customShapePath(const std::string& name, const Rect& box);

// ---- The document's paths (Photoshop's Paths panel) -------------------------------------------------------------
// Saved paths are image resources 2000..2997 (named), the Work Path is 1025, kept in the document's PSD carry (one is
// made for a document that has none), so they go into PSD files and projects as Photoshop keeps them.

struct DocumentPath {
    uint16_t id = 0;                       // the resource id: 1025 is the Work Path
    std::string name;
    VectorPath path;                       // document pixels
};
constexpr uint16_t kWorkPathId = 1025;
std::vector<DocumentPath> documentPaths(const Document& document);
std::optional<DocumentPath> documentPath(const Document& document, uint16_t id);
/// Stores `path` as the path `id` (0: a new saved path named `name`); returns its id.
uint16_t setDocumentPath(Document& document, uint16_t id, const std::string& name, const VectorPath& path);
void renameDocumentPath(Document& document, uint16_t id, const std::string& name);
void removeDocumentPath(Document& document, uint16_t id);
/// A path resource's records (without the vector mask block's version and flags), and back.
std::vector<uint8_t> authorPathResource(const VectorPath& path, int canvasWidth, int canvasHeight);
std::optional<VectorPath> parsePathResource(const std::vector<uint8_t>& data, int canvasWidth, int canvasHeight);

/// Every knot's smoothness (in and out handles on one line through the anchor).
bool knotIsSmooth(const VectorPath::Knot& knot);

} // namespace compositor
