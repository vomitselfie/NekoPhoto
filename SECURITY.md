# Security policy

## Supported versions

Security fixes go into the latest release only. Before reporting, check that the problem is still there in the
[latest release](https://github.com/vomitselfie/nekophoto/releases/latest) or on `main`.

## Reporting a vulnerability

Please report privately, not in a public issue: use GitHub's private vulnerability reporting, on the repository's
**Security** tab, **Report a vulnerability**
([direct link](https://github.com/vomitselfie/nekophoto/security/advisories/new)). That opens a Security Advisory
only the maintainers can see, where we can talk it through and credit you when the fix ships.

Useful in a report: the NekoPhoto version and platform, the steps or the file that triggers it (attach it to the
advisory; don't post it publicly), and what happens (a crash, a sanitizer trace, what an attacker gains).

## Scope

In scope:

- **File parsers fed hostile files**: anything NekoPhoto opens or imports, such as PSD/PSB (with nested smart
  objects), Clip Studio `.clip`, Affinity, Aseprite, GIF, TGA, ICO, SVG, PDF, camera RAW, NekoPhoto `.nekophoto` and `.comp`
  projects, and brush and preset files (`.abr`, `.sut`, `.brushset`, `.brush`, `.pat`, `.asl`, `.grd`, colour
  lookups). A crash, hang, memory-safety bug or out-of-bounds read on a crafted file counts.
- **The automation socket** (`--rpc`, `--rpc-socket`; a named pipe on Windows) and the MCP bridge in `mcp/`:
  another local user connecting, or a request reaching files or commands it should not.
- **The Remove Background model download**: the download and its checksum check, and where the model is stored.

Out of scope: problems that need an attacker who already runs code as your user, bugs in third-party programs
NekoPhoto starts (such as `gmic`) unless NekoPhoto passes them unsafe input, and denial of service by files that
are simply very large within the documented limits.

## The automation socket's threat model

The socket guards against other local users, not against your own: it is a Unix domain socket in a private
runtime folder (`$XDG_RUNTIME_DIR`, else a `runtime-<user>` folder under the temp folder that must be yours and
closed to everyone else) with owner-only access, or a named pipe for your user on Windows. A client that connects
can do what you can do in the editor, including reading and writing files you can reach. Within that:

- A request never replaces an existing file unless it says `overwrite: true` (saving a document to where it lives is
  Save, not a replacement).
- Write roots confine automation writes to chosen folders: the `automation/writeRoots` setting (a list) or
  `--rpc-write-root <dir>` (repeatable). A write must then resolve, through symbolic links in its folders, to a path
  under a root, and its final component may not be a link. Unset, the default, requests write wherever you can.
- A request line is at most 64 MiB; past it the server answers -32600 and closes the connection.
- Before listening, a leftover socket path is removed only when it is a socket owned by you; anything else is left
  alone and the editor does not listen.
- Actions read from a file are checked like `actions.save`: no step may play, record, import, export or batch
  actions (nor an `rpc.batch` step call them), and actions cannot play inside one another more than a few deep. An
  imported action that writes files asks before its first play from the Actions panel or Batch.
- `pixels.gmic` takes only catalogue filters with numbers (see docs/automation.md), within 1..600000 ms.
- An audit log, off by default: the `automation/log` setting or `--rpc-log <file>` appends one line per request
  (time, method, the paths it names, ok or the error code, milliseconds); image data and long lists are never logged.

## Fuzzing

The PSD reader, its block parsers and the smaller format readers have libFuzzer targets under AddressSanitizer
and UndefinedBehaviorSanitizer; how to build and run them is in [docs/fuzzing.md](docs/fuzzing.md). Every crash
found that way is kept as a regression test in `tests/hostile_input_tests.cpp`.
