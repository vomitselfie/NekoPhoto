#!/usr/bin/env python3
"""A fixed performance suite: how much work the editor does for a set of everyday operations, counted rather than
timed (the machine is shared and CI runners vary, so wall-clock numbers would be noise).

Each scenario runs in a process of its own and reports
  - instructions:u, the user-space instructions of every thread, from `perf stat` (Linux perf). Only the measured
    part is counted: perf starts with its counters off (--delay=-1) and the suite switches them on and off through
    perf's control fifo around the operation (for the developer benchmarks, the app does it: NEKOPHOTO_PERF_CONTROL,
    src/app/Bench.cpp). The start-up scenarios count from exec to the first reply;
  - the work counters of the operation (docs/work-counters.md), read before and after with `debug.counters`;
  - peak RSS: the process's (`/usr/bin/time -f %M`) and the operation's own (its high-water mark, reset through
    /proc/<pid>/clear_refs just before it).

    tools/perf_suite.py [--build build] [--quick] [--runs N] [--only a,b] [--json out.json] [--markdown out.md]
                        [--compare tools/perf_baseline.json] [--write-baseline tools/perf_baseline.json]
                        [--real-psd K.psd] [--counter auto|perf|none] [--profile DIR]

--quick is the CI size (a 2000 x 1500 document, smaller benchmarks, one run); the default is the full size (8000 x
6000, three runs, the median kept). --compare prints each metric's change against the baseline's section for the same
size and flags those past the noise threshold (--threshold, percent, for instructions; work counters are exact and any
change is flagged; RSS uses --rss-threshold). --profile records each scenario's measured part with `perf record` and
writes the hottest functions and their callers to DIR. Without perf (or where the CPU's counters are not exposed, as on
most CI virtual machines) --counter auto measures work counters and RSS only, and says so. See docs/performance-suite.md.
"""
import argparse
import datetime
import hashlib
import inspect
import json
import os
import platform
import shutil
import signal
import socket
import statistics
import struct
import subprocess
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# Sizes per mode. The synthetic PSD is generated once per size and kept in the work folder.
SIZES = {
    "full": {"psd": (8000, 6000), "brush": "4000x3000", "mypaint": "1086x1448", "adjust": "4096x4096", "runs": 3},
    "quick": {"psd": (2000, 1500), "brush": "1000x750", "mypaint": "543x724", "adjust": "1024x1024", "runs": 1},
}
WINDOW_SIZE = "1400x1000"


# ---- the editor over its socket ---------------------------------------------------------------------------------

class Rpc:
    def __init__(self, path, timeout=900):
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.connect(path)
        self.sock.settimeout(timeout)
        self.file = self.sock.makefile("rw", encoding="utf-8")
        self.next_id = 0

    def peer_pid(self):
        """The editor's pid (SO_PEERCRED), for /proc."""
        creds = self.sock.getsockopt(socket.SOL_SOCKET, socket.SO_PEERCRED, struct.calcsize("3i"))
        return struct.unpack("3i", creds)[0]

    def call(self, method, **params):
        self.next_id += 1
        self.file.write(json.dumps({"jsonrpc": "2.0", "id": self.next_id, "method": method, "params": params}) + "\n")
        self.file.flush()
        while True:
            line = self.file.readline()
            if not line:
                raise RuntimeError(f"{method}: the editor closed the connection")
            reply = json.loads(line)
            if reply.get("method") != "event":
                break
        if "error" in reply:
            raise RuntimeError(f"{method}: {reply['error'].get('message')}")
        return reply["result"]

    def close(self):
        try:
            self.file.close()
            self.sock.close()
        except OSError:
            pass


def socket_dir():
    """A short folder for sockets: a Unix socket's path must stay under 107 bytes, and a worktree's is often longer."""
    for base in (os.environ.get("NEKOPHOTO_PERF_SOCKET_DIR"), os.environ.get("XDG_RUNTIME_DIR"), "/tmp"):
        if base and os.path.isdir(base) and os.access(base, os.W_OK) and len(base) < 60:
            return tempfile.mkdtemp(prefix="np-perf-", dir=base)
    raise SystemExit("no short writable folder for the socket; set NEKOPHOTO_PERF_SOCKET_DIR")


# ---- counting -----------------------------------------------------------------------------------------------------

def perf_usable():
    """(True, "") when `perf stat -e instructions:u` counts here, else (False, why)."""
    perf = shutil.which("perf")
    if not perf:
        return False, "perf is not installed"
    try:
        run = subprocess.run([perf, "stat", "-x,", "-e", "instructions:u", "--", "true"], capture_output=True, text=True, timeout=30)
    except (OSError, subprocess.TimeoutExpired) as e:
        return False, f"perf did not run: {e}"
    for line in run.stderr.splitlines():
        fields = line.split(",")
        if len(fields) > 2 and fields[2].startswith("instructions"):
            if fields[0].strip().isdigit() and int(fields[0]) > 0:
                return True, ""
            return False, f"perf cannot count instructions:u here ({fields[0].strip() or 'no value'}; a virtual machine without PMU access, or kernel.perf_event_paranoid)"
    return False, "perf stat gave no instruction count: " + (run.stderr.strip().splitlines() or ["(no output)"])[-1]


class Process:
    """One editor process under perf stat (when counting) and /usr/bin/time, with perf's control fifo."""

    def __init__(self, args, env, work, counting, record=None, delay=True, app_controls=False):
        """env: a function from extra variables to the environment. app_controls: the app, not the suite, writes to the
        control fifo (its paths are passed in NEKOPHOTO_PERF_CONTROL)."""
        self.work = work
        self.fifo_dir = tempfile.mkdtemp(prefix="fifo-", dir=work)
        self.ctl, self.ack = os.path.join(self.fifo_dir, "ctl"), os.path.join(self.fifo_dir, "ack")
        self.stat_out = os.path.join(self.fifo_dir, "stat.txt")
        self.rss_out = os.path.join(self.fifo_dir, "rss.txt")
        self.record = record
        command = []
        controlled = counting or record
        if controlled:
            os.mkfifo(self.ctl)
            os.mkfifo(self.ack)
        if record:
            command = ["perf", "record", "-q", "-F", "999", "-g", "--call-graph", "dwarf,16384", "-o", record]
        elif counting:
            command = ["perf", "stat", "-x,", "-e", "instructions:u", "-o", self.stat_out]
        if controlled:
            command += (["--delay=-1"] if delay else []) + ["--control", f"fifo:{self.ctl},{self.ack}", "--"]
        if os.path.exists("/usr/bin/time"):
            command += ["/usr/bin/time", "-f", "%M", "-o", self.rss_out]
        extra = {"NEKOPHOTO_PERF_CONTROL": f"{self.ctl},{self.ack}"} if controlled and app_controls else {}
        self.started = time.monotonic()
        # Output to files, not pipes: nothing reads a pipe while the editor runs, and a full one would stop it.
        self.out_path, self.err_path = os.path.join(self.fifo_dir, "stdout.txt"), os.path.join(self.fifo_dir, "stderr.txt")
        with open(self.out_path, "w") as out, open(self.err_path, "w") as err:
            self.proc = subprocess.Popen(command + args, env=env(extra), stdout=out, stderr=err, cwd=work, start_new_session=True)
        self.controlled = controlled and not app_controls
        self.ctl_file = self.ack_file = None
        if self.controlled:
            # Opening a fifo blocks until the other end opens it: perf opens both before it starts the command.
            self.ctl_file = open(self.ctl, "w")
            self.ack_file = open(self.ack, "r")

    def stdout(self):
        with open(self.out_path, errors="replace") as f:
            return f.read()

    def stderr(self):
        with open(self.err_path, errors="replace") as f:
            return f.read()

    def tell(self, command):
        if not self.controlled:
            return
        self.ctl_file.write(command + "\n")
        self.ctl_file.flush()
        self.ack_file.readline()

    def finish(self, timeout=1800):
        try:
            self.proc.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            os.killpg(self.proc.pid, signal.SIGKILL)
            self.proc.wait()
            raise RuntimeError("timed out")
        out, err = self.stdout(), self.stderr()
        self.wall = time.monotonic() - self.started
        for f in (self.ctl_file, self.ack_file):
            if f:
                f.close()
        result = {"stdout": out, "stderr": err, "status": self.proc.returncode, "wall_s": round(self.wall, 2)}
        if os.path.exists(self.stat_out):
            for line in open(self.stat_out):
                fields = line.strip().split(",")
                if len(fields) > 2 and fields[2].startswith("instructions") and fields[0].strip().isdigit():
                    result["instructions"] = int(fields[0])
        if os.path.exists(self.rss_out):
            for line in open(self.rss_out):
                if line.strip().isdigit():
                    result["peak_rss_kb"] = int(line)
        shutil.rmtree(self.fifo_dir, ignore_errors=True)
        return result


def read_hwm(pid):
    """VmHWM in kB, or None."""
    try:
        for line in open(f"/proc/{pid}/status"):
            if line.startswith("VmHWM:"):
                return int(line.split()[1])
    except OSError:
        pass
    return None


def reset_hwm(pid):
    try:
        with open(f"/proc/{pid}/clear_refs", "w") as f:
            f.write("5")
        return True
    except OSError:
        return False


def cpu_ticks(pid):
    """The process's user + system time so far, in clock ticks (all its threads)."""
    try:
        with open(f"/proc/{pid}/stat") as f:
            fields = f.read().rsplit(")", 1)[1].split()
        return int(fields[11]) + int(fields[12])
    except (OSError, IndexError, ValueError):
        return None


def settle(pid, quiet=3, poll=0.1, limit=60):
    """Waits until the editor has gone quiet: no CPU time used over `quiet` polls in a row. Work the editor does after a
    reply (the Histogram panel's worker, a throttled thumbnail, the file watcher's settle after a save) then belongs to the
    step that caused it, and none of it leaks into the next. The wait is wall-clock, the measure is not."""
    deadline = time.monotonic() + limit
    last, still = cpu_ticks(pid), 0
    while time.monotonic() < deadline and still < quiet:
        time.sleep(poll)
        now = cpu_ticks(pid)
        still = still + 1 if now == last else 0
        last = now


def counters_delta(after, before):
    return {k: int(after[k] - before.get(k, 0)) for k in sorted(after) if after[k] - before.get(k, 0)}


def parse_work_line(stdout, label):
    """The counters of a benchmark's "work: <label>: a=1 b=2" line."""
    for line in stdout.splitlines():
        if line.startswith("work: ") and (label is None or line[6:].startswith(label + ":")):
            body = line.split(": ", 2)[-1]
            return {k: int(v) for k, v in (item.split("=") for item in body.split() if "=" in item)}
    return {}


# ---- the synthetic document ---------------------------------------------------------------------------------------

BLENDS = ["Normal", "Multiply", "Screen", "Overlay", "Soft Light", "Color Dodge", "Darken", "Lighten", "Hard Light", "Color"]


def build_synthetic(rpc, width, height, path):
    """A layered document like a working PSD: a textured base, 28 painted layers (blend modes, opacity, masks, a clipped
    one every seventh) in five folders (one nested), four adjustment layers, a smart object with a Smart Filter, two
    text layers and a layer style. Deterministic: the same requests every time. The full size holds about 0.2 gigapixels
    of layers, well inside the project budget (a gigapixel), as a real 8000 x 6000 PSD would."""
    s = width / 8000.0
    rpc.call("document.new", width=width, height=height, emptyLayer=True)
    base = rpc.call("layers.list")[0]["id"]
    rpc.call("layers.set", id=base, name="Base")
    rpc.call("gradient.draw", x0=0, y0=0, x1=width, y1=height, style="foreground-to-background", foreground="#30609f", background="#e0c080")
    rpc.call("pixels.filter", kind="Add Noise", amount=12, monochromatic=False, seed=7)
    # Painted layers the size of what is on them, as in a real PSD (and as the project's pixel budget expects): each is
    # painted on a scratch layer and lifted off it with Layer via Copy.
    scratch = rpc.call("layers.add", kind="pixels", name="Scratch")["id"]

    def lifted(x, y, w, h, name, paint):
        rpc.call("layers.select", id=scratch)
        rpc.call("selection.rect", x=x, y=y, width=w, height=h)
        paint()
        layer = rpc.call("layers.viaCopy")["id"]
        rpc.call("layers.select", id=scratch)
        rpc.call("pixels.clear")
        rpc.call("selection.none")
        rpc.call("layers.set", id=layer, name=name)
        rpc.call("layers.select", id=layer)
        return layer

    painted = []
    for i in range(28):
        x = int((i * 977) % max(1, width - int(2400 * s)))
        y = int((i * 613) % max(1, height - int(1800 * s)))
        w, h = int((1200 + (i * 331) % 1200) * s), int((900 + (i * 257) % 900) * s)
        color = "#%02x%02x%02x" % ((i * 53) % 256, (i * 97 + 80) % 256, (i * 29 + 160) % 256)

        def paint(x=x, y=y, w=w, h=h, color=color, i=i):
            if i % 2 == 1:
                rpc.call("selection.rect", x=x, y=y, width=w, height=h, ellipse=True, feather=int(8 * s) if i % 4 == 1 else 0)
            rpc.call("gradient.draw", x0=x, y0=y, x1=x + w, y1=y + h, shape="radial" if i % 3 else "linear", foreground=color)
        layer = lifted(x, y, w, h, f"Paint {i + 1}", paint)
        props = {"opacity": 0.55 + (i % 5) * 0.1}
        if BLENDS[i % len(BLENDS)] != "Normal":
            props["blend"] = BLENDS[i % len(BLENDS)]
        if i % 7 == 6:
            props["clipping"] = True
        rpc.call("layers.set", id=layer, **props)
        if i % 3 == 0:
            rpc.call("layers.mask", id=layer, action="add")
            rpc.call("layers.select", id=layer, mask=True)
            rpc.call("selection.rect", x=x, y=y, width=w // 2, height=h, feather=int(20 * s))
            rpc.call("pixels.fill", color="#000000")
            rpc.call("selection.none")
            rpc.call("layers.select", id=layer)
        painted.append(layer)
    # Five folders of the painted layers, the last inside the fourth.
    folders = []
    for g in range(5):
        rpc.call("layers.select", id=painted[g * 5], ids=painted[g * 5:g * 5 + 5])
        folders.append(rpc.call("layers.group")["id"])
    rpc.call("layers.select", id=folders[3], ids=[folders[3], folders[4]])
    rpc.call("layers.group")
    rpc.call("layers.select", id=painted[-1])
    rpc.call("layers.add", kind="adjustment", adjustmentKind="levels", name="Levels",
             settings={"levels": {"channel": "RGB", "ranges": [{"black": 12, "gamma": 1.15, "white": 238, "outputBlack": 0, "outputWhite": 255}] + [{}] * 3}})
    rpc.call("layers.add", kind="adjustment", adjustmentKind="curves", name="Curves",
             settings={"curves": {"channel": "RGB", "channels": [[{"x": 0, "y": 0}, {"x": 96, "y": 80}, {"x": 176, "y": 196}, {"x": 255, "y": 255}]]
                                  + [[{"x": 0, "y": 0}, {"x": 255, "y": 255}]] * 3}})
    rpc.call("layers.add", kind="adjustment", adjustmentKind="hue/saturation", name="Hue/Saturation", settings={"hue": 12, "saturation": 18})
    rpc.call("layers.add", kind="adjustment", adjustmentKind="color balance", name="Color Balance")
    # A smart object with a Smart Filter, from a painted layer of its own.
    source = lifted(int(1000 * s), int(800 * s), int(3000 * s), int(2000 * s), "Smart source",
                    lambda: rpc.call("gradient.draw", x0=int(1000 * s), y0=int(800 * s), x1=int(4000 * s), y1=int(2800 * s), shape="radial", foreground="#c04090"))
    rpc.call("layers.delete", id=scratch)
    smart = rpc.call("smartObject.convert", ids=[source])["id"]
    rpc.call("smartObject.addFilter", id=smart, kind="gaussian blur", radius=max(1, int(6 * s)))
    rpc.call("layers.set", id=smart, blend="Overlay", opacity=0.8)
    rpc.call("layers.add", kind="text", text="NekoPhoto performance suite", x=int(400 * s), y=int(300 * s), size=max(8, int(220 * s)), color="#ffffff")
    rpc.call("layers.setStyle", style={"dropShadows": [{"distance": max(1, int(12 * s)), "size": max(1, int(16 * s))}]})
    rpc.call("layers.add", kind="text", text="Forty layers: folders, masks, adjustments,\na smart object and type.", x=int(400 * s), y=int(5200 * s),
             size=max(8, int(120 * s)), color="#202020")
    count = len(rpc.call("layers.list"))
    rpc.call("document.export", path=path, overwrite=True)
    return count


# ---- scenarios ----------------------------------------------------------------------------------------------------

class Context:
    def __init__(self, args, mode, work, counting):
        self.args = args
        self.mode = mode
        self.size = SIZES[mode]
        self.work = work
        self.counting = counting
        self.binary = os.path.abspath(os.path.join(args.build, "src", "app", "nekophoto"))
        self.home = os.path.join(work, "home")
        self.socket_dir = socket_dir()
        # Named after the generator's code, so a change to it makes a new file. The editor's own PSD writer also shapes
        # the file: after changing it, delete build-perf/synthetic-* to compare like with like.
        recipe = hashlib.sha1(inspect.getsource(build_synthetic).encode()).hexdigest()[:8]
        self.synthetic = os.path.join(work, f"synthetic-{self.size['psd'][0]}x{self.size['psd'][1]}-{recipe}.psd")
        self.real_psd = args.real_psd if args.real_psd and os.path.exists(args.real_psd) else None

    def env(self, extra=None):
        env = dict(os.environ)
        for key in ("DISPLAY", "WAYLAND_DISPLAY", "NEKOPHOTO_PERF_CONTROL", "QT_SCALE_FACTOR"):
            env.pop(key, None)
        # The editor's settings and data in a folder of the suite's own: the same state every run, the person's
        # untouched. The caches (fontconfig's) stay where they are, warm.
        env.update(QT_QPA_PLATFORM="offscreen", XDG_CONFIG_HOME=os.path.join(self.home, "config"), XDG_DATA_HOME=os.path.join(self.home, "data"),
                   NEKOPHOTO_WINDOW_SIZE=WINDOW_SIZE, LC_ALL="C.UTF-8", NEKOPHOTO_CPU="all")
        env.update(extra or {})
        return env


def connect(path, proc, seconds=120):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if proc.proc.poll() is not None:
            raise RuntimeError("the editor exited before listening: " + proc.stderr()[-2000:])
        try:
            return Rpc(path)
        except OSError:
            time.sleep(0.02)
    raise RuntimeError(f"no socket at {path}")


def stop(rpc, proc):
    """Stops the editor by its pid (never by name) and collects perf's numbers."""
    pid = None
    try:
        pid = rpc.peer_pid()
    except OSError:
        pass
    rpc.close()
    if pid:
        try:
            os.kill(pid, signal.SIGTERM)
        except ProcessLookupError:
            pass
    return proc.finish()


def socket_scenario(ctx, setup, operation, record=None):
    """An editor listening on a socket: setup() uncounted, then operation() counted, its work counters and RSS read."""
    sock = os.path.join(ctx.socket_dir, "s")
    if os.path.exists(sock):
        os.unlink(sock)
    proc = Process([ctx.binary, "--headless", "--rpc-socket", sock], ctx.env, ctx.work, ctx.counting, record=record)
    rpc = connect(sock, proc)
    try:
        pid = rpc.peer_pid()
        setup(rpc)
        settle(pid)
        before = rpc.call("debug.counters")
        reset_hwm(pid)
        proc.tell("enable")
        operation(rpc)
        settle(pid)
        proc.tell("disable")
        op_peak = read_hwm(pid)
        work = counters_delta(rpc.call("debug.counters"), before)
    except Exception:
        stop(rpc, proc)
        raise
    result = stop(rpc, proc)
    result.update(work=work, op_peak_rss_kb=op_peak)
    return result


def startup_scenario(ctx, headless, record=None):
    """From exec to the first reply on the socket: the window built, shown (offscreen) and idle, or headless ready."""
    sock = os.path.join(ctx.socket_dir, "s")
    if os.path.exists(sock):
        os.unlink(sock)
    # --lang keeps the window in a process of its own (no hand-off to a running editor, no autosave, no welcome).
    args = ["--headless"] if headless else ["--rpc", "--lang", "en"]
    proc = Process([ctx.binary, *args, "--rpc-socket", sock], ctx.env, ctx.work, ctx.counting, record=record, delay=False)
    rpc = connect(sock, proc)
    rpc.call("app.info")
    settle(rpc.peer_pid())
    proc.tell("disable")
    work = rpc.call("debug.counters")
    result = stop(rpc, proc)
    result.update(work={k: int(v) for k, v in work.items() if v})
    return result


def run_bench(ctx, args, label, record=None):
    """A developer benchmark through the real canvas (src/app/Bench.cpp): the app switches perf on and off itself
    around the part it measures, through the fifo paths it is given in NEKOPHOTO_PERF_CONTROL."""
    proc = Process([ctx.binary, *args], ctx.env, ctx.work, ctx.counting, record=record, app_controls=True)
    result = proc.finish()
    if result["status"] != 0:
        raise RuntimeError(f"benchmark exited with {result['status']}: {result['stderr'][-2000:]}")
    result["work"] = parse_work_line(result["stdout"], label)
    if "work: " not in result["stdout"]:
        raise RuntimeError("the benchmark printed no work line (an older build?)")
    return result


def scenarios(ctx):
    """name -> (description, function(record) -> result)."""
    size = ctx.size
    psd = ctx.synthetic
    out = {}

    def opened(path):
        def setup(rpc):
            rpc.call("document.open", path=path)
        return setup

    def base_selected(rpc):
        rpc.call("document.open", path=psd)
        layers = rpc.call("layers.list")
        base = next(l for l in layers if l["name"] == "Base")
        rpc.call("layers.select", id=base["id"])

    out["startup-window"] = ("cold start to an idle window (offscreen, 1400 x 1000), to the first reply", lambda r: startup_scenario(ctx, False, r))
    out["startup-headless"] = ("cold start, headless, to the first reply", lambda r: startup_scenario(ctx, True, r))
    out["open-psd"] = (f"open the synthetic {size['psd'][0]} x {size['psd'][1]} PSD (42 layers: folders, masks, adjustments, a smart object, type)",
                       lambda r: socket_scenario(ctx, lambda rpc: None, lambda rpc: rpc.call("document.open", path=psd), r))
    if ctx.real_psd and ctx.mode == "full":
        real = ctx.real_psd
        out["open-real-psd"] = (f"open {os.path.basename(real)} (local file)",
                                lambda r: socket_scenario(ctx, lambda rpc: None, lambda rpc: rpc.call("document.open", path=real), r))
    out["brush-round"] = (f"--bench-brush round, 5 strokes of 80 moves, {size['brush']}",
                          lambda r: run_bench(ctx, ["--bench-brush", "round", "--bench-size", size["brush"], "--bench-brush-size", "60"], "strokes", r))
    out["brush-mypaint"] = (f"--bench-brush classic/dry_brush, 5 strokes, {size['mypaint']}",
                            lambda r: run_bench(ctx, ["--bench-brush", "classic/dry_brush", "--bench-size", size["mypaint"]], "strokes", r))
    out["adjust-drag"] = (f"--bench-adjust: 30 ticks of a Levels slider over 5 layers, {size['adjust']}",
                          lambda r: run_bench(ctx, ["--bench-adjust", "--bench-size", size["adjust"]], "Levels: drag white input", r))
    out["filter-gaussian"] = ("Gaussian Blur radius 50 on the base layer",
                              lambda r: socket_scenario(ctx, base_selected, lambda rpc: rpc.call("pixels.filter", kind="Gaussian Blur", radius=50), r))

    def caf_setup(rpc):
        base_selected(rpc)
        w, h = size["psd"]
        rpc.call("selection.rect", x=w // 2 - w // 26, y=h // 2 - h // 20, width=w // 13, height=h // 10)
    out["content-aware-fill"] = ("Content-Aware Fill of a 1/13 x 1/10 region of the base layer",
                                 lambda r: socket_scenario(ctx, caf_setup, lambda rpc: rpc.call("pixels.contentAwareFill", sampling="auto"), r))
    nk = os.path.join(ctx.work, "out.nekophoto")
    out["save-nekophoto"] = ("save the opened PSD as .nekophoto",
                             lambda r: socket_scenario(ctx, opened(psd), lambda rpc: rpc.call("document.save", path=nk, overwrite=True), r))
    psd_out = os.path.join(ctx.work, "out.psd")
    out["save-psd"] = ("export the opened PSD as .psd",
                       lambda r: socket_scenario(ctx, opened(psd), lambda rpc: rpc.call("document.export", path=psd_out, overwrite=True), r))

    def big_edit(rpc):
        base_selected(rpc)
        rpc.call("pixels.invert")

    def undo_redo(rpc):
        rpc.call("history.undo")
        rpc.call("history.redo")
    out["undo-redo"] = ("undo and redo an Invert of the whole base layer", lambda r: socket_scenario(ctx, big_edit, undo_redo, r))
    return out


# ---- running, reporting, comparing --------------------------------------------------------------------------------

def git_commit():
    try:
        return subprocess.run(["git", "-C", ROOT, "rev-parse", "--short", "HEAD"], capture_output=True, text=True).stdout.strip()
    except OSError:
        return ""


def cpu_name():
    try:
        for line in open("/proc/cpuinfo"):
            if line.startswith("model name"):
                return line.split(":", 1)[1].strip()
    except OSError:
        pass
    return platform.processor()


def compiler_of(build):
    try:
        for line in open(os.path.join(build, "CMakeCache.txt")):
            if line.startswith("CMAKE_CXX_COMPILER:"):
                compiler = line.split("=", 1)[1].strip()
                version = subprocess.run([compiler, "--version"], capture_output=True, text=True).stdout.splitlines()[0]
                return version
    except (OSError, IndexError):
        pass
    return ""


def ensure_synthetic(ctx):
    if os.path.exists(ctx.synthetic) and os.path.getsize(ctx.synthetic) > 0:
        return
    w, h = ctx.size["psd"]
    print(f"generating {os.path.basename(ctx.synthetic)} (once per size)...", flush=True)
    sock = os.path.join(ctx.socket_dir, "g")
    if os.path.exists(sock):
        os.unlink(sock)
    proc = Process([ctx.binary, "--headless", "--rpc-socket", sock], ctx.env, ctx.work, False)
    rpc = connect(sock, proc)
    try:
        tmp = ctx.synthetic + ".part.psd"
        count = build_synthetic(rpc, w, h, tmp)
        os.replace(tmp, ctx.synthetic)
        print(f"  {count} layers, {os.path.getsize(ctx.synthetic) / 1e6:.1f} MB", flush=True)
    finally:
        stop(rpc, proc)


def run_suite(args):
    mode = "quick" if args.quick else "full"
    counter = args.counter
    why = ""
    if counter in ("auto", "perf"):
        ok, why = perf_usable()
        if not ok and counter == "perf":
            raise SystemExit(f"--counter perf: {why}")
        counter = "perf" if ok else "none"
    if counter == "none":
        print(f"instructions are not counted ({why or '--counter none'}): work counters and peak RSS only", flush=True)
    work = os.path.abspath(args.work_dir)
    os.makedirs(work, exist_ok=True)
    ctx = Context(args, mode, work, counter == "perf")
    if not os.path.exists(ctx.binary):
        raise SystemExit(f"no editor at {ctx.binary}; build it first")
    try:
        ensure_synthetic(ctx)
        table = scenarios(ctx)
        only = set(args.only.split(",")) if args.only else None
        runs = args.runs or ctx.size["runs"]
        results = {}
        # A first start fills the suite's settings folder (brush presets, the font groups): not measured.
        if not os.path.exists(os.path.join(ctx.home, "warm")):
            startup_scenario(Context(args, mode, work, False), True)
            open(os.path.join(ctx.home, "warm"), "w").close()
        for name, (description, function) in table.items():
            if only and name not in only:
                continue
            print(f"{name}: {description}", flush=True)
            samples = []
            for i in range(runs):
                r = function(None)
                samples.append(r)
                print(f"  run {i + 1}: " + (f"{r['instructions']:,} instructions" if "instructions" in r else "-") +
                      f", peak RSS {r.get('peak_rss_kb', 0) / 1024:.0f} MB", flush=True)
            results[name] = summarise(description, samples)
            if args.profile:
                os.makedirs(args.profile, exist_ok=True)
                profile(ctx, name, function, args.profile)
    finally:
        shutil.rmtree(ctx.socket_dir, ignore_errors=True)
    return {
        "meta": {"mode": mode, "counter": counter, "commit": git_commit(), "date": datetime.date.today().isoformat(),
                 "cpu": cpu_name(), "threads": os.cpu_count(), "compiler": compiler_of(args.build), "runs": args.runs or SIZES[mode]["runs"],
                 "kernel": platform.release(), "note": why},
        "scenarios": results,
    }


def summarise(description, samples):
    out = {"description": description}
    counts = [s["instructions"] for s in samples if "instructions" in s]
    if counts:
        median = int(statistics.median(counts))
        out["instructions"] = median
        out["instructions_runs"] = counts
        out["spread_pct"] = round(100.0 * (max(counts) - min(counts)) / median, 2) if median else 0.0
    # Work counters: the median run's (they are equal across runs when the work is deterministic, which the spread shows).
    works = [s.get("work", {}) for s in samples]
    out["work"] = {k: int(statistics.median(w.get(k, 0) for w in works)) for k in sorted(set().union(*works))}
    out["work_stable"] = all(w == works[0] for w in works)
    if not out["work_stable"]:
        # Each counter's spread over the runs, percent of its median: the noise a comparison allows it.
        out["work_spread_pct"] = {k: round(100.0 * (max(w.get(k, 0) for w in works) - min(w.get(k, 0) for w in works)) / v, 2)
                                  for k, v in out["work"].items() if v and len({w.get(k, 0) for w in works}) > 1}
    rss = [s["peak_rss_kb"] for s in samples if s.get("peak_rss_kb")]
    if rss:
        out["peak_rss_kb"] = int(statistics.median(rss))
    op = [s["op_peak_rss_kb"] for s in samples if s.get("op_peak_rss_kb")]
    if op:
        out["op_peak_rss_kb"] = int(statistics.median(op))
    out["wall_s_informational"] = statistics.median(s["wall_s"] for s in samples)
    return out


def profile(ctx, name, function, folder):
    """perf record of the measured part, then the hottest functions (self) and the call paths into the top ones."""
    data = os.path.join(ctx.work, f"{name}.perf.data")
    pctx = Context(ctx.args, ctx.mode, ctx.work, False)
    function_record = scenarios(pctx)[name][1]
    try:
        function_record(data)
    except Exception as e:  # a profile is a bonus, never a failure
        print(f"  profile failed: {e}", flush=True)
        return
    report = subprocess.run(["perf", "report", "-i", data, "--stdio", "--no-children", "--percent-limit", "1.5", "-g", "caller,0.5,callee,function,percent",
                             "--max-stack", "14", "--sort", "dso,symbol"], capture_output=True, text=True)
    flat = subprocess.run(["perf", "report", "-i", data, "--stdio", "--no-children", "-g", "none", "--percent-limit", "0.5", "--sort", "dso,symbol"],
                          capture_output=True, text=True)
    with open(os.path.join(folder, f"{name}.txt"), "w") as f:
        f.write(flat.stdout)
        f.write("\n\n==== call paths ====\n")
        f.write(report.stdout)
    try:
        os.unlink(data)
    except OSError:
        pass
    print(f"  profile: {os.path.join(folder, name + '.txt')}", flush=True)


KEY_COUNTERS = ["render.calls", "render.pixels", "layer.draws", "adjustment.passes", "mip.builtPixels", "brush.dabs", "smartFilter.passes"]


def human(n):
    if n is None:
        return "-"
    for unit, scale in (("G", 1e9), ("M", 1e6), ("k", 1e3)):
        if abs(n) >= scale:
            return f"{n / scale:.2f}{unit}"
    return str(n)


def markdown(result):
    meta = result["meta"]
    lines = [f"Performance suite, {meta['mode']} size, commit {meta['commit']}, {meta['date']}; {meta['cpu']} ({meta['threads']} threads), "
             f"{meta['compiler']}; counter: {meta['counter']}" + (f" ({meta['note']})" if meta.get("note") else ""), "",
             "| Scenario | instructions:u | spread | peak RSS (process) | peak RSS (operation) | work counters |",
             "|---|---:|---:|---:|---:|---|"]
    for name, s in result["scenarios"].items():
        work = s.get("work", {})
        shown = " ".join(f"{k}={human(work[k])}" for k in KEY_COUNTERS if k in work) or "-"
        if not s.get("work_stable", True):
            shown += " (varied between runs)"
        spread = f"{s['spread_pct']}%" if "spread_pct" in s else "-"
        rss = f"{s['peak_rss_kb'] / 1024:.0f} MB" if s.get("peak_rss_kb") else "-"
        op = f"{s['op_peak_rss_kb'] / 1024:.0f} MB" if s.get("op_peak_rss_kb") else "-"
        lines.append(f"| {name} | {human(s.get('instructions'))} | {spread} | {rss} | {op} | {shown} |")
    return "\n".join(lines) + "\n"


def compare(result, baseline_path, threshold, rss_threshold):
    """Prints and returns the comparison against the baseline's section for this size."""
    try:
        baseline = json.load(open(baseline_path))
    except (OSError, ValueError) as e:
        print(f"no baseline to compare with ({e})")
        return ""
    mode = result["meta"]["mode"]
    base = baseline.get(mode)
    if not base:
        print(f"the baseline has no {mode} section")
        return ""
    lines = [f"Against {os.path.relpath(baseline_path, ROOT)} ({mode}: commit {base['meta'].get('commit')}, {base['meta'].get('date')}, "
             f"{base['meta'].get('cpu')}, {base['meta'].get('compiler')}, counter {base['meta'].get('counter')})"]
    if base["meta"].get("cpu") != result["meta"].get("cpu") or base["meta"].get("compiler") != result["meta"].get("compiler"):
        lines.append("note: another CPU or compiler than the baseline's: instruction counts differ by the code each builds and runs; "
                     "work counters still compare exactly")
    if result["meta"]["counter"] == "none":
        lines.append(f"note: instructions were not counted here ({result['meta'].get('note') or '--counter none'}); work counters and RSS only")
    lines += ["", "| Scenario | metric | baseline | now | change | |", "|---|---|---:|---:|---:|---|"]
    flagged = 0

    def row(name, metric, old, new, limit):
        nonlocal flagged
        if old is None and new is None:
            return
        if old is None or new is None:
            lines.append(f"| {name} | {metric} | {human(old)} | {human(new)} | | {'new' if old is None else 'gone'} |")
            return
        change = 0.0 if old == new else (100.0 * (new - old) / old if old else float("inf"))
        mark = ""
        if abs(change) > limit:
            mark = "**worse**" if change > 0 else "better"
            flagged += 1
        if mark or metric == "instructions:u":
            lines.append(f"| {name} | {metric} | {human(old)} | {human(new)} | {change:+.2f}% | {mark} |")

    for name, now in result["scenarios"].items():
        was = base["scenarios"].get(name)
        if not was:
            lines.append(f"| {name} | | | | | new scenario |")
            continue
        if "instructions" in now or ("instructions" in was and result["meta"]["counter"] != "none"):
            noise = max(threshold, now.get("spread_pct", 0), was.get("spread_pct", 0))
            row(name, "instructions:u", was.get("instructions"), now.get("instructions"), noise)
        for key in sorted(set(was.get("work", {})) | set(now.get("work", {}))):
            noise = max(was.get("work_spread_pct", {}).get(key, 0.0), now.get("work_spread_pct", {}).get(key, 0.0))
            row(name, key, was.get("work", {}).get(key, 0), now.get("work", {}).get(key, 0), noise)
        row(name, "peak RSS kB", was.get("peak_rss_kb"), now.get("peak_rss_kb"), rss_threshold)
        row(name, "operation peak RSS kB", was.get("op_peak_rss_kb"), now.get("op_peak_rss_kb"), rss_threshold)
    lines.append("")
    lines.append(f"{flagged} change(s) past the thresholds (instructions {threshold}% or the runs' spread; work counters exact, or their spread "
                 f"where the runs differed; RSS {rss_threshold}%)")
    text = "\n".join(lines) + "\n"
    print(text)
    return text


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--build", default=os.path.join(ROOT, "build"), help="the build folder (default build)")
    parser.add_argument("--quick", action="store_true", help="the CI size: a 2000 x 1500 document, smaller benchmarks, one run")
    parser.add_argument("--runs", type=int, default=0, help="runs per scenario (default 3, quick 1); the median is kept")
    parser.add_argument("--only", help="comma-separated scenario names")
    parser.add_argument("--list", action="store_true", help="list the scenarios and quit")
    parser.add_argument("--json", help="write the results here")
    parser.add_argument("--markdown", help="write the table (and the comparison) here")
    parser.add_argument("--compare", help="a baseline file to compare with")
    parser.add_argument("--threshold", type=float, default=2.0, help="instruction change that counts as real, percent (default 2; the runs' spread when larger)")
    parser.add_argument("--rss-threshold", type=float, default=10.0, help="peak RSS change that counts as real, percent (default 10)")
    parser.add_argument("--write-baseline", help="store these results as the baseline's section for this size")
    parser.add_argument("--real-psd", default=os.environ.get("NEKOPHOTO_PERF_REAL_PSD"), help="a real PSD to open as well (full size only; never committed)")
    parser.add_argument("--counter", choices=["auto", "perf", "none"], default="auto", help="how instructions are counted (auto: perf when it works here)")
    parser.add_argument("--profile", help="also perf record each scenario and write the hot functions and call paths to this folder")
    parser.add_argument("--work-dir", default=os.path.join(ROOT, "build-perf"), help="where the synthetic PSD, the settings and outputs live (default build-perf)")
    parser.add_argument("--load", help="report, compare or store the results a previous run wrote with --json, instead of running")
    args = parser.parse_args()
    if args.list:
        ctx = Context(args, "quick" if args.quick else "full", os.path.abspath(args.work_dir), False)
        for name, (description, _) in scenarios(ctx).items():
            print(f"{name:20} {description}")
        shutil.rmtree(ctx.socket_dir, ignore_errors=True)
        return 0
    result = json.load(open(args.load)) if args.load else run_suite(args)
    table = markdown(result)
    print()
    print(table)
    comparison = compare(result, args.compare, args.threshold, args.rss_threshold) if args.compare else ""
    if args.json:
        with open(args.json, "w") as f:
            json.dump(result, f, indent=1)
            f.write("\n")
    if args.markdown:
        with open(args.markdown, "w") as f:
            f.write(table + ("\n" + comparison if comparison else ""))
    if args.write_baseline:
        try:
            baseline = json.load(open(args.write_baseline))
        except (OSError, ValueError):
            baseline = {}
        baseline[result["meta"]["mode"]] = result
        with open(args.write_baseline, "w") as f:
            json.dump(baseline, f, indent=1, sort_keys=True)
            f.write("\n")
        print(f"baseline written: {args.write_baseline} ({result['meta']['mode']})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
