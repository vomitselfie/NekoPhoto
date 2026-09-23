// Conversions between the core's premultiplied RGBA buffers and QImage.
#pragma once
#include "compositor/image.h"
#include <QImage>
#include <QImageWriter>
#include <cstring>

namespace app {

/// Wraps a core image as a QImage that shares its memory (the core image must outlive the QImage).
inline QImage wrapImage(const compositor::Image& image) {
    return QImage(image.data(), image.width(), image.height(), image.stride(), QImage::Format_RGBA8888_Premultiplied);
}

inline QImage toQImage(const compositor::Image& image) { return wrapImage(image).copy(); }

inline QImage toQImage(const compositor::GrayImage& image) {
    return QImage(image.data(), image.width(), image.height(), image.stride(), QImage::Format_Grayscale8).copy();
}

/// Copies any QImage into a premultiplied RGBA core image.
inline std::shared_ptr<compositor::Image> fromQImage(const QImage& source) {
    QImage converted = source.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
    auto out = std::make_shared<compositor::Image>(converted.width(), converted.height());
    for (int y = 0; y < converted.height(); y++) std::memcpy(out->row(y), converted.constScanLine(y), size_t(converted.width()) * 4);
    return out;
}

inline std::shared_ptr<compositor::GrayImage> grayFromQImage(const QImage& source) {
    QImage converted = source.convertToFormat(QImage::Format_Grayscale8);
    auto out = std::make_shared<compositor::GrayImage>(converted.width(), converted.height());
    for (int y = 0; y < converted.height(); y++) std::memcpy(out->row(y), converted.constScanLine(y), size_t(converted.width()));
    return out;
}

/// Writes a flattened image through Qt's image plugins (JPEG, WebP, TIFF), with its resolution. For WebP a
/// quality of 100 is lossless; TIFF is LZW-compressed and keeps transparency.
inline bool writeQtImage(const QString& path, const char* format, QImage image, int quality, double dpi, QString* error) {
    const int dotsPerMeter = int(dpi / 0.0254 + 0.5);
    image.setDotsPerMeterX(dotsPerMeter);
    image.setDotsPerMeterY(dotsPerMeter);
    QImageWriter writer(path, format);
    if (qstrcmp(format, "tiff") == 0) writer.setCompression(1);
    else writer.setQuality(quality);
    if (writer.write(image)) return true;
    if (error) *error = writer.errorString();
    return false;
}

/// Whether this Qt has a writer for the format (WebP and TIFF come from the qtimageformats plugins).
inline bool canWriteImageFormat(const char* format) { return QImageWriter::supportedImageFormats().contains(format); }

} // namespace app
