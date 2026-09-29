#!/usr/bin/env python3
"""Smoke test for the automation socket: drives a running NekoPhoto
through a few calls and checks the answers. CI starts the app headless with
--demo first.

    nekophoto --headless --rpc-socket /tmp/c.sock --demo &
    python3 tools/rpc_smoke.py /tmp/c.sock
"""
import base64
import io
import json
import os
import socket
import struct
import sys
import tempfile
import time
import zlib


def pipe_name(path):
    r"""The named pipe the editor listens on for a --rpc-socket value on Windows (src/app/Platform.cpp's rule):
    a full \\.\pipe\ name as it is, anything else with its slashes and backslashes made underscores."""
    prefix = "\\\\.\\pipe\\"
    if path.lower().startswith(prefix):
        return path
    return prefix + path.replace("\\", "_").replace("/", "_")


def open_local_socket(path):
    """Connects to the editor's local socket: a Unix domain socket, or a named pipe on Windows.
    Returns (handle, text file); close both. Raises OSError while nothing is listening."""
    if os.name == "nt":
        raw = open(pipe_name(path), "r+b", buffering=0)
        return raw, io.TextIOWrapper(io.BufferedRWPair(raw, raw), encoding="utf-8", newline="\n")
    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    try:
        sock.connect(path)
    except OSError:
        sock.close()
        raise
    return sock, sock.makefile("rw", encoding="utf-8")


class Rpc:
    def __init__(self, path):
        self.sock, self.file = open_local_socket(path)
        self.next_id = 0
        self.events = []

    def call(self, method, **params):
        return self.request(method, params)

    def request(self, method, params):
        self.next_id += 1
        self.file.write(json.dumps({"jsonrpc": "2.0", "id": self.next_id, "method": method, "params": params}) + "\n")
        self.file.flush()
        reply = json.loads(self.file.readline())
        while reply.get("method") == "event":
            self.events.append(reply["params"])
            reply = json.loads(self.file.readline())
        if "error" in reply:
            raise RuntimeError(f"{method}: {reply['error']['message']}")
        return reply["result"]


def wait_for(path, seconds=30):
    deadline = time.time() + seconds
    while time.time() < deadline:
        try:
            return Rpc(path)
        except OSError:
            time.sleep(0.2)
    raise SystemExit(f"no socket at {path} after {seconds}s")


# ---- Photoshop preset files, built here byte by byte (the layouts in src/core/src/presets.cpp) ----------

def _ps_id(key):
    raw = key.encode()
    return struct.pack(">I", 0) + raw if len(raw) == 4 else struct.pack(">I", len(raw)) + raw


def _ps_text(value):
    units = value.encode("utf-16-be") + b"\0\0"
    return struct.pack(">I", len(units) // 2) + units


def _ps_value(value):
    """(type, payload) tuples: ("TEXT", str), ("enum", (type, value)), ("long", int), ("doub", float),
    ("UntF", (unit, float)), ("bool", bool), ("Objc", (class, items)), ("VlLs", [values])."""
    kind, v = value
    if kind == "TEXT":
        body = _ps_text(v)
    elif kind == "enum":
        body = _ps_id(v[0]) + _ps_id(v[1])
    elif kind == "long":
        body = struct.pack(">i", v)
    elif kind == "doub":
        body = struct.pack(">d", v)
    elif kind == "UntF":
        body = v[0].encode() + struct.pack(">d", v[1])
    elif kind == "bool":
        body = bytes([1 if v else 0])
    elif kind == "Objc":
        body = _ps_descriptor(*v)
    else:
        body = struct.pack(">I", len(v)) + b"".join(_ps_value(item) for item in v)
    return kind.encode() + body


def _ps_descriptor(cls, items):
    out = _ps_text("") + _ps_id(cls) + struct.pack(">I", len(items))
    for key, value in items:
        out += _ps_id(key) + _ps_value(value)
    return out


def _rgb(r, g, b):
    return ("Objc", ("RGBC", [("Rd  ", ("doub", r)), ("Grn ", ("doub", g)), ("Bl  ", ("doub", b))]))


def grd_file(gradients):
    """gradients: (name, [(location 0..1, (r, g, b) or "fg"/"bg")], [(location, opacity 0..1)])."""
    items = []
    for name, colors, alphas in gradients:
        stops = []
        for location, color in colors:
            fields = [] if isinstance(color, str) else [("Clr ", _rgb(*color))]
            fields += [("Type", ("enum", ("Clry", {"fg": "FrgC", "bg": "BckC"}.get(color, "UsrS") if isinstance(color, str) else "UsrS"))),
                       ("Lctn", ("long", round(location * 4096))), ("Mdpn", ("long", 50))]
            stops.append(("Objc", ("Clrt", fields)))
        trns = [("Objc", ("TrnS", [("Opct", ("UntF", ("#Prc", o * 100))), ("Lctn", ("long", round(l * 4096))), ("Mdpn", ("long", 50))])) for l, o in alphas]
        grad = ("Objc", ("Grdn", [("Nm  ", ("TEXT", name)), ("GrdF", ("enum", ("GrdF", "CstS"))), ("Intr", ("doub", 4096.0)),
                                  ("Clrs", ("VlLs", stops)), ("Trns", ("VlLs", trns))]))
        items.append(("Objc", ("Grdn", [("Grad", grad)])))
    return b"8BGR" + struct.pack(">HI", 5, 16) + _ps_descriptor("null", [("GrdL", ("VlLs", items))])


def pat_file(pattern_id, name, width, height, rgb):
    """One 8-bit RGB pattern, raw planes: rgb is a function (x, y) -> (r, g, b)."""
    planes = [bytes(rgb(x, y)[c] for y in range(height) for x in range(width)) for c in range(3)]
    slot = lambda data: struct.pack(">IIIIIIIHB", 1, 23 + len(data), 8, 0, 0, height, width, 8, 0) + data
    vma = struct.pack(">IIIII", 0, 0, height, width, 24) + b"".join(slot(p) for p in planes) + struct.pack(">I", 0) * 23
    record = struct.pack(">IIHH", 1, 3, height, width) + _ps_text(name) + bytes([len(pattern_id)]) + pattern_id.encode()
    record += struct.pack(">II", 3, len(vma)) + vma
    return b"8BPT" + struct.pack(">HI", 1, 1) + record


def asl_file(name, style_id):
    """One style: a red drop shadow (no patterns)."""
    shadow = ("Objc", ("DrSh", [("enab", ("bool", True)), ("Md  ", ("enum", ("BlnM", "Mltp"))), ("Clr ", _rgb(255, 0, 0)),
                                ("Opct", ("UntF", ("#Prc", 75.0))), ("uglg", ("bool", False)), ("lagl", ("UntF", ("#Ang", 90.0))),
                                ("Dstn", ("UntF", ("#Pxl", 7.0))), ("Ckmt", ("UntF", ("#Pxl", 0.0))), ("blur", ("UntF", ("#Pxl", 3.0)))]))
    lefx = ("Objc", ("Lefx", [("Scl ", ("UntF", ("#Prc", 100.0))), ("masterFXSwitch", ("bool", True)), ("DrSh", shadow)]))
    record = struct.pack(">I", 16) + _ps_descriptor("null", [("Nm  ", ("TEXT", name)), ("Idnt", ("TEXT", style_id))])
    record += struct.pack(">I", 16) + _ps_descriptor("Styl", [("Lefx", lefx)])
    record += b"\0" * (-len(record) % 4)
    return struct.pack(">H", 2) + b"8BSL" + struct.pack(">HI", 3, 0) + struct.pack(">I", 1) + struct.pack(">I", len(record)) + record


def presets(rpc, work, layer_id):
    """presets.import / list / remove, layers.applyStyle and gradient.draw with a preset. The names carry a
    smoke-test prefix and are removed again, since the library lives in the person's data folder."""
    paths = {"grd": os.path.join(work, "smoke.grd"), "pat": os.path.join(work, "smoke.pat"), "asl": os.path.join(work, "smoke.asl")}
    with open(paths["grd"], "wb") as f:
        f.write(grd_file([("rpc-smoke RGB", [(0, (255, 0, 0)), (0.5, (0, 255, 0)), (1, (0, 0, 255))], [(0, 1), (1, 1)]),
                          ("rpc-smoke Fade", [(0, "fg"), (1, "bg")], [(0, 1), (1, 0)])]))
    with open(paths["pat"], "wb") as f:
        f.write(pat_file("rpc-smoke-pattern", "rpc-smoke checks", 8, 8, lambda x, y: (255, 255, 255) if (x // 4 + y // 4) % 2 else (0, 0, 0)))
    with open(paths["asl"], "wb") as f:
        f.write(asl_file("rpc-smoke Shadow", "rpc-smoke-style"))
    imported = rpc.call("presets.import", paths=list(paths.values()))
    assert imported["gradients"] == ["rpc-smoke RGB", "rpc-smoke Fade"], imported
    assert imported["patterns"] == ["rpc-smoke checks"] and imported["patternsAddedToDocument"] == 1, imported
    assert imported["styles"] == ["rpc-smoke Shadow"], imported
    listed = rpc.call("presets.list")
    rgb = [g for g in listed["gradients"] if g["name"] == "rpc-smoke RGB"][0]
    assert [c["color"] for c in rgb["colors"]] == ["#ff0000", "#00ff00", "#0000ff"], rgb
    assert any(p["id"] == "rpc-smoke-pattern" and p["width"] == 8 for p in listed["patterns"]), listed["patterns"]
    styled = rpc.call("layers.applyStyle", id=layer_id, style="rpc-smoke Shadow")
    assert styled["dropShadows"][0]["color"] == "#ff0000" and styled["dropShadows"][0]["distance"] == 7, styled
    assert rpc.call("history.info")["undo"] == "Apply Style"
    # A pattern overlay can now use the imported pattern.
    patterned = rpc.call("layers.setStyle", id=layer_id, style={"patternOverlays": [{"pattern": "rpc-smoke-pattern"}]})
    assert patterned["patternOverlays"][0]["pattern"] == "rpc-smoke-pattern", patterned
    rpc.call("render", maxSize=64)
    rpc.call("layers.select", id=layer_id)
    rpc.call("selection.none")
    rpc.call("gradient.draw", x0=0, y0=0, x1=200, y1=0, preset="rpc-smoke RGB")
    try:
        rpc.call("gradient.draw", x0=0, y0=0, x1=10, y1=0, preset="rpc-smoke no such gradient")
        raise AssertionError("an unknown gradient preset should be refused")
    except RuntimeError as e:
        print("expected error:", e)
    for kind, name in (("style", "rpc-smoke Shadow"), ("gradient", "rpc-smoke RGB"), ("gradient", "rpc-smoke Fade"), ("pattern", "rpc-smoke-pattern")):
        assert rpc.call("presets.remove", kind=kind, name=name)["removed"] == name
    assert not any(g["name"].startswith("rpc-smoke") for g in rpc.call("presets.list", kind="gradients")["gradients"])


def remaining_methods(rpc):
    """Every method the checks above do not reach, in a tab of its own that is closed afterwards."""
    work = tempfile.mkdtemp()
    first = rpc.call("tabs.list")
    tab = rpc.call("tabs.new")
    rpc.call("document.new", width=200, height=120)
    rpc.call("shape.draw", kind="ellipse", x=20, y=20, width=160, height=80, color="#aa3355")
    rpc.call("render", maxSize=64)
    image = os.path.join(work, "flat.png")
    rpc.call("document.export", path=image)
    # TGA and ICO through the core's writers and readers: the TGA comes back as a layer, the icon in a tab of its own.
    tga, ico = os.path.join(work, "flat.tga"), os.path.join(work, "flat.ico")
    assert rpc.call("document.export", path=tga)["width"] == 200
    rpc.call("document.export", path=ico)
    rpc.call("document.import", path=tga)
    rpc.call("history.undo")
    here = rpc.call("tabs.list")
    icon = rpc.call("document.open", path=ico)
    assert (icon["width"], icon["layers"]) == (256, 4), icon
    rpc.call("tabs.close", index=icon["tab"], discard=True)
    rpc.call("tabs.select", index=next(t["index"] for t in here if t["current"]))
    placed = rpc.call("document.import", path=image, x=100, y=60)
    rpc.call("layers.duplicate")
    copy = rpc.call("layers.list")[0]
    rpc.call("layers.flip", vertical=True)
    rpc.call("layers.move", id=copy["id"], atBottom=True)
    rpc.call("layers.reorder", id=copy["id"], offset=1)
    rpc.call("layers.render", id=placed["id"], maxSize=32)
    rpc.call("layers.setTransform", id=placed["id"], x=10, y=5, rotation=15)
    styled = rpc.call("layers.setStyle", id=placed["id"], style={"dropShadows": [{"distance": 6, "size": 4}], "strokes": [{"size": 2, "color": "#ff0000"}]})
    assert styled["dropShadows"][0]["distance"] == 6 and styled["strokes"][0]["color"] == "#ff0000", styled
    assert rpc.call("layers.style", id=placed["id"])["strokes"][0]["size"] == 2
    try:
        rpc.call("layers.setStyle", id=placed["id"], style={"strokes": [{"width": 2}]})
        raise AssertionError("an unknown effect setting should be refused")
    except RuntimeError as e:
        print("expected error:", e)
    assert "strokes" not in rpc.call("layers.setStyle", id=placed["id"], style={})
    presets(rpc, work, placed["id"])
    rpc.call("selection.fromLayer", id=placed["id"])
    try:   # needs the downloaded model; without it, a clear error
        rpc.call("pixels.removeBackground")
        rpc.call("history.undo")
    except RuntimeError as e:
        print("removeBackground:", e)
    rpc.call("layers.mask", id=placed["id"], action="add")
    rpc.call("layers.mask", id=placed["id"], action="invert")
    rpc.call("layers.mask", id=placed["id"], action="delete")
    rpc.call("layers.select", id=placed["id"])
    rpc.call("pixels.adjust", kind="Exposure", settings={})
    rpc.call("pixels.invert")
    assert rpc.call("adjustments.defaults", kind="Levels")
    rpc.call("layers.add", kind="adjustment", adjustmentKind="Exposure")
    rpc.call("adjustments.set", settings={})
    rpc.call("layers.select", id=placed["id"])
    before = rpc.call("history.list")["undo"]
    for _ in range(3):
        rpc.call("selection.all")
    after = rpc.call("history.list")["undo"]
    # Select All over a Select All changes nothing: one step, not three.
    assert before[-1] != "Select All" and after[-1] == "Select All" and after[-2] == before[-1], (before[-3:], after[-3:])
    assert rpc.call("selection.info")
    rpc.call("selection.invert")
    rpc.call("selection.invert")
    rpc.call("selection.feather", radius=2)
    rpc.call("selection.grow", amount=2)
    rpc.call("selection.smooth", radius=2)
    rpc.call("selection.border", width=3)
    rpc.call("selection.wand", x=100, y=60, tolerance=20)
    rpc.call("selection.polygon", points=[[10, 10], [90, 10], [50, 80]])
    rpc.call("pixels.clear")
    rpc.call("selection.none")
    rpc.call("selection.rect", x=10, y=10, width=10, height=10)
    assert rpc.call("selection.quickMask", on=True)["quickMask"]
    rpc.call("brush.stroke", points=[[40, 60], [90, 60]], size=16, mask=True)
    assert not rpc.call("selection.quickMask", on=False)["quickMask"]
    grown = rpc.call("selection.info")
    assert grown["active"] and grown["bounds"]["width"] > 60, grown   # painting white in Quick Mask selects
    rpc.call("selection.none")
    # Smoothing: the stabiliser with its modes, input and pressure smoothing, each one undo step.
    smoothed = rpc.call("brush.stroke", points=[[20, 20], [40, 22], [60, 19], [80, 21]], size=6, smoothing=50, pulledString=False,
                        strokeCatchUp=True, catchUpOnEnd=True, adjustForZoom=True, inputSmoothing=20, pressureSmoothing=30, pressures=[0.2, 0.9, 0.3, 0.8])
    assert smoothed.get("smoothed"), smoothed
    assert rpc.call("history.info")["undo"] == "Brush Stroke"
    assert not rpc.call("brush.stroke", points=[[20, 30], [80, 30]], size=6, smoothing=0).get("smoothed")
    rpc.call("layers.group")
    # Folders take Photoshop's modes: Pass Through by default, a blend mode isolates, opacity fades.
    listed = rpc.call("layers.list")
    folder = next(l for l in (listed["layers"] if isinstance(listed, dict) else listed) if l.get("kind") == "group")
    assert folder["blend"] == "Pass Through", folder
    rpc.call("layers.set", id=folder["id"], blend="Multiply", opacity=0.5)
    folder = rpc.call("layers.get", id=folder["id"])
    assert folder["blend"] == "Multiply" and abs(folder["opacity"] - 0.5) < 1e-6, folder
    rpc.call("layers.set", id=folder["id"], blend="Pass Through")
    assert rpc.call("layers.get", id=folder["id"])["blend"] == "Pass Through"
    for _ in range(3):   # the three folder changes (opacity, blend, Pass Through)
        rpc.call("history.undo")
    rpc.call("history.undo")
    rpc.call("history.redo")
    rpc.call("history.undo")
    rpc.call("layers.select", id=placed["id"])
    rpc.call("layers.merge", down=True)
    # Artboards and slices: made, changed, exported, through a PSD and back, removed.
    board = rpc.call("artboards.add", x=10, y=10, width=80, height=60, background="#ff0000", name="Hero")
    moved = rpc.call("artboards.set", id=board["id"], x=20, moveContents=True)
    assert moved["x"] == 20 and moved["background"] == "#ff0000", moved
    assert rpc.call("history.list")["undo"][-1] == "Move Artboard"   # one step, contents and all
    assert [a["name"] for a in rpc.call("artboards.list")["artboards"]] == ["Hero"]
    assert rpc.call("layers.get", id=board["id"])["artboard"]["width"] == 80
    piece = rpc.call("slices.add", x=0, y=0, width=50, height=40, name="top")
    rpc.call("slices.set", id=piece["id"], altTag="Top")
    assert rpc.call("slices.list")["slices"][0]["altTag"] == "Top"
    written = rpc.call("artboards.export", directory=os.path.join(work, "boards"))["files"]
    assert len(written) == 1 and os.path.getsize(written[0]) > 0, written
    written = rpc.call("slices.export", directory=os.path.join(work, "slices"), format="jpeg", prefix="p_")["files"]
    assert len(written) == 1 and written[0].endswith("p_top.jpg"), written
    boards_psd = os.path.join(work, "boards.psd")
    rpc.call("document.export", path=boards_psd)
    here = rpc.call("tabs.list")
    reopened = rpc.call("document.open", path=boards_psd)
    assert [a["name"] for a in rpc.call("artboards.list")["artboards"]] == ["Hero"]
    assert [s["name"] for s in rpc.call("slices.list")["slices"]] == ["top"]
    rpc.call("tabs.close", index=reopened["tab"], discard=True)
    rpc.call("tabs.select", index=next(t["index"] for t in here if t["current"]))
    rpc.call("slices.delete", id=piece["id"])
    rpc.call("artboards.delete", id=board["id"])
    assert rpc.call("artboards.list")["artboards"] == [] and rpc.call("slices.list")["slices"] == []
    for name in ("artboard", "slice"):
        rpc.call("tool.select", name=name)
    rpc.call("canvas.flip", vertical=False)
    rpc.call("canvas.resize", width=220, height=140)
    rpc.call("canvas.crop", x=0, y=0, width=200, height=120)
    trimmed = rpc.call("image.trim", basedOn="topLeft", tolerance=4)
    assert "trimmed" in trimmed, trimmed
    if trimmed["trimmed"]:
        rpc.call("history.undo")
    rpc.call("image.resize", scale=0.5)
    for name in ("quickselect", "text", "brush"):
        rpc.call("tool.select", name=name)
    rpc.call("colors.set", foreground="#102030", background="#ffffff")
    rpc.call("view.zoom", zoom=1)
    rpc.call("screenshot", maxSize=64)
    project = os.path.join(work, "Coverage.comp")
    rpc.call("document.save", path=project)
    rpc.call("document.close", discard=True)
    rpc.call("document.open", path=project)
    assert rpc.call("document.info")["width"] == 100
    rpc.call("tabs.select", index=next(t["index"] for t in first if t["current"]))
    rpc.call("tabs.close", index=tab["index"], discard=True)


def sixteen_bit(rpc):
    """Image > Mode > 16 Bits/Channel (docs/bit-depth.md): what works on a 16-bit document, what is refused with the
    reason, and the files it writes; in a tab of its own that is closed afterwards."""
    work = tempfile.mkdtemp()
    first = rpc.call("tabs.list")
    tab = rpc.call("tabs.new")
    rpc.call("document.new", width=120, height=80)
    rpc.call("shape.draw", kind="ellipse", x=10, y=10, width=100, height=60, color="#3366aa")
    assert rpc.call("document.info")["bits"] == 8
    converted = rpc.call("image.mode", bits=16)
    assert converted["bits"] == 16 and converted["layerPixelBudget"] == 500000000, converted
    assert rpc.call("document.info")["bits"] == 16
    assert rpc.call("history.info")["undo"] == "Convert Mode"
    # The layer structure, masks and transforms work at 16 bits.
    layer = rpc.call("layers.list")[0]
    rpc.call("layers.set", id=layer["id"], opacity=0.75, blend="Multiply")
    rpc.call("layers.duplicate", id=layer["id"])
    rpc.call("layers.mask", id=layer["id"], action="add", revealing=False)
    rpc.call("layers.mask", id=layer["id"], action="invert")
    rpc.call("layers.mask", id=layer["id"], action="toggle")
    rpc.call("layers.setTransform", id=layer["id"], x=4, y=2)
    rpc.call("canvas.flip", vertical=True)
    rpc.call("layers.add", kind="group")
    shot = rpc.call("render", maxSize=64)
    assert base64.b64decode(shot["png"])[:8] == b"\x89PNG\r\n\x1a\n"
    # Selections, fills, adjustments, filters and whole-image edits work at 16 bits (P3a).
    rpc.call("layers.select", id=layer["id"])
    rpc.call("selection.rect", x=20, y=15, width=60, height=40)
    rpc.call("selection.feather", radius=3)
    rpc.call("pixels.adjust", kind="Levels", settings={"ranges": [{"black": 10, "gamma": 1.3, "white": 240, "outputBlack": 0, "outputWhite": 255}]})
    rpc.call("pixels.filter", kind="Gaussian Blur", radius=2)
    assert rpc.call("pixels.mosh", effect="vhs", seed=3)["applied"] == "vhs"
    # Camera Raw at 16 bits, White Balance > Auto included.
    raw = rpc.call("pixels.cameraRaw", settings={"exposure": 0.4, "clarity": 20, "whiteBalance": "Auto", "detail": {"sharpenAmount": 30}})
    assert raw["applied"] and rpc.call("history.info")["undo"] == "Camera Raw Filter", raw
    # G'MIC at 16 bits, when it is installed: the pixels go to it as float and come back at 16 bits.
    if rpc.call("gmic.filters", search="sharpen")["installed"]:
        assert rpc.call("pixels.gmic", command="blur 1.5")["applied"] == "blur 1.5"
        assert rpc.call("history.info")["undo"] == "G'MIC: blur"
    rpc.call("pixels.fill", color="#ffaa00")
    rpc.call("selection.rect", x=40, y=30, width=12, height=10)
    rpc.call("pixels.contentAwareFill")
    rpc.call("selection.none")
    rpc.call("layers.add", kind="adjustment", adjustmentKind="Hue/Saturation")
    assert rpc.call("image.resize", width=150, height=100)["width"] == 150
    assert rpc.call("canvas.crop", x=5, y=5, width=130, height=90)["width"] == 130
    assert rpc.call("document.info")["bits"] == 16
    # Painting and retouching work at 16 bits (P3b): the brush (round and MyPaint), the eraser, a mask, clone, the
    # healers, smudge, blur, dodge, gradients, the bucket and Patch, each one undo step.
    rpc.call("layers.select", id=layer["id"])
    painting = [
        ("Brush Stroke", "brush.stroke", {"points": [[20, 20], [60, 40], [100, 30]], "size": 10, "color": "#ff0000"}),
        ("Eraser", "brush.stroke", {"tool": "eraser", "points": [[30, 50], [80, 50]], "size": 8}),
        ("Clone Stamp", "brush.stroke", {"tool": "clone", "source": {"x": 30, "y": 30}, "points": [[70, 50], [90, 55]], "size": 12}),
        ("Spot Healing", "brush.stroke", {"tool": "healing", "points": [[50, 40], [55, 42]], "size": 10}),
        ("Healing Brush", "brush.stroke", {"tool": "healingbrush", "source": {"x": 30, "y": 30}, "points": [[60, 30], [70, 32]], "size": 10}),
        ("Smudge", "brush.stroke", {"tool": "smudge", "points": [[20, 60], [50, 62]], "size": 12}),
        ("Blur", "brush.stroke", {"tool": "blur", "points": [[40, 20], [80, 22]], "size": 14}),
        ("Burn", "brush.stroke", {"tool": "burn", "points": [[30, 30], [90, 40]], "size": 14, "range": "shadows"}),
        ("Gradient", "gradient.draw", {"x0": 0, "y0": 0, "x1": 120, "y1": 0, "foreground": "#102030", "background": "#f0e0d0",
                                       "style": "foreground-to-background", "opacity": 0.5}),
        ("Paint Bucket", "pixels.bucket", {"x": 5, "y": 5, "color": "#00ff00", "tolerance": 10}),
    ]
    for name, method, params in painting:
        rpc.call(method, **params)
        assert rpc.call("history.info")["undo"] == name, (name, rpc.call("history.info"))
    rpc.call("selection.rect", x=10, y=10, width=20, height=20)
    assert rpc.call("pixels.patch", dx=40, dy=0)["patched"]
    assert rpc.call("history.info")["undo"] == "Patch"
    rpc.call("selection.none")
    rpc.call("brush.stroke", points=[[20, 20], [90, 60]], size=12, mask=True)
    assert rpc.call("history.info")["undo"] == "Paint Mask"
    rpc.call("layers.select", id=layer["id"], mask=True)
    rpc.call("gradient.draw", x0=0, y0=0, x1=0, y1=80)
    assert rpc.call("history.info")["undo"] == "Gradient Mask"
    rpc.call("layers.select", id=layer["id"])
    # Quick Mask painting: white selects, at 16 bits.
    rpc.call("selection.rect", x=10, y=10, width=10, height=10)
    assert rpc.call("selection.quickMask", on=True)["quickMask"]
    rpc.call("brush.stroke", points=[[40, 60], [90, 60]], size=16, mask=True)
    assert not rpc.call("selection.quickMask", on=False)["quickMask"]
    grown = rpc.call("selection.info")
    assert grown["active"] and grown["bounds"]["width"] > 60, grown
    rpc.call("selection.none")
    rpc.call("layers.select", id=layer["id"])
    if rpc.call("brush.presets")["supported"]:
        assert rpc.call("brush.stroke", points=[[30, 40], [100, 45]], preset="classic/pencil", pressures=[0.3, 0.9], color="#000000")["preset"] == "classic/pencil"
    # Apply Mask and Merge Down at 16 bits.
    rpc.call("layers.mask", id=layer["id"], action="apply")
    assert rpc.call("history.info")["undo"] == "Apply Layer Mask"
    above = rpc.call("layers.add")
    rpc.call("brush.stroke", points=[[10, 70], [110, 70]], size=6, color="#224466")
    rpc.call("layers.select", id=above["id"])
    count = len(rpc.call("layers.list"))
    rpc.call("layers.merge")
    assert len(rpc.call("layers.list")) == count - 1
    assert rpc.call("document.info")["bits"] == 16
    # Text at 16 bits: the tool, rich text ranges, editing, and its outlines as a path and a shape.
    assert rpc.call("tool.select", name="text")["tool"] == "text"
    words = rpc.call("layers.add", kind="text", text="Deep", x=10, y=10, size=30, color="#2266cc")
    assert words["kind"] == "text" and words["pixelSize"]["width"] > 20, words
    assert rpc.call("text.set", id=words["id"], text="Deep text", italic=True)["text"]["text"] == "Deep text"
    ranged = rpc.call("text.styleRange", id=words["id"], start=5, length=4, color="#ff0000", size=36)
    assert [r["length"] for r in ranged["text"]["runs"]] == [5, 4], ranged
    assert rpc.call("text.toPath", id=words["id"])["subpaths"] >= 4
    assert rpc.call("text.toShape", id=words["id"])["kind"] == "shape"
    # Shapes, paths and vector masks at 16 bits: a gradient shape with a stroke, a path operation, Fill and Stroke Path.
    assert rpc.call("tool.select", name="shape")["tool"] == "shape"
    box = rpc.call("shape.draw", kind="rectangle", x=15, y=15, width=70, height=40, cornerRadius=5, color="#224488", fillType="gradient",
                   strokeWidth=2, strokeColor="#000000")
    assert box["kind"] == "shape", box
    rpc.call("layers.select", id=box["id"])
    rpc.call("shape.draw", kind="ellipse", x=30, y=20, width=20, height=20, op="subtract")
    assert len(rpc.call("shape.get", id=box["id"])["path"]) == 2
    path = rpc.call("paths.set", name="Deep path", path=[{"knots": [[5, 5], [45, 5], [25, 35]]}])
    pixels = rpc.call("layers.add", kind="pixels", name="Deep paths")
    rpc.call("paths.fill", id=path["id"])
    assert rpc.call("history.info")["undo"] == "Fill Path"
    rpc.call("paths.stroke", id=path["id"])
    assert rpc.call("history.info")["undo"] == "Stroke Path"
    square = rpc.call("vectorMask.set", id=pixels["id"], path=[{"knots": [[10, 10], [60, 10], [60, 60], [10, 60]]}])
    assert len(square["path"]) == 1, square
    # Layer styles at 16 bits: every effect on the shape layer, a folder style, and back to none.
    styled = rpc.call("layers.setStyle", id=box["id"], style={"dropShadows": [{"distance": 4, "size": 3}], "strokes": [{"size": 2, "color": "#ff0000"}],
                                                             "outerGlows": [{"size": 5}], "innerShadows": [{"size": 3}], "innerGlows": [{"size": 4}],
                                                             "bevels": [{"size": 5}], "satins": [{"size": 6}], "colorOverlays": [{"color": "#00ff00", "opacity": 0.3}],
                                                             "gradientOverlays": [{"opacity": 0.5}]})
    assert "strokes" in styled and "bevels" in styled, styled
    folder = rpc.call("layers.add", kind="group")
    assert "outerGlows" in rpc.call("layers.setStyle", id=folder["id"], style={"outerGlows": [{"size": 6}]})
    shot = rpc.call("render", maxSize=64)
    assert base64.b64decode(shot["png"])[:8] == b"\x89PNG\r\n\x1a\n"
    assert "strokes" not in rpc.call("layers.setStyle", id=box["id"], style={})
    assert rpc.call("document.info")["bits"] == 16
    # Smart objects at 16 bits: sources keep their own depth (an 8-bit tile, a 16-bit PNG), instances are drawn at the
    # document's; convert (a 16-bit PSB), edit the contents (a 16-bit tab), Smart Filters, warp, replace, rasterize.
    tile = os.path.join(work, "tile8.png")
    rpc.call("render", region={"x": 0, "y": 0, "width": 16, "height": 16}, maxSize=0, path=tile)
    deep_tile = os.path.join(work, "tile16.png")
    assert rpc.call("document.export", path=deep_tile)["bits"] == 16
    placed8 = rpc.call("smartObject.place", path=tile)
    assert placed8["kind"] == "smartObject" and not placed8["smartObject"]["locked"], placed8
    placed16 = rpc.call("smartObject.place", path=deep_tile)
    assert placed16["kind"] == "smartObject" and not placed16["smartObject"]["locked"], placed16
    assert rpc.call("history.info")["undo"] == "Place Embedded"
    converted16 = rpc.call("smartObject.convert", ids=[placed8["id"]])
    assert converted16["kind"] == "smartObject", converted16
    opened16 = rpc.call("smartObject.editContents", id=converted16["id"])
    assert rpc.call("document.info")["bits"] == 16, "a 16-bit document's layers convert to a 16-bit PSB"
    rpc.call("layers.add", kind="pixels", name="Inside")
    assert rpc.call("smartObject.commit")["committed"]
    rpc.call("tabs.close", index=opened16["tab"])
    assert rpc.call("layers.get", id=converted16["id"])["kind"] == "smartObject"
    fx = rpc.call("smartObject.addFilter", id=converted16["id"], kind="gaussian blur", radius=2)
    assert fx["kind"] == "smartObject", fx
    rpc.call("smartObject.addFilter", id=converted16["id"], kind="mosaic", cellSize=4)
    fx = rpc.call("smartObject.setFilter", id=converted16["id"], index=0, radius=3, opacity=60, blend="screen")
    assert fx["filters"][0]["opacity"] == 60, fx
    assert rpc.call("smartObject.filterMask", id=converted16["id"], action="invert")["mask"]["outside"] == 0
    # The filter mask painted at 16 bits, kept in the stack.
    assert rpc.call("smartObject.filterMask", id=converted16["id"], action="select")["mask"]["painting"]
    rpc.call("brush.stroke", points=[[2, 2], [40, 30]], size=10, color="#ffffff", mask=True)
    rpc.call("smartObject.filterMask", id=converted16["id"], action="deselect")
    assert rpc.call("smartObject.filters", id=converted16["id"])["mask"]["maskedPixels"] > 0
    rpc.call("layers.setTransform", id=converted16["id"], x=12, y=9)
    assert rpc.call("layers.get", id=converted16["id"])["kind"] == "smartObject"
    # Unsharp Mask as a 16-bit Smart Filter.
    usm = rpc.call("smartObject.addFilter", id=converted16["id"], kind="unsharp mask")
    assert any("Unsharp" in f["name"] for f in usm["filters"]), usm
    warped = rpc.call("layers.warp", id=placed16["id"], style="arc", bend=30)
    assert warped["kind"] == "smartObject", warped
    rpc.call("smartObject.replace", id=placed16["id"], path=tile)
    assert rpc.call("layers.get", id=placed16["id"])["kind"] == "smartObject"
    assert rpc.call("smartObject.rasterize", id=placed16["id"])["kind"] == "pixels"
    assert rpc.call("document.info")["bits"] == 16
    shot = rpc.call("render", maxSize=64)
    assert base64.b64decode(shot["png"])[:8] == b"\x89PNG\r\n\x1a\n"
    # What is not ported yet is refused, saying so.
    for method, params in (("tool.select", {"name": "artboard"}),):
        try:
            rpc.call(method, **params)
            raise AssertionError(method + " should be refused on a 16-bit document")
        except RuntimeError as e:
            assert "not available for 16-bit documents yet" in str(e), e
    # PNG at 16 bits; an 8-bit format dithered down, with a note.
    png = os.path.join(work, "deep.png")
    assert rpc.call("document.export", path=png)["bits"] == 16
    with open(png, "rb") as f:
        assert f.read(25)[24] == 16, "a 16-bit PNG"
    jpeg = rpc.call("document.export", path=os.path.join(work, "flat.jpg"))
    assert jpeg["bits"] == 8 and "dithering" in jpeg["note"], jpeg
    psd = os.path.join(work, "deep.psd")
    rpc.call("document.export", path=psd)
    with open(psd, "rb") as f:
        assert f.read(24)[22:24] == b"\x00\x10", "a 16-bit PSD"
    # A 16-bit project stays 16-bit.
    project = os.path.join(work, "Deep.comp")
    rpc.call("document.save", path=project)
    rpc.call("document.close", discard=True)
    rpc.call("document.open", path=project)
    assert rpc.call("document.info")["bits"] == 16
    assert any(l["kind"] == "smartObject" for l in rpc.call("layers.list")), "the smart object survives the 16-bit project"
    # Back to 8 bits, and Undo takes the conversion back in one step.
    assert rpc.call("image.mode", bits=8)["bits"] == 8
    rpc.call("history.undo")
    assert rpc.call("document.info")["bits"] == 16
    try:
        rpc.call("image.mode", bits=32)
        raise AssertionError("32-bit documents are not available yet")
    except RuntimeError as e:
        print("expected error:", e)
    rpc.call("document.close", discard=True)
    rpc.call("tabs.select", index=next(t["index"] for t in first if t["current"]))
    rpc.call("tabs.close", index=tab["index"], discard=True)


def colour_management(rpc):
    """Colour management (docs/color-management.md): Color Settings, Assign and Convert to Profile, the profile in
    exports, PSDs and projects, soft proofing; in a tab of its own that is closed afterwards."""
    work = tempfile.mkdtemp()
    first = rpc.call("tabs.list")
    tab = rpc.call("tabs.new")
    settings = rpc.call("color.settings")
    assert settings["workingSpace"] == "srgb" and settings["policy"] == "preserve" and settings["untagged"] == "srgb", settings
    assert settings["monitor"] is None, "no monitor profile in a headless run"
    rpc.call("document.new", width=64, height=48)
    rpc.call("shape.draw", kind="rectangle", x=0, y=0, width=64, height=48, color="#ff0000")
    got = rpc.call("document.profile")
    assert not got["tagged"] and got["workingSpace"] == "srgb", got
    before = rpc.call("render", maxSize=64)["png"]
    # Assign: the tag only (with no monitor profile the canvas shows the same values), one undo step.
    assigned = rpc.call("document.profile", action="assign", profile="adobe-rgb")
    assert assigned["tagged"] and assigned["profile"] == "Adobe RGB (1998)" and assigned["undo"] == "Assign Profile", assigned
    assert rpc.call("render", maxSize=64)["png"] == before
    assert rpc.call("document.info")["profile"] == "Adobe RGB (1998)"
    rpc.call("history.undo")
    assert not rpc.call("document.profile")["tagged"]
    # Convert: the pixels change so the colours look the same (sRGB red is about 219, 0, 0 in Adobe RGB).
    converted = rpc.call("document.profile", action="convert", profile="adobe-rgb", intent="perceptual", blackPointCompensation=True)
    assert converted["undo"] == "Convert to Profile" and converted["workingSpace"] == "adobe-rgb", converted
    assert rpc.call("render", maxSize=64)["png"] != before
    # Exports embed the profile, or convert to sRGB for the web.
    png = os.path.join(work, "tagged.png")
    assert rpc.call("document.export", path=png)["profile"] == "Adobe RGB (1998)"
    with open(png, "rb") as f:
        assert b"iCCP" in f.read(), "PNG embeds the profile"
    jpg = os.path.join(work, "tagged.jpg")
    rpc.call("document.export", path=jpg)
    with open(jpg, "rb") as f:
        assert b"ICC_PROFILE" in f.read(), "JPEG embeds the profile"
    web = rpc.call("document.export", path=os.path.join(work, "web.png"), convertToSrgb=True)
    assert web["convertedToSrgb"] and web["profile"] is None, web
    with open(os.path.join(work, "web.png"), "rb") as f:
        assert b"iCCP" not in f.read()
    rpc.call("document.export", path=os.path.join(work, "web.gif"))
    # PSD (resource 1039) and the project keep it.
    psd = os.path.join(work, "tagged.psd")
    rpc.call("document.export", path=psd)
    project = os.path.join(work, "Tagged.comp")
    rpc.call("document.save", path=project)
    rpc.call("document.close", discard=True)
    rpc.call("document.open", path=project)
    assert rpc.call("document.profile")["profile"] == "Adobe RGB (1998)"
    rpc.call("document.close", discard=True)
    rpc.call("document.open", path=psd)
    assert rpc.call("document.profile")["profile"] == "Adobe RGB (1998)"
    rpc.call("document.close", discard=True)
    # An image with a profile opens with it (Preserve); into a document it is converted to the document's.
    rpc.call("document.open", path=png)
    assert rpc.call("document.profile")["profile"] == "Adobe RGB (1998)"
    rpc.call("document.close", discard=True)
    # Proof Colors and the gamut warning draw the canvas through the proofing profile: ProPhoto's pure green is far
    # outside sRGB, so the warning colour covers it.
    rpc.call("document.new", width=32, height=32)
    rpc.call("document.profile", action="assign", profile="prophoto")
    rpc.call("shape.draw", kind="rectangle", x=0, y=0, width=32, height=32, color="#00ff00")
    plain = rpc.call("screenshot", maxSize=64)["png"]
    proof = rpc.call("color.settings", proofProfile="srgb", proofIntent="relative", proofColors=True, gamutWarning=True, gamutColor="#ff00ff")
    assert proof["proofColors"] and proof["gamutWarning"] and proof["gamutColor"] == "#ff00ff", proof
    assert rpc.call("screenshot", maxSize=64)["png"] != plain, "the gamut warning shows on the canvas"
    rpc.call("color.settings", proofColors=False, gamutWarning=False)
    assert rpc.call("screenshot", maxSize=64)["png"] == plain
    rpc.call("document.close", discard=True)
    rpc.call("document.open", path=project)
    # At 16 bits too.
    rpc.call("image.mode", bits=16)
    assert rpc.call("document.profile", action="convert", profile="prophoto")["profile"] == "ProPhoto RGB"
    rpc.call("document.close", discard=True)
    # New documents take the working space.
    rpc.call("color.settings", workingSpace="display-p3", policy="convert")
    rpc.call("document.new", width=16, height=16)
    assert rpc.call("document.profile")["profile"] == "Display P3"
    rpc.call("document.close", discard=True)
    rpc.call("color.settings", workingSpace="srgb", policy="preserve")
    for bad in ({"action": "assign", "profile": "cmyk"}, {"action": "convert"}, {"action": "get", "profile": "srgb"}):
        try:
            rpc.call("document.new", width=8, height=8)
            rpc.call("document.profile", **bad)
            raise AssertionError(f"document.profile {bad} should be refused")
        except RuntimeError as e:
            print("expected error:", e)
        finally:
            rpc.call("document.close", discard=True)
    rpc.call("tabs.select", index=next(t["index"] for t in first if t["current"]))
    rpc.call("tabs.close", index=tab["index"], discard=True)


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else None
    if not path:
        raise SystemExit("usage: rpc_smoke.py <socket path>")
    rpc = wait_for(path)
    info = rpc.call("app.info")
    assert info["name"] == "nekophoto", info
    print("version", info["version"], "platform", info["platform"])

    doc = rpc.call("document.info")
    assert doc["width"] > 0 and doc["layers"] > 0, doc
    layers = rpc.call("layers.list")
    print(f"document {doc['width']}x{doc['height']}, {len(layers)} layers")
    pixel_layers = [l for l in layers if l["kind"] in ("pixels", "shape") and not l.get("pixelSize", {}).get("blank", True)]
    assert pixel_layers, layers

    # Observe: a render comes back as PNG.
    shot = rpc.call("render", maxSize=256)
    png = base64.b64decode(shot["png"])
    assert png[:8] == b"\x89PNG\r\n\x1a\n", png[:8]
    assert max(shot["width"], shot["height"]) == 256, shot
    print("render", shot["width"], "x", shot["height"], len(png), "bytes")

    # Edit and undo: a property change is one history step.
    target = pixel_layers[0]
    before = target["opacity"]
    changed = rpc.call("layers.set", id=target["id"], opacity=0.5)
    assert abs(changed["opacity"] - 0.5) < 1e-6, changed
    hist = rpc.call("history.info")
    assert hist["canUndo"], hist
    rpc.call("history.undo")
    restored = rpc.call("layers.get", id=target["id"])
    assert abs(restored["opacity"] - before) < 1e-6, restored

    # A selection, a fill, and a cropped render of that area.
    rpc.call("layers.select", id=target["id"])
    sel = rpc.call("selection.rect", x=10, y=10, width=40, height=30)
    assert sel["bounds"]["width"] == 40, sel
    rpc.call("pixels.fill", color="#ff0000")
    region = rpc.call("render", region={"x": 10, "y": 10, "width": 40, "height": 30}, maxSize=0)
    assert region["width"] == 40 and region["height"] == 30, region
    rpc.call("selection.none")

    # An adjustment layer with settings, and a destructive filter.
    adj = rpc.call("layers.add", kind="adjustment", adjustmentKind="Exposure", settings={"exposure": 0.5}, name="Brighter")
    assert adj["kind"] == "adjustment" and adj["name"] == "Brighter", adj
    got = rpc.call("adjustments.get", id=adj["id"])
    assert abs(got["settings"]["exposure"] - 0.5) < 1e-6, got
    # One of Photoshop's other adjustment layers, with its own settings object.
    balance = rpc.call("layers.add", kind="adjustment", adjustmentKind="color balance", settings={"colorBalanceSettings": {"midtones": [30, 0, -10]}})
    got = rpc.call("adjustments.get", id=balance["id"])
    assert got["settings"]["colorBalanceSettings"]["midtones"][0] == 30, got
    rpc.call("layers.delete", id=balance["id"])
    rpc.call("layers.select", id=target["id"])
    rpc.call("pixels.filter", kind="Gaussian Blur", radius=2)
    rpc.call("pixels.filter", kind="Lens Correction", distortion=20, bicubic=True)
    # Filter > Mosh: an OpenMosh effect by id and key, one undo step named for it; the same seed repeats the pattern.
    before = rpc.call("layers.render", id=target["id"], maxSize=64)
    moshed = rpc.call("pixels.mosh", effect="pixel-sort", params={"low": 0.1, "reverse": True}, seed=12.5)
    assert moshed["applied"] == "pixel-sort" and moshed["params"]["reverse"] is True and abs(moshed["params"]["low"] - 0.1) < 1e-6, moshed
    assert abs(moshed["seed"] - 12.5) < 1e-6 and abs(moshed["params"]["high"] - 0.85) < 1e-6, moshed
    assert rpc.call("history.info")["undo"] == "Pixel Sort"
    first = rpc.call("layers.render", id=target["id"], maxSize=64)
    rpc.call("history.undo")
    rpc.call("pixels.mosh", effect="pixel-sort", params={"low": 0.1, "reverse": True}, seed=12.5)
    assert rpc.call("layers.render", id=target["id"], maxSize=64) == first, "the same seed gives the same pixels"
    rpc.call("history.undo")
    strobe = rpc.call("pixels.mosh", effect="strobe", params={"phase": 0.7, "mode": "Invert"})
    assert strobe["params"]["mode"] == 2 and "seed" not in strobe, strobe
    assert rpc.call("layers.render", id=target["id"], maxSize=64) != before, "the flash inverts the pixels"
    rpc.call("history.undo")
    assert rpc.call("layers.render", id=target["id"], maxSize=64) == before, "one undo step"
    # A Composite effect reads another layer by id, where it lies over this one: Overlay's Multiply with a red layer
    # takes the green and blue out; Caption stamps text.
    source = rpc.call("layers.add", name="Overlay source")
    rpc.call("pixels.fill", color="#ff0000")
    rpc.call("layers.select", id=target["id"])
    over = rpc.call("pixels.mosh", effect="overlay", params={"blend": "Multiply"}, layer=source["id"])
    assert over["applied"] == "overlay" and over["layer"] == source["id"] and over["params"]["blend"] == 1, over
    assert rpc.call("history.info")["undo"] == "Overlay"
    assert rpc.call("layers.render", id=target["id"], maxSize=64) != before, "the red layer multiplies over the pixels"
    rpc.call("history.undo")
    assert rpc.call("layers.render", id=target["id"], maxSize=64) == before
    rpc.call("layers.select", id=source["id"])   # solid red: blue text shows wherever it lands
    red = rpc.call("layers.render", id=source["id"], maxSize=64)
    cap = rpc.call("pixels.mosh", effect="caption", text="NekoPhoto", params={"scale": 6, "y": 0.5, "hue": 0.6, "saturation": 1})
    assert cap["text"] == "NekoPhoto" and rpc.call("layers.render", id=source["id"], maxSize=64) != red, cap
    rpc.call("history.undo")
    rpc.call("layers.select", id=target["id"])
    for bad in ({"effect": "overlay"}, {"effect": "vhs", "layer": source["id"]}, {"effect": "caption"}, {"effect": "overlay", "layer": "nope"}):
        try:
            rpc.call("pixels.mosh", **bad)
            raise AssertionError("pixels.mosh took " + str(bad))
        except RuntimeError as e:
            print("expected error:", e)
    rpc.call("layers.delete", id=source["id"])
    rpc.call("layers.select", id=target["id"])
    for bad in ({"effect": "blur"}, {"effect": "vhs", "params": {"nope": 1}}, {"effect": "strobe", "params": {"mode": "Sideways"}}):
        try:
            rpc.call("pixels.mosh", **bad)
            raise AssertionError("pixels.mosh took " + str(bad))
        except RuntimeError as e:
            print("expected error:", e)
    # Camera Raw Filter: one undo step named for it, the model's keys (nested too), unknown keys refused.
    before = rpc.call("layers.render", id=target["id"], maxSize=64)
    graded = rpc.call("pixels.cameraRaw", settings={"exposure": 0.7, "whiteBalance": "Auto", "detail": {"sharpenAmount": 30},
                                                    "grading": {"shadows": {"hue": 220, "saturation": 25}},
                                                    "mixer": {"hue": {"reds": 20}}, "geometry": {"rotate": 3}})
    assert graded["applied"] and abs(graded["settings"]["exposure"] - 0.7) < 1e-9, graded
    assert graded["settings"]["detail"]["sharpenAmount"] == 30 and graded["settings"]["mixer"]["hue"]["reds"] == 20, graded
    assert rpc.call("history.info")["undo"] == "Camera Raw Filter"
    after = rpc.call("layers.render", id=target["id"], maxSize=64)
    assert after != before, "the grade changes pixels"
    rpc.call("history.undo")
    assert rpc.call("layers.render", id=target["id"], maxSize=64) == before, "one undo takes it back"
    assert rpc.call("pixels.cameraRaw", settings={})["applied"] is False
    for bad in ({"exposure": 1, "sparkle": 2}, {"detail": {"sharpen": 1}}, {"glowStyle": "Sparkle"}):
        try:
            rpc.call("pixels.cameraRaw", settings=bad)
            raise AssertionError("pixels.cameraRaw should refuse %r" % bad)
        except RuntimeError as e:
            assert "settings." in str(e), e
    if info.get("scribble"):
        scribble = rpc.call("selection.scribble", foreground=[[[300, 200], [340, 210]]], background=[[[20, 20], [60, 20]]], size=16, clear=True)
        assert scribble["strokes"] == 2, scribble
        rpc.call("selection.none")
    if info.get("clickSelect"):
        subject = rpc.call("selection.subject", foreground=[[320, 210]], background=[[30, 30]], clear=True)
        assert subject["prompts"] == 2, subject
        rpc.call("selection.none")
    rpc.call("selection.rect", x=40, y=40, width=30, height=30)
    rpc.call("pixels.contentAwareFill")
    layers_before_fill = len(rpc.call("layers.list"))
    filled = rpc.call("pixels.contentAwareFill", sampling="custom", include=[{"x": 0, "y": 0, "width": 140, "height": 140}], exclude=[{"x": 100, "y": 100, "width": 20, "height": 20}], output="new")
    assert filled["filled"] and "layer" in filled, filled
    assert len(rpc.call("layers.list")) == layers_before_fill + 1
    rpc.call("history.undo")
    try:
        rpc.call("pixels.contentAwareFill", sampling="nearby")
        raise AssertionError("an unknown sampling was accepted")
    except RuntimeError as e:
        assert "sampling" in str(e), e
    moved = rpc.call("pixels.contentAwareMove", dx=30, dy=10, adaptation=3)
    assert moved["moved"] and moved["mode"] == "move", moved
    sel = rpc.call("selection.info")
    assert abs(sel["bounds"]["x"] - 70) <= 1 and abs(sel["bounds"]["y"] - 50) <= 1, sel
    rpc.call("history.undo")
    assert rpc.call("pixels.contentAwareMove", dx=-20, dy=0, mode="extend")["mode"] == "extend"
    rpc.call("history.undo")
    scaled = rpc.call("pixels.contentAwareScale", widthPercent=80, protectSelection=True)
    assert scaled["width"] >= 1 and scaled["height"] >= 1, scaled
    rpc.call("history.undo")
    rpc.call("selection.none")
    # Smart objects: place a file, convert layers, edit the contents in their tab, put them back, rasterize.
    import tempfile
    tile = os.path.join(tempfile.mkdtemp(prefix="nekophoto-smoke-"), "tile.png")
    rpc.call("render", region={"x": 0, "y": 0, "width": 16, "height": 16}, maxSize=0, path=tile)
    placed_so = rpc.call("smartObject.place", path=tile)
    assert placed_so["kind"] == "smartObject" and not placed_so["smartObject"]["locked"], placed_so
    converted = rpc.call("smartObject.convert", ids=[placed_so["id"]])
    assert converted["kind"] == "smartObject" and converted["smartObject"]["source"] != placed_so["smartObject"]["source"], converted
    parent_tab = rpc.call("tabs.list")
    opened = rpc.call("smartObject.editContents", id=converted["id"])
    inner = rpc.call("layers.list")
    assert any(l["kind"] == "smartObject" for l in (inner["layers"] if isinstance(inner, dict) else inner)), inner
    rpc.call("layers.add", kind="pixels", name="Inside")
    assert rpc.call("smartObject.commit")["committed"]
    rpc.call("tabs.close", index=opened["tab"])
    after = rpc.call("layers.get", id=converted["id"])
    assert after["kind"] == "smartObject", after
    # Painting or filtering a smart object is refused: its contents, or rasterize first.
    rpc.call("layers.select", id=converted["id"])
    for method, params in (("pixels.fill", {"color": "#ff0000"}), ("brush.stroke", {"points": [[1, 1], [5, 5]]})):
        try:
            rpc.call(method, **params)
            raise AssertionError(method + " should refuse a smart object")
        except RuntimeError as e:
            assert "smart object" in str(e), e
    # A Smart Filter: still a smart object, drawn through its filter.
    filtered = rpc.call("smartObject.addFilter", id=converted["id"], kind="gaussian blur", radius=3)
    assert filtered["kind"] == "smartObject", filtered
    # Editing the stack: settings, blending, order, switches, the shared mask, removal; each one undo step.
    rpc.call("smartObject.addFilter", id=converted["id"], kind="mosaic", cellSize=6)
    fx = rpc.call("smartObject.filters", id=converted["id"])
    assert fx["editable"] and [f["kind"] for f in fx["filters"]] == ["gaussian blur", "mosaic"], fx
    fx = rpc.call("smartObject.setFilter", id=converted["id"], index=0, radius=5, opacity=40, blend="multiply")
    assert fx["filters"][0]["settings"]["radius"] == 5 and fx["filters"][0]["opacity"] == 40 and fx["filters"][0]["blend"] == "Multiply", fx
    fx = rpc.call("smartObject.moveFilter", id=converted["id"], index=1, to=0)
    assert [f["kind"] for f in fx["filters"]] == ["mosaic", "gaussian blur"], fx
    # Dragging rows in the Layers panel (the release, through the debug hook): within the stack only.
    cid = converted["id"]
    assert rpc.call("debug.dragSmartFilter", id=cid, index=0, onto=1, position="above")["dropped"]
    assert [f["kind"] for f in rpc.call("smartObject.filters", id=cid)["filters"]] == ["gaussian blur", "mosaic"]
    assert rpc.call("debug.dragSmartFilter", id=cid, index=1, onto=0, position="below")["dropped"]
    assert [f["kind"] for f in rpc.call("smartObject.filters", id=cid)["filters"]] == ["mosaic", "gaussian blur"]
    assert not rpc.call("debug.dragSmartFilter", id=cid, index=0, onto=7)["dropped"]
    assert not rpc.call("debug.dragSmartFilter", id=cid, index=0, ontoId=next(l["id"] for l in rpc.call("layers.list") if l["id"] != cid), onto=0)["dropped"]
    fx = rpc.call("smartObject.setFilter", id=converted["id"], index=1, enabled=False)
    assert not fx["filters"][1]["enabled"], fx
    assert not rpc.call("smartObject.setFilter", id=converted["id"], enabled=False)["enabled"]
    fx = rpc.call("smartObject.filterMask", id=converted["id"], action="invert")
    assert fx["mask"]["outside"] == 0 and fx["mask"]["maskedPixels"] > 0, fx
    fx = rpc.call("smartObject.filterMask", id=converted["id"], action="select")
    assert fx["mask"]["painting"], fx
    rpc.call("smartObject.filterMask", id=converted["id"], action="deselect")
    fx = rpc.call("smartObject.removeFilter", id=converted["id"], index=0)
    assert [f["kind"] for f in fx["filters"]] == ["gaussian blur"], fx
    assert rpc.call("smartObject.removeFilter", id=converted["id"], all=True)["filters"] == []
    try:
        rpc.call("smartObject.setFilter", id=converted["id"], index=0, radius=2)
        raise AssertionError("a smart object without Smart Filters has none to set")
    except RuntimeError as e:
        assert "no Smart Filters" in str(e), e
    for _ in range(13):   # back to before the first Smart Filter
        rpc.call("history.undo")
    # Warped: the smart object keeps its contents, the warp baked into its placement.
    warped = rpc.call("layers.warp", id=converted["id"], style="arc", bend=40)
    assert warped["kind"] == "smartObject", warped
    rpc.call("history.undo")
    try:
        rpc.call("layers.warp", id=converted["id"], style="spiral")
        raise AssertionError("an unknown warp style should be refused")
    except RuntimeError as e:
        assert "presets" in str(e), e
    assert rpc.call("smartObject.rasterize", id=converted["id"])["kind"] == "pixels"
    rpc.call("history.undo")
    print("smart objects: placed, converted, edited, committed, rasterized")
    text = rpc.call("layers.add", kind="text", text="Hello", x=20, y=20, size=36, color="#ff8800")
    assert text["kind"] == "text" and text["text"]["text"] == "Hello", text
    assert text["pixelSize"]["width"] > 20 and text["pixelSize"]["height"] > 20, text
    edited = rpc.call("text.set", text="Hello there", bold=True, align="center")
    assert edited["text"]["bold"] and edited["text"]["align"] == "center" and edited["pixelSize"]["width"] > text["pixelSize"]["width"], edited
    # Letters in their own style: "there" red, larger, small caps; then back to one style.
    styled = rpc.call("text.styleRange", id=text["id"], start=6, length=5, color="#ff0000", size=48, caps="small", baselineShift=4, leading=60, underline=True)
    runs = styled["text"]["runs"]
    assert [r["length"] for r in runs] == [6, 5], runs
    assert runs[1]["color"] == "#ff0000" and runs[1]["size"] == 48 and runs[1]["caps"] == "small" and runs[1]["leading"] == 60 and runs[1]["underline"], runs
    assert runs[0]["size"] == 36 and "caps" not in runs[0], runs
    try:
        rpc.call("text.styleRange", id=text["id"], start=8, length=10, bold=True)
        raise AssertionError("a range past the text should be refused")
    except RuntimeError as e:
        assert "UTF-16" in str(e), e
    plain = rpc.call("text.styleRange", id=text["id"], color="#ff8800", size=36, caps="normal", baselineShift=0, leading=0, underline=False)
    assert "runs" not in plain["text"], plain
    rpc.call("layers.delete", id=text["id"])
    rpc.call("layers.select", id=target["id"])

    # Painting by coordinates: a stroke, a gradient and a shape layer.
    rpc.call("brush.stroke", points=[[20, 20], [120, 60], [220, 20]], size=12, color="#00ff00", opacity=1)
    # A pen's stroke replayed: pressure, tilt, twist and times per point, and a seed for tip-brush jitter.
    rpc.call("brush.stroke", points=[[20, 40], [120, 70], [220, 40]], size=10, pressures=[0.2, 0.9, 0.4], tilts=[[0, 0], [30, -10], [45, 20]],
             twists=[170, -175, -160], times=[0, 0.01, 0.02], seed=7)
    # The same drawn at 200%: the zoom speed-on-screen dynamics read; a zoom of nothing is refused.
    assert rpc.call("brush.stroke", points=[[20, 50], [120, 80]], size=10, pressures=[0.5, 0.6], times=[0, 0.01], viewScale=2)["points"] == 2
    try:
        rpc.call("brush.stroke", points=[[20, 50], [120, 80]], size=10, viewScale=0)
        raise AssertionError("a view scale of 0 should be refused")
    except RuntimeError as e:
        assert "viewScale" in str(e), e
    for toning in ({"tool": "dodge", "range": "highlights"}, {"tool": "burn", "protectTones": False}, {"tool": "sponge", "saturate": True}, {"tool": "sharpen"}):
        assert rpc.call("brush.stroke", points=[[20, 30], [200, 30]], size=20, opacity=0.5, **toning)["tool"] == toning["tool"]
    assert rpc.call("pixels.bucket", x=5, y=5, color="#336699", tolerance=10)["filled"]
    active_before = rpc.call("document.info")["activeLayer"]
    stripe = rpc.call("shape.draw", kind="rectangle", x=300, y=300, width=60, height=20, color="#00aa00")
    cage = rpc.call("layers.cage", id=stripe["id"])["points"]
    assert len(cage) == 16 and abs(cage[0][0] - 300) < 3 and abs(cage[15][1] - 320) < 3, cage
    cage[15] = [cage[15][0] + 20, cage[15][1] + 30]
    bent = rpc.call("layers.setCage", id=stripe["id"], points=cage)
    assert bent["pixelSize"]["height"] > 20, bent
    rpc.call("layers.delete", id=stripe["id"])
    star = rpc.call("shape.draw", kind="star", x=20, y=20, width=60, height=60, color="#ffaa00", strokeWidth=2, strokeColor="#000000")
    assert star["kind"] == "shape", star
    got = rpc.call("shape.get", id=star["id"])
    assert len(got["path"][0]["knots"]) == 10 and got["stroke"]["enabled"], got
    edited = rpc.call("shape.set", id=star["id"], path=[{"closed": True, "knots": [[10, 10], [50, 10], [30, 40]]}], fill=False, strokeAlign="outside")
    assert len(edited["path"][0]["knots"]) == 3 and not edited["fill"], edited
    rpc.call("layers.select", id=star["id"])
    assert len(rpc.call("paths.addAnchor", x=30, y=10)["path"][0]["knots"]) == 4
    assert len(rpc.call("paths.deleteAnchor", x=30, y=10)["path"][0]["knots"]) == 3
    made = rpc.call("paths.set", name="Triangle", path=[{"knots": [[5, 5], [45, 5], [25, 35]]}])
    assert any(p["id"] == made["id"] and p["name"] == "Triangle" for p in rpc.call("paths.list")["paths"])
    rpc.call("paths.toSelection", id=made["id"])
    assert rpc.call("selection.info")["active"]
    assert rpc.call("paths.fromSelection")["id"] == 1025
    triangle = rpc.call("paths.toShape", id=made["id"])
    rpc.call("paths.delete", id=made["id"])
    for made_layer in (triangle, star):   # the demo's layer list stays as the later checks expect
        rpc.call("layers.delete", id=made_layer["id"])
    # Path operations, gradient paints and live shape properties.
    box = rpc.call("shape.draw", kind="rectangle", x=40, y=40, width=80, height=60, cornerRadius=6, color="#224488", fillType="gradient", gradientAngle=0)
    got = rpc.call("shape.get", id=box["id"])
    assert got["fillType"] == "gradient" and got["live"] and got["live"][0]["radii"] == [6, 6, 6, 6], got
    rpc.call("layers.select", id=box["id"])
    rpc.call("shape.draw", kind="ellipse", x=60, y=60, width=30, height=30, op="subtract")
    got = rpc.call("shape.get", id=box["id"])
    assert len(got["path"]) == 2 and got["path"][1]["op"] == "subtract" and len(got["live"]) == 2, got
    got = rpc.call("shape.set", id=box["id"], live={"group": 0, "width": 100, "radii": [0, 10, 0, 10]}, strokeType="gradient", strokeWidth=3)
    assert got["live"][0]["width"] == 100 and got["live"][0]["radii"] == [0, 10, 0, 10] and got["stroke"]["strokeType"] == "gradient", got
    try:
        rpc.call("shape.set", id=box["id"], fillType="pattern", pattern="no-such-pattern")
        raise AssertionError("a pattern the document lacks should be refused")
    except RuntimeError as e:
        assert "patterns" in str(e), e
    rpc.call("paths.select")
    assert rpc.call("paths.setOperation", subpath=1, op="intersect")["path"][1]["op"] == "intersect"
    merged = rpc.call("paths.mergeComponents")["path"]
    assert merged and all(s["op"] == "add" for s in merged), merged
    assert rpc.call("shape.get", id=box["id"])["live"] == []   # edited directly: no longer live
    rpc.call("layers.delete", id=box["id"])
    # A vector mask on a pixel layer, edited as the target path.
    masked = rpc.call("layers.add", kind="pixels", name="Vector masked")
    assert rpc.call("vectorMask.set", id=masked["id"], mode="hideAll")["inverted"]
    square = rpc.call("vectorMask.set", id=masked["id"], path=[{"knots": [[10, 10], [60, 10], [60, 60], [10, 60]]}])
    assert len(square["path"]) == 1 and not square["inverted"], square
    assert rpc.call("vectorMask.target", id=masked["id"])["targeted"]
    assert len(rpc.call("paths.addAnchor", x=35, y=10)["path"][0]["knots"]) == 5
    assert len(rpc.call("vectorMask.get", id=masked["id"])["path"][0]["knots"]) == 5
    rpc.call("vectorMask.delete", id=masked["id"])
    rpc.call("layers.delete", id=masked["id"])
    # Text to a path and to a shape.
    words = rpc.call("layers.add", kind="text", text="Hi", x=20, y=20, size=40, color="#ff0000")
    assert rpc.call("text.toPath", id=words["id"])["subpaths"] >= 2
    shaped = rpc.call("text.toShape", id=words["id"])
    assert shaped["kind"] == "shape", shaped
    assert len(rpc.call("shape.get", id=words["id"])["path"]) >= 2
    rpc.call("layers.delete", id=words["id"])
    print("vector: path operations, paints, live shapes, vector masks, text to path")
    rpc.call("layers.select", id=active_before)
    rpc.call("selection.none")
    assert rpc.call("brush.stroke", tool="healingbrush", source={"x": 60, "y": 60}, points=[[20, 40], [60, 40]], size=12)["tool"] == "healingbrush"
    rpc.call("selection.rect", x=10, y=10, width=20, height=20)
    assert rpc.call("pixels.patch", dx=40, dy=0)["patched"]
    rpc.call("selection.none")
    presets = rpc.call("brush.presets")
    if presets["supported"]:
        assert len(presets["presets"]) >= 196 and "Classic" in presets["groups"], presets["groups"]
        painted = rpc.call("brush.stroke", points=[[30, 80], [130, 110], [230, 80]], preset="classic/pencil", pressures=[0.2, 0.8, 0.5], color="#000000")
        assert painted["preset"] == "classic/pencil", painted
        try:
            rpc.call("brush.stroke", points=[[0, 0], [5, 5]], preset="no/such-brush")
            raise AssertionError("an unknown preset should be refused")
        except RuntimeError as e:
            print("expected error:", e)
    # An image imported as a tip brush, then painted with (a 24x24 PNG: a dark disc on white).
    tip_path = os.path.join(tempfile.mkdtemp(), "Disc.png")
    rows = b"".join(b"\x00" + b"".join(bytes([0 if (x - 12) ** 2 + (y - 12) ** 2 < 100 else 255] * 3) for x in range(24)) for y in range(24))
    chunk = lambda kind, data: struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))
    with open(tip_path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", 24, 24, 8, 2, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b""))
    imported = rpc.call("brush.import", path=tip_path)
    assert len(imported["presets"]) == 1, imported
    tipped = rpc.call("brush.stroke", points=[[30, 140], [230, 140]], preset=imported["presets"][0], size=20)
    assert tipped["preset"] == imported["presets"][0], tipped
    # Export: PNG always; WebP and TIFF when Qt's image-format plugins are installed.
    out_dir = tempfile.mkdtemp()
    magic = {"png": b"\x89PNG", "jpg": b"\xff\xd8\xff", "webp": b"RIFF", "tif": (b"II*\x00", b"MM\x00*")}
    for suffix, start in magic.items():
        exported = os.path.join(out_dir, "export." + suffix)
        try:
            rpc.call("document.export", path=exported, quality=90)
        except RuntimeError as e:
            assert suffix in ("webp", "tif") and "must end in" in str(e), e
            print("no %s writer in this Qt; skipped" % suffix)
            continue
        with open(exported, "rb") as f:
            assert f.read(4).startswith(start if isinstance(start, bytes) else tuple(start)), suffix
    rpc.call("gradient.draw", x0=0, y0=0, x1=200, y1=0, foreground="#0000ff", opacity=0.5)
    n = len(rpc.call("layers.list"))
    shape = rpc.call("shape.draw", kind="ellipse", x=300, y=100, width=120, height=80, color="#ff00ff")
    assert shape["kind"] == "shape", shape
    assert len(rpc.call("layers.list")) == n + 1
    assert rpc.call("app.info")["currentTab"] == 0

    # The eye swipe: press toggles, dragging across another eye sets it too, release ends one undo step,
    # and a second click still works (the rows are rebuilt underneath the gesture).
    layers = rpc.call("layers.list")
    a, b = [l for l in layers if l["depth"] == 0][:2]
    was = a["visible"]
    rpc.call("debug.eye", id=a["id"], action="press")
    assert rpc.call("layers.get", id=a["id"])["visible"] == (not was)
    rpc.call("debug.eye", id=a["id"], action="move", to=b["id"])
    assert rpc.call("layers.get", id=b["id"])["visible"] == (not was)
    rpc.call("debug.eye", id=a["id"], action="release")
    hist = rpc.call("history.info")
    assert hist["canUndo"] and "Layer" in hist["undo"], hist
    rpc.call("debug.eye", id=a["id"], action="press")
    rpc.call("debug.eye", id=a["id"], action="release")
    assert rpc.call("layers.get", id=a["id"])["visible"] == was
    rpc.call("history.undo", steps=2)
    assert rpc.call("layers.get", id=b["id"])["visible"] == b["visible"]

    # A new layer below the active one, for a fresh background.
    rpc.call("layers.select", id=target["id"])
    under = rpc.call("layers.add", kind="pixels", name="Backdrop", below=True)
    order = [l["id"] for l in rpc.call("layers.list")]
    assert order.index(under["id"]) == order.index(target["id"]) + 1, order

    # History listing and a selection mask.
    names = rpc.call("history.list")
    assert names["undo"] and isinstance(names["undo"][-1], str), names
    rpc.call("selection.rect", x=0, y=0, width=50, height=50)
    mask = rpc.call("selection.render", maxSize=64)
    assert base64.b64decode(mask["png"])[:4] == b"\x89PNG", mask
    rpc.call("selection.none")

    # Events: after subscribing, an edit produces notifications before the next reply.
    rpc.call("events.subscribe", kinds=["layers", "history"])
    rpc.call("layers.set", id=target["id"], opacity=0.7)
    rpc.call("app.info")
    kinds = {e["kind"] for e in rpc.events}
    assert "layers" in kinds and "history" in kinds, rpc.events
    rpc.call("events.unsubscribe")
    rpc.call("history.undo")

    # The command-line client and batch mode.
    import subprocess
    binary = os.environ.get("COMPOSITOR_BIN", os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "build", "src", "app", "nekophoto.exe" if os.name == "nt" else "nekophoto"))
    if os.path.exists(binary):
        env = dict(os.environ); env.pop("QT_QPA_PLATFORM", None)
        out = subprocess.run([binary, "--rpc-socket", path, "--call", "app.info"], capture_output=True, text=True, env=env, timeout=60)
        assert out.returncode == 0 and '"name": "nekophoto"' in out.stdout, (out.returncode, out.stdout, out.stderr)
        bad = subprocess.run([binary, "--rpc-socket", path, "--call", "layers.get", "--params", '{"id": "nope"}'], capture_output=True, text=True, env=env, timeout=60)
        assert bad.returncode == 1 and "no layer" in bad.stderr, (bad.returncode, bad.stderr)
        script = '{"method":"document.new","params":{"width":64,"height":48}}\n{"method":"shape.draw","params":{"x":4,"y":4,"width":20,"height":20,"color":"#ff0000"}}\n{"method":"layers.list"}\n'
        batch = subprocess.run([binary, "--headless", "--batch", "-"], input=script, capture_output=True, text=True, env=env, timeout=120)
        lines = [json.loads(l) for l in batch.stdout.splitlines() if l.strip()]
        assert batch.returncode == 0 and len(lines) == 3 and len(lines[2]["result"]) == 2, (batch.returncode, batch.stdout, batch.stderr)
        print("--call and --batch ok")

    # G'MIC, when the executable is installed: a core command on the active layer, and the catalogue listing.
    cat = rpc.call("gmic.filters", search="sharpen")
    if cat["installed"]:
        rpc.call("layers.select", id=target["id"])
        applied = rpc.call("pixels.gmic", command="unsharp 2,1.5")
        assert applied["applied"].startswith("unsharp"), applied
        rpc.call("history.undo")
        # G'MIC can run shell commands; automation takes only filter names and numbers.
        for unsafe in ('x "touch /tmp/compositor-gmic-check"', "blur 3 exec ls", "blur ${x}"):
            try:
                rpc.call("pixels.gmic", command=unsafe)
                raise AssertionError("pixels.gmic ran " + unsafe)
            except RuntimeError as e:
                assert "not allowed" in str(e) or "not a filter" in str(e), e
        if cat["catalogue"]:
            assert cat["filters"], cat
            print("gmic", cat["version"], "catalogue entries matching 'sharpen':", len(cat["filters"]))
        else:
            print("gmic", cat["version"], "(no catalogue file)")

    remaining_methods(rpc)
    sixteen_bit(rpc)
    colour_management(rpc)

    # Errors come back as errors, not crashes.
    try:
        rpc.call("layers.get", id="nope")
    except RuntimeError as e:
        print("expected error:", e)
    else:
        raise SystemExit("missing-layer lookup should have failed")
    # The budgets: a side may be 30,000 pixels, but a canvas holds 100 megapixels (at 8 bits).
    budget_refusals = [("document.new", {"width": 30000, "height": 30000}),
                       ("canvas.resize", {"width": 20000, "height": 20000}),
                       ("image.resize", {"width": 20000, "height": 20000})]
    for method, params in budget_refusals:
        try:
            rpc.call(method, **params)
        except RuntimeError as e:
            assert "megapixels" in str(e), e
            print("expected error:", e)
        else:
            raise SystemExit(f"{method} past the canvas budget should have failed")
    # A PNG whose header claims 20000 x 20000 is refused from the header, before anything is decoded.
    header = struct.pack(">IIBBBBB", 20000, 20000, 8, 6, 0, 0, 0)
    png_chunk = lambda kind, data: struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))
    huge = os.path.join(tempfile.mkdtemp(), "huge.png")
    with open(huge, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + png_chunk(b"IHDR", header) + png_chunk(b"IDAT", zlib.compress(b"\0" * 64)) + png_chunk(b"IEND", b""))
    try:
        rpc.call("document.import", path=huge)
    except RuntimeError as e:
        assert "megapixels" in str(e), e
        print("expected error:", e)
    else:
        raise SystemExit("a 400-megapixel PNG should have been refused")
    methods = rpc.call("rpc.methods")
    # Every method is described, request keys are checked against the description, and names are forgiving.
    described = rpc.call("rpc.describe")
    missing = sorted(set(methods) - set(described))
    assert not missing, f"methods without a description in AutomationDescriptions.cpp: {missing}"
    for m in methods:
        d = rpc.request("rpc.describe", {"method": m})
        assert d["summary"], m
    for bad, words in ((dict(method="layers.set", params={"id": "x", "opacty": 0.5}), "has no parameter 'opacty'"),
                       (dict(method="canvas.crop", params={"x": 0, "y": 0, "width": 4}), "needs 'height'")):
        try:
            rpc.call(bad["method"], **bad["params"])
            raise AssertionError(f"{bad} should have been refused")
        except RuntimeError as e:
            assert words in str(e) and "rpc.describe" in str(e), e
    tools = next(p for p in rpc.request("rpc.describe", {"method": "tool.select"})["params"] if p["name"] == "name")["values"]
    for t in tools:
        rpc.call("tool.select", name=t)
    rpc.call("tool.select", name="move")
    pixels = next(l["id"] for l in rpc.call("layers.list") if l["kind"] == "pixels")
    assert rpc.call("layers.set", id=pixels, blend="color-dodge")["blend"] == "Color Dodge"
    rpc.call("history.undo")

    # Edit groups: the steps between begin and end become one undo step...
    count = len(rpc.call("layers.list"))
    rpc.call("history.beginGroup", name="Agent test")
    rpc.call("layers.add", kind="pixels", name="Grouped")
    rpc.call("pixels.fill", color="#00ff00")
    ended = rpc.call("history.endGroup")
    assert ended["merged"] >= 2, ended
    assert rpc.call("history.info")["undo"] == "Agent test"
    rpc.call("history.undo")
    assert len(rpc.call("layers.list")) == count
    # ...unless someone else edited in between (a second connection stands in for the person).
    other = Rpc(sys.argv[1])
    rpc.call("history.beginGroup", name="Agent test")
    rpc.call("layers.add", kind="pixels", name="Mine")
    other.call("layers.add", kind="pixels", name="Theirs")
    ended = rpc.call("history.endGroup")
    assert ended["merged"] == 0 and "separate" in ended["note"], ended
    while len(rpc.call("layers.list")) > count:
        rpc.call("history.undo")
    # A client that goes away with a group open has it closed for it.
    leaver = Rpc(sys.argv[1])
    leaver.call("history.beginGroup", name="Left open")
    leaver.call("layers.add", kind="pixels", name="Leaver")
    leaver.file.close()
    leaver.sock.close()
    for _ in range(50):
        if rpc.call("history.info")["undo"] == "Left open":
            break
        time.sleep(0.05)
    assert rpc.call("history.info")["undo"] == "Left open", rpc.call("history.info")
    rpc.call("history.undo")
    # A named batch is one step, and all or nothing.
    done = rpc.call("rpc.batch", name="Batch test", calls=[
        {"method": "layers.add", "params": {"kind": "pixels", "name": "Batched"}},
        {"method": "pixels.fill", "params": {"color": "#ff00ff"}}])
    assert done["completed"] == 2 and done["merged"] >= 2, done
    assert rpc.call("history.info")["undo"] == "Batch test"
    rpc.call("history.undo")
    failed = rpc.call("rpc.batch", name="Batch test", calls=[
        {"method": "layers.add", "params": {"kind": "pixels"}},
        {"method": "layers.set", "params": {"id": "nope", "opacity": 0.5}}])
    assert failed["error"]["index"] == 1 and failed["rolledBack"], failed
    assert len(rpc.call("layers.list")) == count

    # The edge-aware wand: a keep-out (subtract) click right after a wand click is evidence for the same
    # selection, one undo step, not a second selection.
    background = next(l for l in rpc.call("layers.list") if l["name"] == "Background")
    rpc.call("layers.select", id=background["id"])   # a layer with pixels under both clicks
    rpc.call("selection.wand", x=20, y=20, tolerance=120)
    steps = len(rpc.call("history.list")["undo"])
    first = rpc.call("selection.info")["bounds"]
    rpc.call("selection.wand", x=600, y=380, tolerance=120, mode="subtract")
    assert len(rpc.call("history.list")["undo"]) == steps, "the keep-out click replaced the step"
    assert rpc.call("history.info")["undo"] == "Magic Wand"
    rpc.call("history.undo")
    rpc.call("selection.none")

    # A layered PSD export: the demo's folder, mask, clipping and adjustment come back counted.
    psd_path = os.path.join(tempfile.mkdtemp(), "smoke.psd")
    exported = rpc.call("document.export", path=psd_path)
    assert exported["layers"] >= 3 and exported["folders"] >= 1 and os.path.getsize(psd_path) > 1000, exported
    with open(psd_path, "rb") as f:
        assert f.read(4) == b"8BPS"

    # Zoomed renders enlarge with square pixels; a masked layer renders as it shows.
    zoomed = rpc.call("render", region={"x": 10, "y": 10, "width": 16, "height": 12}, zoom=4)
    assert (zoomed["width"], zoomed["height"]) == (64, 48), zoomed
    try:
        rpc.call("render", zoom=8)
        raise AssertionError("a whole-document render at zoom 8 should be refused")
    except RuntimeError as e:
        assert "4096" in str(e), e
    masked_layer = next(l for l in rpc.call("layers.list") if l.get("mask") and l["kind"] == "pixels")
    shown = rpc.call("layers.render", id=masked_layer["id"])
    raw = rpc.call("layers.render", id=masked_layer["id"], masked=False)
    assert shown["masked"] and "masked" not in raw, (shown.keys(), raw.keys())
    assert shown["png"] != raw["png"]

    # SVG: shape layers go out as paths (the rest as images) and come back as shape layers, in a tab of their own.
    top = rpc.call("layers.list")[0]
    drawn = rpc.call("shape.draw", kind="ellipse", x=40, y=40, width=100, height=60, color="#3366cc", strokeWidth=3, strokeColor="#000000")
    rpc.call("layers.move", id=drawn["id"], above=top["id"])   # over the demo's adjustment, which flattens what is below it
    svg_path = os.path.join(tempfile.mkdtemp(), "smoke.svg")
    written = rpc.call("document.export", path=svg_path)
    assert written["shapes"] >= 1 and written["images"] >= 1, written
    with open(svg_path, "rb") as f:
        assert b"<svg" in f.read(400)
    size = (rpc.call("document.info")["width"], rpc.call("document.info")["height"])
    reopened = rpc.call("document.open", path=svg_path)
    assert (reopened["width"], reopened["height"]) == size and reopened["layers"] >= 2, reopened
    assert any(l["kind"] == "shape" for l in rpc.call("layers.list")), "the SVG's paths came back as pixels"
    try:
        rpc.call("document.open", path=svg_path, page=2)
        raise AssertionError("page is for PDF files")
    except RuntimeError as e:
        assert "PDF" in str(e), e

    # PDF (when this build has Qt PDF): a hand-written two-page file, its second page at 144 ppi.
    if rpc.call("app.info")["pdf"]:
        objects = [b"<< /Type /Catalog /Pages 2 0 R >>", b"<< /Type /Pages /Kids [3 0 R 5 0 R] /Count 2 >>"]
        for n, content in ((4, b"1 0 0 rg 36 18 36 36 re f"), (6, b"0 0 1 rg 36 18 36 36 re f")):
            objects.append(b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 144 72] /Contents %d 0 R >>" % n)
            objects.append(b"<< /Length %d >>\nstream\n%s\nendstream" % (len(content), content))
        pdf, offsets = b"%PDF-1.4\n", []
        for n, body in enumerate(objects, 1):
            offsets.append(len(pdf))
            pdf += b"%d 0 obj\n%s\nendobj\n" % (n, body)
        xref = len(pdf)
        pdf += b"xref\n0 %d\n0000000000 65535 f \n" % (len(objects) + 1) + b"".join(b"%010d 00000 n \n" % o for o in offsets)
        pdf += b"trailer\n<< /Size %d /Root 1 0 R >>\nstartxref\n%d\n%%%%EOF\n" % (len(objects) + 1, xref)
        pdf_path = os.path.join(tempfile.mkdtemp(), "smoke.pdf")
        with open(pdf_path, "wb") as f:
            f.write(pdf)
        page = rpc.call("document.open", path=pdf_path, page=2, resolution=144)
        assert (page["width"], page["height"]) == (288, 144), page
        try:
            rpc.call("document.open", path=pdf_path, page=3)
            raise AssertionError("the PDF has two pages")
        except RuntimeError:
            pass

    # Actions: record requests, play them back as one undo step, edit, export, import and batch a folder.
    name = "rpc-smoke action"
    for leftover in (name, name + " 2"):
        if any(a["name"] == leftover for a in rpc.call("actions.list")["actions"]):
            rpc.call("actions.delete", name=leftover)
    rpc.call("document.new", width=48, height=32)
    rpc.call("actions.record", action="start", name=name)
    assert rpc.call("actions.list")["recording"]
    rpc.call("selection.rect", x=2, y=2, width=10, height=10)
    rpc.call("pixels.fill", color="#ff8800")
    rpc.call("layers.list")   # looks are not recorded
    rpc.call("selection.none")
    stopped = rpc.call("actions.record", action="stop")
    assert stopped["added"] == 3, stopped
    recorded = rpc.call("actions.list", name=name)["actions"][0]
    assert [s["method"] for s in recorded["steps"]] == ["selection.rect", "pixels.fill", "selection.none"], recorded
    played = rpc.call("actions.play", name=name)
    assert played["completed"] and played["played"] == 3, played
    assert rpc.call("history.info")["undo"] == name, rpc.call("history.info")
    steps = recorded["steps"]
    steps[1]["enabled"] = False
    steps.insert(0, {"method": "pixels.invert", "params": {}})
    rpc.call("actions.save", name=name, steps=[{k: s[k] for k in ("method", "params", "enabled") if k in s} for s in steps])
    assert rpc.call("actions.play", name=name)["played"] == 3
    try:
        rpc.call("actions.save", name=name + " bad", steps=[{"params": {}}])
        raise AssertionError("a step without a method should be refused")
    except RuntimeError:
        pass
    failing = rpc.call("actions.save", name=name + " 2", steps=[{"method": "canvas.crop", "params": {"x": 0}}])
    assert failing["name"] == name + " 2"
    stopped_at = rpc.call("actions.play", name=name + " 2")
    assert not stopped_at["completed"] and stopped_at["error"]["index"] == 0, stopped_at
    rpc.call("actions.delete", name=name + " 2")
    work = tempfile.mkdtemp(prefix="nekophoto-actions-")
    exported = rpc.call("actions.export", name=name, path=os.path.join(work, "actions.json"))
    imported = rpc.call("actions.import", path=exported["path"])
    assert imported["imported"] == [name + " 2"], imported
    rpc.call("actions.delete", name=name + " 2")
    source = os.path.join(work, "in")
    os.makedirs(source)
    rpc.call("document.export", path=os.path.join(source, "one.png"))
    rpc.call("document.export", path=os.path.join(source, "two.png"))
    tabs = len(rpc.call("tabs.list"))
    batch = rpc.call("actions.batch", name=name, input=source, output=os.path.join(work, "out"), format="jpg")
    assert sorted(batch["written"]) == ["one.jpg", "two.jpg"] and not batch["failed"], batch
    assert len(rpc.call("tabs.list")) == tabs, "the batch left tabs open"
    assert rpc.call("actions.batch", name=name, input=source, output=os.path.join(work, "out"), format="jpg")["skipped"] == ["one.png", "two.png"]
    rpc.call("actions.delete", name=name)

    # Frame animation: frames of layer states, delays and looping; an animated GIF out and back in.
    rpc.call("document.new", width=32, height=24)
    rpc.call("pixels.fill", color="#ff0000")
    assert rpc.call("timeline.info")["count"] == 0
    created = rpc.call("timeline.frame", action="create")
    assert created["count"] == 1 and created["current"] == 0, created
    second = rpc.call("layers.add")
    rpc.call("pixels.fill", color="#0000ff")
    rpc.call("timeline.frame", action="duplicate")
    steps = len(rpc.call("history.list")["undo"])
    rpc.call("timeline.frame", action="select", index=0)
    assert len(rpc.call("history.list")["undo"]) == steps, "selecting a frame is not an undo step"
    rpc.call("layers.set", id=second["id"], visible=False)
    frames = rpc.call("timeline.set", delay=300, loopCount=2)
    assert frames["count"] == 2 and frames["loopCount"] == 2 and frames["frames"][0]["delay"] == 300, frames
    assert second["id"] not in frames["frames"][0]["visibleLayers"] and second["id"] in frames["frames"][1]["visibleLayers"], frames
    rpc.call("timeline.frame", action="select", index=1)
    assert next(l for l in rpc.call("layers.list") if l["id"] == second["id"])["visible"]
    moved = rpc.call("timeline.frame", action="move", index=1, to=0)
    assert moved["current"] == 0 and moved["frames"][1]["delay"] == 300, moved
    gif_path = os.path.join(work, "frames.gif")
    written = rpc.call("document.export", path=gif_path)
    assert written["frames"] == 2, written
    with open(gif_path, "rb") as f:
        assert f.read(6) == b"GIF89a"
    rpc.call("history.undo")   # the move
    assert rpc.call("timeline.info")["current"] == 1, "undoing the move returns to the frame it moved"
    rpc.call("tabs.new")
    rpc.call("document.open", path=gif_path)
    back = rpc.call("timeline.info")
    assert back["count"] == 2 and back["loopCount"] == 2 and [f["delay"] for f in back["frames"]] == [100, 300], back
    cleared = rpc.call("timeline.frame", action="clear")
    assert cleared["count"] == 0

    print(len(methods), "methods; smoke test passed")


if __name__ == "__main__":
    main()
