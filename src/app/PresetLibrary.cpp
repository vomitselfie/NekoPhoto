#include "PresetLibrary.h"
#include "EditorSession.h"
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QSettings>
#include <QStandardPaths>

using namespace compositor;

namespace app {

namespace {

std::vector<uint8_t> readAll(const QString& path, QString* error) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) { if (error) *error = f.errorString(); return {}; }
    const QByteArray bytes = f.readAll();
    return std::vector<uint8_t>(bytes.begin(), bytes.end());
}

/// Written beside the file and renamed over it, so a crash leaves the old file. (Not QSaveFile: its O_TMPFILE and
/// linkat path fails in some sandboxes.)
bool writeAll(const QString& path, const QByteArray& bytes) {
    const QString temporary = path + ".part";
    QFile f(temporary);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate) || f.write(bytes) != bytes.size()) { f.remove(); return false; }
    f.close();
    QFile::remove(path);
    return QFile::rename(temporary, path);
}

QByteArray bytesOf(const std::vector<uint8_t>& v) { return QByteArray(reinterpret_cast<const char*>(v.data()), qsizetype(v.size())); }

void noteAll(QStringList& out, const QString& file, const std::vector<std::string>& notes) {
    for (auto& n : notes) out << QStringLiteral("%1: %2").arg(file, QString::fromStdString(n));
}

} // namespace

PresetLibrary& PresetLibrary::instance() {
    static PresetLibrary library;
    return library;
}

QString PresetLibrary::folder() {
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/presets";
}

void PresetLibrary::load() {
    if (loaded_) return;
    loaded_ = true;
    const QString dir = folder();
    if (QFile::exists(dir + "/patterns.pat"))
        if (auto p = readPat(readAll(dir + "/patterns.pat", nullptr), nullptr)) patterns_ = std::move(*p);
    if (QFile::exists(dir + "/gradients.grd"))
        if (auto g = readGrd(readAll(dir + "/gradients.grd", nullptr), nullptr)) gradients_ = std::move(*g);
    QFile f(dir + "/styles.json");
    if (f.open(QIODevice::ReadOnly)) {
        for (const QJsonValue v : QJsonDocument::fromJson(f.readAll()).array()) {
            const QJsonObject o = v.toObject();
            StylePreset s;
            s.name = o.value("name").toString().toStdString();
            s.id = o.value("id").toString().toStdString();
            const QByteArray json = QJsonDocument(o.value("style").toObject()).toJson(QJsonDocument::Compact);
            if (s.name.empty() || !layerStyleFromJson(json.toStdString(), s.style, nullptr)) continue;
            styles_.push_back(std::move(s));
        }
    }
}

void PresetLibrary::save() {
    const QString dir = folder();
    QDir().mkpath(dir);
    QJsonArray array;
    for (auto& s : styles_)
        array.append(QJsonObject{{"name", QString::fromStdString(s.name)}, {"id", QString::fromStdString(s.id)},
                                 {"style", QJsonDocument::fromJson(QByteArray::fromStdString(layerStyleToJson(s.style))).object()}});
    writeAll(dir + "/styles.json", QJsonDocument(array).toJson());
    if (patterns_.empty()) QFile::remove(dir + "/patterns.pat");
    else { writeAll(dir + "/patterns.pat", bytesOf(writePat(patterns_))); }
    if (gradients_.empty()) QFile::remove(dir + "/gradients.grd");
    else { writeAll(dir + "/gradients.grd", bytesOf(writeGrd(gradients_))); }
    emit changed();
}

const std::vector<StylePreset>& PresetLibrary::styles() { load(); return styles_; }
const std::vector<PatternPreset>& PresetLibrary::patterns() { load(); return patterns_; }
const std::vector<GradientPreset>& PresetLibrary::gradients() { load(); return gradients_; }

const StylePreset* PresetLibrary::findStyle(const QString& name) {
    load();
    for (auto& s : styles_) if (QString::fromStdString(s.name) == name) return &s;
    for (auto& s : styles_) if (QString::fromStdString(s.name).compare(name, Qt::CaseInsensitive) == 0) return &s;
    return nullptr;
}

const GradientPreset* PresetLibrary::findGradient(const QString& name) {
    load();
    for (auto& g : gradients_) if (QString::fromStdString(g.name) == name) return &g;
    for (auto& g : gradients_) if (QString::fromStdString(g.name).compare(name, Qt::CaseInsensitive) == 0) return &g;
    return nullptr;
}

std::vector<PatternPreset> PresetLibrary::patternsFor(const LayerStyle& style) {
    load();
    std::vector<PatternPreset> out;
    for (auto& id : stylePatternIds(style))
        for (auto& p : patterns_) if (p.id == id) { out.push_back(p); break; }
    return out;
}

PresetLibrary::ImportResult PresetLibrary::importFiles(const QStringList& paths) {
    load();
    ImportResult result;
    auto addPatterns = [&](const std::vector<PatternPreset>& patterns) {
        for (auto& p : patterns)
            if (std::none_of(patterns_.begin(), patterns_.end(), [&](const PatternPreset& q) { return q.id == p.id; })) patterns_.push_back(p);
    };
    for (const QString& path : paths) {
        const QString file = QFileInfo(path).fileName(), suffix = QFileInfo(path).suffix().toLower();
        QString readError;
        const std::vector<uint8_t> bytes = readAll(path, &readError);
        if (bytes.empty()) { result.errors << QStringLiteral("%1: %2").arg(file, readError.isEmpty() ? tr("the file is empty") : readError); continue; }
        std::string error;
        std::vector<std::string> notes;
        // By content first ('8BSL' sits after a version word), then by the name's suffix for the message.
        const bool asl = bytes.size() >= 6 && std::string(bytes.begin() + 2, bytes.begin() + 6) == "8BSL";
        const std::string magic = bytes.size() >= 4 ? std::string(bytes.begin(), bytes.begin() + 4) : std::string();
        if (asl || (suffix == "asl" && magic != "8BPT" && magic != "8BGR")) {
            auto library = readAsl(bytes, &error, &notes);
            if (!library) { result.errors << QStringLiteral("%1: %2").arg(file, QString::fromStdString(error)); continue; }
            addPatterns(library->patterns);
            for (auto& s : library->styles) {
                auto same = std::find_if(styles_.begin(), styles_.end(), [&](const StylePreset& t) { return t.name == s.name; });
                if (same != styles_.end()) *same = s; else styles_.push_back(s);
                result.styles << QString::fromStdString(s.name);
            }
        } else if (magic == "8BPT" || suffix == "pat") {
            auto patterns = readPat(bytes, &error, &notes);
            if (!patterns) { result.errors << QStringLiteral("%1: %2").arg(file, QString::fromStdString(error)); continue; }
            addPatterns(*patterns);
            for (auto& p : *patterns) {
                result.patterns << QString::fromStdString(p.name.empty() ? p.id : p.name);
                result.importedPatterns.push_back(p);
            }
        } else if (magic == "8BGR" || suffix == "grd") {
            auto gradients = readGrd(bytes, &error, &notes);
            if (!gradients) { result.errors << QStringLiteral("%1: %2").arg(file, QString::fromStdString(error)); continue; }
            for (auto& g : *gradients) {
                auto same = std::find_if(gradients_.begin(), gradients_.end(), [&](const GradientPreset& h) { return h.name == g.name; });
                if (same != gradients_.end()) *same = g; else gradients_.push_back(g);
                result.gradients << QString::fromStdString(g.name);
            }
        } else {
            result.errors << tr("%1: not a Photoshop styles (.asl), patterns (.pat) or gradients (.grd) file").arg(file);
            continue;
        }
        noteAll(result.notes, file, notes);
    }
    if (!result.styles.isEmpty() || !result.patterns.isEmpty() || !result.gradients.isEmpty()) save();
    return result;
}

bool PresetLibrary::removeStyle(const QString& name) {
    const StylePreset* s = findStyle(name);
    if (!s) return false;
    styles_.erase(styles_.begin() + (s - styles_.data()));
    save();
    return true;
}

bool PresetLibrary::removeGradient(const QString& name) {
    const GradientPreset* g = findGradient(name);
    if (!g) return false;
    gradients_.erase(gradients_.begin() + (g - gradients_.data()));
    save();
    return true;
}

bool PresetLibrary::removePattern(const QString& idOrName) {
    load();
    auto it = std::find_if(patterns_.begin(), patterns_.end(), [&](const PatternPreset& p) { return QString::fromStdString(p.id) == idOrName; });
    if (it == patterns_.end())
        it = std::find_if(patterns_.begin(), patterns_.end(), [&](const PatternPreset& p) { return QString::fromStdString(p.name) == idOrName; });
    if (it == patterns_.end()) return false;
    patterns_.erase(it);
    save();
    return true;
}

QString presetFileFilter() {
    return QObject::tr("Photoshop presets (*.asl *.pat *.grd);;Styles (*.asl);;Patterns (*.pat);;Gradients (*.grd)");
}

void importPresetsInteractively(QWidget* parent, EditorSession* session) {
    QSettings settings;
    const QStringList paths = QFileDialog::getOpenFileNames(parent, QObject::tr("Import Presets"), settings.value("presets/importDir", QDir::homePath()).toString(), presetFileFilter());
    if (paths.isEmpty()) return;
    settings.setValue("presets/importDir", QFileInfo(paths.first()).absolutePath());
    const PresetLibrary::ImportResult result = PresetLibrary::instance().importFiles(paths);
    int added = 0;
    if (session && session->document() && !result.importedPatterns.empty()) added = session->addPatterns(result.importedPatterns);
    QStringList lines;
    if (!result.styles.isEmpty())
        lines << QObject::tr("%n style(s), in Layer ▸ Layer Style ▸ Apply Style.", nullptr, int(result.styles.size()));
    if (!result.gradients.isEmpty())
        lines << QObject::tr("%n gradient(s), in the Gradient tool's options.", nullptr, int(result.gradients.size()));
    if (!result.patterns.isEmpty())
        lines << (added ? QObject::tr("%n pattern(s), added to this document for pattern overlays and textures.", nullptr, int(result.patterns.size()))
                        : QObject::tr("%n pattern(s), kept for the styles that use them.", nullptr, int(result.patterns.size())));
    QString text = lines.isEmpty() ? QString() : QObject::tr("Imported:") + "\n" + lines.join('\n');
    if (!result.notes.isEmpty()) text += "\n\n" + QObject::tr("Notes:") + "\n" + result.notes.join('\n');
    if (!result.errors.isEmpty()) text += (text.isEmpty() ? QString() : QStringLiteral("\n\n")) + QObject::tr("Not imported:") + "\n" + result.errors.join('\n');
    if (lines.isEmpty()) QMessageBox::warning(parent, QObject::tr("Import Presets"), text);
    else QMessageBox::information(parent, QObject::tr("Import Presets"), text);
}

} // namespace app
