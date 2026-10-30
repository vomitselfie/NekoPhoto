#!/usr/bin/env python3
"""The shape of the code: the biggest files, the longest functions, and the headers most translation units include.
An informational report (CI prints it and never fails on it), to steer refactoring rather than police it.

    tools/code_shape.py [--build build] [--top 25] [--json out.json] [--markdown out.md]

- Files: lines (and lines of code: neither blank nor only a comment) of our own sources under src/ and tests/
  (src/third_party is left out), from `git ls-files`.
- Functions: every function body at namespace or class scope, found by matching braces after the comments, strings
  and preprocessor lines are blanked out; a body's length runs from the line its signature starts on to its closing
  brace (lambdas and local classes count towards the function they are in). A heuristic, not a parser: a macro that
  expands to a signature or an unbalanced brace in a #if branch can fool it, which the totals would show.
- Header fan-in: per header of ours, how many translation units include it directly (an #include line in the unit
  itself, resolved against that unit's include paths from compile_commands.json) and transitively (anywhere in its
  include tree, from the dependencies the compiler recorded for the build: `ninja -t deps`). The transitive count is
  how many units a change to the header rebuilds. Needs a configured and built build folder; without one the fan-in
  part is skipped with a note.
"""
import argparse
import json
import os
import re
import shlex
import subprocess
import sys
from collections import defaultdict

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SOURCE_EXT = (".cpp", ".cc", ".c", ".h", ".hpp", ".inc")
HEADER_EXT = (".h", ".hpp", ".inc")


def own_files():
    out = subprocess.run(["git", "-C", ROOT, "ls-files", "src", "tests"], capture_output=True, text=True, check=True).stdout.split()
    return sorted(f for f in out if f.endswith(SOURCE_EXT) and not f.startswith("src/third_party/") and not f.startswith("tests/fuzz/corpus"))


# ---- blanking comments, literals and the preprocessor -------------------------------------------------------------

RAW_STRING = re.compile(r'(?:u8|u|U|L)?R"([^()\\ ]{0,16})\(')


def blank(text):
    """The text with comments, string and character literals and preprocessor lines replaced by spaces (newlines kept),
    so braces and parentheses left are code's. Returns (blanked, comment_only_lines)."""
    out = []
    i, n = 0, len(text)
    comment_lines = set()
    line = 1
    at_line_start = True
    while i < n:
        c = text[i]
        if c == "\n":
            out.append(c)
            line += 1
            i += 1
            at_line_start = True
            continue
        if at_line_start and c in " \t":
            out.append(c)
            i += 1
            continue
        if at_line_start and c == "#":
            # A preprocessor line, with its continuations.
            while i < n and text[i] != "\n":
                if text[i] == "\\" and i + 1 < n and text[i + 1] == "\n":
                    out.append(" \n")
                    line += 1
                    i += 2
                    continue
                out.append(" ")
                i += 1
            continue
        at_line_start = False
        if text.startswith("//", i):
            comment_lines.add(line)
            while i < n and text[i] != "\n":
                out.append(" ")
                i += 1
            continue
        if text.startswith("/*", i):
            end = text.find("*/", i + 2)
            end = n if end < 0 else end + 2
            for ch in text[i:end]:
                if ch == "\n":
                    out.append("\n")
                    line += 1
                else:
                    out.append(" ")
                comment_lines.add(line)
            i = end
            continue
        m = RAW_STRING.match(text, i) if c in 'uULR' else None
        if m and (i == 0 or not (text[i - 1].isalnum() or text[i - 1] == "_")):
            close = ")" + m.group(1) + '"'
            end = text.find(close, m.end())
            end = n if end < 0 else end + len(close)
            for ch in text[i:end]:
                out.append("\n" if ch == "\n" else " ")
                if ch == "\n":
                    line += 1
            out[-1:] = ['"'] if out and out[-1] != "\n" else out[-1:]
            i = end
            continue
        if c == '"' or (c == "'" and not (i > 0 and (text[i - 1].isalnum()))):   # 1'000 digit separators stay
            quote = c
            out.append(quote)
            i += 1
            while i < n and text[i] != quote and text[i] != "\n":
                if text[i] == "\\" and i + 1 < n:
                    out.append("  ")
                    i += 2
                    continue
                out.append(" ")
                i += 1
            if i < n and text[i] == quote:
                out.append(quote)
                i += 1
            continue
        out.append(c)
        i += 1
    return "".join(out), comment_lines


# ---- functions ----------------------------------------------------------------------------------------------------

KEYWORDS_NOT_FUNCTION = {"if", "for", "while", "switch", "catch", "return", "sizeof", "alignof", "decltype", "static_assert",
                         "noexcept", "requires", "alignas", "defined", "throw", "new", "delete"}
TRAILING_QUALIFIERS = re.compile(r"^\s*(const|volatile|noexcept\s*(\([^()]*\))?|override|final|mutable|&&|&|->\s*[\w:<>,\s\*&]+|try|"
                                 r"requires\b.*)*\s*$", re.S)
IDENT_BEFORE = re.compile(r"((?:[A-Za-z_]\w*\s*(?:<[^<>;{}()]*>)?\s*::\s*)*~?[A-Za-z_]\w*|operator\s*(?:\(\)|[^\s(]+))\s*$")


def matching_paren_start(head, close):
    """Index of the '(' matching the ')' at `close`."""
    depth = 0
    for j in range(close, -1, -1):
        if head[j] == ")":
            depth += 1
        elif head[j] == "(":
            depth -= 1
            if depth == 0:
                return j
    return -1


def function_name(head):
    """The function's name when `head` (the code between the previous ; { or } and this {) is a function signature,
    else None."""
    h = head.rstrip()
    if not h:
        return None
    # A lambda (at namespace scope: auto f = [](...) {): named after its variable.
    # A constructor's initializer list: cut it at the ':' after the parameter list's ')'.
    parts = None
    depth = 0
    for j, ch in enumerate(h):
        if ch in "([<":
            depth += 1 if ch != "<" else 0
        if ch == "(":
            pass
        if ch in ")]":
            depth -= 1
        if ch == ":" and depth == 0 and j + 1 < len(h) and h[j + 1] != ":" and (j == 0 or h[j - 1] != ":"):
            before = h[:j].rstrip()
            if before.endswith(")") or re.search(r"\)\s*(noexcept|const)?\s*$", before):
                parts = before
                break
    if parts is not None:
        h = parts
    # The parameter list: the last ')' after which only qualifiers follow.
    close = h.rfind(")")
    while close >= 0 and not TRAILING_QUALIFIERS.match(h[close + 1:]):
        close = h.rfind(")", 0, close)
    if close < 0:
        return None
    open_ = matching_paren_start(h, close)
    if open_ <= 0:
        return None
    before = h[:open_]
    if before.rstrip().endswith("]"):
        lam = re.search(r"([A-Za-z_]\w*)\s*=\s*\[[^\]]*\]\s*$", before)
        return (lam.group(1) + " (lambda)") if lam else None
    m = IDENT_BEFORE.search(before)
    if not m:
        return None
    name = re.sub(r"\s+", "", m.group(1))
    last = name.split("::")[-1]
    if last in KEYWORDS_NOT_FUNCTION:
        return None
    rest = before[:m.start()].rstrip()
    if rest.endswith(("=", "return", ",", "(", "[")) or rest.endswith("."):
        return None   # an initializer or a call, not a definition
    if re.match(r"^[A-Z_][A-Z0-9_]*$", last) and not rest:
        return name + "(...)"   # a macro that opens a body (TEST(...) {)
    return name


def functions_in(path, text):
    """[(name, start_line, end_line)] of function bodies at namespace or class scope."""
    code, _ = blank(text)
    found = []
    # A stack of scopes: 'ns' (namespace, extern "C", class) lets functions appear inside; 'body' and 'other' do not.
    stack = []
    head_start = 0
    i, n = 0, len(code)
    line_of = [0] * (n + 1)
    line = 1
    for k, ch in enumerate(code):
        line_of[k] = line
        if ch == "\n":
            line += 1
    line_of[n] = line
    while i < n:
        ch = code[i]
        if ch == "{":
            head = code[head_start:i]
            inside_body = any(s[0] in ("body", "other") for s in stack)
            if inside_body:
                stack.append(("other", i, None))
            else:
                flat = " ".join(head.split())
                if re.search(r"(^|\s)(namespace|extern\s+\"\s*\")(\s+[\w:]+)?\s*$", flat) or re.search(r"(^|\s)namespace\s*$", flat) or flat.endswith('extern "C"'):
                    stack.append(("ns", i, None))
                elif re.search(r"(^|[\s>])(class|struct|union)\s+[\w:]+(\s+final)?(\s*:[^{]*)?$", flat) or re.search(r"(^|\s)(class|struct|union)\s*$", flat):
                    stack.append(("ns", i, None))
                elif re.search(r"(^|\s)enum(\s+(class|struct))?(\s+\w+)?(\s*:\s*[\w:]+)?\s*$", flat):
                    stack.append(("other", i, None))
                else:
                    name = function_name(flat)
                    if name:
                        # The signature's first line: the head's first non-blank character.
                        offset = len(head) - len(head.lstrip())
                        stack.append(("body", i, (name, line_of[head_start + offset])))
                    else:
                        stack.append(("other", i, None))
            head_start = i + 1
        elif ch == "}":
            if stack:
                kind, _, info = stack.pop()
                if kind == "body" and info:
                    found.append((info[0], info[1], line_of[i]))
            head_start = i + 1
        elif ch == ";":
            if not any(s[0] in ("body", "other") for s in stack):
                head_start = i + 1
        elif ch == ":" and not any(s[0] in ("body", "other") for s in stack):
            # Access specifiers end a head (public: void f() {...}).
            prev = code[head_start:i].strip()
            if prev in ("public", "private", "protected", "signals", "Q_SIGNALS", "public slots", "private slots", "protected slots", "public Q_SLOTS", "private Q_SLOTS"):
                head_start = i + 1
        i += 1
    return found


# ---- header fan-in ------------------------------------------------------------------------------------------------

INCLUDE = re.compile(r'^\s*#\s*include\s*([<"])([^>"]+)[>"]', re.M)


def include_paths(command):
    args = shlex.split(command)
    paths = []
    for k, a in enumerate(args):
        if a in ("-I", "-isystem", "-iquote") and k + 1 < len(args):
            paths.append(args[k + 1])
        elif a.startswith("-I") and len(a) > 2:
            paths.append(a[2:])
        elif a.startswith("-isystem") and len(a) > 8:
            paths.append(a[8:])
    return paths


def resolve(name, quoted, here, paths):
    candidates = ([here] if quoted else []) + paths
    for base in candidates:
        p = os.path.normpath(os.path.join(base, name))
        if os.path.isfile(p):
            return p
    return None


def fan_in(build, own_headers):
    """{header: {"direct": n, "transitive": n}} over the build's translation units, plus a note."""
    db_path = os.path.join(build, "compile_commands.json")
    if not os.path.exists(db_path):
        return None, f"no {db_path}: configure with -DCMAKE_EXPORT_COMPILE_COMMANDS=ON (the default here) and build"
    db = json.load(open(db_path))
    own = {os.path.realpath(os.path.join(ROOT, h)): h for h in own_headers}
    direct = defaultdict(set)
    units = set()
    for entry in db:
        src = os.path.realpath(entry["file"] if os.path.isabs(entry["file"]) else os.path.join(entry["directory"], entry["file"]))
        units.add(src)
        try:
            text = open(src, errors="replace").read()
        except OSError:
            continue
        paths = [p if os.path.isabs(p) else os.path.join(entry["directory"], p) for p in include_paths(entry.get("command") or " ".join(entry.get("arguments", [])))]
        for quote, name in INCLUDE.findall(text):
            hit = resolve(name, quote == '"', os.path.dirname(src), paths)
            if hit and os.path.realpath(hit) in own:
                direct[own[os.path.realpath(hit)]].add(src)
    transitive = defaultdict(set)
    note = ""
    try:
        deps = subprocess.run(["ninja", "-C", build, "-t", "deps"], capture_output=True, text=True, check=True).stdout
        target = None
        for line in deps.splitlines():
            if not line.startswith(" ") and line.strip():
                target = line.split(":")[0]
                if not target.endswith(".o"):
                    target = None
                continue
            if target and line.strip():
                p = os.path.realpath(line.strip())
                if p in own:
                    transitive[own[p]].add(target)
        if not transitive:
            note = "ninja -t deps listed no dependencies of ours (has the build been built?)"
    except (OSError, subprocess.CalledProcessError) as e:
        note = f"ninja -t deps failed ({e}): transitive counts are missing"
    result = {}
    for h in own_headers:
        if h.endswith(HEADER_EXT):
            result[h] = {"direct": len(direct.get(h, ())), "transitive": len(transitive.get(h, ()))}
    return {"units": len(db), "headers": result}, note


# ---- report -------------------------------------------------------------------------------------------------------

def analyse(build, top):
    files = own_files()
    file_rows, func_rows = [], []
    for f in files:
        text = open(os.path.join(ROOT, f), errors="replace").read()
        lines = text.count("\n") + (0 if text.endswith("\n") or not text else 1)
        code, comment_lines = blank(text)
        code_lines = sum(1 for ln in code.split("\n") if ln.strip())
        file_rows.append({"file": f, "lines": lines, "code": code_lines})
        for name, start, end in functions_in(f, text):
            func_rows.append({"file": f, "function": name, "line": start, "lines": end - start + 1})
    file_rows.sort(key=lambda r: -r["lines"])
    func_rows.sort(key=lambda r: -r["lines"])
    fan, note = fan_in(build, files)
    out = {
        "totals": {"files": len(file_rows), "lines": sum(r["lines"] for r in file_rows), "code": sum(r["code"] for r in file_rows),
                   "functions": len(func_rows),
                   "functions_over_100": sum(1 for r in func_rows if r["lines"] > 100), "functions_over_300": sum(1 for r in func_rows if r["lines"] > 300),
                   "files_over_1000": sum(1 for r in file_rows if r["lines"] > 1000)},
        "files": file_rows,
        "functions": func_rows[:max(top * 4, 100)],
        "fan_in": fan,
        "note": note,
    }
    return out


def markdown(r, top):
    t = r["totals"]
    lines = [f"Code shape: {t['files']} files, {t['lines']:,} lines ({t['code']:,} of code), {t['functions']:,} functions; "
             f"{t['files_over_1000']} files over 1,000 lines, {t['functions_over_100']} functions over 100 lines, {t['functions_over_300']} over 300.", ""]
    lines += [f"### Biggest files (top {top})", "", "| File | lines | code |", "|---|---:|---:|"]
    for row in r["files"][:top]:
        lines.append(f"| {row['file']} | {row['lines']:,} | {row['code']:,} |")
    lines += ["", f"### Longest functions (top {top})", "", "| Function | where | lines |", "|---|---|---:|"]
    for row in r["functions"][:top]:
        lines.append(f"| `{row['function']}` | {row['file']}:{row['line']} | {row['lines']:,} |")
    fan = r.get("fan_in")
    if fan:
        headers = fan["headers"]
        lines += ["", f"### Headers by transitive fan-in (top {top}; of {fan['units']} translation units)", "",
                  "| Header | units including it (transitive) | directly |", "|---|---:|---:|"]
        for h, v in sorted(headers.items(), key=lambda kv: (-kv[1]["transitive"], -kv[1]["direct"]))[:top]:
            lines.append(f"| {h} | {v['transitive']} | {v['direct']} |")
        lines += ["", f"### Headers by direct fan-in (top {top})", "", "| Header | units including it directly | transitive |", "|---|---:|---:|"]
        for h, v in sorted(headers.items(), key=lambda kv: (-kv[1]["direct"], -kv[1]["transitive"]))[:top]:
            lines.append(f"| {h} | {v['direct']} | {v['transitive']} |")
    if r.get("note"):
        lines += ["", f"note: {r['note']}"]
    return "\n".join(lines) + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--build", default=os.path.join(ROOT, "build"), help="a configured, built build folder, for the fan-in (default build)")
    parser.add_argument("--top", type=int, default=25, help="rows per list (default 25)")
    parser.add_argument("--json", help="write the full results here")
    parser.add_argument("--markdown", help="write the tables here")
    args = parser.parse_args()
    r = analyse(args.build, args.top)
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
