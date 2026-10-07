// File > Export > Export As and Quick Export, and Layer > Export As (docs/features.md): the flat formats written from
// the composite (or one layer's pixels) at a chosen size, with each format's settings remembered. The pixel steps here
// are shared by document.export, which every export path commits through, and by the dialog's preview and file-size
// estimate, so the estimate is the bytes the file will hold.
#pragma once
#include "ColorManagement.h"
#include "compositor/document.h"
#include "compositor/imaget.h"
#include <QByteArray>
#include <QColor>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <memory>
#include <optional>
#include <string>

namespace app::exportas {

/// How a size change resamples the flattened image, as in Photoshop's Export As.
enum class Resample { Bicubic, Bilinear, Nearest, Lanczos };
/// "bicubic", "bilinear", "nearest", "lanczos".
QString resampleKey(Resample r);
/// By key, any case; "nearestNeighbor" and "nearest-neighbour" too. Nullopt for an unknown name.
std::optional<Resample> resampleNamed(const QString& name);
QString resampleLabel(Resample r);

/// The formats Export As offers, by key (png, jpg, gif, webp, tif, tga): WebP and TIFF only where Qt writes them.
QStringList formats();
QString formatLabel(const QString& format);
/// "jpg" for jpeg, "tif" for tiff; the key for the rest. Empty for a format Export As does not write.
QString formatKey(const QString& suffixOrName);
bool hasQuality(const QString& format);
/// Whether the format can keep transparency (all but JPEG).
bool hasTransparency(const QString& format);
int defaultQuality(const QString& format);

/// What is remembered per format, in QSettings under export/<format>/.
struct Settings {
    int quality = 85;
    bool transparency = true;
    QColor matte = Qt::white;
    Resample resample = Resample::Bicubic;
    bool convertToSrgb = true;
    bool embedProfile = true;
};
Settings remembered(const QString& format);
void remember(const QString& format, const Settings& settings);
/// The format Export As opened with last (export/lastFormat).
QString lastFormat();
void setLastFormat(const QString& format);
/// Quick Export's format (Preferences; export/quickFormat), PNG by default.
QString quickExportFormat();
void setQuickExportFormat(const QString& format);

/// document.export's parameters for these settings (the path and overwrite are the caller's). width and height 0 keep
/// the source's size; `layer` (an id or "active") exports that layer alone.
QJsonObject commandParams(const QString& format, const Settings& settings, int width = 0, int height = 0, const QString& layer = {});

// ---- the pixels ------------------------------------------------------------------------------------------------

/// The document of one layer for Layer > Export As: the layer (with its contents when it is a folder) alone,
/// visible, at the top level, over a transparent canvas of the document's size, mode, depth and profile.
std::optional<compositor::Document> layerDocument(const compositor::Document& document, const compositor::Uuid& id, std::string* error);

/// The composite flattened for a format, at full size: 16 bits where the format keeps them (PNG, TIFF when Qt writes
/// 16-bit TIFF) and the document is deeper than 8, else 8; under `plan`. `trim` crops it to its visible pixels.
struct Flat {
    std::shared_ptr<compositor::Image> image8;
    std::shared_ptr<compositor::Image16> image16;
    int width() const { return image16 ? image16->width() : image8 ? image8->width() : 0; }
    int height() const { return image16 ? image16->height() : image8 ? image8->height() : 0; }
    bool empty() const { return width() <= 0 || height() <= 0; }
};
bool keepsSixteenBits(const QString& format, const compositor::Document& document);
Flat flatten(const compositor::Document& document, const QString& format, const color::ExportPlan& plan, bool trim);

/// `image` at width × height with `r` (the image's edges kept opaque; no change at its own size).
std::shared_ptr<compositor::Image> resampled(const std::shared_ptr<compositor::Image>& image, int width, int height, Resample r);
std::shared_ptr<compositor::Image16> resampled(const std::shared_ptr<compositor::Image16>& image, int width, int height, Resample r);

/// What an export writes, encoded: the file's bytes.
struct Encoded {
    QByteArray bytes;
    int width = 0, height = 0, bits = 8;
    QString error;
    bool ok() const { return error.isEmpty() && !bytes.isEmpty(); }
};
struct Options {
    QString format;           // a key from formats(), or "ico"
    int quality = -1;         // -1: the format's default
    bool transparency = true; // false: flattened onto `matte` (JPEG always is)
    QColor matte = Qt::white;
    int width = 0, height = 0;   // 0: the flat image's size
    Resample resample = Resample::Bicubic;
    double resolution = 72;
};
/// Scales, mattes and encodes `flat`. `icc` is embedded where the format carries a profile (PNG, JPEG, WebP, TIFF);
/// `document` gives an icon its exact-size layers.
Encoded encode(const Flat& flat, const Options& options, const std::vector<uint8_t>& icc, const compositor::Document* document = nullptr);

/// "about 1.2 MB", "34 KB", in the interface's words.
QString sizeText(qint64 bytes, bool estimate);

} // namespace app::exportas
