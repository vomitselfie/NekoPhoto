#include "Autosave.h"
#include "EditorSession.h"
#include "compositor/project.h"
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>
#include <QUuid>
#include <algorithm>

namespace app {

Autosave::Autosave(QObject* parent) : QObject(parent) {
    pool_.setMaxThreadCount(1);   // one save at a time, in order
    id_ = QUuid::createUuid().toString(QUuid::WithoutBraces);
    dir_ = root() + "/" + id_;
    QDir().mkpath(dir_);
    lock_ = std::make_unique<QLockFile>(root() + "/" + id_ + ".lock");
    lock_->setStaleLockTime(0);   // stale only when its process is gone, never by age
    lock_->tryLock(0);
    timer_ = new QTimer(this);
    connect(timer_, &QTimer::timeout, this, [this] { tick(); });
    restart();
}

Autosave::~Autosave() { pool_.waitForDone(); }

QString Autosave::root() { return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/recovery"; }

int Autosave::intervalMinutes() { return std::clamp(QSettings().value("autosave/minutes", 3).toInt(), 0, 120); }
void Autosave::setIntervalMinutes(int minutes) { QSettings().setValue("autosave/minutes", std::clamp(minutes, 0, 120)); }

void Autosave::restart() {
    // COMPOSITOR_AUTOSAVE_MS overrides the interval, for tests.
    const int override = qEnvironmentVariableIntValue("COMPOSITOR_AUTOSAVE_MS"), minutes = intervalMinutes();
    if (override > 0) timer_->start(override);
    else if (minutes > 0) timer_->start(minutes * 60 * 1000);
    else timer_->stop();
}

void Autosave::watch(EditorSession* session, std::function<QString()> title) {
    Entry entry;
    entry.key = QUuid::createUuid().toString(QUuid::WithoutBraces);
    entry.title = std::move(title);
    entries_[session] = std::move(entry);
    connect(session, &EditorSession::historyChanged, this, [this, session] {
        if (auto it = entries_.find(session); it != entries_.end()) it->second.dirty = true;
    });
}

void Autosave::forget(EditorSession* session) {
    auto it = entries_.find(session);
    if (it == entries_.end()) return;
    disconnect(session, nullptr, this, nullptr);
    if (!it->second.saving) removeFiles(it->second.key);   // one in flight removes itself when it lands
    entries_.erase(it);
}

void Autosave::removeFiles(const QString& key) const {
    QDir(dir_ + "/" + key + ".comp").removeRecursively();
    QDir(dir_ + "/" + key + ".saving.comp").removeRecursively();
    QFile::remove(dir_ + "/" + key + ".json");
}

void Autosave::tick() {
    for (auto& [session, entry] : entries_) {
        if (entry.saving) continue;
        if (!session->document() || !session->isModified()) {
            // Saved, or emptied: the copy is no longer needed.
            if (entry.onDisk) { removeFiles(entry.key); entry.onDisk = false; }
            continue;
        }
        if (!entry.dirty) continue;
        entry.dirty = false;
        entry.saving = true;
        // The copy shares the layers' pixels with the document; the worker only reads it.
        auto document = std::make_shared<const compositor::Document>(*session->document());
        const std::optional<compositor::Uuid> active = session->activeLayerId();
        const QString key = entry.key, dir = dir_, title = entry.title ? entry.title() : QString(), original = session->projectPath();
        EditorSession* owner = session;
        pool_.start([this, document, active, key, dir, title, original, owner] {
            const QString saving = dir + "/" + key + ".saving.comp", final = dir + "/" + key + ".comp";
            QDir(saving).removeRecursively();
            compositor::ProjectError error;
            bool ok = compositor::saveProject(*document, active, saving.toStdString(), error);
            if (ok) {
                QDir(final).removeRecursively();
                ok = QDir().rename(saving, final);
            }
            if (ok) {
                QFile meta(dir + "/" + key + ".json");
                if (meta.open(QIODevice::WriteOnly))
                    meta.write(QJsonDocument(QJsonObject{{"title", title}, {"originalPath", original}, {"saved", QDateTime::currentDateTimeUtc().toString(Qt::ISODate)}}).toJson());
            } else {
                QDir(saving).removeRecursively();
            }
            QMetaObject::invokeMethod(this, [this, owner, key, ok] {
                auto it = entries_.find(owner);
                if (it == entries_.end() || it->second.key != key) { removeFiles(key); return; }   // the tab closed meanwhile
                it->second.saving = false;
                it->second.onDisk = it->second.onDisk || ok;
                if (!ok) it->second.dirty = true;
            }, Qt::QueuedConnection);
        });
    }
}

void Autosave::finish() {
    timer_->stop();
    pool_.waitForDone();
    QDir(dir_).removeRecursively();
    entries_.clear();
    lock_->unlock();
}

std::vector<Autosave::Recovered> Autosave::claimOrphans() {
    std::vector<Recovered> out;
    const QDir base(root());
    for (const QString& name : base.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        if (name == id_) continue;
        auto lock = std::make_unique<QLockFile>(base.filePath(name + ".lock"));
        lock->setStaleLockTime(0);
        if (!lock->tryLock(0)) continue;   // its instance is still running
        const QDir folder(base.filePath(name));
        bool any = false;
        for (const QString& meta : folder.entryList({"*.json"}, QDir::Files)) {
            const QString key = meta.chopped(5), project = folder.filePath(key + ".comp");
            if (!QFileInfo(project).isDir()) continue;
            QFile file(folder.filePath(meta));
            if (!file.open(QIODevice::ReadOnly)) continue;
            const QJsonObject o = QJsonDocument::fromJson(file.readAll()).object();
            out.push_back({project, o.value("title").toString(), o.value("originalPath").toString(), QDateTime::fromString(o.value("saved").toString(), Qt::ISODate)});
            any = true;
        }
        if (!any) { QDir(folder).removeRecursively(); lock->unlock(); continue; }   // nothing worth offering
        claimed_.emplace_back(std::move(lock), folder.absolutePath());
    }
    std::sort(out.begin(), out.end(), [](const Recovered& a, const Recovered& b) { return a.saved > b.saved; });
    return out;
}

void Autosave::discardClaimed() {
    for (auto& [lock, folder] : claimed_) { QDir(folder).removeRecursively(); lock->unlock(); }
    claimed_.clear();
}

void Autosave::releaseClaimed() {
    for (auto& [lock, folder] : claimed_) lock->unlock();
    claimed_.clear();
}

} // namespace app
