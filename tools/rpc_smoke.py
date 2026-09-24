#!/usr/bin/env python3
"""Smoke test for the automation socket: drives a running NekoPhoto
through a few calls and checks the answers. CI starts the app headless with
--demo first.

    nekophoto --headless --rpc-socket /tmp/c.sock --demo &
    python3 tools/rpc_smoke.py /tmp/c.sock
"""
import base64
import json
import os
import socket
import struct
import sys
import tempfile
import time
import zlib


class Rpc:
    def __init__(self, path):
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.connect(path)
        self.file = self.sock.makefile("rw", encoding="utf-8")
        self.next_id = 0
        self.events = []

    def call(self, method, **params):
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
    placed = rpc.call("document.import", path=image, x=100, y=60)
    rpc.call("layers.duplicate")
    copy = rpc.call("layers.list")[0]
    rpc.call("layers.flip", vertical=True)
    rpc.call("layers.move", id=copy["id"], atBottom=True)
    rpc.call("layers.reorder", id=copy["id"], offset=1)
    rpc.call("layers.render", id=placed["id"], maxSize=32)
    rpc.call("layers.setTransform", id=placed["id"], x=10, y=5, rotation=15)
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
    rpc.call("selection.all")
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
    rpc.call("layers.group")
    rpc.call("history.undo")
    rpc.call("history.redo")
    rpc.call("history.undo")
    rpc.call("layers.select", id=placed["id"])
    rpc.call("layers.merge", down=True)
    rpc.call("canvas.flip", vertical=False)
    rpc.call("canvas.resize", width=220, height=140)
    rpc.call("canvas.crop", x=0, y=0, width=200, height=120)
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
    rpc.call("layers.select", id=target["id"])
    rpc.call("pixels.filter", kind="Gaussian Blur", radius=2)
    rpc.call("pixels.filter", kind="Lens Correction", distortion=20, bicubic=True)
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
    rpc.call("selection.none")
    text = rpc.call("layers.add", kind="text", text="Hello", x=20, y=20, size=36, color="#ff8800")
    assert text["kind"] == "text" and text["text"]["text"] == "Hello", text
    assert text["pixelSize"]["width"] > 20 and text["pixelSize"]["height"] > 20, text
    edited = rpc.call("text.set", text="Hello there", bold=True, align="center")
    assert edited["text"]["bold"] and edited["text"]["align"] == "center" and edited["pixelSize"]["width"] > text["pixelSize"]["width"], edited
    rpc.call("layers.delete", id=text["id"])
    rpc.call("layers.select", id=target["id"])

    # Painting by coordinates: a stroke, a gradient and a shape layer.
    rpc.call("brush.stroke", points=[[20, 20], [120, 60], [220, 20]], size=12, color="#00ff00", opacity=1)
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
    binary = os.environ.get("COMPOSITOR_BIN", os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "build", "src", "app", "nekophoto"))
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

    # Errors come back as errors, not crashes.
    try:
        rpc.call("layers.get", id="nope")
    except RuntimeError as e:
        print("expected error:", e)
    else:
        raise SystemExit("missing-layer lookup should have failed")
    methods = rpc.call("rpc.methods")
    print(len(methods), "methods; smoke test passed")


if __name__ == "__main__":
    main()
