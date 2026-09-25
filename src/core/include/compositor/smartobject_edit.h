// Making and changing smart objects: Convert to Smart Object, Place Embedded, Replace Contents, committing edited
// contents, Rasterize. Each works on a Document (the app wraps it in one undo step); smartobject.h has the model.
#pragma once
#include "document.h"
#include "psd_writer.h"
#include <optional>
#include <string>
#include <vector>

namespace compositor {

/// A source's file: its bytes, name and Photoshop file type ("8BPS", "8BPB", "png ", "JPEG", ...), and its
/// contents as an image (the caller decodes what the core cannot).
struct SmartObjectContents {
    std::vector<uint8_t> bytes;
    std::string fileName, fileType;
    ImagePtr image;
    double resolution = 72;
};

/// A new source (fresh id) from `contents`; null without an image.
std::shared_ptr<const SmartObjectSource> makeSmartObjectSource(SmartObjectContents contents);

/// The Photoshop file type for a file name's extension ("8BPS" for .psd, "png " for .png, ...); empty if unknown.
std::string smartObjectFileType(const std::string& fileName);

/// A layer placing `source` on `quad` (a new Photoshop placement authored for it); not yet in any document.
Layer smartObjectLayer(const std::shared_ptr<const SmartObjectSource>& source, const std::array<double, 8>& quad, const std::string& name);

/// Photoshop's Place: 1:1 in the middle of the canvas, scaled down to fit when larger.
std::array<double, 8> placementQuad(const Document& document, int width, int height);

/// Adds `source` to the document and a layer placing it above the active layer's position `index` (or on top),
/// under `parent`; returns the new layer's id.
Uuid placeSmartObject(Document& document, const std::shared_ptr<const SmartObjectSource>& source, size_t index, std::optional<Uuid> parent);

/// The layers `ids` (with everything in the folders among them) as one smart object: a PSD of them (canvas =
/// their bounds) becomes the source, and a layer placing it takes the topmost one's place and name. None, with
/// `error`, when they cannot be (nothing to convert, too large for PSD).
std::optional<Uuid> convertToSmartObject(Document& document, const std::vector<Uuid>& ids, std::string* error, const PsdExportOptions& options = {});

/// Points every layer placing source `from` at `replacement` (added to the document), each rebuilt about its own
/// centre at its own scale; `from` is dropped. The instances must all be editable. Returns how many changed.
int replaceSmartObjectSource(Document& document, const std::string& from, const std::shared_ptr<const SmartObjectSource>& replacement);

/// Whether every layer placing `sourceId` can take new contents (none is preview-locked).
bool smartObjectContentsEditable(const Document& document, const std::string& sourceId, std::string* why = nullptr);

/// The source as a document to edit: its PSD's layers, or its image as one layer. None when it is not readable.
std::optional<Document> smartObjectContentsDocument(const Document& document, const std::string& sourceId);

/// The edited contents written back in the source's own format where the core can (PSD; PNG); other types need the
/// app's encoder and come back empty here.
std::vector<uint8_t> encodeSmartObjectContents(const Document& contents, const SmartObjectSource& source, const PsdExportOptions& options = {});

/// The layer as plain pixels (it keeps what it shows).
void rasterizeSmartObject(Layer& layer);

} // namespace compositor
