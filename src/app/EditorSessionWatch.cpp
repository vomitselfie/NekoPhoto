// Following an open project's package on disk: when another app or an agent writes it, the document is reloaded in
// place (same tab, same view, the active layer kept where it still exists). Only a real change counts (a fingerprint
// of the manifest's bytes and every file's name and size, so a package merely touched is left alone); a package caught
// half written is left alone until the next change; unsaved work is never replaced without asking. After upstream
// Compositor's ProjectWatcher / ProjectDigest / ProjectController+ExternalChanges (MIT, LICENSES/MIT-Compositor.txt).
#include "EditorSession.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QMetaMethod>
#include <QTimer>

namespace app {

namespace {

/// The package's fingerprint; empty when it cannot be read (half written, gone).
QByteArray projectDigest(const QString& path) {
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
    // Re-arm by path each time: an atomic save swaps the package folder (and its files) for new ones.
    const QDir dir(projectPath_);
    QStringList paths{projectPath_, dir.filePath("manifest.json"), dir.filePath("images"), dir.filePath("smartobjects")};
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
    auto project = readProject(projectPath_, nullptr);
    if (!project) return;   // half written or mid-sync: the next change is checked afresh
    commitTransform();
    const std::optional<compositor::Uuid> active = activeLayerId_;
    document_ = std::move(project->document);
    setActiveLayer(active && document_->find(*active) ? active : project->activeLayer);
    history_.reset();
    knownDigest_ = projectDigest(projectPath_);
    externalReloads_++;
    notifyDocument();
    emit selectionChanged();
    emit reloadedFromDisk();
}

} // namespace app
