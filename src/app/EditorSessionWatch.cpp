// Following an open project on disk: when another app or an agent writes it, the document is reloaded in place (same
// tab, same view, the active layer kept where it still exists). Only a real change counts (for a .comp folder, a
// fingerprint of the manifest's bytes and every file's name and size; for a .nekophoto file, of its ZIP directory: every
// entry's name, size and checksum), so a project merely touched is left alone; one caught half written is left alone
// until the next change; unsaved work is never replaced without asking. After upstream
// Compositor's ProjectWatcher / ProjectDigest / ProjectController+ExternalChanges (MIT, LICENSES/MIT-Compositor.txt).
#include "EditorSession.h"
#include "compositor/zipfile.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QMetaMethod>
#include <QTimer>

namespace app {

namespace {

/// A .nekophoto file's fingerprint: its directory, which names every entry with its size and checksum.
QByteArray documentFileDigest(const QString& path) {
    compositor::ZipFileReader zip;
    if (!zip.openFile(path.toStdString())) return {};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    for (const compositor::ZipFileEntry& e : zip.entries()) {
        hash.addData(QByteArray::fromStdString(e.name));
        const quint64 fields[2] = {e.size, e.crc};
        hash.addData(QByteArrayView(reinterpret_cast<const char*>(fields), sizeof fields));
    }
    return hash.result();
}

/// The project's fingerprint; empty when it cannot be read (half written, gone).
QByteArray projectDigest(const QString& path) {
    if (QFileInfo(path).isFile()) return documentFileDigest(path);
    QFile manifest(QDir(path).filePath("manifest.json"));
    if (!manifest.open(QIODevice::ReadOnly)) return {};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(manifest.readAll());
    for (const char* folder : {"images", "smartobjects"}) {
        QDir dir(QDir(path).filePath(folder));
        const QFileInfoList files = dir.entryInfoList(QDir::Files | QDir::NoSymLinks, QDir::Name);
        for (const QFileInfo& f : files) {
            hash.addData(QByteArray(folder) + '/' + f.fileName().toUtf8());
            const qint64 size = f.size();
            hash.addData(QByteArrayView(reinterpret_cast<const char*>(&size), sizeof size));
        }
    }
    return hash.result();
}

} // namespace

void EditorSession::watchProject() {
    stopWatchingProject();
    if (projectPath_.isEmpty()) return;
    watcher_ = new QFileSystemWatcher(this);
    settle_ = new QTimer(this);
    settle_->setSingleShot(true);
    settle_->setInterval(300);   // a package is written in several steps: wait for them to settle
    connect(settle_, &QTimer::timeout, this, &EditorSession::checkExternalChange);
    connect(watcher_, &QFileSystemWatcher::directoryChanged, this, &EditorSession::noteExternalChange);
    connect(watcher_, &QFileSystemWatcher::fileChanged, this, &EditorSession::noteExternalChange);
    knownDigest_ = projectDigest(projectPath_);
    noteExternalChange();   // arms the paths
    settle_->stop();
}

void EditorSession::stopWatchingProject() {
    delete watcher_;
    watcher_ = nullptr;
    delete settle_;
    settle_ = nullptr;
    knownDigest_.clear();
    asking_ = false;
}

void EditorSession::noteExternalChange() {
    if (!watcher_) return;
    // Re-arm by path each time: an atomic save swaps the package folder (and its files), or the file, for new ones. A
    // .nekophoto file is renamed over, so its folder is watched too: that is where the new one appears.
    const QDir dir(projectPath_);
    const QFileInfo info(projectPath_);
    const QStringList paths = projectPath_.endsWith(".nekophoto", Qt::CaseInsensitive) && !info.isDir()
                                  ? QStringList{projectPath_, info.absolutePath()}
                                  : QStringList{projectPath_, dir.filePath("manifest.json"), dir.filePath("images"), dir.filePath("smartobjects")};
    QStringList existing;
    for (const QString& p : paths) if (QFileInfo::exists(p)) existing << p;
    const QStringList watched = watcher_->files() + watcher_->directories();
    for (const QString& p : existing) if (!watched.contains(p)) watcher_->addPath(p);
    settle_->start();
}

void EditorSession::checkExternalChange() {
    if (!watcher_ || !document_ || projectPath_.isEmpty() || asking_) return;
    const QByteArray digest = projectDigest(projectPath_);
    if (digest.isEmpty() || digest == knownDigest_) return;   // half written, or only touched
    // An edit in progress finishes first.
    if (textEditing_ || brushActive() || transformEdit_) { settle_->start(1000); return; }
    if (isModified()) {
        // Asked in its window; a tab in the background asks once it is in front (checked again every few seconds).
        if (!isSignalConnected(QMetaMethod::fromSignal(&EditorSession::externalChangeConflict))) { settle_->start(3000); return; }
        asking_ = true;
        emit externalChangeConflict(projectPath_);
        return;
    }
    reloadFromDisk();
}

void EditorSession::resolveExternalChange(bool revert) {
    asking_ = false;
    if (revert) reloadFromDisk();
    else knownDigest_ = projectDigest(projectPath_);   // keep ours; the next change is asked about afresh
}

void EditorSession::reloadFromDisk() {
    if (!reloadProject()) return;   // half written or mid-sync: the next change is checked afresh
    externalReloads_++;
    emit reloadedFromDisk();
}

bool EditorSession::revertToSaved() {
    if (!document_ || projectPath_.isEmpty()) return false;
    cancelBrush();
    return reloadProject();
}

bool EditorSession::reloadProject() {
    auto project = readProject(projectPath_, nullptr);
    if (!project) return false;
    commitTransform();
    const std::optional<compositor::Uuid> active = activeLayerId_;
    previewBase_.reset();
    document_ = std::move(project->document);
    setActiveLayer(active && document_->find(*active) ? active : project->activeLayer);
    history_.reset();
    knownDigest_ = projectDigest(projectPath_);
    lastStepUndone_ = false;
    notifyDocument();
    emit selectionChanged();
    emit titleChanged();
    emit historyChanged();
    return true;
}

} // namespace app
