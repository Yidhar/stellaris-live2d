"""Where the plugin and its files are (plugin spec v2 of the Stellaris launcher, its docs/PLUGINS.md):

    Documents\\Paradox Interactive\\Stellaris\\plugins\\stellaris-live2d\\
        stl-plugin.json, stellaris_live2d.dll, Live2DCubismCore.dll, defaults\\
        config\\stellaris_live2d.ini     the settings (the launcher's Plugins page edits it; the plugin re-reads it every 2 s)
        logs\\stellaris_live2d.log       the plugin's log

The game folder is STELLARIS_DIR when set, else found in the Steam libraries of this machine (registry + libraryfolders.vdf). The helper
scripts of the sibling stellaris-perf repository (used by some development scripts) are STELLARIS_PERF_SCRIPTS when set, else
../stellaris-perf/bench/scripts next to this repository. Nothing here is specific to one machine.

Shared by the scripts and tools of this repository.
"""
import ctypes
import ctypes.wintypes as w
import os
import re
import uuid

PLUGIN_ID = "stellaris-live2d"


def documents_dir():
    """The user's Documents folder (where the game keeps its data), from the known-folder API: it may have been moved or be on OneDrive."""

    class GUID(ctypes.Structure):
        _fields_ = [("Data1", w.DWORD), ("Data2", w.WORD), ("Data3", w.WORD), ("Data4", ctypes.c_ubyte * 8)]

    u = uuid.UUID("FDD39AD0-238F-46AF-ADB4-6C85480369C7")  # FOLDERID_Documents
    g = GUID(u.fields[0], u.fields[1], u.fields[2], (ctypes.c_ubyte * 8)(*u.bytes[8:]))
    out = ctypes.c_wchar_p()
    if ctypes.windll.shell32.SHGetKnownFolderPath(ctypes.byref(g), 0, None, ctypes.byref(out)) != 0:
        return os.path.join(os.path.expanduser("~"), "Documents")
    path = out.value
    ctypes.windll.ole32.CoTaskMemFree(out)
    return path


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
        try:
            with open(os.path.join(root, "steamapps", "libraryfolders.vdf"), encoding="utf-8", errors="replace") as f:
                for m in re.finditer(r'"path"\s+"([^"]+)"', f.read()):
                    libraries.append(os.path.join(m.group(1).replace("\\\\", "\\"), "steamapps"))
        except OSError:
            pass
    return list(dict.fromkeys(os.path.normcase(os.path.normpath(l)) for l in libraries))


def find_game_dir():
    """The folder of stellaris.exe: STELLARIS_DIR, else the first Steam library that has it; None when there is none."""
    candidates = [os.environ.get("STELLARIS_DIR")] + [os.path.join(lib, "common", "Stellaris") for lib in steam_libraries()]
    return next((c for c in candidates if c and os.path.isfile(os.path.join(c, "stellaris.exe"))), None)


DATA_DIR = os.path.join(documents_dir(), "Paradox Interactive", "Stellaris")
PLUGIN_DIR = os.environ.get("L2D_PLUGIN_DIR") or os.path.join(DATA_DIR, "plugins", PLUGIN_ID)
CONFIG_INI = os.path.join(PLUGIN_DIR, "config", "stellaris_live2d.ini")
LOG = os.path.join(PLUGIN_DIR, "logs", "stellaris_live2d.log")
GAME_DIR = find_game_dir()  # None when Stellaris is not installed through Steam here and STELLARIS_DIR is not set
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PERF_SCRIPTS = os.environ.get("STELLARIS_PERF_SCRIPTS") or os.path.normpath(os.path.join(REPO, "..", "stellaris-perf", "bench", "scripts"))


def need_game_dir():
    if not GAME_DIR:
        raise SystemExit("could not find Stellaris in the Steam libraries: set STELLARIS_DIR to the folder of stellaris.exe")
    return GAME_DIR
