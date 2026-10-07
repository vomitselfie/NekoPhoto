// Export As and Quick Export: settings per format, a layer's own document, and the pixel steps document.export takes
// for the flat formats (flatten, trim, resample, matte, encode).
#include "ExportAs.h"
#include "ImageConvert.h"
#include "compositor/gif.h"
#include "compositor/ico.h"
#include "compositor/png.h"
#include "compositor/resample.h"
#include "compositor/tga.h"
#include <QBuffer>
#include <QCoreApplication>
#include <QImageWriter>
#include <QPainter>
#include <QSettings>
#include <algorithm>
#include <cmath>
#include <set>

using namespace compositor;

namespace app::exportas {

QString resampleKey(Resample r) {
    switch (r) {
    case Resample::Bilinear: return QStringLiteral("bilinear");
    case Resample::Nearest: return QStringLiteral("nearest");
    case Resample::Lanczos: return QStringLiteral("lanczos");
    case Resample::Bicubic: break;
    }
    return QStringLiteral("bicubic");
}

std::optional<Resample> resampleNamed(const QString& name) {
    QString n = name.toLower();
    n.remove('-').remove('_').remove(' ');
    if (n == "bicubic") return Resample::Bicubic;
    if (n == "bilinear") return Resample::Bilinear;
    if (n == "nearest" || n == "nearestneighbor" || n == "nearestneighbour") return Resample::Nearest;
    if (n == "lanczos") return Resample::Lanczos;
    return std::nullopt;
}

QString resampleLabel(Resample r) {
    switch (r) {
    case Resample::Bilinear: return QCoreApplication::translate("ExportAs", "Bilinear");
    case Resample::Nearest: return QCoreApplication::translate("ExportAs", "Nearest Neighbour (hard edges)");
    case Resample::Lanczos: return QCoreApplication::translate("ExportAs", "Lanczos (sharpest when reducing)");
    case Resample::Bicubic: break;
    }
    return QCoreApplication::translate("ExportAs", "Bicubic");
}

QStringList formats() {
    QStringList out{QStringLiteral("png"), QStringLiteral("jpg"), QStringLiteral("gif")};
    if (canWriteImageFormat("webp")) out << QStringLiteral("webp");
    if (canWriteImageFormat("tiff")) out << QStringLiteral("tif");
    out << QStringLiteral("tga");
    return out;
}

QString formatLabel(const QString& format) {
    if (format == "jpg") return QStringLiteral("JPEG");
    if (format == "tif") return QStringLiteral("TIFF");
    if (format == "webp") return QStringLiteral("WebP");
    return format.toUpper();
}

QString formatKey(const QString& suffixOrName) {
    const QString s = suffixOrName.toLower();
    if (s == "jpg" || s == "jpeg") return QStringLiteral("jpg");
    if (s == "tif" || s == "tiff") return QStringLiteral("tif");
    if (s == "png" || s == "gif" || s == "webp" || s == "tga" || s == "ico") return s;
    return {};
}

bool hasQuality(const QString& format) { return format == "jpg" || format == "webp"; }
bool hasTransparency(const QString& format) { return format != "jpg"; }
int defaultQuality(const QString& format) { return format == "webp" ? 90 : 85; }

namespace {
QString settingsKey(const QString& format, const char* name) { return QStringLiteral("export/%1/%2").arg(format, QLatin1String(name)); }
}

Settings remembered(const QString& format) {
    QSettings s;
    Settings out;
    out.quality = std::clamp(s.value(settingsKey(format, "quality"), defaultQuality(format)).toInt(), 1, 100);
    out.transparency = s.value(settingsKey(format, "transparency"), true).toBool();
    const QColor matte(s.value(settingsKey(format, "matte"), QStringLiteral("#ffffff")).toString());
    out.matte = matte.isValid() ? matte : QColor(Qt::white);
    out.resample = resampleNamed(s.value(settingsKey(format, "resample"), QStringLiteral("bicubic")).toString()).value_or(Resample::Bicubic);
    out.convertToSrgb = s.value(settingsKey(format, "convertToSrgb"), true).toBool();
    out.embedProfile = s.value(settingsKey(format, "embedProfile"), true).toBool();
    return out;
}

void remember(const QString& format, const Settings& settings) {
    QSettings s;
    s.setValue(settingsKey(format, "quality"), settings.quality);
    s.setValue(settingsKey(format, "transparency"), settings.transparency);
    s.setValue(settingsKey(format, "matte"), settings.matte.name());
    s.setValue(settingsKey(format, "resample"), resampleKey(settings.resample));
    s.setValue(settingsKey(format, "convertToSrgb"), settings.convertToSrgb);
    s.setValue(settingsKey(format, "embedProfile"), settings.embedProfile);
}

QString lastFormat() {
    const QString f = QSettings().value("export/lastFormat", QStringLiteral("png")).toString();
    return formats().contains(f) ? f : QStringLiteral("png");
}
void setLastFormat(const QString& format) { QSettings().setValue("export/lastFormat", format); }

QString quickExportFormat() {
    const QString f = QSettings().value("export/quickFormat", QStringLiteral("png")).toString();
    return formats().contains(f) ? f : QStringLiteral("png");
}
void setQuickExportFormat(const QString& format) { QSettings().setValue("export/quickFormat", format); }

QJsonObject commandParams(const QString& format, const Settings& settings, int width, int height, const QString& layer) {
    QJsonObject p;
    if (hasQuality(format)) p["quality"] = settings.quality;
    if (format == "jpg") p["background"] = settings.matte.name();
    else if (!settings.transparency) { p["transparency"] = false; p["background"] = settings.matte.name(); }
    if (width > 0) p["width"] = width;
    if (height > 0) p["height"] = height;
    if (width > 0 || height > 0) p["resample"] = resampleKey(settings.resample);
    p["convertToSrgb"] = settings.convertToSrgb;
    p["embedProfile"] = settings.embedProfile;
    if (format == "gif") p["animated"] = false;   // Export As writes the composite; the timeline has File > Export > Animated GIF
    if (!layer.isEmpty()) p["layer"] = layer;
    return p;
}

// ---- the pixels ------------------------------------------------------------------------------------------------

std::optional<Document> layerDocument(const Document& document, const Uuid& id, std::string* error) {
    const Layer* target = document.find(id);
    if (!target) { if (error) *error = "no such layer"; return std::nullopt; }
    // The layer and, for a folder, everything inside it.
    std::set<Uuid> keep{id};
    for (bool grew = true; grew;) {
        grew = false;
        for (const Layer& l : document.layers)
            if (l.parentId && keep.count(*l.parentId) && keep.insert(l.id).second) grew = true;
    }
    Document solo = document;
    solo.layers.clear();
    for (const Layer& l : document.layers) {
        if (!keep.count(l.id)) continue;
        Layer copy = l;
        if (l.id == id) {
            copy.parentId.reset();
            copy.visible = true;
            copy.maskSourceId.reset();   // a clipped layer shows its own pixels, without its base
        } else if (copy.maskSourceId && !keep.count(*copy.maskSourceId)) copy.maskSourceId.reset();
        solo.layers.push_back(std::move(copy));
    }
    solo.selection.reset();
    solo.channels.clear();
    solo.slices.clear();
    solo.guides.clear();
    solo.animation = Animation{};
    return solo;
}

bool keepsSixteenBits(const QString& format, const Document& document) {
    if (document.sampleType == SampleType::U8) return false;
    return format == "png" || (format == "tif" && canWriteDeepTiff());
}

namespace {

/// The bounds of the pixels whose alpha is above zero; empty when there are none.
template <class Img, class Sample>
Rect visibleBounds(const Img& image) {
    int x0 = image.width(), y0 = image.height(), x1 = -1, y1 = -1;
    for (int y = 0; y < image.height(); y++) {
        const Sample* row = reinterpret_cast<const Sample*>(image.row(y));
        for (int x = 0; x < image.width(); x++)
            if (row[size_t(x) * 4 + 3]) { x0 = std::min(x0, x); x1 = std::max(x1, x); y0 = std::min(y0, y); y1 = std::max(y1, y); }
    }
    if (x1 < 0) return {};
    return Rect(x0, y0, x1 - x0 + 1, y1 - y0 + 1);
}

template <class Img, class Sample>
std::shared_ptr<Img> cropped(const Img& image, const Rect& r) {
    auto out = std::make_shared<Img>(int(r.width), int(r.height));
    for (int y = 0; y < out->height(); y++)
        std::memcpy(out->row(y), reinterpret_cast<const Sample*>(image.row(int(r.y) + y)) + size_t(r.x) * 4, size_t(out->width()) * 4 * sizeof(Sample));
    return out;
}

template <class Img, class Sample>
std::shared_ptr<Img> nearest(const Img& image, int width, int height) {
    auto out = std::make_shared<Img>(width, height);
    const double sx = double(image.width()) / width, sy = double(image.height()) / height;
    for (int y = 0; y < height; y++) {
        const int from = std::min(image.height() - 1, int((y + 0.5) * sy));
        const Sample* s = reinterpret_cast<const Sample*>(image.row(from));
        Sample* d = reinterpret_cast<Sample*>(out->row(y));
        for (int x = 0; x < width; x++) {
            const int at = std::min(image.width() - 1, int((x + 0.5) * sx));
            std::memcpy(d + size_t(x) * 4, s + size_t(at) * 4, 4 * sizeof(Sample));
        }
    }
    return out;
}

ResampleFilter filterOf(Resample r) {
    return r == Resample::Bilinear ? ResampleFilter::Triangle : r == Resample::Lanczos ? ResampleFilter::Lanczos3 : ResampleFilter::CatmullRom;
}

/// The QImage encoders (JPEG, WebP, TIFF) into memory, as writeQtImage writes a file.
bool encodeQt(QImage image, const char* format, int quality, double dpi, const std::vector<uint8_t>& icc, QByteArray& out, QString& error) {
    if (!icc.empty()) {
        const QColorSpace space = QColorSpace::fromIccProfile(QByteArray(reinterpret_cast<const char*>(icc.data()), qsizetype(icc.size())));
        if (space.isValid()) image.setColorSpace(space);
    }
    const int dotsPerMeter = int(dpi / 0.0254 + 0.5);
    image.setDotsPerMeterX(dotsPerMeter);
    image.setDotsPerMeterY(dotsPerMeter);
    QBuffer buffer(&out);
    buffer.open(QIODevice::WriteOnly);
    QImageWriter writer(&buffer, format);
    if (qstrcmp(format, "tiff") == 0) writer.setCompression(1);
    else writer.setQuality(quality);
    if (writer.write(image)) return true;
    error = writer.errorString();
    return false;
}

/// Over an opaque colour, as the JPEG export always flattened (Qt's source-over on premultiplied pixels).
std::shared_ptr<Image> matted(const Image& image, const QColor& matte) {
    QImage flat(image.width(), image.height(), QImage::Format_RGBA8888_Premultiplied);
    flat.fill(QColor(matte.red(), matte.green(), matte.blue()));
    QPainter p(&flat);
    p.drawImage(0, 0, wrapImage(image));
    p.end();
    return fromQImage(flat);
}

std::shared_ptr<Image16> matted(const Image16& image, const QColor& matte) {
    constexpr int one = 32768;
    const int m[3] = {(matte.red() * one + 127) / 255, (matte.green() * one + 127) / 255, (matte.blue() * one + 127) / 255};
    auto out = std::make_shared<Image16>(image.width(), image.height());
    for (int y = 0; y < image.height(); y++) {
        const uint16_t* s = image.row(y);
        uint16_t* d = out->row(y);
        for (int x = 0; x < image.width(); x++, s += 4, d += 4) {
            const int keep = one - s[3];
            for (int c = 0; c < 3; c++) d[c] = uint16_t(std::min(one, s[c] + int((int64_t(m[c]) * keep + one / 2) / one)));
            d[3] = one;
        }
    }
    return out;
}

} // namespace

Flat flatten(const Document& document, const QString& format, const color::ExportPlan& plan, bool trim) {
    Flat out;
    if (keepsSixteenBits(format, document)) out.image16 = color::flatten16(document, plan);
    else out.image8 = color::flatten8(document, plan);
    if (trim && !out.empty()) {
        const Rect r = out.image16 ? visibleBounds<Image16, uint16_t>(*out.image16) : visibleBounds<Image, uint8_t>(*out.image8);
        if (r.width <= 0 || r.height <= 0) return {};
        if (int(r.width) != out.width() || int(r.height) != out.height()) {
            if (out.image16) out.image16 = cropped<Image16, uint16_t>(*out.image16, r);
            else out.image8 = cropped<Image, uint8_t>(*out.image8, r);
        }
    }
    return out;
}

std::shared_ptr<Image> resampled(const std::shared_ptr<Image>& image, int width, int height, Resample r) {
    if (!image || (image->width() == width && image->height() == height)) return image;
    if (r == Resample::Nearest) return nearest<Image, uint8_t>(*image, width, height);
    const double sx = double(image->width()) / width, sy = double(image->height()) / height;
    return resampleAxisAligned(*image, width, height, sx / 2, sx, sy / 2, sy, filterOf(r));
}

std::shared_ptr<Image16> resampled(const std::shared_ptr<Image16>& image, int width, int height, Resample r) {
    if (!image || (image->width() == width && image->height() == height)) return image;
    if (r == Resample::Nearest) return nearest<Image16, uint16_t>(*image, width, height);
    const double sx = double(image->width()) / width, sy = double(image->height()) / height;
    return resampleAxisAligned(*image, width, height, sx / 2, sx, sy / 2, sy, filterOf(r));
}

Encoded encode(const Flat& flat, const Options& o, const std::vector<uint8_t>& icc, const Document* document) {
    Encoded out;
    if (flat.empty()) { out.error = QStringLiteral("nothing to export"); return out; }
    const int width = o.width > 0 ? o.width : flat.width(), height = o.height > 0 ? o.height : flat.height();
    if (!Document::validDimension(width) || !Document::validDimension(height)) { out.error = QStringLiteral("width and height must be 1..%1").arg(maxImageSide); return out; }
    std::shared_ptr<Image> image8 = resampled(flat.image8, width, height, o.resample);
    std::shared_ptr<Image16> image16 = resampled(flat.image16, width, height, o.resample);
    const bool flatten = o.format == "jpg" || !o.transparency;
    if (flatten) {
        if (image8) image8 = matted(*image8, o.matte);
        if (image16) image16 = matted(*image16, o.matte);
    }
    out.width = width;
    out.height = height;
    out.bits = image16 ? 16 : 8;
    const int quality = std::clamp(o.quality > 0 ? o.quality : defaultQuality(o.format), 1, 100);
    const std::vector<uint8_t>* profile = icc.empty() ? nullptr : &icc;
    std::vector<uint8_t> bytes;
    std::string error;
    bool ok = true;
    if (o.format == "png") ok = image16 ? encodePngImage16(*image16, bytes, o.resolution, &error, profile) : encodePngImage(*image8, bytes, o.resolution, &error, profile);
    else if (o.format == "jpg" || o.format == "webp" || o.format == "tif") {
        if (!image16 && !image8) { out.error = QStringLiteral("nothing to export"); return out; }
        const QImage image = image16 ? toQImage16(*image16) : o.format == "jpg" ? toQImage(*image8).convertToFormat(QImage::Format_RGB32) : toQImage(*image8);
        QString qerror;
        if (!encodeQt(image, o.format == "jpg" ? "jpeg" : o.format == "webp" ? "webp" : "tiff", quality, o.resolution, icc, out.bytes, qerror)) { out.error = qerror; out.bytes.clear(); }
        return out;
    } else if (o.format == "tga") ok = encodeTgaImage(*image8, bytes, &error);
    else if (o.format == "gif") {
        const std::vector<GifEncodeFrame> frames{GifEncodeFrame{image8.get(), 100}};
        bytes = encodeGif(frames, 1, &error);
        ok = !bytes.empty();
    } else if (o.format == "ico") ok = encodeIco(*image8, defaultIcoSizes, bytes, &error, document);
    else { out.error = QStringLiteral("unknown format %1").arg(o.format); return out; }
    if (!ok) { out.error = QString::fromStdString(error.empty() ? std::string("couldn't encode the image") : error); return out; }
    out.bytes = QByteArray(reinterpret_cast<const char*>(bytes.data()), qsizetype(bytes.size()));
    return out;
}

QString sizeText(qint64 bytes, bool estimate) {
    const double kb = double(bytes) / 1024;
    if (kb >= 1024) {
        const QString mb = QString::number(kb / 1024, 'f', 1);
        return estimate ? QCoreApplication::translate("ExportAs", "about %1 MB").arg(mb) : QCoreApplication::translate("ExportAs", "%1 MB").arg(mb);
    }
    const QString k = QString::number(std::max(1, int(std::lround(kb))));
    return estimate ? QCoreApplication::translate("ExportAs", "about %1 KB").arg(k) : QCoreApplication::translate("ExportAs", "%1 KB").arg(k);
}

} // namespace app::exportas
