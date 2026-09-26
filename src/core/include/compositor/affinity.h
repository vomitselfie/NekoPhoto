// Affinity documents (.afphoto, .afdesign, .afpub and the unified app's .af) as layered documents: pixel layers,
// groups, opacity, visibility, blend modes, raster and vector masks, clipped children, artboards, embedded
// documents (flattened), vector curves and parametric shapes drawn as pixels, and artistic and frame text as text
// layers the app draws afterwards (`PsdImport::pendingTexts`). Adjustments, live filters and layer effects are not
// read; the notes list every layer left out or approximated.
//
// Ported from Patchy (MIT, src/third_party/patchy_psd/README.md), its src/formats/af_document_io.cpp and
// af_tree.cpp: the container walk, the stream table, the tile planes, the blend-mode table and the placement
// rules are Patchy's findings, verified there against documents authored in Affinity. Streams compressed with
// zstd need libzstd at build time (COMPOSITOR_HAVE_ZSTD); without it such a file is refused with a message.
#pragma once
#include "psd.h"
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace compositor {

/// Reads an Affinity file. `PsdImport::composite` holds the file's embedded preview (at most about 512 pixels);
/// `notes` lists what was approximated or left out. `options.decodeImage` decodes placed JPEG originals.
std::optional<PsdImport> importAffinity(const std::string& path, std::string* error = nullptr, const PsdImportOptions& options = {});
std::optional<PsdImport> importAffinityBytes(const std::vector<uint8_t>& file, std::string* error = nullptr, const PsdImportOptions& options = {});

/// Whether the file starts like an Affinity document (00 FF 4B 41).
bool isAffinityFile(const std::string& path);
/// Whether this build decompresses zstd streams (most documents from Affinity 2 and later use them).
bool affinityZstdSupported();

namespace affinity_detail {
/// The stream predictors, undone in place: a running byte sum, a running little-endian 16-bit sum, and the 16-bit
/// tile layout's two half planes of byte pairs (for a 64 KiB tile only).
void undoByteDelta(std::vector<uint8_t>& bytes);
void undoU16Delta(std::vector<uint8_t>& bytes);
void undoTileInterleave(std::vector<uint8_t>& bytes);
/// Affinity's blend wire value (id, enum version) as NekoPhoto's mode; none when there is no counterpart.
std::optional<BlendMode> blendMode(uint16_t id, uint16_t version);
/// A 2x3 affine in Affinity's wire order [a, b, tx, c, d, ty], applied parent then child.
std::vector<double> composeTransforms(const std::vector<double>& parent, const std::vector<double>& child);
} // namespace affinity_detail

} // namespace compositor
