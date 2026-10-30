// What a drop or the clipboard offers, an image's type by its bytes, and fetching one from the web (IncomingImage.h).
#include "IncomingImage.h"
#include <QBuffer>
#include <algorithm>
#include <QCoreApplication>
#include <QFileInfo>
#include <QImage>
#include <QMimeData>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QRegularExpression>
#include <cstring>
#include <memory>
#include <string_view>

using namespace std::literals;

namespace app::incoming {

namespace {

qint64 cap = qint64(200) * 1000 * 1000;

bool fetchable(const QUrl& url) {
    const QString scheme = url.scheme().toLower();
    return url.isValid() && (scheme == QLatin1String("http") || scheme == QLatin1String("https") || scheme == QLatin1String("data"));
}

/// The first <img src> of a page fragment, with the few entities an address carries decoded.
QUrl imageInHtml(const QString& html) {
    static const QRegularExpression img(QStringLiteral(R"re(<img\b[^>]*?\ssrc\s*=\s*(?:"([^"]*)"|'([^']*)'|([^\s>]+)))re"), QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch m = img.match(html);
    if (!m.hasMatch()) return {};
    QString src = !m.captured(1).isEmpty() ? m.captured(1) : !m.captured(2).isEmpty() ? m.captured(2) : m.captured(3);
    src.replace(QLatin1String("&amp;"), QLatin1String("&")).replace(QLatin1String("&quot;"), QLatin1String("\"")).replace(QLatin1String("&#39;"), QLatin1String("'"));
    return QUrl(src.trimmed());
}

/// Firefox's text/x-moz-url: UTF-16 (little-endian here, a byte-order mark optional), the address then the title.
QUrl mozUrl(const QByteArray& data) {
    if (data.size() < 2) return {};
    QString text;
    if (static_cast<unsigned char>(data[0]) == 0xfe && static_cast<unsigned char>(data[1]) == 0xff) {
        for (qsizetype i = 2; i + 1 < data.size(); i += 2) text += QChar(char16_t((static_cast<unsigned char>(data[i]) << 8) | static_cast<unsigned char>(data[i + 1])));
    } else {
        const qsizetype start = static_cast<unsigned char>(data[0]) == 0xff && static_cast<unsigned char>(data[1]) == 0xfe ? 2 : 0;
        for (qsizetype i = start; i + 1 < data.size(); i += 2) text += QChar(char16_t(static_cast<unsigned char>(data[i]) | (static_cast<unsigned char>(data[i + 1]) << 8)));
    }
    return QUrl(text.section(QLatin1Char('\n'), 0, 0).trimmed());
}

/// The address to fetch the image from, if the drop names one.
QUrl addressIn(const QMimeData* mime) {
    // A linked image's <img src> is the image; the URL list then holds the link's address.
    if (mime->hasHtml())
        if (const QUrl url = imageInHtml(mime->html()); fetchable(url)) return url;
    for (const QUrl& url : mime->urls())
        if (fetchable(url)) return url;
    if (mime->hasFormat(QStringLiteral("text/x-moz-url")))
        if (const QUrl url = mozUrl(mime->data(QStringLiteral("text/x-moz-url"))); fetchable(url)) return url;
    if (mime->hasText()) {
        const QString text = mime->text().trimmed();
        if (!text.contains(QRegularExpression(QStringLiteral("\\s"))))
            if (const QUrl url(text); fetchable(url)) return url;
    }
    return {};
}

} // namespace

QString sniffSuffix(const QByteArray& bytes) {
    auto at = [&](qsizetype offset, std::string_view signature) {
        return bytes.size() >= offset + qsizetype(signature.size()) && std::memcmp(bytes.constData() + offset, signature.data(), signature.size()) == 0;
    };
    if (at(0, "\x89PNG\r\n\x1a\n"sv)) return QStringLiteral("png");
    if (at(0, "\xff\xd8\xff"sv)) return QStringLiteral("jpg");
    if (at(0, "GIF87a"sv) || at(0, "GIF89a"sv)) return QStringLiteral("gif");
    if (at(0, "RIFF"sv) && at(8, "WEBP"sv)) return QStringLiteral("webp");
    if (at(0, "II*\0"sv) || at(0, "MM\0*"sv)) return QStringLiteral("tif");
    if (at(0, "8BPS"sv)) return QStringLiteral("psd");
    if (at(0, "%PDF-"sv)) return QStringLiteral("pdf");
    if (at(0, "\xff\x0a"sv) || at(0, "\0\0\0\x0cJXL \r\n\x87\n"sv)) return QStringLiteral("jxl");
    if (at(4, "ftyp"sv) && bytes.size() >= 16) {
        // ISO media: AVIF names itself among the brands; HEIF's brands otherwise.
        const QByteArray box = bytes.mid(8, 32);
        if (box.contains("avif") || box.contains("avis")) return QStringLiteral("avif");
        for (const char* brand : {"heic", "heix", "hevc", "heim", "heis", "mif1", "msf1"})
            if (box.startsWith(brand)) return QStringLiteral("heic");
    }
    if (at(0, "BM"sv) && bytes.size() >= 18) {
        // The info header's size is one of the few Windows and OS/2 define, which plain text starting "BM" is not.
        const quint32 header = quint32(static_cast<unsigned char>(bytes[14])) | quint32(static_cast<unsigned char>(bytes[15])) << 8
                             | quint32(static_cast<unsigned char>(bytes[16])) << 16 | quint32(static_cast<unsigned char>(bytes[17])) << 24;
        if (header == 12 || header == 40 || header == 52 || header == 56 || header == 64 || header == 108 || header == 124) return QStringLiteral("bmp");
    }
    if ((at(0, "\0\0\1\0"sv) || at(0, "\0\0\2\0"sv)) && bytes.size() >= 22) {
        const int count = static_cast<unsigned char>(bytes[4]) | static_cast<unsigned char>(bytes[5]) << 8;
        if (count >= 1 && count <= 256) return bytes[2] == 1 ? QStringLiteral("ico") : QStringLiteral("cur");
    }
    // SVG: markup whose first element is <svg (after an XML declaration, comments or a doctype).
    QByteArray head = bytes.left(4096);
    if (head.startsWith("\xef\xbb\xbf")) head.remove(0, 3);
    head = head.trimmed().toLower();
    if (head.startsWith('<')) {
        const qsizetype svg = head.indexOf("<svg");
        const qsizetype html = head.indexOf("<html");
        if (svg >= 0 && (html < 0 || svg < html)) return QStringLiteral("svg");
    }
    return {};
}

bool decodeDataUrl(const QUrl& url, QByteArray* bytes, QString* error) {
    const QByteArray encoded = url.toEncoded();
    const qsizetype comma = encoded.indexOf(',');
    if (!encoded.toLower().startsWith("data:") || comma < 0) { if (error) *error = QCoreApplication::translate("IncomingImage", "The address is not a data: URL."); return false; }
    const QByteArray meta = encoded.mid(5, comma - 5).toLower();
    const QByteArray payload = QByteArray::fromPercentEncoding(encoded.mid(comma + 1));
    if (meta.endsWith(";base64")) {
        const auto decoded = QByteArray::fromBase64Encoding(payload, QByteArray::AbortOnBase64DecodingErrors);
        if (!decoded) { if (error) *error = QCoreApplication::translate("IncomingImage", "The data: URL’s base64 is damaged."); return false; }
        *bytes = *decoded;
    } else *bytes = payload;
    return true;
}

QString nameFromUrl(const QUrl& url) {
    if (url.scheme().toLower() == QLatin1String("data")) return {};
    QString name = QFileInfo(QFileInfo(url.path()).fileName()).completeBaseName();
    static const QRegularExpression unsafe(QStringLiteral(R"([\\/:*?"<>|\x00-\x1f])"));
    name.replace(unsafe, QStringLiteral("_"));
    return name.trimmed().left(80);
}

qint64 downloadCap() { return cap; }
void setDownloadCap(qint64 bytes) { cap = bytes; }

Offer readOffer(const QMimeData* mime) {
    Offer offer;
    if (!mime) return offer;
    for (const QUrl& url : mime->urls())
        if (url.isLocalFile()) offer.files << url.toLocalFile();
    if (!offer.files.isEmpty()) { offer.kind = Offer::Kind::Files; return offer; }
    const QUrl address = addressIn(mime);
    const QString named = fetchable(address) ? nameFromUrl(address) : QString();
    // The bytes of an image, PNG first (what a browser decodes an image to: exact, with its transparency).
    QStringList formats;
    for (const QString& format : mime->formats())
        if (format.startsWith(QLatin1String("image/"), Qt::CaseInsensitive)) formats << format;
    std::stable_partition(formats.begin(), formats.end(), [](const QString& f) { return f.compare(QLatin1String("image/png"), Qt::CaseInsensitive) == 0; });
    for (const QString& format : formats) {
        const QByteArray bytes = mime->data(format);
        if (sniffSuffix(bytes).isEmpty()) continue;
        offer.kind = Offer::Kind::Bytes;
        offer.bytes = bytes;
        offer.name = named;
        return offer;
    }
    if (mime->hasImage()) {
        const QImage image = qvariant_cast<QImage>(mime->imageData());
        QBuffer buffer(&offer.bytes);
        if (!image.isNull() && buffer.open(QIODevice::WriteOnly) && image.save(&buffer, "PNG")) {
            offer.kind = Offer::Kind::Bytes;
            offer.name = named;
            return offer;
        }
        offer.bytes.clear();
    }
    if (!fetchable(address)) return offer;
    if (address.scheme().toLower() == QLatin1String("data")) {
        QByteArray bytes;
        if (!decodeDataUrl(address, &bytes, &offer.error)) return offer;
        if (sniffSuffix(bytes).isEmpty()) { offer.error = QCoreApplication::translate("IncomingImage", "The data: URL does not hold an image NekoPhoto opens."); return offer; }
        offer.kind = Offer::Kind::Bytes;
        offer.bytes = bytes;
        return offer;
    }
    offer.kind = Offer::Kind::Url;
    offer.url = address;
    offer.name = named;
    return offer;
}

bool mayOffer(const QMimeData* mime) {
    if (!mime) return false;
    if (mime->hasUrls() || mime->hasImage() || mime->hasHtml() || mime->hasFormat(QStringLiteral("text/x-moz-url"))) return true;
    for (const QString& format : mime->formats())
        if (format.startsWith(QLatin1String("image/"), Qt::CaseInsensitive)) return true;
    // Text only when it is one address (a link dragged from a text field, say).
    if (mime->hasText()) {
        const QString text = mime->text().trimmed();
        return !text.contains(QRegularExpression(QStringLiteral("\\s"))) && fetchable(QUrl(text));
    }
    return false;
}

void download(const QUrl& url, QObject* context, std::function<void(qint64, qint64)> progress, std::function<void(const QByteArray&, const QString&)> done) {
    auto* manager = new QNetworkAccessManager(context);
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setMaximumRedirectsAllowed(10);
    request.setTransferTimeout(30000);   // no data for this long: the site is down or the connection gone
    request.setRawHeader("Accept", "image/avif,image/webp,image/png,image/*;q=0.9,*/*;q=0.5");
    QNetworkReply* reply = manager->get(request);
    const qint64 limit = cap;
    const QString tooLarge = QCoreApplication::translate("IncomingImage", "The image is larger than %1 MB, the most a drop or a paste downloads.").arg(double(limit) / 1e6, 0, 'g', 3);
    auto body = std::make_shared<QByteArray>();
    auto refused = std::make_shared<QString>();
    QObject::connect(reply, &QNetworkReply::metaDataChanged, reply, [reply, limit, tooLarge, refused] {
        const QVariant length = reply->header(QNetworkRequest::ContentLengthHeader);
        if (refused->isEmpty() && length.isValid() && length.toLongLong() > limit) { *refused = tooLarge; reply->abort(); }
    });
    QObject::connect(reply, &QNetworkReply::readyRead, reply, [reply, limit, tooLarge, body, refused] {
        if (!refused->isEmpty()) return;
        body->append(reply->readAll());
        if (body->size() > limit) { *refused = tooLarge; body->clear(); reply->abort(); }
    });
    if (progress) QObject::connect(reply, &QNetworkReply::downloadProgress, reply, [progress](qint64 received, qint64 total) { progress(received, total > 0 ? total : -1); });
    QPointer<QObject> alive(context);
    QObject::connect(reply, &QNetworkReply::finished, reply, [reply, manager, body, refused, alive, done] {
        QString error = *refused;
        if (error.isEmpty() && reply->error() == QNetworkReply::OperationCanceledError) error = QCoreApplication::translate("IncomingImage", "The server sent nothing for 30 seconds.");
        else if (error.isEmpty() && reply->error() != QNetworkReply::NoError) error = reply->errorString();
        if (error.isEmpty()) body->append(reply->readAll());
        reply->deleteLater();
        manager->deleteLater();
        if (alive) done(error.isEmpty() ? *body : QByteArray(), error);
    });
}

} // namespace app::incoming
