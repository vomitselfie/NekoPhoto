// incoming: images from elsewhere (MainWindowIncoming.cpp, IncomingImage.h). Edit > Paste with no document opens the
// clipboard's image as a document (sized to it, transparency and colour profile kept); onto a blank document untouched
// since File > New it replaces it, onto one worked on it pastes a layer. Drops of image bytes, a file: URL, an http
// address (served here by a small QTcpServer, never the network), a data: URL and a linked image's <img src> open the
// same way; a missing page, a page that is not an image, junk bytes and a download past the size cap change nothing
// and say why. The drops are QMimeData shaped as Firefox and Chromium offer them (IncomingImage.h).
#include "SelfTestSupport.h"
#include "ColorManagement.h"
#include "DocumentSource.h"
#include "EditorSession.h"
#include "IncomingImage.h"
#include "MainWindow.h"
#include "CanvasWidget.h"
#include <QAction>
#include <QApplication>
#include <QBuffer>
#include <QClipboard>
#include <QColorSpace>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QMimeData>
#include <QNetworkProxy>
#include <QNetworkProxyFactory>
#include <QSettings>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <array>
#include <cmath>
#include <cstdio>
#include <memory>

using namespace nekophoto;

namespace app {

using namespace selftest;

namespace {

QByteArray pngBytes(const QImage& image) {
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    return bytes;
}

/// The 8-bit pixel (premultiplied RGBA) of the document's bottom layer at x, y.
std::array<int, 4> pixelAt(EditorSession& s, int x, int y) {
    if (!s.hasDocument() || s.document()->layers.empty() || !s.document()->layers.front().asset) return {-1, -1, -1, -1};
    const auto& image = s.document()->layers.front().asset->image.u8();
    if (!image || x >= image->width() || y >= image->height()) return {-1, -1, -1, -1};
    const uint8_t* p = image->pixel(x, y);
    return {p[0], p[1], p[2], p[3]};
}

} // namespace

int incomingImages(MainWindow& w) {
    int failures = 0;
    auto expect = [&](bool ok, const QString& what) { if (!ok) { std::fprintf(stderr, "incoming: %s\n", qPrintable(what)); failures++; } };
    QString errors;
    w.setErrorSink(&errors);
    w.resize(1200, 800);
    w.show();
    QApplication::processEvents();
    // No proxy from the environment between the test and its own server.
    QNetworkProxyFactory::setUseSystemConfiguration(false);
    QNetworkProxy::setApplicationProxy(QNetworkProxy::NoProxy);
    {
        color::Settings quiet = color::settings();
        quiet.askMissing = quiet.askMismatch = false;
        color::setSettings(quiet);
    }

    // 30x20: transparent but for a half-opaque green corner and an opaque red right half, tagged Display P3.
    QImage image(30, 20, QImage::Format_ARGB32);
    image.fill(Qt::transparent);
    for (int y = 0; y < 20; y++) for (int x = 15; x < 30; x++) image.setPixel(x, y, qRgba(255, 0, 0, 255));
    image.setPixel(0, 0, qRgba(0, 255, 0, 128));
    QImage tagged = image;
    tagged.setColorSpace(QColorSpace(QColorSpace::DisplayP3));
    const QByteArray png = pngBytes(tagged);
    QImage noise(160, 160, QImage::Format_ARGB32);
    for (int y = 0; y < 160; y++) for (int x = 0; x < 160; x++) noise.setPixel(x, y, qRgba((x * 7919 + y * 104729) % 251, (x * x + y * 31) % 253, (x ^ y) * 3 % 255, 255));
    const QByteArray big = pngBytes(noise);

    // The server: /photo.png, /redirect (to it), /noext (a PNG as octet-stream), /page.png (HTML), /missing (404),
    // /big and /big-stream (a large PNG with and without a Content-Length).
    QTcpServer server;
    if (!server.listen(QHostAddress::LocalHost)) { std::fprintf(stderr, "incoming: the test server could not listen\n"); return 1; }
    const QString base = QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort());
    int requests = 0;
    auto respond = [&](const QByteArray& path) -> QByteArray {
        requests++;
        auto reply = [](const QByteArray& status, const QByteArray& type, const QByteArray& body, bool length = true, const QByteArray& extra = {}) {
            QByteArray r = "HTTP/1.1 " + status + "\r\nContent-Type: " + type + "\r\nConnection: close\r\n" + extra;
            if (length) r += "Content-Length: " + QByteArray::number(body.size()) + "\r\n";
            return r + "\r\n" + body;
        };
        if (path == "/photo.png") return reply("200 OK", "image/png", png);
        if (path == "/noext") return reply("200 OK", "application/octet-stream", png);
        if (path == "/redirect") return reply("302 Found", "text/plain", "", true, "Location: /photo.png\r\n");
        if (path == "/page.png") return reply("200 OK", "text/html", "<html><body>Not an image</body></html>");
        if (path == "/big") return reply("200 OK", "image/png", big);
        if (path == "/big-stream") return reply("200 OK", "image/png", big, false);
        return reply("404 Not Found", "text/plain", "missing");
    };
    QObject::connect(&server, &QTcpServer::newConnection, &server, [&] {
        while (QTcpSocket* socket = server.nextPendingConnection()) {
            auto request = std::make_shared<QByteArray>();
            QObject::connect(socket, &QTcpSocket::readyRead, socket, [socket, request, &respond] {
                request->append(socket->readAll());
                if (!request->contains("\r\n\r\n")) return;
                socket->write(respond(request->split(' ').value(1).split('?').value(0)));
                socket->disconnectFromHost();
            });
            QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        }
    });
    auto settle = [&] {
        QElapsedTimer timer;
        timer.start();
        while (w.incomingPending() > 0 && timer.elapsed() < 15000) QApplication::processEvents(QEventLoop::AllEvents, 20);
        QApplication::processEvents();
        return w.incomingPending() == 0;
    };
    // Every tab closed: the window's empty state, one tab with no document.
    auto reset = [&] {
        while (w.tabCount() > 1) w.closeTabAt(w.tabCount() - 1);
        w.closeTabAt(0);
        QApplication::processEvents();
        errors.clear();
        w.takeIncomingError();
    };
    auto newDocument = [&](int width, int height) {
        w.runCommand("document.new", {{"width", width}, {"height", height}});
        QApplication::processEvents();
    };
    QAction* paste = action(w, "command.pixels.paste");
    if (!paste) return 1;
    auto pasteNow = [&](QMimeData* mime) {
        QApplication::clipboard()->setMimeData(mime);
        QApplication::processEvents();
        if (!paste->isEnabled()) std::fprintf(stderr, "incoming: Edit > Paste is greyed\n");
        paste->trigger();
        QApplication::processEvents();
    };
    // A drop over the middle of the canvas, entered first as a real drag is.
    auto drop = [&](QMimeData* mime) {
        CanvasWidget* canvas = w.canvasAt(w.currentTabIndex());
        const QPoint at = canvas->mapTo(&w, canvas->rect().center());
#if QT_VERSION >= QT_VERSION_CHECK(6, 12, 0)
        QDragEnterEvent enter(QPointF(at), Qt::CopyAction, mime, Qt::LeftButton, Qt::NoModifier);   // the QPoint one is deprecated
#else
        QDragEnterEvent enter(at, Qt::CopyAction, mime, Qt::LeftButton, Qt::NoModifier);
#endif
        QApplication::sendEvent(&w, &enter);
        QDropEvent event(QPointF(at), Qt::CopyAction, mime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(&w, &event);
        QApplication::processEvents();
        delete mime;
    };
    auto mimeWith = [](const QString& format, const QByteArray& data) { auto* m = new QMimeData; m->setData(format, data); return m; };
    auto urls = [](const QList<QUrl>& list) { auto* m = new QMimeData; m->setUrls(list); return m; };
    auto opened = [&](const char* what, int width, int height, const QString& title) {
        EditorSession& s = *w.session();
        const bool ok = w.tabCount() == 1 && s.hasDocument() && s.document()->width == width && s.document()->height == height && s.document()->layers.size() == 1;
        expect(ok, QStringLiteral("%1: not a %2x%3 document of one layer (%4 tabs, %5x%6) %7 %8").arg(QLatin1String(what)).arg(width).arg(height).arg(w.tabCount())
                       .arg(s.hasDocument() ? s.document()->width : 0).arg(s.hasDocument() ? s.document()->height : 0).arg(w.takeIncomingError(), errors));
        expect(title.isEmpty() || w.tabTitle(w.currentTabIndex()) == title, QStringLiteral("%1: titled %2, not %3").arg(QLatin1String(what), w.tabTitle(w.currentTabIndex()), title));
        return ok;
    };
    // Returns the error given.
    auto nothingChanged = [&](const char* what, bool hadDocument, int width = 0, int layers = 0) {
        EditorSession& s = *w.session();
        expect(w.tabCount() == 1 && s.hasDocument() == hadDocument && (!hadDocument || (s.document()->width == width && int(s.document()->layers.size()) == layers)),
               QStringLiteral("%1: the window changed").arg(QLatin1String(what)));
        const QString why = w.takeIncomingError() + errors;
        expect(!why.isEmpty(), QStringLiteral("%1: no error was given").arg(QLatin1String(what)));
        return why;
    };

    // Paste with no document: a document of the image's size, its transparency and its profile kept, unsaved.
    reset();
    expect(paste->isEnabled(), QStringLiteral("Edit > Paste is greyed with no document"));
    pasteNow(mimeWith(QStringLiteral("image/png"), png));
    if (opened("paste, no document", 30, 20, QStringLiteral("Pasted"))) {
        EditorSession& s = *w.session();
        const auto corner = pixelAt(s, 0, 0), clear = pixelAt(s, 5, 5), red = pixelAt(s, 20, 10);
        expect(std::abs(corner[3] - 128) <= 1 && clear[3] == 0 && red[3] == 255, QStringLiteral("paste: the transparency was not kept (%1, %2, %3)").arg(corner[3]).arg(clear[3]).arg(red[3]));
        expect(!s.document()->profile.empty(), QStringLiteral("paste: the clipboard's Display P3 profile was dropped"));
        expect(s.isModified() && !s.source().valid() && !s.canRevert(), QStringLiteral("paste: the document should be unsaved, with no file behind it"));
        expect(!s.isUntouchedBlank(), QStringLiteral("paste: a pasted document counts as blank"));
    }
    // A QImage alone (what Qt offers for a bitmap other apps put on the clipboard).
    reset();
    {
        auto* mime = new QMimeData;
        mime->setImageData(image);
        pasteNow(mime);
    }
    opened("paste of a QImage", 30, 20, QStringLiteral("Pasted"));
    // Nothing to paste: nothing happens, and the status bar says so.
    reset();
    pasteNow(mimeWith(QStringLiteral("text/plain"), "just words"));
    nothingChanged("paste of text", false);

    // The untouched blank document, defined: File > New's, with no history, one layer, one colour, no selection.
    reset();
    newDocument(100, 80);
    expect(w.session()->isUntouchedBlank(), QStringLiteral("File > New's document is not an untouched blank"));
    w.runCommand("pixels.fill", {{"color", "#ffffff"}});
    expect(!w.session()->isUntouchedBlank(), QStringLiteral("a filled document still counts as blank"));
    w.session()->undo();
    expect(!w.session()->isUntouchedBlank(), QStringLiteral("a document with a step to redo counts as blank"));
    reset();
    w.runCommand("document.new", {{"width", 50}, {"height", 50}, {"emptyLayer", false}});
    expect(w.session()->isUntouchedBlank(), QStringLiteral("a new document with no layer is not blank"));
    w.runCommand("selection.rect", {{"x", 0}, {"y", 0}, {"width", 10}, {"height", 10}});
    expect(!w.session()->isUntouchedBlank(), QStringLiteral("a document with a selection counts as blank"));

    // Paste onto the untouched blank: replaced, in the same tab count.
    reset();
    newDocument(100, 80);
    EditorSession* blank = w.session();
    pasteNow(mimeWith(QStringLiteral("image/png"), png));
    if (opened("paste onto a blank document", 30, 20, QStringLiteral("Pasted")))
        expect(w.session() != blank, QStringLiteral("paste onto a blank document: the blank one is still the document"));
    // Paste onto a document worked on: a layer, as before.
    reset();
    newDocument(100, 80);
    w.runCommand("pixels.fill", {{"color", "#336699"}});
    {
        // As a platform clipboard offers PNG bytes: also as an image (Qt's offscreen clipboard hands back the QMimeData
        // as it was given, where X11's and Wayland's decode image/png for imageData()).
        auto* mime = mimeWith(QStringLiteral("image/png"), png);
        mime->setImageData(tagged);
        pasteNow(mime);
    }
    expect(w.tabCount() == 1 && w.session()->document()->width == 100 && w.session()->document()->layers.size() == 2,
           QStringLiteral("paste onto an edited document: not a layer added (%1 layers)").arg(w.session()->hasDocument() ? int(w.session()->document()->layers.size()) : -1));

    // Drops. Image bytes with the address beside them, as Firefox offers: the bytes open, nothing is fetched.
    reset();
    requests = 0;
    {
        auto* mime = mimeWith(QStringLiteral("image/png"), png);
        mime->setUrls({QUrl(base + QStringLiteral("/gallery/cat-photo.png"))});
        drop(mime);
    }
    expect(w.incomingPending() == 0 && requests == 0, QStringLiteral("drop of image bytes: the address was fetched"));
    opened("drop of image bytes", 30, 20, QStringLiteral("cat-photo"));
    // A file: URL (a file manager's drag, a browser's saved download).
    reset();
    QTemporaryDir dir;
    const QString file = dir.filePath(QStringLiteral("local-file.png"));
    image.save(file);
    drop(urls({QUrl::fromLocalFile(file)}));
    if (opened("drop of a file", 30, 20, QStringLiteral("local-file")))
        expect(w.session()->source().path == file && !w.session()->isModified(), QStringLiteral("drop of a file: the document is not the file, unmodified"));
    // A file dropped on the untouched blank replaces it too.
    reset();
    newDocument(64, 64);
    drop(urls({QUrl::fromLocalFile(file)}));
    opened("drop of a file onto a blank document", 30, 20, QStringLiteral("local-file"));
    // An http address (Chromium offers no bytes): fetched, then opened, named after the address.
    reset();
    drop(urls({QUrl(base + QStringLiteral("/photo.png"))}));
    expect(settle(), QStringLiteral("drop of an address: the download never finished"));
    opened("drop of an http address", 30, 20, QStringLiteral("photo"));
    expect(w.session()->isModified() && !w.session()->source().valid(), QStringLiteral("drop of an address: the document should be unsaved, with no file behind it"));
    // Through a redirect, and an address with no extension: the bytes decide.
    reset();
    drop(urls({QUrl(base + QStringLiteral("/redirect"))}));
    settle();
    opened("drop of a redirect", 30, 20, QString());
    reset();
    drop(urls({QUrl(base + QStringLiteral("/noext"))}));
    settle();
    opened("drop of an address with no extension", 30, 20, QStringLiteral("noext"));
    // A linked image (Firefox): the URL list holds the link, the HTML the image.
    reset();
    {
        auto* mime = new QMimeData;
        mime->setUrls({QUrl(base + QStringLiteral("/page.png"))});
        mime->setHtml(QStringLiteral("<a href=\"%1/page.png\"><img alt=\"x\" src=\"%1/photo.png?size=large&amp;v=2\"></a>").arg(base));
        drop(mime);
    }
    settle();
    opened("drop of a linked image", 30, 20, QStringLiteral("photo"));
    // Firefox's text/x-moz-url alone (UTF-16, address then title).
    reset();
    {
        const QString moz = base + QStringLiteral("/photo.png\nA photo");
        QByteArray utf16(reinterpret_cast<const char*>(moz.utf16()), moz.size() * 2);
        drop(mimeWith(QStringLiteral("text/x-moz-url"), utf16));
    }
    settle();
    opened("drop of text/x-moz-url", 30, 20, QStringLiteral("photo"));
    // A data: URL, decoded without any download.
    reset();
    drop(urls({QUrl(QStringLiteral("data:image/png;base64,") + QString::fromLatin1(png.toBase64()))}));
    expect(w.incomingPending() == 0, QStringLiteral("drop of a data: URL started a download"));
    opened("drop of a data: URL", 30, 20, QStringLiteral("Dropped Image"));

    // A drop onto a document worked on: a layer where it was dropped, the address fetched first.
    reset();
    newDocument(200, 160);
    w.runCommand("pixels.fill", {{"color", "#808080"}});
    drop(urls({QUrl(base + QStringLiteral("/photo.png"))}));
    settle();
    {
        EditorSession& s = *w.session();
        const bool layered = w.tabCount() == 1 && s.document()->width == 200 && s.document()->layers.size() == 2;
        expect(layered, QStringLiteral("drop onto an edited document: no layer was added"));
        if (layered) {
            const Layer& top = s.document()->layers.back();
            CanvasWidget* canvas = w.canvasAt(w.currentTabIndex());
            const QPointF centre = canvas->documentPoint(QPointF(canvas->rect().center()));
            const double cx = top.origin().x + 15, cy = top.origin().y + 10;
            expect(std::abs(cx - centre.x()) <= 1.5 && std::abs(cy - centre.y()) <= 1.5 && QString::fromStdString(top.name) == QStringLiteral("photo"),
                   QStringLiteral("drop onto an edited document: the layer %1 sits at %2, %3, not at the drop %4, %5").arg(QString::fromStdString(top.name)).arg(cx).arg(cy).arg(centre.x()).arg(centre.y()));
        }
        // Image bytes too (once a QImage put as a layer named Dropped Image, now through the file's reader).
        drop(mimeWith(QStringLiteral("image/png"), png));
        expect(s.document()->layers.size() == 3, QStringLiteral("drop of image bytes onto an edited document: no layer was added"));
    }

    // What cannot open changes nothing and says why.
    reset();
    drop(urls({QUrl(base + QStringLiteral("/missing"))}));
    settle();
    nothingChanged("a missing address", false);
    reset();
    drop(urls({QUrl(base + QStringLiteral("/page.png"))}));
    settle();
    if (const QString why = nothingChanged("a page that is not an image", false); !why.contains(QStringLiteral("not an image")))
        expect(false, QStringLiteral("a page: the error was %1").arg(why));
    reset();
    drop(mimeWith(QStringLiteral("image/png"), "certainly not a PNG"));
    nothingChanged("junk image bytes", false);
    reset();
    drop(urls({QUrl(QStringLiteral("data:text/plain,hello"))}));
    nothingChanged("a data: URL of text", false);
    reset();
    {
        // A port nothing listens on.
        QTcpServer closed;
        closed.listen(QHostAddress::LocalHost);
        const quint16 port = closed.serverPort();
        closed.close();
        drop(urls({QUrl(QStringLiteral("http://127.0.0.1:%1/photo.png").arg(port))}));
    }
    settle();
    nothingChanged("a refused connection", false);
    // Past the size cap: refused at once by its length, or as the bytes arrive without one; an edited document too.
    const qint64 cap = incoming::downloadCap();
    incoming::setDownloadCap(big.size() / 2);
    reset();
    drop(urls({QUrl(base + QStringLiteral("/big"))}));
    settle();
    if (const QString why = nothingChanged("a download past the cap", false); !why.contains(QStringLiteral("larger")))
        expect(false, QStringLiteral("the cap: the error was %1").arg(why));
    reset();
    newDocument(100, 80);
    w.runCommand("pixels.fill", {{"color", "#ffffff"}});
    drop(urls({QUrl(base + QStringLiteral("/big-stream"))}));
    settle();
    nothingChanged("a streamed download past the cap", true, 100, 1);
    incoming::setDownloadCap(cap);
    reset();
    drop(urls({QUrl(base + QStringLiteral("/big-stream"))}));
    settle();
    opened("a streamed download within the cap", 160, 160, QStringLiteral("big-stream"));

    // Nothing written for a drop is left behind in Open Recent.
    for (const QString& recent : QSettings().value("recent").toStringList())
        expect(!recent.contains(QStringLiteral("nekophoto-incoming")), QStringLiteral("Open Recent lists %1").arg(recent));
    // Sniffing decides by content.
    expect(incoming::sniffSuffix("<?xml version=\"1.0\"?>\n<svg xmlns=\"http://www.w3.org/2000/svg\"/>") == QStringLiteral("svg"), QStringLiteral("SVG is not sniffed"));
    expect(incoming::sniffSuffix("<!DOCTYPE html><html><svg/></html>").isEmpty(), QStringLiteral("a page with an inline SVG sniffs as SVG"));
    expect(incoming::sniffSuffix(QByteArray("GIF89a") + QByteArray(10, '\0')) == QStringLiteral("gif"), QStringLiteral("GIF is not sniffed"));
    expect(incoming::sniffSuffix(QByteArray("RIFF\0\0\0\0WEBPVP8 ", 16)) == QStringLiteral("webp"), QStringLiteral("WebP is not sniffed"));
    expect(incoming::sniffSuffix(QByteArray("\0\0\0\x1c" "ftypavif\0\0\0\0avifmif1", 24)) == QStringLiteral("avif"), QStringLiteral("AVIF is not sniffed"));
    expect(incoming::sniffSuffix("BMW is a car brand, not a bitmap").isEmpty(), QStringLiteral("text starting BM sniffs as a bitmap"));

    w.setErrorSink(nullptr);
    if (failures == 0) std::printf("incoming: ok\n");
    return failures == 0 ? 0 : 1;
}

} // namespace app
