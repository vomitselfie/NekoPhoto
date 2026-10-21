#!/usr/bin/env python3
"""Fits the Camera Raw look (src/core/src/rawtone_tables.inc) to Camera Raw 18.7's renders of RAWmakase's synthetic chart
(tests/fixtures/camera-raw-parity, MIT; docs/camera-raw.md#fitted-responses).

From the chart's 24-step grey ramp (-8 to +3.5 EV in half stops):

- The default tone curve: the "default" case's ramp against the ramp's scene values, as LibRaw decodes the chart (linear,
  white-balanced, 1 = the raw white point), with the scene values beyond the raw white point left out.
- Exposure's highlight shoulder: what positive Exposure does to the ramp beyond a plain scale before that curve.
- The Basic panel's tone sliders (Whites, Blacks, Highlights, Shadows, Dehaze, Contrast) and the parametric curve's
  regions: each case that sets one of them alone gives the curve from the default render's ramp to its own, sampled on
  a fixed grid of encoded values.
- Shadow Tint's push, per encoded luminance.

From all of the chart's patches, starting from Camera Raw's own default render (so a fit does not carry the default
render's error), by grid or simplex search on the mean CIEDE2000 the parity test computes:

- whether each tone slider works on the colour's channels or its luminance; Camera Calibration's strengths; the post-crop
  vignette's mask and strengths; lens vignetting's strength and power.

The colour mixer, Vibrance, Colour Grading, Black & White and the parametric curve's composed regions are not fitted here:
they render through RAWmakase's measured tables (src/third_party/rawmakase, src/core/src/rawmeasured.cpp).

    python3 tools/camera_raw_fit.py            print the fits
    python3 tools/camera_raw_fit.py --write    rewrite src/core/src/rawtone_tables.inc (about a minute and a half)

Only numpy is needed. The Transform, Distortion and Clarity constants in src/core/src/cameraraw.cpp were fitted by hand
with the parity test's NEKOPHOTO_CAMERA_RAW_PARITY_TRY and ONLY variables (docs/camera-raw.md#fitted-responses).
"""
import itertools
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
        tables.append((SLIDERS[key], float(text), np.clip(f(GRID), 0, 1), case["name"]))
    tables.sort(key=lambda t: (list(SLIDERS.values()).index(t[0]), t[1]))
    return tables


# ---- Colour: what rawtone.cpp does to a colour, in numpy ---------------------------------------------------------------

# LibRaw's linear sRGB -> ProPhoto matrix (its ProPhoto output table), ProPhoto's luminance row.
SRGB_TO_PP = np.array([[0.529317, 0.330092, 0.140588], [0.098368, 0.873465, 0.028169], [0.016879, 0.117663, 0.865457]])
PP_TO_SRGB = np.linalg.inv(SRGB_TO_PP)
PP_Y = np.array([0.2880402, 0.7118741, 0.0000857])


def srgb_encode(lin):
    lin = np.clip(lin, 0, 1)
    return np.where(lin <= 0.0031308, lin * 12.92, 1.055 * np.power(lin, 1 / 2.4) - 0.055)


def pp_encoded(srgb16):
    """Camera Raw's patch values (encoded sRGB, 16-bit) as ProPhoto RGB with the sRGB transfer, 0..1."""
    return srgb_encode(srgb_decode(srgb16 / 65535) @ SRGB_TO_PP.T)


def srgb16(pp_enc):
    return srgb_encode(srgb_decode(np.clip(pp_enc, 0, 1)) @ PP_TO_SRGB.T) * 65535


def hue_keeping(e, f):
    hi = e.max(1)
    lo = e.min(1)
    t = np.where((hi > lo)[:, None], (e - lo[:, None]) / np.maximum(hi - lo, 1e-12)[:, None], 0)
    return f(lo)[:, None] + (f(hi) - f(lo))[:, None] * t


def by_luminance(e, f):
    lin = srgb_decode(e)
    y = np.maximum(lin @ PP_Y, 1e-9)
    ratio = srgb_decode(np.clip(f(srgb_encode(y)), 0, 1)) / y
    return srgb_encode(lin * ratio[:, None])


def lab(srgb16_values):
    lin = srgb_decode(srgb16_values / 65535)
    xyz = lin @ np.array([[0.4124564, 0.3575761, 0.1804375], [0.2126729, 0.7151522, 0.0721750], [0.0193339, 0.1191920, 0.9503041]]).T
    xyz = xyz / np.array([0.95047, 1.0, 1.08883])
    f = np.where(xyz > 216 / 24389, np.cbrt(np.maximum(xyz, 0)), (24389 / 27 * xyz + 16) / 116)
    return np.stack([116 * f[:, 1] - 16, 500 * (f[:, 0] - f[:, 1]), 200 * (f[:, 1] - f[:, 2])], 1)


def delta_e00(a16, b16):
    """CIEDE2000, as tests/camera_raw_parity.cpp computes it."""
    (L1, a1, b1), (L2, a2, b2) = lab(a16).T, lab(b16).T
    cb = (np.hypot(a1, b1) + np.hypot(a2, b2)) / 2
    g = 0.5 * (1 - np.sqrt(cb**7 / (cb**7 + 25.0**7)))
    a1p, a2p = (1 + g) * a1, (1 + g) * a2
    c1p, c2p = np.hypot(a1p, b1), np.hypot(a2p, b2)
    h1 = np.where(c1p == 0, 0, np.degrees(np.arctan2(b1, a1p)) % 360)
    h2 = np.where(c2p == 0, 0, np.degrees(np.arctan2(b2, a2p)) % 360)
    dh = h2 - h1
    dh = np.where(dh > 180, dh - 360, np.where(dh < -180, dh + 360, dh))
    dh = np.where(c1p * c2p == 0, 0, dh)
    dH = 2 * np.sqrt(c1p * c2p) * np.sin(np.radians(dh / 2))
    lb, cpb, hs = (L1 + L2) / 2, (c1p + c2p) / 2, h1 + h2
    hb = np.where(c1p * c2p == 0, hs, np.where(np.abs(h1 - h2) > 180, np.where(hs < 360, (hs + 360) / 2, (hs - 360) / 2), hs / 2))
    t = 1 - 0.17 * np.cos(np.radians(hb - 30)) + 0.24 * np.cos(np.radians(2 * hb)) + 0.32 * np.cos(np.radians(3 * hb + 6)) - 0.20 * np.cos(np.radians(4 * hb - 63))
    sl = 1 + 0.015 * (lb - 50) ** 2 / np.sqrt(20 + (lb - 50) ** 2)
    sc, sh = 1 + 0.045 * cpb, 1 + 0.015 * cpb * t
    rt = -np.sin(np.radians(2 * 30 * np.exp(-(((hb - 275) / 25) ** 2)))) * 2 * np.sqrt(cpb**7 / (cpb**7 + 25.0**7))
    return np.sqrt(((L2 - L1) / sl) ** 2 + ((c2p - c1p) / sc) ** 2 + (dH / sh) ** 2 + rt * ((c2p - c1p) / sc) * (dH / sh))


class Scorer:
    """Mean ΔE00 of a render of the chart against a case's, the wide-gamut row left out."""

    def __init__(self, layout, renders):
        self.keep = np.array([p["group"] != "wide" for p in layout])
        self.renders = renders

    def __call__(self, ours16, case):
        return float(delta_e00(ours16, np.array(self.renders[case], float))[self.keep].mean())


def case_settings(cases, prefix_keys):
    """Cases that set only attributes for which prefix_keys(key) is true, with no look or curves."""
    out = []
    for case in cases["cases"]:
        settings = case.get("settings", {})
        if settings and not case.get("look") and not case.get("curves") and all(prefix_keys(k) for k in settings):
            out.append((case["name"], {k: float(v) for k, v in settings.items()}))
    return out


def slider_luminance(renders, tables, score):
    """For each slider and sign: how much of its curve works on the colour's luminance rather than on its brightest and
    darkest channel (0, 1/4, ... 1), whichever fits the colours of its cases best, applied to Camera Raw's default render."""
    e = pp_encoded(np.array(renders["default"], float))
    names = list(SLIDERS.values())
    weights = np.zeros((len(names), 2))
    for i, name in enumerate(names):
        if name.startswith("Parametric"):
            continue
        for sign in (0, 1):
            group = [(out, case) for n, a, out, case in tables if n == name and (a > 0) == (sign == 1)]
            best = None
            for w in (0, 0.25, 0.5, 0.75, 1):
                err = 0
                for out, case in group:
                    f = pchip(GRID, out)
                    err += score(srgb16(hue_keeping(e, f) * (1 - w) + by_luminance(e, f) * w), case)
                if best is None or err < best[0] - 1e-9:
                    best = (err, w)
            weights[i, sign] = best[1]
    return weights


def grade_luma(renders, ramp):
    """The grey ramp's encoded luminance in the default render: where Shadow Tint's push is read."""
    e0 = pp_encoded(np.array(renders["default"], float))[ramp]
    return srgb_encode(srgb_decode(e0) @ PP_Y)


# ---- Camera Calibration -----------------------------------------------------------------------------------------------

def nelder_mead(f, x0, step=0.05, iterations=300):
    n = len(x0)
    pts = [np.array(x0, float)] + [np.array(x0, float) + step * np.eye(n)[i] for i in range(n)]
    vals = [f(p) for p in pts]
    for _ in range(iterations):
        order = np.argsort(vals)
        pts, vals = [pts[i] for i in order], [vals[i] for i in order]
        c = np.mean(pts[:-1], 0)
        r = c + (c - pts[-1])
        fr = f(r)
        if fr < vals[0]:
            e = c + 2 * (c - pts[-1])
            fe = f(e)
            pts[-1], vals[-1] = (e, fe) if fe < fr else (r, fr)
        elif fr < vals[-2]:
            pts[-1], vals[-1] = r, fr
        else:
            k = c + 0.5 * (pts[-1] - c)
            fk = f(k)
            if fk < vals[-1]:
                pts[-1], vals[-1] = k, fk
            else:
                pts = [pts[0]] + [pts[0] + 0.5 * (p - pts[0]) for p in pts[1:]]
                vals = [vals[0]] + [f(p) for p in pts[1:]]
    i = int(np.argmin(vals))
    return pts[i], vals[i]


# The primaries' sliders as a matrix on scene-light ProPhoto RGB that keeps greys: Hue moves a primary into the two other
# channels in opposite directions, Saturation out of both alike. (row, column) pairs each slider feeds, with the sign Hue
# gives the first.
CALIBRATION = {"RedHue": (0, (1, 0), (2, 0)), "GreenHue": (1, (0, 1), (2, 1)), "BlueHue": (2, (0, 2), (1, 2))}


def calibration_matrix(primary, hue, saturation):
    """Neutral-keeping matrix for one primary (0 red, 1 green, 2 blue): `hue` and `saturation` already scaled to strengths."""
    (_, first, second) = list(CALIBRATION.values())[primary]
    e = np.zeros((3, 3))
    e[first] += hue - saturation
    e[second] += -hue - saturation
    for r in range(3):
        e[r, r] = -(e[r].sum() - e[r, r])
    return np.eye(3) + e


def fit_calibration(cases, renders, score, knots, values):
    """Each primary's Hue and Saturation strength per 100 (the matrix above, in scene light), and Shadow Tint's push per
    unit along its measured direction at each encoded luminance of the ramp (like Colour Grading's)."""
    curve = pchip(knots, values)
    grid = np.exp2(np.linspace(knots[0], 0, 8001))
    display = srgb_decode(curve(np.log2(grid)))
    pp = np.clip(srgb_decode(np.array(renders["default"], float) / 65535) @ SRGB_TO_PP.T, 0, 1)
    scene = hue_keeping(pp, lambda v: np.interp(v, display, grid))

    def render(m):
        s = np.maximum(scene @ m.T, 0)
        return srgb_encode(hue_keeping(s, lambda v: np.interp(np.clip(v, 0, 1), grid, display)) @ PP_TO_SRGB.T) * 65535

    keys = {"RedHue": (0, "hue"), "RedSaturation": (0, "sat"), "GreenHue": (1, "hue"), "GreenSaturation": (1, "sat"),
            "BlueHue": (2, "hue"), "BlueSaturation": (2, "sat")}
    strengths = {}
    for key, (primary, kind) in keys.items():
        group = [(n, s[key] / 100) for n, s in case_settings(cases, lambda k, key=key: k == key)]
        estimates = []
        for n, a in group:
            p, _ = nelder_mead(lambda p: score(render(calibration_matrix(primary, p[0] if kind == "hue" else 0, p[0] if kind == "sat" else 0)), n), [0.0], 0.1, 60)
            estimates.append(p[0] / a)
        strengths[key] = float(np.mean(estimates))
    return strengths


def shadow_tint_table(renders, ramp):
    """Shadow Tint on the ramp: a push per unit (tint / 100) along its direction in ProPhoto RGB with the sRGB transfer."""
    e0 = pp_encoded(np.array(renders["default"], float))[ramp]
    pushes = []
    for name, amount in (("calibration-shadow-tint+30", 0.3), ("calibration-shadow-tint-60", -0.6)):
        off = pp_encoded(np.array(renders[name], float))[ramp] - e0
        pushes.append((off - off.mean(1)[:, None]) / amount)
    push = np.mean(pushes, 0)
    direction = push.sum(0) / np.linalg.norm(push.sum(0))
    return direction, push @ direction


# ---- Vignettes: masks over each patch, sampled 4 x 4 as the render averages it ----------------------------------------

class Frame:
    def __init__(self, layout_json):
        self.width, self.height = layout_json["width"], layout_json["height"]
        pts = []
        for p in layout_json["patches"]:
            xs = p["x"] + (np.arange(4) + 0.5) * p["w"] / 4
            ys = p["y"] + (np.arange(4) + 0.5) * p["h"] / 4
            gx, gy = np.meshgrid(xs, ys)
            pts.append(np.stack([gx.ravel(), gy.ravel()], 1))
        pts = np.array(pts)
        w2, h2 = self.width / 2, self.height / 2
        nx, ny = (pts[..., 0] - w2) / w2, (pts[..., 1] - h2) / h2
        self.ellipse = np.hypot(nx, ny) / np.sqrt(2)
        self.circle = np.hypot(pts[..., 0] - w2, pts[..., 1] - h2) / np.hypot(w2, h2)


def smoothstep(t):
    t = np.clip(t, 0, 1)
    return t * t * (3 - 2 * t)


def vignette_mask(frame, midpoint, feather, roundness, start, span, shrink):
    rho = roundness / 100
    d = frame.ellipse * (1 - rho) + frame.circle * rho if rho >= 0 else frame.ellipse * (1 + shrink * rho)
    return smoothstep((d - (start[0] + start[1] * midpoint / 100)) / np.maximum(0.02, span[0] + span[1] * feather / 100))


def per_patch(e, m):
    """e (patches, 3) through m (patches, samples) -> the patch means of a per-sample function's result."""
    return np.repeat(e[:, None, :], m.shape[1], 1)


def fit_vignettes(cases, renders, score, frame, knots, values):
    """Post-crop vignette: the mask (start and span against Midpoint and Feather, how negative Roundness shrinks it) from
    Paint Overlay, which darkens the encoded value; how far Highlight and Color Priority darken in scene light (stops at a
    full mask) and how much Highlights spares bright colours; how strongly each lightens. Lens vignetting: a gain in
    display light of 2^(amount * strength * r^power) with r 1 at the corners, the power following Midpoint."""
    e = np.array(renders["default"], float) / 65535
    vig = {}
    for name, s in case_settings(cases, lambda k: k.startswith("PostCropVignette")):
        vig[name] = (s.get("PostCropVignetteAmount", 0) / 100, int(s.get("PostCropVignetteStyle", 1)), s.get("PostCropVignetteMidpoint", 50),
                     s.get("PostCropVignetteFeather", 50), s.get("PostCropVignetteRoundness", 0), s.get("PostCropVignetteHighlightContrast", 0))

    def paint(a, m, lighten):
        E = per_patch(e, m)
        out = E * (1 + a * m[..., None]) if a < 0 else E + (1 - E) * lighten * a * m[..., None]
        return np.clip(out, 0, 1).mean(1) * 65535

    paints = {n: v for n, v in vig.items() if v[1] == 3}
    curve = pchip(knots, values)
    grid = np.exp2(np.linspace(knots[0], 0, 8001))
    display = srgb_decode(curve(np.log2(grid)))

    def priority(a, m, stops, protect, hl, lighten):
        E = per_patch(e, m)
        if a > 0:
            return np.clip(E + (1 - E) * lighten * a * m[..., None], 0, 1).mean(1) * 65535
        lin = srgb_decode(E) @ SRGB_TO_PP.T
        hi, lo = lin.max(-1), lin.min(-1)
        gain = np.exp2(stops * a * m * (1 - protect * hl / 100 * np.clip((hi - 0.2) / 0.8, 0, 1)))
        new_hi = np.interp(np.minimum(np.interp(hi, display, grid) * gain, 1), grid, display)
        new_lo = np.interp(np.minimum(np.interp(lo, display, grid) * gain, 1), grid, display)
        t = np.where((hi > lo)[..., None], (lin - lo[..., None]) / np.maximum(hi - lo, 1e-12)[..., None], 0)
        pp = new_lo[..., None] + (new_hi - new_lo)[..., None] * t
        return (srgb_encode(pp @ PP_TO_SRGB.T)).mean(1) * 65535

    others = {n: v for n, v in vig.items() if v[1] != 3}

    # The mask, from every darkening case at the default Roundness without Highlights (Paint Overlay and the priorities, these
    # at 3 stops): a coarse grid, then a finer one around its best.
    darkening = [(n, v) for n, v in vig.items() if v[0] < 0 and v[4] == 0 and v[5] == 0]

    def mask_error(s0, s1, w0, w1):
        err = 0
        for n, (a, st, mid, fe, ro, hl) in darkening:
            m = vignette_mask(frame, mid, fe, 0, (s0, s1), (w0, w1), 0)
            err += score(paint(a, m, 1) if st == 3 else priority(a, m, 3.0, 0, 0, 1), n)
        return err

    steps = (0.1, 0.2, 0.1, 0.25)
    coarse = itertools.product(np.arange(-0.2, 0.41, steps[0]), np.arange(0.4, 1.21, steps[1]), np.arange(0.05, 0.66, steps[2]), np.arange(0, 1.01, steps[3]))
    best = min((mask_error(*p), p) for p in coarse)
    fine = itertools.product(*[np.arange(c - s, c + s * 1.01, s / 4) for c, s in zip(best[1], steps)])
    best = min([best] + [(mask_error(*p), p) for p in fine if p[2] > 0])
    start, span = best[1][:2], best[1][2:]
    shrink = min(np.arange(0, 1.01, 0.05), key=lambda k: sum(score(paint(a, vignette_mask(frame, mid, fe, ro, start, span, k), 1), n)
                                                             for n, (a, st, mid, fe, ro, hl) in paints.items() if ro < 0))
    paint_lighten = min(np.arange(0.1, 1.51, 0.05), key=lambda k: sum(score(paint(a, vignette_mask(frame, mid, fe, ro, start, span, shrink), k), n)
                                                                      for n, (a, st, mid, fe, ro, hl) in paints.items() if a > 0))
    masks = {n: vignette_mask(frame, mid, fe, ro, start, span, shrink) for n, (a, st, mid, fe, ro, hl) in others.items()}
    stops = min(np.arange(1, 6.01, 0.2), key=lambda k: sum(score(priority(v[0], masks[n], k, 0, 0, 1), n) for n, v in others.items()
                                                            if v[0] < 0 and v[5] == 0))
    protect = min(np.arange(0, 1.01, 0.05), key=lambda k: sum(score(priority(v[0], masks[n], stops, k, v[5], 1), n) for n, v in others.items()
                                                               if v[0] < 0 and v[5] > 0))
    lighten = min(np.arange(0.1, 1.51, 0.05), key=lambda k: sum(score(priority(v[0], masks[n], stops, protect, v[5], k), n)
                                                                for n, v in others.items() if v[0] > 0))
    # Lens vignetting.
    lens = [(n, s["VignetteAmount"] / 100, s.get("VignetteMidpoint", 50)) for n, s in case_settings(cases, lambda k: k.startswith("Vignette"))]
    radius = frame.circle

    def lens_render(a, strength, power):
        lin = srgb_decode(per_patch(e, radius)) * np.exp2(a * strength * radius**power)[..., None]
        return srgb_encode(lin).mean(1) * 65535

    def power_at(midpoint, p20, p50):
        return np.clip(p20 + (midpoint - 20) / 30 * (p50 - p20), 1, 12)

    best = None
    for dark, p20, p50 in itertools.product(np.arange(0.5, 5.01, 0.25), np.arange(1, 8.01, 0.5), np.arange(2, 10.01, 0.5)):
        err = sum(score(lens_render(a, dark, power_at(mid, p20, p50)), n) for n, a, mid in lens if a < 0)
        if best is None or err < best[0]:
            best = (err, dark, p20, p50)
    _, dark, p20, p50 = best
    light = min(np.arange(0.5, 5.01, 0.25), key=lambda k: sum(score(lens_render(a, k, power_at(mid, p20, p50)), n) for n, a, mid in lens if a > 0))
    return {"start": start, "span": span, "shrink": shrink, "paintLighten": paint_lighten, "stops": stops, "protect": protect,
            "lighten": lighten, "lens": (dark, light, p20, p50)}


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
    print("slider curves:", ", ".join("%s %+g" % (name, amount) for name, amount, _, _ in tables))
    score = Scorer(json.load(open(os.path.join(CORPUS, "layout.json")))["patches"], renders)
    luminance = slider_luminance(renders, tables, score)
    print("slider luminance weights (negative, positive):", {n: tuple(luminance[i]) for i, n in enumerate(SLIDERS.values()) if luminance[i].any()})
    luma = grade_luma(renders, ramp)
    calibration = fit_calibration(cases, renders, score, knots, values)
    print("calibration strengths per 100:", calibration)
    tint_direction, tint = shadow_tint_table(renders, ramp)
    print("fitting the vignettes (a few minutes)...")
    vig = fit_vignettes(cases, renders, score, Frame(json.load(open(os.path.join(CORPUS, "layout.json")))), knots, values)
    print("vignettes:", vig)
    if "--write" not in sys.argv:
        return
    lines = [
        "// Generated by tools/camera_raw_fit.py from Camera Raw 18.7's renders of RAWmakase's synthetic chart",
        "// (tests/fixtures/camera-raw-parity, MIT, LICENSES/RAWmakase-MIT.txt). Do not edit; rerun the script.",
        "// docs/camera-raw.md#fitted-responses describes the fit.",
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
    for name, amount, out, _ in tables:
        lines.append("    {ToneSlider::%s, %g, {" % (name, amount))
        lines.append(numbers(out, 8, "        "))
        lines.append("    }},")
    lines.append("};")
    lines += [
        "",
        "// How much of each slider's curve works on the colour's luminance instead of its brightest and darkest channel,",
        "// per ToneSlider for lowering and raising it.",
        "constexpr double kSliderLuminance[][2] = {",
    ]
    lines += ["    {%g, %g},   // %s" % (luminance[i][0], luminance[i][1], n) for i, n in enumerate(SLIDERS.values())]
    lines += [
        "};",
        "",
        "// The grey ramp's encoded luminances, where Shadow Tint's push is measured.",
        "constexpr double kGradeLuma[] = {",
        numbers(luma),
        "};",
        "",
        "// Camera Calibration: per 100 of each primary's Hue and Saturation, how much of it moves into the other two channels",
        "// of scene-light ProPhoto RGB (rawtone.cpp's calibration matrix); Shadow Tint's direction and its push per 100 at",
        "// each kGradeLuma.",
        "constexpr double kCalibrationHue[3] = {%g, %g, %g};" % (calibration["RedHue"], calibration["GreenHue"], calibration["BlueHue"]),
        "constexpr double kCalibrationSaturation[3] = {%g, %g, %g};" % (calibration["RedSaturation"], calibration["GreenSaturation"],
                                                                         calibration["BlueSaturation"]),
        "constexpr double kShadowTintDirection[3] = {%.6f, %.6f, %.6f};" % tuple(tint_direction),
        "constexpr double kShadowTint[] = {",
        numbers(tint),
        "};",
        "",
        "// Post-crop vignette: the mask starts at start[0] + start[1] * Midpoint / 100 and spans span[0] + span[1] * Feather / 100",
        "// of the distance to the corners; negative Roundness shrinks that distance by `shrink` per unit. Paint Overlay scales",
        "// the encoded value; Highlight and Color Priority darken by up to `stops` in scene light, spared on bright colours by",
        "// `protect` * Highlights, and lighten toward white by `lighten` (Paint Overlay by `paintLighten`).",
        "constexpr double kVignetteStart[2] = {%g, %g};" % tuple(vig["start"]),
        "constexpr double kVignetteSpan[2] = {%g, %g};" % tuple(vig["span"]),
        "constexpr double kVignetteShrink = %g;" % vig["shrink"],
        "constexpr double kVignetteStops = %g;" % vig["stops"],
        "constexpr double kVignetteProtect = %g;" % vig["protect"],
        "constexpr double kVignetteLighten = %g;" % vig["lighten"],
        "constexpr double kVignettePaintLighten = %g;" % vig["paintLighten"],
        "",
        "// Lens vignetting (Optics): a gain in display light of 2^(amount / 100 * strength * r^power), r 1 at the corners,",
        "// strength for darkening and lightening, the power at Midpoint 20 and 50 (linear in Midpoint).",
        "constexpr double kLensVignetteStrength[2] = {%g, %g};" % vig["lens"][:2],
        "constexpr double kLensVignettePower[2] = {%g, %g};" % vig["lens"][2:],
    ]
    with open(OUT, "w") as f:
        f.write("\n".join(lines) + "\n")
    print("wrote", os.path.relpath(OUT, ROOT))


if __name__ == "__main__":
    main()
