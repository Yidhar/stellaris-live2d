"""Where the plugin and its files are (plugin spec v2 of the Stellaris launcher, its docs/PLUGINS.md):

    Documents\\Paradox Interactive\\Stellaris\\plugins\\stellaris-live2d\\
        stl-plugin.json, stellaris_live2d.dll, Live2DCubismCore.dll, defaults\\
        config\\stellaris_live2d.ini     the settings (the launcher's Plugins page edits it; the plugin re-reads it every 2 s)
        logs\\stellaris_live2d.log       the plugin's log

Shared by the scripts and tools of this repository.
"""
import ctypes
import ctypes.wintypes as w
import os
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


DATA_DIR = os.path.join(documents_dir(), "Paradox Interactive", "Stellaris")
PLUGIN_DIR = os.environ.get("L2D_PLUGIN_DIR") or os.path.join(DATA_DIR, "plugins", PLUGIN_ID)
CONFIG_INI = os.path.join(PLUGIN_DIR, "config", "stellaris_live2d.ini")
LOG = os.path.join(PLUGIN_DIR, "logs", "stellaris_live2d.log")
GAME_DIR = os.environ.get("STELLARIS_DIR", r"E:\Program Files (x86)\Steam\steamapps\common\Stellaris")
