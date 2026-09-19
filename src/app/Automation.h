// The automation socket: newline-delimited JSON-RPC 2.0 over a local socket, so
// an agent (through the MCP bridge in mcp/) or a script can drive the editor.
// Every request runs on the main thread against the window's current tab and
// is answered with a JSON object on one line. See docs/automation.md.
#pragma once
#include <QJsonObject>
#include <QJsonValue>
#include <QObject>
#include <QString>
#include <functional>
#include <map>

class QLocalServer;

namespace app {

class MainWindow;

class AutomationServer : public QObject {
    Q_OBJECT
public:
    explicit AutomationServer(MainWindow* window);
    ~AutomationServer() override;

    /// $XDG_RUNTIME_DIR/compositor-linux.sock, or a per-user file under the temp directory.
    static QString defaultSocketPath();
    bool listen(const QString& path, QString* error);
    QString socketPath() const { return path_; }
    int clients() const { return clients_; }

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

    MainWindow* window_;
    QLocalServer* server_ = nullptr;
    QString path_;
    int clients_ = 0;
    std::map<QString, Handler> handlers_;
};

} // namespace app
