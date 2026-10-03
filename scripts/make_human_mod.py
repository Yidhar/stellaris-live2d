"""Builds a test portrait-group mod that replaces the vanilla human portraits with Live2D models.

The mod holds the models (DXT5 texture files, see l2d_pack) under gfx/live2d/ and one script file under
gfx/portraits/live2d/ that registers the plugin's keys for the vanilla portrait keys of the human portrait group
(human_male_01..05, human_female_01..05 and the legacy ones). The engine never reads that folder, and a game without the
plugin shows the vanilla portraits as before.

    python make_human_mod.py [--out <mod folder>] [--models <folder with packed models>] [--enable]

--enable also adds the mod to the playset (dlc_load.json, the original is kept as dlc_load.json.live2d_backup).
"""
import argparse
import json
import os
import shutil

DOCS = os.path.join(os.path.expanduser("~"), "Documents", "Paradox Interactive", "Stellaris")
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MOD_NAME = "live2d_humans"

# portrait key -> model folder. The test models are all female characters; the male keys get the ones left over.
ASSIGN = {
    "human_female_01": "pa15_5802", "human_female_02": "d243_s2901", "human_female_03": "d261_s3801",
    "human_female_04": "d119_s3001", "human_female_05": "d95_s50001",
    "human_male_01": "d307_s4703", "human_male_02": "d253_s6901", "human_male_03": "d351_s9001",
    "human_male_04": "d316_s11603", "human_male_05": "d243_s2901",
}
# a model whose automatic crop is wrong gets its own
VIEWS = {"d307_s4703": "x = 0.48 y = 0.49 height = 0.17"}


def entry(key, model):
    view = VIEWS.get(model)
    view = f"{{ {view} }}" if view else "{ auto = yes  body = 0.46 }"
    return f"""	{key} = {{
		live2d = yes
		live2d_model = "gfx/live2d/{model}/model.model3.json"
		live2d_view = {view}
		live2d_actions = {{
			mouse_follow = {{ enabled = yes  strength = 0.6 }}
			click = {{ enabled = yes  motion_group = "TapBody" }}
			drag = {{ enabled = no }}
			scale = {{ enabled = yes  min = 0.8  max = 1.6 }}
		}}
	}}
"""


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", default=os.path.join(DOCS, "mod", MOD_NAME))
    ap.add_argument("--models", default=os.path.join(ROOT, "models_dxt5"))
    ap.add_argument("--enable", action="store_true")
    a = ap.parse_args()

    if os.path.isdir(a.out):
        shutil.rmtree(a.out)
    for model in sorted(set(ASSIGN.values())):
        shutil.copytree(os.path.join(a.models, model), os.path.join(a.out, "gfx", "live2d", model))

    keys = dict(ASSIGN)
    for key, model in ASSIGN.items():  # the legacy human portraits use the same models
        keys[key.replace("human_", "human_legacy_")] = model
    side = os.path.join(a.out, "gfx", "portraits", "live2d")
    os.makedirs(side)
    with open(os.path.join(side, "00_live2d_humans.txt"), "w", encoding="utf-8", newline="\n") as f:
        f.write("# Live2D replacements for the human portraits. Read by stellaris_live2d.dll, not by the game.\n"
                "# The keys are the ones of the `portraits = { }` entries in gfx/portraits/portraits/07_portraits_human.txt.\n\n"
                "portraits = {\n" + "".join(entry(k, m) for k, m in keys.items()) + "}\n")

    descriptor = ('version="0.1.0"\ntags={\n\t"Graphics"\n\t"Species"\n}\nname="Live2D Human Portraits (test)"\n'
                  'supported_version="v4.5.*"\n')
    with open(os.path.join(a.out, "descriptor.mod"), "w", encoding="utf-8", newline="\n") as f:
        f.write(descriptor)
    outer = os.path.join(os.path.dirname(a.out), MOD_NAME + ".mod")
    with open(outer, "w", encoding="utf-8", newline="\n") as f:
        f.write(descriptor + f'path="{a.out.replace(os.sep, "/")}"\n')
    size = sum(os.path.getsize(os.path.join(d, n)) for d, _, ns in os.walk(a.out) for n in ns)
    print(f"mod written to {a.out} ({size / 1048576:.1f} MB), descriptor {outer}; {len(keys)} portrait keys, "
          f"{len(set(ASSIGN.values()))} models")

    if a.enable:
        playset = os.path.join(DOCS, "dlc_load.json")
        backup = playset + ".live2d_backup"
        if not os.path.exists(backup):
            shutil.copyfile(playset, backup)
        data = json.load(open(playset, encoding="utf-8"))
        entry_name = f"mod/{MOD_NAME}.mod"
        if entry_name not in data["enabled_mods"]:
            data["enabled_mods"].append(entry_name)
        with open(playset, "w", encoding="utf-8") as f:
            json.dump(data, f, separators=(",", ":"))
        print(f"enabled in {playset} (original kept as {backup})")


if __name__ == "__main__":
    main()
