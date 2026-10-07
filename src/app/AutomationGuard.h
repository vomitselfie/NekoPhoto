// Limits on what the automation socket may do, kept apart from the handlers so they can be tested with Qt Core
// alone: how a connection's bytes become requests (with a size cap), where requests may write files (the
// overwrite rule and the optional write roots), and the optional audit log. See SECURITY.md and
// docs/automation.md ("Files and limits").
#pragma once
#include <QByteArray>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

namespace app::automation {

/// The most a connection may send without finishing a request line: past it the server answers -32600 and
/// closes the connection.
constexpr qsizetype maxRequestBytes = qsizetype(64) << 20;

struct Framed {
    QList<QByteArray> lines;   // complete request lines, trimmed, empty ones dropped
    bool overflow = false;     // a line, or the unfinished rest, is longer than the cap
};
/// Takes the complete lines out of `buffer`. On overflow the lines before the long one are still returned and the
/// buffer is cleared.
Framed takeLines(QByteArray& buffer, qsizetype cap = maxRequestBytes);

/// Folders automation writes must land in (the automation/writeRoots setting and --rpc-write-root); empty, the
/// default, lets requests write anywhere the user can.
void setWriteRoots(const QStringList& roots);
QStringList writeRoots();

/// Why a request may not write `path` (a file, or a folder it writes into), or an empty string when it may:
/// outside every write root (the parent folder resolved through symbolic links), or a symbolic link itself while
/// roots are set. `path` should be absolute.
QString writeRootRefusal(const QString& path);
/// While one lives, write roots don't apply: the request is the person's own, made from a menu or dialog through
/// the command path (MainWindow::runCommand), and they chose the file in a file dialog. Actions are not covered.
struct PersonsRequest {
    PersonsRequest();
    ~PersonsRequest();
    PersonsRequest(const PersonsRequest&) = delete;
    PersonsRequest& operator=(const PersonsRequest&) = delete;
};
/// writeRootRefusal, and then, unless `overwrite`, a refusal when `path` already exists.
QString writeRefusal(const QString& path, bool overwrite);

/// Appends one line per request to `path` (the automation/log setting and --rpc-log); an empty path stops.
void setAuditLog(const QString& path);
bool auditing();
/// The audit line for a request: time, method, the path-bearing parameters (never image data or long lists),
/// ok or the error code, and how long it took.
QString auditLine(const QString& time, const QString& method, const QJsonObject& params, const QJsonObject& response, qint64 milliseconds);
/// Writes auditLine for a request to the log, when one is set.
void audit(const QString& method, const QJsonObject& params, const QJsonObject& response, qint64 milliseconds);

} // namespace app::automation
