"""Installs the plugin next to stellaris.exe, so the game loads it by itself when it starts. From an unpacked release zip, or from a source
checkout after a build:

    python deploy.py [--game <folder of stellaris.exe>] [--build build] [--remove]

Copies (the game folder is found in the Steam libraries unless --game or the STELLARIS_DIR environment variable names it):
  stellaris_live2d.dll  the plugin: replaced; a copy the running game has loaded is asked to unload itself first
  d3dx9_43.dll          the loader, a stand-in for the system's DLL that loads the plugin: copied only when the game is not running (it keeps
                        it loaded)
  Live2DCubismCore.dll  the Cubism Core (Purism Core in a release zip), when there is one next to scripts/ or in the build folder
and writes stellaris_live2d.ini with live2d=1 and core_dll pointing at the copy of the Core, or adds those two keys to an ini that lacks them
(a value that is already there is never changed).
--remove takes the three DLLs away again (the ini and the log stay).
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)


def steam_libraries():
    """The `steamapps` folders of the Steam libraries of this machine."""
    roots = []
    try:
        import winreg
        for hive, sub, name in ((winreg.HKEY_CURRENT_USER, r"Software\Valve\Steam", "SteamPath"),
                                (winreg.HKEY_LOCAL_MACHINE, r"SOFTWARE\WOW6432Node\Valve\Steam", "InstallPath")):
            try:
                with winreg.OpenKey(hive, sub) as key:
                    roots.append(os.path.normpath(winreg.QueryValueEx(key, name)[0]))
            except OSError:
                pass
    except ImportError:
        pass
    libraries = []
    for root in roots:
        libraries.append(os.path.join(root, "steamapps"))
        vdf = os.path.join(root, "steamapps", "libraryfolders.vdf")
        try:
            with open(vdf, encoding="utf-8", errors="replace") as f:
                for m in re.finditer(r'"path"\s+"([^"]+)"', f.read()):
                    libraries.append(os.path.join(m.group(1).replace("\\\\", "\\"), "steamapps"))
        except OSError:
            pass
    return list(dict.fromkeys(libraries))


def find_game(explicit):
    candidates = [explicit] if explicit else []
    if os.environ.get("STELLARIS_DIR"):
        candidates.append(os.environ["STELLARIS_DIR"])
    candidates += [os.path.join(lib, "common", "Stellaris") for lib in steam_libraries()]
    for c in candidates:
        if c and os.path.isfile(os.path.join(c, "stellaris.exe")):
            return c
    where = explicit or os.environ.get("STELLARIS_DIR")
    raise SystemExit((f"there is no stellaris.exe in {where}" if where else "could not find Stellaris in the Steam libraries")
                     + ": use --game <folder of stellaris.exe>")


def game_running():
    out = subprocess.run(["tasklist", "/FI", "IMAGENAME eq stellaris.exe", "/NH"], capture_output=True, text=True).stdout
    return "stellaris.exe" in out.lower()


def unload_plugin():
    out = subprocess.run([sys.executable, os.path.join(HERE, "l2dctl.py"), "unload"], capture_output=True, text=True).stdout.strip()
    print(out)
    time.sleep(1.0)


def first_existing(*paths):
    return next((p for p in paths if os.path.exists(p)), None)


def same_file(a, b):
    try:
        if os.path.getsize(a) != os.path.getsize(b):
            return False
        with open(a, "rb") as fa, open(b, "rb") as fb:
            return fa.read() == fb.read()
    except OSError:
        return False


def copy(src, dst, locked_note):
    if os.path.exists(dst) and same_file(src, dst):
        print("already there:", dst)
        return True
    try:
        shutil.copyfile(src, dst)
    except PermissionError:
        print(f"could not copy {dst}: {locked_note}")
        return False
    print("copied", dst)
    return True


def write_ini(game, core_path):
    """Creates the ini, or adds `live2d=1` and `core_dll=` to one that lacks them."""
    ini = os.path.join(game, "stellaris_live2d.ini")
    wanted = [("live2d", "1")] + ([("core_dll", core_path)] if core_path else [])
    if not os.path.exists(ini):
        with open(ini, "w", encoding="utf-8", newline="\r\n") as f:
            f.write("[live2d]\n" + "".join(f"{k}={v}\n" for k, v in wanted))
        print("wrote", ini)
        return
    with open(ini, encoding="utf-8", errors="replace") as f:
        text = f.read()
    present = {}
    for line in text.splitlines():
        m = re.match(r"\s*([A-Za-z_0-9]+)\s*=\s*(.*?)\s*$", line)
        if m:
            present[m.group(1).lower()] = m.group(2)
    added = [(k, v) for k, v in wanted if not present.get(k)]
    if added:
        with open(ini, "a", encoding="utf-8", newline="\r\n") as f:
            if text and not text.endswith("\n"):
                f.write("\n")
            f.write("".join(f"{k}={v}\n" for k, v in added))
        print("added to", ini + ":", ", ".join(k for k, _ in added))
    if present.get("live2d") == "0":
        print(f"note: {ini} says live2d=0, so the plugin draws nothing until it is 1")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--build", default=os.path.join(ROOT, "build"))
    ap.add_argument("--game", help="the folder of stellaris.exe (default: found in the Steam libraries)")
    ap.add_argument("--remove", action="store_true")
    a = ap.parse_args()
    game = find_game(a.game)
    plugin_dst = os.path.join(game, "stellaris_live2d.dll")
    loader_dst = os.path.join(game, "d3dx9_43.dll")
    core_dst = os.path.join(game, "Live2DCubismCore.dll")
    running = game_running()
    if running:
        unload_plugin()
    if a.remove:
        for path in (plugin_dst, loader_dst, core_dst):
            if os.path.exists(path):
                try:
                    os.remove(path)
                    print("removed", path)
                except OSError as e:
                    print(f"could not remove {path}: {e} (the game has it loaded: close the game first)")
        return 0
    # in an unpacked release zip the DLLs sit next to scripts/; in a source checkout they are in the build folder
    plugin_src = first_existing(os.path.join(ROOT, "stellaris_live2d.dll"), os.path.join(a.build, "Release", "stellaris_live2d.dll"))
    loader_src = first_existing(os.path.join(ROOT, "d3dx9_43.dll"), os.path.join(a.build, "loader", "d3dx9_43.dll"))
    core_src = first_existing(os.path.join(ROOT, "Live2DCubismCore.dll"), os.path.join(a.build, "Release", "Live2DCubismCore.dll"))
    if not plugin_src or not loader_src:
        raise SystemExit("stellaris_live2d.dll / d3dx9_43.dll not found next to scripts/ or in the build folder (--build)")
    print("game folder:", game)
    copy(plugin_src, plugin_dst, "the game has it loaded")
    if running and os.path.exists(loader_dst):
        print("the game is running and has d3dx9_43.dll loaded: not replacing it (close the game to update the loader)")
    else:
        copy(loader_src, loader_dst, "the game has it loaded: close the game first")
        if running:
            print("(the game picks the loader up the next time it starts)")
    core_path = None
    if core_src and copy(core_src, core_dst, "the game has it loaded: close the game first"):
        core_path = core_dst
    elif os.path.exists(core_dst):
        core_path = core_dst  # an earlier copy
    elif not core_src:
        print("no Live2DCubismCore.dll found to copy: put one next to stellaris.exe and set core_dll in the ini")
    write_ini(game, core_path)
    print(f"\nDone. Start Stellaris with a portrait mod enabled (see the demo mod); the plugin's log is {os.path.join(game, 'stellaris_live2d.log')}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
