"""Copies the plugin and the loader next to stellaris.exe, so the game loads the plugin by itself when it starts.

    python deploy.py [--build build] [--game <folder of stellaris.exe>] [--remove]

stellaris_live2d.dll: replaced; a copy the running game has loaded is asked to unload itself first (the plugin does that on request).
d3dx9_43.dll (the loader, a stand-in for the system's): copied only when the game is not running, since the game keeps it loaded.
--remove takes both away again (the ini and the log stay).
"""
import argparse
import os
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
DEFAULT_GAME = os.environ.get("STELLARIS_DIR", r"E:\Program Files (x86)\Steam\steamapps\common\Stellaris")


def game_running():
    out = subprocess.run(["tasklist", "/FI", "IMAGENAME eq stellaris.exe", "/NH"], capture_output=True, text=True).stdout
    return "stellaris.exe" in out.lower()


def unload_plugin():
    out = subprocess.run([sys.executable, os.path.join(HERE, "l2dctl.py"), "unload"], capture_output=True, text=True).stdout.strip()
    print(out)
    time.sleep(1.0)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--build", default=os.path.join(ROOT, "build"))
    ap.add_argument("--game", default=DEFAULT_GAME)
    ap.add_argument("--remove", action="store_true")
    a = ap.parse_args()
    plugin_dst = os.path.join(a.game, "stellaris_live2d.dll")
    loader_dst = os.path.join(a.game, "d3dx9_43.dll")
    running = game_running()
    if running:
        unload_plugin()
    if a.remove:
        for path in (plugin_dst, loader_dst):
            if os.path.exists(path):
                try:
                    os.remove(path)
                    print("removed", path)
                except OSError as e:
                    print(f"could not remove {path}: {e} (the game has it loaded: close the game first)")
        return 0
    plugin_src = os.path.join(a.build, "Release", "stellaris_live2d.dll")
    loader_src = os.path.join(a.build, "loader", "d3dx9_43.dll")
    shutil.copyfile(plugin_src, plugin_dst)
    print("copied", plugin_dst)
    if running and os.path.exists(loader_dst):
        print("the game is running and has d3dx9_43.dll loaded: not replacing it (close the game to update the loader)")
    elif running:
        shutil.copyfile(loader_src, loader_dst)
        print("copied", loader_dst, "(the game picks it up the next time it starts)")
    else:
        shutil.copyfile(loader_src, loader_dst)
        print("copied", loader_dst)
    return 0


if __name__ == "__main__":
    sys.exit(main())
