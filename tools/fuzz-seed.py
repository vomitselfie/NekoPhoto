#!/usr/bin/env python3
"""Seed the fuzzers' corpora (docs/fuzzing.md) from a folder of real PSD/PSB files:
    tools/fuzz-seed.py <psd folder> <corpus_psd> <corpus_parts>
Small whole files go to corpus_psd; their blocks, each prefixed with fuzz_psd_parts' selector byte, to corpus_parts."""
import glob, os, shutil, struct, sys

SELECTORS = {"vmsk": [0], "vsms": [0], "TySh": [1], "SoLd": [2, 4, 11], "PlLd": [3], "FEid": [5], "lnk2": [6], "GdFl": [8], "PtFl": [8]}

def main():
    if len(sys.argv) != 4:
        raise SystemExit(__doc__)
    source, whole, parts = sys.argv[1:]
    os.makedirs(whole, exist_ok=True)
    os.makedirs(parts, exist_ok=True)
    files = glob.glob(os.path.join(source, "*.ps[db]"))
    n = 0
    for f in files:
        if os.path.getsize(f) < 200_000:
            shutil.copy(f, whole)
        data = open(f, "rb").read()
        for key, selectors in SELECTORS.items():
            at = 0
            while (at := data.find(b"8BIM" + key.encode(), at)) >= 0:
                length = struct.unpack(">I", data[at + 8:at + 12])[0]
                if 0 < length < 400_000:
                    for s in selectors:
                        with open(os.path.join(parts, f"{key}_{s}_{n}"), "wb") as out:
                            out.write(bytes([s]) + data[at + 12:at + 12 + length])
                        n += 1
                at += 8
    print(f"{len(files)} files, {n} block seeds")

if __name__ == "__main__":
    main()
