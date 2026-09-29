#include "Platform.h"

#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QStandardPaths>

#ifdef _WIN32
#include <windows.h>
#include <cstdio>
#include <string>
#elif defined(__linux__)
#include <QtGui/qguiapplication_platform.h>
#include <dlfcn.h>
#include <sys/resource.h>
#include <cstdint>
#include <cstdlib>
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

QByteArray systemMonitorProfile() {
    // The display's colour profile, as Windows' colour management has it for the screen's device context.
    HDC screen = GetDC(nullptr);
    if (!screen) return {};
    DWORD length = 0;
    GetICMProfileW(screen, &length, nullptr);
    QByteArray bytes;
    if (length > 0 && length < 32768) {
        std::wstring path(length, L'\0');
        if (GetICMProfileW(screen, &length, path.data())) {
            path.resize(wcslen(path.c_str()));
            QFile file(QString::fromStdWString(path));
            if (file.open(QIODevice::ReadOnly) && file.size() < (qint64(64) << 20)) bytes = file.readAll();
        }
    }
    ReleaseDC(nullptr, screen);
    return bytes;
}

QString localServerName(const QString& socket) {
    static const QString prefix = QStringLiteral("\\\\.\\pipe\\");
    if (socket.startsWith(prefix, Qt::CaseInsensitive)) return socket;
    QString name = socket;
    name.replace(QLatin1Char('\\'), QLatin1Char('_')).replace(QLatin1Char('/'), QLatin1Char('_'));
    return name;
}

void lowerProcessPriority(int level) {
    if (level > 0) SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);
}

#else

void lowerProcessPriority(int level) {
    // On Linux this sets the calling (main) thread; the worker pool, made after it, inherits the value, and the
    // few threads Qt started earlier keep theirs. An unprivileged process can only lower its priority, never raise it.
    if (level > 0) (void)setpriority(PRIO_PROCESS, 0, level >= 2 ? 10 : 5);
}

void attachParentConsole() {}

int finishProcess(int status) { return status; }

QString defaultLocalSocket(const QString& baseName) {
    QString runtime = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    if (runtime.isEmpty()) runtime = QDir::tempPath();
    return runtime + QLatin1Char('/') + baseName;
}

QString localServerName(const QString& socket) { return socket; }

#if defined(__linux__) && QT_CONFIG(xcb)
namespace {
// The few libxcb calls, declared here and resolved at run time, so the build needs no xcb headers or library.
struct XcbCookie { unsigned int sequence; };
struct XcbAtomReply { uint8_t responseType, pad0; uint16_t sequence; uint32_t length; uint32_t atom; };
struct XcbPropertyReply { uint8_t responseType, format; uint16_t sequence; uint32_t length, type, bytesAfter, valueLength; uint8_t pad0[12]; };
struct XcbScreenIterator { const uint32_t* data; int rem, index; };   // an xcb_screen_t starts with its root window
} // namespace

QByteArray systemMonitorProfile() {
    auto* x11 = qGuiApp ? qGuiApp->nativeInterface<QNativeInterface::QX11Application>() : nullptr;
    xcb_connection_t* connection = x11 ? x11->connection() : nullptr;
    if (!connection) return {};
    void* xcb = dlopen("libxcb.so.1", RTLD_LAZY | RTLD_LOCAL);
    if (!xcb) return {};
    using InternAtom = XcbCookie (*)(xcb_connection_t*, uint8_t, uint16_t, const char*);
    using InternAtomReply = XcbAtomReply* (*)(xcb_connection_t*, XcbCookie, void**);
    using GetProperty = XcbCookie (*)(xcb_connection_t*, uint8_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
    using GetPropertyReply = XcbPropertyReply* (*)(xcb_connection_t*, XcbCookie, void**);
    using PropertyValue = void* (*)(const XcbPropertyReply*);
    using PropertyLength = int (*)(const XcbPropertyReply*);
    using GetSetup = const void* (*)(xcb_connection_t*);
    using Roots = XcbScreenIterator (*)(const void*);
    auto internAtom = reinterpret_cast<InternAtom>(dlsym(xcb, "xcb_intern_atom"));
    auto internAtomReply = reinterpret_cast<InternAtomReply>(dlsym(xcb, "xcb_intern_atom_reply"));
    auto getProperty = reinterpret_cast<GetProperty>(dlsym(xcb, "xcb_get_property"));
    auto getPropertyReply = reinterpret_cast<GetPropertyReply>(dlsym(xcb, "xcb_get_property_reply"));
    auto propertyValue = reinterpret_cast<PropertyValue>(dlsym(xcb, "xcb_get_property_value"));
    auto propertyLength = reinterpret_cast<PropertyLength>(dlsym(xcb, "xcb_get_property_value_length"));
    auto getSetup = reinterpret_cast<GetSetup>(dlsym(xcb, "xcb_get_setup"));
    auto roots = reinterpret_cast<Roots>(dlsym(xcb, "xcb_setup_roots_iterator"));
    QByteArray bytes;
    if (internAtom && internAtomReply && getProperty && getPropertyReply && propertyValue && propertyLength && getSetup && roots) {
        const XcbScreenIterator screens = roots(getSetup(connection));
        const char name[] = "_ICC_PROFILE";
        XcbAtomReply* atom = screens.data ? internAtomReply(connection, internAtom(connection, 1, uint16_t(sizeof name - 1), name), nullptr) : nullptr;
        if (atom && atom->atom) {
            // Up to 16 MB (the length is in 32-bit units); AnyPropertyType.
            XcbPropertyReply* reply = getPropertyReply(connection, getProperty(connection, 0, screens.data[0], atom->atom, 0, 0, 4u << 20), nullptr);
            if (reply) {
                const int length = propertyLength(reply);
                if (reply->format == 8 && length > 0) bytes = QByteArray(static_cast<const char*>(propertyValue(reply)), length);
                std::free(reply);
            }
        }
        std::free(atom);
    }
    dlclose(xcb);
    return bytes;
}
#else
QByteArray systemMonitorProfile() { return {}; }
#endif

#endif

unsigned long long availableMemory() {
#ifdef _WIN32
    MEMORYSTATUSEX status;
    status.dwLength = sizeof status;
    return GlobalMemoryStatusEx(&status) ? (unsigned long long)status.ullAvailPhys : 0;
#else
    QFile file(QStringLiteral("/proc/meminfo"));
    if (!file.open(QIODevice::ReadOnly)) return 0;
    for (const QByteArray& line : file.readAll().split('\n'))
        if (line.startsWith("MemAvailable:")) return line.mid(13).trimmed().split(' ').value(0).toULongLong() * 1024ull;
    return 0;
#endif
}

} // namespace app::platform
