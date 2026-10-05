#!/usr/bin/env python3
"""Crash recovery against the states a crash can leave: launches NekoPhoto, which offers a dead instance's copies,
and checks which come back. Usage: tools/recovery_check.py [path/to/nekophoto]"""
# Crash states in a dead instance's recovery folder, then a launch that answers "recover": which tabs come back.
import os, sys, subprocess, time, shutil, json, tempfile
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__))); from rpc_smoke import Rpc
APP = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "build", "src", "app", "nekophoto")
WORK = tempfile.mkdtemp(prefix="nekorec")
X = WORK + "/xdg"; shutil.rmtree(X, ignore_errors=True)
env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY")}
env.update(QT_QPA_PLATFORM="offscreen", XDG_CONFIG_HOME=X + "/config", XDG_DATA_HOME=X + "/data")
def run(extra_env, args):
    S = WORK + "/s.sock"
    if os.path.exists(S): os.remove(S)
    p = subprocess.Popen([APP, "--rpc", "--rpc-socket", S] + args, env={**env, **extra_env}, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    for _ in range(150):
        if os.path.exists(S): break
        time.sleep(.1)
    return p, Rpc(S)
# 1. Make a real project to copy into the crash states (the demo, saved).
p, r = run({}, [])
r.call("document.new", width=64, height=48)
r.call("layers.add", name="Top")
proj = WORK + "/src.comp"; shutil.rmtree(proj, ignore_errors=True)
r.call("document.save", path=proj)
top = [l for l in r.call("layers.list") if l["name"] == "Top"][0]["id"]
p.kill(); p.wait()
# 2. A dead instance's folder: three documents caught at three moments.
root = X + "/data/nekophoto/nekophoto/recovery"
dead = root + "/deadbeef"; shutil.rmtree(root, ignore_errors=True); os.makedirs(dead)
open(root + "/deadbeef.lock", "w").write("999999\nnekophoto\n" + os.uname().nodename + "\n")
meta = lambda t: json.dumps({"title": t, "originalPath": "", "saved": "2026-10-05T10:00:00Z"})
shutil.copytree(proj, dead + "/a.old.comp"); open(dead + "/a.json", "w").write(meta("MidSwap"))      # crashed mid-swap
shutil.copytree(proj, dead + "/b.comp")                                                              # first copy, no description
shutil.copytree(proj, dead + "/c.saving.comp"); open(dead + "/c.json", "w").write(meta("Partial"))   # only a partial write
shutil.copytree(proj, dead + "/d.comp"); open(dead + "/d.json", "w").write(meta("Normal"))          # ordinary
# 3. Launch, answering recover.
p, r = run({"COMPOSITOR_RECOVERY_ANSWER": "recover"}, [])
time.sleep(2)
try:
    tabs = r.call("tabs.list")
    tabs = tabs["tabs"] if isinstance(tabs, dict) else tabs
    titles = sorted(t["title"] for t in tabs)
    assert titles == ["MidSwap (recovered)", "Normal (recovered)", "Untitled (recovered)"], titles   # never the partial copy
    for t in tabs:
        r.call("tabs.select", index=t["index"])
        assert r.call("document.info").get("activeLayer") == top, "the active layer comes back"
    assert "deadbeef" not in os.listdir(root), "the recovered folder is removed"
    print("recovery: mid-swap, first and ordinary copies recovered, the partial one not; active layers kept")
finally:
    p.kill(); p.wait()
    shutil.rmtree(WORK, ignore_errors=True)
