// Images arriving from elsewhere: dropped on the window (files, a browser's image or its address) or pasted with no
// document open. What the drop or the clipboard offers is read by IncomingImage.h; this decides where it goes:
// - no document open, or a blank one untouched since File > New (EditorSession::isUntouchedBlank): it opens as a
//   document of its own, as File > Open would open it, the blank one closing in its favour;
// - a document worked on: a drop places it as a layer where it was dropped (a project or layered file in its own tab,
//   as a dropped file always did), and Edit > Paste pastes pixels as before (EditorSessionPixels.cpp);
// - dropped on the tab strip: always a document of its own.
// An image's bytes (from the browser, a data: URL, a download) are written to a file of the type their first bytes
// say, named after the address, and opened through the same readers as a file; the document keeps no link to that file
// and counts as unsaved, since it exists nowhere else.
#include "MainWindow.h"
#include "EditorSession.h"
#include "DocumentSource.h"
#include "Theme.h"
#include <QApplication>
#include <QClipboard>
#include <QDir>
#include <QFile>
#include <QStatusBar>

namespace app {

void MainWindow::pasteAsDocument() {
    const incoming::Offer offer = incoming::readOffer(QApplication::clipboard()->mimeData());
    if (offer.kind == incoming::Offer::Kind::None) { incomingFailed(offer.error.isEmpty() ? tr("The clipboard holds no image.") : offer.error); return; }
    receiveOffer(offer, Incoming::Paste, std::nullopt);
}

void MainWindow::receiveOffer(const incoming::Offer& offer, Incoming how, std::optional<QPointF> at) {
    switch (offer.kind) {
    case incoming::Offer::Kind::None: return;
    case incoming::Offer::Kind::Files:
        // The first file opens (or replaces a blank document); the rest follow it as a drop onto that document does.
        for (const QString& path : offer.files) openIncomingFile(path, how, at, false);
        return;
    case incoming::Offer::Kind::Bytes:
        openIncomingBytes(offer.bytes, offer.name, how, at, how == Incoming::Paste ? tr("The clipboard’s contents") : tr("The dropped image"));
        return;
    case incoming::Offer::Kind::Url: break;
    }
    // Fetched without blocking: the window stays usable, the status bar shows the progress, and the image goes to the
    // tab it was dropped on (by the rules above as they stand when it arrives).
    const QUrl url = offer.url;
    const QString host = url.host().isEmpty() ? url.toDisplayString() : url.host();
    QPointer<EditorSession> target(session_);
    incomingPending_++;
    QApplication::setOverrideCursor(themedCursor(Qt::BusyCursor));
    statusBar()->showMessage(tr("Downloading the image from %1…").arg(host));
    incoming::download(url, this,
        [this, host](qint64 received, qint64 total) {
            statusBar()->showMessage(total > 0 ? tr("Downloading the image from %1… %2%").arg(host).arg(received * 100 / total)
                                               : tr("Downloading the image from %1… %2 KB").arg(host).arg(received / 1000));
        },
        [this, target, how, at, url, name = offer.name](const QByteArray& bytes, const QString& error) mutable {
            incomingPending_--;
            QApplication::restoreOverrideCursor();
            statusBar()->clearMessage();
            if (!error.isEmpty()) { incomingFailed(tr("Couldn’t download %1: %2").arg(url.toDisplayString(QUrl::RemoveUserInfo), error)); return; }
            bool found = false;
            for (int i = 0; target && i < tabCount(); i++)
                if (sessionAt(i) == target) { if (i != current_) switchTo(i); found = true; }
            if (!found) at.reset();   // its tab was closed: the drop point means nothing in this one
            openIncomingBytes(bytes, name, how, at, url.toDisplayString(QUrl::RemoveUserInfo));
        });
}

void MainWindow::openIncomingBytes(const QByteArray& bytes, const QString& name, Incoming how, std::optional<QPointF> at, const QString& what) {
    // The type by content alone: an address ending in .jpg may serve a WebP, or a page.
    const QString suffix = incoming::sniffSuffix(bytes);
    if (suffix.isEmpty()) { incomingFailed(tr("%1 is not an image NekoPhoto opens.").arg(what)); return; }
    if (!incomingDir_) incomingDir_ = std::make_unique<QTemporaryDir>(QDir::tempPath() + QStringLiteral("/nekophoto-incoming-XXXXXX"));
    // A folder each, so two drops of the same name never meet; the file's name becomes the document's or layer's.
    const QString folder = incomingDir_->filePath(QString::number(++incomingCount_));
    const QString base = name.isEmpty() ? (how == Incoming::Paste ? tr("Pasted") : tr("Dropped Image")) : name;
    const QString path = folder + QLatin1Char('/') + base + QLatin1Char('.') + suffix;
    QFile file(path);
    if (!incomingDir_->isValid() || !QDir().mkpath(folder) || !file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size()) {
        incomingFailed(tr("Couldn’t write the image to %1.").arg(QDir::tempPath()));
        return;
    }
    file.close();
    openIncomingFile(path, how, at, true);
    QFile::remove(path);
    QDir().rmdir(folder);
}

void MainWindow::openIncomingFile(const QString& path, Incoming how, std::optional<QPointF> at, bool transient) {
    EditorSession* before = session_;
    const bool hadDocument = session_->hasDocument();
    const bool replaces = hadDocument && session_->isUntouchedBlank();
    suppressRecent_ = transient;
    if (!hadDocument || replaces || how == Incoming::NewDocument) openAsDocument(path);   // a blank one's tab holds a document, so a new tab
    else if (how == Incoming::Paste) importFile(path);   // a paste that arrived after its document was worked on: in the middle
    else dropFile(path, at, false);
    suppressRecent_ = false;
    const bool opened = session_ != before || (!hadDocument && session_->hasDocument());
    if (!opened) return;
    EditorSession* now = session_;
    if (transient) {
        // Nothing on disk to revert to or to list in Open Recent, and unsaved until saved somewhere.
        now->setSource({});
        now->markUnsaved();
    }
    if (replaces && now != before) {
        // The blank document gives way, as if the image had been opened instead of it.
        for (int i = 0; i < tabCount(); i++)
            if (sessionAt(i) == before && before->isUntouchedBlank()) { closeTabAt(i); break; }
        for (int i = 0; i < tabCount(); i++)
            if (sessionAt(i) == now) { switchTo(i); break; }
    }
}

void MainWindow::incomingFailed(const QString& message) {
    lastIncomingError_ = message;
    statusBar()->showMessage(message, 10000);
}

} // namespace app
