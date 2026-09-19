# /// script
# requires-python = ">=3.10"
# dependencies = ["mcp>=1.2,<2"]
# ///
"""MCP bridge for compositor-linux.

Exposes the editor's automation socket as MCP tools over stdio, so Claude Code
(or any MCP client) can open documents, inspect and edit layers, and look at
renders. Start it with `uv run mcp/compositor_mcp.py`; it connects to a running
compositor-linux (started with --rpc, or with the automation preference on) or
launches one itself.

Environment:
  COMPOSITOR_RPC_SOCKET  socket path (default $XDG_RUNTIME_DIR/compositor-linux.sock)
  COMPOSITOR_BIN         binary to launch when nothing is listening (default: compositor-linux on PATH)
  COMPOSITOR_MCP_LAUNCH  "0" to never launch the app; "headless" to launch it without a window
"""
from __future__ import annotations

import base64
import json
import os
import shutil
import socket
import subprocess
import sys
import time
from typing import Any, Optional

from mcp.server.fastmcp import FastMCP, Image

mcp = FastMCP("compositor-linux", instructions=(
    "Drives the compositor-linux image editor (a layered, Photoshop-like editor). Coordinates are document pixels "
    "with the origin at the top-left. Layers are addressed by id from layers_list. Every edit is one undo step "
    "(history_undo reverts it). Call render after edits to see the result; use region and max_size to keep images small."
))


def socket_path() -> str:
    env = os.environ.get("COMPOSITOR_RPC_SOCKET")
    if env:
        return env
    runtime = os.environ.get("XDG_RUNTIME_DIR") or "/tmp"
    return os.path.join(runtime, "compositor-linux.sock")


class Connection:
    def __init__(self) -> None:
        self.sock: Optional[socket.socket] = None
        self.file = None
        self.next_id = 0
        self.child: Optional[subprocess.Popen] = None

    def connect(self) -> None:
        path = socket_path()
        try:
            self._open(path)
            return
        except OSError:
            pass
        mode = os.environ.get("COMPOSITOR_MCP_LAUNCH", "window")
        if mode == "0":
            raise RuntimeError(f"compositor-linux is not listening at {path}; start it with --rpc (or turn on Preferences > Automation)")
        binary = os.environ.get("COMPOSITOR_BIN") or shutil.which("compositor-linux")
        if not binary:
            for candidate in ("./build/src/app/compositor-linux", os.path.expanduser("~/Development/linpositor/Compositor/build/src/app/compositor-linux")):
                if os.path.exists(candidate):
                    binary = candidate
                    break
        if not binary:
            raise RuntimeError(f"nothing listening at {path} and no compositor-linux binary found; set COMPOSITOR_BIN")
        args = [binary, "--rpc", "--rpc-socket", path]
        if mode == "headless" or not (os.environ.get("WAYLAND_DISPLAY") or os.environ.get("DISPLAY")):
            args.append("--headless")
        self.child = subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)
        deadline = time.time() + 20
        while time.time() < deadline:
            try:
                self._open(path)
                return
            except OSError:
                time.sleep(0.25)
        raise RuntimeError(f"launched {binary} but no socket appeared at {path}")

    def _open(self, path: str) -> None:
        sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        sock.connect(path)
        self.sock = sock
        self.file = sock.makefile("rw", encoding="utf-8")

    def call(self, method: str, params: Optional[dict] = None) -> Any:
        if self.file is None:
            self.connect()
        self.next_id += 1
        request = {"jsonrpc": "2.0", "id": self.next_id, "method": method, "params": params or {}}
        try:
            self.file.write(json.dumps(request) + "\n")
            self.file.flush()
            line = self.file.readline()
        except OSError:
            line = ""
        if not line:
            # The app went away; reconnect once.
            self.file = None
            self.connect()
            self.file.write(json.dumps(request) + "\n")
            self.file.flush()
            line = self.file.readline()
        reply = json.loads(line)
        while reply.get("method") == "event":   # notifications for a subscription we never asked for
            reply = json.loads(self.file.readline())
        if "error" in reply:
            raise RuntimeError(reply["error"]["message"])
        return reply["result"]


conn = Connection()


def call(method: str, **params: Any) -> Any:
    return conn.call(method, {k: v for k, v in params.items() if v is not None})


def text(value: Any) -> str:
    return json.dumps(value, indent=1)


def png(result: dict) -> Image:
    return Image(data=base64.b64decode(result.pop("png")), format="png")


# ---- looking ------------------------------------------------------------------------------------

@mcp.tool()
def app_info() -> str:
    """Version, open tabs, and whether Remove Background is available."""
    return text(call("app.info"))


@mcp.tool()
def document_info() -> str:
    """Size, resolution, path, active layer, selection bounds and undo/redo names of the current document."""
    return text(call("document.info"))


@mcp.tool()
def layers_list() -> str:
    """All layers top to bottom with id, name, kind (pixels, shape, group, adjustment), depth, parent, visibility, opacity, blend mode, transform (document pixels), mask and adjustment settings."""
    return text(call("layers.list"))


@mcp.tool()
def history_list() -> str:
    """The recorded edits: undo (oldest first; the last is what history_undo reverts) and redo."""
    return text(call("history.list"))


@mcp.tool()
def selection_render(max_size: int = 512) -> Image:
    """The current selection as a mask image: white selected, black not."""
    return png(call("selection.render", maxSize=max_size))


@mcp.tool()
def render(max_size: int = 1024, x: Optional[float] = None, y: Optional[float] = None, width: Optional[float] = None, height: Optional[float] = None, checkerboard: bool = False) -> Image:
    """The composited document as a PNG (what an export would give), downscaled so its longest side is max_size. Give x, y, width, height to render only that region at up to full resolution. checkerboard shows transparency like the canvas does."""
    region = {"x": x, "y": y, "width": width, "height": height} if None not in (x, y, width, height) else None
    return png(call("render", maxSize=max_size, region=region, checkerboard=checkerboard))


@mcp.tool()
def render_layer(id: str, max_size: int = 1024) -> Image:
    """One layer's own pixels (not composited, transparency kept) as a PNG."""
    return png(call("layers.render", id=id, maxSize=max_size))


@mcp.tool()
def screenshot(window: bool = False, max_size: int = 1600) -> Image:
    """What the person sees: the canvas widget (or the whole window with window=true), including selection outlines and transform handles."""
    return png(call("screenshot", window=window, maxSize=max_size))


# ---- documents and tabs -------------------------------------------------------------------------

@mcp.tool()
def tabs_list() -> str:
    """The open tabs (index, title, path, modified, size) and which is current."""
    return text(call("tabs.list"))


@mcp.tool()
def tabs_select(index: int) -> str:
    """Make a tab current; every other tool works on the current tab."""
    return text(call("tabs.select", index=index))


@mcp.tool()
def document_new(width: int = 1920, height: int = 1080, resolution: float = 72) -> str:
    """A new document (in a new tab if the current one is in use) with one empty layer."""
    return text(call("document.new", width=width, height=height, resolution=resolution))


@mcp.tool()
def document_open(path: str) -> str:
    """Open a .comp project (in its own tab), a Photoshop .psd/.psb (in its own tab, with its layers; the reply lists what could not be carried), or an image file (as a layer; a first image creates the canvas)."""
    return text(call("document.open", path=os.path.abspath(path)))


@mcp.tool()
def document_import(path: str, x: Optional[float] = None, y: Optional[float] = None) -> str:
    """Add an image file as a new layer, centred on x, y (document pixels) or on the canvas."""
    return text(call("document.import", path=os.path.abspath(path), x=x, y=y))


@mcp.tool()
def document_save(path: Optional[str] = None) -> str:
    """Save the project as a .comp package, to its current path or to path."""
    return text(call("document.save", path=os.path.abspath(path) if path else None))


@mcp.tool()
def document_export(path: str, quality: int = 85, background: str = "#ffffff") -> str:
    """Flatten and export to a .png (keeps transparency) or .jpg (over background, at quality)."""
    return text(call("document.export", path=os.path.abspath(path), quality=quality, background=background))


# ---- layers -------------------------------------------------------------------------------------

@mcp.tool()
def layers_select(id: str, mask: bool = False) -> str:
    """Make a layer active (edits like fills and filters apply to the active layer); mask=true targets its mask."""
    return text(call("layers.select", id=id, mask=mask))


@mcp.tool()
def layers_set(id: str, name: Optional[str] = None, visible: Optional[bool] = None, opacity: Optional[float] = None, blend: Optional[str] = None, clipping: Optional[bool] = None) -> str:
    """Change a layer's name, visibility, opacity (0..1), blend mode (Normal, Multiply, Screen, Overlay, Soft Light, Hard Light, Color Dodge, Color Burn, Darken, Lighten, Difference, Exclusion, Luminosity) or whether it clips to the layer beneath."""
    return text(call("layers.set", id=id, name=name, visible=visible, opacity=opacity, blend=blend, clipping=clipping))


@mcp.tool()
def layers_add(kind: str = "pixels", name: Optional[str] = None, adjustment_kind: Optional[str] = None, settings: Optional[dict] = None, below: bool = False,
               text_content: Optional[str] = None, x: Optional[float] = None, y: Optional[float] = None, font: Optional[str] = None, size: Optional[float] = None,
               bold: Optional[bool] = None, italic: Optional[bool] = None, color: Optional[str] = None, align: Optional[str] = None) -> str:
    """Add a layer above the active one (or below it with below=true, e.g. a new background): kind pixels (blank), group, adjustment with adjustment_kind Levels, Curves, Hue/Saturation, Exposure, Gradient Map or Grain and optional settings (see adjustments_defaults), or text with text_content at x, y (document pixels), font family, size in pixels, bold, italic, color (CSS) and align (left, center, right)."""
    return text(call("layers.add", kind=kind, name=name, adjustmentKind=adjustment_kind, settings=settings, below=below, text=text_content, x=x, y=y, font=font, size=size, bold=bold, italic=italic, color=color, align=align))


@mcp.tool()
def text_set(id: Optional[str] = None, text_content: Optional[str] = None, font: Optional[str] = None, size: Optional[float] = None, bold: Optional[bool] = None,
             italic: Optional[bool] = None, color: Optional[str] = None, align: Optional[str] = None, line_spacing: Optional[float] = None, letter_spacing: Optional[float] = None) -> str:
    """Change a text layer's content or style (the active layer, or id): text_content, font, size (px), bold, italic, color (CSS), align (left, center, right), line_spacing (multiple of the line height), letter_spacing (px). The layer must still be text, not painted on."""
    return text(call("text.set", id=id, text=text_content, font=font, size=size, bold=bold, italic=italic, color=color, align=align, lineSpacing=line_spacing, letterSpacing=letter_spacing))


@mcp.tool()
def layers_delete(ids: list[str]) -> str:
    """Delete layers by id (layers clipped to them keep their masked look baked in)."""
    return text(call("layers.delete", ids=ids))


@mcp.tool()
def layers_duplicate(id: str) -> str:
    """A copy of the layer directly above it; the copy becomes active."""
    return text(call("layers.duplicate", id=id))


@mcp.tool()
def layers_move(id: str, parent: Optional[str] = None, above: Optional[str] = None, at_bottom: bool = False) -> str:
    """Re-parent and reorder: into group parent (or the top level), directly above layer above, or at the bottom / top of that level."""
    return text(call("layers.move", id=id, parent=parent, above=above, atBottom=at_bottom))


@mcp.tool()
def layers_set_transform(id: str, x: Optional[float] = None, y: Optional[float] = None, width: Optional[float] = None, height: Optional[float] = None, rotation: Optional[float] = None, scale: Optional[float] = None, flip_x: Optional[bool] = None, flip_y: Optional[bool] = None) -> str:
    """Place a layer non-destructively: top-left x, y and width, height in document pixels, rotation in degrees clockwise, or scale (a factor about its centre)."""
    return text(call("layers.setTransform", id=id, x=x, y=y, width=width, height=height, rotation=rotation, scale=scale, flipX=flip_x, flipY=flip_y))


@mcp.tool()
def layers_mask(id: str, action: str, revealing: bool = True) -> str:
    """Layer masks: action add (reveal all, or revealing=false to hide all), addFromSelection, delete, toggle, invert, apply, link."""
    return text(call("layers.mask", id=id, action=action, revealing=revealing))


@mcp.tool()
def layers_merge(down: bool = False) -> str:
    """Merge the selected layers, a selected folder, or (down=true) the active layer into the one beneath."""
    return text(call("layers.merge", down=down))


@mcp.tool()
def layers_group() -> str:
    """Put the selected layers into a new folder."""
    return text(call("layers.group"))


# ---- adjustments and filters ---------------------------------------------------------------------

@mcp.tool()
def adjustments_defaults(kind: str) -> str:
    """The settings object an adjustment kind takes (Levels, Curves, Hue/Saturation, Exposure, Gradient Map, Grain) with its default values."""
    return text(call("adjustments.defaults", kind=kind))


@mcp.tool()
def adjustments_get(id: str) -> str:
    """An adjustment layer's current settings."""
    return text(call("adjustments.get", id=id))


@mcp.tool()
def adjustments_set(id: str, settings: dict) -> str:
    """Change an adjustment layer's settings; a partial object is merged over the current one."""
    return text(call("adjustments.set", id=id, settings=settings))


@mcp.tool()
def pixels_adjust(kind: str, settings: Optional[dict] = None) -> str:
    """Bake an adjustment (Levels, Curves, Hue/Saturation, Exposure, Gradient Map, Grain) into the active layer's pixels, inside the selection if there is one."""
    return text(call("pixels.adjust", kind=kind, settings=settings or {}))


@mcp.tool()
def pixels_filter(kind: str, radius: Optional[float] = None, angle: Optional[float] = None, distance: Optional[float] = None, amount: Optional[float] = None, gaussian: Optional[bool] = None, monochromatic: Optional[bool] = None, distortion: Optional[float] = None, bicubic: Optional[bool] = None) -> str:
    """Run a filter on the active layer's pixels: Gaussian Blur (radius), Motion Blur (angle, distance), Add Noise (amount, gaussian, monochromatic) or Lens Correction (distortion -100..100, bicubic for a sharper resample)."""
    return text(call("pixels.filter", kind=kind, radius=radius, angle=angle, distance=distance, amount=amount, gaussian=gaussian, monochromatic=monochromatic, distortion=distortion, bicubic=bicubic))


@mcp.tool()
def pixels_fill(color: str = "#000000") -> str:
    """Fill the selection (or the whole active layer) with a CSS colour."""
    return text(call("pixels.fill", color=color))


@mcp.tool()
def pixels_clear() -> str:
    """Make the selected pixels of the active layer transparent."""
    return text(call("pixels.clear"))


@mcp.tool()
def pixels_invert() -> str:
    """Invert the active layer's colours."""
    return text(call("pixels.invert"))


@mcp.tool()
def pixels_content_aware_fill() -> str:
    """Fill the selection from its surroundings (also extends an image past its edge when the selection reaches outside it)."""
    return text(call("pixels.contentAwareFill"))


@mcp.tool()
def gmic_filters(search: str = "") -> str:
    """The G'MIC filter catalogue (name, folder, command, parameters with defaults and ranges), optionally narrowed by a search string. G'MIC is the open-source filter framework GIMP and Krita use as a plugin."""
    return text(call("gmic.filters", search=search))


@mcp.tool()
def pixels_gmic(command: str) -> str:
    """Run a G'MIC command line on the active layer's pixels inside the selection, e.g. "unsharp 2,1.5", "cartoon 3,150,20,0.25,1.5,8" or a catalogue filter's defaultCommand with edited values."""
    return text(call("pixels.gmic", command=command))


@mcp.tool()
def remove_background(refine: bool = True, refine_edges: Optional[float] = None, contrast: Optional[float] = None, shift_edge: Optional[float] = None, matting: Optional[float] = None, cleanup: bool = True, decontaminate: bool = True) -> str:
    """Mask out the active layer's background with the local segmentation model (needs Preferences > AI background removal enabled and a downloaded model). With refine, the guided edge refinement (refine_edges 0..40), matte contrast (0..100), shift_edge (-10..10), the matting band (0..400 px, solves hair opacity), speckle cleanup and edge-colour decontamination (the edge pixels take the subject's own colour) apply."""
    return text(call("pixels.removeBackground", refine=refine, refineEdges=refine_edges, contrast=contrast, shiftEdge=shift_edge, matting=matting, cleanup=cleanup, decontaminate=decontaminate))


# ---- painting by coordinates ---------------------------------------------------------------------

@mcp.tool()
def brush_stroke(points: list[list[float]], tool: str = "brush", size: Optional[float] = None, hardness: Optional[float] = None, opacity: Optional[float] = None, color: Optional[str] = None, mask: bool = False, source_x: Optional[float] = None, source_y: Optional[float] = None) -> str:
    """Paint one stroke through [x, y] points on the active layer (or its mask with mask=true): tool brush, eraser, healing, clone (with source_x/source_y), smudge, blur or liquify; size in pixels, hardness and opacity 0..1, a CSS color."""
    source = {"x": source_x, "y": source_y} if source_x is not None and source_y is not None else None
    return text(call("brush.stroke", points=points, tool=tool, size=size, hardness=hardness, opacity=opacity, color=color, mask=mask, source=source))


@mcp.tool()
def gradient_draw(x0: float, y0: float, x1: float, y1: float, shape: str = "linear", style: str = "foreground-to-transparent", reversed: bool = False, opacity: float = 1.0, foreground: Optional[str] = None, background: Optional[str] = None) -> str:
    """Draw a gradient on the active layer from (x0, y0) to (x1, y1): shape linear or radial; style foreground-to-transparent or foreground-to-background; colours as CSS strings."""
    return text(call("gradient.draw", x0=x0, y0=y0, x1=x1, y1=y1, shape=shape, style=style, reversed=reversed, opacity=opacity, foreground=foreground, background=background))


@mcp.tool()
def shape_draw(x: float, y: float, width: float, height: float, kind: str = "rectangle", corner_radius: float = 0, color: Optional[str] = None) -> str:
    """Add a filled rectangle (optionally rounded) or ellipse as a new shape layer."""
    return text(call("shape.draw", x=x, y=y, width=width, height=height, kind=kind, cornerRadius=corner_radius, color=color))


# ---- selection ----------------------------------------------------------------------------------

@mcp.tool()
def selection_info() -> str:
    """Whether there is a selection and its bounds."""
    return text(call("selection.info"))


@mcp.tool()
def selection_rect(x: float, y: float, width: float, height: float, ellipse: bool = False, mode: str = "replace") -> str:
    """Select a rectangle or ellipse (document pixels); mode replace, add or subtract."""
    return text(call("selection.rect", x=x, y=y, width=width, height=height, ellipse=ellipse, mode=mode))


@mcp.tool()
def selection_polygon(points: list[list[float]], mode: str = "replace") -> str:
    """Select a polygon from [x, y] points."""
    return text(call("selection.polygon", points=points, mode=mode))


@mcp.tool()
def selection_wand(x: float, y: float, tolerance: int = 32, contiguous: bool = True, sample_all: bool = False, mode: str = "replace") -> str:
    """Magic wand: select the colour at a point within tolerance (0..255), reading the active layer's own pixels (sample_all=true reads the visible composite instead)."""
    return text(call("selection.wand", x=x, y=y, tolerance=tolerance, contiguous=contiguous, sampleAll=sample_all, mode=mode))


@mcp.tool()
def selection_from_layer(id: str, mask: bool = False, mode: str = "replace") -> str:
    """Load a layer's opaque pixels (or its mask) as the selection."""
    return text(call("selection.fromLayer", id=id, mask=mask, mode=mode))


@mcp.tool()
def selection_edit(action: str, amount: float = 0) -> str:
    """action all, none, invert, grow (by amount pixels; negative contracts), feather (Gaussian of that radius), smooth (disc majority of that radius) or border (a band that wide)."""
    if action == "grow":
        return text(call("selection.grow", amount=int(amount)))
    if action == "feather":
        return text(call("selection.feather", radius=amount))
    if action == "smooth":
        return text(call("selection.smooth", radius=int(amount)))
    if action == "border":
        return text(call("selection.border", width=int(amount)))
    return text(call(f"selection.{action}"))


# ---- canvas and history ---------------------------------------------------------------------------

@mcp.tool()
def canvas_resize(width: int, height: int, anchor_x: float = 0.5, anchor_y: float = 0.5) -> str:
    """Change the canvas size without scaling pixels; the anchor (0..1) says which side stays put."""
    return text(call("canvas.resize", width=width, height=height, anchorX=anchor_x, anchorY=anchor_y))


@mcp.tool()
def canvas_crop(x: float, y: float, width: float, height: float) -> str:
    """Crop the document to a rectangle."""
    return text(call("canvas.crop", x=x, y=y, width=width, height=height))


@mcp.tool()
def image_resize(width: Optional[int] = None, height: Optional[int] = None, scale: Optional[float] = None, sampling: str = "high") -> str:
    """Resample the whole image (every layer) to width x height (one keeps the aspect ratio) or by scale; sampling nearest, smooth or high."""
    return text(call("image.resize", width=width, height=height, scale=scale, sampling=sampling))


@mcp.tool()
def history_undo(steps: int = 1) -> str:
    """Undo the last edit(s)."""
    return text(call("history.undo", steps=steps))


@mcp.tool()
def history_redo(steps: int = 1) -> str:
    """Redo."""
    return text(call("history.redo", steps=steps))


@mcp.tool()
def rpc(method: str, params: Optional[dict] = None) -> str:
    """Call any automation method directly (rpc.methods lists them); the escape hatch for anything without a tool."""
    return text(conn.call(method, params or {}))


if __name__ == "__main__":
    mcp.run()
