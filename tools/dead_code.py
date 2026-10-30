#!/usr/bin/env python3
"""Candidates for dead code, with the evidence for each: a report to read, never a list to delete from blindly.

    tools/dead_code.py [--prepare] [--gc-build build-gc] [--coverage-build build-cov] [--skip gc,coverage,settings,resources,translations]
                       [--json out.json] [--markdown out.md]

Five sources of evidence:

1. Unreferenced functions (gc): a build of its own (build-gc: -O0, so nothing is inlined away, with -ffunction-sections:
   each function in a section of its own) links every executable with -Wl,--gc-sections; the report links each again
   with -Wl,--print-gc-sections -Wl,--trace, and the linker names each function section nothing reached and which
   archive members it loaded at all.
   A function of ours (a strong definition in src/, not an inline or template copy) is
   - unreferenced: dropped, or its object never loaded, in every executable (the app and every test);
   - test-only: dropped from the app but kept by a test;
   Virtual functions are reached through their vtables and slots through moc's tables, so they count as referenced
   while their class is; inline functions in headers are emitted only where used, so an unused one never shows here.
2. Never executed (coverage): a --coverage -O0 build (build-cov) runs the whole ctest, then tools/rpc_smoke.py against a
   headless instance (stopped with SIGTERM, which a small preloaded handler turns into exit() so gcov writes its
   counts); gcov's JSON gives per-file line coverage and every function no test ran. "Never executed" means no test
   reached it, not that no person can.
3. QSettings keys: every QSettings value/setValue/contains/remove with its key (a literal, a constant in the same file,
   or a literal prefix of a key built at run time); keys written and never read, read and never written.
4. Resources: every file in the .qrc and qt_add_resources lists that no source, stylesheet or code-built path mentions
   (by its resource path, a folder it is listed from, or its name as a string, as Icons.h looks icons up).
5. Translations: lupdate over the sources into a copy of each .ts; messages it marks vanished or obsolete
   (tools/check_translations.py fails on those too).

--prepare configures and builds build-gc and build-cov (-j4) and runs the tests in build-cov; it is heavy, so run it as
`../.tools/heavy.sh python3 tools/dead_code.py --prepare` on a shared machine. Without --prepare the report uses what
the build folders already hold and skips a part whose folder is missing, saying so. The corpus oracles run in the
coverage ctest when PATCHY_FIXTURES, PHOTOSHOP_CORPUS and PSD_TOOLS_CORPUS are set.
"""
import argparse
import glob
import json
import os
import re
import shlex
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import xml.etree.ElementTree as ET
from collections import defaultdict
from concurrent.futures import ThreadPoolExecutor

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
JOBS = 4


def run(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, errors="replace", **kw)


def own_source(path):
    """Our own source (src/, not third_party), as a path relative to the repository, or None."""
    p = os.path.realpath(path)
    root = os.path.realpath(ROOT) + os.sep
    if not p.startswith(root):
        return None
    rel = p[len(root):]
    if not rel.startswith("src/") or rel.startswith("src/third_party/"):
        return None
    return rel


def demangle(names):
    if not names:
        return {}
    out = run(["c++filt"], input="\n".join(names)).stdout.split("\n")
    return dict(zip(names, out))


# ---- preparing the build folders ----------------------------------------------------------------------------------

def configure(build, c_flags, linker_flags=""):
    if os.path.exists(os.path.join(build, "build.ninja")):
        return
    # GNU ld rather than lld: the report reads its --print-gc-sections and --trace output.
    cmd = ["cmake", "-S", ROOT, "-B", build, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=None", "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
           "-DNEKOPHOTO_LLD=OFF", f"-DCMAKE_C_FLAGS={c_flags}", f"-DCMAKE_CXX_FLAGS={c_flags}"]
    if linker_flags:
        cmd.append(f"-DCMAKE_EXE_LINKER_FLAGS={linker_flags}")
    subprocess.run(cmd, check=True)


# The build links with --gc-sections; the report links each executable again with the listing flags added (their
# output runs to megabytes per executable, too much for the build's log).
GC_LINK_FLAGS = "-Wl,--gc-sections"
GC_LIST_FLAGS = " -Wl,--print-gc-sections -Wl,--trace -Wl,-Map,{map}"


def prepare(gc_build, cov_build, skip):
    if "gc" not in skip:
        configure(gc_build, "-O0 -ffunction-sections -fdata-sections", GC_LINK_FLAGS)
        subprocess.run(["cmake", "--build", gc_build, f"-j{JOBS}"], check=True)
    if "coverage" not in skip:
        configure(cov_build, "-O0 --coverage", "--coverage")
        subprocess.run(["cmake", "--build", cov_build, f"-j{JOBS}"], check=True)
        for gcda in glob.glob(os.path.join(cov_build, "**", "*.gcda"), recursive=True):
            os.unlink(gcda)
        env = dict(os.environ)
        for key in ("DISPLAY", "WAYLAND_DISPLAY"):
            env.pop(key, None)
        env["QT_QPA_PLATFORM"] = "offscreen"
        # Tests at -O0 with coverage counters run several times slower than in a release build.
        subprocess.run(["ctest", "--test-dir", cov_build, f"-j{JOBS}", "--timeout", "7200", "--output-on-failure"], env=env)
        run_rpc_smoke(cov_build, env)


# SIGTERM becomes exit(), so gcov writes its counts; an abort (a failed standard-library assertion at -O0) becomes
# exit() too, so the requests that ran before it still count (gcov's own dump is not exported to a preloaded library).
SIGTERM_SHIM = r"""
#include <signal.h>
#include <stdlib.h>
static void quit(int s) { (void)s; exit(0); }
static void aborted(int s) { (void)s; exit(134); }   /* not signal-safe, but gcov's exit handler is what is wanted */
__attribute__((constructor)) static void install(void) { signal(SIGTERM, quit); signal(SIGABRT, aborted); }
"""


def run_rpc_smoke(cov_build, env):
    """tools/rpc_smoke.py against a headless coverage build; SIGTERM becomes exit() so the counts are written."""
    shim_dir = os.path.join(cov_build, "dead-code")
    os.makedirs(shim_dir, exist_ok=True)
    src, lib = os.path.join(shim_dir, "sigterm.c"), os.path.join(shim_dir, "sigterm.so")
    with open(src, "w") as f:
        f.write(SIGTERM_SHIM)
    subprocess.run(["cc", "-shared", "-fPIC", "-o", lib, src], check=True)
    sock_dir = tempfile.mkdtemp(prefix="np-cov-", dir="/tmp")
    sock = os.path.join(sock_dir, "s")
    app_env = dict(env, LD_PRELOAD=lib, XDG_CONFIG_HOME=os.path.join(shim_dir, "config"), XDG_DATA_HOME=os.path.join(shim_dir, "data"))
    app = subprocess.Popen([os.path.join(cov_build, "src", "app", "nekophoto"), "--headless", "--rpc-socket", sock, "--demo"], env=app_env,
                           stdout=subprocess.DEVNULL, stderr=open(os.path.join(shim_dir, "app.log"), "w"))
    try:
        for _ in range(600):
            if os.path.exists(sock):
                break
            time.sleep(0.1)
        smoke = subprocess.run([sys.executable, os.path.join(ROOT, "tools", "rpc_smoke.py"), sock], env=env, capture_output=True, text=True)
        print("rpc_smoke:", "passed" if smoke.returncode == 0 else f"failed ({smoke.returncode}): " + (smoke.stderr.strip().splitlines() or ["(no output)"])[-1],
              flush=True)
    finally:
        app.send_signal(signal.SIGTERM)
        try:
            app.wait(timeout=300)
        except subprocess.TimeoutExpired:
            app.kill()
        shutil.rmtree(sock_dir, ignore_errors=True)


# ---- 1. unreferenced functions (gc) --------------------------------------------------------------------------------

# GNU ld: "removing unused section '.text.x' in file 'a.o'"; lld: "removing unused section a.o:(.text.x)".
REMOVING = re.compile(r"removing unused section '([^']+)' in file '([^']+)'")
REMOVING_LLD = re.compile(r"removing unused section (\S+?):\(([^)]+)\)")
ARCHIVE_MEMBER = re.compile(r"^(.*?\.a)\(([^()]*)\)$")


def link_commands(build):
    """[(executable, command)] of every link with the gc flags."""
    out = run(["ninja", "-C", build, "-t", "commands"]).stdout.splitlines()
    links = []
    for cmd in out:
        if "--gc-sections" not in cmd:
            continue
        m = re.search(r"\s-o\s+(\S+)", cmd)
        if m:
            links.append((m.group(1), cmd))
    return links


def relink(build, link):
    exe, cmd = link
    # The executable is written to a scratch name, so the build's own stays as ninja left it.
    scratch = exe + ".deadcode"
    map_path = os.path.join(build, exe + ".deadcode.map")
    cmd = re.sub(r"(\s-o\s+)(\S+)", lambda m: m.group(1) + scratch, cmd, count=1)
    # ninja wraps a link in ": && ... && :", so the flags go next to the build's own rather than at the end.
    cmd = cmd.replace("-Wl,--gc-sections", "-Wl,--gc-sections" + GC_LIST_FLAGS.format(map=shlex.quote(map_path)), 1)
    r = subprocess.run(cmd, shell=True, cwd=build, capture_output=True, text=True, errors="replace")
    members = set()
    try:
        with open(map_path, errors="replace") as f:
            for line in f:
                if line.startswith(("Discarded input sections", "Memory Configuration")):
                    break
                a = ARCHIVE_MEMBER.match(line.split()[0]) if line.strip() else None
                if a:
                    members.add((os.path.normpath(os.path.join(build, a.group(1))), a.group(2)))
    except OSError:
        pass
    for path in (os.path.join(build, scratch), map_path):
        try:
            os.unlink(path)
        except OSError:
            pass
    removed, loaded = set(), set()
    for line in (r.stdout + "\n" + r.stderr).splitlines():
        m = REMOVING.search(line) or REMOVING_LLD.search(line)
        if m:
            section, where = (m.group(1), m.group(2)) if m.re is REMOVING else (m.group(2), m.group(1))
            removed.add((object_of(build, where), section.split("[", 1)[0]))
            continue
        line = line.strip()
        if line.endswith(".o") and not ARCHIVE_MEMBER.match(line):
            loaded.add(os.path.normpath(os.path.join(build, line)))
    # The map's first part names every archive member the link pulled in.
    for archive, member in members:
        loaded.add(object_of(build, f"{archive}({member})"))
    return exe, r.returncode, removed, loaded


_objects_cache = {}


def object_of(build, where):
    """An object file's path from the linker's name for it: a path, or "lib<target>.a(member.o)" for an archive member
    (CMake keeps the members under <archive's folder>/CMakeFiles/<target>.dir/)."""
    if where in _objects_cache:
        return _objects_cache[where]
    _objects_cache[where] = found = _object_of(build, where)
    return found


def _object_of(build, where):
    a = ARCHIVE_MEMBER.match(where)
    if not a:
        return os.path.normpath(os.path.join(build, where))
    archive = os.path.normpath(os.path.join(build, a.group(1)))
    target = re.sub(r"^lib|\.a$", "", os.path.basename(archive))
    hits = glob.glob(os.path.join(os.path.dirname(archive), "CMakeFiles", target + ".dir", "**", a.group(2)), recursive=True)
    return os.path.normpath(hits[0]) if len(hits) == 1 else f"{archive}({a.group(2)})"


def object_functions(obj):
    """{section: (symbol, bind)} of the FUNC symbols an object defines, each in its own section."""
    r = run(["readelf", "-SsW", obj])
    sections = {}
    funcs = {}
    in_sections = in_symbols = False
    for line in r.stdout.splitlines():
        if line.startswith("Section Headers:"):
            in_sections, in_symbols = True, False
            continue
        if line.startswith("Symbol table"):
            in_sections, in_symbols = False, True
            continue
        if in_sections:
            m = re.match(r"\s*\[\s*(\d+)\]\s+(\S+)", line)
            if m:
                sections[m.group(1)] = m.group(2)
        elif in_symbols:
            parts = line.split()
            if len(parts) >= 8 and parts[3] == "FUNC" and parts[6].isdigit():
                section = sections.get(parts[6], "")
                if section.startswith(".text"):
                    funcs[section] = (parts[7], parts[4])
    return funcs


def strip_templates(text):
    out, depth = [], 0
    for ch in text:
        if ch == "<":
            depth += 1
        elif ch == ">":
            depth = max(0, depth - 1)
        elif depth == 0:
            out.append(ch)
    return "".join(out)


def is_ours(demangled):
    """Whether a demangled function is one of ours: in the app:: or nekophoto:: namespaces, or a plain C name."""
    name = strip_templates(demangled.replace("operator<", "operator").replace("operator>", "operator"))
    name = name.split("(", 1)[0].strip().split()[-1] if name.split("(", 1)[0].strip() else name
    first = name.split("::", 1)[0]
    if first in ("app", "nekophoto"):
        return True
    return "::" not in name and not re.match(r"^(q|Q|_|operator)", name)


def gc_report(build):
    if not os.path.exists(os.path.join(build, "build.ninja")):
        return None, f"no gc build at {build} (run with --prepare)"
    db = json.load(open(os.path.join(build, "compile_commands.json")))
    objects = {}
    for e in db:
        src = own_source(e["file"] if os.path.isabs(e["file"]) else os.path.join(e["directory"], e["file"]))
        if src and "_autogen" not in e["file"]:
            obj = os.path.normpath(os.path.join(e["directory"], e["output"]))
            if os.path.exists(obj):
                objects[obj] = src
    links = link_commands(build)
    if not links:
        return None, f"{build} links nothing with --print-gc-sections (configured without the gc flags?)"
    with ThreadPoolExecutor(JOBS) as pool:
        results = list(pool.map(lambda l: relink(build, l), links))
    failed = [exe for exe, code, _, _ in results if code != 0]
    # Per object: which executables loaded it, and which of its sections each dropped.
    loaded_by = defaultdict(set)
    removed_in = defaultdict(lambda: defaultdict(set))
    for exe, code, removed, loaded in results:
        for obj in loaded:
            loaded_by[obj].add(exe)
        for obj, section in removed:
            removed_in[obj][exe].add(section)
    app = next((exe for exe, _, _, _ in results if exe.endswith("src/app/nekophoto")), None)
    candidates = []
    with ThreadPoolExecutor(JOBS) as pool:
        tables = dict(zip(objects, pool.map(object_functions, objects)))
    seen = set()
    for obj, src in objects.items():
        for section, (symbol, bind) in tables[obj].items():
            if (src, symbol) in seen:
                continue
            seen.add((src, symbol))
            if bind == "WEAK" or symbol in ("main",) or symbol.startswith("_GLOBAL__"):
                continue
            kept = {exe for exe in loaded_by.get(obj, ()) if section not in removed_in[obj].get(exe, ())}
            if app in kept:
                continue
            candidates.append({"file": src, "symbol": symbol, "kept_by": sorted(os.path.basename(k) for k in kept), "local": bind == "LOCAL"})
    names = demangle(sorted({c["symbol"] for c in candidates}))
    for c in candidates:
        c["function"] = names.get(c["symbol"], c["symbol"])
        c["status"] = "test-only" if c["kept_by"] else "unreferenced"
    # Copies of library templates and inline helpers (Qt's, the standard library's) that a file instantiated and did
    # not use are not ours to remove: only functions named in our namespaces, or plain C names, stay.
    library = len(candidates)
    candidates = [c for c in candidates if is_ours(c["function"])]
    library -= len(candidates)
    candidates.sort(key=lambda c: (c["status"] != "unreferenced", c["file"], c["function"]))
    note = f"{len(results)} executables relinked; {library} unused copies of library templates and inline helpers left out" + (f"; {len(failed)} failed to link: {', '.join(failed[:5])}" if failed else "")
    return {"executables": len(results), "app": app, "functions": candidates}, note


# ---- 2. never executed (coverage) ----------------------------------------------------------------------------------

def gcov_json(objdir, gcdas):
    r = run(["gcov", "--json-format", "--stdout", "--object-directory", objdir] + gcdas, cwd=objdir)
    docs = []
    decoder = json.JSONDecoder()
    text = r.stdout
    i = 0
    while i < len(text):
        while i < len(text) and text[i].isspace():
            i += 1
        if i >= len(text):
            break
        try:
            doc, i = decoder.raw_decode(text, i)
        except ValueError:
            break
        docs.append(doc)
    return docs


def coverage_report(build):
    gcdas = glob.glob(os.path.join(build, "**", "*.gcda"), recursive=True)
    if not gcdas:
        return None, f"no coverage counts in {build} (run with --prepare)"
    by_dir = defaultdict(list)
    for g in gcdas:
        by_dir[os.path.dirname(g)].append(g)
    batches = []
    for d, files in by_dir.items():
        for k in range(0, len(files), 20):
            batches.append((d, files[k:k + 20]))
    with ThreadPoolExecutor(JOBS) as pool:
        results = list(pool.map(lambda b: gcov_json(*b), batches))
    lines = defaultdict(dict)        # file -> line -> count
    functions = {}                   # (file, start) -> record
    for docs in results:
        for doc in docs:
            cwd = doc.get("current_working_directory", build)
            for f in doc.get("files", []):
                path = f["file"] if os.path.isabs(f["file"]) else os.path.join(cwd, f["file"])
                src = own_source(path)
                if not src:
                    continue
                for ln in f.get("lines", []):
                    n = ln["line_number"]
                    lines[src][n] = lines[src].get(n, 0) + ln["count"]
                for fn in f.get("functions", []):
                    key = (src, fn["start_line"])
                    rec = functions.setdefault(key, {"file": src, "line": fn["start_line"], "end": fn["end_line"],
                                                     "function": fn.get("demangled_name") or fn["name"], "count": 0})
                    rec["count"] += fn["execution_count"]
    files = []
    for src, counts in lines.items():
        total = len(counts)
        hit = sum(1 for c in counts.values() if c)
        files.append({"file": src, "lines": total, "covered": hit, "percent": round(100.0 * hit / total, 1) if total else 100.0})
    files.sort(key=lambda r: -(r["lines"] - r["covered"]))
    never = [dict(r, lines=r["end"] - r["line"] + 1) for r in functions.values() if r["count"] == 0]
    never.sort(key=lambda r: -r["lines"])
    total = sum(r["lines"] for r in files)
    hit = sum(r["covered"] for r in files)
    summary = {"lines": total, "covered": hit, "percent": round(100.0 * hit / total, 1) if total else 0.0,
               "functions": len(functions), "functions_never_run": len(never)}
    return {"summary": summary, "files": files, "never_executed": never}, f"{len(gcdas)} .gcda files"


# ---- 3. QSettings keys ---------------------------------------------------------------------------------------------

STRING_ARG = r'(?:QStringLiteral\(|QLatin1String\(|QString\(|QLatin1StringView\()?\s*"((?:[^"\\]|\\.)*)"'
CONSTANT = re.compile(r'\b(\w+)\s*(?:\[\])?\s*(?:=|\{)\s*' + STRING_ARG)


def settings_report():
    """Self-tests (SelfTest*.cpp) are left out: they write keys to check the code that reads them."""
    reads, writes = defaultdict(set), defaultdict(set)
    dynamic = []
    sources = {}
    files = sorted(glob.glob(os.path.join(ROOT, "src", "app", "*.cpp")) + glob.glob(os.path.join(ROOT, "src", "app", "*.h")))
    files = [f for f in files if not os.path.basename(f).startswith("SelfTest")]
    for path in files:
        rel = os.path.relpath(path, ROOT)
        text = open(path, errors="replace").read()
        sources[rel] = text
        constants = {m.group(1): m.group(2) for m in CONSTANT.finditer(text)}
        names = set(re.findall(r"\bQSettings\s*&?\s+(\w+)\s*[;({,)]", text)) | set(re.findall(r"\bQSettings\s*&\s*(\w+)", text))
        names.discard("QSettings")
        subject = r"(?:QSettings\s*\(\s*\)" + ("|\\b(?:" + "|".join(map(re.escape, sorted(names))) + r")\b" if names else "") + r")\s*(?:\.|->)\s*"
        access = re.compile(subject + r"(value|setValue|contains|remove|beginGroup|endGroup)\s*\(\s*([^,()]*(?:\([^()]*\))?[^,()]*)")
        group = []
        for m in access.finditer(text):
            op, arg = m.group(1), m.group(2).strip()
            line = text.count("\n", 0, m.start()) + 1
            where = f"{rel}:{line}"
            lit = re.match(r"^" + STRING_ARG + r"\s*\)?\s*$", arg)
            if op == "endGroup":
                if group:
                    group.pop()
                continue
            key = None
            if lit:
                key = lit.group(1)
            elif re.match(r"^\w+$", arg) and arg in constants:
                key = constants[arg]
            else:
                prefix = re.match(r"^" + STRING_ARG, arg)
                if prefix:
                    key = prefix.group(1).split("%")[0] + "*"
                else:
                    if op != "beginGroup":
                        dynamic.append({"where": where, "op": op, "expression": arg, "group": "/".join(group)})
                        if group:
                            # A key built at run time inside a group: every key under it may be meant.
                            (writes if op == "setValue" else reads)["/".join(group) + "/*"].add(where)
                    continue
            if "%" in key:
                key = key.split("%")[0] + "*"
            if op == "beginGroup":
                group.append(key)
                continue
            full = "/".join(group + [key])
            if op == "remove":
                # Removing a key (or a whole group) clears what is read elsewhere; it is neither a read nor a write to report.
                continue
            (writes if op == "setValue" else reads)[full].add(where)

    def matches(key, others):
        if key in others:
            return True
        if key.endswith("*") and any(o.startswith(key[:-1]) for o in others):
            return True
        return any(o.endswith("*") and key.startswith(o[:-1]) for o in others)

    def evidence(key, places):
        """Why a one-sided key may still be fine: its name as a string somewhere else, or keys built at run time in the
        same file."""
        accesses = {p for p in places}
        other = []
        for rel, text in sources.items():
            for m in re.finditer(re.escape('"' + key + '"'), text):
                where = f"{rel}:{text.count(chr(10), 0, m.start()) + 1}"
                if where not in accesses:
                    other.append(where)
        files_ = {p.split(":")[0] for p in places}
        built = [d["where"] for d in dynamic if d["where"].split(":")[0] in files_]
        if other:
            return "low", f"the key also appears as a string at {', '.join(other[:3])}"
        if built:
            return "low", f"the same file uses keys built at run time ({', '.join(built[:3])})"
        return "high", ""

    def rows(side, opposite):
        out = []
        for k, w in sorted(side.items()):
            if matches(k, opposite) or k.endswith("/*"):
                continue
            confidence, why = evidence(k, w)
            out.append({"key": k, "where": sorted(w), "confidence": confidence, "evidence": why})
        return out

    return {"keys": len(set(reads) | set(writes)),
            "written_never_read": rows(writes, reads),
            "read_never_written": rows(reads, writes),
            "unresolved": dynamic}, "keys from literals, same-file constants, groups and literal prefixes of keys built at run time; self-tests left out"


# ---- 4. resources --------------------------------------------------------------------------------------------------

def resource_files():
    """[(resource path, source file)] from the .qrc files and qt_add_resources lists."""
    out = []
    for qrc in glob.glob(os.path.join(ROOT, "src", "**", "*.qrc"), recursive=True):
        tree = ET.parse(qrc)
        for res in tree.getroot().iter("qresource"):
            prefix = res.get("prefix", "/").rstrip("/")
            for f in res.iter("file"):
                alias = f.get("alias") or f.text
                out.append((f"{prefix}/{alias}", os.path.relpath(os.path.join(os.path.dirname(qrc), f.text), ROOT)))
    cmake = os.path.join(ROOT, "src", "app", "CMakeLists.txt")
    text = open(cmake).read()
    for m in re.finditer(r"qt_add_resources\(\s*\w+\s+\"(\w+)\"(.*?)\)", text, re.S):
        body = m.group(2)
        prefix = (re.search(r'PREFIX\s+"([^"]*)"', body) or [None, "/"])[1].rstrip("/")
        base = (re.search(r"BASE\s+(\S+)", body) or [None, ""])[1]
        files = re.search(r"FILES\s+(.*)", body, re.S)
        if not files:
            continue
        for item in files.group(1).split():
            if item.startswith("${"):
                continue   # a globbed list (the brush presets): listed by folder at run time, checked below
            rel = os.path.relpath(item, base) if base and not base.startswith("$") else item
            out.append((f"{prefix}/{rel}", os.path.join("src", "app", item)))
    return out


def resources_report():
    texts = []
    for pattern in ("src/app/**/*.cpp", "src/app/**/*.h", "src/app/**/*.qss", "src/app/**/*.ui", "src/core/**/*.cpp"):
        for p in glob.glob(os.path.join(ROOT, pattern), recursive=True):
            texts.append(open(p, errors="replace").read())
    corpus = "\n".join(texts)
    literals = set(re.findall(r'"([^"\n]*)"', corpus))
    folders = {lit for lit in literals if lit.startswith(":/") and lit.endswith("/")}
    templated = [lit for lit in literals if lit.startswith(":/") and "%" in lit]
    unused, weak = [], []
    files = resource_files()
    for res, src in files:
        path = ":" + res
        stem = os.path.splitext(os.path.basename(res))[0]
        if path in corpus or ("qrc" + path) in corpus or any(res.startswith(f[1:]) for f in folders):
            continue
        if any(re.fullmatch(re.escape(t).replace("%1", ".+").replace(r"\%1", ".+"), path) for t in templated) and stem in literals:
            continue   # a path built from a template (":/icons/%1.svg") and the name as a string elsewhere
        if stem in literals:
            weak.append({"resource": path, "file": src, "evidence": f'"{stem}" appears as a string'})
            continue
        unused.append({"resource": path, "file": src})
    return {"resources": len(files), "unused": unused, "named_only": weak}, \
        "a resource counts as used when its path, a folder it is listed from, or its name as a string appears in the code"


# ---- 5. translations -----------------------------------------------------------------------------------------------

def translations_report():
    sys.path.insert(0, os.path.join(ROOT, "tools"))
    import check_translations
    lupdate = check_translations.find_lupdate(None)
    if not lupdate:
        return None, "no lupdate (Qt Linguist tools) found"
    out = []
    for ts in sorted(glob.glob(os.path.join(ROOT, "translations", "*.ts"))):
        committed = ET.parse(ts)
        already = sum(1 for t in committed.getroot().iter("translation") if t.get("type") in ("vanished", "obsolete"))
        with tempfile.TemporaryDirectory() as tmp:
            copy = os.path.join(tmp, os.path.basename(ts))
            shutil.copy(ts, copy)
            r = run([lupdate, "-silent", "-locations", "none", os.path.join(ROOT, "src", "app"), "-ts", copy])
            if r.returncode != 0:
                return None, "lupdate failed: " + r.stderr[-500:]
            tree = ET.parse(copy)
        gone, total = [], 0
        for context in tree.getroot().iter("context"):
            for message in context.iter("message"):
                total += 1
                t = message.find("translation")
                if t is not None and t.get("type") in ("vanished", "obsolete"):
                    gone.append({"context": context.findtext("name"), "source": message.findtext("source")})
        out.append({"file": os.path.relpath(ts, ROOT), "messages": total, "already_obsolete_in_file": already, "no_longer_in_code": gone})
    return out, ""


# ---- report --------------------------------------------------------------------------------------------------------

def markdown(r, top):
    L = ["# Dead-code candidates", ""]
    gc = r.get("gc")
    L.append("## Functions nothing references (gc-sections)")
    L.append("")
    if gc:
        unref = [c for c in gc["functions"] if c["status"] == "unreferenced"]
        test_only = [c for c in gc["functions"] if c["status"] == "test-only"]
        cov = {(n["file"], n["function"]) for n in (r.get("coverage") or {}).get("never_executed", [])}
        L.append(f"{len(unref)} functions referenced by no executable, {len(test_only)} referenced only by tests ({gc['executables']} executables). "
                 "Confidence: high when coverage also never ran it.")
        L.append("")
        L += ["| Function | file | status | also never executed |", "|---|---|---|---|"]
        for c in (unref + test_only)[:top * 4]:
            never = "yes" if any(f == c["file"] and (fn == c["function"] or fn.split("(")[0] == c["function"].split("(")[0]) for f, fn in cov) else ""
            kept = c["status"] + (f" ({', '.join(c['kept_by'][:3])}{'…' if len(c['kept_by']) > 3 else ''})" if c["kept_by"] else "")
            L.append(f"| `{c['function']}` | {c['file']} | {kept} | {never} |")
    L.append("")
    if r["notes"].get("gc"):
        L += [f"note: {r['notes']['gc']}", ""]
    cov = r.get("coverage")
    L.append("## Coverage (ctest + rpc_smoke)")
    L.append("")
    if cov:
        s = cov["summary"]
        L.append(f"{s['covered']:,} of {s['lines']:,} lines executed ({s['percent']}%); {s['functions_never_run']:,} of {s['functions']:,} functions never ran.")
        L += ["", f"### Files with the most lines never executed (top {top})", "", "| File | lines | executed | % |", "|---|---:|---:|---:|"]
        for f in cov["files"][:top]:
            L.append(f"| {f['file']} | {f['lines']:,} | {f['covered']:,} | {f['percent']} |")
        L += ["", f"### Largest functions never executed (top {top * 2})", "", "| Function | where | lines |", "|---|---|---:|"]
        for f in cov["never_executed"][:top * 2]:
            L.append(f"| `{f['function'][:110]}` | {f['file']}:{f['line']} | {f['lines']} |")
    L.append("")
    if r["notes"].get("coverage"):
        L += [f"note: {r['notes']['coverage']}", ""]
    st = r.get("settings")
    L.append("## QSettings keys")
    L.append("")
    if st:
        L.append(f"{st['keys']} keys. Written and never read: {len(st['written_never_read'])}; read and never written (a default only): "
                 f"{len(st['read_never_written'])}; accesses with a key the scan cannot resolve: {len(st['unresolved'])}.")
        for title, rows in (("Written, never read", st["written_never_read"]), ("Read, never written", st["read_never_written"])):
            L += ["", f"### {title}", ""]
            for row in rows:
                L.append(f"- `{row['key']}` ({row['confidence']} confidence): {', '.join(row['where'][:3])}" + (f"; {row['evidence']}" if row["evidence"] else ""))
        if st["unresolved"]:
            L += ["", "### Keys built at run time (not resolved)", ""]
            for row in st["unresolved"]:
                L.append(f"- {row['where']}: {row['op']}({row['expression']})")
    L.append("")
    res = r.get("resources")
    L.append("## Resources")
    L.append("")
    if res:
        L.append(f"{res['resources']} resources; {len(res['unused'])} never mentioned; {len(res['named_only'])} mentioned only by name.")
        L.append("")
        for row in res["unused"]:
            L.append(f"- unused: `{row['resource']}` ({row['file']})")
        for row in res["named_only"]:
            L.append(f"- by name only: `{row['resource']}`: {row['evidence']}")
    L.append("")
    tr = r.get("translations")
    L.append("## Translations")
    L.append("")
    if tr:
        for t in tr:
            L.append(f"- {t['file']}: {t['messages']} messages, {len(t['no_longer_in_code'])} no longer in the code, "
                     f"{t['already_obsolete_in_file']} kept as obsolete in the file")
            for g in t["no_longer_in_code"][:top]:
                L.append(f"  - {g['context']}: {g['source']!r}")
    for part in ("settings", "resources", "translations"):
        if r["notes"].get(part):
            L += ["", f"note ({part}): {r['notes'][part]}"]
    return "\n".join(L) + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--prepare", action="store_true", help="configure and build build-gc and build-cov, run the tests for coverage (heavy)")
    parser.add_argument("--gc-build", default=os.path.join(ROOT, "build-gc"))
    parser.add_argument("--coverage-build", default=os.path.join(ROOT, "build-cov"))
    parser.add_argument("--skip", default="", help="comma-separated parts to leave out: gc, coverage, settings, resources, translations")
    parser.add_argument("--top", type=int, default=30)
    parser.add_argument("--json")
    parser.add_argument("--markdown")
    args = parser.parse_args()
    skip = set(filter(None, args.skip.split(",")))
    if args.prepare:
        prepare(os.path.abspath(args.gc_build), os.path.abspath(args.coverage_build), skip)
    r = {"notes": {}}
    parts = [("gc", lambda: gc_report(os.path.abspath(args.gc_build))), ("coverage", lambda: coverage_report(os.path.abspath(args.coverage_build))),
             ("settings", settings_report), ("resources", resources_report), ("translations", translations_report)]
    for name, fn in parts:
        if name in skip:
            r[name], r["notes"][name] = None, "skipped"
            continue
        print(f"{name}...", file=sys.stderr, flush=True)
        r[name], r["notes"][name] = fn()
    text = markdown(r, args.top)
    print(text)
    if args.json:
        with open(args.json, "w") as f:
            json.dump(r, f, indent=1)
            f.write("\n")
    if args.markdown:
        with open(args.markdown, "w") as f:
            f.write(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
