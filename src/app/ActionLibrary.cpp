#include "ActionLibrary.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSet>
#include <QStandardPaths>

namespace app {

namespace {

constexpr int formatVersion = 1;
const char* formatName = "nekophoto.actions";

/// Written beside the file and renamed over it, so a crash leaves the old file.
bool writeAll(const QString& path, const QByteArray& bytes) {
    const QString temporary = path + ".part";
    QFile f(temporary);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate) || f.write(bytes) != bytes.size()) { f.remove(); return false; }
    f.close();
    QFile::remove(path);
    return QFile::rename(temporary, path);
}

/// Friendlier names for the steps the menus record.
QString title(const QString& method) {
    static const QHash<QString, QString> names = {
        {"pixels.filter", "Filter"}, {"pixels.adjust", "Adjust"}, {"pixels.invert", "Invert"}, {"pixels.fill", "Fill"}, {"pixels.clear", "Clear"},
        {"pixels.contentAwareFill", "Content-Aware Fill"}, {"pixels.gmic", "G'MIC"}, {"pixels.cameraRaw", "Camera Raw Filter"},
        {"canvas.resize", "Canvas Size"}, {"image.resize", "Image Size"}, {"image.trim", "Trim"}, {"canvas.crop", "Crop"}, {"canvas.flip", "Flip Canvas"},
        {"layers.add", "New Layer"}, {"layers.duplicate", "Duplicate Layer"}, {"layers.delete", "Delete Layer"}, {"layers.merge", "Merge"},
        {"layers.group", "Group Layers"}, {"layers.flip", "Flip Layer"}, {"layers.mask", "Layer Mask"}, {"layers.set", "Set Layer"},
        {"selection.all", "Select All"}, {"selection.none", "Deselect"}, {"selection.invert", "Select Inverse"}, {"selection.rect", "Marquee"},
        {"selection.grow", "Expand/Contract Selection"}, {"selection.feather", "Feather"}, {"selection.smooth", "Smooth Selection"},
        {"selection.border", "Border Selection"}, {"selection.fromLayer", "Load Selection"}, {"brush.stroke", "Brush Stroke"},
        {"colors.set", "Set Colours"}, {"document.export", "Export"}, {"document.save", "Save"}, {"timeline.frame", "Frame"}, {"timeline.set", "Timeline"},
    };
    return names.value(method, method);
}

QString valueText(const QJsonValue& v) {
    if (v.isDouble()) return QString::number(v.toDouble(), 'g', 6);
    if (v.isBool()) return v.toBool() ? QStringLiteral("on") : QStringLiteral("off");
    if (v.isString()) return v.toString();
    if (v.isArray()) return QStringLiteral("[%1 items]").arg(v.toArray().size());
    return QString::fromUtf8(QJsonDocument(v.toObject()).toJson(QJsonDocument::Compact));
}

} // namespace

ActionLibrary& ActionLibrary::instance() {
    static ActionLibrary library;
    return library;
}

QString ActionLibrary::filePath() {
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/actions.json";
}

void ActionLibrary::load() {
    if (loaded_) return;
    loaded_ = true;
    QFile f(filePath());
    if (!f.open(QIODevice::ReadOnly)) return;
    const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
    for (const QJsonValue& v : root.value("actions").toArray())
        if (auto action = fromJson(v.toObject(), nullptr)) actions_.push_back(std::move(*action));
}

void ActionLibrary::save() {
    QJsonArray array;
    for (const RecordedAction& a : actions_) array.append(toJson(a));
    QDir().mkpath(QFileInfo(filePath()).path());
    writeAll(filePath(), QJsonDocument(QJsonObject{{"format", formatName}, {"version", formatVersion}, {"actions", array}}).toJson());
    emit changed();
}

const std::vector<RecordedAction>& ActionLibrary::actions() { load(); return actions_; }

const RecordedAction* ActionLibrary::find(const QString& name) {
    load();
    for (const RecordedAction& a : actions_) if (a.name == name) return &a;
    return nullptr;
}

void ActionLibrary::put(const RecordedAction& action) {
    load();
    for (RecordedAction& a : actions_) if (a.name == action.name) { a = action; save(); return; }
    actions_.push_back(action);
    save();
}

bool ActionLibrary::remove(const QString& name) {
    load();
    for (size_t i = 0; i < actions_.size(); i++) {
        if (actions_[i].name != name) continue;
        if (recordingName_ == name) stopRecording();
        actions_.erase(actions_.begin() + std::ptrdiff_t(i));
        save();
        return true;
    }
    return false;
}

bool ActionLibrary::rename(const QString& from, const QString& to) {
    load();
    const QString name = to.trimmed();
    if (name.isEmpty() || (name != from && find(name))) return false;
    for (RecordedAction& a : actions_) {
        if (a.name != from) continue;
        a.name = name;
        if (recordingName_ == from) recordingName_ = name;
        save();
        return true;
    }
    return false;
}

QString ActionLibrary::uniqueName(const QString& base) {
    const QString trimmed = base.trimmed().isEmpty() ? QStringLiteral("Action") : base.trimmed();
    if (!find(trimmed)) return trimmed;
    for (int n = 2;; n++) if (!find(QStringLiteral("%1 %2").arg(trimmed).arg(n))) return QStringLiteral("%1 %2").arg(trimmed).arg(n);
}

QJsonObject ActionLibrary::toJson(const RecordedAction& action) {
    QJsonArray steps;
    for (const ActionStep& s : action.steps) {
        QJsonObject o{{"method", s.method}, {"params", s.params}};
        if (!s.enabled) o["enabled"] = false;
        steps.append(o);
    }
    return {{"name", action.name}, {"steps", steps}};
}

std::optional<RecordedAction> ActionLibrary::fromJson(const QJsonObject& json, QString* error) {
    auto fail = [error](const QString& why) { if (error) *error = why; return std::nullopt; };
    RecordedAction action;
    action.name = json.value("name").toString().trimmed();
    if (action.name.isEmpty()) return fail(QStringLiteral("an action needs a name"));
    if (!json.value("steps").isArray()) return fail(QStringLiteral("%1: steps must be an array").arg(action.name));
    for (const QJsonValue& v : json.value("steps").toArray()) {
        const QJsonObject o = v.toObject();
        ActionStep step;
        step.method = o.value("method").toString();
        if (step.method.isEmpty()) return fail(QStringLiteral("%1: every step needs a method").arg(action.name));
        if (o.contains("params") && !o.value("params").isObject()) return fail(QStringLiteral("%1: a step's params must be an object").arg(action.name));
        step.params = o.value("params").toObject();
        step.enabled = o.value("enabled").toBool(true);
        action.steps.push_back(std::move(step));
    }
    return action;
}

QString ActionLibrary::describe(const ActionStep& step) {
    QStringList parts;
    if (step.method == "brush.stroke") parts << QStringLiteral("%1 points").arg(step.params.value("points").toArray().size());
    for (auto it = step.params.begin(); it != step.params.end(); ++it) {
        if (step.method == "brush.stroke" && (it.key() == "points" || it.key() == "pressures")) continue;
        parts << it.key() + " " + valueText(it.value());
    }
    QString text = title(step.method);
    if (!parts.isEmpty()) text += "  " + parts.join(", ");
    return text.size() > 160 ? text.left(157) + "..." : text;
}

QStringList ActionLibrary::importFile(const QString& path, QString* error) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) { if (error) *error = f.errorString(); return {}; }
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &parseError);
    if (!doc.isObject()) { if (error) *error = parseError.errorString(); return {}; }
    QJsonArray list = doc.object().contains("actions") ? doc.object().value("actions").toArray() : QJsonArray{doc.object()};
    std::vector<RecordedAction> read;
    for (const QJsonValue& v : list) {
        auto action = fromJson(v.toObject(), error);
        if (!action) return {};
        read.push_back(std::move(*action));
    }
    if (read.empty()) { if (error) *error = QStringLiteral("the file holds no actions"); return {}; }
    load();
    QStringList names;
    for (RecordedAction& a : read) {
        a.name = uniqueName(a.name);
        actions_.push_back(a);
        names << a.name;
    }
    save();
    return names;
}

bool ActionLibrary::exportFile(const QStringList& names, const QString& path, QString* error) {
    QJsonArray array;
    for (const QString& name : names) {
        const RecordedAction* a = find(name);
        if (!a) { if (error) *error = QStringLiteral("there is no action named %1").arg(name); return false; }
        array.append(toJson(*a));
    }
    if (!writeAll(path, QJsonDocument(QJsonObject{{"format", formatName}, {"version", formatVersion}, {"actions", array}}).toJson())) {
        if (error) *error = QStringLiteral("couldn't write %1").arg(path);
        return false;
    }
    return true;
}

void ActionLibrary::startRecording(const QString& name) {
    load();
    const QString trimmed = name.trimmed();
    if (trimmed.isEmpty()) return;
    if (!find(trimmed)) { actions_.push_back({trimmed, {}}); save(); }
    recordingName_ = trimmed;
    recordedSteps_ = 0;
    emit recordingChanged(true);
}

int ActionLibrary::stopRecording() {
    if (recordingName_.isEmpty()) return 0;
    recordingName_.clear();
    const int steps = recordedSteps_;
    recordedSteps_ = 0;
    emit recordingChanged(false);
    return steps;
}

void ActionLibrary::record(const QString& method, const QJsonObject& params) {
    if (recordingName_.isEmpty() || quiet_ > 0 || !recordable(method)) return;
    load();
    for (RecordedAction& a : actions_) {
        if (a.name != recordingName_) continue;
        a.steps.push_back({method, params, true});
        recordedSteps_++;
        save();
        return;
    }
}

bool ActionLibrary::recordable(const QString& method) {
    static const QSet<QString> prefixes = {"rpc", "app", "events", "tabs", "history", "debug", "actions", "view", "tool"};
    static const QSet<QString> looks = {"info", "list", "get", "render", "overview", "describe", "methods", "filters", "presets", "defaults",
                                        "style", "cage", "import", "remove", "screenshot"};
    if (method == "rpc.batch") return true;
    const QString area = method.section('.', 0, 0), verb = method.section('.', 1);
    if (method == "screenshot" || method == "render" || prefixes.contains(area)) return false;
    // presets.import / presets.remove / brush.import change libraries, not the document; document.import is an edit.
    if (method == "document.import") return true;
    return !looks.contains(verb);
}

} // namespace app
