// Crash recovery. Every few minutes each tab with unsaved changes is written as a project into this
// instance's recovery folder, on a worker thread from a copy of its document (layers share their pixels, so
// the copy is cheap and the tab is never paused for the encode). A save or closing the tab removes that
// copy, and a clean quit removes the folder. After a crash the next launch finds the folder, whose lock file
// no running instance holds, and offers its projects back.
#pragma once
#include <QDateTime>
#include <QLockFile>
#include <QObject>
#include <QString>
#include <QThreadPool>
#include <functional>
#include <map>
#include <memory>
#include <vector>

class QTimer;

namespace app {

class EditorSession;

class Autosave : public QObject {
public:
    explicit Autosave(QObject* parent = nullptr);
    ~Autosave() override;

    /// Minutes between autosaves; 0 turns it off. Stored in QSettings as autosave/minutes (default 3).
    static int intervalMinutes();
    static void setIntervalMinutes(int minutes);

    /// Starts watching a tab; `title` names it in the recovery offer.
    void watch(EditorSession* session, std::function<QString()> title);
    /// The tab closed: its copy goes.
    void forget(EditorSession* session);
    /// A clean quit: waits for any save in flight, then removes this instance's folder.
    void finish();
    /// Rereads the interval.
    void restart();

    /// A project left by an instance that is no longer running.
    struct Recovered { QString project, title, originalPath; QDateTime saved; };
    /// Claims the folders of instances that are no longer running (their locks are taken, so another launch
    /// does not offer them too) and lists their projects, newest first.
    std::vector<Recovered> claimOrphans();
    /// Deletes the claimed folders (after their projects were reopened, or declined).
    void discardClaimed();
    /// Lets the claimed folders go without deleting them, to be offered again next launch.
    void releaseClaimed();

    /// The folder all instances keep their recovery folders in.
    static QString root();

private:
    struct Entry { QString key; std::function<QString()> title; bool dirty = true, saving = false, onDisk = false; };
    void tick();
    void removeFiles(const QString& key) const;

    QString id_, dir_;
    std::unique_ptr<QLockFile> lock_;
    QTimer* timer_ = nullptr;
    QThreadPool pool_;
    std::map<EditorSession*, Entry> entries_;
    std::vector<std::pair<std::unique_ptr<QLockFile>, QString>> claimed_;   // lock and folder
};

} // namespace app
