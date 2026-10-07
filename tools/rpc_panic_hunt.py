#!/usr/bin/env python3
"""Hostile parameters for every automation method: starts a headless NekoPhoto on the demo document, asks it for its
methods (rpc.methods) and their parameters (rpc.describe), and sends each method wrong types, NaN and infinities,
huge sizes and counts, negative and nonexistent ids, empty and oversized strings, deep nesting and unknown keys.
Every reply must be a JSON-RPC error or result, in time; the editor must never die or hang. A crash is reported with
the request that caused it, the editor is started again and the hunt goes on; the exit status is 1 if anything died,
hung or answered out of protocol.

    COMPOSITOR_BIN=build/src/app/nekophoto python3 tools/rpc_panic_hunt.py [--only REGEX] [--budget SECONDS]

Writes are contained: the editor runs with --rpc-write-root in a temporary folder and its settings, presets and
actions in temporary XDG folders. Methods in SKIP are never called (each with its reason).
"""
import argparse
import json
import os
import re
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from rpc_smoke import open_local_socket  # noqa: E402

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))

# Methods never sent to, and why.
SKIP = {
}
# Not wrapped in a batch: they cannot run in one (the batch refuses them) or end the connection's state.
BATCH_EXCLUDED = ("rpc.batch", "history.beginGroup", "history.endGroup", "events.subscribe", "events.unsubscribe")
# Sent last: they take away the document (or the tab) the other methods work on.
LAST = ("document.close", "tabs.close", "document.new", "document.open")

RAW = "\u0001raw\u0001"   # placeholder for a token json.dumps cannot write (1e999, NaN)


class Died(Exception):
    pass


class Hung(Exception):
    pass


class Editor:
    """A headless NekoPhoto with the demo document, its writes kept in a temporary folder."""

    def __init__(self, binary, work, timeout):
        self.binary, self.work, self.timeout = binary, work, timeout
        self.proc = None
        self.sock = self.file = None
        self.next_id = 0
        self.starts = 0

    def start(self):
        self.starts += 1
        path = os.path.join(self.work, f"s{self.starts}.sock")
        root = os.path.join(self.work, "root")
        os.makedirs(root, exist_ok=True)
        env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY")}
        env.update(QT_QPA_PLATFORM="offscreen", XDG_CONFIG_HOME=os.path.join(self.work, "config"),
                   XDG_DATA_HOME=os.path.join(self.work, "data"), XDG_CACHE_HOME=os.path.join(self.work, "cache"))
        self.log = open(os.path.join(self.work, f"editor{self.starts}.log"), "wb")
        self.proc = subprocess.Popen([self.binary, "--headless", "--rpc-socket", path, "--rpc-write-root", root, "--demo"],
                                     env=env, stdout=self.log, stderr=subprocess.STDOUT)
        deadline = time.time() + 60
        while True:
            if self.proc.poll() is not None:
                raise SystemExit(f"the editor exited at start ({self.proc.returncode}); see {self.log.name}")
            try:
                self.sock, self.file = open_local_socket(path)
                break
            except OSError:
                if time.time() > deadline:
                    raise SystemExit(f"no socket at {path} after 60 s")
                time.sleep(0.1)
        if hasattr(self.sock, "settimeout"):
            self.sock.settimeout(self.timeout)

    def stop(self):
        for f in (self.file, self.sock):
            try:
                f.close()
            except Exception:
                pass
        if self.proc and self.proc.poll() is None:
            self.proc.kill()
            self.proc.wait()
        self.proc = None

    def alive(self):
        return self.proc is not None and self.proc.poll() is None

    def send(self, text):
        """Sends one request line; returns the reply object (events skipped). Raises Died or Hung."""
        try:
            self.file.write(text + "\n")
            self.file.flush()
            while True:
                line = self.file.readline()
                if not line:
                    raise Died("connection closed")
                reply = json.loads(line)
                if reply.get("method") != "event":
                    return reply
        except (socket.timeout, TimeoutError):
            raise Hung(f"no reply in {self.timeout} s")
        except (OSError, ValueError) as e:
            if not self.alive():
                raise Died(str(e))
            raise

    def request(self, method, params, raw=None):
        self.next_id += 1
        text = json.dumps({"jsonrpc": "2.0", "id": self.next_id, "method": method, "params": params}, allow_nan=False)
        if raw is not None:
            text = text.replace(json.dumps(RAW), raw)
        return self.next_id, self.send(text)

    def call(self, name, /, **params):
        _, reply = self.request(name, params)
        if "error" in reply:
            raise RuntimeError(f"{name}: {reply['error'].get('message')}")
        return reply.get("result")


def valid_reply(reply, rid, raw):
    if not isinstance(reply, dict):
        return "not an object"
    if "error" in reply:
        e = reply["error"]
        if not isinstance(e, dict) or not isinstance(e.get("code"), int) or not isinstance(e.get("message"), str):
            return f"malformed error {e!r}"
        if reply.get("id") not in (rid, None if raw is not None else rid):
            return f"wrong id {reply.get('id')!r}"
        return None
    if "result" not in reply:
        return "neither result nor error"
    if reply.get("id") != rid:
        return f"wrong id {reply.get('id')!r}"
    return None


def nested(depth, leaf=0):
    value = leaf
    for i in range(depth):
        value = [value] if i % 2 else {"a": value}
    return value


# Every parameter gets the wrong types; then what fits its type: sizes past every limit, ids that do not exist, colours,
# strings and nesting that are not what it wants.
WRONG_TYPES = [None, True, "", [], {}, -1, 0.5, 1e308]
NUMBERS = [0, -1, -100000, 46341, 65536, 2**31 - 1, 2**31, 10**9, 2**63, -(2**63), 1e308, -1e308, 5e-324, -0.5]
RAWS = ["1e999", "-1e999", "NaN", "-Infinity", "-0"]


# Plausible values by parameter name, for the parameters with no default (so the request reaches the handler).
NAMED = {
    "points": [[20, 20], [200, 40], [120, 200], [30, 150]],
    "box": [10, 10, 200, 200],
    "region": {"x": 0, "y": 0, "width": 64, "height": 64},
    "source": {"x": 10, "y": 10},
    "calls": [{"method": "app.info", "params": {}}],
    "path": [{"closed": True, "knots": [[10, 10], [100, 10], [100, 100]]}],
}


def typed(value, t):
    """A described default (the description gives it as text) as a value of its type."""
    if not isinstance(value, str):
        return value
    try:
        if t == "bool":
            return value == "true"
        if t == "integer":
            return int(value)
        if t == "number":
            return float(value)
    except ValueError:
        pass
    return value


def base_value(param, ctx):
    """A plausible value of the parameter's type, so the other parameters reach the handler."""
    t, name = param.get("type", ""), param["name"]
    values = param.get("values") or []
    if param.get("default") is not None:
        return typed(param["default"], t)
    if values:
        return values[0]
    if t == "layer" or name in ("id", "layer", "target"):
        return ctx["layer"]
    if name == "ids":
        return [ctx["layer"]]
    if name in NAMED and (t in ("array", "object") or not t):
        if name == "path" and t != "array":
            return os.path.join(ctx["root"], "out-path.png")
        return NAMED[name]
    if name in ("foreground", "background") and t == "array":
        return [[[60, 60], [90, 90]]] if ctx.get("method") == "selection.scribble" else [[100, 100]]
    if t == "integer":
        return 64 if re.search(r"width|height|size", name, re.I) else 10
    if t == "number":
        return 64.0 if re.search(r"width|height|size", name, re.I) else 10.0
    if t == "bool":
        return True
    if t == "color":
        return "#ff0000"
    if t == "array":
        return []
    if t == "object":
        return {}
    if re.search(r"path|file|folder|dir", name, re.I):
        return os.path.join(ctx["root"], f"out-{name}.png")
    return "x"


def hostile_values(param, ctx):
    t, name = param.get("type", ""), param["name"]
    out = list(WRONG_TYPES)
    if t in ("integer", "number") or re.search(r"width|height|size|count|radius|steps|index|amount|x$|y$|scale|columns|rows", name, re.I):
        out += NUMBERS
    if t == "layer" or re.search(r"id$|ids$|layer", name, re.I):
        out += ["00000000-0000-0000-0000-000000000000", "{00000000-0000-0000-0000-000000000000}", "nope", 2**40,
                [ctx["layer"]] * 5000, [ctx["layer"], "nope"]]
    if t == "color":
        out += ["#", "#gggggg", "#12345", "rgb(1e999,0,0)", "notacolor", "#" + "f" * 5000]
    if param.get("values"):
        out += ["NOPE", param["values"][0].upper()]
    if t == "array":
        out += [[0] * 200000, [[0, 0]] * 20000, nested(200), [1e308, -1e308, 0, 0], ["", None, {}], [[1e308, -1e308]] * 3]
    if t == "object":
        out += [nested(200), {"": None}, {"x": 1e308, "y": -1e308, "width": 2**31, "height": 2**31}]
    if t == "string" or re.search(r"path|file|folder|dir|name", name, re.I):
        root = ctx["root"]
        out += ["x" * 70000, "\u0000\uffff\U0001f600", "/", root, os.path.join(root, "nonexistent", "x.png"),
                os.path.join(root, "..", "escape.png"), os.path.join(root, "a" * 300 + ".png"), os.path.join(root, "x.unknownext"),
                "%s%s%n"]
    return out


def cases(method, params, ctx):
    """(description, params, raw) for one method."""
    yield "no params", {}, None
    yield "unknown key", {"__bogus": 1, "constructor": {}}, None
    yield "params not an object", RAW, "[]"
    yield "params a string", RAW, '"x"'
    ctx["method"] = method
    # The base request: the required parameters and those with a default, each at a plausible value; one at a time is
    # then made hostile.
    required = {p["name"]: base_value(p, ctx) for p in params if p.get("required")}
    base = {p["name"]: base_value(p, ctx) for p in params if p.get("required") or p.get("default") is not None}
    if params:
        yield "required only", dict(required), None
        yield "base", dict(base), None
        yield "every param", {p["name"]: base_value(p, ctx) for p in params}, None
        # Tokens JSON has no word for: the request must come back as a parse error, whatever the method.
        for r in RAWS:
            yield f"{params[0]['name']}={r} (raw)", {**base, params[0]["name"]: RAW}, r
    # Every number at once: sizes that only overflow multiplied together (width * height), or zero everywhere.
    numeric = [p["name"] for p in params if p.get("type") in ("integer", "number")]
    if len(numeric) > 1:
        full = {p["name"]: base_value(p, ctx) for p in params}
        for v in [0, -1, 46341, 65536, 2**31 - 1, 10**9, 1e308, -1e308, 0.25]:
            yield f"every number={v}", {**full, **{n: v for n in numeric}}, None
    for p in params:
        for v in hostile_values(p, ctx):
            shown = json.dumps(v)[:60] if not isinstance(v, float) else repr(v)
            yield f"{p['name']}={shown}", {**base, p["name"]: v}, None


# ---- Deep: settings objects, one key at a time -------------------------------------------------------------------

LEAF = [None, True, -1, 0, 1e308, -1e308, 2**31, -(2**31), 1e-300, "", "nope", [], {}, [1e308, -1e308], [[0, 0]] * 3000]


def mutations(sample, path=(), depth=0):
    """Copies of `sample` with one value (at any depth up to 3) replaced by a hostile one: (where, copy)."""
    if depth > 3:
        return
    items = list(sample.items()) if isinstance(sample, dict) else list(enumerate(sample)) if isinstance(sample, list) else []
    for key, value in items[:40]:
        for v in LEAF:
            copy = json.loads(json.dumps(sample))
            copy[key] = v
            yield path + (key,), copy
        if isinstance(value, (dict, list)) and value:
            for where, inner in mutations(value, path + (key,), depth + 1):
                copy = json.loads(json.dumps(sample))
                copy[key] = inner
                yield where, copy
    if isinstance(sample, list):
        yield path + ("+",), sample * 50 if sample else [0] * 5000
        yield path + ("-",), []


def deep_cases(ed, ctx, described):
    """(method, description, params, raw): settings objects taken from the editor's own replies, mutated."""
    adjustment_kinds = next((p.get("values") for p in described.get("adjustments.defaults", []) if p["name"] == "kind"), None) or []
    for kind in adjustment_kinds:
        try:
            sample = ed.call("adjustments.defaults", kind=kind)
        except RuntimeError:
            continue
        settings = sample.get("settings", sample) if isinstance(sample, dict) else sample
        for where, copy in mutations(settings):
            yield "pixels.adjust", f"{kind} {'.'.join(map(str, where))}", {"kind": kind, "settings": copy}, None
        for where, copy in list(mutations(settings))[::7]:
            yield "layers.add", f"{kind} layer {'.'.join(map(str, where))}", {"kind": "adjustment", "adjustmentKind": kind, "settings": copy}, None
    # Every effect at its defaults, as layers.style shows them, then one setting at a time.
    try:
        effects = ["dropShadows", "innerShadows", "outerGlows", "innerGlows", "bevels", "satins", "colorOverlays", "gradientOverlays",
                   "patternOverlays", "strokes"]
        ed.call("layers.setStyle", id=ctx["layer"], style={e: [{}] for e in effects})
        style = ed.call("layers.style", id=ctx["layer"])
    except RuntimeError:
        style = None
    if isinstance(style, dict):
        for where, copy in mutations(style):
            yield "layers.setStyle", ".".join(map(str, where)), {"id": ctx["layer"], "style": copy}, None
    try:
        raw_settings = ed.call("pixels.cameraRaw", settings={})
        raw_settings = raw_settings.get("settings", raw_settings) if isinstance(raw_settings, dict) else None
    except RuntimeError:
        raw_settings = None
    if isinstance(raw_settings, dict):
        for where, copy in list(mutations(raw_settings))[::3]:
            yield "pixels.cameraRaw", ".".join(map(str, where)), {"settings": copy}, None


# ---- The run -----------------------------------------------------------------------------------------------------

def ensure_document(ed, ctx):
    """Puts the editor back in a state where methods have something to work on: the demo document as it was at the start
    (reopened from a copy, so neither the last method's edits nor its history carry over), with a fresh, painted pixel
    layer active, one tab and no edit group open."""
    try:
        ed.request("history.endGroup", {})
        # Whatever the method left, undone and redone in one go.
        ed.request("history.undo", {"steps": 10000})
        ed.request("history.redo", {"steps": 10000})
        tabs = ed.call("tabs.list")
        tabs = tabs["tabs"] if isinstance(tabs, dict) else tabs
        for t in sorted(tabs, key=lambda t: -t["index"]):
            if len(tabs) > 1 and t["index"] > 0:
                ed.request("tabs.close", {"index": t["index"], "discard": True})
        ed.request("tabs.select", {"index": 0})
        if ctx.get("demo"):
            ed.request("document.close", {"discard": True})
            ed.call("document.open", path=ctx["demo"])
        else:
            try:
                ed.call("document.info")
            except RuntimeError:
                ed.call("document.new", width=320, height=240)
        ed.request("selection.none", {})
        before = {l["id"] for l in ed.call("layers.list")}
        ed.call("layers.add", kind="pixels", name="Hunt")
        added = [l["id"] for l in ed.call("layers.list") if l["id"] not in before]
        if added:
            ctx["layer"] = added[0]
            ed.request("layers.select", {"id": ctx["layer"]})
            ed.request("pixels.fill", {"color": "#3080c0"})
            ed.request("selection.rect", {"x": 40, "y": 30, "width": 120, "height": 90})
            ed.request("pixels.fill", {"color": "#e04020"})
            ed.request("selection.none", {})
    except (RuntimeError, KeyError, TypeError) as e:
        print(f"  (could not reset the document: {e})")


def snapshot_demo(ed, work, ctx):
    """Saves the demo document once, outside the write root, so every method starts from it."""
    saved = os.path.join(ctx["root"], "demo.nekophoto")
    try:
        ed.call("document.save", path=saved)
    except RuntimeError as e:
        print(f"  (could not save the demo document, methods start from a blank one: {e})")
        return
    ctx["demo"] = os.path.join(work, "demo.nekophoto")
    shutil.copyfile(saved, ctx["demo"])
    os.remove(saved)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bin", default=os.environ.get("COMPOSITOR_BIN", os.path.join(ROOT, "build", "src", "app", "nekophoto")))
    ap.add_argument("--only", help="only methods matching this regular expression (the deep phase runs when it matches 'deep')")
    ap.add_argument("--timeout", type=float, default=10, help="seconds one reply may take; longer is reported as a hang (default 10)")
    ap.add_argument("--budget", type=float, default=0, help="stop starting new methods after this many seconds (0: no limit)")
    ap.add_argument("--keep", action="store_true", help="keep the temporary folder (logs, written files)")
    ap.add_argument("--verbose", action="store_true", help="print every error reply")
    ap.add_argument("--deep", type=int, default=1000, help="at most this many settings mutations, spread evenly (0: all of them)")
    args = ap.parse_args()
    if not os.path.exists(args.bin):
        raise SystemExit(f"no editor at {args.bin}; build it or set COMPOSITOR_BIN")

    work = tempfile.mkdtemp(prefix="nphunt")
    ed = Editor(args.bin, work, args.timeout)
    problems, slow, sent, began = [], [], 0, time.time()
    ctx = {"root": os.path.join(work, "root"), "layer": ""}

    def run(method, what, params, raw):
        """One request; a crash or hang is recorded and the editor started again. Returns whether it was a result."""
        try:
            t = time.time()
            rid, reply = ed.request(method, params, raw)
            if time.time() - t > 2:
                slow.append((time.time() - t, method, what))
            bad = valid_reply(reply, rid, raw)
            if args.verbose and "error" in reply:
                print(f"    {what}: {reply['error'].get('message', '')[:150]}")
            if bad:
                problems.append(f"{method} [{what}]: {bad}")
            return "result" in reply
        except (Died, Hung, OSError, ValueError) as e:
            kind = "HANG" if isinstance(e, Hung) else "CRASH"
            code = ed.proc.poll() if ed.proc else None
            detail = f" (exit {code}, signal {signal.Signals(-code).name})" if code is not None and code < 0 else \
                f" (exit {code})" if code is not None else ""
            problems.append(f"{kind} {method} [{what}]{detail}: {e}")
            print(f"  {kind}: {method} [{what}]{detail}")
            print(f"    request: {json.dumps(params)[:400]}")
            ed.stop()
            ed.start()
            ensure_document(ed, ctx)
            return False

    try:
        ed.start()
        methods = sorted(ed.call("rpc.methods"))
        described = {m: ed.call("rpc.describe", method=m)["params"] for m in methods}
        snapshot_demo(ed, work, ctx)
        ensure_document(ed, ctx)
        skipped = [m for m in methods if m in SKIP]
        todo = [m for m in methods if m not in SKIP and (not args.only or re.search(args.only, m))]
        todo.sort(key=lambda m: LAST.index(m) + 1 if m in LAST else 0)
        out_of_time = False
        for n, method in enumerate(todo):
            if args.budget and time.time() - began > args.budget:
                # A slow runner covers fewer methods; that is a warning, not a failure.
                print(f"warning: time budget of {args.budget:.0f} s reached; {len(todo) - n} of {len(todo)} methods not reached")
                out_of_time = True
                break
            t0, count, results, crashes = time.time(), 0, 0, len(problems)
            for i, (what, params, raw) in enumerate(cases(method, described[method], ctx)):
                count += 1
                results += run(method, what, params, raw)
                if i % 8 == 5 and raw is None and method not in BATCH_EXCLUDED:
                    # Inside a named batch after an edit: an error must take the edit back, a success keep both.
                    count += 1
                    run("rpc.batch", f"{method} {what} in a named batch",
                        {"name": "Hunt batch", "calls": [{"method": "pixels.fill", "params": {"color": "#00ff00"}},
                                                         {"method": method, "params": params}]}, None)
                # A crash in one case should not hide the rest of the method's cases, unless they keep failing.
                if sum(1 for p in problems[crashes:] if p.startswith(("HANG ", "CRASH "))) >= 3:
                    break
            sent += count
            ensure_document(ed, ctx)
            print(f"{method}: {count} requests ({results} results) in {time.time() - t0:.1f} s")
            sys.stdout.flush()
        if not out_of_time and (not args.only or re.search(args.only, "deep")):
            t0, count, results, last = time.time(), 0, 0, None
            deep = list(deep_cases(ed, ctx, described))
            stride = max(1, -(-len(deep) // args.deep)) if args.deep else 1
            for method, what, params, raw in deep[::stride]:
                if args.budget and time.time() - began > args.budget:
                    print(f"warning: time budget of {args.budget:.0f} s reached in the deep phase")
                    break
                if method != last:
                    ensure_document(ed, ctx)
                    last = method
                if "id" in params:
                    params["id"] = ctx["layer"]   # the layer the reset made
                count += 1
                results += run(method, f"deep {what}", params, raw)
            sent += count
            print(f"deep (adjustment settings, layer styles, Camera Raw): {count} of {len(deep)} mutations ({results} results) in {time.time() - t0:.1f} s")
        if ed.alive():
            ed.call("app.info")   # still answering at the end
    finally:
        ed.stop()
        if args.keep:
            print("kept", work)
        else:
            shutil.rmtree(work, ignore_errors=True)

    print(f"\n{sent} requests to {len(methods) - len(skipped)} methods in {time.time() - began:.0f} s"
          + (f"; skipped {len(skipped)} ({', '.join(f'{m}: {SKIP[m]}' for m in skipped)})" if skipped else ""))
    if slow:
        print(f"{len(slow)} requests took over 2 s (not failures; the slowest):")
        for seconds, method, what in sorted(slow, reverse=True)[:15]:
            print(f"  {seconds:5.1f} s  {method} [{what[:100]}]")
    if problems:
        print(f"{len(problems)} problems:")
        for p in problems:
            print("  " + p)
        return 1
    print("no crash, no hang, every reply an error or a result")
    return 0


if __name__ == "__main__":
    sys.exit(main())
