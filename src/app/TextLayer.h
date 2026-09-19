// Text layers: the raster a LayerText renders to, through Qt's font engine (the core has no fonts).
#pragma once
#include "compositor/document.h"
#include <QFont>
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
/// would exceed the document's pixel budget.
std::shared_ptr<compositor::Image> renderTextLayer(const compositor::LayerText& text);

} // namespace app
