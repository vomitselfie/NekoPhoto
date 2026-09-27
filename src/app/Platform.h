#pragma once

// The few places where Linux and Windows differ, kept in one file so the rest of the app has no #ifdef:
// where the local sockets live (Unix domain sockets here, named pipes on Windows) and how a GUI-subsystem
// executable on Windows still prints --version, --help and --call results to the console it was started from.

#include <QString>

namespace app::platform {

/// Before QApplication: on Windows, reconnect stdout and stderr to the console of the process that started
/// us (cmd, PowerShell), so command-line output shows up although the executable is a GUI program. Output
/// already redirected to a file or pipe is left alone. Nothing to do elsewhere.
void attachParentConsole();

/// The default name of a per-user local socket, e.g. "nekophoto.sock": a file in the runtime directory on
/// Linux ($XDG_RUNTIME_DIR, else Qt's private temp folder), a named pipe carrying the user name on Windows
/// (\\.\pipe\nekophoto-<user>), since pipes live in one namespace for the whole machine.
QString defaultLocalSocket(const QString& baseName);

/// What QLocalServer and QLocalSocket are given for a --rpc-socket value. The identity on Linux. On Windows a
/// pipe name cannot hold a backslash, so a value that looks like a path ("C:/tmp/c.sock") becomes a pipe
/// named after it with the separators replaced; a full \\.\pipe\ name is kept. mcp/nekophoto_mcp.py and
/// tools/rpc_smoke.py apply the same rule.
QString localServerName(const QString& socket);

} // namespace app::platform
