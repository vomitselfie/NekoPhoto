#include "Platform.h"

#include <QDir>
#include <QStandardPaths>

#ifdef _WIN32
#include <windows.h>
#include <cstdio>
#endif

namespace app::platform {

#ifdef _WIN32

void attachParentConsole() {
    // A stream that already goes somewhere (a pipe or a file, as in `nekophoto --version > v.txt` or a
    // subprocess capturing output) keeps going there.
    auto redirected = [](DWORD which) {
        HANDLE h = GetStdHandle(which);
        return h != nullptr && h != INVALID_HANDLE_VALUE && GetFileType(h) != FILE_TYPE_UNKNOWN;
    };
    const bool out = redirected(STD_OUTPUT_HANDLE), err = redirected(STD_ERROR_HANDLE);
    if (!out || !err) {
        if (AttachConsole(ATTACH_PARENT_PROCESS)) {
            FILE* ignored = nullptr;
            if (!out) freopen_s(&ignored, "CONOUT$", "w", stdout);
            if (!err) freopen_s(&ignored, "CONOUT$", "w", stderr);
        } else if (!out && !err) {
            return;   // started from Explorer: nowhere to print, so Qt keeps showing --help in a message box
        }
    }
    // Qt's parser shows --help and --version in a message box in a GUI program; print them for the caller instead.
    _putenv_s("QT_COMMAND_LINE_PARSER_NO_GUI_MESSAGE_BOXES", "1");
}

int finishProcess(int status) {
    std::fflush(nullptr);
    TerminateProcess(GetCurrentProcess(), UINT(status));
    return status;
}

QString defaultLocalSocket(const QString& baseName) {
    QString user = qEnvironmentVariable("USERNAME");
    if (user.isEmpty()) user = QStringLiteral("user");
    QString name = baseName;
    if (name.endsWith(QLatin1String(".sock"))) name.chop(5);
    return name + QLatin1Char('-') + user;
}

QString localServerName(const QString& socket) {
    static const QString prefix = QStringLiteral("\\\\.\\pipe\\");
    if (socket.startsWith(prefix, Qt::CaseInsensitive)) return socket;
    QString name = socket;
    name.replace(QLatin1Char('\\'), QLatin1Char('_')).replace(QLatin1Char('/'), QLatin1Char('_'));
    return name;
}

#else

void attachParentConsole() {}

int finishProcess(int status) { return status; }

QString defaultLocalSocket(const QString& baseName) {
    QString runtime = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    if (runtime.isEmpty()) runtime = QDir::tempPath();
    return runtime + QLatin1Char('/') + baseName;
}

QString localServerName(const QString& socket) { return socket; }

#endif

} // namespace app::platform
