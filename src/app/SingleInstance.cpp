#include "SingleInstance.h"
#include "MainWindow.h"
#include <QDir>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include "Platform.h"
#include <QLocalServer>
#include <QLocalSocket>
#include <QLockFile>
#include <QStandardPaths>
#include <QThread>

namespace app {

namespace {

/// The private runtime folder (Platform.h); with none, the lock and socket go nowhere and every launch is its own.
QString runtimeDirectory() { return platform::runtimeDirectory(); }

} // namespace

SingleInstance::SingleInstance(QObject* parent) : QObject(parent) {}
SingleInstance::~SingleInstance() = default;

QString SingleInstance::socketPath() { return platform::localServerName(platform::defaultLocalSocket("nekophoto-instance.sock")); }
QString SingleInstance::lockPath() { return runtimeDirectory() + "/nekophoto-instance.lock"; }

bool SingleInstance::handOff(const QStringList& files, const QString& rpcSocket) {
    // The lock says whether an instance is running (a dead one's lock is cleared by the PID check); by age a
    // lock is never stale, or a long-running editor would lose it after half a minute.
    if (runtimeDirectory().isEmpty()) return false;
    QLockFile probe(lockPath());
    probe.setStaleLockTime(std::chrono::milliseconds(0));
    if (probe.tryLock(0)) { probe.unlock(); return false; }
    // The instance may still be starting up (two files double-clicked in a row): keep trying for a moment.
    QLocalSocket socket;
    QElapsedTimer waited;
    waited.start();
    while (true) {
        socket.connectToServer(socketPath());
        if (socket.waitForConnected(250)) break;
        if (waited.elapsed() > 4000) return false;
        QThread::msleep(100);
    }
    QJsonArray paths;
    for (const QString& f : files) paths.append(f);
    QJsonObject request{{"open", paths}, {"raise", true}};
    if (!rpcSocket.isEmpty()) request["rpc"] = rpcSocket;
    socket.write(QJsonDocument(request).toJson(QJsonDocument::Compact) + "\n");
    socket.flush();
    if (!socket.waitForReadyRead(5000)) return false;
    return socket.readAll().trimmed() == "ok";
}

bool SingleInstance::serve(MainWindow& window) {
    if (runtimeDirectory().isEmpty()) return false;
    lock_ = std::make_unique<QLockFile>(lockPath());
    lock_->setStaleLockTime(std::chrono::milliseconds(0));
    if (!lock_->tryLock(0)) { lock_.reset(); return false; }
    if (!platform::removeStaleSocket(socketPath(), nullptr)) { lock_.reset(); return false; }   // left behind by an instance that died
    server_ = new QLocalServer(this);
    server_->setSocketOptions(QLocalServer::UserAccessOption);
    if (!server_->listen(socketPath())) { delete server_; server_ = nullptr; lock_.reset(); return false; }
    connect(server_, &QLocalServer::newConnection, this, [this, &window] {
        while (QLocalSocket* client = server_->nextPendingConnection()) {
            auto* buffer = new QByteArray;
            connect(client, &QLocalSocket::disconnected, client, [client, buffer] { delete buffer; client->deleteLater(); });
            connect(client, &QLocalSocket::readyRead, client, [client, buffer, &window] {
                *buffer += client->readAll();
                const int newline = buffer->indexOf('\n');
                if (newline < 0) return;
                const QJsonObject request = QJsonDocument::fromJson(buffer->left(newline)).object();
                buffer->clear();
                for (QJsonValue v : request.value("open").toArray()) {
                    const QString path = v.toString();
                    if (!path.isEmpty()) window.openAsDocument(path);
                }
                const QString rpc = request.value("rpc").toString();
                if (!rpc.isEmpty()) window.startAutomation(rpc);
                if (request.value("raise").toBool(true)) {
                    if (window.isMinimized()) window.showNormal();
                    window.raise();
                    window.activateWindow();
                }
                client->write("ok\n");
                client->flush();
                client->disconnectFromServer();
            });
        }
    });
    return true;
}

} // namespace app
