#!/usr/bin/env python3
"""Generates the "At a glance" tables of docs/compatibility.md from the repository itself: the render-hash scenes,
golden PNGs, brush-parity baseline rows, CTest suites, the capability matrix per mode and depth, automation methods
registered and named in tools/rpc_smoke.py, the methods tools/rpc_panic_hunt.py leaves out, the fuzz targets and the
merged-composite oracle's floor. Everything outside the generated blocks stays hand-written.

    python3 tools/compat_table.py            print the blocks
    python3 tools/compat_table.py --check    exit 1 when docs/compatibility.md is stale (a ctest test)
    python3 tools/compat_table.py --write    rewrite the blocks in docs/compatibility.md

COMPAT_TABLE_WRITE=1 with --check rewrites instead of failing.
"""
import ast
import glob
import os
import re
import sys

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
DOC = os.path.join(ROOT, "docs", "compatibility.md")


def read(rel):
    # Universal newlines: a Windows checkout's CRLF reads the same.
    with open(os.path.join(ROOT, rel), encoding="utf-8") as f:
        return f.read()


def lines(rel):
    return [l for l in read(rel).splitlines() if l.strip() and not l.startswith("#")]


def render_hashes():
    """Scenes per depth and mode, from the first path component of each name in tests/render_hashes.txt."""
    groups = {"8-bit RGB": 0, "16-bit RGB": 0, "32-bit RGB": 0, "8-bit CMYK": 0, "16-bit CMYK": 0, "8-bit Lab": 0, "16-bit Lab": 0}
    key = {"u16": "16-bit RGB", "f32": "32-bit RGB", "cmyk": "8-bit CMYK", "cmyk16": "16-bit CMYK", "lab": "8-bit Lab", "lab16": "16-bit Lab"}
    for l in lines("tests/render_hashes.txt"):
        groups[key.get(l.split("/", 1)[0], "8-bit RGB")] += 1
    return sum(groups.values()), groups


def golden():
    pngs = len(glob.glob(os.path.join(ROOT, "tests", "golden", "*.png")))
    cases = len(re.findall(r"^TEST_CASE\(", read("tests/golden_tests.cpp"), re.M))
    return cases, pngs


def brush_parity():
    rows = lines("tests/brush_parity_baseline.txt")
    fixtures = {r.split()[0].split("/", 1)[0] for r in rows}
    presets = {r.split()[0].split("/", 1)[1] for r in rows if "/" in r.split()[0]}
    return len(rows), len(fixtures), len(presets)


def ctest_suites():
    names = set()
    for top in ("CMakeLists.txt", "src", "tests"):
        for folder, dirs, files in os.walk(os.path.join(ROOT, top)) if top != "CMakeLists.txt" else [(ROOT, [], [top])]:
            dirs[:] = [d for d in dirs if d != "third_party"]
            if "CMakeLists.txt" in files:
                with open(os.path.join(folder, "CMakeLists.txt"), encoding="utf-8") as f:
                    names.update(re.findall(r"add_test\(\s*NAME\s+([A-Za-z0-9_]+)", f.read()))
    cases = 0
    for path in glob.glob(os.path.join(ROOT, "tests", "*.cpp")):
        with open(path, encoding="utf-8") as f:
            cases += len(re.findall(r"^TEST_CASE\(", f.read(), re.M))
    return len(names), cases


def mode_matrix():
    """Per column of docs/mode-matrix.md: native, native through RGB, greyed (Photoshop lacks), greyed (not yet)."""
    header, counts, features = None, {}, 0
    for l in read("docs/mode-matrix.md").splitlines():
        if not l.startswith("|"):
            continue
        cells = [c.strip() for c in re.split(r"(?<!\\)\|", l)[1:-1]]
        if cells and cells[0] == "Feature":
            header = cells
            counts = {c: [0, 0, 0, 0] for c in header[2:-1]}
            continue
        if header is None or set(cells[0]) <= set("-"):
            continue
        features += 1
        for name, cell in zip(header[2:-1], cells[2:-1]):
            i = {"native": 0, "native\\*": 1, "greyed (Photoshop lacks)": 2, "greyed (not yet)": 3}[cell]
            counts[name][i] += 1
    return features, counts


def automation():
    registered = set()
    for path in glob.glob(os.path.join(ROOT, "src", "app", "Automation*.cpp")):
        with open(path, encoding="utf-8") as f:
            registered.update(re.findall(r'\badd\("([A-Za-z0-9_.]+)"', f.read()))
    smoke = read("tools/rpc_smoke.py")
    named = {m for m in registered if re.search(r"""["']""" + re.escape(m) + r"""["']""", smoke)}
    return len(registered), len(named)


def panic_hunt_skips():
    """The SKIP set of tools/rpc_panic_hunt.py: methods it never sends to."""
    tree = ast.parse(read("tools/rpc_panic_hunt.py"))
    for node in tree.body:
        if isinstance(node, ast.Assign) and any(isinstance(t, ast.Name) and t.id == "SKIP" for t in node.targets):
            value = ast.literal_eval(node.value)
            return len(value)
    return 0


def fuzz_targets():
    text = read("tests/fuzz/CMakeLists.txt")
    count = 0
    for body in re.findall(r"foreach\(\s*\w+\s+([^)]*)\)", text):
        count += len(body.split())
    return count


def oracle():
    path = os.path.join(ROOT, "tests", "psd_oracle.txt")
    if not os.path.exists(path):
        return None
    floor = compared = None
    for l in lines("tests/psd_oracle.txt"):
        word, _, value = l.partition(" ")
        if word == "floor":
            floor = int(value)
        elif word == "compared":
            compared = int(value)
    return (floor, compared) if floor is not None else None


def tables():
    scenes, by_mode = render_hashes()
    gcases, gpngs = golden()
    brows, bfixtures, bpresets = brush_parity()
    suites, cases = ctest_suites()
    features, matrix = mode_matrix()
    registered, named = automation()
    skipped = panic_hunt_skips()
    fuzz = fuzz_targets()
    orc = oracle()
    modes = ", ".join(f"{n} at {k}" for k, n in by_mode.items() if n)

    en = ["| Check | Result | How to rerun |", "|---|---|---|"]
    if orc:
        en.append(f"| Photoshop's merged image as the oracle | **{orc[0]} of {orc[1]}** Patchy files with a merged image render within "
                  "2 levels of it on 99% of pixels, mean under 1 level (the floor in `tests/psd_oracle.txt`; it may only rise) "
                  "| `ctest -R psd_composite_oracle` with Patchy beside this checkout (or `PATCHY_FIXTURES`) |")
    en += [
        f"| Render hashes | **{scenes} scenes**: {modes}; each rendered on the worker pool and serially | `ctest -R render_hash_tests` |",
        f"| Golden images | **{gcases} golden test cases over {gpngs} reference PNGs** in `tests/golden/` | `ctest -R golden_tests` |",
        f"| Brush parity | **{brows} baseline rows**: {bpresets} presets over {bfixtures} stroke fixtures | `ctest -R brush_parity` |",
        f"| Test suites | **{suites} CTest tests** registered ({cases} `TEST_CASE`s); a few need optional dependencies | `ctest --test-dir build` |",
        f"| Capability matrix | **{features} features** in 7 modes and depths, generated from `supports()` (the table below) "
        "| `ctest -R mode_matrix_check` |",
        f"| Automation | **{registered} methods**, {named} of them called in `tools/rpc_smoke.py`; "
        + (f"every method but {skipped}" if skipped else "every method")
        + " sent hostile parameters by `tools/rpc_panic_hunt.py` | `python3 tools/rpc_smoke.py <socket>`, `python3 tools/rpc_panic_hunt.py` |",
        f"| Fuzz targets | **{fuzz} libFuzzer targets** (PSD and its block parsers, the smaller readers) | [fuzzing.md](fuzzing.md) |",
        "| Compiler warnings | none: CI builds with `-Werror` on GCC and Clang | `-DCOMPOSITOR_WARNINGS_AS_ERRORS=ON` |",
        "",
        "| Mode | native | native, partly through RGB | greyed: Photoshop lacks | greyed: not yet |",
        "|---|---:|---:|---:|---:|",
    ]
    for name, (n, through, lacks, notyet) in matrix.items():
        en.append(f"| {name} | {n} | {through} | {lacks} | {notyet} |")

    ja = []
    if orc:
        ja.append(f"- **Photoshop の統合画像との比較**: 統合画像を持つ Patchy のファイル {orc[1]} 個のうち **{orc[0]} 個**が、"
                  "99% のピクセルで 2 レベル以内・平均 1 レベル未満(下限は `tests/psd_oracle.txt`、下げることはできません)。")
    ja += [
        f"- **描画のハッシュ**: {scenes} シーン。**ゴールデン画像**: {gcases} テスト・参照 PNG {gpngs} 枚。"
        f"**ブラシの基準値**: {brows} 行。**テストスイート**: CTest {suites} 個({cases} `TEST_CASE`)。",
        f"- **自動操作**: メソッド {registered} 個、うち {named} 個を `tools/rpc_smoke.py` で呼び出し、"
        + (f"{skipped} 個を除くすべてに " if skipped else "すべてに ")
        + f"`tools/rpc_panic_hunt.py` が不正な引数を送ります。**ファジング**: libFuzzer のターゲット {fuzz} 個。",
    ]
    return "\n".join(en), "\n".join(ja)


BLOCKS = ("at-a-glance", "at-a-glance-ja")


def splice(doc, name, body):
    begin = f"<!-- BEGIN GENERATED {name}: tools/compat_table.py --write; do not edit by hand -->"
    end = f"<!-- END GENERATED {name} -->"
    pattern = re.compile(re.escape(begin) + r".*?" + re.escape(end), re.S)
    if not pattern.search(doc):
        raise SystemExit(f"{DOC}: no generated block '{name}' (markers {begin!r} ... {end!r})")
    return pattern.sub(lambda _: begin + "\n" + body + "\n" + end, doc)


def main():
    en, ja = tables()
    with open(DOC, encoding="utf-8") as f:
        current = f.read()
    wanted = splice(splice(current, BLOCKS[0], en), BLOCKS[1], ja)
    args = sys.argv[1:]
    write = "--write" in args or ("--check" in args and os.environ.get("COMPAT_TABLE_WRITE", "") not in ("", "0"))
    if write:
        if wanted != current:
            with open(DOC, "w", encoding="utf-8", newline="\n") as f:
                f.write(wanted)
            print(f"rewrote the generated blocks in {DOC}")
        return 0
    if "--check" in args:
        if wanted != current:
            print(f"{DOC} is stale: run python3 tools/compat_table.py --write (or COMPAT_TABLE_WRITE=1 ctest -R compat_table_check)")
            return 1
        print("docs/compatibility.md is up to date")
        return 0
    print(en + "\n\n" + ja)
    return 0


if __name__ == "__main__":
    sys.exit(main())
