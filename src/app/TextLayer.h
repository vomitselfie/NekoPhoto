// Text layers: the raster a LayerText renders to, through Qt's font engine (the core has no fonts).
#pragma once
#include "compositor/document.h"
#include "compositor/psd.h"
#include "compositor/psd_writer.h"
#include "compositor/vectormask.h"
#include <QFont>
#include <QPointF>
#include <QString>
#include <memory>

namespace app {

/// The family new text starts with: the system's general-purpose font.
QString defaultTextFamily();
/// The QFont a text style asks for, at document pixels.
QFont fontFor(const compositor::LayerText& text);
/// Margin, in pixels, the raster keeps around the glyphs (so antialiasing and overhangs are not clipped).
constexpr int textPadding = 4;
/// The text drawn at 1:1 document pixels, premultiplied, sized to its lines plus the padding; null when it
/// would exceed the document's pixel budget. Warped text (Warp Text) is bent over its layout box; `warpOffset`
/// then gives where the bent raster's top-left sits from the upright one's.
std::shared_ptr<compositor::Image> renderTextLayer(const compositor::LayerText& text, QPointF* warpOffset = nullptr);
/// The same painted at 16 bits per channel (Qt's 16-bit raster; the glyphs' antialiasing is Qt's 8-bit coverage), for
/// a 16-bit document.
std::shared_ptr<compositor::Image16> renderTextLayer16(const compositor::LayerText& text, QPointF* warpOffset = nullptr);
/// The text at a document's depth; empty when it would exceed the pixel budget.
compositor::AnyImage renderTextLayerAt(const compositor::LayerText& text, compositor::SampleType type, QPointF* warpOffset = nullptr);
/// Type > Create Work Path: the text layer's glyph outlines as it is laid out upright, in document pixels (each glyph
/// a shape group); none (with `error`) for warped text or text with no outlines.
std::optional<compositor::VectorPath> textLayerOutline(const compositor::Layer& layer, QString* error = nullptr);
/// How renderTextLayer lays `text` out, for writing it as a Photoshop type layer.
std::optional<compositor::PsdTextMetrics> psdTextMetrics(const compositor::LayerText& text);
/// PSD export options with text layers written as Photoshop type layers.
compositor::PsdExportOptions psdExportOptions();
/// Finishes the type layers a PSD opened as text: the installed family for each face, the line spacing for
/// Photoshop's leading. A face that is not installed is noted; the text keeps Photoshop's pixels until edited.
void finishPsdText(compositor::PsdImport& imported);
/// Draws the text layers an import left without pixels (`PsdImport::pendingTexts`, Affinity's) and places each
/// where its frame says: point text on its first baseline, frame text with the first line's cap height at the top.
void finishPendingText(compositor::PsdImport& imported);
/// PSD import options with the app's decoders for smart object contents the core cannot read (JPEG, TIFF, ...).
compositor::PsdImportOptions psdImportOptions();
/// Affinity import options: placed JPEG and other originals decoded by Qt exactly as stored (the import applies
/// the EXIF orientation Affinity records itself).
compositor::PsdImportOptions affinityImportOptions();

} // namespace app
