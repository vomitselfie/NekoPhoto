#include "ActionLibrary.h"
#include <QCoreApplication>
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
    static const QHash<QString, const char*> names = {
        {"pixels.filter", QT_TRANSLATE_NOOP("app::ActionLibrary", "Filter")}, {"pixels.adjust", QT_TRANSLATE_NOOP("app::ActionLibrary", "Adjust")}, {"pixels.invert", QT_TRANSLATE_NOOP("app::ActionLibrary", "Invert")}, {"pixels.fill", QT_TRANSLATE_NOOP("app::ActionLibrary", "Fill")}, {"pixels.clear", QT_TRANSLATE_NOOP("app::ActionLibrary", "Clear")},
        {"pixels.contentAwareFill", QT_TRANSLATE_NOOP("app::ActionLibrary", "Content-Aware Fill")}, {"pixels.gmic", QT_TRANSLATE_NOOP("app::ActionLibrary", "G'MIC")}, {"pixels.cameraRaw", QT_TRANSLATE_NOOP("app::ActionLibrary", "Camera Raw Filter")}, {"pixels.mosh", QT_TRANSLATE_NOOP("app::ActionLibrary", "Mosh")},
        {"canvas.resize", QT_TRANSLATE_NOOP("app::ActionLibrary", "Canvas Size")}, {"image.resize", QT_TRANSLATE_NOOP("app::ActionLibrary", "Image Size")}, {"image.trim", QT_TRANSLATE_NOOP("app::ActionLibrary", "Trim")}, {"canvas.crop", QT_TRANSLATE_NOOP("app::ActionLibrary", "Crop")}, {"canvas.flip", QT_TRANSLATE_NOOP("app::ActionLibrary", "Flip Canvas")},
        {"layers.add", QT_TRANSLATE_NOOP("app::ActionLibrary", "New Layer")}, {"layers.duplicate", QT_TRANSLATE_NOOP("app::ActionLibrary", "Duplicate Layer")}, {"layers.delete", QT_TRANSLATE_NOOP("app::ActionLibrary", "Delete Layer")}, {"layers.merge", QT_TRANSLATE_NOOP("app::ActionLibrary", "Merge")},
        {"layers.group", QT_TRANSLATE_NOOP("app::ActionLibrary", "Group Layers")}, {"layers.flip", QT_TRANSLATE_NOOP("app::ActionLibrary", "Flip Layer")}, {"layers.mask", QT_TRANSLATE_NOOP("app::ActionLibrary", "Layer Mask")}, {"layers.set", QT_TRANSLATE_NOOP("app::ActionLibrary", "Set Layer")},
        {"layers.reorder", QT_TRANSLATE_NOOP("app::ActionLibrary", "Arrange")}, {"layers.viaCopy", QT_TRANSLATE_NOOP("app::ActionLibrary", "Layer via Copy")},
        {"smartObject.convert", QT_TRANSLATE_NOOP("app::ActionLibrary", "Convert to Smart Object")}, {"smartObject.rasterize", QT_TRANSLATE_NOOP("app::ActionLibrary", "Rasterize Smart Object")},
        {"smartObject.viaCopy", QT_TRANSLATE_NOOP("app::ActionLibrary", "New Smart Object via Copy")},
        {"smartObject.replace", QT_TRANSLATE_NOOP("app::ActionLibrary", "Replace Contents")}, {"smartObject.place", QT_TRANSLATE_NOOP("app::ActionLibrary", "Place Embedded")}, {"image.mode", QT_TRANSLATE_NOOP("app::ActionLibrary", "Mode")}, {"document.profile", QT_TRANSLATE_NOOP("app::ActionLibrary", "Profile")},
        {"paths.fromSelection", QT_TRANSLATE_NOOP("app::ActionLibrary", "Make Work Path")}, {"paths.fill", QT_TRANSLATE_NOOP("app::ActionLibrary", "Fill Path")}, {"paths.stroke", QT_TRANSLATE_NOOP("app::ActionLibrary", "Stroke Path")},
        {"paths.toSelection", QT_TRANSLATE_NOOP("app::ActionLibrary", "Load Path as Selection")}, {"paths.toShape", QT_TRANSLATE_NOOP("app::ActionLibrary", "Shape from Path")}, {"paths.delete", QT_TRANSLATE_NOOP("app::ActionLibrary", "Delete Path")},
        {"channels.new", QT_TRANSLATE_NOOP("app::ActionLibrary", "New Channel")}, {"channels.saveSelection", QT_TRANSLATE_NOOP("app::ActionLibrary", "Save Selection")},
        {"selection.all", QT_TRANSLATE_NOOP("app::ActionLibrary", "Select All")}, {"selection.none", QT_TRANSLATE_NOOP("app::ActionLibrary", "Deselect")}, {"selection.invert", QT_TRANSLATE_NOOP("app::ActionLibrary", "Select Inverse")}, {"selection.rect", QT_TRANSLATE_NOOP("app::ActionLibrary", "Marquee")},
        {"selection.grow", QT_TRANSLATE_NOOP("app::ActionLibrary", "Expand/Contract Selection")}, {"selection.feather", QT_TRANSLATE_NOOP("app::ActionLibrary", "Feather")}, {"selection.smooth", QT_TRANSLATE_NOOP("app::ActionLibrary", "Smooth Selection")},
        {"selection.border", QT_TRANSLATE_NOOP("app::ActionLibrary", "Border Selection")}, {"selection.fromLayer", QT_TRANSLATE_NOOP("app::ActionLibrary", "Load Selection")}, {"brush.stroke", QT_TRANSLATE_NOOP("app::ActionLibrary", "Brush Stroke")},
        {"colors.set", QT_TRANSLATE_NOOP("app::ActionLibrary", "Set Colours")}, {"document.export", QT_TRANSLATE_NOOP("app::ActionLibrary", "Export")}, {"document.save", QT_TRANSLATE_NOOP("app::ActionLibrary", "Save")}, {"timeline.frame", QT_TRANSLATE_NOOP("app::ActionLibrary", "Frame")}, {"timeline.set", QT_TRANSLATE_NOOP("app::ActionLibrary", "Timeline")},
    };
    const char* name = names.value(method, nullptr);
    return name ? QCoreApplication::translate("app::ActionLibrary", name) : method;
}

QString valueText(const QJsonValue& v) {
    if (v.isDouble()) return QString::number(v.toDouble(), 'g', 6);
    if (v.isBool()) return v.toBool() ? QCoreApplication::translate("app::ActionLibrary", "on") : QCoreApplication::translate("app::ActionLibrary", "off");
    if (v.isString()) return v.toString();
    if (v.isArray()) return QCoreApplication::translate("app::ActionLibrary", "[%n item(s)]", nullptr, int(v.toArray().size()));
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
    const QString trimmed = base.trimmed().isEmpty() ? tr("Action") : base.trimmed();
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
    if (action.name.isEmpty()) return fail(tr("an action needs a name"));
    if (!json.value("steps").isArray()) return fail(tr("%1: steps must be an array").arg(action.name));
    for (const QJsonValue& v : json.value("steps").toArray()) {
        const QJsonObject o = v.toObject();
        ActionStep step;
        step.method = o.value("method").toString();
        if (step.method.isEmpty()) return fail(tr("%1: every step needs a method").arg(action.name));
        if (o.contains("params") && !o.value("params").isObject()) return fail(tr("%1: a step's params must be an object").arg(action.name));
        step.params = o.value("params").toObject();
        step.enabled = o.value("enabled").toBool(true);
        action.steps.push_back(std::move(step));
    }
    return action;
}

QString ActionLibrary::describe(const ActionStep& step) {
    QStringList parts;
    if (step.method == "brush.stroke") parts << tr("%n point(s)", nullptr, int(step.params.value("points").toArray().size()));
    for (auto it = step.params.begin(); it != step.params.end(); ++it) {
        if (step.method == "brush.stroke" && (it.key() == "points" || it.key() == "pressures" || it.key() == "tilts" || it.key() == "twists" || it.key() == "times")) continue;
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
    if (read.empty()) { if (error) *error = tr("the file holds no actions"); return {}; }
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
        if (!a) { if (error) *error = tr("there is no action named %1").arg(name); return false; }
        array.append(toJson(*a));
    }
    if (!writeAll(path, QJsonDocument(QJsonObject{{"format", formatName}, {"version", formatVersion}, {"actions", array}}).toJson())) {
        if (error) *error = tr("couldn't write %1").arg(path);
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
                                        "style", "cage", "import", "remove", "screenshot", "histogram"};
    if (method == "rpc.batch") return true;
    const QString area = method.section('.', 0, 0), verb = method.section('.', 1);
    if (method == "screenshot" || method == "render" || prefixes.contains(area)) return false;
    // presets.import / presets.remove / brush.import change libraries, not the document; document.import is an edit.
    if (method == "document.import") return true;
    return !looks.contains(verb);
}

} // namespace app
