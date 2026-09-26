#include "compositor/raw.h"
#include "Automation.h"
#include "AutomationHandlers.h"
#include "LayersPanel.h"
#include "ModelStore.h"
#include "compositor/scribble.h"
#include <QApplication>
#include <QDir>
#include <QLocalServer>
#include <QLocalSocket>
#include <QMouseEvent>
#include <QToolButton>
#include <QStandardPaths>
#include <QJsonDocument>
#include <QTimer>
#include <algorithm>

using namespace compositor;
using namespace app::rpc;

namespace app {

// ---- Server -------------------------------------------------------------------------------------

AutomationServer::AutomationServer(MainWindow* window) : QObject(window), window_(window) {
    registerHandlers();
    connect(window_, &MainWindow::automationEvent, this, &AutomationServer::notify);
}

AutomationServer::~AutomationServer() {
    if (server_) { server_->close(); QLocalServer::removeServer(path_); }
}

QString AutomationServer::defaultSocketPath() {
    QString env = qEnvironmentVariable("COMPOSITOR_RPC_SOCKET");
    if (!env.isEmpty()) return env;
    QString runtime = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    if (runtime.isEmpty()) runtime = QDir::tempPath();
    return runtime + "/nekophoto.sock";
}

bool AutomationServer::listen(const QString& path, QString* error) {
    path_ = path;
    QLocalServer::removeServer(path);
    server_ = new QLocalServer(this);
    server_->setSocketOptions(QLocalServer::UserAccessOption);
    if (!server_->listen(path)) {
        if (error) *error = server_->errorString();
        return false;
    }
    connect(server_, &QLocalServer::newConnection, this, [this] {
        while (QLocalSocket* socket = server_->nextPendingConnection()) {
            clients_[socket] = Client{};
            emit clientsChanged(clients());
            connect(socket, &QLocalSocket::readyRead, this, [this, socket] {
                auto it = clients_.find(socket);
                if (it == clients_.end()) return;
                QByteArray& buffer = it->second.buffer;
                buffer.append(socket->readAll());
                int newline;
                while ((newline = buffer.indexOf('\n')) >= 0) {
                    QByteArray line = buffer.left(newline).trimmed();
                    buffer.remove(0, newline + 1);
                    if (line.isEmpty()) continue;
                    QJsonParseError parseError;
                    QJsonDocument doc = QJsonDocument::fromJson(line, &parseError);
                    QJsonObject response;
                    if (parseError.error != QJsonParseError::NoError || !doc.isObject())
                        response = {{"jsonrpc", "2.0"}, {"id", QJsonValue::Null}, {"error", QJsonObject{{"code", -32700}, {"message", "parse error: " + parseError.errorString()}}}};
                    else { current_ = socket; response = handle(doc.object()); current_ = nullptr; }
                    if (!clients_.count(socket)) return;   // the request closed the connection
                    socket->write(QJsonDocument(response).toJson(QJsonDocument::Compact) + "\n");
                    socket->flush();
                }
            });
            connect(socket, &QLocalSocket::disconnected, this, [this, socket] {
                if (groups_.count(socket)) endGroup(socket);   // a client that goes away closes its group
                clients_.erase(socket);
                socket->deleteLater();
                emit clientsChanged(clients());
            });
        }
    });
    return true;
}

void AutomationServer::notify(const QString& kind) {
    for (auto& [socket, client] : clients_) {
        if (!client.kinds.contains("all") && !client.kinds.contains(kind)) continue;
        client.pending.insert(kind);
        if (client.flushScheduled) continue;
        client.flushScheduled = true;
        QPointer<QLocalSocket> guard(socket);
        QTimer::singleShot(0, this, [this, guard] { if (guard) flush(guard); });
    }
}

void AutomationServer::flush(QLocalSocket* socket) {
    auto it = clients_.find(socket);
    if (it == clients_.end()) return;
    Client& client = it->second;
    client.flushScheduled = false;
    QStringList kinds(client.pending.begin(), client.pending.end());
    kinds.sort();
    client.pending.clear();
    for (const QString& kind : kinds) {
        QJsonObject event{{"jsonrpc", "2.0"}, {"method", "event"}, {"params", QJsonObject{{"kind", kind}, {"tab", window_->currentTabIndex()}}}};
        socket->write(QJsonDocument(event).toJson(QJsonDocument::Compact) + "\n");
    }
    socket->flush();
}

QStringList AutomationServer::methods() const {
    QStringList names;
    for (auto& [name, handler] : handlers_) names << name;
    return names;
}

namespace {

/// Where to look when a request's parameters are wrong.
QString describeHint(const QString& method) {
    return QStringLiteral(" (rpc.describe {\"method\": \"%1\"} lists its parameters)").arg(method);
}

} // namespace

QJsonValue AutomationServer::runTracked(const Handler& handler, const QJsonObject& params) {
    auto group = groups_.find(current_);
    QPointer<EditorSession> session = group != groups_.end() ? group->second.session : nullptr;
    std::set<uint64_t> before;
    if (session) for (uint64_t r : session->historyRevisions()) before.insert(r);
    QJsonValue result = handler(params);
    group = groups_.find(current_);   // the handler may have closed it
    if (session && group != groups_.end() && group->second.session == session)
        for (uint64_t r : session->historyRevisions()) if (!before.count(r)) group->second.own.insert(r);
    return result;
}

QJsonObject AutomationServer::endGroup(QLocalSocket* owner) {
    auto it = groups_.find(owner);
    if (it == groups_.end()) return {};
    Group group = std::move(it->second);
    groups_.erase(it);
    QJsonObject out{{"name", group.name}, {"merged", 0}};
    if (!group.session) { out["note"] = "the group's tab was closed"; return out; }
    auto steps = group.session->historyRevisionsSince(group.since);
    if (!steps) { out["note"] = "undone past where the group began; nothing merged"; return out; }
    for (uint64_t r : *steps) {
        if (group.own.count(r)) continue;
        out["steps"] = int(steps->size());
        out["note"] = "someone else edited the document during the group, so its steps stay separate";
        return out;
    }
    out["merged"] = group.session->squashHistory(group.since, group.name);
    return out;
}

QJsonObject AutomationServer::handle(const QJsonObject& request) {
    QJsonValue id = request.value("id");
    QString method = request.value("method").toString();
    QJsonObject params = request.value("params").toObject();
    QJsonObject response{{"jsonrpc", "2.0"}, {"id", id}};
    auto it = handlers_.find(method);
    if (it == handlers_.end()) {
        response["error"] = QJsonObject{{"code", methodNotFound}, {"message", "unknown method '" + method + "'; call rpc.methods for the list"}};
        return response;
    }
    // Keys the method does not take are refused rather than ignored: a misspelt key would otherwise do nothing.
    QString wrong = unknownParameter(method, params);
    if (wrong.isEmpty()) wrong = missingParameter(method, params);
    if (!wrong.isEmpty()) {
        response["error"] = QJsonObject{{"code", invalidParams}, {"message", wrong + describeHint(method)}};
        return response;
    }
    // Errors the editor would have shown in a dialog come back in the response instead.
    QString captured;
    window_->setErrorSink(&captured);
    try {
        QJsonValue result = runTracked(it->second, params);
        window_->setErrorSink(nullptr);
        if (!captured.isEmpty()) response["error"] = QJsonObject{{"code", appError}, {"message", captured.trimmed()}};
        else response["result"] = result.isUndefined() ? QJsonValue(QJsonObject{}) : result;
    } catch (const RpcError& e) {
        window_->setErrorSink(nullptr);
        QString message = QString::fromUtf8(e.what());
        if (e.code == invalidParams && method != "rpc.describe") message += describeHint(method);
        response["error"] = QJsonObject{{"code", e.code}, {"message", message}};
    } catch (const std::exception& e) {
        window_->setErrorSink(nullptr);
        response["error"] = QJsonObject{{"code", appError}, {"message", QString::fromUtf8(e.what())}};
    }
    return response;
}

// ---- Handlers -----------------------------------------------------------------------------------

void AutomationServer::registerHandlers() {
    registerAppHandlers();
    registerDocumentHandlers();
    registerLayersHandlers();
    registerPixelsHandlers();
    registerSelectionHandlers();
    registerPaintHandlers();
}

void AutomationServer::registerAppHandlers() {
    MainWindow* w = window_;
    const SessionOf session{w};
    const DocumentOf document{w};
    const LayerOf layer{document};
    auto tabJson = [w](int i) {
        EditorSession* s = w->sessionAt(i);
        QJsonObject o{{"index", i}, {"title", w->tabTitle(i)}, {"current", i == w->currentTabIndex()}, {"hasDocument", s->hasDocument()}, {"modified", s->isModified()}};
        if (!s->projectPath().isEmpty()) o["path"] = s->projectPath();
        if (s->hasDocument()) { o["width"] = s->document()->width; o["height"] = s->document()->height; }
        return o;
    };
    auto tabIndex = [w](const QJsonObject& p) {
        int i = integer(p, "index");
        if (i < 0 || i >= w->tabCount()) fail(QStringLiteral("no tab %1 (there are %2)").arg(i).arg(w->tabCount()), invalidParams);
        return i;
    };

    // ---- app / tabs
    add("rpc.methods", [this](const QJsonObject&) { return QJsonArray::fromStringList(methods()); });
    add("history.beginGroup", [this, session, document](const QJsonObject& p) {
        document();
        auto open = groups_.find(current_);
        if (open != groups_.end()) fail("the group \"" + open->second.name + "\" is still open; history.endGroup first");
        const QString name = str(p, "name").trimmed();
        if (name.isEmpty()) fail("name the group: it is what Undo will say", invalidParams);
        groups_[current_] = Group{session(), session()->historyRevision(), name, {}};
        return QJsonObject{{"group", name}};
    });
    add("history.endGroup", [this](const QJsonObject&) {
        if (!groups_.count(current_)) fail("no group is open on this connection; history.beginGroup starts one");
        return endGroup(current_);
    });
    add("rpc.batch", [this, w](const QJsonObject& p) -> QJsonValue {
        // Calls in order in one request, stopping at the first error. Named, they are one undo step, and an
        // error takes back what the earlier calls did.
        const QJsonArray calls = p.value("calls").toArray();
        if (calls.isEmpty()) fail("calls must list {\"method\", \"params\"} objects", invalidParams);
        const QString name = has(p, "name") ? str(p, "name").trimmed() : QString();
        for (int i = 0; i < calls.size(); i++) {
            const QString method = calls[i].toObject().value("method").toString();
            if (method == "rpc.batch" || method == "history.beginGroup" || method == "history.endGroup")
                fail(QStringLiteral("call %1: %2 can't run inside a batch").arg(i).arg(method), invalidParams);
        }
        QPointer<EditorSession> session = w->session();
        const uint64_t since = session->historyRevision();
        QJsonArray results;
        for (int i = 0; i < calls.size(); i++) {
            const QJsonObject call = calls[i].toObject();
            const QString method = call.value("method").toString();
            const QJsonObject reply = handle({{"method", method}, {"params", call.value("params").toObject()}, {"id", i}});
            if (reply.contains("error")) {
                QJsonObject out{{"results", results}, {"completed", i},
                                {"error", QJsonObject{{"index", i}, {"method", method}, {"message", reply["error"].toObject()["message"]}}}};
                if (!name.isEmpty() && session) {
                    while (session->historyRevision() != since && session->canUndo()) session->undo();
                    out["rolledBack"] = session->historyRevision() == since;
                }
                return out;
            }
            results.append(reply.value("result"));
        }
        QJsonObject out{{"results", results}, {"completed", int(calls.size())}};
        if (!name.isEmpty() && session) out["merged"] = session->squashHistory(since, name);
        return out;
    });
    add("rpc.describe", [](const QJsonObject& p) -> QJsonValue {
        return has(p, "method") ? describeMethod(str(p, "method")) : describeAll();
    });
    add("app.info", [this, w](const QJsonObject&) {
        return QJsonObject{{"name", "nekophoto"}, {"version", QApplication::applicationVersion()}, {"protocolVersion", protocolVersion}, {"socket", path_},
                           {"platform", QApplication::platformName()}, {"tabs", w->tabCount()}, {"currentTab", w->currentTabIndex()},
                           {"removeBackground", ModelStore::ready()}, {"scribble", scribbleSelectionSupported()}, {"clickSelect", ModelStore::promptReady()}, {"raw", compositor::rawSupported()}};
    });
    add("events.subscribe", [this](const QJsonObject& p) {
        // Notifications on this connection: {"method":"event","params":{"kind":...,"tab":N}}, one per kind per event-loop turn.
        if (!current_ || !clients_.count(current_)) fail("events need a socket connection (not --batch)");
        QSet<QString> kinds;
        for (QJsonValue v : p.value("kinds").toArray()) kinds.insert(v.toString());
        if (kinds.isEmpty()) kinds.insert("all");
        clients_[current_].kinds = kinds;
        QStringList list(kinds.begin(), kinds.end());
        list.sort();
        return QJsonObject{{"subscribed", QJsonArray::fromStringList(list)}, {"kinds", QJsonArray{"document", "layers", "selection", "history", "tool", "view", "tabs"}}};
    });
    add("events.unsubscribe", [this](const QJsonObject&) {
        if (current_ && clients_.count(current_)) { clients_[current_].kinds.clear(); clients_[current_].pending.clear(); }
        return QJsonObject{{"subscribed", QJsonArray{}}};
    });
    add("tabs.list", [w, tabJson](const QJsonObject&) { QJsonArray a; for (int i = 0; i < w->tabCount(); i++) a.append(tabJson(i)); return a; });
    add("tabs.select", [w, tabJson, tabIndex](const QJsonObject& p) { int i = tabIndex(p); w->selectTab(i); return tabJson(i); });
    add("tabs.new", [w, tabJson](const QJsonObject&) { return tabJson(w->newTab()); });
    add("tabs.close", [w, tabIndex](const QJsonObject& p) {
        int i = tabIndex(p);
        if (w->sessionAt(i)->isModified() && !flag(p, "discard", false)) fail("the tab has unsaved changes; save first or pass discard: true");
        w->closeTabAt(i);
        return QJsonObject{{"tabs", w->tabCount()}, {"currentTab", w->currentTabIndex()}};
    });

    // ---- history
    add("history.info", [session, document](const QJsonObject&) {
        document();
        EditorSession* s = session();
        return QJsonObject{{"canUndo", s->canUndo()}, {"undo", s->undoName()}, {"canRedo", s->canRedo()}, {"redo", s->redoName()}};
    });
    add("history.list", [session, document](const QJsonObject&) {
        document();
        QJsonArray undo, redo;
        for (auto& n : session()->undoNames()) undo.append(qs(n));
        for (auto& n : session()->redoNames()) redo.append(qs(n));
        return QJsonObject{{"undo", undo}, {"redo", redo}, {"note", "undo is oldest first (the last entry is what history.undo reverts); redo is next first"}};
    });
    add("history.undo", [session, document](const QJsonObject& p) {
        document();
        int n = std::max(1, integer(p, "steps", 1)), done = 0;
        for (; done < n && session()->canUndo(); done++) session()->undo();
        return QJsonObject{{"undone", done}, {"undo", session()->undoName()}};
    });
    add("history.redo", [session, document](const QJsonObject& p) {
        document();
        int n = std::max(1, integer(p, "steps", 1)), done = 0;
        for (; done < n && session()->canRedo(); done++) session()->redo();
        return QJsonObject{{"redone", done}, {"redo", session()->redoName()}};
    });

    // ---- test hooks: the pointer gesture on a layer's eye (press, move to another eye, release), synthesised
    add("debug.eye", [w, layer](const QJsonObject& p) {
        LayersPanel* panel = w->layersPanelAt(w->currentTabIndex());
        QToolButton* eye = panel->eyeButton(layer(p).id);
        if (!eye) fail("no eye button for that layer (is the Layers panel showing it?)");
        QToolButton* target = has(p, "to") ? panel->eyeButton(layer(p, "to").id) : eye;
        if (!target) fail("no eye button for 'to'");
        QString action = str(p, "action").toLower();
        QPoint global = target->mapToGlobal(target->rect().center());
        QPointF local = eye->mapFromGlobal(global);
        QEvent::Type type = action == "press" ? QEvent::MouseButtonPress : action == "move" ? QEvent::MouseMove : action == "release" ? QEvent::MouseButtonRelease : QEvent::None;
        if (type == QEvent::None) fail("action must be press, move or release", invalidParams);
        QMouseEvent event(type, local, local, QPointF(global), Qt::LeftButton, type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(eye, &event);
        return QJsonObject{{"sent", action}};
    });

    // ---- tools and view (what the person sees)
    add("tool.select", [session](const QJsonObject& p) {
        static const QMap<QString, Tool> tools{{"move", Tool::Move}, {"marquee", Tool::Marquee}, {"lasso", Tool::Lasso}, {"wand", Tool::Wand}, {"crop", Tool::Crop},
            {"brush", Tool::Brush}, {"healing", Tool::SpotHealing}, {"clone", Tool::CloneStamp}, {"smudge", Tool::Smudge}, {"dodge", Tool::Dodge}, {"bucket", Tool::PaintBucket}, {"gradient", Tool::Gradient},
            {"shape", Tool::Shape}, {"eyedropper", Tool::Eyedropper}, {"hand", Tool::Hand}, {"zoom", Tool::Zoom},
            {"quickselect", Tool::Scribble}, {"text", Tool::Text}};
        QString name = str(p, "name").toLower();
        if (!tools.contains(name)) fail("unknown tool; one of " + toolNames().join(", "), invalidParams);
        session()->selectTool(tools.value(name));
        return QJsonObject{{"tool", name}};
    });
    add("colors.set", [session](const QJsonObject& p) {
        EditorSession* s = session();
        if (has(p, "foreground")) { QColor c(str(p, "foreground")); if (!c.isValid()) fail("foreground must be a CSS colour", invalidParams); s->foregroundColor = c; }
        if (has(p, "background")) { QColor c(str(p, "background")); if (!c.isValid()) fail("background must be a CSS colour", invalidParams); s->backgroundColor = c; }
        emit s->toolChanged();
        return QJsonObject{{"foreground", s->foregroundColor.name()}, {"background", s->backgroundColor.name()}};
    });
    add("view.zoom", [session, document](const QJsonObject& p) {
        document();
        if (flag(p, "fit", false)) session()->fitView(); else session()->zoomTo(num(p, "zoom"));
        return QJsonObject{{"zoom", session()->viewport.zoom}};
    });
}

} // namespace app
