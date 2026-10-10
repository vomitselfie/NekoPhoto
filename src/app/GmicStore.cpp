#include "GmicStore.h"
#include "Gmic.h"
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTemporaryDir>
#include <QUrl>
#include <string>
#include <vector>

namespace app {

namespace {

/// One download at a time, whichever window started it: they share the part file.
int& activeDownloads() {
    static int count = 0;
    return count;
}

/// Leftovers of an unpack or a replacement that was interrupted (a crash, a power cut): never used, so removed.
void removeLeftovers(const QString& root) {
    QDir dir(root);
    for (const QString& name : dir.entryList({QStringLiteral(".unpack-*"), QStringLiteral(".old-*")}, QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot))
        QDir(dir.filePath(name)).removeRecursively();
}

QString partPath() {
    return GmicRunner::downloadRoot() + '/' + QUrl(GmicStore::pinned().url).fileName() + QStringLiteral(".part");
}

} // namespace

const GmicStore::Build& GmicStore::pinned() {
    static const Build build{GmicRunner::downloadVersion(), QStringLiteral("https://gmic.eu/files/windows/gmic_4.0.5_cli_win64.zip"),
                             QStringLiteral("3f7ae429fa9758db41dae89d3fdc048e60d5d7e820e9ff03419c5bd7a10c2d95"), 14561476,
                             QStringLiteral("gmic.exe")};
    return build;
}

bool GmicStore::offered() {
#ifdef _WIN32
    return true;
#else
    return qEnvironmentVariableIntValue("COMPOSITOR_GMIC_OFFER_DOWNLOAD") == 1;
#endif
}

bool GmicStore::available() { return GmicRunner::available(); }

bool GmicStore::downloaded() {
    const QFileInfo info(GmicRunner::downloadedDirectory() + '/' + pinned().executable);
    return info.isFile();
}

QString GmicStore::directory() { return GmicRunner::downloadRoot(); }

bool GmicStore::remove(QString* error) {
    const QString root = directory(), dest = GmicRunner::downloadedDirectory();
    if (!QFileInfo::exists(dest)) return true;
    // Moved aside first, in one step, so the lookup never finds part of a copy; on Windows this fails while
    // gmic.exe runs (a preview), and then nothing is touched.
    const QString aside = root + QStringLiteral("/.old-") + QString::number(QDateTime::currentMSecsSinceEpoch());
    if (!QDir().rename(dest, aside)) {
        if (error) *error = tr("Couldn’t remove G'MIC from %1; it may be running. Close the G'MIC dialog and try again.").arg(dest);
        return false;
    }
    QDir(aside).removeRecursively();
    return true;
}

compositor::ZipLimits GmicStore::limits() {
    compositor::ZipLimits limits;
    limits.entries = 500;
    limits.entryBytes = 128ull << 20;
    limits.totalBytes = 512ull << 20;
    return limits;
}

bool GmicStore::install(const QString& archive, const Build& build, const QString& root, QString* error, const compositor::ZipLimits& limits) {
    struct RemoveArchive {
        QString path;
        ~RemoveArchive() { QFile::remove(path); }
    } removeArchive{archive};
    auto fail = [error](const QString& why) { if (error) *error = why; return false; };

    QFile file(archive);
    if (!file.open(QIODevice::ReadOnly)) return fail(tr("Couldn’t read the downloaded file %1.").arg(archive));
    if (file.size() != build.bytes)
        return fail(tr("The download is %1 bytes, not the %2 expected, so it was not used. Try again later.").arg(file.size()).arg(build.bytes));
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&file) || QString::fromLatin1(hash.result().toHex()) != build.sha256)
        return fail(tr("The downloaded file didn’t match its expected checksum, so it was not used. Try again later."));
    file.seek(0);
    const QByteArray bytes = file.readAll();
    file.close();
    if (bytes.size() != build.bytes) return fail(tr("Couldn’t read the downloaded file %1.").arg(archive));

    const QString unreadable = tr("The downloaded G'MIC archive could not be unpacked: %1");
    compositor::ZipFileReader zip;
    std::string why;
    if (!zip.openMemory(std::vector<uint8_t>(bytes.begin(), bytes.end()), &why, limits)) return fail(unreadable.arg(QString::fromStdString(why)));
    // Everything sits in one top folder (gmic-4.0.5-cli-win64/), which is left out.
    std::string top;
    for (const compositor::ZipFileEntry& entry : zip.entries()) {
        const size_t slash = entry.name.find('/');
        if (slash == std::string::npos || slash == 0) return fail(unreadable.arg(tr("it is not laid out as expected")));
        const std::string first = entry.name.substr(0, slash);
        if (top.empty()) top = first;
        else if (first != top) return fail(unreadable.arg(tr("it is not laid out as expected")));
    }
    if (top.empty()) return fail(unreadable.arg(tr("it is empty")));

    if (!QDir().mkpath(root)) return fail(tr("Couldn’t create the folder %1.").arg(root));
    removeLeftovers(root);
    QTemporaryDir unpack(root + QStringLiteral("/.unpack-XXXXXX"));
    if (!unpack.isValid()) return fail(tr("Couldn’t create a folder in %1.").arg(root));
    for (const compositor::ZipFileEntry& entry : zip.entries()) {
        const std::string inside = entry.name.substr(top.size() + 1);
        if (inside.empty()) continue;   // the top folder's own entry
        if (!compositor::isSafeZipEntryName(inside)) return fail(unreadable.arg(QString::fromStdString(entry.name)));
        const QString path = unpack.path() + '/' + QString::fromStdString(inside);
        if (entry.isDirectory()) {
            if (!QDir().mkpath(path)) return fail(tr("Couldn’t create the folder %1.").arg(path));
            continue;
        }
        std::vector<uint8_t> data;
        if (!zip.read(entry, data, &why)) return fail(unreadable.arg(QString::fromStdString(why)));
        if (!QDir().mkpath(QFileInfo(path).path())) return fail(tr("Couldn’t create the folder %1.").arg(QFileInfo(path).path()));
        QFile out(path);
        if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate) || out.write(reinterpret_cast<const char*>(data.data()), qint64(data.size())) != qint64(data.size()))
            return fail(tr("Couldn’t write %1.").arg(path));
        out.close();
    }
    const QString executable = unpack.path() + '/' + build.executable;
    if (!QFileInfo(executable).isFile()) return fail(unreadable.arg(tr("%1 is missing").arg(build.executable)));
    QFile::setPermissions(executable, QFile::permissions(executable) | QFileDevice::ExeOwner | QFileDevice::ExeUser | QFileDevice::ExeGroup | QFileDevice::ExeOther);

    // Into place in one rename; a copy already there (the same version, downloaded again) is moved aside first.
    const QString dest = root + '/' + build.version;
    QString aside;
    if (QFileInfo::exists(dest)) {
        aside = root + QStringLiteral("/.old-") + QString::number(QDateTime::currentMSecsSinceEpoch());
        if (!QDir().rename(dest, aside)) return fail(tr("Couldn’t replace %1; G'MIC may be running. Close the G'MIC dialog and try again.").arg(dest));
    }
    if (!QDir().rename(unpack.path(), dest)) {
        if (!aside.isEmpty()) QDir().rename(aside, dest);
        return fail(tr("Couldn’t move G'MIC into %1.").arg(dest));
    }
    unpack.setAutoRemove(false);
    if (!aside.isEmpty()) QDir(aside).removeRecursively();
    return true;
}

GmicStore::GmicStore(QObject* parent) : QObject(parent) {}

bool GmicStore::busy() const { return !reply_.isNull(); }

GmicStore::~GmicStore() {
    if (reply_) {
        reply_->disconnect(this);
        reply_->abort();
        reply_->deleteLater();
        activeDownloads()--;
    }
    discard();
}

void GmicStore::discard() {
    if (!file_) return;
    file_->close();
    file_->remove();
    delete file_;
    file_ = nullptr;
}

void GmicStore::download() {
    if (busy()) return;
    if (activeDownloads() > 0) { emit failed(tr("G'MIC is already being downloaded.")); return; }
    const QString root = directory();
    if (!QDir().mkpath(root)) { emit failed(tr("Couldn’t create the folder %1.").arg(root)); return; }
    removeLeftovers(root);
    file_ = new QFile(partPath());
    if (!file_->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        delete file_;
        file_ = nullptr;
        emit failed(tr("Couldn’t write to %1.").arg(root));
        return;
    }
    if (!network_) network_ = new QNetworkAccessManager(this);
    QNetworkRequest request{QUrl(pinned().url)};
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setTransferTimeout(30000);   // no data for this long: the site is down or the connection gone
    received_ = 0;
    abortReason_.clear();
    activeDownloads()++;
    reply_ = network_->get(request);
    connect(reply_, &QNetworkReply::readyRead, this, [this] {
        if (!reply_ || !file_) return;
        const QByteArray chunk = reply_->readAll();
        received_ += chunk.size();
        // Larger than the pinned file: not the file we want, and no reason to fill the disk with it.
        if (received_ > pinned().bytes) {
            abortReason_ = tr("gmic.eu sent a larger file than expected, so it was not used. Try again later.");
            reply_->abort();
            return;
        }
        if (file_->write(chunk) != chunk.size()) {
            abortReason_ = tr("Couldn’t write to %1.").arg(directory());
            reply_->abort();
        }
    });
    connect(reply_, &QNetworkReply::downloadProgress, this, [this](qint64 received, qint64 total) { emit progress(received, total > 0 ? total : pinned().bytes); });
    connect(reply_, &QNetworkReply::finished, this, &GmicStore::finish);
}

void GmicStore::cancel() {
    if (reply_) reply_->abort();
}

void GmicStore::finish() {
    QNetworkReply* reply = reply_;
    if (!reply) return;
    reply_ = nullptr;
    activeDownloads()--;
    reply->deleteLater();
    const QNetworkReply::NetworkError code = reply->error();
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (code == QNetworkReply::OperationCanceledError && abortReason_.isEmpty()) { discard(); emit cancelled(); return; }
    QString error;
    if (!abortReason_.isEmpty()) error = abortReason_;
    else if (status >= 400)
        error = tr("gmic.eu answered %1 (%2): the file may have moved or the site may be down. Try again later, or install G'MIC yourself.")
                    .arg(status).arg(reply->attribute(QNetworkRequest::HttpReasonPhraseAttribute).toString());
    else if (code != QNetworkReply::NoError)
        error = tr("Couldn’t download G'MIC from gmic.eu: %1. Check the internet connection and try again.").arg(reply->errorString());
    if (error.isEmpty()) {
        const QByteArray rest = reply->readAll();
        if (file_->write(rest) != rest.size()) error = tr("Couldn’t write to %1.").arg(directory());
    }
    if (!error.isEmpty()) { discard(); emit failed(error); return; }
    const QString part = file_->fileName();
    file_->close();
    delete file_;
    file_ = nullptr;
    if (!install(part, pinned(), directory(), &error)) { emit failed(error); return; }
    emit succeeded();
}

} // namespace app
