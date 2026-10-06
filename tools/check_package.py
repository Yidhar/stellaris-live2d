"""Checks a release zip against the plugin spec of the Stellaris launcher (its docs/PLUGINS.md), as CI does before publishing:

    python tools/check_package.py package/stellaris-live2d-<version>.zip [--tag vX.Y.Z]

- the zip's root is the plugin folder: stl-plugin.json, the DLL it names, the defaults its config entries name; no config/ (the user's)
- the manifest is schema 2 and names its update source (update.github, update.asset) and the zip's name matches update.asset
- with --tag: the manifest's version is the tag without the leading v
- <zip>.sha256 exists and its first field is the zip's SHA-256
"""
import argparse
import fnmatch
import hashlib
import json
import os
import sys
import zipfile


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("zip")
    ap.add_argument("--tag")
    a = ap.parse_args()
    problems = []
    with zipfile.ZipFile(a.zip) as z:
        names = [n.replace("\\", "/") for n in z.namelist()]
        if "stl-plugin.json" not in names:
            raise SystemExit("FAIL: no stl-plugin.json at the zip's root (the zip must hold the plugin folder's contents, not a folder)")
        m = json.loads(z.read("stl-plugin.json").decode("utf-8-sig"))
        if m.get("schema") != 2:
            problems.append(f"schema is {m.get('schema')!r}, not 2")
        if m.get("dll") not in names:
            problems.append(f"the DLL {m.get('dll')!r} is not in the zip")
        for c in m.get("config", []):
            if c.get("default") and c["default"] not in names:
                problems.append(f"the default {c['default']!r} is not in the zip")
        if any(n == "config" or n.startswith("config/") for n in names):
            problems.append("config/ is packed (it is the user's and must not be)")
        up = m.get("update") or {}
        if not up.get("github") or not up.get("asset"):
            problems.append("update.github / update.asset are missing")
        elif not fnmatch.fnmatch(os.path.basename(a.zip), up["asset"]):
            problems.append(f"the zip's name {os.path.basename(a.zip)!r} does not match update.asset {up['asset']!r}")
        if not m.get("game", {}).get("exe_timestamps"):
            problems.append("game.exe_timestamps is empty")
    if a.tag:
        want = a.tag[1:] if a.tag.startswith("v") else a.tag
        if m.get("version") != want:
            problems.append(f"version {m.get('version')!r} is not the tag's {want!r}")
    sha = a.zip + ".sha256"
    if not os.path.exists(sha):
        problems.append(f"{sha} is missing")
    else:
        digest = hashlib.sha256(open(a.zip, "rb").read()).hexdigest()
        first = open(sha, encoding="ascii").read().split()[0].lower()
        if first != digest:
            problems.append(f"{sha} says {first}, the zip is {digest}")
    for p in problems:
        print("FAIL:", p)
    if problems:
        return 1
    print(f"ok: {os.path.basename(a.zip)} is plugin {m['id']} {m['version']} for {', '.join(m['game']['exe_timestamps'])}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
