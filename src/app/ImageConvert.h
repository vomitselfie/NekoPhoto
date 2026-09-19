// Conversions between the core's premultiplied RGBA buffers and QImage.
#pragma once
#include "compositor/image.h"
#include <QImage>
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

} // namespace app
