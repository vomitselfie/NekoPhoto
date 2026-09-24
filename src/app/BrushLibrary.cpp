#include "BrushLibrary.h"
#include "compositor/mypaint.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <algorithm>

namespace app {

namespace {

const char* collection = ":/brushes/mypaint/";

/// The collection's own group names, credited to their authors where the name is a nickname.
QString groupTitle(const QString& group) {
    static const QHash<QString, QString> titles{
        {"Deevad", QStringLiteral("David Revoy")}, {"Ramon", QStringLiteral("Ramón Miranda")}, {"Dieterle", QStringLiteral("Brien Dieterle")}};
    return titles.value(group, group);
}

/// "8B_Pencil#1" becomes "8B Pencil".
QString displayName(const QString& file) {
    QString name = file;
    int hash = name.indexOf('#');
    if (hash > 0) name.truncate(hash);
    name.replace('_', ' ');
    name.replace('-', ' ');
    name = name.simplified();
    if (!name.isEmpty()) name[0] = name[0].toUpper();
    return name;
}

bool load(BrushPreset& preset, const QString& mybPath) {
    QFile file(mybPath);
    if (!file.open(QIODevice::ReadOnly)) return false;
    preset.json = file.readAll().toStdString();
    compositor::MyPaintPresetInfo info = compositor::myPaintPresetInfo(preset.json);
    if (!info.valid) return false;
    preset.diameter = std::clamp(info.radius * 2, 1.0, 2000.0);
    preset.eraser = info.eraser;
    QString preview = mybPath.left(mybPath.size() - 4) + "_prev.png";
    if (QFile::exists(preview)) preset.previewPath = preview;
    return true;
}

/// Imported tip brushes: importFolder()/<set>/<brush>/brush.json, the set's folders in name order.
void loadImported(std::vector<BrushPreset>& out) {
    const QDir root(BrushLibrary::importFolder());
    for (const QFileInfo& set : root.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name | QDir::IgnoreCase)) {
        const QDir setDir(set.absoluteFilePath());
        for (const QFileInfo& brush : setDir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name | QDir::IgnoreCase)) {
            const QString folder = brush.absoluteFilePath();
            QFile json(folder + "/brush.json");
            if (!json.open(QIODevice::ReadOnly)) continue;
            const QJsonObject o = QJsonDocument::fromJson(json.readAll()).object();
            BrushPreset preset;
            preset.engine = BrushPreset::Engine::Tip;
            preset.id = "imported/" + set.fileName() + "/" + brush.fileName();
            preset.group = set.fileName();
            preset.name = o.value("name").toString(brush.fileName());
            preset.folder = folder;
            preset.diameter = std::clamp(o.value("diameter").toDouble(30), 1.0, 2000.0);
            if (QFile::exists(folder + "/preview.png")) preset.previewPath = folder + "/preview.png";
            out.push_back(std::move(preset));
        }
    }
}

std::vector<BrushPreset> loadAll() {
    std::vector<BrushPreset> out;
    QFile order(QString(collection) + "order.conf");
    if (order.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QString group;
        while (!order.atEnd()) {
            QString line = QString::fromUtf8(order.readLine()).trimmed();
            if (line.isEmpty() || line.startsWith('#')) continue;
            if (line.startsWith("Group:")) { group = groupTitle(line.mid(6).trimmed()); continue; }
            BrushPreset preset;
            preset.id = line;
            preset.group = group;
            preset.name = displayName(QFileInfo(line).fileName());
            if (load(preset, QString(collection) + line + ".myb")) out.push_back(std::move(preset));
        }
    }
    const QDir user(BrushLibrary::userFolder());
    for (const QFileInfo& file : user.entryInfoList({"*.myb"}, QDir::Files, QDir::Name)) {
        BrushPreset preset;
        preset.id = "user/" + file.completeBaseName();
        preset.group = QObject::tr("My Brushes");
        preset.name = displayName(file.completeBaseName());
        if (load(preset, file.absoluteFilePath())) out.push_back(std::move(preset));
    }
    loadImported(out);
    return out;
}

std::vector<BrushPreset>& storage() {
    static std::vector<BrushPreset> all = loadAll();
    return all;
}

} // namespace

QIcon BrushPreset::icon() const { return previewPath.isEmpty() ? QIcon() : QIcon(previewPath); }

const std::vector<BrushPreset>& BrushLibrary::presets() { return storage(); }

void BrushLibrary::reload() { storage() = loadAll(); }

QString BrushLibrary::importFolder() { return userFolder() + "/imported"; }

std::shared_ptr<const compositor::TipPreset> BrushPreset::tip() const {
    if (engine != Engine::Tip) return nullptr;
    if (!tip_) {
        std::string error;
        if (auto loaded = compositor::loadTipPreset(folder.toStdString(), &error)) tip_ = std::make_shared<const compositor::TipPreset>(std::move(*loaded));
    }
    return tip_;
}

const BrushPreset* BrushLibrary::find(const QString& id) {
    for (const BrushPreset& preset : presets()) if (preset.id == id) return &preset;
    return nullptr;
}

QStringList BrushLibrary::groups() {
    QStringList out;
    for (const BrushPreset& preset : presets()) if (!out.contains(preset.group)) out.append(preset.group);
    return out;
}

QString BrushLibrary::userFolder() {
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/brushes";
}

void warmBrushEngines() {
    for (const BrushPreset& preset : BrushLibrary::presets())
        if (preset.engine != BrushPreset::Engine::Tip && !preset.json.empty()) { compositor::warmMyPaint(preset.json); return; }
}

} // namespace app
