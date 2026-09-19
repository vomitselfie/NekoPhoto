// One editor per user: opening a file from the launcher while the editor runs adds a tab to the running
// window instead of starting a second process. The running instance holds a lock file and listens on a local
// socket in the runtime directory; a later launch hands its files over that socket and quits.
#pragma once
#include <QObject>
#include <QStringList>
#include <memory>

class QLocalServer;
class QLockFile;

namespace app {

class MainWindow;

class SingleInstance : public QObject {
    Q_OBJECT
public:
    explicit SingleInstance(QObject* parent = nullptr);
    ~SingleInstance() override;

    static QString socketPath();
    static QString lockPath();

    /// Hands `files` (absolute paths, possibly none) to a running instance, which opens them and raises its
    /// window; with `rpcSocket` it also asks that instance to start its automation server on that path, so
    /// an agent that launched the editor attaches to the window the person already has. True when an
    /// instance took the request, so this process should quit; false when there is none, or it could not be
    /// reached within a moment, so this process should become the instance.
    static bool handOff(const QStringList& files, const QString& rpcSocket = QString());

    /// Becomes the instance for `window`: takes the lock and listens. False when another instance holds the
    /// lock or the socket cannot be made; the window then simply runs without the handoff.
    bool serve(MainWindow& window);

private:
    std::unique_ptr<QLockFile> lock_;
    QLocalServer* server_ = nullptr;
};

} // namespace app
