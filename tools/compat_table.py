#!/usr/bin/env python3
"""Generates the "At a glance" tables of docs/compatibility.md from the repository itself: the render-hash scenes,
golden PNGs, brush-parity baseline rows, CTest suites, the capability matrix per mode and depth, automation methods
registered and named in tools/rpc_smoke.py, the methods tools/rpc_panic_hunt.py leaves out, the fuzz targets and the
merged-composite oracle's floor. Everything outside the generated blocks stays hand-written.

It also writes the measured-results badges at the top of README.md (the compat-badges blocks, in English and
Japanese) from the same counts, and keeps the README's prose figures for the oracle and the automation methods in
step with them. The round-trip badge reads the hand-measured row of docs/compatibility.md and fails when the corpus
pinned in tests/patchy-manifest.txt has a different number of files than it says (rerun psd_roundtrip).

    python3 tools/compat_table.py            print the blocks
    python3 tools/compat_table.py --check    exit 1 when docs/compatibility.md or README.md is stale (a ctest test)
    python3 tools/compat_table.py --write    rewrite the blocks in docs/compatibility.md and README.md

COMPAT_TABLE_WRITE=1 with --check rewrites instead of failing.
"""
import ast
import glob
import os
import re
import sys

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
DOC = os.path.join(ROOT, "docs", "compatibility.md")
README = os.path.join(ROOT, "README.md")


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
    flatten = 0
    for l in lines("tests/psd_oracle.txt"):
        word, _, value = l.partition(" ")
        if word == "floor":
            floor = int(value)
        elif word == "compared":
            compared = int(value)
        elif word == "flatten":
            flatten = int(value)
    return (floor, compared, flatten) if floor is not None else None


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
        en.append(f"| Photoshop as the oracle | **{orc[0]} of {orc[1]}** Patchy files render within 2 levels of what Photoshop shows "
                  f"on 99% of pixels, mean under 1 level: Photoshop's own flatten beside the file for {orc[2]} of them, the merged image "
                  "stored in the file for the rest (the floor in `tests/psd_oracle.txt`; it may only rise) "
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
        ja.append(f"- **Photoshop の表示との比較**: Patchy のファイル {orc[1]} 個(うち {orc[2]} 個はファイルに添えられた Photoshop 自身の"
                  f"統合結果、残りはファイル内の統合画像と比較)のうち **{orc[0]} 個**が、"
                  "99% のピクセルで 2 レベル以内・平均 1 レベル未満(下限は `tests/psd_oracle.txt`、下げることはできません)。")
    ja += [
        f"- **描画のハッシュ**: {scenes} シーン。**ゴールデン画像**: {gcases} テスト・参照 PNG {gpngs} 枚。"
        f"**ブラシの基準値**: {brows} 行。**テストスイート**: CTest {suites} 個({cases} `TEST_CASE`)。",
        f"- **自動操作**: メソッド {registered} 個、うち {named} 個を `tools/rpc_smoke.py` で呼び出し、"
        + (f"{skipped} 個を除くすべてに " if skipped else "すべてに ")
        + f"`tools/rpc_panic_hunt.py` が不正な引数を送ります。**ファジング**: libFuzzer のターゲット {fuzz} 個。",
    ]
    return "\n".join(en), "\n".join(ja)


def patchy_corpus():
    """PSD and PSB files pinned in tests/patchy-manifest.txt: the round-trip corpus."""
    return sum(1 for l in lines("tests/patchy-manifest.txt") if re.search(r"\.ps[bd]$", l.split()[-1], re.I))


def round_trip(doc):
    """The hand-measured round trip of docs/compatibility.md: (passed, files)."""
    m = re.search(r"\| PSD round trip over Patchy's fixtures \| \*\*(\d+) of (\d+) files pass\*\*", doc)
    if not m:
        raise SystemExit(f"{DOC}: no 'PSD round trip over Patchy's fixtures | **N of N files pass**' row")
    passed, files = int(m.group(1)), int(m.group(2))
    corpus = patchy_corpus()
    if files != corpus:
        raise SystemExit(f"{DOC} reports the round trip over {files} files, but tests/patchy-manifest.txt pins {corpus}: "
                         "rerun build/tests/psd_roundtrip over Patchy's fixtures and update that row")
    return passed, files


def shields_escape(text):
    """A static shields.io badge's label or message: '-' and '_' doubled, then percent-encoded (a space as %20)."""
    from urllib.parse import quote
    return quote(text.replace("-", "--").replace("_", "__"), safe="")


GREEN, YELLOWGREEN, YELLOW, BLUE = "2ea44f", "97ca00", "dfb317", "2f7bf5"


def ratio_colour(passed, total):
    if total and passed == total:
        return GREEN
    return YELLOWGREEN if total and passed / total >= 0.75 else YELLOW


def badge(label, message, colour, alt, href):
    src = f"https://img.shields.io/badge/{shields_escape(label)}-{shields_escape(message)}-{colour}?style=flat-square"
    return f'  <a href="{href}"><img alt="{alt}" src="{src}"></a>'


def badges(doc):
    passed, files = round_trip(doc)
    suites, _ = ctest_suites()
    registered, _ = automation()
    orc = oracle()
    en = [badge("PSD round trip", f"{passed}/{files}", ratio_colour(passed, files),
                f"PSD round trip: {passed} of {files} files", "docs/compatibility.md#at-a-glance")]
    ja = [badge("PSD 往復", f"{passed}/{files}", ratio_colour(passed, files),
                f"PSD の往復: {files} 個中 {passed} 個", "docs/compatibility.md#日本語")]
    if orc:
        en.append(badge("Photoshop match", f"{orc[0]}/{orc[1]}", ratio_colour(orc[0], orc[1]),
                        f"Matches Photoshop's render: {orc[0]} of {orc[1]} files", "docs/compatibility.md#at-a-glance"))
        ja.append(badge("Photoshop と一致", f"{orc[0]}/{orc[1]}", ratio_colour(orc[0], orc[1]),
                        f"Photoshop の描画と一致: {orc[1]} 個中 {orc[0]} 個", "docs/compatibility.md#日本語"))
    en += [badge("tests", f"{suites} suites", BLUE, f"Tests: {suites} CTest suites", "docs/compatibility.md#at-a-glance"),
           badge("automation", f"{registered} methods", BLUE, f"Automation: {registered} methods", "docs/automation.md")]
    ja += [badge("テスト", f"CTest {suites} 個", BLUE, f"テスト: CTest {suites} 個", "docs/compatibility.md#日本語"),
           badge("自動操作", f"メソッド {registered} 個", BLUE, f"自動操作: メソッド {registered} 個", "docs/automation.md")]
    return "\n".join(en), "\n".join(ja)


def readme_prose(text):
    """The README's own figures for the oracle and the automation methods, kept in step with the counts."""
    registered, _ = automation()
    orc = oracle()
    if orc:
        text = re.sub(r"\d+ of \d+ files render within", f"{orc[0]} of {orc[1]} files render within", text)
        text = re.sub(r"\d+ 個中 \d+ 個のファイルが", f"{orc[1]} 個中 {orc[0]} 個のファイルが", text)
    text = re.sub(r"\d+ automation methods", f"{registered} automation methods", text)
    text = re.sub(r"\d+ 個の自動操作メソッド", f"{registered} 個の自動操作メソッド", text)
    return text


README_BLOCKS = ("compat-badges", "compat-badges-ja")


def splice_readme(text, name, body):
    begin, end = f"<!-- {name}:start (tools/compat_table.py --write) -->", f"<!-- {name}:end -->"
    pattern = re.compile(re.escape(begin) + r".*?" + re.escape(end), re.S)
    if not pattern.search(text):
        raise SystemExit(f"{README}: no generated block '{name}' (markers {begin!r} ... {end!r})")
    return pattern.sub(lambda _: begin + "\n" + body + "\n  " + end, text)


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
    with open(README, encoding="utf-8") as f:
        readme = f.read()
    badges_en, badges_ja = badges(current)
    readme_wanted = readme_prose(splice_readme(splice_readme(readme, README_BLOCKS[0], badges_en), README_BLOCKS[1], badges_ja))
    pending = [(path, have, want) for path, have, want in ((DOC, current, wanted), (README, readme, readme_wanted)) if have != want]
    args = sys.argv[1:]
    write = "--write" in args or ("--check" in args and os.environ.get("COMPAT_TABLE_WRITE", "") not in ("", "0"))
    if write:
        for path, _, want in pending:
            with open(path, "w", encoding="utf-8", newline="\n") as f:
                f.write(want)
            print(f"rewrote the generated blocks in {path}")
        return 0
    if "--check" in args:
        if pending:
            for path, _, _ in pending:
                print(f"{path} is stale: run python3 tools/compat_table.py --write (or COMPAT_TABLE_WRITE=1 ctest -R compat_table_check)")
            return 1
        print("docs/compatibility.md and README.md are up to date")
        return 0
    print(en + "\n\n" + ja + "\n\n" + badges_en + "\n\n" + badges_ja)
    return 0


if __name__ == "__main__":
    sys.exit(main())
