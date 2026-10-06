#!/usr/bin/env python3
"""Source checks for the legal boundaries in docs/legal-boundaries.md.

Behaviour is tested in the C++ tests; this catches the regressions that are easiest to see in the source: a live
Quick Select or healing preview coming back, random or propagated search in the fills, a painted sampling area, a
per-pixel smudge patch, grain that is not anchored to the canvas, and G'MIC run paths that skip the exclusion list.
Run from anywhere: python3 tools/check_legal_boundaries.py
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
SRC = ROOT / "src"
failures = []


def code(path):
    """The file without // comments and string literals' contents, so the boundary notes themselves do not match."""
    text = (SRC / path).read_text(encoding="utf-8")
    text = re.sub(r'"(?:\\.|[^"\\])*"', '""', text)
    return re.sub(r"//[^\n]*", "", text)


def forbid(path, pattern, why):
    if re.search(pattern, code(path)):
        failures.append(f"{path}: {why} (matched {pattern!r})")


def require(path, pattern, why, count=1):
    found = len(re.findall(pattern, code(path)))
    if found < count:
        failures.append(f"{path}: {why} (expected {count} of {pattern!r}, found {found})")


# Quick Select solves once, when the pointer is released: the canvas never starts a solve while dragging.
for f in sorted((SRC / "app").glob("CanvasWidget*.cpp")):
    forbid(f.relative_to(SRC), r"startQuickSelectJob", "the canvas must not start a Quick Select solve (solve on release only)")
# Enhance Edge is geometric: the Quick Select refinement never reads the image through the matting refiner.
forbid("app/EditorSessionQuickSelect.cpp", r"refineMatte\s*\(", "Quick Select's refinement must stay geometric")
# Healing shows no live healed result.
for f in sorted((SRC / "app").glob("*.cpp")) + sorted((SRC / "core" / "src").glob("*.cpp")):
    forbid(f.relative_to(SRC), r"previewHeal|healPreview_", "no live healing preview")
# The fill: no randomness, propagation, perturbation or coarse-to-fine maps.
for f in ("core/src/inpaint.cpp",):
    forbid(f, r"\b(rand|random|xorshift|mt19937|uniform_int|uniform_real|propagate\w*|coarsen|pyramid|dominantOffsets)\b",
           "content-aware fill must be the exhaustive exemplar scan")
require("core/src/inpaint.cpp", r"bestSource\(", "the exemplar scan", 2)
# Spot Healing's source search: an exhaustive scan, no random candidates or halving random search.
forbid("core/src/heal.cpp", r"rng\s*\^=|xorshift|ringScore", "Spot Healing's patch search must be the exhaustive scan")
# Global-sampling matting: no PatchMatch-style propagation or random pairs.
forbid("core/src/matte.cpp", r"previousF|previousB|it \* 64 \+ step", "matting must not propagate or perturb sample pairs")
# Content-Aware Fill: no user-drawn sampling area.
for f in ("app/EditorSession.h", "app/EditorSessionPixels.cpp", "app/ContentFillDialog.cpp", "app/AutomationPixels.cpp"):
    forbid(f, r"Sampling::Custom|sampleArea", "no user-drawn sampling area for Content-Aware Fill")
# Smudge carries one colour, not a patch.
forbid("core/src/warpstroke.cpp", r"carried_\.assign\(size_t\(side\)", "the smudge must not carry a per-pixel patch")
# Grain is static and canvas-anchored; deposition never follows speed.
require("core/src/tipbrush.cpp", r"tip_\.grainMode = BrushTip::GrainMode::Canvas;", "tip strokes force canvas grain")
require("core/src/brushdynamics.cpp", r"mappingAllowed\(m\.target, m\.input\)", "dynamics skip disallowed mappings", 2)
# MyPaint presets are filtered.
require("core/src/mypaint.cpp", r"const dropped\[\]", "MyPaint presets lose the smudge-bucket and paint-mode settings")
# G'MIC: every run path checks the exclusion list.
require("app/Gmic.cpp", r"excludedIn\(command\)", "every G'MIC run path refuses excluded commands", 5)

if failures:
    print("Legal boundary checks failed (docs/legal-boundaries.md):")
    for f in failures:
        print("  " + f)
    sys.exit(1)
print("legal boundary checks: ok")
