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

## Fuzzing

The PSD reader, its block parsers and the smaller format readers have libFuzzer targets under AddressSanitizer
and UndefinedBehaviorSanitizer; how to build and run them is in [docs/fuzzing.md](docs/fuzzing.md). Every crash
found that way is kept as a regression test in `tests/hostile_input_tests.cpp`.
