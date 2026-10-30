// Images arriving from elsewhere: what a drop or the clipboard offers (files, an image's bytes, a web address), the
// file type an image's bytes are (by their first bytes, never by a name), and fetching an image from the web without
// blocking. MainWindowIncoming.cpp decides where the image goes.
//
// What the browsers put on a drag of an image (on X11 and Wayland alike; Qt hands them over as QMimeData):
// - Firefox: text/x-moz-url (UTF-16: the address, a newline, the title), text/uri-list (the image's address, or the
//   link's for an image inside a link), text/html (an <img src> inside the link's <a>), text/plain (the address), and
//   the decoded image again as image/png (and other image/* types) through GTK. Its file promise (XdndDirectSave) is
//   not something Qt reads.
// - Chromium and Chrome: text/uri-list and text/plain (the address), text/html (<img src="...">), _NETSCAPE_URL, and
//   on X11 a file promise (XdndDirectSave) Qt does not read; no image bytes, so the address is fetched.
// - Copy Image (either browser) puts image/png on the clipboard with text/html; a file manager's Copy puts file: URLs.
// A file a browser already saved (a download dragged from its list) arrives as a file: URL.
#pragma once
#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <functional>

class QMimeData;
class QObject;

namespace app::incoming {

/// What can open from a drop or the clipboard, the best of what was offered.
struct Offer {
    enum class Kind { None, Files, Bytes, Url };
    Kind kind = Kind::None;
    /// Local files (file: URLs): opened as a drop from a file manager opens them.
    QStringList files;
    /// An image file's bytes: image/png and the other image/* types, a data: URL decoded, or a QImage encoded as PNG
    /// (transparency and any colour space kept).
    QByteArray bytes;
    /// An http or https address to fetch (MainWindowIncoming.cpp checks the bytes when they arrive).
    QUrl url;
    /// What to call the document or layer: the address's file name without its extension, else empty.
    QString name;
    /// Why nothing could be taken (a data: URL that is not an image, say), in the reader's language.
    QString error;
};

/// The best offer in `mime`: local files, then an image's bytes (what the browser decoded keeps the pixels exactly),
/// then an address (an <img src> in the HTML before the URL list, which is a link's address for a linked image, then
/// text/x-moz-url, then plain text that is one address). None when nothing in it is an image or a place to fetch one.
Offer readOffer(const QMimeData* mime);
/// Whether `mime` may hold an image, from its formats alone (a drag's enter event, before any bytes are read).
bool mayOffer(const QMimeData* mime);
/// The file suffix for the image `bytes` start with ("png", "jpg", "gif", "webp", "bmp", "tif", "psd", "svg", "avif",
/// "heic", "jxl", "ico", "pdf"), or empty when they are not one NekoPhoto opens. Decided by content alone.
QString sniffSuffix(const QByteArray& bytes);
/// A data: URL's bytes (base64 or percent-encoded); false with `error` when it is not one.
bool decodeDataUrl(const QUrl& url, QByteArray* bytes, QString* error);
/// The name an address gives a document: its last path part without the extension, made safe for a file name.
QString nameFromUrl(const QUrl& url);

/// The most bytes a download may bring (200 MB); a test sets less.
qint64 downloadCap();
void setDownloadCap(qint64 bytes);

/// Fetches `url` (http or https) on the event loop: redirects followed (never from HTTPS to HTTP), refused past
/// downloadCap() bytes (at once when the server says the length, else as the bytes arrive), given up after 30 seconds
/// with no data. `progress` hears the bytes so far and the total (-1 when unknown); `done` gets the body, or an error in
/// the reader's language. The download ends with `context`.
void download(const QUrl& url, QObject* context, std::function<void(qint64, qint64)> progress, std::function<void(const QByteArray&, const QString&)> done);

} // namespace app::incoming
