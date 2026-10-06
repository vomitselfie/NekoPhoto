// Automation for actions (actions.*: list, record, play, batch, import, export, delete) and the frame animation
// timeline (timeline.*). See docs/actions.md and docs/animation.md.
#include "ActionLibrary.h"
#include "Automation.h"
#include "AutomationHandlers.h"
#include "compositor/animation.h"
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QPointer>

using namespace compositor;
using namespace app::rpc;

namespace app {

namespace {

QJsonObject actionJson(const RecordedAction& a) {
    QJsonObject o = ActionLibrary::toJson(a);
    QJsonArray steps = o.value("steps").toArray();
    for (int i = 0; i < steps.size(); i++) {
        QJsonObject s = steps[i].toObject();
        s["label"] = ActionLibrary::describe(a.steps[size_t(i)]);
        s["enabled"] = a.steps[size_t(i)].enabled;
        steps[i] = s;
    }
    o["steps"] = steps;
    return o;
}

/// Methods that leave the document the action started in (their steps cannot merge into one undo step).
bool leavesDocument(const QString& method) {
    return method.startsWith("tabs.") || method == "document.open" || method == "document.new" || method == "document.close" || method == "rpc.batch";
}

const QStringList batchFormats = {"png", "jpg", "webp", "tif", "psd", "gif", "tga"};

} // namespace

QJsonObject AutomationServer::playAction(const QString& name) {
    const RecordedAction* found = ActionLibrary::instance().find(name);
    if (!found) fail("there is no action named '" + name + "'; actions.list shows them", invalidParams);
    const RecordedAction action = *found;   // the library may change while it plays (a step that imports actions)
    ActionLibrary::Quiet quiet;
    QPointer<EditorSession> session = window_->session();
    const uint64_t since = session ? session->historyRevision() : 0;
    bool merge = true;
    int played = 0;
    for (size_t i = 0; i < action.steps.size(); i++) {
        const ActionStep& step = action.steps[i];
        if (!step.enabled) continue;
        merge = merge && !leavesDocument(step.method);
        const QJsonObject reply = handle({{"jsonrpc", "2.0"}, {"id", int(i)}, {"method", step.method}, {"params", step.params}});
        if (reply.contains("error")) {
            return {{"action", name}, {"played", played}, {"completed", false},
                    {"error", QJsonObject{{"index", int(i)}, {"method", step.method}, {"message", reply["error"].toObject()["message"]}}}};
        }
        played++;
    }
    QJsonObject out{{"action", name}, {"played", played}, {"completed", true}};
    if (merge && played > 1 && session && session == window_->session()) out["merged"] = session->squashHistory(since, name);
    return out;
}

QJsonObject AutomationServer::runBatch(const QString& actionName, const QString& input, const QString& output, const QString& formatName, bool overwrite,
                                       const std::function<bool(int, int, const QString&)>& progress) {
    if (!ActionLibrary::instance().find(actionName)) fail("there is no action named '" + actionName + "'; actions.list shows them", invalidParams);
    const QString format = formatName.toLower() == "jpeg" ? QStringLiteral("jpg") : formatName.toLower() == "tiff" ? QStringLiteral("tif") : formatName.toLower();
    if (!batchFormats.contains(format)) fail("format must be one of " + batchFormats.join(", "), invalidParams);
    const QDir in(input);
    if (input.isEmpty() || !in.exists()) fail("the input folder does not exist: " + input, invalidParams);
    if (output.isEmpty() || !QDir().mkpath(output)) fail("couldn't make the output folder " + output, invalidParams);
    const QDir out(output);
    const QStringList patterns = {"*.png", "*.jpg", "*.jpeg", "*.webp", "*.tif", "*.tiff", "*.bmp", "*.gif", "*.tga", "*.psd", "*.psb", "*.nekophoto", "*.comp", "*.ase", "*.aseprite", "*.clip", "*.ico"};
    QStringList files;
    for (const QFileInfo& f : in.entryInfoList(patterns, QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::Readable, QDir::Name | QDir::IgnoreCase))
        if (f.isFile() || f.suffix().compare("comp", Qt::CaseInsensitive) == 0) files << f.absoluteFilePath();   // .comp projects are folders
    QJsonArray done, failed, skipped;
    const int startTabs = window_->tabCount();
    auto call = [this](const QString& method, const QJsonObject& params) {
        return handle({{"jsonrpc", "2.0"}, {"id", 0}, {"method", method}, {"params", params}});
    };
    auto closeExtraTabs = [this, startTabs] {
        for (int i = window_->tabCount() - 1; i >= startTabs && window_->tabCount() > startTabs; i--) window_->closeTabAt(i);
    };
    for (int n = 0; n < files.size(); n++) {
        const QString& file = files[n];
        if (progress && !progress(n, int(files.size()), file)) { skipped.append(QFileInfo(file).fileName()); continue; }
        QString target = out.absoluteFilePath(QFileInfo(file).completeBaseName() + "." + format);
        if (QFileInfo::exists(target) && !overwrite) { skipped.append(QFileInfo(file).fileName()); continue; }
        auto failure = [&](const QString& stage, const QJsonObject& reply) {
            failed.append(QJsonObject{{"file", QFileInfo(file).fileName()}, {"stage", stage}, {"message", reply.value("error").toObject().value("message").toString(reply.value("result").toObject().value("error").toObject().value("message").toString())}});
            closeExtraTabs();
        };
        QJsonObject reply;
        {
            ActionLibrary::Quiet quiet;
            reply = call("tabs.new", {});
            if (!reply.contains("error")) reply = call("document.open", {{"path", file}});
        }
        if (reply.contains("error")) { failure("open", reply); continue; }
        const QJsonObject played = playAction(actionName);
        if (!played.value("completed").toBool()) { failure("play", QJsonObject{{"error", played.value("error")}}); continue; }
        {
            ActionLibrary::Quiet quiet;
            reply = call("document.export", {{"path", target}});
        }
        if (reply.contains("error")) { failure("export", reply); continue; }
        done.append(QFileInfo(target).fileName());
        closeExtraTabs();
    }
    if (progress) progress(int(files.size()), int(files.size()), QString());
    return {{"action", actionName}, {"files", int(files.size())}, {"written", done}, {"failed", failed}, {"skipped", skipped}, {"output", out.absolutePath()}};
}

void AutomationServer::registerActionsHandlers() {
    add("actions.list", [](const QJsonObject& p) {
        ActionLibrary& library = ActionLibrary::instance();
        QJsonArray list;
        for (const RecordedAction& a : library.actions()) {
            if (has(p, "name") && a.name != str(p, "name")) continue;
            list.append(actionJson(a));
        }
        return QJsonObject{{"actions", list}, {"recording", library.recording()}, {"recordingName", library.recordingName()}, {"file", ActionLibrary::filePath()}};
    });
    add("actions.record", [](const QJsonObject& p) {
        ActionLibrary& library = ActionLibrary::instance();
        const QString what = str(p, "action").toLower();
        if (what == "start") {
            if (library.recording()) fail("already recording '" + library.recordingName() + "'; stop first");
            const QString name = str(p, "name").trimmed();
            if (name.isEmpty()) fail("start needs a name", invalidParams);
            library.startRecording(name);
            return QJsonObject{{"recording", true}, {"name", name}};
        }
        if (what == "stop") {
            const QString name = library.recordingName();
            if (name.isEmpty()) fail("nothing is recording");
            const int steps = library.stopRecording();
            return QJsonObject{{"recording", false}, {"name", name}, {"added", steps}};
        }
        fail("action must be start or stop", invalidParams);
    });
    add("actions.play", [this](const QJsonObject& p) {
        const int times = std::clamp(integer(p, "times", 1), 1, 1000);
        QJsonObject last;
        for (int i = 0; i < times; i++) {
            last = playAction(str(p, "name"));
            if (!last.value("completed").toBool()) break;
        }
        return last;
    });
    add("actions.batch", [this](const QJsonObject& p) {
        return runBatch(str(p, "name"), QFileInfo(str(p, "input")).absoluteFilePath(), QFileInfo(str(p, "output")).absoluteFilePath(),
                        str(p, "format", QStringLiteral("png")), flag(p, "overwrite", false));
    });
    add("actions.save", [](const QJsonObject& p) {
        // A whole action from JSON: new, or replacing the one with its name (how an agent edits steps).
        QString error;
        QJsonObject json{{"name", str(p, "name")}, {"steps", p.value("steps")}};
        auto action = ActionLibrary::fromJson(json, &error);
        if (!action) fail(error, invalidParams);
        for (const ActionStep& s : action->steps)
            if (s.method.startsWith("actions.")) fail("a step can't be an actions method (" + s.method + ")", invalidParams);
        ActionLibrary::instance().put(*action);
        return actionJson(*action);
    });
    add("actions.delete", [](const QJsonObject& p) {
        if (!ActionLibrary::instance().remove(str(p, "name"))) fail("there is no action named '" + str(p, "name") + "'", invalidParams);
        return QJsonObject{{"deleted", str(p, "name")}};
    });
    add("actions.import", [](const QJsonObject& p) {
        QString error;
        const QStringList names = ActionLibrary::instance().importFile(QFileInfo(str(p, "path")).absoluteFilePath(), &error);
        if (names.isEmpty()) fail("couldn't import: " + error);
        return QJsonObject{{"imported", QJsonArray::fromStringList(names)}};
    });
    add("actions.export", [](const QJsonObject& p) {
        QStringList names;
        for (const QJsonValue& v : p.value("names").toArray()) names << v.toString();
        if (has(p, "name")) names << str(p, "name");
        if (names.isEmpty()) for (const RecordedAction& a : ActionLibrary::instance().actions()) names << a.name;
        QString error;
        const QString path = QFileInfo(str(p, "path")).absoluteFilePath();
        if (!ActionLibrary::instance().exportFile(names, path, &error)) fail(error);
        return QJsonObject{{"path", path}, {"actions", QJsonArray::fromStringList(names)}};
    });
}

void AutomationServer::registerTimelineHandlers() {
    MainWindow* w = window_;
    const SessionOf session{w};
    const DocumentOf document{w};
    auto info = [document] {
        const Document& d = document();
        QJsonArray frames;
        for (size_t i = 0; i < d.animation.frames.size(); i++) {
            const AnimationFrame& f = d.animation.frames[i];
            QJsonArray shown;
            for (const Layer& l : d.layers) {
                auto it = f.layers.find(l.id);
                if (it != f.layers.end() ? it->second.visible : l.visible) shown.append(qs(l.id));
            }
            frames.append(QJsonObject{{"index", int(i)}, {"delay", f.delayMs}, {"visibleLayers", shown}});
        }
        return QJsonObject{{"frames", frames}, {"count", int(d.animation.frames.size())}, {"current", d.animation.frames.empty() ? -1 : d.animation.current},
                           {"loopCount", d.animation.loopCount}};
    };
    add("timeline.info", [info](const QJsonObject&) { return info(); });
    add("timeline.frame", [session, document, info](const QJsonObject& p) {
        EditorSession* s = session();
        const Document& d = document();
        const QString what = str(p, "action").toLower();
        const int count = int(d.animation.frames.size());
        auto indexParam = [&](const char* key, int fallback) {
            const int i = integer(p, key, fallback);
            if (i < 0 || i >= count) fail(QStringLiteral("%1 must be a frame index 0..%2").arg(key).arg(count - 1), invalidParams);
            return i;
        };
        bool ok = false;
        if (what == "create") { if (count) fail("the document has frames already"); ok = s->timelineCreate(); }
        else if (what == "fromlayers") ok = s->timelineFramesFromLayers();
        else if (what == "duplicate" || what == "new") {
            if (count && has(p, "index")) s->timelineSelectFrame(indexParam("index", 0));
            ok = s->timelineDuplicateFrame();
        }
        else if (what == "clear") { if (!count) fail("the document has no frames"); ok = s->timelineClear(); }
        else {
            if (!count) fail("the document has no frames; timeline.frame {\"action\": \"create\"} makes the first");
            const int current = d.animation.current;
            if (what == "select") ok = s->timelineSelectFrame(indexParam("index", current));
            else if (what == "delete") ok = s->timelineDeleteFrame(indexParam("index", current));
            else if (what == "move") {
                const int from = indexParam("index", current), to = indexParam("to", current);
                ok = from == to || s->timelineMoveFrame(from, to);
            }
            else fail("action must be create, fromLayers, duplicate, select, delete, move or clear", invalidParams);
        }
        if (!ok) fail("the timeline couldn't do that");
        return info();
    });
    add("timeline.set", [session, document, info](const QJsonObject& p) {
        EditorSession* s = session();
        const Document& d = document();
        if (d.animation.empty()) fail("the document has no frames; timeline.frame {\"action\": \"create\"} makes the first");
        if (has(p, "delay")) {
            const int index = has(p, "index") ? integer(p, "index") : d.animation.current;
            if (index < -1 || index >= int(d.animation.frames.size())) fail("index must be a frame index, or -1 for every frame", invalidParams);
            const int delay = integer(p, "delay");
            if (delay < 0 || delay > maxFrameDelayMs) fail(QStringLiteral("delay must be 0..%1 milliseconds").arg(maxFrameDelayMs), invalidParams);
            s->timelineSetDelay(index, delay);
        }
        if (has(p, "loopCount")) {
            const int loops = integer(p, "loopCount");
            if (loops < 0 || loops > 65535) fail("loopCount must be 0 (forever) to 65535", invalidParams);
            s->timelineSetLoopCount(loops);
        }
        return info();
    });
}

} // namespace app
