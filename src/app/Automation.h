// The automation socket: newline-delimited JSON-RPC 2.0 over a local socket, so
// an agent (through the MCP bridge in mcp/) or a script can drive the editor.
// Every request runs on the main thread against the window's current tab and
// is answered with a JSON object on one line. See docs/automation.md.
#pragma once
#include <QJsonObject>
#include <QPointer>
#include <QSet>
#include <QJsonValue>
#include <QObject>
#include <QString>
#include <functional>
#include <map>

class QLocalServer;
class QLocalSocket;

namespace app {

class MainWindow;

class AutomationServer : public QObject {
    Q_OBJECT
public:
    explicit AutomationServer(MainWindow* window);
    ~AutomationServer() override;

    /// $XDG_RUNTIME_DIR/nekophoto.sock, or a per-user file under the temp directory.
    static QString defaultSocketPath();
    bool listen(const QString& path, QString* error);
    QString socketPath() const { return path_; }
    int clients() const { return int(clients_.size()); }

    /// Runs one JSON-RPC request object and returns the response object (also used by tests).
    QJsonObject handle(const QJsonObject& request);
    /// The method names, for `rpc.methods`.
    QStringList methods() const;

signals:
    void clientsChanged(int count);

private:
    using Handler = std::function<QJsonValue(const QJsonObject&)>;
    void registerHandlers();
    void add(const QString& name, Handler handler) { handlers_[name] = std::move(handler); }
    /// An event for every subscribed client, coalesced per kind until the event loop turns.
    void notify(const QString& kind);
    void flush(QLocalSocket* socket);

    struct Client {
        QByteArray buffer;
        QSet<QString> kinds;   // subscribed event kinds; "all" for everything
        QSet<QString> pending;
        bool flushScheduled = false;
    };
    MainWindow* window_;
    QLocalServer* server_ = nullptr;
    QString path_;
    std::map<QLocalSocket*, Client> clients_;
    QLocalSocket* current_ = nullptr;   // the socket whose request is being handled
    std::map<QString, Handler> handlers_;
};

} // namespace app
