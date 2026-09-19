#include "Automation.h"
#include "CanvasWidget.h"
#include "EditorSession.h"
#include "ImageConvert.h"
#include "LayersPanel.h"
#include "MainWindow.h"
#include "ModelStore.h"
#include "compositor/filters.h"
#include "compositor/png.h"
#include "compositor/render.h"
#include "compositor/selection.h"
#include "compositor/subject.h"
#include <QApplication>
#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QImageWriter>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocalServer>
#include <QLocalSocket>
#include <QMouseEvent>
#include <QPainter>
#include <QToolButton>
#include <QStandardPaths>
#include <QTimer>
#include <cmath>
#include <stdexcept>

using namespace compositor;

namespace app {

namespace {

struct RpcError : std::runtime_error {
    int code;
    RpcError(int c, const QString& message) : std::runtime_error(message.toStdString()), code(c) {}
};
constexpr int invalidParams = -32602, methodNotFound = -32601, appError = -32000;

[[noreturn]] void fail(const QString& message, int code = appError) { throw RpcError(code, message); }

// ---- Parameter access ---------------------------------------------------------------------------

bool has(const QJsonObject& p, const char* key) { return p.contains(QLatin1String(key)) && !p.value(QLatin1String(key)).isNull(); }

double num(const QJsonObject& p, const char* key, std::optional<double> fallback = std::nullopt) {
    if (!has(p, key)) { if (fallback) return *fallback; fail(QStringLiteral("missing number parameter '%1'").arg(key), invalidParams); }
    QJsonValue v = p.value(QLatin1String(key));
    if (!v.isDouble()) fail(QStringLiteral("parameter '%1' must be a number").arg(key), invalidParams);
    return v.toDouble();
}

int integer(const QJsonObject& p, const char* key, std::optional<int> fallback = std::nullopt) {
    return int(std::lround(num(p, key, fallback ? std::optional<double>(*fallback) : std::nullopt)));
}

QString str(const QJsonObject& p, const char* key, std::optional<QString> fallback = std::nullopt) {
    if (!has(p, key)) { if (fallback) return *fallback; fail(QStringLiteral("missing string parameter '%1'").arg(key), invalidParams); }
    QJsonValue v = p.value(QLatin1String(key));
    if (!v.isString()) fail(QStringLiteral("parameter '%1' must be a string").arg(key), invalidParams);
    return v.toString();
}

bool flag(const QJsonObject& p, const char* key, bool fallback) {
    if (!has(p, key)) return fallback;
    QJsonValue v = p.value(QLatin1String(key));
    if (!v.isBool()) fail(QStringLiteral("parameter '%1' must be true or false").arg(key), invalidParams);
    return v.toBool();
}

QJsonObject obj(const QJsonObject& p, const char* key) {
    if (!has(p, key)) return {};
    QJsonValue v = p.value(QLatin1String(key));
    if (!v.isObject()) fail(QStringLiteral("parameter '%1' must be an object").arg(key), invalidParams);
    return v.toObject();
}

QString qs(const std::string& s) { return QString::fromStdString(s); }

SelectionMode selectionMode(const QJsonObject& p) {
    QString m = str(p, "mode", QStringLiteral("replace")).toLower();
    if (m == "replace") return SelectionMode::Replace;
    if (m == "add") return SelectionMode::Add;
    if (m == "subtract") return SelectionMode::Subtract;
    fail("mode must be replace, add or subtract", invalidParams);
}

std::optional<AdjustmentKind> adjustmentKindNamed(QString name) {
    name = name.toLower().remove('/').remove(' ').remove('-');
    for (int i = 0; i < 6; i++) {
        QString n = QString::fromUtf8(adjustmentKindName(AdjustmentKind(i))).toLower().remove('/').remove(' ').remove('-');
        if (n == name) return AdjustmentKind(i);
    }
    return std::nullopt;
}

std::optional<FilterKind> filterKindNamed(QString name) {
    name = name.toLower().remove(' ').remove('-');
    for (int i = 0; i < 4; i++) {
        QString n = QString::fromUtf8(filterKindName(FilterKind(i))).toLower().remove(' ').remove('-');
        if (n == name) return FilterKind(i);
    }
    if (name == "gaussian" || name == "blur") return FilterKind::GaussianBlur;
    if (name == "motion") return FilterKind::MotionBlur;
    if (name == "noise") return FilterKind::AddNoise;
    if (name == "lens") return FilterKind::LensCorrection;
    return std::nullopt;
}

// ---- JSON views of the model --------------------------------------------------------------------

QJsonObject rectJson(const Rect& r) { return {{"x", r.x}, {"y", r.y}, {"width", r.width}, {"height", r.height}}; }

QJsonObject transformJson(const LayerTransform& t) {
    return {{"x", t.origin.x}, {"y", t.origin.y}, {"width", t.size.width}, {"height", t.size.height},
            {"rotation", t.rotation}, {"flipX", t.flipX}, {"flipY", t.flipY}, {"sampling", QString::fromUtf8(samplingName(t.sampling))}};
}

QJsonObject layerJson(const Layer& layer, int depth) {
    QJsonObject o{
        {"id", qs(layer.id)}, {"name", qs(layer.name)}, {"depth", depth},
        {"kind", layer.isGroup ? "group" : layer.adjustment ? "adjustment" : layer.isLiveShape() ? "shape" : "pixels"},
        {"visible", layer.visible}, {"opacity", layer.opacity}, {"blend", QString::fromUtf8(blendModeName(layer.blendMode))},
        {"clipping", layer.maskSourceId.has_value()}, {"transform", transformJson(layer.transform)},
    };
    if (layer.parentId) o["parent"] = qs(*layer.parentId);
    if (!layer.isGroup && !layer.adjustment) o["pixelSize"] = QJsonObject{{"width", layer.pixelWidth()}, {"height", layer.pixelHeight()}, {"blank", !layer.asset || !layer.asset->image}};
    if (layer.mask) o["mask"] = QJsonObject{{"enabled", layer.mask->enabled}, {"linked", layer.mask->linked}, {"placed", layer.mask->placement.has_value()}};
    if (layer.adjustment) {
        o["adjustmentKind"] = QString::fromUtf8(adjustmentKindName(layer.adjustment->kind));
        o["adjustment"] = QJsonDocument::fromJson(QByteArray::fromStdString(layer.adjustment->json)).object();
    }
    return o;
}

QJsonArray layersJson(const Document& doc) {
    QJsonArray out;
    for (const HierarchyEntry& e : hierarchyEntries(doc.layers, true)) out.append(layerJson(*e.layer, e.depth));
    return out;
}

QString base64Png(const Image& image) {
    std::vector<uint8_t> bytes;
    std::string error;
    if (!encodePngImage(image, bytes, 0, &error)) fail("PNG encoding failed: " + qs(error));
    return QString::fromLatin1(QByteArray(reinterpret_cast<const char*>(bytes.data()), int(bytes.size())).toBase64());
}

/// `image` as PNG: written to params.path when given (result carries the path), else base64 in "png".
QJsonObject deliverPng(const Image& image, const QJsonObject& p, QJsonObject result) {
    result["width"] = image.width();
    result["height"] = image.height();
    if (has(p, "path")) {
        QString path = str(p, "path");
        std::string error;
        if (!writePngImage(path.toStdString(), image, 0, &error)) fail("couldn't write " + path + ": " + qs(error));
        result["path"] = QFileInfo(path).absoluteFilePath();
    } else result["png"] = base64Png(image);
    return result;
}

std::shared_ptr<Image> scaledCopy(const Image& image, double maxSize) {
    int longest = std::max(image.width(), image.height());
    if (maxSize <= 0 || longest <= maxSize) return std::make_shared<Image>(image);
    double scale = maxSize / longest;
    int w = std::max(1, int(std::lround(image.width() * scale))), h = std::max(1, int(std::lround(image.height() * scale)));
    LayerTransform full(Point(0, 0), Size(image.width(), image.height()));
    return resampleLayer(image, full, full, w, h);
}

} // namespace

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
    return runtime + "/compositor-linux.sock";
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
    // Errors the editor would have shown in a dialog come back in the response instead.
    QString captured;
    window_->setErrorSink(&captured);
    try {
        QJsonValue result = it->second(params);
        window_->setErrorSink(nullptr);
        if (!captured.isEmpty()) response["error"] = QJsonObject{{"code", appError}, {"message", captured.trimmed()}};
        else response["result"] = result.isUndefined() ? QJsonValue(QJsonObject{}) : result;
    } catch (const RpcError& e) {
        window_->setErrorSink(nullptr);
        response["error"] = QJsonObject{{"code", e.code}, {"message", QString::fromUtf8(e.what())}};
    } catch (const std::exception& e) {
        window_->setErrorSink(nullptr);
        response["error"] = QJsonObject{{"code", appError}, {"message", QString::fromUtf8(e.what())}};
    }
    return response;
}

// ---- Handlers -----------------------------------------------------------------------------------

void AutomationServer::registerHandlers() {
    MainWindow* w = window_;
    auto session = [w]() -> EditorSession* { return w->session(); };
    auto document = [w]() -> const Document& {
        EditorSession* s = w->session();
        if (!s->hasDocument()) fail("no document is open in the current tab; use document.new or document.open");
        return *s->document();
    };
    auto layer = [document](const QJsonObject& p, const char* key = "id") -> const Layer& {
        const Document& doc = document();
        QString id = str(p, key);
        const Layer* l = doc.find(id.toStdString());
        if (!l) fail("no layer with id " + id);
        return *l;
    };
    auto layerOrActive = [w, document, layer](const QJsonObject& p) -> const Layer& {
        if (has(p, "id")) return layer(p, "id");
        const Layer* l = w->session()->activeLayer();
        if (!l) { document(); fail("no active layer; pass an id or layers.select first"); }
        return *l;
    };
    /// Runs `f` with `id` as the active layer, restoring the previous selection afterwards.
    auto withActive = [w](const Uuid& id, auto f) {
        EditorSession* s = w->session();
        auto previous = s->activeLayerId();
        bool previousMask = s->isMaskSelected();
        if (previous != id) s->selectLayer(id);
        f();
        if (previous && previous != id && s->document() && s->document()->find(*previous)) s->selectLayer(previous, previousMask);
    };
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
    add("app.info", [this, w](const QJsonObject&) {
        return QJsonObject{{"name", "compositor-linux"}, {"version", QApplication::applicationVersion()}, {"socket", path_},
                           {"platform", QApplication::platformName()}, {"tabs", w->tabCount()}, {"currentTab", w->currentTabIndex()},
                           {"removeBackground", ModelStore::ready()}};
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

    // ---- document
    add("document.info", [w, session, document](const QJsonObject&) {
        const Document& doc = document();
        EditorSession* s = session();
        QJsonObject o{{"width", doc.width}, {"height", doc.height}, {"resolution", doc.resolution}, {"layers", int(doc.layers.size())},
                      {"modified", s->isModified()}, {"title", s->title()}, {"tab", w->currentTabIndex()}};
        if (!s->projectPath().isEmpty()) o["path"] = s->projectPath();
        if (auto id = s->activeLayerId()) { o["activeLayer"] = qs(*id); o["maskSelected"] = s->isMaskSelected(); }
        if (doc.selection && !doc.selection->isEmpty()) o["selection"] = rectJson(doc.selection->bounds());
        o["undo"] = s->canUndo() ? QJsonValue(s->undoName()) : QJsonValue::Null;
        o["redo"] = s->canRedo() ? QJsonValue(s->redoName()) : QJsonValue::Null;
        return o;
    });
    add("document.new", [w, session](const QJsonObject& p) {
        int width = integer(p, "width", 1920), height = integer(p, "height", 1080);
        if (!Document::validDimension(width) || !Document::validDimension(height)) fail("width and height must be 1..30000", invalidParams);
        if (session()->hasDocument()) w->newTab();
        session()->createDocument(width, height, num(p, "resolution", 72), flag(p, "emptyLayer", true));
        return QJsonObject{{"tab", w->currentTabIndex()}, {"width", width}, {"height", height}};
    });
    add("document.open", [w, session](const QJsonObject& p) {
        QString path = QFileInfo(str(p, "path")).absoluteFilePath();
        if (!QFileInfo::exists(path)) fail("no such file: " + path, invalidParams);
        w->openPath(path);
        EditorSession* s = session();
        return QJsonObject{{"tab", w->currentTabIndex()}, {"title", s->title()}, {"width", s->hasDocument() ? s->document()->width : 0}, {"height", s->hasDocument() ? s->document()->height : 0}};
    });
    add("document.import", [w](const QJsonObject& p) {
        // An image file as a new layer (a first import creates the canvas).
        QString path = QFileInfo(str(p, "path")).absoluteFilePath(), error;
        std::optional<QPointF> at;
        if (has(p, "x") && has(p, "y")) at = QPointF(num(p, "x"), num(p, "y"));
        if (!w->importImageFile(path, at, &error)) fail(error);
        const Layer* l = w->session()->activeLayer();
        return l ? layerJson(*l, 0) : QJsonObject{};
    });
    add("document.save", [w, session, document](const QJsonObject& p) {
        document();
        QString path = has(p, "path") ? QFileInfo(str(p, "path")).absoluteFilePath() : session()->projectPath();
        if (path.isEmpty()) fail("the document has no path yet; pass path", invalidParams);
        if (!path.endsWith(".comp", Qt::CaseInsensitive)) path += ".comp";
        QString error;
        if (!session()->saveProject(path, &error)) fail(error);
        w->noteRecent(path);
        return QJsonObject{{"path", path}};
    });
    add("document.export", [session, document](const QJsonObject& p) {
        document();
        QString path = QFileInfo(str(p, "path")).absoluteFilePath();
        auto flat = session()->flattened();
        if (!flat) fail("nothing to export");
        QString suffix = QFileInfo(path).suffix().toLower();
        if (suffix == "png") {
            std::string error;
            if (!writePngImage(path.toStdString(), *flat, session()->document()->resolution, &error)) fail("couldn't write " + path + ": " + qs(error));
        } else if (suffix == "jpg" || suffix == "jpeg") {
            QImage image(flat->width(), flat->height(), QImage::Format_RGB32);
            image.fill(QColor(str(p, "background", QStringLiteral("#ffffff"))));
            QPainter painter(&image);
            painter.drawImage(0, 0, wrapImage(*flat));
            painter.end();
            QImageWriter writer(path, "jpeg");
            writer.setQuality(integer(p, "quality", 85));
            if (!writer.write(image)) fail("couldn't write " + path + ": " + writer.errorString());
        } else fail("path must end in .png, .jpg or .jpeg", invalidParams);
        return QJsonObject{{"path", path}, {"width", flat->width()}, {"height", flat->height()}};
    });
    add("document.close", [session](const QJsonObject& p) {
        if (session()->isModified() && !flag(p, "discard", false)) fail("the document has unsaved changes; save first or pass discard: true");
        session()->closeDocument();
        return QJsonObject{};
    });

    // ---- layers
    add("layers.list", [document](const QJsonObject& p) {
        // thumbnails: true adds each pixel layer's 96 px thumbnail (and its mask's) as base64 PNG.
        QJsonArray out = layersJson(document());
        if (!flag(p, "thumbnails", false)) return out;
        const Document& doc = document();
        for (int i = 0; i < out.size(); i++) {
            QJsonObject o = out[i].toObject();
            const Layer* l = doc.find(o["id"].toString().toStdString());
            if (l && l->asset && l->asset->thumbnail) o["thumbnail"] = base64Png(*l->asset->thumbnail);
            if (l && l->mask && l->mask->asset.thumbnail) o["maskThumbnail"] = base64Png(*fromQImage(toQImage(*l->mask->asset.thumbnail)));
            out[i] = o;
        }
        return out;
    });
    add("layers.get", [layer](const QJsonObject& p) { return layerJson(layer(p), 0); });
    add("layers.select", [session, layer](const QJsonObject& p) {
        EditorSession* s = session();
        if (has(p, "ids")) {
            std::set<Uuid> ids;
            for (QJsonValue v : p.value("ids").toArray()) ids.insert(layer(QJsonObject{{"id", v}}).id);
            std::optional<Uuid> primary = has(p, "id") ? std::optional<Uuid>(layer(p).id) : (ids.empty() ? std::nullopt : std::optional<Uuid>(*ids.begin()));
            s->selectLayers(ids, primary);
        } else s->selectLayer(layer(p).id, flag(p, "mask", false));
        return QJsonObject{{"activeLayer", s->activeLayerId() ? qs(*s->activeLayerId()) : QString()}, {"maskSelected", s->isMaskSelected()}};
    });
    add("layers.set", [session, layer, withActive](const QJsonObject& p) {
        const Layer& l = layer(p);
        Uuid id = l.id;
        EditorSession* s = session();
        if (has(p, "name")) s->renameLayer(id, str(p, "name"));
        if (has(p, "visible") && flag(p, "visible", true) != l.visible) s->toggleLayerVisibility(id);
        if (has(p, "opacity") || has(p, "blend") || has(p, "sampling")) {
            std::optional<BlendMode> blend;
            std::optional<Sampling> sampling;
            if (has(p, "blend")) { BlendMode b; if (!parseBlendMode(str(p, "blend").toStdString(), b)) fail("unknown blend mode; use the names layers.list reports", invalidParams); blend = b; }
            if (has(p, "sampling")) { Sampling sm; if (!parseSampling(str(p, "sampling").toStdString(), sm)) fail("sampling must be Nearest, Smooth or High quality", invalidParams); sampling = sm; }
            withActive(id, [&] {
                if (has(p, "opacity")) s->setLayerOpacity(std::clamp(num(p, "opacity"), 0.0, 1.0));
                if (blend) s->setLayerBlendMode(*blend);
                if (sampling) s->setLayerSampling(*sampling);
            });
        }
        if (has(p, "clipping") && flag(p, "clipping", false) != l.maskSourceId.has_value()) {
            if (!s->canToggleClippingMask(id)) fail("this layer can't be clipped (it needs a pixel layer beneath it in the same folder)");
            s->toggleClippingMask(id);
        }
        const Layer* now = s->document()->find(id);
        return now ? layerJson(*now, 0) : QJsonObject{};
    });
    add("layers.add", [session, document](const QJsonObject& p) {
        document();
        EditorSession* s = session();
        QString kind = str(p, "kind", QStringLiteral("pixels")).toLower();
        if (kind == "pixels" || kind == "blank") s->addBlankLayer(flag(p, "below", false));
        else if (kind == "group" || kind == "folder") s->addGroup();
        else if (kind == "adjustment") {
            auto ak = adjustmentKindNamed(str(p, "adjustmentKind"));
            if (!ak) fail("adjustmentKind must be Levels, Curves, Hue/Saturation, Exposure, Gradient Map or Grain", invalidParams);
            s->addAdjustmentLayer(*ak);
            if (has(p, "settings")) {
                const Layer* l = s->activeLayer();
                QJsonObject settings = obj(p, "settings");
                settings["kind"] = QString::fromUtf8(adjustmentKindName(*ak));
                AdjustmentSettings parsed;
                if (!l || !AdjustmentSettings::parse(QJsonDocument(settings).toJson(QJsonDocument::Compact).toStdString(), parsed)) fail("couldn't parse settings", invalidParams);
                s->setAdjustment(l->id, parsed);
            }
        } else fail("kind must be pixels, group or adjustment", invalidParams);
        if (has(p, "name") && s->activeLayer()) s->renameLayer(s->activeLayer()->id, str(p, "name"));
        const Layer* l = s->activeLayer();
        return l ? layerJson(*l, 0) : QJsonObject{};
    });
    add("layers.delete", [session, layer](const QJsonObject& p) {
        std::vector<Uuid> ids;
        if (has(p, "ids")) for (QJsonValue v : p.value("ids").toArray()) ids.push_back(layer(QJsonObject{{"id", v}}).id);
        else ids.push_back(layer(p).id);
        session()->deleteLayersResolvingClipping(ids, flag(p, "bakeClipping", true));
        return QJsonObject{{"deleted", int(ids.size())}};
    });
    add("layers.duplicate", [session, layerOrActive, withActive](const QJsonObject& p) {
        Uuid id = layerOrActive(p).id;
        withActive(id, [&] { session()->duplicateActiveLayer(); });
        // The duplicate is placed above and made active by duplicateActiveLayer; report it.
        const Layer* l = session()->activeLayer();
        return l ? layerJson(*l, 0) : QJsonObject{};
    });
    add("layers.move", [session, layer](const QJsonObject& p) {
        // Into `parent` (or the top level) directly above `above` (or at the bottom / top).
        const Layer& l = layer(p);
        std::optional<Uuid> parent, above;
        if (has(p, "parent")) parent = layer(p, "parent").id;
        if (has(p, "above")) above = layer(p, "above").id;
        if (!session()->placeLayer(l.id, parent, above, flag(p, "atBottom", false))) fail("that placement isn't allowed (a folder can't move into itself)");
        return QJsonObject{{"moved", true}};
    });
    add("layers.reorder", [session, layerOrActive, withActive](const QJsonObject& p) {
        Uuid id = layerOrActive(p).id;
        int offset = integer(p, "offset");
        withActive(id, [&] { if (session()->canMoveActiveLayer(offset)) session()->moveActiveLayer(offset); else fail("can't move the layer that far"); });
        return QJsonObject{{"moved", true}};
    });
    add("layers.setTransform", [session, layerOrActive, withActive](const QJsonObject& p) {
        const Layer& l = layerOrActive(p);
        LayerTransform t = l.transform;
        if (has(p, "x")) t.origin.x = num(p, "x");
        if (has(p, "y")) t.origin.y = num(p, "y");
        if (has(p, "width")) t.size.width = std::max(1.0, num(p, "width"));
        if (has(p, "height")) t.size.height = std::max(1.0, num(p, "height"));
        if (has(p, "rotation")) t.rotation = num(p, "rotation");
        if (has(p, "flipX")) t.flipX = flag(p, "flipX", false);
        if (has(p, "flipY")) t.flipY = flag(p, "flipY", false);
        if (has(p, "scale")) { double k = num(p, "scale"); Point c = t.center(); t.size = {std::max(1.0, t.size.width * k), std::max(1.0, t.size.height * k)}; t.origin = {c.x - t.size.width / 2, c.y - t.size.height / 2}; }
        Uuid id = l.id;
        withActive(id, [&] {
            EditorSession* s = session();
            if (!s->canTransform()) fail("this layer can't be transformed");
            s->beginTransform(true);
            s->previewTransform(t);
            s->commitTransform();
        });
        const Layer* now = session()->document()->find(id);
        return now ? transformJson(now->transform) : QJsonObject{};
    });
    add("layers.flip", [session, layerOrActive, withActive](const QJsonObject& p) {
        Uuid id = layerOrActive(p).id;
        withActive(id, [&] { session()->flipLayer(!flag(p, "vertical", false)); });
        return QJsonObject{{"flipped", true}};
    });
    add("layers.mask", [session, layerOrActive, withActive](const QJsonObject& p) {
        // action: add (revealing all, or hiding), addFromSelection, delete, toggle, invert, apply, link
        Uuid id = layerOrActive(p).id;
        QString action = str(p, "action").toLower();
        withActive(id, [&] {
            EditorSession* s = session();
            if (action == "add") s->addLayerMask(flag(p, "revealing", true));
            else if (action == "addfromselection") s->addMaskFromSelection(flag(p, "revealing", true));
            else if (action == "delete") s->deleteLayerMask();
            else if (action == "toggle") s->toggleLayerMask();
            else if (action == "invert") s->invertMask();
            else if (action == "apply") s->applyMask();
            else if (action == "link") s->toggleMaskLink(id);
            else fail("action must be add, addFromSelection, delete, toggle, invert, apply or link", invalidParams);
        });
        const Layer* now = session()->document()->find(id);
        return now ? layerJson(*now, 0) : QJsonObject{};
    });
    add("layers.merge", [session, document](const QJsonObject& p) {
        document();
        EditorSession* s = session();
        if (flag(p, "down", false)) { s->mergeDown(); return QJsonObject{{"merged", true}}; }
        if (!s->canMergeLayers()) fail("nothing to merge: select two or more layers, a folder, or a layer with one beneath it");
        QString title = s->mergeTitle();
        s->mergeLayers();
        return QJsonObject{{"merged", true}, {"action", title}};
    });
    add("layers.group", [session, document](const QJsonObject&) { document(); session()->groupSelectedLayers(); const Layer* l = session()->activeLayer(); return l ? layerJson(*l, 0) : QJsonObject{}; });
    add("layers.render", [layer](const QJsonObject& p) {
        // A layer's own pixels (not composited), downscaled to maxSize.
        const Layer& l = layer(p);
        if (!l.asset || !l.asset->image) fail("the layer has no pixels");
        auto copy = scaledCopy(*l.asset->image, num(p, "maxSize", 1024));
        return deliverPng(*copy, p, {{"id", qs(l.id)}, {"transform", transformJson(l.transform)}, {"pixelWidth", l.pixelWidth()}, {"pixelHeight", l.pixelHeight()}});
    });

    // ---- adjustment layers
    add("adjustments.get", [layerOrActive](const QJsonObject& p) {
        const Layer& l = layerOrActive(p);
        if (!l.adjustment) fail("not an adjustment layer");
        return QJsonObject{{"id", qs(l.id)}, {"kind", QString::fromUtf8(adjustmentKindName(l.adjustment->kind))},
                           {"settings", QJsonDocument::fromJson(QByteArray::fromStdString(l.adjustment->json)).object()}};
    });
    add("adjustments.set", [session, layerOrActive](const QJsonObject& p) {
        const Layer& l = layerOrActive(p);
        if (!l.adjustment) fail("not an adjustment layer");
        // Merge over the current settings so a partial object works.
        QJsonObject settings = QJsonDocument::fromJson(QByteArray::fromStdString(l.adjustment->json)).object();
        QJsonObject patch = obj(p, "settings");
        for (auto it = patch.begin(); it != patch.end(); ++it) settings[it.key()] = it.value();
        settings["kind"] = QString::fromUtf8(adjustmentKindName(l.adjustment->kind));
        AdjustmentSettings parsed;
        if (!AdjustmentSettings::parse(QJsonDocument(settings).toJson(QJsonDocument::Compact).toStdString(), parsed)) fail("couldn't parse settings; adjustments.get shows the shape", invalidParams);
        session()->setAdjustment(l.id, parsed);
        return QJsonObject{{"settings", QJsonDocument::fromJson(QByteArray::fromStdString(parsed.toJson())).object()}};
    });
    add("adjustments.defaults", [](const QJsonObject& p) {
        auto kind = adjustmentKindNamed(str(p, "kind"));
        if (!kind) fail("kind must be Levels, Curves, Hue/Saturation, Exposure, Gradient Map or Grain", invalidParams);
        return QJsonDocument::fromJson(QByteArray::fromStdString(AdjustmentSettings::defaults(*kind).toJson())).object();
    });

    // ---- destructive pixel edits on the active layer, inside the selection
    add("pixels.adjust", [session, document](const QJsonObject& p) {
        document();
        EditorSession* s = session();
        if (!s->canAdjustPixels()) fail("the active layer has no pixels to adjust; select a pixel layer");
        auto kind = adjustmentKindNamed(str(p, "kind"));
        if (!kind) fail("kind must be Levels, Curves, Hue/Saturation, Exposure, Gradient Map or Grain", invalidParams);
        QJsonObject settings = QJsonDocument::fromJson(QByteArray::fromStdString(AdjustmentSettings::defaults(*kind).toJson())).object();
        QJsonObject patch = obj(p, "settings");
        for (auto it = patch.begin(); it != patch.end(); ++it) settings[it.key()] = it.value();
        settings["kind"] = QString::fromUtf8(adjustmentKindName(*kind));
        AdjustmentSettings parsed;
        if (!AdjustmentSettings::parse(QJsonDocument(settings).toJson(QJsonDocument::Compact).toStdString(), parsed)) fail("couldn't parse settings; adjustments.defaults shows the shape", invalidParams);
        LayerTransform transform;
        auto source = s->adjustmentSource(0, transform);
        if (!source) fail("the active layer has no pixels");
        auto out = std::make_shared<Image>(*source);
        applyAdjustment(parsed, *out, Rect(0, 0, source->width(), source->height()), 1);
        if (auto coverage = s->selectionOnGrid(transform, source->width(), source->height())) blendThroughCoverage(*out, *source, *coverage);
        s->commitPixels(out, transform, QString::fromUtf8(adjustmentKindName(*kind)));
        return QJsonObject{{"applied", QString::fromUtf8(adjustmentKindName(*kind))}};
    });
    add("pixels.filter", [session, document](const QJsonObject& p) {
        document();
        EditorSession* s = session();
        if (!s->canAdjustPixels()) fail("the active layer has no pixels to filter; select a pixel layer");
        auto kind = filterKindNamed(str(p, "kind"));
        if (!kind) fail("kind must be Gaussian Blur, Motion Blur, Add Noise or Lens Correction", invalidParams);
        FilterSettings settings;
        settings.radius = num(p, "radius", settings.radius);
        settings.angle = num(p, "angle", settings.angle);
        settings.distance = num(p, "distance", settings.distance);
        settings.amount = num(p, "amount", settings.amount);
        settings.gaussian = flag(p, "gaussian", settings.gaussian);
        settings.monochromatic = flag(p, "monochromatic", settings.monochromatic);
        settings.distortion = num(p, "distortion", settings.distortion);
        int margin = int(std::ceil(blurMargin(*kind, settings)));
        LayerTransform transform;
        auto source = s->adjustmentSource(margin, transform);
        if (!source) fail("the active layer has no pixels");
        auto out = std::make_shared<Image>(*source);
        applyFilter(*kind, *out, settings, 1, uint32_t(integer(p, "seed", 1)));
        if (auto coverage = s->selectionOnGrid(transform, source->width(), source->height())) blendThroughCoverage(*out, *source, *coverage);
        LayerTransform placed = transform;
        std::shared_ptr<const Image> image = out;
        if (*kind == FilterKind::GaussianBlur || *kind == FilterKind::MotionBlur) image = trimToPixels(*out, transform, placed);
        s->commitPixels(image, placed, QString::fromUtf8(filterKindName(*kind)));
        return QJsonObject{{"applied", QString::fromUtf8(filterKindName(*kind))}};
    });
    add("pixels.invert", [session, document](const QJsonObject&) { document(); session()->invertActive(); return QJsonObject{}; });
    add("pixels.fill", [session, document](const QJsonObject& p) {
        document();
        QColor color(str(p, "color", QStringLiteral("#000000")));
        if (!color.isValid()) fail("color must be a CSS colour such as #ff8800", invalidParams);
        session()->fillSelection(color);
        return QJsonObject{};
    });
    add("pixels.clear", [session, document](const QJsonObject&) { document(); session()->clearSelectionPixels(); return QJsonObject{}; });
    add("pixels.contentAwareFill", [session, document](const QJsonObject&) {
        document();
        QString error;
        if (!session()->contentAwareFill(&error)) fail(error.isEmpty() ? "content-aware fill needs a selection on a pixel layer" : error);
        return QJsonObject{};
    });
    add("pixels.removeBackground", [session, document](const QJsonObject& p) {
        document();
        if (!ModelStore::supported()) fail("this build has no OpenCV, so the segmentation model can't run");
        if (!ModelStore::ready()) fail("Remove Background is off or its model isn't downloaded: enable it in Edit > Preferences (or run compositor-linux --download-model isnet)");
        EditorSession* s = session();
        LayerTransform transform;
        auto source = s->adjustmentSource(0, transform);
        if (!source) fail("the active layer has no pixels");
        std::string error;
        auto mask = subjectMask(*source, ModelStore::pathFor(ModelStore::selected()).toStdString(), &error);
        if (!mask) fail("the model failed: " + qs(error));
        if (flag(p, "refine", true)) {
            MatteSettings settings;
            settings.refineEdges = num(p, "refineEdges", settings.refineEdges);
            settings.contrast = num(p, "contrast", settings.contrast);
            settings.shiftEdge = num(p, "shiftEdge", settings.shiftEdge);
            mask = refineMatte(*mask, *source, settings, 0);
        }
        s->applySubjectMask(mask);
        return QJsonObject{{"applied", true}};
    });

    // ---- selection (document pixels)
    add("selection.info", [document](const QJsonObject&) {
        const Document& doc = document();
        if (!doc.selection || doc.selection->isEmpty()) return QJsonObject{{"active", false}};
        return QJsonObject{{"active", true}, {"bounds", rectJson(doc.selection->bounds())}, {"antialiased", doc.selection->antialiased}};
    });
    add("selection.render", [document](const QJsonObject& p) {
        // The selection as a mask image: white selected, black not, downscaled to maxSize.
        const Document& doc = document();
        if (!doc.selection || !doc.selection->coverage) fail("there is no selection");
        QImage mask = toQImage(*doc.selection->coverage);
        double maxSize = num(p, "maxSize", 1024);
        if (maxSize > 0 && std::max(mask.width(), mask.height()) > maxSize) mask = mask.scaled(int(maxSize), int(maxSize), Qt::KeepAspectRatio, Qt::SmoothTransformation);
        return deliverPng(*fromQImage(mask), p, {{"bounds", rectJson(doc.selection->bounds())}});
    });
    add("selection.all", [session, document](const QJsonObject&) { document(); session()->selectAll(); return QJsonObject{}; });
    add("selection.none", [session, document](const QJsonObject&) { document(); session()->deselect(); return QJsonObject{}; });
    add("selection.invert", [session, document](const QJsonObject&) { document(); session()->invertSelection(); return QJsonObject{}; });
    add("selection.rect", [session, document](const QJsonObject& p) {
        const Document& doc = document();
        Rect r(num(p, "x"), num(p, "y"), num(p, "width"), num(p, "height"));
        auto shape = flag(p, "ellipse", false) ? rasterizeEllipse(r, doc.width, doc.height, session()->selectionAntialiased) : rasterizeRect(r, doc.width, doc.height, session()->selectionAntialiased);
        session()->applySelectionShape(*shape, selectionMode(p), flag(p, "ellipse", false) ? "Ellipse" : "Marquee");
        return QJsonObject{{"bounds", rectJson(doc.selection ? doc.selection->bounds() : Rect())}};
    });
    add("selection.polygon", [session, document](const QJsonObject& p) {
        const Document& doc = document();
        std::vector<Point> points;
        for (QJsonValue v : p.value("points").toArray()) {
            QJsonArray pt = v.toArray();
            if (pt.size() == 2) points.push_back({pt[0].toDouble(), pt[1].toDouble()});
            else { QJsonObject o = v.toObject(); points.push_back({num(o, "x"), num(o, "y")}); }
        }
        if (points.size() < 3) fail("points needs at least three [x, y] pairs", invalidParams);
        auto shape = rasterizePolygon(points, doc.width, doc.height, session()->selectionAntialiased);
        session()->applySelectionShape(*shape, selectionMode(p), "Lasso");
        return QJsonObject{{"bounds", rectJson(doc.selection ? doc.selection->bounds() : Rect())}};
    });
    add("selection.wand", [session, document](const QJsonObject& p) {
        const Document& doc = document();
        EditorSession* s = session();
        s->magicWand(QPointF(num(p, "x"), num(p, "y")), integer(p, "tolerance", s->wandTolerance), flag(p, "contiguous", s->wandContiguous), flag(p, "sampleAll", s->wandSampleAll), selectionMode(p));
        return QJsonObject{{"bounds", rectJson(doc.selection ? doc.selection->bounds() : Rect())}};
    });
    add("selection.fromLayer", [session, layerOrActive, document](const QJsonObject& p) {
        const Document& doc = document();
        session()->loadLayerAsSelection(layerOrActive(p).id, flag(p, "mask", false), selectionMode(p));
        return QJsonObject{{"bounds", rectJson(doc.selection ? doc.selection->bounds() : Rect())}};
    });
    add("selection.grow", [session, document](const QJsonObject& p) {
        const Document& doc = document();
        int amount = integer(p, "amount");
        if (amount >= 0) session()->selectionExpand(amount); else session()->selectionContract(-amount);
        return QJsonObject{{"bounds", rectJson(doc.selection ? doc.selection->bounds() : Rect())}};
    });

    // ---- canvas
    add("canvas.resize", [session, document](const QJsonObject& p) {
        document();
        int width = integer(p, "width"), height = integer(p, "height");
        if (!Document::validDimension(width) || !Document::validDimension(height)) fail("width and height must be 1..30000", invalidParams);
        session()->resizeCanvas(width, height, std::clamp(num(p, "anchorX", 0.5), 0.0, 1.0), std::clamp(num(p, "anchorY", 0.5), 0.0, 1.0));
        return QJsonObject{{"width", session()->document()->width}, {"height", session()->document()->height}};
    });
    add("canvas.crop", [session, document](const QJsonObject& p) {
        document();
        session()->cropTo(QRectF(num(p, "x"), num(p, "y"), num(p, "width"), num(p, "height")));
        return QJsonObject{{"width", session()->document()->width}, {"height", session()->document()->height}};
    });
    add("canvas.flip", [session, document](const QJsonObject& p) { document(); session()->flipCanvas(!flag(p, "vertical", false)); return QJsonObject{}; });
    add("image.resize", [session, document](const QJsonObject& p) {
        const Document& doc = document();
        int width = integer(p, "width", 0), height = integer(p, "height", 0);
        if (has(p, "scale")) { double k = num(p, "scale"); width = int(std::lround(doc.width * k)); height = int(std::lround(doc.height * k)); }
        else if (width > 0 && height <= 0) height = int(std::lround(double(width) * doc.height / doc.width));
        else if (height > 0 && width <= 0) width = int(std::lround(double(height) * doc.width / doc.height));
        if (!Document::validDimension(width) || !Document::validDimension(height)) fail("give width and/or height (1..30000) or scale", invalidParams);
        QString sampling = str(p, "sampling", QStringLiteral("high")).toLower();
        int mode = sampling.startsWith("near") ? 0 : sampling.startsWith("smooth") || sampling == "bilinear" ? 1 : 2;
        session()->resizeImage(width, height, num(p, "resolution", doc.resolution), mode);
        return QJsonObject{{"width", session()->document()->width}, {"height", session()->document()->height}};
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

    // ---- seeing the result
    add("render", [session, document](const QJsonObject& p) {
        // The composite (what an export would give) or a region of it, downscaled so its longest side is maxSize.
        const Document& doc = document();
        Rect region = doc.rect();
        if (has(p, "region")) {
            QJsonObject r = obj(p, "region");
            region = Rect(num(r, "x"), num(r, "y"), num(r, "width"), num(r, "height"));
            region = region.intersection(doc.rect());
            if (region.width < 1 || region.height < 1) fail("region lies outside the document", invalidParams);
        }
        double maxSize = num(p, "maxSize", 1024);
        double scale = maxSize > 0 ? std::min(1.0, maxSize / std::max(region.width, region.height)) : 1.0;
        int w = std::max(1, int(std::lround(region.width * scale))), h = std::max(1, int(std::lround(region.height * scale)));
        Image out(w, h);
        RenderOptions options;
        options.region = region;
        options.scale = scale;
        Overrides overrides = session()->renderOverrides();
        render(doc, options, out, &overrides);
        if (flag(p, "checkerboard", false)) {
            // Transparency over a checkerboard, as the canvas shows it.
            QImage flat(w, h, QImage::Format_RGB32);
            QPainter painter(&flat);
            for (int y = 0; y < h; y += 8) for (int x = 0; x < w; x += 8) painter.fillRect(x, y, 8, 8, ((x / 8 + y / 8) % 2) ? QColor(204, 204, 204) : Qt::white);
            painter.drawImage(0, 0, wrapImage(out));
            painter.end();
            out = *fromQImage(flat);
        }
        return deliverPng(out, p, {{"region", rectJson(region)}, {"scale", scale}});
    });
    add("screenshot", [w, session](const QJsonObject& p) {
        QImage image = (flag(p, "window", false) ? w->grab() : w->canvasAt(w->currentTabIndex())->grab()).toImage();
        double maxSize = num(p, "maxSize", 1600);
        if (maxSize > 0 && std::max(image.width(), image.height()) > maxSize) image = image.scaled(int(maxSize), int(maxSize), Qt::KeepAspectRatio, Qt::SmoothTransformation);
        return deliverPng(*fromQImage(image), p, {{"zoom", session()->viewport.zoom}});
    });

    // ---- painting by coordinates (document pixels); the person's tool and settings are put back afterwards
    auto pointList = [](const QJsonObject& p, const char* key) {
        std::vector<QPointF> pts;
        for (QJsonValue v : p.value(QLatin1String(key)).toArray()) {
            if (v.isArray()) { QJsonArray a = v.toArray(); if (a.size() == 2) pts.emplace_back(a[0].toDouble(), a[1].toDouble()); }
            else { QJsonObject o = v.toObject(); pts.emplace_back(num(o, "x"), num(o, "y")); }
        }
        return pts;
    };
    add("brush.stroke", [session, document, pointList](const QJsonObject& p) {
        document();
        EditorSession* s = session();
        std::vector<QPointF> pts = pointList(p, "points");
        if (pts.empty()) fail("points needs at least one [x, y]", invalidParams);
        QString tool = str(p, "tool", QStringLiteral("brush")).toLower();
        Tool previousTool = s->tool();
        BrushSettings previousBrush = s->brushSettings;
        bool previousErase = s->brushErase, previousMask = s->isMaskSelected();
        QColor previousColor = s->foregroundColor;
        BlurToolMode previousBlur = s->blurMode;
        auto previousClone = s->cloneSource;
        auto previousActive = s->activeLayerId();
        auto restore = [&] {
            s->brushSettings = previousBrush; s->brushErase = previousErase; s->foregroundColor = previousColor; s->blurMode = previousBlur; s->cloneSource = previousClone;
            if (previousActive && s->document() && s->document()->find(*previousActive)) s->selectLayer(previousActive, previousMask);
            s->selectTool(previousTool);
        };
        if (has(p, "size")) s->brushSettings.diameter = std::clamp(num(p, "size"), 1.0, 2000.0);
        if (has(p, "hardness")) s->brushSettings.hardness = std::clamp(num(p, "hardness"), 0.0, 1.0);
        if (has(p, "opacity")) s->brushSettings.opacity = std::clamp(num(p, "opacity"), 0.0, 1.0);
        if (has(p, "color")) { QColor c(str(p, "color")); if (!c.isValid()) { restore(); fail("color must be a CSS colour", invalidParams); } s->foregroundColor = c; }
        if (has(p, "mask") && s->activeLayerId()) s->selectLayer(s->activeLayerId(), flag(p, "mask", false));
        bool warp = false;
        if (tool == "brush" || tool == "eraser") { s->selectTool(Tool::Brush); s->brushErase = tool == "eraser" || flag(p, "erase", false); }
        else if (tool == "healing") s->selectTool(Tool::SpotHealing);
        else if (tool == "clone") {
            s->selectTool(Tool::CloneStamp);
            if (has(p, "source")) { QJsonObject src = obj(p, "source"); s->setCloneSource(QPointF(num(src, "x"), num(src, "y"))); }
        } else if (tool == "smudge" || tool == "blur" || tool == "liquify") {
            s->selectTool(Tool::Smudge);
            s->blurMode = tool == "blur" ? BlurToolMode::Blur : tool == "smudge" ? BlurToolMode::Smudge : BlurToolMode::Liquify;
            warp = true;
        } else { restore(); fail("tool must be brush, eraser, healing, clone, smudge, blur or liquify", invalidParams); }
        bool started = warp ? s->beginWarp(pts[0]) : s->beginBrush(pts[0], false);
        if (!started) { restore(); fail("couldn't start the stroke: the active layer must have pixels (clone needs a source; healing and clone can't paint a mask)"); }
        for (size_t i = 1; i < pts.size(); i++) { if (warp) s->continueWarp(pts[i]); else s->continueBrush(pts[i]); }
        if (warp) s->endWarp(); else s->endBrush();
        restore();
        return QJsonObject{{"points", int(pts.size())}, {"tool", tool}};
    });
    add("gradient.draw", [session, document](const QJsonObject& p) {
        document();
        EditorSession* s = session();
        GradientSettings previous = s->gradientSettings;
        QColor fg = s->foregroundColor, bg = s->backgroundColor;
        auto restore = [&] { s->gradientSettings = previous; s->foregroundColor = fg; s->backgroundColor = bg; };
        QString shape = str(p, "shape", QStringLiteral("linear")).toLower();
        QString style = str(p, "style", QStringLiteral("foreground-to-transparent")).toLower();
        s->gradientSettings.shape = shape == "radial" ? GradientShape::Radial : GradientShape::Linear;
        s->gradientSettings.style = style.contains("background") ? GradientStyle::ForegroundToBackground : GradientStyle::ForegroundToTransparent;
        s->gradientSettings.reversed = flag(p, "reversed", false);
        s->gradientSettings.opacity = std::clamp(num(p, "opacity", 1), 0.0, 1.0);
        if (has(p, "foreground")) s->foregroundColor = QColor(str(p, "foreground"));
        if (has(p, "background")) s->backgroundColor = QColor(str(p, "background"));
        QPointF a(num(p, "x0"), num(p, "y0")), b(num(p, "x1"), num(p, "y1"));
        s->beginGradient(a);
        if (!s->gradientPending()) { restore(); fail("couldn't start a gradient: select a pixel layer (or its mask)"); }
        s->moveGradient(b);
        s->endGradientDrag();
        s->commitGradient();
        restore();
        return QJsonObject{{"drawn", true}};
    });
    add("shape.draw", [session, document](const QJsonObject& p) {
        // A new shape layer; kind rectangle (with cornerRadius) or ellipse, in color (default: the foreground).
        document();
        EditorSession* s = session();
        ShapeKind previousKind = s->shapeKind;
        double previousRadius = s->shapeCornerRadius;
        QColor fg = s->foregroundColor;
        auto restore = [&] { s->shapeKind = previousKind; s->shapeCornerRadius = previousRadius; s->foregroundColor = fg; };
        QString kind = str(p, "kind", QStringLiteral("rectangle")).toLower();
        s->shapeKind = kind.startsWith("ell") || kind == "circle" ? ShapeKind::Ellipse : ShapeKind::Rectangle;
        s->shapeCornerRadius = std::max(0.0, num(p, "cornerRadius", 0));
        if (has(p, "color")) { QColor c(str(p, "color")); if (!c.isValid()) { restore(); fail("color must be a CSS colour", invalidParams); } s->foregroundColor = c; }
        double x = num(p, "x"), y = num(p, "y"), w = num(p, "width"), h = num(p, "height");
        s->beginShape(QPointF(x, y));
        if (!s->shapeDraft()) { restore(); fail("couldn't start a shape: the document has no editable layer"); }
        s->dragShape(QPointF(x + w, y + h), false, false);
        s->finishShape();
        restore();
        const Layer* l = s->activeLayer();
        return l ? layerJson(*l, 0) : QJsonObject{};
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
            {"brush", Tool::Brush}, {"healing", Tool::SpotHealing}, {"clone", Tool::CloneStamp}, {"smudge", Tool::Smudge}, {"gradient", Tool::Gradient},
            {"shape", Tool::Shape}, {"eyedropper", Tool::Eyedropper}, {"hand", Tool::Hand}, {"zoom", Tool::Zoom}};
        QString name = str(p, "name").toLower();
        if (!tools.contains(name)) fail("unknown tool; one of " + QStringList(tools.keys()).join(", "), invalidParams);
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
