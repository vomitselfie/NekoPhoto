#include "AutomationGuard.h"
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <memory>

namespace app::automation {

Framed takeLines(QByteArray& buffer, qsizetype cap) {
    Framed out;
    qsizetype start = 0;
    while (true) {
        const qsizetype newline = buffer.indexOf('\n', start);
        if (newline < 0) break;
        if (newline - start > cap) { out.overflow = true; break; }
        QByteArray line = buffer.mid(start, newline - start).trimmed();
        start = newline + 1;
        if (!line.isEmpty()) out.lines.append(std::move(line));
    }
    if (!out.overflow && buffer.size() - start > cap) out.overflow = true;
    if (out.overflow) buffer.clear();
    else buffer.remove(0, start);
    return out;
}

namespace {

QStringList& roots() {
    static QStringList list;
    return list;
}

#ifdef _WIN32
constexpr Qt::CaseSensitivity pathCase = Qt::CaseInsensitive;
#else
constexpr Qt::CaseSensitivity pathCase = Qt::CaseSensitive;
#endif

bool within(const QString& path, const QString& root) {
    if (path.compare(root, pathCase) == 0) return true;
    const QString prefix = root.endsWith('/') ? root : root + '/';
    return path.startsWith(prefix, pathCase);
}

/// `path` with its deepest existing folder resolved through symbolic links and the parts below it kept as written.
QString resolved(const QString& path) {
    const QString clean = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
    QString existing = QFileInfo(clean).absolutePath(), rest = QFileInfo(clean).fileName();
    while (!QFileInfo(existing).exists()) {
        const QFileInfo up(existing);
        if (up.absolutePath() == existing) break;   // the root of a missing drive
        rest = up.fileName() + '/' + rest;
        existing = up.absolutePath();
    }
    const QString canonical = QFileInfo(existing).canonicalFilePath();
    if (canonical.isEmpty()) return {};
    return QDir::cleanPath(canonical + '/' + rest);
}

std::unique_ptr<QFile>& logFile() {
    static std::unique_ptr<QFile> file;
    return file;
}

} // namespace

void setWriteRoots(const QStringList& list) {
    QStringList& r = roots();
    r.clear();
    for (const QString& root : list) {
        if (root.trimmed().isEmpty()) continue;
        const QString canonical = QFileInfo(root.trimmed()).canonicalFilePath();
        // A root that does not exist yet still counts, as written, so a typo refuses rather than allows.
        r << (canonical.isEmpty() ? QDir::cleanPath(QFileInfo(root.trimmed()).absoluteFilePath()) : canonical);
    }
    r.removeDuplicates();
}

QStringList writeRoots() { return roots(); }

namespace {
int personsRequests = 0;
}

PersonsRequest::PersonsRequest() { personsRequests++; }
PersonsRequest::~PersonsRequest() { personsRequests--; }

QString writeRootRefusal(const QString& path) {
    const QStringList& r = roots();
    if (r.isEmpty() || personsRequests > 0) return {};
    if (QFileInfo(path).isSymLink()) return QStringLiteral("%1 is a symbolic link; automation writes don't follow links while write roots are set").arg(path);
    const QString target = resolved(path);
    if (!target.isEmpty())
        for (const QString& root : r) if (within(target, root)) return {};
    return QStringLiteral("%1 is outside the folders automation may write to (%2)").arg(path, r.join(", "));
}

QString writeRefusal(const QString& path, bool overwrite) {
    QString why = writeRootRefusal(path);
    if (!why.isEmpty()) return why;
    const QFileInfo info(path);
    if (!overwrite && (info.exists() || info.isSymLink()))
        return QStringLiteral("%1 already exists; pass overwrite: true to replace it").arg(path);
    return {};
}

void setAuditLog(const QString& path) {
    std::unique_ptr<QFile>& file = logFile();
    file.reset();
    if (path.isEmpty()) return;
    auto f = std::make_unique<QFile>(path);
    if (!f->open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        qWarning("automation: couldn't open the log %s: %s", qPrintable(path), qPrintable(f->errorString()));
        return;
    }
    f->setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    file = std::move(f);
}

bool auditing() { return logFile() != nullptr; }

QString auditLine(const QString& time, const QString& method, const QJsonObject& params, const QJsonObject& response, qint64 milliseconds) {
    // Only where a request reads or writes: never image data, strokes, step lists or other bulk.
    static const char* keys[] = {"path", "paths", "directory", "input", "output"};
    auto clip = [](const QString& s) { return s.size() > 1024 ? s.left(1024) + QStringLiteral("…") : s; };
    QJsonObject shown;
    for (const char* key : keys) {
        const QJsonValue v = params.value(QLatin1String(key));
        if (v.isString()) shown[QLatin1String(key)] = clip(v.toString());
        else if (v.isArray()) {
            const QJsonArray list = v.toArray();
            QJsonArray kept;
            for (qsizetype i = 0; i < list.size() && i < 32; i++) if (list[i].isString()) kept.append(clip(list[i].toString()));
            if (list.size() > 32) kept.append(QStringLiteral("… %1 more").arg(list.size() - 32));
            shown[QLatin1String(key)] = kept;
        }
    }
    const QString outcome = response.contains("error") ? QStringLiteral("error %1").arg(response.value("error").toObject().value("code").toInt()) : QStringLiteral("ok");
    QString name = method;
    name.replace('\t', ' ').replace('\n', ' ');
    return time + '\t' + name + '\t' + QString::fromUtf8(QJsonDocument(shown).toJson(QJsonDocument::Compact)) + '\t' + outcome + '\t' + QString::number(milliseconds) + "ms";
}

void audit(const QString& method, const QJsonObject& params, const QJsonObject& response, qint64 milliseconds) {
    std::unique_ptr<QFile>& file = logFile();
    if (!file) return;
    const QString time = QDateTime::currentDateTime().toString(Qt::ISODateWithMs);
    file->write((auditLine(time, method, params, response, milliseconds) + '\n').toUtf8());
    file->flush();
}

} // namespace app::automation
