// G'MIC downloaded on request. NekoPhoto does not ship G'MIC; on Windows, where there is no package manager to install
// it from, Filter > G'MIC and Preferences offer to fetch the command-line build from gmic.eu (CeCILL 2.1). The archive
// is pinned by size and SHA-256, checked before anything is unpacked, unpacked with the core's ZIP reader into a
// temporary folder and renamed into place whole, so the lookup (GmicRunner::locate) never sees half a copy.
#pragma once
#include "compositor/zipfile.h"
#include <QObject>
#include <QPointer>
#include <QString>

class QFile;
class QNetworkAccessManager;
class QNetworkReply;

namespace app {

class GmicStore : public QObject {
    Q_OBJECT
public:
    struct Build {
        QString version;      // the folder it unpacks into under GmicRunner::downloadRoot()
        QString url;
        QString sha256;       // of the archive
        qint64 bytes = 0;     // of the archive
        QString executable;   // the file the archive must hold at its top (after its own top folder)
    };
    /// The build on offer: G'MIC's win64 command-line zip.
    static const Build& pinned();
    /// This platform gets a Download button (Windows; COMPOSITOR_GMIC_OFFER_DOWNLOAD=1 shows it elsewhere, for testing).
    static bool offered();
    /// G'MIC can run (GmicRunner::available()).
    static bool available();
    /// The pinned build is downloaded and unpacked.
    static bool downloaded();
    /// The folder downloads go into (GmicRunner::downloadRoot()).
    static QString directory();
    /// Deletes the downloaded build. False with `error` when it could not (in use, on Windows).
    static bool remove(QString* error = nullptr);

    /// The limits an archive is unpacked under: the real one has about 35 entries and 40 MB.
    static compositor::ZipLimits limits();
    /// Checks `archive` against `build`'s size and SHA-256, then unpacks it into `root/<version>`, the archive's top
    /// folder stripped: into a temporary folder in `root` first, renamed into place once complete. The archive is
    /// deleted either way; on failure nothing is left in `root` and `error` says why.
    static bool install(const QString& archive, const Build& build, const QString& root, QString* error,
                        const compositor::ZipLimits& limits = GmicStore::limits());

    explicit GmicStore(QObject* parent = nullptr);
    ~GmicStore() override;
    bool busy() const;
    /// Fetches and installs the pinned build; ends in exactly one of succeeded, failed or cancelled.
    void download();
    void cancel();

signals:
    void progress(qint64 received, qint64 total);
    void succeeded();
    void failed(QString error);
    void cancelled();

private:
    void finish();
    void discard();
    QNetworkAccessManager* network_ = nullptr;
    QPointer<QNetworkReply> reply_;
    QFile* file_ = nullptr;
    qint64 received_ = 0;
    QString abortReason_;   // why the download was stopped, when it was not the person's Cancel
};

} // namespace app
