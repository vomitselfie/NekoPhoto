#include "Gmic.h"
#include "compositor/png.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QDateTime>
#include <mutex>
#ifdef COMPOSITOR_HAVE_LIBGMIC
#include <gmic.h>
#endif
#include <QRegularExpression>
#include <QSet>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTextStream>
#include <QTimer>
#include <QProcessEnvironment>
#include <algorithm>

namespace app {

// ---- parameters and filters -----------------------------------------------------------------------

QString GmicParam::argument() const {
    switch (kind) {
    case Float: return QString::number(value, 'g', 6);
    case Int: return QString::number(int(value));
    case Bool: return value != 0 ? "1" : "0";
    case Choice: return QString::number(int(value));
    case Color: return hasAlpha ? QStringLiteral("%1,%2,%3,%4").arg(int(r)).arg(int(g)).arg(int(b)).arg(int(a)) : QStringLiteral("%1,%2,%3").arg(int(r)).arg(int(g)).arg(int(b));
    case Text: { QString t = text; t.replace('"', "'"); return '"' + t + '"'; }
    case Point: case Value: return text;
    default: return {};
    }
}

QString GmicFilter::commandLine(bool preview) const {
    QStringList args;
    for (const GmicParam& p : params) if (p.contributes()) args << p.argument();
    QString base = preview && !previewCommand.isEmpty() ? previewCommand : command;
    return args.isEmpty() ? base : base + " " + args.join(',');
}

// ---- catalogue: the #@gui lines --------------------------------------------------------------------

namespace {

/// Splits `a,b,"c,d"` at top-level commas.
QStringList splitArguments(const QString& s) {
    QStringList out;
    QString current;
    bool quoted = false;
    int depth = 0;
    for (QChar c : s) {
        if (c == '"') quoted = !quoted;
        else if (!quoted && (c == '(' || c == '{' || c == '[')) depth++;
        else if (!quoted && (c == ')' || c == '}' || c == ']')) depth--;
        if (c == ',' && !quoted && depth == 0) { out << current.trimmed(); current.clear(); continue; }
        current += c;
    }
    if (!current.trimmed().isEmpty() || !out.isEmpty()) out << current.trimmed();
    return out;
}

QString unquote(QString s) {
    s = s.trimmed();
    if (s.size() >= 2 && s.startsWith('"') && s.endsWith('"')) s = s.mid(1, s.size() - 2);
    return s;
}

/// A single `Label=type(args)` declaration into a parameter; false for the kinds we can't drive.
bool parseParam(const QString& decl, GmicParam& p) {
    int eq = decl.indexOf('=');
    if (eq < 0) return false;
    p.label = decl.left(eq).trimmed();
    QString rest = decl.mid(eq + 1).trimmed();
    // `type(args)`, perhaps marked _ or ~, with [] or {} as well as () around the arguments, and G'MIC-Qt's optional suffix after
    // them (_0, _1, _2, with a +: how the preview reacts to the control), which says nothing about the value.
    static const QRegularExpression typeRe(R"(^[_~]*([a-z]+)\s*(?:[(\[{](.*)[)\]}])?\s*(?:_\d+\+?)?\s*$)", QRegularExpression::DotMatchesEverythingOption);
    auto m = typeRe.match(rest);
    if (!m.hasMatch()) return false;
    QString type = m.captured(1).toLower(), inner = m.captured(2);
    QStringList args = splitArguments(inner);
    auto num = [&](int i, double fallback) { bool ok = false; double v = i < args.size() ? args[i].toDouble(&ok) : 0; return ok ? v : fallback; };
    if (type == "float" || type == "int") {
        p.kind = type == "float" ? GmicParam::Float : GmicParam::Int;
        p.value = num(0, 0); p.min = num(1, 0); p.max = num(2, std::max(1.0, p.value * 2));
        if (p.max < p.min) std::swap(p.min, p.max);
        p.decimals = type == "int" ? 0 : (p.max - p.min <= 2 ? 3 : 2);
        return true;
    }
    if (type == "bool") { p.kind = GmicParam::Bool; QString v = args.value(0).toLower(); p.value = (v == "1" || v == "true") ? 1 : 0; return true; }
    if (type == "choice") {
        p.kind = GmicParam::Choice;
        int start = 0;
        bool ok = false;
        int def = args.value(0).toInt(&ok);
        if (ok && !args.value(0).trimmed().startsWith('"')) { p.value = def; start = 1; }
        for (int i = start; i < args.size(); i++) p.choices << unquote(args[i]);
        if (p.choices.isEmpty()) return false;
        p.value = std::clamp(p.value, 0.0, double(p.choices.size() - 1));
        return true;
    }
    if (type == "color") {
        p.kind = GmicParam::Color;
        const QString first = args.value(0).trimmed();
        if (first.startsWith('#')) {
            // #rrggbb, or #rrggbbaa with alpha (which G'MIC then passes as a fourth value).
            const QString hex = first.mid(1);
            bool ok = (hex.size() == 6 || hex.size() == 8);
            auto byte = [&](int i) { bool good = false; const int v = hex.mid(i * 2, 2).toInt(&good, 16); ok = ok && good; return v; };
            p.r = byte(0); p.g = byte(1); p.b = byte(2);
            p.hasAlpha = hex.size() == 8;
            p.a = p.hasAlpha ? byte(3) : 255;
            return ok;
        }
        p.r = num(0, 0); p.g = num(1, 0); p.b = num(2, 0);
        p.hasAlpha = args.size() >= 4;
        p.a = p.hasAlpha ? num(3, 255) : 255;
        return true;
    }
    if (type == "text") {
        p.kind = GmicParam::Text;
        // text(_multiline, "default") or text("default")
        QString last = args.isEmpty() ? QString() : args.last();
        p.text = args.size() >= 2 && (args[0] == "0" || args[0] == "1") ? unquote(last) : unquote(args.value(0));
        return true;
    }
    if (type == "note") { p.kind = GmicParam::Note; p.text = unquote(inner); p.text.remove(QRegularExpression("<[^>]*>")); p.text.replace("\\n", "\n"); p.text = p.text.trimmed(); return true; }
    if (type == "separator") { p.kind = GmicParam::Separator; return true; }
    if (type == "point") { p.kind = GmicParam::Point; p.text = QStringLiteral("%1,%2").arg(num(0, 50)).arg(num(1, 50)); return true; }
    if (type == "value") { p.kind = GmicParam::Value; p.text = inner.trimmed(); return true; }   // all of it: value(50,50) passes both
    if (type == "link" || type == "url") { p.kind = GmicParam::Note; p.text = unquote(args.value(0)); return true; }
    // A button passes 0, or 1 on the run its press starts; a filter run from here passes 0.
    if (type == "button") { p.kind = GmicParam::Value; p.text = "0"; return true; }
    return false;   // file, folder: they need a path to be chosen
}

} // namespace

namespace {

/// The definitions as text. gmic.eu serves them compressed, as a one-image G'MIC file: the header lines
/// "1 uint8 little_endian" and "1 <bytes> 1 1 #<compressed bytes>", then a zlib stream of the text.
QByteArray catalogueText(const QByteArray& raw) {
    if (!raw.startsWith("1 uint8")) return raw;
    const qsizetype first = raw.indexOf('\n'), second = first < 0 ? -1 : raw.indexOf('\n', first + 1);
    if (second < 0) return {};
    const QList<QByteArray> fields = raw.mid(first + 1, second - first - 1).split(' ');
    bool ok = false;
    const qint64 size = fields.size() >= 2 ? fields[1].toLongLong(&ok) : 0;
    if (!ok || size <= 0 || size > 256 * 1024 * 1024) return {};
    // qUncompress takes the expected size as four big-endian bytes before the zlib stream.
    QByteArray framed(4, '\0');
    for (int i = 0; i < 4; i++) framed[i] = char((size >> (24 - 8 * i)) & 0xFF);
    framed += raw.mid(second + 1);
    return qUncompress(framed);
}

} // namespace

bool GmicCatalogue::load(const QString& path, QString* error) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) { if (error) *error = QObject::tr("Couldn't read %1").arg(path); return false; }
    const QByteArray text = catalogueText(file.readAll());
    if (text.isEmpty()) { if (error) *error = QObject::tr("%1 is not a G'MIC definition file").arg(path); return false; }
    QTextStream in(text);
    in.setEncoding(QStringConverter::Utf8);
    filters_.clear();
    source_ = path;
    // Folder lines nest by leading underscores: `_<b>Name</b>` opens a folder at the top, and `__<b>Name</b>`
    // (G'MIC 3.4 on) or a plain `<b>Name</b>` (older files) a subfolder of the last top folder; filters show
    // under "Top / Sub". Italic lines are author subfolders of the top folder (Testing's, in practice).
    QString topFolder, folder;
    GmicFilter* current = nullptr;
    bool skipping = false;
    static const QRegularExpression folderRe(R"(^(_*)<b>(.*)</b>\s*$)");
    // Under Testing, each author's filters sit in an italic subfolder: `<i>Name</i>` (or `_<i>Name</i>`).
    static const QRegularExpression authorRe(R"(^_*<i>(.*)</i>\s*$)");
    // `Name:command, preview(zoom)` with G'MIC-Qt's marks after the preview (+ accepts resizing, * random).
    static const QRegularExpression filterRe(R"(^([^:]+):([^,]+)(?:,([^(]*)(?:\(.*\))?[+*]*)?\s*$)");
    while (!in.atEnd()) {
        QString line = in.readLine();
        if (!line.startsWith("#@gui ") && !line.startsWith("#@gui:")) continue;
        QString body = line.mid(5).trimmed();
        if (body.startsWith(':')) {
            // A parameter of the current filter.
            if (!current || skipping) continue;
            GmicParam p;
            if (parseParam(body.mid(1), p)) current->params.push_back(p);
            else { skipping = true; filters_.pop_back(); current = nullptr; }   // a control we can't drive: drop the filter
            continue;
        }
        auto fm = folderRe.match(body);
        if (fm.hasMatch()) {
            QString name = fm.captured(2).trimmed();
            name.remove(QRegularExpression("<[^>]*>"));
            if (fm.captured(1).size() != 1 && !topFolder.isEmpty()) folder = topFolder + " / " + name;
            else { topFolder = name; folder = name; }
            current = nullptr; skipping = false;
            continue;
        }
        if (auto am = authorRe.match(body); am.hasMatch() && !topFolder.isEmpty()) {
            QString name = am.captured(1).trimmed();
            name.remove(QRegularExpression("<[^>]*>"));
            folder = topFolder + " / " + name;
            current = nullptr; skipping = false;
            continue;
        }
        auto m = filterRe.match(body);
        if (!m.hasMatch()) { current = nullptr; skipping = false; continue; }
        GmicFilter f;
        f.name = m.captured(1).trimmed();
        f.name.remove(QRegularExpression("<[^>]*>"));
        f.command = m.captured(2).trimmed();
        f.previewCommand = m.captured(3).trimmed();
        f.folder = folder;
        // Hidden entries, and the pages with no command (About, Release Notes, ...).
        if (f.name.startsWith('_') || f.command.isEmpty() || f.command == "_none_") { current = nullptr; skipping = true; continue; }
        filters_.push_back(f);
        current = &filters_.back();
        skipping = false;
    }
    std::stable_sort(filters_.begin(), filters_.end(), [](const GmicFilter& a, const GmicFilter& b) { return a.folder == b.folder ? a.name.localeAwareCompare(b.name) < 0 : a.folder.localeAwareCompare(b.folder) < 0; });
    return !filters_.empty();
}

QString GmicCatalogue::ownFile() {
    QString digits = GmicRunner::version();
    digits.remove('.');
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/gmic/update" + digits + ".gmic";
}

const QHash<QString, QString>& GmicCatalogue::unsupported() {
    static const QHash<QString, QString> list = [] {
        QHash<QString, QString> out;
        QFile file(":/gmic/unsupported.txt");
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return out;
        for (const QByteArray& raw : file.readAll().split('\n')) {
            const QString line = QString::fromUtf8(raw);
            if (line.isEmpty() || line.startsWith('#')) continue;
            const QStringList fields = line.split('\t');
            if (fields.size() >= 2) out.insert(fields[0], fields[1]);
        }
        return out;
    }();
    return list;
}

QString GmicCatalogue::updateUrl() {
    QString digits = GmicRunner::version();
    digits.remove('.');
    return "https://gmic.eu/update" + digits + ".gmic";
}

QString GmicCatalogue::preferredFile() {
    QString own = ownFile();
    if (QFileInfo::exists(own)) return own;
    // G'MIC-Qt keeps its copy in ~/.config/gmic; take the newest version there.
    QDir dir(QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + "/gmic");
    QStringList names = dir.entryList({"update*.gmic"}, QDir::Files, QDir::Name);
    if (names.isEmpty()) return {};
    return dir.filePath(names.last());
}

// ---- running ---------------------------------------------------------------------------------------

GmicRunner::GmicRunner(QObject* parent) : QObject(parent) {}
GmicRunner::~GmicRunner() { cancel(); }

QString GmicRunner::executable() {
    QString env = qEnvironmentVariable("COMPOSITOR_GMIC");
    if (!env.isEmpty() && QFileInfo(env).isExecutable()) return env;
    return QStandardPaths::findExecutable("gmic");
}

bool GmicRunner::inProcess() {
#ifdef COMPOSITOR_HAVE_LIBGMIC
    // Opt-in: libgmic 4.0.5 crashes inside `sharpen` (its default inverse-diffusion mode) when called as a
    // library although the executable handles it, and a crash in-process takes the editor down with it.
    return qEnvironmentVariableIsSet("COMPOSITOR_GMIC_INPROCESS");
#else
    return false;
#endif
}

bool GmicRunner::available() { return inProcess() || !executable().isEmpty(); }

QString GmicRunner::version() {
    static QString cached;
    if (!cached.isEmpty()) return cached;
#ifdef COMPOSITOR_HAVE_LIBGMIC
    if (inProcess()) { cached = QString("%1.%2.%3").arg(gmic_version / 100).arg(gmic_version / 10 % 10).arg(gmic_version % 10); return cached; }
#endif
    QString exe = executable();
    if (exe.isEmpty()) return {};
    QProcess p;
    p.start(exe, {"-version"});
    p.waitForFinished(5000);
    static const QRegularExpression re(R"(Version\s+(\d+\.\d+\.\d+))");
    auto m = re.match(QString::fromUtf8(p.readAllStandardOutput()) + QString::fromUtf8(p.readAllStandardError()));
    cached = m.hasMatch() ? m.captured(1) : QStringLiteral("0.0.0");
    return cached;
}

QStringList GmicRunner::tokenize(const QString& command) {
    QStringList out;
    QString current;
    bool quoted = false;
    for (QChar c : command) {
        if (c == '"') { quoted = !quoted; current += c; continue; }
        if (c.isSpace() && !quoted) { if (!current.isEmpty()) { out << current; current.clear(); } continue; }
        current += c;
    }
    if (!current.isEmpty()) out << current;
    return out;
}

bool GmicRunner::allowedForAutomation(const QString& command, QString* why) {
    if (qEnvironmentVariableIntValue("COMPOSITOR_GMIC_UNRESTRICTED") == 1) return true;
    auto refuse = [&](const QString& reason) { if (why) *why = reason; return false; };
    // Characters that make strings, paths, URLs, substitutions, math expressions or definitions.
    static const QRegularExpression forbidden(QStringLiteral(R"([$@{}`\"'/:;()<>|&=\n\r])"));
    const QRegularExpressionMatch bad = forbidden.match(command);
    if (bad.hasMatch()) return refuse(QStringLiteral("automation runs only filter names and numbers; \"%1\" is not allowed").arg(bad.captured()));
    static const QSet<QString> builtins = {
        "blur", "blur_x", "blur_y", "blur_xy", "blur_angular", "blur_radial", "blur_linear", "bilateral", "sharpen", "unsharp",
        "deblur", "smooth", "denoise", "median", "erode", "dilate", "edges", "gradient_norm", "normalize", "equalize", "negate",
        "threshold", "cut", "sepia", "cartoon", "pencilbw", "sketchbw", "drawing", "painting", "cubism", "kuwahara", "noise",
        "pixelize", "vignette", "mirror", "solarize", "posterize", "glow", "emboss"};
    // The catalogue's filter commands (and their preview variants), read once.
    static const QSet<QString> catalogue = [] {
        QSet<QString> names;
        GmicCatalogue c;
        if (const QString path = GmicCatalogue::preferredFile(); !path.isEmpty() && c.load(path))
            for (const GmicFilter& f : c.filters()) {
                names.insert(f.command);
                if (!f.previewCommand.isEmpty()) names.insert(f.previewCommand);
            }
        return names;
    }();
    static const QRegularExpression numbers(QStringLiteral(R"(^[-+]?[0-9.,eE%+-]*[0-9][0-9.,eE%+-]*$)"));
    const QStringList tokens = tokenize(command);
    if (tokens.isEmpty()) return refuse(QStringLiteral("the command is empty"));
    for (int i = 0; i < tokens.size(); i++) {
        const QString& token = tokens[i];
        if (numbers.match(token).hasMatch()) {
            if (i == 0) return refuse(QStringLiteral("the command must start with a filter name"));
            continue;
        }
        QString name = token;
        if (name.startsWith('-') || name.startsWith('+')) name.remove(0, 1);
        if (!builtins.contains(name) && !catalogue.contains(name))
            return refuse(QStringLiteral("\"%1\" is not a filter from the catalogue (gmic.filters) or a known built-in").arg(token));
    }
    return true;
}

namespace {

/// The gmic invocation for one round trip: input, the command's tokens, output.
/// G'MIC runs with no display: some catalogue entries are interactive programs (games, editors that wait
/// for clicks in a window of their own), and without a display they fail at once instead of opening one.
void withoutDisplay(QProcess& p) {
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.remove("DISPLAY");
    env.remove("WAYLAND_DISPLAY");
    p.setProcessEnvironment(env);
}

/// G'MIC's last error line without its terminal colour codes.
QString errorLine(QProcess& p) {
    static const QRegularExpression ansi(QStringLiteral("\x1b\\[[0-9;]*m"));
    QString text = QString::fromUtf8(p.readAllStandardError()).remove(ansi).trimmed();
    return text.isEmpty() ? QString() : text.section('\n', -1).trimmed();
}

/// The gmic invocation for one round trip. The command goes through a one-line script, so G'MIC parses it as
/// G'MIC-Qt's does: text arguments with spaces or quotes ("(c) G'MIC", an expression) stay one argument, where
/// passing it item by item on the command line split them.
QStringList argumentsFor(const QString& command, const QString& inPath, const QString& outPath) {
    const QString script = QFileInfo(inPath).dir().filePath("command.gmic");
    QFile file(script);
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        QString line = command;
        line.replace('\n', ' ').replace('\r', ' ');
        file.write(("compositor_run :\n  " + line + "\n").toUtf8());
    }
    return {"-v", "-1", "-m", script, inPath, "compositor_run", "-o", outPath};
}

std::shared_ptr<compositor::Image> readResult(const QString& outPath, int width, int height, QString* error) {
    std::string err;
    if (!QFileInfo::exists(outPath)) {
        // Several images come out as out_000000.png, out_000001.png, ...: a filter that makes layers.
        const QFileInfo info(outPath);
        const int count = int(QDir(info.path()).entryList({info.completeBaseName() + "_*." + info.suffix()}, QDir::Files).size());
        if (error) *error = count > 1 ? QObject::tr("The filter makes %1 layers; only filters that give back one image are supported here.").arg(count)
                                      : QObject::tr("G'MIC produced no image.");
        return nullptr;
    }
    auto image = compositor::readPngImage(outPath.toStdString(), &err);
    if (!image) { if (error) *error = QObject::tr("G'MIC produced no image (%1).").arg(QString::fromStdString(err)); return nullptr; }
    if (image->width() != width || image->height() != height) {
        if (error) *error = QObject::tr("The filter changed the image size (%1 x %2 to %3 x %4); only filters that keep it are supported here.").arg(width).arg(height).arg(image->width()).arg(image->height());
        return nullptr;
    }
    return image;
}

} // namespace

#ifdef COMPOSITOR_HAVE_LIBGMIC
namespace {

/// One interpreter, kept between runs with the catalogue's definitions loaded (parsing them costs more
/// than most filters), reloaded when the definition file changes. Runs are serialised.
class Interpreter {
public:
    static Interpreter& shared() { static Interpreter instance; return instance; }

    /// Asks the run in progress (if any) to stop; the interpreter polls the flag between commands.
    void abort() { abortFlag_ = true; }

    std::shared_ptr<compositor::Image> run(const compositor::Image& source, const QString& command, QString* error) {
        std::lock_guard<std::mutex> lock(mutex_);
        reload();
        abortFlag_ = false;
        // Straight RGBA as G'MIC's planar floats, 0..255.
        const int w = source.width(), h = source.height();
        gmic_list<float> images;
        gmic_list<char> names;
        images.assign(1);
        names.assign(1);
        images[0].assign(w, h, 1, 4);
        float* planes = images[0]._data;
        const size_t plane = size_t(w) * h;
        for (int y = 0; y < h; y++) {
            const uint8_t* p = source.row(y);
            for (int x = 0; x < w; x++, p += 4) {
                const size_t i = size_t(y) * w + size_t(x);
                const float a = p[3];
                for (int c = 0; c < 3; c++) planes[c * plane + i] = a > 0 ? std::min(255.0f, p[c] * 255.0f / a) : 0.0f;
                planes[3 * plane + i] = a;
            }
        }
        try {
            gmic_->run(("v -1 " + command).toUtf8().constData(), images, names);
        } catch (gmic_exception& e) {
            if (error) *error = abortFlag_ ? QString() : QString::fromUtf8(e.what()).trimmed().section('\n', -1);
            return nullptr;
        } catch (std::exception& e) {
            if (error) *error = QString::fromUtf8(e.what());
            return nullptr;
        }
        if (images._width < 1) { if (error) *error = QObject::tr("G'MIC produced no image."); return nullptr; }
        const gmic_image<float>& out = images[0];
        if (int(out._width) != w || int(out._height) != h) {
            if (error) *error = QObject::tr("The filter changed the image size (%1 x %2 to %3 x %4); only filters that keep it are supported here.").arg(w).arg(h).arg(out._width).arg(out._height);
            return nullptr;
        }
        // Back to premultiplied bytes; a gray or RGB result keeps the source's alpha.
        auto result = std::make_shared<compositor::Image>(w, h);
        const int spectrum = int(out._spectrum);
        const float* o = out._data;
        for (int y = 0; y < h; y++) {
            uint8_t* p = result->row(y);
            const uint8_t* src = source.row(y);
            for (int x = 0; x < w; x++, p += 4, src += 4) {
                const size_t i = size_t(y) * w + size_t(x);
                float rgb[3];
                if (spectrum >= 3) for (int c = 0; c < 3; c++) rgb[c] = o[c * plane + i];
                else for (int c = 0; c < 3; c++) rgb[c] = o[i];
                const float alpha = spectrum == 4 ? o[3 * plane + i] : spectrum == 2 ? o[plane + i] : float(src[3]);
                const unsigned a = unsigned(std::clamp(alpha + 0.5f, 0.0f, 255.0f));
                for (int c = 0; c < 3; c++) p[c] = uint8_t(std::min(a, unsigned(std::clamp(rgb[c], 0.0f, 255.0f) * a / 255 + 0.5f)));
                p[3] = uint8_t(a);
            }
        }
        return result;
    }

private:
    void reload() {
        QString path = GmicCatalogue::preferredFile();
        QDateTime stamp = path.isEmpty() ? QDateTime() : QFileInfo(path).lastModified();
        if (gmic_ && path == loadedPath_ && stamp == loadedStamp_) return;
        gmic_ = std::make_unique<gmic>("", nullptr, true, nullptr, &abortFlag_, 0.0f);
        loadedPath_ = path; loadedStamp_ = stamp;
        if (path.isEmpty()) return;
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) return;
        QByteArray text = file.readAll();
        try { gmic_->add_commands(text.constData()); } catch (...) {}
    }

    std::mutex mutex_;
    std::unique_ptr<gmic> gmic_;
    bool abortFlag_ = false;   // read by the interpreter mid-run, written by whoever cancels
    QString loadedPath_;
    QDateTime loadedStamp_;
};

} // namespace
#endif

std::shared_ptr<compositor::Image> GmicRunner::runSync(const compositor::Image& source, const QString& command, QString* error, int timeoutMs) {
#ifdef COMPOSITOR_HAVE_LIBGMIC
    if (inProcess()) return Interpreter::shared().run(source, command, error);
#endif
    QString exe = executable();
    if (exe.isEmpty()) { if (error) *error = QObject::tr("G'MIC is not installed (no gmic executable on PATH)."); return nullptr; }
    QTemporaryDir dir;
    if (!dir.isValid()) { if (error) *error = QObject::tr("Couldn't create a temporary folder."); return nullptr; }
    QString inPath = dir.filePath("in.png"), outPath = dir.filePath("out.png");
    std::string err;
    if (!compositor::writePngImage(inPath.toStdString(), source, 0, &err)) { if (error) *error = QString::fromStdString(err); return nullptr; }
    QProcess p;
    withoutDisplay(p);
    p.start(exe, argumentsFor(command, inPath, outPath));
    if (!p.waitForFinished(timeoutMs)) { p.kill(); if (error) *error = QObject::tr("G'MIC took too long and was stopped."); return nullptr; }
    if (p.exitStatus() != QProcess::NormalExit || p.exitCode() != 0) {
        const QString text = errorLine(p);
        if (error) *error = text.isEmpty() ? QObject::tr("G'MIC failed.") : text;
        return nullptr;
    }
    return readResult(outPath, source.width(), source.height(), error);
}

void GmicRunner::start(std::shared_ptr<const compositor::Image> source, const QString& command, int timeoutMs) {
    cancel();
#ifdef COMPOSITOR_HAVE_LIBGMIC
    if (inProcess()) {
        const uint64_t run = ++run_;
        abort_ = std::make_shared<std::atomic<bool>>(false);
        std::shared_ptr<std::atomic<bool>> abort = abort_;
        worker_ = std::thread([this, source, command, run, abort] {
            if (abort->load()) return;
            QString error;
            std::shared_ptr<compositor::Image> result = Interpreter::shared().run(*source, command, &error);
            if (abort->load()) return;
            QMetaObject::invokeMethod(this, [this, run, result, error] { if (run == run_) emit finished(result, error); }, Qt::QueuedConnection);
        });
        return;
    }
#endif
    QString exe = executable();
    if (exe.isEmpty()) { emit finished(nullptr, QObject::tr("G'MIC is not installed (no gmic executable on PATH).")); return; }
    dir_ = std::make_unique<QTemporaryDir>();
    if (!dir_->isValid()) { emit finished(nullptr, QObject::tr("Couldn't create a temporary folder.")); return; }
    QString inPath = dir_->filePath("in.png"), outPath = dir_->filePath("out.png");
    std::string err;
    if (!compositor::writePngImage(inPath.toStdString(), *source, 0, &err)) { emit finished(nullptr, QString::fromStdString(err)); return; }
    expectedWidth_ = source->width();
    expectedHeight_ = source->height();
    process_ = new QProcess(this);
    withoutDisplay(*process_);
    if (timeoutMs > 0) {
        if (!limit_) { limit_ = new QTimer(this); limit_->setSingleShot(true); }
        limit_->disconnect();
        connect(limit_, &QTimer::timeout, this, [this] {
            if (!process_) return;
            cancel();
            emit finished(nullptr, QObject::tr("G'MIC took too long and was stopped."));
        });
        limit_->start(timeoutMs);
    }
    connect(process_, &QProcess::finished, this, [this, outPath](int code, QProcess::ExitStatus status) {
        if (limit_) limit_->stop();
        QProcess* p = process_;
        process_ = nullptr;
        std::shared_ptr<compositor::Image> result;
        QString error;
        if (status != QProcess::NormalExit || code != 0) {
            const QString text = errorLine(*p);
            error = text.isEmpty() ? QObject::tr("G'MIC failed.") : text;
        } else result = readResult(outPath, expectedWidth_, expectedHeight_, &error);
        p->deleteLater();
        emit finished(result, error);
    });
    process_->start(exe, argumentsFor(command, inPath, outPath));
}

bool GmicRunner::running() const { return process_ != nullptr || (abort_ && worker_.joinable() && !abort_->load()); }

void GmicRunner::cancel() {
    if (worker_.joinable()) {
        // The interpreter polls its flag between commands and throws its way out.
        if (abort_) abort_->store(true);
        run_++;
#ifdef COMPOSITOR_HAVE_LIBGMIC
        Interpreter::shared().abort();
#endif
        worker_.join();
        abort_.reset();
    }
    if (limit_) limit_->stop();
    if (!process_) return;
    QProcess* p = process_;
    process_ = nullptr;
    p->disconnect(this);
    p->kill();
    p->waitForFinished(1000);
    p->deleteLater();
}


} // namespace app
