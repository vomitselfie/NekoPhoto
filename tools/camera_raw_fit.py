#!/usr/bin/env python3
"""Fits the Camera Raw look (src/core/src/rawtone_tables.inc) to Camera Raw 18.7's renders of RAWmakase's synthetic chart
(tests/fixtures/camera-raw-parity, MIT; docs/camera-raw.md#the-default-render).

Everything here is read off the chart's 24-step grey ramp (-8 to +3.5 EV in half stops):

- The default tone curve: the "default" case's ramp against the ramp's scene values, as LibRaw decodes the chart (linear,
  white-balanced, 1 = the raw white point), with the scene values beyond the raw white point left out.
- Exposure's highlight shoulder: what positive Exposure does to the ramp beyond a plain scale before that curve.
- The Basic panel's tone sliders (Whites, Blacks, Highlights, Shadows, Dehaze, Contrast) and the parametric curve's
  regions: each case that sets one of them alone gives the curve from the default render's ramp to its own, sampled on
  a fixed grid of encoded values.

    python3 tools/camera_raw_fit.py            print the fit's residuals on the ramp
    python3 tools/camera_raw_fit.py --write    rewrite src/core/src/rawtone_tables.inc

Only numpy is needed.
"""
import json
import math
import os
import sys

import numpy as np

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
CORPUS = os.path.join(ROOT, "tests", "fixtures", "camera-raw-parity")
OUT = os.path.join(ROOT, "src", "core", "src", "rawtone_tables.inc")

# The 0 EV grey of the chart as LibRaw decodes it (linear ProPhoto output, camera white balance, 16 bits): 9579 / 65535.
# The other steps are this times 2^EV; LibRaw clips them at the raw white point, 1.
GREY_0EV = 9579 / 65535

# Encoded inputs the slider curves are sampled at: sin^2, so dense in the shadows and near white, where the ramp is.
GRID = [math.sin(math.pi * k / 64) ** 2 for k in range(33)]
# How far below the darkest step (log2) the default curve's first knots go.
CURVE_BELOW = [1.0, 0.5]

SLIDERS = {
    "Whites2012": "Whites", "Blacks2012": "Blacks", "Highlights2012": "Highlights", "Shadows2012": "Shadows",
    "Dehaze": "Dehaze", "Contrast2012": "Contrast", "ParametricShadows": "ParametricShadows",
    "ParametricDarks": "ParametricDarks", "ParametricLights": "ParametricLights", "ParametricHighlights": "ParametricHighlights",
}


def pchip(xk, yk):
    """Fritsch-Carlson monotone cubic through the knots; rawtone.cpp evaluates the same."""
    xk = np.asarray(xk, float)
    yk = np.asarray(yk, float)
    h = np.diff(xk)
    d = np.diff(yk) / h
    m = np.zeros_like(yk)
    with np.errstate(divide="ignore", invalid="ignore"):
        inner = np.where(d[:-1] * d[1:] > 0, 2 / (1 / d[:-1] + 1 / d[1:]), 0)
    m[1:-1] = inner
    m[0] = d[0]
    m[-1] = d[-1]

    def f(xq):
        xq = np.asarray(xq, float)
        i = np.clip(np.searchsorted(xk, xq) - 1, 0, len(xk) - 2)
        t = np.clip((xq - xk[i]) / h[i], 0, 1)
        h00 = 2 * t**3 - 3 * t**2 + 1
        h10 = t**3 - 2 * t**2 + t
        h01 = -2 * t**3 + 3 * t**2
        h11 = t**3 - t**2
        return h00 * yk[i] + h10 * h[i] * m[i] + h01 * yk[i + 1] + h11 * h[i] * m[i + 1]

    return f


def srgb_decode(v):
    v = np.clip(v, 0, 1)
    return np.where(v <= 0.04045, v / 12.92, ((v + 0.055) / 1.055) ** 2.4)


def load():
    layout = json.load(open(os.path.join(CORPUS, "layout.json")))["patches"]
    cases = json.load(open(os.path.join(CORPUS, "cases.json")))
    renders = json.load(open(os.path.join(CORPUS, "camera-raw-synthetic-d65.json")))["cases"]
    ramp = [i for i, p in enumerate(layout) if p["group"] == "ramp"]
    ev = [float(layout[i]["name"].split()[1]) for i in ramp]
    return cases, renders, ramp, ev


def ramp_of(renders, name, ramp):
    return np.array([np.mean(renders[name][i]) for i in ramp]) / 65535


def default_curve(renders, ramp, ev):
    scene = GREY_0EV * np.exp2(ev)
    encoded = ramp_of(renders, "default", ramp)
    # The steps below the raw white point; the white point itself renders white, as clipped highlights do in Camera Raw
    # (the steps past it, which the raw file clips, render within 1/80 of white there).
    keep = [i for i in range(len(ev)) if scene[i] < 1]
    f = pchip(np.append(np.log2(scene[keep]), 0.0), np.append(encoded[keep], 1.0))
    # Knots at the steps themselves, the white point, and two half stops below the darkest step.
    knots = np.concatenate([np.log2(scene[0]) - np.array(CURVE_BELOW), np.log2(scene[keep]), [0.0]])
    values = f(np.maximum(knots, np.log2(scene[0])))
    # Below the darkest step the display value is proportional to the scene value (rawtone.cpp).
    low = knots < np.log2(scene[0])
    first = srgb_decode(encoded[0]) / scene[0]
    lin = first * np.exp2(knots[low])
    values[low] = np.where(lin <= 0.0031308, lin * 12.92, 1.055 * lin ** (1 / 2.4) - 0.055)
    values = np.maximum.accumulate(values)
    # How far the knots' own curve (what rawtone.cpp evaluates) is from the ramp.
    resid = pchip(knots, values)(np.log2(scene[keep])) - encoded[keep]
    return knots, values, np.abs(resid).max()


def shoulder(renders, ramp, ev, knots, values):
    """Positive Exposure: the scene value scale·x goes through x / (1 + x^p)^(1/p) before the curve; p fitted."""
    curve = pchip(knots, values)
    grid = np.exp2(np.linspace(knots[0], 0, 4001))
    table = curve(np.log2(grid))
    scene = GREY_0EV * np.exp2(ev)
    xs, ys = [], []
    for e in (1, 2, 3):
        name = "exposure%+.2f" % e
        if name not in renders:
            continue
        encoded = ramp_of(renders, name, ramp)
        for i in range(len(ev)):
            if scene[i] < 0.9 and scene[i] * 2**e > 0.05 and encoded[i] < 0.999:
                xs.append(scene[i] * 2**e)
                ys.append(encoded[i])
    xs = np.array(xs)
    ys = np.array(ys)
    best = None
    for p in np.arange(1.0, 8.0001, 0.05):
        shaped = xs / (1 + xs**p) ** (1 / p)
        err = np.sum((np.interp(np.log2(np.minimum(shaped, 1)), np.log2(grid), table) - ys) ** 2)
        if best is None or err < best[0]:
            best = (err, p)
    return round(best[1], 2), math.sqrt(best[0] / len(xs))


def slider_tables(cases, renders, ramp):
    base = ramp_of(renders, "default", ramp)
    tables = []
    for case in cases["cases"]:
        settings = case.get("settings", {})
        if case.get("look") or case.get("curves") or len(settings) != 1:
            continue
        (key, text), = settings.items()
        if key not in SLIDERS or case["name"] not in renders:
            continue
        out = ramp_of(renders, case["name"], ramp)
        xs, ys = [0.0], [None]
        for a, b in zip(base, out):
            if a > xs[-1] + 1e-4:
                xs.append(a)
                ys.append(b)
        # Black: extended from the two darkest steps; white: from the two brightest.
        ys[0] = max(0.0, ys[1] - (ys[2] - ys[1]) / (xs[2] - xs[1]) * xs[1])
        if xs[-1] < 1:
            slope = (ys[-1] - ys[-2]) / (xs[-1] - xs[-2])
            xs.append(1.0)
            ys.append(min(1.0, ys[-1] + slope * (1 - xs[-2])))
        ys = np.maximum.accumulate(np.clip(ys, 0, 1))
        f = pchip(xs, ys)
        tables.append((SLIDERS[key], float(text), np.clip(f(GRID), 0, 1)))
    tables.sort(key=lambda t: (list(SLIDERS.values()).index(t[0]), t[1]))
    return tables


def numbers(values, per_line=8, indent="    "):
    items = ["%.6f" % v for v in values]
    return ",\n".join(indent + ", ".join(items[i:i + per_line]) for i in range(0, len(items), per_line))


def main():
    cases, renders, ramp, ev = load()
    knots, values, resid = default_curve(renders, ramp, ev)
    p, shoulder_rms = shoulder(renders, ramp, ev, knots, values)
    tables = slider_tables(cases, renders, ramp)
    print("default curve: %d knots, largest residual on the ramp %.5f (encoded)" % (len(knots), resid))
    print("exposure shoulder: p = %.2f, rms %.5f (encoded)" % (p, shoulder_rms))
    for name, amount, _ in tables:
        print("  %s %+g" % (name, amount))
    if "--write" not in sys.argv:
        return
    lines = [
        "// Generated by tools/camera_raw_fit.py from Camera Raw 18.7's renders of RAWmakase's synthetic chart",
        "// (tests/fixtures/camera-raw-parity, MIT, LICENSES/RAWmakase-MIT.txt). Do not edit; rerun the script.",
        "// docs/camera-raw.md#the-default-render describes the fit.",
        "",
        "// The default tone curve: knots of log2(scene value) and the encoded sRGB value Camera Raw renders there.",
        "constexpr double kCurveLog2[] = {",
        numbers(knots),
        "};",
        "constexpr double kCurveEncoded[] = {",
        numbers(values),
        "};",
        "",
        "// Positive Exposure's highlight shoulder: x / (1 + x^p)^(1/p) on the scaled scene value.",
        "constexpr double kShoulderPower = %.2f;" % p,
        "",
        "// The encoded inputs every slider curve is sampled at.",
        "constexpr double kSliderGrid[] = {",
        numbers(GRID),
        "};",
        "",
        "struct SliderTable {",
        "    ToneSlider slider;",
        "    double amount;",
        "    double out[%d];" % len(GRID),
        "};",
        "",
        "// Each case that moves one slider alone: the curve from the default render's ramp to the case's.",
        "constexpr SliderTable kSliderTables[] = {",
    ]
    for name, amount, out in tables:
        lines.append("    {ToneSlider::%s, %g, {" % (name, amount))
        lines.append(numbers(out, 8, "        "))
        lines.append("    }},")
    lines.append("};")
    with open(OUT, "w") as f:
        f.write("\n".join(lines) + "\n")
    print("wrote", os.path.relpath(OUT, ROOT))


if __name__ == "__main__":
    main()
