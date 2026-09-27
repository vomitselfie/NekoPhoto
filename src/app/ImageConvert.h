// Conversions between the core's premultiplied RGBA buffers and QImage.
#pragma once
#include "compositor/depth.h"
#include "compositor/image.h"
#include <QBuffer>
#include <QColorSpace>
#include <QImage>
#include <QImageReader>
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
/// With `icc`, the file carries that colour profile (JPEG APP2, TIFF, WebP ICCP, as Qt's writers embed it).
inline bool writeQtImage(const QString& path, const char* format, QImage image, int quality, double dpi, QString* error, const QByteArray& icc = {}) {
    if (!icc.isEmpty()) {
        const QColorSpace space = QColorSpace::fromIccProfile(icc);
        if (space.isValid()) image.setColorSpace(space);
    }
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

/// A 16-bit core image as a QImage of 16 bits per channel (premultiplied, 0..65535), for Qt's 16-bit writers.
inline QImage toQImage16(const compositor::Image16& image) {
    QImage out(image.width(), image.height(), QImage::Format_RGBA64_Premultiplied);
    for (int y = 0; y < image.height(); y++) {
        const uint16_t* s = image.row(y);
        auto* d = reinterpret_cast<QRgba64*>(out.scanLine(y));
        for (int x = 0; x < image.width(); x++, s += 4)
            d[x] = QRgba64::fromRgba64(compositor::to65535(s[0]), compositor::to65535(s[1]), compositor::to65535(s[2]), compositor::to65535(s[3]));
    }
    return out;
}

/// A QImage of 16 bits per channel (a 16-bit TIFF through Qt) as a 16-bit core image.
inline std::shared_ptr<compositor::Image16> fromQImage16(const QImage& source) {
    const QImage converted = source.convertToFormat(QImage::Format_RGBA64_Premultiplied);
    auto out = std::make_shared<compositor::Image16>(converted.width(), converted.height());
    for (int y = 0; y < converted.height(); y++) {
        const auto* s = reinterpret_cast<const QRgba64*>(converted.constScanLine(y));
        uint16_t* d = out->row(y);
        for (int x = 0; x < converted.width(); x++, d += 4) {
            d[0] = compositor::from65535(s[x].red()); d[1] = compositor::from65535(s[x].green());
            d[2] = compositor::from65535(s[x].blue()); d[3] = compositor::from65535(s[x].alpha());
        }
    }
    return out;
}

/// Whether Qt's TIFF plugin keeps 16 bits per channel, found once by writing a 16-bit pixel and reading it back.
inline bool canWriteDeepTiff() {
    static const bool deep = [] {
        if (!canWriteImageFormat("tiff")) return false;
        QImage probe(1, 1, QImage::Format_RGBA64);
        probe.setPixelColor(0, 0, QColor::fromRgba64(QRgba64::fromRgba64(0x1235, 0x5679, 0x9abd, 0xffff)));
        QBuffer buffer;
        buffer.open(QIODevice::ReadWrite);
        if (!QImageWriter(&buffer, "tiff").write(probe)) return false;
        buffer.seek(0);
        const QImage back = QImageReader(&buffer, "tiff").read();
        return back.depth() == 64 && back.pixelColor(0, 0).rgba64().red() == 0x1235;
    }();
    return deep;
}

} // namespace app
