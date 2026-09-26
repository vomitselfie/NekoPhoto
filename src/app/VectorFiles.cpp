// SVG and PDF files opened as layered documents (VectorFiles.h).
#include "VectorFiles.h"
#include "ImageConvert.h"
#include "compositor/svg.h"
#include <QFileInfo>
#include <QInputDialog>
#include <QPainter>
#include <QSvgRenderer>
#include <algorithm>
#include <cmath>
#ifdef COMPOSITOR_HAVE_QTPDF
#include <QPdfDocument>
#include <QPdfDocumentRenderOptions>
#endif

using namespace compositor;

namespace app {

namespace {

/// `svg` drawn over a width x height canvas, cropped to what it paints; none when it paints nothing.
std::optional<std::pair<ImagePtr, Point>> renderSvg(const QByteArray& svg, int width, int height, bool fitViewBox) {
    QSvgRenderer renderer(svg);
    if (!renderer.isValid()) return std::nullopt;
    if (!fitViewBox) renderer.setAspectRatioMode(Qt::IgnoreAspectRatio);
    QImage image(width, height, QImage::Format_RGBA8888_Premultiplied);
    image.fill(Qt::transparent);
    {
        QPainter painter(&image);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        renderer.render(&painter, QRectF(0, 0, width, height));
    }
    int x0 = width, y0 = height, x1 = -1, y1 = -1;
    for (int y = 0; y < height; y++) {
        const uchar* row = image.constScanLine(y);
        for (int x = 0; x < width; x++)
            if (row[x * 4 + 3]) { x0 = std::min(x0, x); x1 = std::max(x1, x); y0 = std::min(y0, y); y1 = y; }
    }
    if (x1 < 0) return std::nullopt;
    return std::pair{fromQImage(image.copy(x0, y0, x1 - x0 + 1, y1 - y0 + 1)), Point(x0, y0)};
}

} // namespace

std::optional<PsdImport> importSvgDocument(const QString& path, QString* error) {
    std::string message;
    auto imported = importSvgFile(path.toStdString(), &message);
    PsdImport result;
    if (!imported) {
        // Not something the layered reader takes (or past its limits): the whole file as one layer, as Qt SVG draws it.
        auto bytes = readSvgBytes(path.toStdString());
        if (!bytes) { if (error) *error = QString::fromStdString(message); return std::nullopt; }
        const QByteArray data(reinterpret_cast<const char*>(bytes->data()), qsizetype(bytes->size()));
        QSvgRenderer probe(data);
        if (!probe.isValid()) { if (error) *error = QObject::tr("The SVG could not be read: %1").arg(QString::fromStdString(message)); return std::nullopt; }
        QSize size = probe.defaultSize();
        if (size.isEmpty()) size = QSize(300, 150);
        if (size.width() > maxImageSide || size.height() > maxImageSide) size.scale(maxImageSide, maxImageSide, Qt::KeepAspectRatio);
        auto drawn = renderSvg(data, size.width(), size.height(), true);
        result.document = Document(size.width(), size.height());
        if (drawn) {
            Layer layer(Asset::make(drawn->first, QFileInfo(path).completeBaseName().toStdString()), drawn->second);
            result.document.layers.push_back(std::move(layer));
        }
        result.notes.push_back(QObject::tr("The SVG could not be opened as layers (%1); it was drawn as one pixel layer.").arg(QString::fromStdString(message)).toStdString());
        return result;
    }
    result.document = std::move(imported->document);
    result.notes = std::move(imported->notes);
    Document& doc = result.document;
    int blank = 0;
    for (const SvgRasterPart& part : imported->rasterParts) {
        Layer* layer = doc.find(part.layer);
        if (!layer) continue;
        auto drawn = renderSvg(QByteArray::fromStdString(part.svg), doc.width, doc.height, false);
        if (!drawn) { doc.layers.erase(doc.layers.begin() + doc.indexOf(part.layer)); blank++; continue; }
        const LayerTransform transform(drawn->second, Size(drawn->first->width(), drawn->first->height()));
        layer->asset = Asset::make(drawn->first, layer->name);
        layer->transform = transform;
    }
    if (blank) result.notes.push_back(QObject::tr("%n SVG element(s) Qt SVG could not draw (filters, masks and clip paths need Qt 6.7 or later) were left out.", nullptr, blank).toStdString());
    if (doc.layers.empty()) doc.layers.push_back(Layer("Layer 1", doc.size()));
    return result;
}

PdfOpenOptions& pdfOpenOptions() {
    static PdfOpenOptions options;
    return options;
}

bool pdfSupported() {
#ifdef COMPOSITOR_HAVE_QTPDF
    return true;
#else
    return false;
#endif
}

std::optional<PsdImport> importPdfDocument(const QString& path, QString* error, QWidget* parent) {
    const PdfOpenOptions options = pdfOpenOptions();
    pdfOpenOptions() = PdfOpenOptions();   // one open only
#ifdef COMPOSITOR_HAVE_QTPDF
    QPdfDocument pdf;
    const QPdfDocument::Error status = pdf.load(path);
    if (status != QPdfDocument::Error::None) {
        if (error) {
            switch (status) {
            case QPdfDocument::Error::IncorrectPassword: *error = QObject::tr("The PDF is password protected."); break;
            case QPdfDocument::Error::UnsupportedSecurityScheme: *error = QObject::tr("The PDF uses a security scheme that cannot be opened."); break;
            case QPdfDocument::Error::FileNotFound: *error = QObject::tr("The file could not be found."); break;
            default: *error = QObject::tr("This is not a PDF that can be read."); break;
            }
        }
        return std::nullopt;
    }
    const int count = pdf.pageCount();
    int pageNumber = options.page;
    if (parent && count > 1 && !options.pageGiven) {
        bool ok = false;
        pageNumber = QInputDialog::getInt(parent, QObject::tr("Open PDF"), QObject::tr("%1 has %2 pages. Open page:").arg(QFileInfo(path).fileName()).arg(count), 1, 1, count, 1, &ok);
        if (!ok) { if (error) error->clear(); return std::nullopt; }
    }
    if (pageNumber < 1 || pageNumber > count) {
        if (error) *error = QObject::tr("The PDF has %n page(s); page %1 is not one of them.", nullptr, count).arg(pageNumber);
        return std::nullopt;
    }
    const int index = pageNumber - 1;
    const double ppi = std::clamp(options.resolution, 18.0, 1200.0);
    const QSizeF points = pdf.pagePointSize(index);
    if (points.width() <= 0 || points.height() <= 0) { if (error) *error = QObject::tr("The page has no size."); return std::nullopt; }
    double width = points.width() / 72 * ppi, height = points.height() / 72 * ppi;
    PsdImport result;
    double scale = 1;
    const double budget = std::sqrt(double(Document::pixelBudget) / (width * height));
    scale = std::min({1.0, maxImageSide / width, maxImageSide / height, budget});
    if (scale < 1) result.notes.push_back(QObject::tr("The page was rendered at %1 pixels per inch to fit the canvas limits.").arg(int(ppi * scale)).toStdString());
    width *= scale; height *= scale;
    const QSize size(std::max(1, int(std::lround(width))), std::max(1, int(std::lround(height))));
    QPdfDocumentRenderOptions render;
    render.setRenderFlags(QPdfDocumentRenderOptions::RenderFlag::Annotations);
    const QImage page = pdf.render(index, size, render);
    if (page.isNull()) { if (error) *error = QObject::tr("The page could not be rendered."); return std::nullopt; }
    result.document = Document(size.width(), size.height());
    result.document.resolution = ppi * scale;
    const QString name = QObject::tr("Page %1").arg(pageNumber);
    result.document.layers.push_back(Layer(Asset::make(fromQImage(page), name.toStdString()), Point(0, 0)));
    return result;
#else
    (void)path;
    (void)options;
    (void)parent;
    if (error) *error = QObject::tr("This build of NekoPhoto opens PDF files only with Qt PDF, which was not found when it was built.");
    return std::nullopt;
#endif
}

} // namespace app
