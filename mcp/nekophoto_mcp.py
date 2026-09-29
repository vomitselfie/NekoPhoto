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
                         /tmp/runtime-<user>/, never the shared /tmp itself; on Windows the named
                         pipe nekophoto-<user>)
  COMPOSITOR_BIN         binary to launch when nothing is listening (default: nekophoto on PATH,
                         else the build next to this file)
  COMPOSITOR_MCP_LAUNCH  "0" to never launch the app; "headless" to launch it without a window
"""
from __future__ import annotations

import base64
import getpass
import io
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
    if os.name == "nt":
        return "nekophoto-" + (os.environ.get("USERNAME") or "user")
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


class Connection:
    def __init__(self) -> None:
        self.sock: Any = None
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
            candidate = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "build", "src", "app",
                                     "nekophoto.exe" if os.name == "nt" else "nekophoto")
            if os.path.isfile(candidate) and os.access(candidate, os.X_OK):
                binary = candidate
        if not binary:
            raise RuntimeError(f"nothing listening at {path} and no nekophoto binary found; set COMPOSITOR_BIN")
        args = [binary, "--rpc", "--rpc-socket", path]
        if mode == "headless" or (os.name != "nt" and not (os.environ.get("WAYLAND_DISPLAY") or os.environ.get("DISPLAY"))):
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
        self.sock, self.file = open_local_socket(path)

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
def document_open(path: str, page: Optional[int] = None, resolution: Optional[float] = None) -> str:
    """Open a .comp project (in its own tab); a layered file in its own tab, the reply listing its layers and what could
    not be carried: Photoshop .psd/.psb, Clip Studio .clip, Affinity .afphoto/.afdesign/.afpub/.af, Aseprite .ase/.aseprite (first frame), SVG .svg/.svgz (shapes as editable vector shape layers, the rest as pixels),
    a PDF page (page, 1-based; resolution in ppi, default 150; when app_info reports pdf), an icon .ico/.cur (a
    layer per size) or an animated GIF (a layer per frame, frame 1 visible); or an image file, .tga and camera RAW files included (CR2, NEF, ARW, DNG, ...; developed with the
    camera white balance), as a layer (a first image creates the canvas)."""
    return text(call("document.open", path=os.path.abspath(path)))
    return text(call("document.open", path=os.path.abspath(path), page=page, resolution=resolution))


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
def document_export(path: str, quality: int = 85, background: str = "#ffffff", embed_profile: bool = True,
                    convert_to_srgb: Optional[bool] = None) -> str:
    """Flatten and export to a .png, .webp, .tif or .tga (these keep transparency; WebP at quality 100 is
    lossless), a .ico (16, 32, 48 and 256 px sizes), or .jpg (over background, at quality); .psd/.psb keep layers;
    an .svg writes vector shape layers as paths, folders as groups and every other layer as an embedded PNG; a .gif
    writes the timeline's frames as an animated GIF (the composite when there are none). PNG, JPEG, WebP, TIFF and PSD
    carry the document's colour profile (embed_profile=false leaves it out); convert_to_srgb converts the pixels to
    sRGB first, for the web (the default for GIF)."""
    return text(call("document.export", path=os.path.abspath(path), quality=quality, background=background,
                     embedProfile=embed_profile, convertToSrgb=convert_to_srgb))


# ---- actions and the timeline --------------------------------------------------------------------

@look("Actions")
def actions_list(name: Optional[str] = None) -> str:
    """The recorded actions (Window > Actions): each one's steps as automation requests (method, params, enabled,
    label), whether one is recording, and the library file. name lists one action."""
    return text(call("actions.list", name=name))


@outside("Record an action")
def actions_record(action: str, name: Optional[str] = None) -> str:
    """action start (with name: the action to record into, created or appended to) or stop. While recording, every
    editing request and the person's menu commands, dialogs and brush strokes become steps."""
    return text(call("actions.record", action=action, name=name))


@edit("Play an action")
def actions_play(name: str, times: int = 1) -> str:
    """Play an action's enabled steps on the current document, stopping at the first error (the reply says which
    step); steps that stay in one document become one undo step named after the action."""
    return text(call("actions.play", name=name, times=times))


@outside("Batch a folder through an action")
def actions_batch(name: str, input: str, output: str, format: str = "png", overwrite: bool = False) -> str:
    """File > Automate > Batch: every image, PSD or project in the input folder opened in a tab of its own, the
    action played, the result exported to the output folder as format (png, jpg, webp, tif, psd, gif, tga) and
    the tab closed. The reply lists what was written, failed and skipped."""
    return text(call("actions.batch", name=name, input=os.path.abspath(input), output=os.path.abspath(output), format=format, overwrite=overwrite))


@outside("Save an action")
def actions_save(name: str, steps: list[dict]) -> str:
    """Create an action or replace the one with this name: steps are {"method", "params", "enabled"} objects as
    actions_list shows them (how to edit, reorder or switch off steps)."""
    return text(call("actions.save", name=name, steps=steps))


@outside("Delete an action")
def actions_delete(name: str) -> str:
    """Delete an action from the library."""
    return text(call("actions.delete", name=name))


@outside("Import actions")
def actions_import(path: str) -> str:
    """Import actions from a JSON file written by actions_export (names already used get a number)."""
    return text(call("actions.import", path=os.path.abspath(path)))


@outside("Export actions")
def actions_export(path: str, name: Optional[str] = None, names: Optional[list[str]] = None) -> str:
    """Write actions (one, several, or all) to a JSON file."""
    return text(call("actions.export", path=os.path.abspath(path), name=name, names=names))


@look("Timeline")
def timeline_info() -> str:
    """The frame animation (Window > Timeline): each frame's delay (ms) and visible layer ids, the current frame
    (-1 without frames) and the loop count (0 forever)."""
    return text(call("timeline.info"))


@edit("Change the frames")
def timeline_frame(action: str, index: Optional[int] = None, to: Optional[int] = None) -> str:
    """action create (the first frame from the layers as they are), fromLayers (a frame per top-level layer),
    duplicate (a copy of the current frame after it), select (the layers then show that frame; changes to layer
    visibility, position and opacity go into it), delete, move (index to to) or clear. Export frames with
    document_export to a .gif."""
    return text(call("timeline.frame", action=action, index=index, to=to))


@edit("Set frame delay and looping")
def timeline_set(index: Optional[int] = None, delay: Optional[int] = None, loop_count: Optional[int] = None) -> str:
    """Set a frame's delay in milliseconds (index defaults to the current frame; -1 sets every frame) and how many
    times the animation plays (loop_count 0 forever)."""
    return text(call("timeline.set", index=index, delay=delay, loopCount=loop_count))


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


@edit("Style letters of a text layer")
def text_style_range(id: Optional[str] = None, start: Optional[int] = None, length: Optional[int] = None, font: Optional[str] = None, size: Optional[float] = None,
                     bold: Optional[bool] = None, weight: Optional[int] = None, italic: Optional[bool] = None, color: Optional[str] = None,
                     letter_spacing: Optional[float] = None, baseline_shift: Optional[float] = None, leading: Optional[float] = None, caps: Optional[str] = None,
                     underline: Optional[bool] = None, strikethrough: Optional[bool] = None) -> str:
    """Style some letters of a text layer (the active layer, or id), like Photoshop's Character panel on a selection: start and length count UTF-16 units of the text (default all of it); font, size (px), bold, weight (100..900, 0 for auto), italic, color (CSS), letter_spacing (tracking, px), baseline_shift (px up), leading (px, 0 auto), caps (normal, small, all), underline, strikethrough. Only the fields given change; layers_get lists the resulting text.runs."""
    return text(call("text.styleRange", id=id, start=start, length=length, font=font, size=size, bold=bold, weight=weight, italic=italic, color=color,
                     letterSpacing=letter_spacing, baselineShift=baseline_shift, leading=leading, caps=caps, underline=underline, strikethrough=strikethrough))


@edit("Delete layers")
def layers_delete(ids: list[str]) -> str:
    """Delete layers by id (layers clipped to them keep their masked look baked in)."""
    return text(call("layers.delete", ids=ids))


@edit("Duplicate a layer")
def layers_duplicate(id: str) -> str:
    """A copy of the layer directly above it; the copy becomes active."""
    return text(call("layers.duplicate", id=id))


@outside("Copy layers")
def layers_copy(ids: Optional[list[str]] = None) -> str:
    """Copy whole layers (default the selected ones) to the layer clipboard, as Edit > Copy with no selection:
    masks, styles, text, shapes, smart objects and adjustments come along. layers_paste puts them in any open document."""
    return text(call("layers.copy", **({"ids": ids} if ids else {})))


@edit("Paste layers")
def layers_paste() -> str:
    """Paste the copied layers into the current document above the active layer (one undo step); answers their ids."""
    return text(call("layers.paste"))


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
    """Place a layer non-destructively: top-left x, y and width, height in document pixels, rotation in degrees clockwise, or scale (a factor about its centre). For a folder the values are its contents' box and everything inside moves with it."""
    return text(call("layers.setTransform", id=id, x=x, y=y, width=width, height=height, rotation=rotation, scale=scale, flipX=flip_x, flipY=flip_y))


@look("Layer style")
def layers_style(id: str) -> str:
    """A layer's effects (Photoshop's layer style): dropShadows, innerShadows, outerGlows, innerGlows, bevels, satins, colorOverlays, gradientOverlays, patternOverlays and strokes, each a list (switched-off ones too, with enabled false), plus visible, maskHidesEffects and blendInteriorAsGroup."""
    return text(call("layers.style", id=id))


@edit("Set layer style")
def layers_set_style(id: str, style: dict) -> str:
    """Replace a layer's effects, shaped as layers_style shows; settings left out take Photoshop's defaults and {} clears the style. Colours are "#rrggbb"; opacity, scale and depth are fractions (1 = 100%); spread, choke and range percent; sizes and distances pixels; angles degrees; mode a blend mode (normal, multiply, screen, overlay, linearDodge, ...). Example: {"dropShadows": [{"distance": 8, "size": 10}], "strokes": [{"size": 3, "color": "#ffffff", "position": "outside"}]}."""
    return text(call("layers.setStyle", id=id, style=style))


@edit("Apply a style preset")
def layers_apply_style(id: str, style: str) -> str:
    """Give a layer an imported style preset by name (presets_list): its effects replace the layer's, and the document gets the patterns the style uses. Answers the layer's style as layers_style shows it."""
    return text(call("layers.applyStyle", id=id, style=style))


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


@look("List artboards")
def artboards_list() -> str:
    """The artboards: folders with a rectangle and a background that clip their layers (Photoshop's artboards)."""
    return text(call("artboards.list"))


@edit("Add an artboard")
def artboards_add(width: int, height: int, x: int = 0, y: int = 0, name: Optional[str] = None, background: Optional[str] = None) -> str:
    """A new, empty artboard at the top of the stack; background is white, black, transparent or a CSS colour.
    Put layers inside it with layers_move (parent)."""
    return text(call("artboards.add", x=x, y=y, width=width, height=height, name=name, background=background))


@edit("Change an artboard")
def artboards_set(id: str, x: Optional[int] = None, y: Optional[int] = None, width: Optional[int] = None, height: Optional[int] = None,
                  name: Optional[str] = None, background: Optional[str] = None, move_contents: bool = True) -> str:
    """Move, resize, rename or recolour an artboard; moving it takes its layers along unless move_contents is false."""
    return text(call("artboards.set", id=id, x=x, y=y, width=width, height=height, name=name, background=background, moveContents=move_contents))


@edit("Remove an artboard")
def artboards_delete(id: str, contents: bool = False) -> str:
    """Turn an artboard back into a plain folder, or with contents=true delete it and its layers."""
    return text(call("artboards.delete", id=id, contents=contents))


@outside("Export artboards to files")
def artboards_export(directory: str, format: str = "png", prefix: Optional[str] = None, quality: int = 90) -> str:
    """Each visible artboard as its own PNG or JPEG in directory, named after the artboard."""
    return text(call("artboards.export", directory=os.path.abspath(directory), format=format, prefix=prefix, quality=quality))


@look("List slices")
def slices_list() -> str:
    """The slices: named rectangles for export, kept in PSDs as Photoshop's slices."""
    return text(call("slices.list"))


@edit("Add a slice")
def slices_add(width: int, height: int, x: int = 0, y: int = 0, name: Optional[str] = None, url: Optional[str] = None,
               target: Optional[str] = None, alt_tag: Optional[str] = None) -> str:
    """A new user slice."""
    return text(call("slices.add", x=x, y=y, width=width, height=height, name=name, url=url, target=target, altTag=alt_tag))


@edit("Change a slice")
def slices_set(id: int, x: Optional[int] = None, y: Optional[int] = None, width: Optional[int] = None, height: Optional[int] = None,
               name: Optional[str] = None, url: Optional[str] = None, target: Optional[str] = None, alt_tag: Optional[str] = None) -> str:
    """Change a slice's rectangle, name, link or alt text."""
    return text(call("slices.set", id=id, x=x, y=y, width=width, height=height, name=name, url=url, target=target, altTag=alt_tag))


@edit("Delete a slice")
def slices_delete(id: int) -> str:
    """Delete a slice by its id."""
    return text(call("slices.delete", id=id))


@outside("Export slices")
def slices_export(directory: str, format: str = "png", prefix: Optional[str] = None, quality: int = 90) -> str:
    """Each slice as its own PNG or JPEG in directory, named after the slice."""
    return text(call("slices.export", directory=os.path.abspath(directory), format=format, prefix=prefix, quality=quality))


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


@look("List Smart Filters")
def smart_object_filters(id: Optional[str] = None) -> str:
    """A smart object's Smart Filters: the stack's switch, its shared mask, and each filter in running order (index 0
    is applied first) with its settings, switch, opacity and blend. drawn false marks one NekoPhoto does not draw
    (the stack is then read-only)."""
    return text(call("smartObject.filters", id=id))


@edit("Change a Smart Filter")
def smart_object_set_filter(id: Optional[str] = None, index: Optional[int] = None, enabled: Optional[bool] = None,
                            radius: Optional[float] = None, threshold: Optional[float] = None, amount: Optional[float] = None,
                            angle: Optional[float] = None, distance: Optional[float] = None, cell_size: Optional[float] = None,
                            height: Optional[float] = None, opacity: Optional[float] = None, blend: Optional[str] = None) -> str:
    """Change one Smart Filter (index from smart_object_filters): its settings (only those the filter uses), enabled,
    opacity (percent) and blend mode. Without index, enabled switches the whole stack. One undo step."""
    return text(call("smartObject.setFilter", id=id, index=index, enabled=enabled, radius=radius, threshold=threshold, amount=amount,
                     angle=angle, distance=distance, cellSize=cell_size, height=height, opacity=opacity, blend=blend))


@edit("Remove Smart Filters")
def smart_object_remove_filter(id: Optional[str] = None, index: Optional[int] = None, all: bool = False) -> str:
    """Delete one Smart Filter (index), or all of them (all=True, Clear Smart Filters)."""
    return text(call("smartObject.removeFilter", id=id, index=index, all=all or None))


@edit("Reorder Smart Filters")
def smart_object_move_filter(index: int, to: int, id: Optional[str] = None) -> str:
    """Move a Smart Filter to another place in the running order (0 runs first)."""
    return text(call("smartObject.moveFilter", id=id, index=index, to=to))


@edit("Smart Filter mask")
def smart_object_filter_mask(action: str, id: Optional[str] = None, show: bool = False) -> str:
    """The Smart Filters' shared mask: enable, disable, invert, delete (all white), select (then brush_stroke with
    mask=True, fills, gradients and filters paint it; erase=True paints black) or deselect."""
    return text(call("smartObject.filterMask", id=id, action=action, show=show or None))


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


@edit("Mosh effect")
def pixels_mosh(effect: str, params: Optional[dict] = None, seed: float = 0, layer: Optional[str] = None,
                caption: Optional[str] = None) -> str:
    """Filter > Mosh: one of OpenMosh's effects on the active layer's pixels, inside the selection. effect is an OpenMosh
    id. Glitch: soft-glitch, hard-glitch, decimate, data-mosh, splitter, jitter, slices, shake, pixel-sort, strobe.
    Distort: wave, bulge, stretch, push, luma-mesh, transform-3d, tile, kaleidoscope, mirror, wobble, smear, twirl,
    optical-flow. Retro: pixelate, scanlines, vhs, super8, cga-8bit, crt, dither, bad-tv, dot-screen, halftone, ascii.
    Stylize: bleach, edges, emboss, vignette, noise-displace, watercolor, zoom-blur, glow, light-streak, feedback.
    Color: color-correction, duotone, solarize, chromatic-warp, sepia. Composite: overlay, mask, mask-blocks, chroma-key,
    caption. params takes OpenMosh's keys (pixel-sort: low, high, reverse, vertical; vhs: tracking, bleed, noise; ...),
    numbers, booleans for switches, an index or option name for a choice; the rest keep their defaults. seed (0..100)
    picks the random pattern of the seeded effects; the same seed repeats it. overlay and mask need layer, the id of the
    layer they read (where it lies over the active one); caption needs caption, its text. The reply has the settings
    applied."""
    extra = {}
    if layer is not None:
        extra["layer"] = layer
    if caption is not None:
        extra["text"] = caption
    return text(call("pixels.mosh", effect=effect, params=params or {}, seed=seed, **extra))


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
def pixels_content_aware_fill(sampling: str = "auto", include: list | None = None, exclude: list | None = None, output: str = "current") -> str:
    """Fill the selection from its surroundings (also extends an image past its edge when the selection reaches outside it).
    sampling: auto (around the selection), all (anywhere on the layer) or custom (the include rectangles, whole canvas when none, less the exclude rectangles; each {x, y, width, height}).
    output: current (fill the active layer) or new (only the filled pixels on a new layer)."""
    params: dict = {"sampling": sampling, "output": output}
    if include is not None:
        params["include"] = include
    if exclude is not None:
        params["exclude"] = exclude
    return text(call("pixels.contentAwareFill", **params))


@edit("Content-Aware Move")
def pixels_content_aware_move(dx: float, dy: float, mode: str = "move", adaptation: int = 2) -> str:
    """Content-Aware Move (Photoshop's tool): the selected pixels move dx, dy; the hole left behind is filled from its surroundings and the patch blended into its new place; the selection follows. mode extend keeps the original and adds the copy. adaptation 0 (very strict) .. 4 (very loose). Select the object first (selection_rect, selection_polygon, selection_subject)."""
    return text(call("pixels.contentAwareMove", dx=dx, dy=dy, mode=mode, adaptation=adaptation))


@edit("Content-Aware Scale")
def pixels_content_aware_scale(width: int = 0, height: int = 0, width_percent: float = 100, height_percent: float = 100, protect_selection: bool = False) -> str:
    """Content-Aware Scale the active layer (seam carving): width/height in pixels, or widthPercent/heightPercent; protect_selection keeps the selected pixels."""
    params: dict = {"protectSelection": protect_selection}
    if width: params["width"] = width
    else: params["widthPercent"] = width_percent
    if height: params["height"] = height
    else: params["heightPercent"] = height_percent
    return text(call("pixels.contentAwareScale", **params))


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
def brush_stroke(points: list[list[float]], tool: str = "brush", size: Optional[float] = None, hardness: Optional[float] = None, opacity: Optional[float] = None, color: Optional[str] = None, mask: bool = False, source_x: Optional[float] = None, source_y: Optional[float] = None, preset: Optional[str] = None, pressure: Optional[float] = None, pressures: Optional[list[float]] = None, tilts: Optional[list[list[float]]] = None, twists: Optional[list[float]] = None, times: Optional[list[float]] = None, seed: Optional[int] = None, view_scale: Optional[float] = None, tone_range: Optional[str] = None, protect_tones: Optional[bool] = None, saturate: Optional[bool] = None, smoothing: Optional[float] = None, pulled_string: Optional[bool] = None, stroke_catch_up: Optional[bool] = None, catch_up_on_end: Optional[bool] = None, adjust_for_zoom: Optional[bool] = None, input_smoothing: Optional[float] = None, pressure_smoothing: Optional[float] = None) -> str:
    """Paint one stroke through [x, y] points on the active layer (or its mask with mask=true): tool brush, eraser, healing (spot healing), healingbrush or clone (both with source_x/source_y: healingbrush matches the copied texture to the tone around the stroke), smudge, blur, sharpen or liquify, or dodge / burn (lighten / darken; tone_range shadows, midtones or highlights, protect_tones keeps the colour) and sponge (desaturates, or saturates with saturate=true), where opacity is the Exposure or Flow; size in pixels, hardness and opacity 0..1, a CSS color.
    With tool brush or eraser, preset picks a MyPaint brush from brush_presets (pencils, inks, charcoal, paint, smudging; "round" for the plain tip); it starts at its own size unless size is given, and follows pen pressure: one pressure 0..1 for the stroke, or pressures with one value per point (a ramp tapers the line). A pen's tilts ([tiltX, tiltY] degrees per point), twists (barrel rotation, degrees per point) and times (seconds per point) replay a recorded stroke; seed repeats an imported tip brush's jitter; view_scale is the zoom the stroke is taken as drawn at (2 for 200%, default 1), which speed-on-screen dynamics read.
    Smoothing (brush and eraser, none unless given): smoothing 0..100 is Photoshop's stabiliser (the line trails the pen), with pulled_string (moves only once the string is taut), stroke_catch_up (keeps closing on a paused pen, default on), catch_up_on_end (the line ends where the pen lifted) and adjust_for_zoom (default on); input_smoothing 0..100 filters tablet jitter with little lag; pressure_smoothing 0..100 filters the pressure alone."""
    source = {"x": source_x, "y": source_y} if source_x is not None and source_y is not None else None
    return text(call("brush.stroke", points=points, tool=tool, size=size, hardness=hardness, opacity=opacity, color=color, mask=mask, source=source, preset=preset, pressure=pressure, pressures=pressures, tilts=tilts, twists=twists, times=times, seed=seed, viewScale=view_scale, range=tone_range, protectTones=protect_tones, saturate=saturate,
                     smoothing=smoothing, pulledString=pulled_string, strokeCatchUp=stroke_catch_up, catchUpOnEnd=catch_up_on_end, adjustForZoom=adjust_for_zoom, inputSmoothing=input_smoothing, pressureSmoothing=pressure_smoothing))


@look("Warp cage")
def layers_cage(id: str) -> str:
    """A layer's warp cage: the 16 [x, y] control points (row by row, document pixels) of the 4 x 4 Bezier mesh over it."""
    return text(call("layers.cage", id=id))


@edit("Warp through a cage")
def layers_set_cage(id: str, points: list[list[float]]) -> str:
    """Warp a layer freely (Photoshop's Custom warp): move some of the 16 points layers_cage gives and pass all 16 back.
    Corners are points 0, 3, 12 and 15; the rest shape the edges and the inside. Pixels bend for good; a smart object keeps an editable warp."""
    return text(call("layers.setCage", id=id, points=points))


@edit("Quick Mask")
def selection_quick_mask(on: Optional[bool] = None) -> str:
    """Quick Mask: enter (on=true) to edit the selection as a red overlay (paint it with brush_stroke mask=true: white selects, black masks; gradients, fills and blurs work too), then leave (on=false) to turn it back into the selection. Left out, it toggles."""
    return text(call("selection.quickMask", on=on))


@look("Channels")
def channels_list() -> str:
    """The Channels panel: which colour channels edits write to (activeColors) and the canvas shows, the alpha channel being edited (target), Quick Mask, and every alpha and spot channel (id, name, kind, colour, opacity, colorIndicates, visible)."""
    return text(call("channels.list"))


@edit("Channel")
def channels_new(name: Optional[str] = None, from_selection: bool = False, duplicate: Optional[str] = None, delete: Optional[str] = None) -> str:
    """Add an alpha channel (black, or the selection with from_selection), duplicate the channel whose id is `duplicate`, or delete the one whose id is `delete`. Alpha channels are saved selections."""
    if delete:
        return text(call("channels.delete", id=delete))
    if duplicate:
        return text(call("channels.duplicate", id=duplicate, name=name))
    return text(call("channels.new", name=name, fromSelection=from_selection))


@edit("Target channel")
def channels_select(channel: str, extend: bool = False) -> str:
    """Make a channel the target: rgb (the composite, the usual case), red, green or blue (painting, fills, adjustments, filters and paste then change only that channel, shown alone in grey), or an alpha channel's id (paint it with brush_stroke mask=true: white selects). extend adds a colour channel (Shift-click)."""
    return text(call("channels.select", channel=channel, extend=extend))


@edit("Channel options")
def channels_set(channel: str, name: Optional[str] = None, color: Optional[str] = None, opacity: Optional[float] = None, color_indicates: Optional[str] = None, index: Optional[int] = None, visible: Optional[bool] = None) -> str:
    """Channel Options and the eye: rename, overlay color (#rrggbb) and opacity 0..1, color_indicates masked or selected, move to index, show or hide. channel is an alpha or spot channel's id, or rgb/red/green/blue for visibility only."""
    return text(call("channels.set", channel=channel, name=name, color=color, opacity=opacity, colorIndicates=color_indicates, index=index, visible=visible))


@edit("Save selection")
def channels_save_selection(id: Optional[str] = None, name: Optional[str] = None, mode: str = "replace") -> str:
    """Select > Save Selection: into a new alpha channel (name), or into channel id combined with mode replace/add/subtract/intersect."""
    return text(call("channels.saveSelection", id=id, name=name, mode=mode))


@edit("Load selection")
def channels_load_selection(channel: Optional[str] = None, layer: Optional[str] = None, mask: bool = False, invert: bool = False, mode: str = "replace") -> str:
    """Select > Load Selection: from an alpha channel's id, rgb (the composite's luminosity), red/green/blue, or a layer's transparency (layer) or mask (layer, mask=true); invert first if asked; mode replace/add/subtract/intersect."""
    return text(call("channels.loadSelection", channel=channel, layer=layer, mask=mask if layer else None, invert=invert, mode=mode))


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
def gradient_draw(x0: float, y0: float, x1: float, y1: float, shape: str = "linear", style: str = "foreground-to-transparent", reversed: bool = False, opacity: float = 1.0, foreground: Optional[str] = None, background: Optional[str] = None, preset: Optional[str] = None) -> str:
    """Draw a gradient on the active layer from (x0, y0) to (x1, y1): shape linear or radial; style foreground-to-transparent or foreground-to-background, or preset, an imported gradient's name from presets_list (its foreground and background stops take the colours); colours as CSS strings."""
    return text(call("gradient.draw", x0=x0, y0=y0, x1=x1, y1=y1, shape=shape, style=style, reversed=reversed, opacity=opacity, foreground=foreground, background=background, preset=preset))


@edit("Import presets")
def presets_import(paths: list[str]) -> str:
    """Import Photoshop preset files into the preset library: layer styles (.asl, with the patterns they use), patterns (.pat, also added to the open document for pattern overlays and bevel textures) and gradients (.grd). Answers the names imported and notes on anything left out."""
    return text(call("presets.import", paths=[os.path.abspath(p) for p in paths]))


@look("Presets")
def presets_list(kind: Optional[str] = None) -> str:
    """The imported presets: styles (for layers_apply_style), gradients (for gradient_draw's preset, with their stops) and patterns (id, name, size); kind styles, gradients or patterns for one list."""
    return text(call("presets.list", kind=kind))


@outside("Remove a preset")
def presets_remove(kind: str, name: str) -> str:
    """Remove an imported style, gradient or pattern from the library (kind style, gradient or pattern; a pattern by id or name); undo does not bring it back."""
    return text(call("presets.remove", kind=kind, name=name))


@edit("Draw a shape")
def shape_draw(x: float, y: float, width: float = 0, height: float = 0, kind: str = "rectangle", corner_radius: float = 0, sides: int = 5, star: Optional[float] = None,
               x2: Optional[float] = None, y2: Optional[float] = None, weight: float = 4, name: Optional[str] = None, color: Optional[str] = None, fill: bool = True,
               stroke_width: Optional[float] = None, stroke_color: Optional[str] = None, stroke_align: Optional[str] = None, stroke_dashes: Optional[list[float]] = None,
               fill_type: Optional[str] = None, gradient: Optional[str] = None, gradient_type: Optional[str] = None, gradient_angle: Optional[float] = None,
               pattern: Optional[str] = None, stroke_type: Optional[str] = None, stroke_gradient: Optional[str] = None, stroke_pattern: Optional[str] = None,
               op: Optional[str] = None) -> str:
    """Add a vector shape layer (stays editable, and is a Photoshop shape layer in PSD exports). kind rectangle (corner_radius), ellipse, polygon (sides), star (sides, star = inset 0..0.99),
    line (from x, y to x2, y2, weight pixels) or custom (name: Heart, Star, Arrow, Speech Bubble, Check Mark, Lightning), in box x, y, width, height. color fills it (default the foreground);
    fill=false with a stroke gives an outline; stroke_width / stroke_color / stroke_align (inside, center, outside) / stroke_dashes (in stroke widths, e.g. [4, 2]) stroke it.
    fill_type gradient (gradient: a preset name, empty for foreground to background; gradient_type linear/radial/angle/reflected/diamond; gradient_angle degrees) or pattern
    (pattern: one of the document's patterns) paints the fill; stroke_type / stroke_gradient / stroke_pattern the stroke. op (combine, subtract, intersect, exclude) adds the
    outline to the active shape layer instead, combined that way. Rectangles and ellipses keep live properties (shape_set live=...)."""
    return text(call("shape.draw", x=x, y=y, width=width, height=height, kind=kind, cornerRadius=corner_radius, sides=sides, star=star, x2=x2, y2=y2, weight=weight, name=name,
                     color=color, fill=fill, strokeWidth=stroke_width, strokeColor=stroke_color, strokeAlign=stroke_align, strokeDashes=stroke_dashes,
                     fillType=fill_type, gradient=gradient, gradientType=gradient_type, gradientAngle=gradient_angle, pattern=pattern,
                     strokeType=stroke_type, strokeGradient=stroke_gradient, strokePattern=stroke_pattern, op=op))


@look("Shape")
def shape_get(id: str) -> str:
    """A vector shape layer's path (subpaths of [inX, inY, x, y, outX, outY] knots, document pixels), fill and stroke."""
    return text(call("shape.get", id=id))


@edit("Edit shape")
def shape_set(id: str, path: Optional[list] = None, color: Optional[str] = None, fill: Optional[bool] = None, stroke: Optional[bool] = None, stroke_width: Optional[float] = None,
              stroke_color: Optional[str] = None, stroke_align: Optional[str] = None, stroke_dashes: Optional[list[float]] = None,
              fill_type: Optional[str] = None, gradient: Optional[str] = None, gradient_type: Optional[str] = None, gradient_angle: Optional[float] = None,
              pattern: Optional[str] = None, stroke_type: Optional[str] = None, stroke_gradient: Optional[str] = None, stroke_pattern: Optional[str] = None,
              live: Optional[dict] = None) -> str:
    """Change a vector shape layer: its path (as shape_get gives it; a knot may be just [x, y] for a corner), fill colour, fill on/off, or stroke; fill_type / gradient /
    pattern and stroke_type / stroke_gradient / stroke_pattern as shape_draw's. live changes a live rectangle's or ellipse's properties
    {group, x, y, width, height, radius, radii: [topLeft, topRight, bottomRight, bottomLeft]} (shape_get lists them; they end once the path is edited directly)."""
    return text(call("shape.set", id=id, path=path, color=color, fill=fill, stroke=stroke, strokeWidth=stroke_width, strokeColor=stroke_color, strokeAlign=stroke_align, strokeDashes=stroke_dashes,
                     fillType=fill_type, gradient=gradient, gradientType=gradient_type, gradientAngle=gradient_angle, pattern=pattern,
                     strokeType=stroke_type, strokeGradient=stroke_gradient, strokePattern=stroke_pattern, live=live))


@look("Paths")
def paths_list() -> str:
    """The document's paths (Photoshop's Paths panel): the Work Path (id 1025) and saved paths, with their knots."""
    return text(call("paths.list"))


@edit("Set path")
def paths_set(path: list, id: Optional[int] = None, name: Optional[str] = None, work: bool = False) -> str:
    """Make a saved path (name), the Work Path (work=true), or replace path id. path is a list of subpaths {closed, knots: [[x, y] or [inX, inY, x, y, outX, outY], ...]}."""
    return text(call("paths.set", path=path, id=id, name=name, work=work))


@edit("Use path")
def paths_apply(id: int, action: str, mode: str = "replace") -> str:
    """Use a path: action fill (foreground colour on the active layer), stroke (brush size), select (load as selection with mode replace/add/subtract/intersect),
    shape (a new vector shape layer), delete, or choose (target it for the Pen and Direct Selection)."""
    methods = {"fill": "paths.fill", "stroke": "paths.stroke", "select": "paths.toSelection", "shape": "paths.toShape", "delete": "paths.delete", "choose": "paths.select"}
    if action not in methods:
        return "action must be one of " + ", ".join(methods)
    return text(call(methods[action], id=id, mode=mode) if action == "select" else call(methods[action], id=id))


@edit("Anchor point")
def paths_anchor(x: float, y: float, action: str = "add", radius: float = 6) -> str:
    """Add an anchor on the target path's outline at x, y (the curve is split, its shape unchanged), or delete the anchor there (action delete).
    The target path is the one chosen with paths_apply(action=choose), or else the active shape layer's."""
    if action not in ("add", "delete"):
        return "action must be add or delete"
    return text(call("paths.addAnchor" if action == "add" else "paths.deleteAnchor", x=x, y=y, radius=radius))


@edit("Path operation")
def paths_operation(action: str = "set", subpath: int = 0, op: str = "combine") -> str:
    """Photoshop's path operations on the target path (the chosen path, a targeted vector mask, or the active shape layer's): action set changes the component
    holding subpath to combine, subtract, intersect or exclude with those before it; action merge is Merge Shape Components (add-only outlines, curves as corners)."""
    if action == "merge":
        return text(call("paths.mergeComponents"))
    if action != "set":
        return "action must be set or merge"
    return text(call("paths.setOperation", subpath=subpath, op=op))


@edit("Vector mask")
def vector_mask(id: str, action: str = "get", mode: Optional[str] = None, path: Optional[list] = None, inverted: Optional[bool] = None) -> str:
    """A layer's own vector mask (Layer > Vector Mask). action get reads it; set makes or replaces it (mode revealAll, hideAll, currentPath = the path paths_apply chose,
    or path = subpaths as paths_list gives them; inverted hides inside); delete removes it; target makes it the path the Pen, Direct Selection and paths_anchor edit."""
    methods = {"get": "vectorMask.get", "set": "vectorMask.set", "delete": "vectorMask.delete", "target": "vectorMask.target"}
    if action not in methods:
        return "action must be get, set, delete or target"
    if action == "set":
        return text(call("vectorMask.set", id=id, mode=mode, path=path, inverted=inverted))
    return text(call(methods[action], id=id))


@edit("Text to path")
def text_to_path(id: str, shape: bool = False) -> str:
    """Type > Create Work Path: a text layer's glyph outlines as the Work Path (id 1025); shape=true is Type > Convert to Shape (the text layer becomes a shape layer)."""
    return text(call("text.toShape" if shape else "text.toPath", id=id))


@edit("Path from selection")
def paths_from_selection(tolerance: float = 1.0) -> str:
    """Make the Work Path (id 1025) from the selection's outline; tolerance in pixels trades points for accuracy."""
    return text(call("paths.fromSelection", tolerance=tolerance))


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
def canvas_crop(x: float, y: float, width: float, height: float, ratio: str | None = None) -> str:
    """Crop the document to a rectangle. ratio (W:H such as "16:9") crops to the largest box of that shape
    centred in the rectangle, as the Crop tool's ratio presets do."""
    return text(call("canvas.crop", x=x, y=y, width=width, height=height, **({"ratio": ratio} if ratio else {})))


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


@edit("Assign or convert the colour profile")
def document_profile(action: str = "get", profile: Optional[str] = None, intent: Optional[str] = None,
                     black_point_compensation: Optional[bool] = None) -> str:
    """The document's colour profile. action=get reads it; assign (Edit > Assign Profile) changes only the tag, so the
    same values look different; convert (Edit > Convert to Profile) converts every layer's pixels and the stored colours
    so the document looks the same. profile: srgb, adobe-rgb, display-p3, prophoto, working, none (assign: untagged,
    treated as sRGB) or an ICC file's path; intent perceptual or relative (default). One undo step."""
    if profile is not None and os.path.exists(profile):
        profile = os.path.abspath(profile)
    return text(call("document.profile", action=action, profile=profile, intent=intent, blackPointCompensation=black_point_compensation))


@edit("Change the bit depth")
def image_mode(bits: int) -> str:
    """Image > Mode: convert the document to 8 or 16 bits per channel (every layer, mask and the selection, one undo
    step). A 16-bit document holds half the pixels of an 8-bit one in the same memory; methods not yet ported to 16 bits
    are refused on it with "<method> is not available for 16-bit documents yet" (docs/bit-depth.md)."""
    return text(call("image.mode", bits=bits))


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
    """Switch the tool the person sees (move, marquee, lasso, wand, quickselect, crop, brush, healing, clone, smudge, gradient, shape, text, eyedropper, hand, zoom, artboard, slice). Tools that paint by coordinates do not need this."""
    return text(call("tool.select", name=name))


@edit("Set colours")
def colors_set(foreground: Optional[str] = None, background: Optional[str] = None) -> str:
    """Set the foreground and background colours (CSS), which painting, fills and gradients default to."""
    return text(call("colors.set", foreground=foreground, background=background))


@outside("Colour settings")
def color_settings(working_space: Optional[str] = None, policy: Optional[str] = None, ask_missing: Optional[bool] = None,
                   ask_mismatch: Optional[bool] = None, monitor_profile: Optional[str] = None, use_system_monitor: Optional[bool] = None,
                   proof_profile: Optional[str] = None, proof_intent: Optional[str] = None, proof_black_point: Optional[bool] = None,
                   proof_colors: Optional[bool] = None, gamut_warning: Optional[bool] = None, gamut_color: Optional[str] = None) -> str:
    """Edit > Color Settings (working_space srgb, adobe-rgb, display-p3 or prophoto; policy preserve, convert or off for
    embedded profiles; ask_missing, ask_mismatch), the monitor profile (an ICC path, "" for the system's) and View >
    Proof Setup / Proof Colors / Gamut Warning. With no arguments it only reads them. Untagged images count as sRGB."""
    return text(call("color.settings", workingSpace=working_space, policy=policy, askMissing=ask_missing, askMismatch=ask_mismatch,
                     monitorProfile=monitor_profile, useSystemMonitor=use_system_monitor, proofProfile=proof_profile,
                     proofIntent=proof_intent, proofBlackPoint=proof_black_point, proofColors=proof_colors,
                     gamutWarning=gamut_warning, gamutColor=gamut_color))


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
