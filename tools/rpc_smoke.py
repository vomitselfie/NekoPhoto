#!/usr/bin/env python3
"""Smoke test for the automation socket: drives a running compositor-linux
through a few calls and checks the answers. CI starts the app headless with
--demo first.

    compositor-linux --headless --rpc-socket /tmp/c.sock --demo &
    python3 tools/rpc_smoke.py /tmp/c.sock
"""
import base64
import json
import socket
import sys
import time


class Rpc:
    def __init__(self, path):
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.connect(path)
        self.file = self.sock.makefile("rw", encoding="utf-8")
        self.next_id = 0

    def call(self, method, **params):
        self.next_id += 1
        self.file.write(json.dumps({"jsonrpc": "2.0", "id": self.next_id, "method": method, "params": params}) + "\n")
        self.file.flush()
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


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else None
    if not path:
        raise SystemExit("usage: rpc_smoke.py <socket path>")
    rpc = wait_for(path)
    info = rpc.call("app.info")
    assert info["name"] == "compositor-linux", info
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

    # Painting by coordinates: a stroke, a gradient and a shape layer.
    rpc.call("brush.stroke", points=[[20, 20], [120, 60], [220, 20]], size=12, color="#00ff00", opacity=1)
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
