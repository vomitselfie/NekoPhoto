#!/usr/bin/env python3
"""Checks that every translation is complete against the current sources.

Runs lupdate over src/app into a copy of each translations/nekophoto_<lang>.ts (so strings added to the code since
the file was last updated show up as unfinished), then fails when any message is unfinished or no longer in the
code. The committed file is never touched; `cmake --build build --target update_translations` updates it.

    tools/check_translations.py [--lupdate /path/to/lupdate] [--verbose]
"""
import argparse
import glob
import os
import shutil
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def find_lupdate(given):
    if given:
        return given
    for name in ("lupdate", "lupdate6", "lupdate-qt6"):
        path = shutil.which(name)
        if path:
            return path
    for path in ("/usr/lib/qt6/bin/lupdate", "/usr/lib64/qt6/bin/lupdate", "/usr/lib/x86_64-linux-gnu/qt6/bin/lupdate"):
        if os.path.exists(path):
            return path
    return None


def check(ts_path, lupdate, verbose):
    with tempfile.TemporaryDirectory() as tmp:
        copy = os.path.join(tmp, os.path.basename(ts_path))
        shutil.copy(ts_path, copy)
        run = subprocess.run([lupdate, "-silent", "-locations", "none", os.path.join(ROOT, "src", "app"), "-ts", copy],
                             capture_output=True, text=True)
        if run.returncode != 0:
            print(run.stdout + run.stderr)
            return False
        tree = ET.parse(copy)
    total = unfinished = gone = 0
    problems = []
    for context in tree.getroot().iter("context"):
        cname = context.findtext("name")
        for message in context.iter("message"):
            translation = message.find("translation")
            kind = translation.get("type") if translation is not None else "unfinished"
            source = message.findtext("source")
            if kind in ("vanished", "obsolete"):
                gone += 1
                problems.append(f"  no longer in the code: {cname}: {source!r}")
                continue
            total += 1
            text = "".join(translation.itertext()) if translation is not None else ""
            if kind == "unfinished" or not text.strip():
                unfinished += 1
                problems.append(f"  untranslated: {cname}: {source!r}")
    name = os.path.basename(ts_path)
    print(f"{name}: {total} strings, {total - unfinished} translated, {unfinished} unfinished, {gone} obsolete")
    if problems:
        shown = problems if verbose else problems[:40]
        print("\n".join(shown))
        if len(problems) > len(shown):
            print(f"  ... and {len(problems) - len(shown)} more (--verbose lists all)")
        print("Update with `cmake --build build --target update_translations`, then translate in Qt Linguist "
              "(see docs/translating.md).")
    return not problems


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--lupdate")
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args()
    lupdate = find_lupdate(args.lupdate)
    if not lupdate:
        print("lupdate not found (Qt LinguistTools); pass --lupdate")
        return 2
    files = sorted(glob.glob(os.path.join(ROOT, "translations", "nekophoto_*.ts")))
    ok = all([check(f, lupdate, args.verbose) for f in files])
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
