# /// script
# requires-python = ">=3.10"
# dependencies = ["mcp>=1.10,<2"]
# ///
"""MCP bridge for NekoPhoto.

Exposes the editor's automation socket as MCP tools over stdio, so Claude Code
(or any MCP client) can open documents, inspect and edit layers, and look at
renders. Start it with `uv run mcp/nekophoto_mcp.py`; it connects to a running
nekophoto (started with --rpc, or with the automation preference on) or
launches one itself.

Environment:
  COMPOSITOR_RPC_SOCKET  socket path (default $XDG_RUNTIME_DIR/nekophoto.sock, else Qt's private
                         /tmp/runtime-<user>/, never the shared /tmp itself)
  COMPOSITOR_BIN         binary to launch when nothing is listening (default: nekophoto on PATH,
                         else the build next to this file)
  COMPOSITOR_MCP_LAUNCH  "0" to never launch the app; "headless" to launch it without a window
"""
from __future__ import annotations

import base64
import getpass
import json
import os
import shutil
import socket
import stat
import subprocess
import sys
import tempfile
import time
from typing import Any, Optional

from mcp.server.fastmcp import FastMCP, Image
from mcp.types import ToolAnnotations

mcp = FastMCP("nekophoto", instructions=(
    "Drives the NekoPhoto image editor (layers, masks, adjustments, filters, painting, selections). "
    "Work in a loop: look (document_overview; render at max_size 512 to orient), act in small steps, then verify "
    "with render (a region at full size for details). Every edit is one undo step: history_undo when a render "
    "shows the wrong thing. Coordinates are document pixels, origin top-left; layers are addressed by the ids "
    "document_overview lists. Filters, fills and adjustments act on the active layer inside the selection. "
    "Wrap a change of several steps in history_group_begin / history_group_end so the person undoes it at once. "
    "Before using rpc for a method without a tool, describe_method shows what it takes. "
    "The edit_photo prompt has recipes."
))


def look(title: str, **options: Any):
    """A tool that only reads."""
    return mcp.tool(title=title, annotations=ToolAnnotations(readOnlyHint=True, openWorldHint=False), **options)


def edit(title: str):
    """A tool that changes the document, as one undo step."""
    return mcp.tool(title=title, annotations=ToolAnnotations(readOnlyHint=False, destructiveHint=False, openWorldHint=False))


def outside(title: str, **options: Any):
    """A tool whose effect undo does not reach: files written, tabs or documents closed."""
    return mcp.tool(title=title, annotations=ToolAnnotations(readOnlyHint=False, destructiveHint=True, openWorldHint=False), **options)


def socket_path() -> str:
    env = os.environ.get("COMPOSITOR_RPC_SOCKET")
    if env:
        return env
    runtime = os.environ.get("XDG_RUNTIME_DIR") or private_runtime_dir()
    return os.path.join(runtime, "nekophoto.sock")


def private_runtime_dir() -> str:
    """Qt's fallback when XDG_RUNTIME_DIR is unset (the editor uses it too): /tmp/runtime-<user>, which must be
    ours and closed to everyone else, never the shared /tmp itself."""
    path = os.path.join(tempfile.gettempdir(), "runtime-" + getpass.getuser())
    os.makedirs(path, mode=0o700, exist_ok=True)
    info = os.lstat(path)
    if not stat.S_ISDIR(info.st_mode) or info.st_uid != os.getuid() or info.st_mode & 0o077:
        raise RuntimeError(f"{path} is not a private folder of this user; set XDG_RUNTIME_DIR or COMPOSITOR_RPC_SOCKET")
    return path


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
            raise RuntimeError(f"NekoPhoto is not listening at {path}; start it with --rpc (or turn on Preferences > Automation)")
        binary = os.environ.get("COMPOSITOR_BIN") or shutil.which("nekophoto")
        if not binary:
            # The build next to this bridge (mcp/ sits in the source tree), never one relative to the current folder.
            candidate = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "build", "src", "app", "nekophoto")
            if os.path.isfile(candidate) and os.access(candidate, os.X_OK):
                binary = candidate
        if not binary:
            raise RuntimeError(f"nothing listening at {path} and no nekophoto binary found; set COMPOSITOR_BIN")
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

@look("App info")
def app_info() -> str:
    """Version, open tabs, and whether Remove Background is available."""
    return text(call("app.info"))


@look("Document info")
def document_info() -> str:
    """Size, resolution, path, active layer, selection bounds and undo/redo names of the current document."""
    return text(call("document.info"))


@look("Document overview", structured_output=False)
def document_overview(render: bool = False, max_size: int = 512, max_layers: int = 80) -> list:
    """Start here. The document at a glance as text: size, selection, what undo would revert, and the layer tree
    top first with each layer's kind, bounds, opacity, blend, mask, visibility and id (* marks the active one).
    render=true adds a picture of the composite at max_size."""
    result = call("document.overview", maxLayers=max_layers)
    out: list = [result["overview"]]
    if render:
        out.append(png(call("render", maxSize=max_size)))
    return out


@look("Describe a method")
def describe_method(method: Optional[str] = None) -> str:
    """What an editor method takes: its parameters with types, defaults and valid values (blend modes, tools,
    filter kinds...). Without a method, one line per method, including those only rpc reaches."""
    return text(conn.call("rpc.describe", {"method": method} if method else {}))


@look("List layers")
def layers_list() -> str:
    """All layers top to bottom with id, name, kind (pixels, shape, group, adjustment), depth, parent, visibility, opacity, blend mode, transform (document pixels), mask and adjustment settings."""
    return text(call("layers.list"))


@look("Edit history")
def history_list() -> str:
    """The recorded edits: undo (oldest first; the last is what history_undo reverts) and redo."""
    return text(call("history.list"))


@look("Show the selection")
def selection_render(max_size: int = 512) -> Image:
    """The current selection as a mask image: white selected, black not."""
    return png(call("selection.render", maxSize=max_size))


@look("Render the document")
def render(max_size: int = 1024, x: Optional[float] = None, y: Optional[float] = None, width: Optional[float] = None, height: Optional[float] = None, checkerboard: bool = False, zoom: Optional[float] = None) -> Image:
    """The composited document as a PNG (what an export would give), downscaled so its longest side is max_size. Give x, y, width, height to render only that region at up to full resolution; zoom (2..32) enlarges that region with square pixels to judge an edge, a seam or a gap exactly (region times zoom within 4096). checkerboard shows transparency like the canvas does."""
    region = {"x": x, "y": y, "width": width, "height": height} if None not in (x, y, width, height) else None
    return png(call("render", maxSize=max_size, region=region, checkerboard=checkerboard, zoom=zoom))


@look("Render one layer")
def render_layer(id: str, max_size: int = 1024, masked: bool = True) -> Image:
    """One layer alone as a PNG, not composited with the others (transparency kept). A layer with a mask shows
    as it looks on the canvas, the mask applied over the layer's bounds; masked=false gives its raw pixels."""
    return png(call("layers.render", id=id, maxSize=max_size, masked=masked))


@look("Screenshot the editor")
def screenshot(window: bool = False, max_size: int = 1600) -> Image:
    """What the person sees: the canvas widget (or the whole window with window=true), including selection outlines and transform handles."""
    return png(call("screenshot", window=window, maxSize=max_size))


# ---- documents and tabs -------------------------------------------------------------------------

@look("List tabs")
def tabs_list() -> str:
    """The open tabs (index, title, path, modified, size) and which is current."""
    return text(call("tabs.list"))


@edit("Switch tab")
def tabs_select(index: int) -> str:
    """Make a tab current; every other tool works on the current tab."""
    return text(call("tabs.select", index=index))


@edit("New tab")
def tabs_new() -> str:
    """Open an empty tab and make it current."""
    return text(call("tabs.new"))


@outside("Close a tab")
def tabs_close(index: int, discard: bool = False) -> str:
    """Close a tab; with unsaved changes it refuses unless discard=true (those changes are lost)."""
    return text(call("tabs.close", index=index, discard=discard))


@outside("Close the document")
def document_close(discard: bool = False) -> str:
    """Close the current tab's document; with unsaved changes it refuses unless discard=true (those changes are lost)."""
    return text(call("document.close", discard=discard))


@edit("New document")
def document_new(width: int = 1920, height: int = 1080, resolution: float = 72) -> str:
    """A new document (in a new tab if the current one is in use) with one empty layer."""
    return text(call("document.new", width=width, height=height, resolution=resolution))


@edit("Open a file")
def document_open(path: str) -> str:
    """Open a .comp project (in its own tab), a Photoshop .psd/.psb, Clip Studio .clip or Affinity .afphoto/.afdesign/.afpub/.af (in its own tab, with its layers; the reply lists what could not be carried), or an image file, camera RAW files included (CR2, NEF, ARW, DNG, ...; developed with the camera white balance), as a layer (a first image creates the canvas)."""
    return text(call("document.open", path=os.path.abspath(path)))


@edit("Import an image as a layer")
def document_import(path: str, x: Optional[float] = None, y: Optional[float] = None) -> str:
    """Add an image file as a new layer, centred on x, y (document pixels) or on the canvas."""
    return text(call("document.import", path=os.path.abspath(path), x=x, y=y))


@outside("Save the project")
def document_save(path: Optional[str] = None) -> str:
    """Save the project as a .comp package, to its current path or to path. macCompatible in the answer is
    false when the layers total more than the 100 megapixels Compositor for macOS opens (up to a gigapixel
    saves and opens here)."""
    return text(call("document.save", path=os.path.abspath(path) if path else None))


@outside("Export an image")
def document_export(path: str, quality: int = 85, background: str = "#ffffff") -> str:
    """Flatten and export to a .png, .webp or .tif (these keep transparency; WebP at quality 100 is
    lossless) or .jpg (over background, at quality)."""
    return text(call("document.export", path=os.path.abspath(path), quality=quality, background=background))


# ---- layers -------------------------------------------------------------------------------------

@edit("Select a layer")
def layers_select(id: str, mask: bool = False) -> str:
    """Make a layer active (edits like fills and filters apply to the active layer); mask=true targets its mask."""
    return text(call("layers.select", id=id, mask=mask))


@look("Get a layer")
def layers_get(id: str) -> str:
    """One layer's full record, as layers_list reports it (transform, mask, adjustment settings, text style)."""
    return text(call("layers.get", id=id))


@edit("Set layer properties")
def layers_set(id: str, name: Optional[str] = None, visible: Optional[bool] = None, opacity: Optional[float] = None, blend: Optional[str] = None, clipping: Optional[bool] = None) -> str:
    """Change a layer's name, visibility, opacity (0..1), blend mode (any of Photoshop's: Normal, Dissolve, Darken, Multiply, Color Burn, Linear Burn, Darker Color, Lighten, Screen, Color Dodge, Linear Dodge (Add), Lighter Color, Overlay, Soft Light, Hard Light, Vivid Light, Linear Light, Pin Light, Hard Mix, Difference, Exclusion, Subtract, Divide, Hue, Saturation, Color, Luminosity; a folder also Pass Through) or whether it clips to the layer beneath."""
    return text(call("layers.set", id=id, name=name, visible=visible, opacity=opacity, blend=blend, clipping=clipping))


@edit("Add a layer")
def layers_add(kind: str = "pixels", name: Optional[str] = None, adjustment_kind: Optional[str] = None, settings: Optional[dict] = None, below: bool = False,
               text_content: Optional[str] = None, x: Optional[float] = None, y: Optional[float] = None, font: Optional[str] = None, size: Optional[float] = None,
               bold: Optional[bool] = None, italic: Optional[bool] = None, color: Optional[str] = None, align: Optional[str] = None) -> str:
    """Add a layer above the active one (or below it with below=true, e.g. a new background): kind pixels (blank), group, adjustment with adjustment_kind Levels, Curves, Hue/Saturation, Exposure, Gradient Map or Grain and optional settings (see adjustments_defaults), or text with text_content at x, y (document pixels), font family, size in pixels, bold, italic, color (CSS) and align (left, center, right)."""
    return text(call("layers.add", kind=kind, name=name, adjustmentKind=adjustment_kind, settings=settings, below=below, text=text_content, x=x, y=y, font=font, size=size, bold=bold, italic=italic, color=color, align=align))


@edit("Edit a text layer")
def text_set(id: Optional[str] = None, text_content: Optional[str] = None, font: Optional[str] = None, size: Optional[float] = None, bold: Optional[bool] = None,
             italic: Optional[bool] = None, color: Optional[str] = None, align: Optional[str] = None, line_spacing: Optional[float] = None, letter_spacing: Optional[float] = None) -> str:
    """Change a text layer's content or style (the active layer, or id): text_content, font, size (px), bold, italic, color (CSS), align (left, center, right), line_spacing (multiple of the line height), letter_spacing (px). The layer must still be text, not painted on."""
    return text(call("text.set", id=id, text=text_content, font=font, size=size, bold=bold, italic=italic, color=color, align=align, lineSpacing=line_spacing, letterSpacing=letter_spacing))


@edit("Delete layers")
def layers_delete(ids: list[str]) -> str:
    """Delete layers by id (layers clipped to them keep their masked look baked in)."""
    return text(call("layers.delete", ids=ids))


@edit("Duplicate a layer")
def layers_duplicate(id: str) -> str:
    """A copy of the layer directly above it; the copy becomes active."""
    return text(call("layers.duplicate", id=id))


@edit("Move a layer in the tree")
def layers_move(id: str, parent: Optional[str] = None, above: Optional[str] = None, at_bottom: bool = False) -> str:
    """Re-parent and reorder: into group parent (or the top level), directly above layer above, or at the bottom / top of that level."""
    return text(call("layers.move", id=id, parent=parent, above=above, atBottom=at_bottom))


@edit("Reorder a layer")
def layers_reorder(offset: int, id: Optional[str] = None) -> str:
    """Move a layer (default the active one) up (positive offset) or down among its siblings."""
    return text(call("layers.reorder", id=id, offset=offset))


@edit("Flip a layer")
def layers_flip(id: Optional[str] = None, vertical: bool = False) -> str:
    """Flip a layer's pixels left to right, or top to bottom with vertical=true."""
    return text(call("layers.flip", id=id, vertical=vertical))


@edit("Place a layer")
def layers_set_transform(id: str, x: Optional[float] = None, y: Optional[float] = None, width: Optional[float] = None, height: Optional[float] = None, rotation: Optional[float] = None, scale: Optional[float] = None, flip_x: Optional[bool] = None, flip_y: Optional[bool] = None) -> str:
    """Place a layer non-destructively: top-left x, y and width, height in document pixels, rotation in degrees clockwise, or scale (a factor about its centre)."""
    return text(call("layers.setTransform", id=id, x=x, y=y, width=width, height=height, rotation=rotation, scale=scale, flipX=flip_x, flipY=flip_y))


@look("Layer style")
def layers_style(id: str) -> str:
    """A layer's effects (Photoshop's layer style): dropShadows, innerShadows, outerGlows, innerGlows, bevels, satins, colorOverlays, gradientOverlays, patternOverlays and strokes, each a list (switched-off ones too, with enabled false), plus visible, maskHidesEffects and blendInteriorAsGroup."""
    return text(call("layers.style", id=id))


@edit("Set layer style")
def layers_set_style(id: str, style: dict) -> str:
    """Replace a layer's effects, shaped as layers_style shows; settings left out take Photoshop's defaults and {} clears the style. Colours are "#rrggbb"; opacity, scale and depth are fractions (1 = 100%); spread, choke and range percent; sizes and distances pixels; angles degrees; mode a blend mode (normal, multiply, screen, overlay, linearDodge, ...). Example: {"dropShadows": [{"distance": 8, "size": 10}], "strokes": [{"size": 3, "color": "#ffffff", "position": "outside"}]}."""
    return text(call("layers.setStyle", id=id, style=style))


@edit("Layer mask")
def layers_mask(id: str, action: str, revealing: bool = True) -> str:
    """Layer masks: action add (reveal all, or revealing=false to hide all), addFromSelection, delete, toggle, invert, apply, link."""
    return text(call("layers.mask", id=id, action=action, revealing=revealing))


@edit("Merge layers")
def layers_merge(down: bool = False) -> str:
    """Merge the selected layers, a selected folder, or (down=true) the active layer into the one beneath."""
    return text(call("layers.merge", down=down))


@edit("Group layers")
def layers_group() -> str:
    """Put the selected layers into a new folder."""
    return text(call("layers.group"))


@edit("Convert to smart object")
def smart_object_convert(ids: Optional[list[str]] = None) -> str:
    """Turn the selected layers (or these layer ids) into one smart object: their PSD becomes its contents, placed
    where they were. Moving or scaling it later resamples the original, never the last result."""
    return text(call("smartObject.convert", ids=ids))


@edit("Place a smart object")
def smart_object_place(path: str) -> str:
    """Place an image or PSD file as an embedded smart object above the active layer, 1:1 in the middle (scaled
    down to fit a smaller canvas)."""
    return text(call("smartObject.place", path=os.path.abspath(path)))


@edit("Replace smart object contents")
def smart_object_replace(path: str, id: Optional[str] = None) -> str:
    """Swap a smart object's contents for a file's in every layer placing them; each keeps its centre and scale."""
    return text(call("smartObject.replace", id=id, path=os.path.abspath(path)))


@edit("Rasterize a smart object")
def smart_object_rasterize(id: Optional[str] = None) -> str:
    """Make a smart object plain pixels (it keeps what it shows)."""
    return text(call("smartObject.rasterize", id=id))


@edit("Add a Smart Filter")
def smart_object_add_filter(kind: str, id: Optional[str] = None, radius: Optional[float] = None, threshold: Optional[float] = None,
                            amount: Optional[float] = None, angle: Optional[float] = None, distance: Optional[float] = None,
                            cell_size: Optional[float] = None, height: Optional[float] = None, opacity: float = 100) -> str:
    """Add a Smart Filter on top of a smart object's stack, as Photoshop keeps it (non-destructive): gaussian blur,
    high pass, median, dust and scratches, surface blur, unsharp mask, motion blur, plastic wrap, mosaic, emboss,
    box blur, radial blur or add noise. Only the settings the filter uses matter."""
    return text(call("smartObject.addFilter", id=id, kind=kind, radius=radius, threshold=threshold, amount=amount, angle=angle,
                     distance=distance, cellSize=cell_size, height=height, opacity=opacity))


@edit("Warp a layer")
def layers_warp(style: str, id: Optional[str] = None, bend: float = 50, horizontal: float = 0, vertical: float = 0,
                orientation: str = "horizontal") -> str:
    """Warp a layer with one of Photoshop's presets (arc, arc lower, arc upper, arch, bulge, shell lower, shell upper,
    flag, wave, fish, rise, fisheye, inflate, squeeze, twist). Text gets Warp Text (style "none" removes it), a smart
    object keeps its contents and has the warp baked into its placement, pixels are bent for good. bend, horizontal and
    vertical are percents (-100..100)."""
    return text(call("layers.warp", id=id, style=style, bend=bend, horizontal=horizontal, vertical=vertical, orientation=orientation))


@outside("Open smart object contents")
def smart_object_edit_contents(id: Optional[str] = None) -> str:
    """Open a smart object's contents in a new tab. Edit them there with the usual tools, then smart_object_commit
    puts them back into every layer placing them (and tabs_select returns to the document)."""
    return text(call("smartObject.editContents", id=id))


@edit("Put smart object contents back")
def smart_object_commit() -> str:
    """In a contents tab opened by smart_object_edit_contents: put the contents back into the smart object."""
    return text(call("smartObject.commit"))


# ---- adjustments and filters ---------------------------------------------------------------------

@look("Adjustment settings shape")
def adjustments_defaults(kind: str) -> str:
    """The settings object an adjustment kind takes (Levels, Curves, Hue/Saturation, Exposure, Gradient Map, Grain) with its default values."""
    return text(call("adjustments.defaults", kind=kind))


@look("Adjustment settings")
def adjustments_get(id: str) -> str:
    """An adjustment layer's current settings."""
    return text(call("adjustments.get", id=id))


@edit("Change an adjustment")
def adjustments_set(id: str, settings: dict) -> str:
    """Change an adjustment layer's settings; a partial object is merged over the current one."""
    return text(call("adjustments.set", id=id, settings=settings))


@edit("Adjust pixels")
def pixels_adjust(kind: str, settings: Optional[dict] = None) -> str:
    """Bake an adjustment (Levels, Curves, Hue/Saturation, Exposure, Gradient Map, Grain) into the active layer's pixels, inside the selection if there is one."""
    return text(call("pixels.adjust", kind=kind, settings=settings or {}))


@edit("Filter pixels")
def pixels_filter(kind: str, radius: Optional[float] = None, angle: Optional[float] = None, distance: Optional[float] = None, amount: Optional[float] = None, gaussian: Optional[bool] = None, monochromatic: Optional[bool] = None, distortion: Optional[float] = None, bicubic: Optional[bool] = None) -> str:
    """Run a filter on the active layer's pixels: Gaussian Blur (radius), Motion Blur (angle, distance), Add Noise (amount, gaussian, monochromatic) or Lens Correction (distortion -100..100, bicubic for a sharper resample)."""
    return text(call("pixels.filter", kind=kind, radius=radius, angle=angle, distance=distance, amount=amount, gaussian=gaussian, monochromatic=monochromatic, distortion=distortion, bicubic=bicubic))


@edit("Camera Raw Filter")
def pixels_camera_raw(settings: dict, seed: int = 1) -> str:
    """Filter > Camera Raw Filter on the active layer's pixels, inside the selection. settings uses the model's keys and
    ranges (describe_method pixels.cameraRaw lists them all); anything left out keeps its default, which changes nothing.
    Common ones: temperature, tint, exposure (-5..5 stops), contrast, highlights, shadows, whites, blacks, texture, clarity,
    dehaze, vibrance, saturation (-100..100); whiteBalance "Auto" balances the layer; nested objects curve, mixer, grading,
    detail, optics, geometry, calibration, e.g. {"detail": {"sharpenAmount": 40}, "grading": {"shadows": {"hue": 220, "saturation": 30}}}.
    Replies with the settings it applied."""
    return text(call("pixels.cameraRaw", settings=settings, seed=seed))


@edit("Fill")
def pixels_fill(color: str = "#000000") -> str:
    """Fill the selection (or the whole active layer) with a CSS colour."""
    return text(call("pixels.fill", color=color))


@edit("Clear pixels")
def pixels_clear() -> str:
    """Make the selected pixels of the active layer transparent."""
    return text(call("pixels.clear"))


@edit("Invert colours")
def pixels_invert() -> str:
    """Invert the active layer's colours."""
    return text(call("pixels.invert"))


@edit("Content-Aware Fill")
def pixels_content_aware_fill() -> str:
    """Fill the selection from its surroundings (also extends an image past its edge when the selection reaches outside it)."""
    return text(call("pixels.contentAwareFill"))


@look("G'MIC catalogue")
def gmic_filters(search: str = "") -> str:
    """The G'MIC filter catalogue (name, folder, command, parameters with defaults and ranges), optionally narrowed by a search string. G'MIC is the open-source filter framework GIMP and Krita use as a plugin."""
    return text(call("gmic.filters", search=search))


@edit("Run a G'MIC filter")
def pixels_gmic(command: str) -> str:
    """Run a G'MIC command line on the active layer's pixels inside the selection, e.g. "unsharp 2,1.5", "cartoon 3,150,20,0.25,1.5,8" or a catalogue filter's defaultCommand with edited values. Only filter names (from gmic_filters, or common built-ins such as blur, sharpen, unsharp, denoise, cartoon) followed by numbers are accepted: no strings, paths or other G'MIC commands."""
    return text(call("pixels.gmic", command=command))


@edit("Remove the background")
def remove_background(refine: bool = True, refine_edges: Optional[float] = None, contrast: Optional[float] = None, shift_edge: Optional[float] = None, matting: Optional[float] = None, cleanup: bool = True, decontaminate: bool = True, detail: bool = False, flip: Optional[bool] = None) -> str:
    """Mask out the active layer's background with the local segmentation model (needs Preferences > AI background removal enabled and a downloaded model). With refine, the guided edge refinement (refine_edges 0..40), matte contrast (0..100), shift_edge (-10..10), the matting band (0..400 px, solves hair opacity), speckle cleanup and edge-colour decontamination (the edge pixels take the subject's own colour) apply. detail runs the model again on full-resolution windows along the edge of a large photo (slower, sharper hair). flip averages the model's mask with the mirrored image's (the preference's default when omitted; steadier edges, twice the model time)."""
    return text(call("pixels.removeBackground", refine=refine, refineEdges=refine_edges, contrast=contrast, shiftEdge=shift_edge, matting=matting, cleanup=cleanup, decontaminate=decontaminate, detail=detail, flip=flip))


# ---- painting by coordinates ---------------------------------------------------------------------

@edit("Paint a stroke")
def brush_stroke(points: list[list[float]], tool: str = "brush", size: Optional[float] = None, hardness: Optional[float] = None, opacity: Optional[float] = None, color: Optional[str] = None, mask: bool = False, source_x: Optional[float] = None, source_y: Optional[float] = None, preset: Optional[str] = None, pressure: Optional[float] = None, pressures: Optional[list[float]] = None, tone_range: Optional[str] = None, protect_tones: Optional[bool] = None, saturate: Optional[bool] = None) -> str:
    """Paint one stroke through [x, y] points on the active layer (or its mask with mask=true): tool brush, eraser, healing (spot healing), healingbrush or clone (both with source_x/source_y: healingbrush matches the copied texture to the tone around the stroke), smudge, blur, sharpen or liquify, or dodge / burn (lighten / darken; tone_range shadows, midtones or highlights, protect_tones keeps the colour) and sponge (desaturates, or saturates with saturate=true), where opacity is the Exposure or Flow; size in pixels, hardness and opacity 0..1, a CSS color.
    With tool brush or eraser, preset picks a MyPaint brush from brush_presets (pencils, inks, charcoal, paint, smudging; "round" for the plain tip); it starts at its own size unless size is given, and follows pen pressure: one pressure 0..1 for the stroke, or pressures with one value per point (a ramp tapers the line)."""
    source = {"x": source_x, "y": source_y} if source_x is not None and source_y is not None else None
    return text(call("brush.stroke", points=points, tool=tool, size=size, hardness=hardness, opacity=opacity, color=color, mask=mask, source=source, preset=preset, pressure=pressure, pressures=pressures, range=tone_range, protectTones=protect_tones, saturate=saturate))


@edit("Quick Mask")
def selection_quick_mask(on: Optional[bool] = None) -> str:
    """Quick Mask: enter (on=true) to edit the selection as a red overlay (paint it with brush_stroke mask=true: white selects, black masks; gradients, fills and blurs work too), then leave (on=false) to turn it back into the selection. Left out, it toggles."""
    return text(call("selection.quickMask", on=on))


@edit("Patch")
def pixels_patch(dx: float, dy: float) -> str:
    """Patch: replace the selection's pixels on the active layer with those dx, dy pixels away, their tone blended to meet the selection's edge (Photoshop's Patch tool). Select the blemish first (selection_rect, selection_polygon, ...)."""
    return text(call("pixels.patch", dx=dx, dy=dy))


@edit("Paint Bucket")
def pixels_bucket(x: float, y: float, color: Optional[str] = None, opacity: float = 1, tolerance: int = 32, contiguous: bool = True, antialias: bool = True, all_layers: bool = False) -> str:
    """Paint Bucket: fill the pixels like the one at x, y (document pixels) on the active layer (or its mask) with color (CSS; default the foreground), inside the selection. tolerance 0..255 as the Magic Wand's; contiguous only reaches connected pixels; all_layers compares with the document as shown instead of the active layer (an empty layer then fills the shape the click is in)."""
    return text(call("pixels.bucket", x=x, y=y, color=color, opacity=opacity, tolerance=tolerance, contiguous=contiguous, antialias=antialias, allLayers=all_layers))


@edit("Import brushes")
def brush_import(paths: list[str]) -> str:
    """Import brush files into the brush library: Photoshop .abr, Procreate .brushset and .brush, Clip Studio .sut, or images to use as tips. Answers the new preset ids (use them as brush_stroke's preset) and notes on anything approximated."""
    return text(call("brush.import", paths=[os.path.abspath(p) for p in paths]))


@look("Brush presets")
def brush_presets(group: Optional[str] = None) -> str:
    """The MyPaint brush presets brush_stroke can paint with (id, name, group, own size, whether it erases), optionally one group: Classic, David Revoy, Ramón Miranda, Tanda, Kaerhon, Brien Dieterle, Experimental."""
    return text(call("brush.presets", group=group))


@edit("Draw a gradient")
def gradient_draw(x0: float, y0: float, x1: float, y1: float, shape: str = "linear", style: str = "foreground-to-transparent", reversed: bool = False, opacity: float = 1.0, foreground: Optional[str] = None, background: Optional[str] = None) -> str:
    """Draw a gradient on the active layer from (x0, y0) to (x1, y1): shape linear or radial; style foreground-to-transparent or foreground-to-background; colours as CSS strings."""
    return text(call("gradient.draw", x0=x0, y0=y0, x1=x1, y1=y1, shape=shape, style=style, reversed=reversed, opacity=opacity, foreground=foreground, background=background))


@edit("Draw a shape")
def shape_draw(x: float, y: float, width: float, height: float, kind: str = "rectangle", corner_radius: float = 0, color: Optional[str] = None) -> str:
    """Add a filled rectangle (optionally rounded) or ellipse as a new shape layer."""
    return text(call("shape.draw", x=x, y=y, width=width, height=height, kind=kind, cornerRadius=corner_radius, color=color))


# ---- selection ----------------------------------------------------------------------------------

@look("Selection info")
def selection_info() -> str:
    """Whether there is a selection and its bounds."""
    return text(call("selection.info"))


@edit("Select a rectangle")
def selection_rect(x: float, y: float, width: float, height: float, ellipse: bool = False, mode: str = "replace") -> str:
    """Select a rectangle or ellipse (document pixels); mode replace, add or subtract."""
    return text(call("selection.rect", x=x, y=y, width=width, height=height, ellipse=ellipse, mode=mode))


@edit("Select a polygon")
def selection_polygon(points: list[list[float]], mode: str = "replace") -> str:
    """Select a polygon from [x, y] points."""
    return text(call("selection.polygon", points=points, mode=mode))


@edit("Magic wand")
def selection_wand(x: float, y: float, tolerance: int = 32, contiguous: bool = True, sample_all: bool = False, mode: str = "replace", edge_aware: Optional[bool] = None, refine_edge: Optional[bool] = None) -> str:
    """Magic wand: select the region around a point within tolerance (0..255), reading the active layer's own pixels (sample_all=true reads the visible composite instead). Edge-aware by default: shading and texture stay in and edges between similar colours hold; edge_aware=false is the classic per-channel tolerance. refine_edge (on) unmixes the edge so a line's fringe is partly selected; pixels_clear right after then leaves the line its own colour, without a rim of the background's (clearing a background around line art in one click)."""
    return text(call("selection.wand", x=x, y=y, tolerance=tolerance, contiguous=contiguous, sampleAll=sample_all, mode=mode, edgeAware=edge_aware, refineEdge=refine_edge))


@edit("Quick Select by scribble")
def selection_scribble(foreground: Optional[list[list[list[float]]]] = None, background: Optional[list[list[list[float]]]] = None, size: int = 24, refine: int = 8, clear: bool = True, mode: str = "replace") -> str:
    """Quick Select by scribble: strokes over the subject and over the background (each a list of [x, y] points, size pixels wide) segment the subject on the flattened document with GrabCut, refined onto the image's edges (refine 0..40); clear forgets earlier strokes first."""
    return text(call("selection.scribble", foreground=foreground or [], background=background or [], size=size, refine=refine, clear=clear, mode=mode))


@edit("Click to select")
def selection_subject(foreground: Optional[list[list[float]]] = None, background: Optional[list[list[float]]] = None, box: Optional[list[float]] = None, refine: int = 8, clear: bool = True, mode: str = "replace") -> str:
    """Click to select: foreground points on the subject and background points on what is not it (each [x, y] in document pixels), or a box [x0, y0, x1, y1]; the EfficientSAM model (needs its download, see app_info clickSelect) finds the object and the selection is pulled onto the image's edges (refine 0..40). Up to six prompts count. One foreground point on a subject usually selects all of it."""
    return text(call("selection.subject", foreground=foreground or [], background=background or [], box=box, refine=refine, clear=clear, mode=mode))


@edit("Select from a layer")
def selection_from_layer(id: str, mask: bool = False, mode: str = "replace") -> str:
    """Load a layer's opaque pixels (or its mask) as the selection."""
    return text(call("selection.fromLayer", id=id, mask=mask, mode=mode))


@edit("Modify the selection")
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

@edit("Resize the canvas")
def canvas_resize(width: int, height: int, anchor_x: float = 0.5, anchor_y: float = 0.5) -> str:
    """Change the canvas size without scaling pixels; the anchor (0..1) says which side stays put."""
    return text(call("canvas.resize", width=width, height=height, anchorX=anchor_x, anchorY=anchor_y))


@edit("Crop")
def canvas_crop(x: float, y: float, width: float, height: float) -> str:
    """Crop the document to a rectangle."""
    return text(call("canvas.crop", x=x, y=y, width=width, height=height))


@edit("Flip the canvas")
def canvas_flip(vertical: bool = False) -> str:
    """Flip the whole image left to right, or top to bottom with vertical=true."""
    return text(call("canvas.flip", vertical=vertical))


@edit("Trim the canvas")
def image_trim(based_on: str = "transparent", top: bool = True, bottom: bool = True, left: bool = True, right: bool = True,
               tolerance: int = 0) -> str:
    """Cut the canvas down to its content, as Photoshop's Image > Trim: based_on transparent (transparent pixels),
    topLeft or bottomRight (that corner's colour, within tolerance 0..255), on the sides chosen."""
    return text(call("image.trim", basedOn=based_on, top=top, bottom=bottom, left=left, right=right, tolerance=tolerance))


@edit("Resize the image")
def image_resize(width: Optional[int] = None, height: Optional[int] = None, scale: Optional[float] = None, sampling: str = "high") -> str:
    """Resample the whole image (every layer) to width x height (one keeps the aspect ratio) or by scale; sampling nearest, smooth or high."""
    return text(call("image.resize", width=width, height=height, scale=scale, sampling=sampling))


@edit("Undo")
def history_undo(steps: int = 1) -> str:
    """Undo the last edit(s)."""
    return text(call("history.undo", steps=steps))


@edit("Redo")
def history_redo(steps: int = 1) -> str:
    """Redo."""
    return text(call("history.redo", steps=steps))


@edit("Start an undo group")
def history_group_begin(name: str) -> str:
    """Start an edit group: the steps made until history_group_end become one undo step called name
    (e.g. "Retouch by agent"), so the person can take the whole change back at once. If the person edits
    meanwhile, the steps stay separate. Close it before finishing."""
    return text(call("history.beginGroup", name=name))


@edit("Finish the undo group")
def history_group_end() -> str:
    """Close the edit group and merge its steps into one undo step (the reply says how many, or why not)."""
    return text(call("history.endGroup"))


@outside("Run several calls", structured_output=False)
def batch(calls: list[dict], name: Optional[str] = None) -> list:
    """Run editor methods in order in one round trip, stopping at the first error: calls is a list of
    {"method": "layers.set", "params": {...}} using the editor's method names and camelCase keys
    (describe_method shows them). With name, the calls are one undo step and all or nothing: an error
    takes back what the earlier calls did. Rendered images in the results come back as images."""
    result = call("rpc.batch", calls=calls, name=name)
    images = []
    for r in result.get("results", []):
        if isinstance(r, dict) and "png" in r:
            images.append(png(r))
            r["png"] = f"(image {len(images)} below)"
    if "error" in result:
        raise RuntimeError(f"call {result['error']['index']} ({result['error']['method']}) failed: {result['error']['message']}"
                           + ("; the earlier calls were taken back" if result.get("rolledBack") else "")
                           + "\n" + text(result))
    return [text(result), *images]


# ---- what the person sees -----------------------------------------------------------------------

@edit("Pick a tool")
def tool_select(name: str) -> str:
    """Switch the tool the person sees (move, marquee, lasso, wand, quickselect, crop, brush, healing, clone, smudge, gradient, shape, text, eyedropper, hand, zoom). Tools that paint by coordinates do not need this."""
    return text(call("tool.select", name=name))


@edit("Set colours")
def colors_set(foreground: Optional[str] = None, background: Optional[str] = None) -> str:
    """Set the foreground and background colours (CSS), which painting, fills and gradients default to."""
    return text(call("colors.set", foreground=foreground, background=background))


@edit("Zoom the view")
def view_zoom(zoom: Optional[float] = None, fit: bool = False) -> str:
    """Zoom the person's view (1 = 100%) or fit the document in the window; the document is not changed."""
    return text(call("view.zoom", zoom=zoom, fit=fit))


# ---- prompts ------------------------------------------------------------------------------------

@mcp.prompt(title="Edit a photo in NekoPhoto")
def edit_photo(goal: str) -> str:
    """The working loop and recipes for editing an image with these tools."""
    return f"""Goal: {goal}

Work in NekoPhoto with this loop:
1. Look: document_overview (render=true for a picture). Open files with document_open first if nothing is open.
2. Act in small steps. Each tool call is one undo step; history_undo takes back one that went wrong.
3. Verify with render after each meaningful change; render a region at max_size 0 to check edges, text or a spot.
4. Finish with document_export (a .png/.jpg/.webp) or document_save (the editable project) only when asked.

Recipes:
- Cut out a subject: remove_background (app_info says whether its model is ready; if not, tell the person to
  enable it in Edit > Preferences), then layers_add kind pixels below=true for a new backdrop, gradient_draw on it.
- Non-destructive colour: layers_add kind adjustment (adjustments_defaults shows the settings shape), then
  adjustments_set to tweak after a render. pixels_adjust bakes the same into the pixels instead.
- One object: selection_subject with a foreground point on it (a box for a part of one object), then
  layers_mask action addFromSelection to keep it, or pixels_clear to remove it.
- A flat background colour: layers_select the layer, selection_wand on the colour, selection_edit grow 2,
  pixels_clear, selection_edit none.
- A blemish: render the region at full size, then brush_stroke tool healing through it.
- Drawings and line art: remove_background and selection_subject are made for photos and bleed on flat
  art. Clear a flat or baked-in checkerboard background with selection_wand (tolerance about 80) from a
  corner, adding each separate pocket with mode add; the outlines stop it. Separate touching figures with a
  selection_polygon through the gap, checked with render zoom 4.
- A photo object in a drawing: selection_from_layer mask=true, selection_edit grow 4, layers_add below=true,
  pixels_fill #111111 gives it the drawing's outline.

Things to know: filters, fills and adjustments act on the active layer inside the selection (selection_edit none
for the whole layer). Opacity is 0..1. Folders are Pass Through (children blend into what is below) unless given a blend mode, which isolates them as in Photoshop. brush_stroke puts the person's tool and colours
back afterwards. For a method without a tool, describe_method, then rpc."""


@outside("Call any method")
def rpc(method: str, params: Optional[dict] = None) -> str:
    """Call any editor method directly: the escape hatch for anything without a tool. describe_method lists the methods and what each takes."""
    return text(conn.call(method, params or {}))


if __name__ == "__main__":
    mcp.run()
