// The small sheets: New canvas, Canvas Size, Image Size, JPEG export.
#pragma once
#include <QColor>
#include <QDialog>
#include <QImage>
#include <optional>

namespace app {

struct NewDocumentOptions { int width = 1920, height = 1080; double resolution = 72; };
std::optional<NewDocumentOptions> askNewDocument(QWidget* parent, NewDocumentOptions initial);

struct CanvasSizeOptions { int width, height; double anchorX = 0.5, anchorY = 0.5; };
std::optional<CanvasSizeOptions> askCanvasSize(QWidget* parent, int width, int height);

struct ImageSizeOptions { int width, height; double resolution; };
std::optional<ImageSizeOptions> askImageSize(QWidget* parent, int width, int height, double resolution);

struct JpegOptions { int quality = 85; QColor background = Qt::white; };
/// Shows a live preview of `flattened` over the background at the chosen quality.
std::optional<JpegOptions> askJpegExport(QWidget* parent, const QImage& flattened);

} // namespace app
