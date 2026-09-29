#pragma once

// The few places where Linux and Windows differ, kept in one file so the rest of the app has no #ifdef:
// where the local sockets live (Unix domain sockets here, named pipes on Windows) and how a GUI-subsystem
// executable on Windows still prints --version, --help and --call results to the console it was started from.

#include <QByteArray>
#include <QString>

namespace app::platform {

/// Before QApplication: on Windows, reconnect stdout and stderr to the console of the process that started
/// us (cmd, PowerShell), so command-line output shows up although the executable is a GUI program. Output
/// already redirected to a file or pipe is left alone. Nothing to do elsewhere.
void attachParentConsole();

/// Called with main's exit status once everything main owns is destroyed; returns it on Linux. On Windows it
/// flushes the C streams and ends the process there, without the DLL teardown that follows main: under Wine that
/// teardown hung for minutes in libstdc++'s DLL detach once a document had been open.
int finishProcess(int status);

/// The default name of a per-user local socket, e.g. "nekophoto.sock": a file in the runtime directory on
/// Linux ($XDG_RUNTIME_DIR, else Qt's private temp folder), a named pipe carrying the user name on Windows
/// (\\.\pipe\nekophoto-<user>), since pipes live in one namespace for the whole machine.
QString defaultLocalSocket(const QString& baseName);

/// What QLocalServer and QLocalSocket are given for a --rpc-socket value. The identity on Linux. On Windows a
/// pipe name cannot hold a backslash, so a value that looks like a path ("C:/tmp/c.sock") becomes a pipe
/// named after it with the separators replaced; a full \\.\pipe\ name is kept. mcp/nekophoto_mcp.py and
/// tools/rpc_smoke.py apply the same rule.
QString localServerName(const QString& socket);

/// The ICC profile the system has for the (primary) monitor, empty when it reports none: X11's _ICC_PROFILE on the
/// root window (set by colord and other colour managers; read through libxcb, loaded at run time), the display
/// profile on Windows (GetICMProfile). Wayland has no such property: Preferences takes a file there.
QByteArray systemMonitorProfile();

/// Lowers the whole process's scheduling priority so other programs get the CPU first: `level` 1 a little
/// (nice 5; Windows below normal), 2 more (nice 10; Windows below normal too, as idle priority would starve the
/// interface). 0 leaves it. The process can only lower itself.
void lowerProcessPriority(int level);

/// Memory the system can give this process now without swapping (Linux: MemAvailable; Windows: available
/// physical memory), in bytes; 0 when it cannot tell.
unsigned long long availableMemory();

} // namespace app::platform
