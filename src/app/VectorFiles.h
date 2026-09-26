// SVG and PDF files opened as layered documents (docs/svg-pdf.md). SVG shapes come from the core's reader
// (compositor/svg.h) as vector shape layers; what it hands on as raster parts, or a whole file it cannot read, is
// drawn here through Qt SVG. PDF pages are rendered through Qt PDF when the build has it.
#pragma once
#include "compositor/psd.h"
#include <QString>
#include <optional>

class QWidget;

namespace app {

/// An .svg or .svgz file as a document; `notes` says what became pixels or was approximated.
std::optional<compositor::PsdImport> importSvgDocument(const QString& path, QString* error);

/// Which page a PDF opens at (1-based) and how many pixels to the inch; document.open sets these for one open.
struct PdfOpenOptions {
    int page = 1;
    double resolution = 150;
    bool pageGiven = false;   // otherwise a multi-page PDF opened with a window asks which page
};
PdfOpenOptions& pdfOpenOptions();
/// Whether this build reads PDF files (Qt PDF was found).
bool pdfSupported();
/// A PDF page as a document of one pixel layer.
/// With `parent`, a PDF of several pages asks which one (unless document.open named it).
std::optional<compositor::PsdImport> importPdfDocument(const QString& path, QString* error, QWidget* parent = nullptr);

} // namespace app
