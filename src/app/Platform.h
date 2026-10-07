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
/// Linux (runtimeDirectory(); empty when there is none), a named pipe carrying the user name on Windows
/// (\\.\pipe\nekophoto-<user>), since pipes live in one namespace for the whole machine.
QString defaultLocalSocket(const QString& baseName);

/// Before listening at a local socket path: on Unix, removes what an instance that died left there, but only when it
/// is a socket owned by this user; a file, folder, link or someone else's socket stays and the call returns false
/// with the reason in `error`. Nothing there is fine. On Windows (named pipes) it does what QLocalServer::removeServer
/// does.
bool removeStaleSocket(const QString& serverName, QString* error);

/// The per-user folder for sockets and locks: $XDG_RUNTIME_DIR (through Qt), else <temp>/runtime-<user>, made
/// 0700 and refused unless it is a real folder owned by this user and closed to everyone else (the MCP bridge
/// checks the same). Empty when there is no such folder. Windows: Qt's runtime folder, else the temp folder.
QString runtimeDirectory();

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
