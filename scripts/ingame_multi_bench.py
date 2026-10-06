"""In-game cost of several different Live2D models drawn at once: frames per second and process CPU with the plugin off
and with N models, one fresh plugin load per configuration so its counters start at zero.

Needs: a running Stellaris with a save loaded and a screen with portraits open (the council screen shows six), the
stellaris_bench.dll frame counter from the stellaris-perf repo loaded (python dllctl.py load bench), and the Live2D models in
models/ (see README). Writes the plugin's config\stellaris_live2d.ini and restores it afterwards.

    python ingame_multi_bench.py [--seconds 30] [--shots captures/multi]
"""
import argparse
import ctypes
import ctypes.wintypes as w
import os
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
PERF_SCRIPTS = r"D:\stellaris-perf\bench\scripts"
sys.path.insert(0, PERF_SCRIPTS)
from benchlib import Bench, game_pid  # noqa: E402

GAME_DIR = r"E:\Program Files (x86)\Steam\steamapps\common\Stellaris"
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from plugin_paths import CONFIG_INI as INI, LOG  # noqa: E402
CORE = os.path.join(ROOT, "scratch", "core", "Live2DCubismCore.dll")
M = os.path.join(ROOT, "models")

# name -> (folder, view): the view is "auto" or "x,y,h"
MODELS = {
    "pa15": ("pa15_5802", "auto"), "d243": ("d243_s2901", "auto"), "d261": ("d261_s3801", "auto"),
    "d119": ("d119_s3001", "auto"), "d95": ("d95_s50001", "auto"), "d307": ("d307_s4703", "0.48,0.49,0.17"),
    "d253": ("d253_s6901", "auto"), "d351": ("d351_s9001", "auto"), "d316": ("d316_s11603", "auto"),
}


def models_line(names):
    return ";".join(f"{os.path.join(M, MODELS[n][0], 'model.model3.json')}|{MODELS[n][1]}" for n in names)


CONFIGS = [
    ("plugin off", None),
    ("1 model (pa15)", ["pa15"]),
    ("3 models, light", ["d243", "d119", "pa15"]),
    ("6 models, light", ["d243", "d119", "pa15", "d261", "d95", "d307"]),
    ("6 models, heavy", ["d351", "d316", "d253", "d307", "d95", "d261"]),
    ("9 models (6 portraits take the first 6)", ["d351", "d316", "d253", "d307", "d95", "d261", "d119", "pa15", "d243"]),
]

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
k32.OpenProcess.restype = w.HANDLE


def cpu_seconds(pid):
    h = k32.OpenProcess(0x1000, False, pid)  # PROCESS_QUERY_LIMITED_INFORMATION
    c, e, kt, ut = (w.FILETIME() for _ in range(4))
    k32.GetProcessTimes(h, ctypes.byref(c), ctypes.byref(e), ctypes.byref(kt), ctypes.byref(ut))
    k32.CloseHandle(h)
    f = lambda t: ((t.dwHighDateTime << 32) | t.dwLowDateTime) / 1e7
    return f(kt) + f(ut)


def write_ini(names):
    lines = ["[live2d]", "test_pattern=0", "only_width=575", "only_height=380", f"live2d={0 if names is None else 1}",
             f"core_dll={CORE}", "fps=30", "physics=1"]
    if names:
        lines.append("models=" + models_line(names))
    with open(INI, "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")


def l2dctl(*args):
    return subprocess.run([sys.executable, os.path.join(HERE, "l2dctl.py"), *args], capture_output=True, text=True, timeout=60).stdout.strip()


def measure(bench, pid, seconds):
    s0 = bench.status()
    c0 = cpu_seconds(pid)
    time.sleep(seconds)
    s1 = bench.status()
    c1 = cpu_seconds(pid)
    dt = s1["t"] - s0["t"]
    return (s1["frames"] - s0["frames"]) / dt, (c1 - c0) / dt * 100.0


def stats_after(offset):
    """The last statistics line the plugin logged after byte `offset`."""
    with open(LOG, "rb") as f:
        f.seek(offset)
        text = f.read().decode("utf-8", "replace")
    lines = [l for l in text.splitlines() if "mode=2 |" in l and "live2d:" in l]
    return lines[-1] if lines else ""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--seconds", type=float, default=30.0)
    ap.add_argument("--shots", default=os.path.join(ROOT, "captures", "multi"))
    ap.add_argument("--only", default="", help="comma separated indices of CONFIGS to run")
    args = ap.parse_args()
    pid = game_pid()
    bench = Bench()
    saved_ini = open(INI, "rb").read() if os.path.exists(INI) else None
    only = {int(i) for i in args.only.split(",") if i != ""}
    print(f"game pid {pid}; {args.seconds:.0f} s per configuration\n")
    print(f"{'configuration':44s} {'fps':>7s} {'game CPU %':>11s}   plugin")
    try:
        for i, (label, names) in enumerate(CONFIGS):
            if only and i not in only:
                continue
            l2dctl("unload")
            time.sleep(1.5)
            offset = os.path.getsize(LOG) if os.path.exists(LOG) else 0
            write_ini(names)
            if names is not None:
                l2dctl("load")
            time.sleep(6.0)  # ini applied, models loaded, GPU resources made, a few steps run
            if names is not None:
                subprocess.run([sys.executable, os.path.join(HERE, "capture_game.py"), f"{args.shots}_{i}.png"], capture_output=True, timeout=60)
            fps, cpu = measure(bench, pid, args.seconds)
            line = ""
            if names is not None:
                time.sleep(max(0.0, 36.0 - 6.0 - args.seconds))
                line = stats_after(offset)
                m = re.search(r"live2d: .*", line)
                line = m.group(0) if m else "(no statistics line yet)"
            print(f"{label:44s} {fps:7.1f} {cpu:11.1f}   {line}", flush=True)
    finally:
        l2dctl("unload")
        if saved_ini is not None:
            with open(INI, "wb") as f:
                f.write(saved_ini)


if __name__ == "__main__":
    main()
