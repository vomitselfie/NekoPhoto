// A text layer's style runs as a QTextDocument, for editing letter by letter: each character's format carries its
// run's style exactly (as user properties, so a round trip through the document loses nothing) and shows it
// approximately (the font, a size scaled to fit an editor, weight, colour, spacing, caps, lines). Positions match
// the layer's: both count UTF-16 units, a paragraph break standing for the text's newline.
#pragma once
#include <compositor/document.h>
#include <QTextCharFormat>

class QTextDocument;

namespace app {

/// The format showing `run` (its size times `displayScale`) and holding it.
QTextCharFormat charFormatFor(const compositor::TextRun& run, double displayScale);
/// The run a format holds (length 0), or `fallback` when it holds none (text pasted from elsewhere).
compositor::TextRun runFromFormat(const QTextCharFormat& format, const compositor::TextRun& fallback);
/// `document` cleared and filled with the text in its runs.
void fillTextDocument(QTextDocument& document, const compositor::LayerText& text, double displayScale);
/// The runs of `document` over the UTF-16 units [from, to) (to -1: the end), in order, adjacent equal ones merged.
std::vector<compositor::TextRun> documentRuns(const QTextDocument& document, const compositor::TextRun& fallback, int from = 0, int to = -1);
/// The document's text and runs over `base` (its paragraph settings kept), settled (settleTextRuns).
compositor::LayerText layerTextFromDocument(const QTextDocument& document, compositor::LayerText base, const compositor::TextRun& fallback);

} // namespace app
