"""Installs the plugin into its folder of the plugins folder (plugin spec v2 of the Stellaris launcher), from an unpacked release zip or from
a source checkout after a build:

    python deploy.py [--build build] [--remove] [--clean-legacy]

  Documents\\Paradox Interactive\\Stellaris\\plugins\\stellaris-live2d\\
    stl-plugin.json, stellaris_live2d.dll, Live2DCubismCore.dll, defaults\\   replaced
    config\\stellaris_live2d.ini                                             made from defaults\\ when missing, never overwritten

The game loads the plugin only when it is started by the Stellaris launcher (`stl launch`, or its Play button), which injects it; there is
no loader in the game folder any more. With the launcher, `stl plugin install <this folder>` (or its Plugins page) does the same as this
script. A copy the running game has loaded is asked to unload itself first.

--remove        takes the plugin folder away again (config\\ stays, so the settings survive a reinstall)
--clean-legacy  removes what earlier versions put into the game folder: d3dx9_43.dll (the old loader), stellaris_live2d.dll,
                Live2DCubismCore.dll, stellaris_live2d.disabled, the log; an old stellaris_live2d.ini there is moved into config\\ if
                config\\ has none (otherwise removed)
"""
import argparse
import os
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
from plugin_paths import CONFIG_INI, GAME_DIR, LOG, PLUGIN_DIR  # noqa: E402

LEGACY = ["d3dx9_43.dll", "stellaris_live2d.dll", "Live2DCubismCore.dll", "stellaris_live2d.disabled"]


def game_running():
    out = subprocess.run(["tasklist", "/FI", "IMAGENAME eq stellaris.exe", "/NH"], capture_output=True, text=True).stdout
    return "stellaris.exe" in out.lower()


def unload_plugin():
    out = subprocess.run([sys.executable, os.path.join(HERE, "l2dctl.py"), "unload"], capture_output=True, text=True).stdout.strip()
    print(out)
    time.sleep(1.0)


def first_existing(*paths):
    return next((p for p in paths if os.path.exists(p)), None)


def copy(src, dst):
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    try:
        shutil.copyfile(src, dst)
    except PermissionError:
        raise SystemExit(f"could not replace {dst}: the game has it loaded and did not unload it (close the game)")
    print("copied", dst)


def make_config():
    """config\\stellaris_live2d.ini from defaults\\ (with {plugin_dir} / {config_dir} filled in), unless it exists."""
    if os.path.exists(CONFIG_INI):
        print("settings kept:", CONFIG_INI)
        return
    with open(os.path.join(PLUGIN_DIR, "defaults", "stellaris_live2d.ini"), encoding="utf-8-sig") as f:
        text = f.read()
    text = text.replace("{plugin_dir}", PLUGIN_DIR).replace("{config_dir}", os.path.dirname(CONFIG_INI))
    os.makedirs(os.path.dirname(CONFIG_INI), exist_ok=True)
    with open(CONFIG_INI, "w", encoding="utf-8", newline="\r\n") as f:
        f.write(text)
    print("wrote", CONFIG_INI)


def clean_legacy():
    old_ini = os.path.join(GAME_DIR, "stellaris_live2d.ini")
    if os.path.exists(old_ini) and not os.path.exists(CONFIG_INI):
        os.makedirs(os.path.dirname(CONFIG_INI), exist_ok=True)
        shutil.move(old_ini, CONFIG_INI)
        print(f"moved {old_ini} -> {CONFIG_INI}")
    for name in LEGACY + ["stellaris_live2d.ini", "stellaris_live2d.log"]:
        p = os.path.join(GAME_DIR, name)
        if os.path.exists(p):
            try:
                os.remove(p)
                print("removed", p)
            except OSError as e:
                print(f"could not remove {p}: {e} (the game has it loaded: close the game first)")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--build", default=os.path.join(ROOT, "build"))
    ap.add_argument("--remove", action="store_true")
    ap.add_argument("--clean-legacy", action="store_true")
    a = ap.parse_args()
    if a.clean_legacy:
        if game_running():
            raise SystemExit("close the game first: it has the old loader (d3dx9_43.dll) loaded, so it cannot be removed")
        clean_legacy()
        if not a.remove:
            return 0
    if game_running():
        unload_plugin()
    if a.remove:
        if os.path.isdir(PLUGIN_DIR):
            for name in os.listdir(PLUGIN_DIR):
                if name == "config":
                    continue
                p = os.path.join(PLUGIN_DIR, name)
                shutil.rmtree(p) if os.path.isdir(p) else os.remove(p)
            print("removed", PLUGIN_DIR, "(config\\ kept)")
        return 0
    # in an unpacked release zip the files sit next to scripts/; in a source checkout they are in the build folder and plugin/
    release = os.path.join(a.build, "Release")
    files = {
        "stellaris_live2d.dll": first_existing(os.path.join(ROOT, "stellaris_live2d.dll"), os.path.join(release, "stellaris_live2d.dll")),
        "Live2DCubismCore.dll": first_existing(os.path.join(ROOT, "Live2DCubismCore.dll"), os.path.join(release, "Live2DCubismCore.dll")),
        "stl-plugin.json": first_existing(os.path.join(ROOT, "stl-plugin.json"), os.path.join(ROOT, "plugin", "stl-plugin.json")),
        os.path.join("defaults", "stellaris_live2d.ini"): first_existing(os.path.join(ROOT, "defaults", "stellaris_live2d.ini"),
                                                                          os.path.join(ROOT, "plugin", "defaults", "stellaris_live2d.ini")),
    }
    missing = [k for k, v in files.items() if not v]
    if "stellaris_live2d.dll" in missing:
        raise SystemExit("stellaris_live2d.dll not found next to scripts/ or in the build folder (--build)")
    if [m for m in missing if m != "Live2DCubismCore.dll"]:
        raise SystemExit(f"missing: {', '.join(missing)}")
    print("plugin folder:", PLUGIN_DIR)
    for rel, src in files.items():
        if src:
            copy(src, os.path.join(PLUGIN_DIR, rel))
    if not files["Live2DCubismCore.dll"]:
        print("no Live2DCubismCore.dll found: put one into the plugin folder, or set core_dll in the settings")
    make_config()
    if any(os.path.exists(os.path.join(GAME_DIR, n)) for n in LEGACY):
        print(f"\nnote: {GAME_DIR} still has files of an earlier version (the old loader d3dx9_43.dll loads the old plugin when the game is"
              " started from Steam): python deploy.py --clean-legacy removes them")
    print(f"\nDone. Start Stellaris with the Stellaris launcher (stl launch, or Play) and a portrait mod enabled; the log is {LOG}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
