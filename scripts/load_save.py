"""Restarts Stellaris on a given save and injects stellaris_live2d.dll once the save has loaded, so a mod that registers
Live2D portraits can be checked against a real save without any clicking.

    python load_save.py autosave_2200.07.01 --folder 12_-513968080 [--bench]
    python load_save.py restore            # put the original continue_game.json back

Uses the helpers of the stellaris-perf repo (D:\\stellaris-perf\\bench\\scripts): stellaris_bench.dll is injected as the
frame counter and to see when the save is loaded and paused; stellaris_perf.dll is NOT loaded.
"""
import argparse
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
PERF_SCRIPTS = r"D:\stellaris-perf\bench\scripts"
sys.path.insert(0, PERF_SCRIPTS)
import dllctl  # noqa: E402
import game_session as gs  # noqa: E402
from benchlib import Bench  # noqa: E402


def l2dctl(*args):
    return subprocess.run([sys.executable, os.path.join(HERE, "l2dctl.py"), *args], capture_output=True, text=True, timeout=90).stdout.strip()


def load(name, folder, timeout):
    gs.close_game()
    gs.point_continue_at(folder, name)
    subprocess.Popen([os.path.join(gs.GAME_DIR, "stellaris.exe"), "-dx11", "--continuelastsave"], cwd=gs.GAME_DIR,
                     creationflags=subprocess.DETACHED_PROCESS | subprocess.CREATE_NEW_PROCESS_GROUP)
    deadline = time.time() + timeout
    while len(gs.game_pids()) != 1:
        if time.time() > deadline:
            raise SystemExit("stellaris.exe did not start")
        time.sleep(1)
    pid = gs.game_pids()[0]
    time.sleep(5)
    if not dllctl.load(pid, "bench"):
        raise SystemExit("could not load the bench dll")
    bench = Bench(tries=200)
    last, stable = None, 0
    while time.time() < deadline:
        try:
            st = bench.status()
        except SystemExit:
            st = {"ok": False}
        if st.get("ok") and st.get("in_game") and st.get("hours", 0) > 0:
            stable = stable + 1 if st["hours"] == last else 0
            last = st["hours"]
            if stable >= 3:
                break
        time.sleep(1)
    else:
        raise SystemExit("the save did not finish loading in time")
    bench.cmd("pause 1")
    st = bench.status()
    bench.close()
    print(f"loaded {name}: day {st['day']} paused={st['paused']}")
    print(l2dctl("load"))
    return pid


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("name")
    ap.add_argument("--folder", default="12_-513968080")
    ap.add_argument("--timeout", type=float, default=400)
    a = ap.parse_args()
    if a.name == "restore":
        import shutil
        if os.path.exists(gs.BACKUP):
            shutil.copyfile(gs.BACKUP, gs.CONTINUE)
            print(f"restored {gs.CONTINUE}")
        return 0
    load(a.name, a.folder, a.timeout)
    return 0


if __name__ == "__main__":
    sys.exit(main())
