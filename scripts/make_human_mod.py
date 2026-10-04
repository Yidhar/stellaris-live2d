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
# live2d_scale per portrait key, to show the option: the first council slot is magnified by 30 percent
SCALES = {"human_female_04": 1.3}
# lines bound to single motion groups, to show `voices`: this portrait says line1 for touch_1, line2 for touch_2, line3 for touch_3, and
# nothing for its other touch motions (the others say any of the lines in turn)
VOICES = {"human_female_01": {"touch_1": "line1.wav", "touch_2": "line2.wav", "touch_3": "line3.wav"}}


def entry(key, model, sounds):
    view = VIEWS.get(model)
    view = f"{{ {view} }}" if view else "{ auto = yes  body = 0.46 }"
    lines = " ".join(f'"{s}"' for s in sounds)
    say = f"  sounds = {{ {lines} }}" if sounds else ""
    if key in VOICES:
        bound = " ".join(f'{group} = "sound/live2d_test/{name}"' for group, name in VOICES[key].items())
        say = f"  voices = {{ {bound} }}"
    scale = f"		live2d_scale = {SCALES[key]}\n" if key in SCALES else ""
    return f"""	{key} = {{
		live2d = yes
		live2d_unmirror = yes
{scale}		live2d_model = "gfx/live2d/{model}/model.model3.json"
		live2d_view = {view}
		live2d_actions = {{
			mouse_follow = {{ enabled = yes  strength = 0.6 }}
			click = {{ motion_group = "touch*"{say} }}
			click_head = {{ motion_group = "touch*" expression = "smile"{say} }}
			hover = {{ expression = "smile"  expression_hold = 1.5 }}
			appear = {{ motion_group = "login" }}
			idle = {{ motion_group = "wait*"  interval = {{ 15 30 }} }}
		}}
	}}
"""


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", default=os.path.join(DOCS, "mod", MOD_NAME))
    ap.add_argument("--models", default=os.path.join(ROOT, "models_dxt5"))
    ap.add_argument("--enable", action="store_true")
    ap.add_argument("--voices", default=os.path.join(ROOT, "scratch", "voice"),
                    help="folder of WAV/MP3/FLAC/OGG lines every portrait says when clicked (see make_test_voices.ps1); none if missing")
    a = ap.parse_args()

    if os.path.isdir(a.out):
        shutil.rmtree(a.out)
    for model in sorted(set(ASSIGN.values())):
        shutil.copytree(os.path.join(a.models, model), os.path.join(a.out, "gfx", "live2d", model))
        # the test models have no expressions: give each a small one, the way a model author would (exp3.json + the entry in model3.json)
        folder = os.path.join(a.out, "gfx", "live2d", model)
        os.makedirs(os.path.join(folder, "expressions"), exist_ok=True)
        with open(os.path.join(folder, "expressions", "smile.exp3.json"), "w", encoding="utf-8") as f:
            json.dump({"Type": "Live2D Expression", "FadeInTime": 0.4, "FadeOutTime": 0.6, "Parameters": [
                {"Id": "ParamCheek", "Value": 1, "Blend": "Add"}, {"Id": "ParamEyeLSmile", "Value": 1, "Blend": "Add"},
                {"Id": "ParamEyeRSmile", "Value": 1, "Blend": "Add"}, {"Id": "ParamMouthForm", "Value": 0.8, "Blend": "Add"}]}, f, indent=1)
        model3 = os.path.join(folder, "model.model3.json")
        with open(model3, encoding="utf-8") as f:
            doc = json.load(f)
        doc["FileReferences"]["Expressions"] = [{"Name": "smile", "File": "expressions/smile.exp3.json"}]
        with open(model3, "w", encoding="utf-8") as f:
            json.dump(doc, f, indent=1)
    sounds = []
    if os.path.isdir(a.voices):
        for name in sorted(os.listdir(a.voices)):
            if os.path.splitext(name)[1].lower() in (".wav", ".mp3", ".flac", ".ogg"):
                os.makedirs(os.path.join(a.out, "sound", "live2d_test"), exist_ok=True)
                shutil.copyfile(os.path.join(a.voices, name), os.path.join(a.out, "sound", "live2d_test", name))
                sounds.append(f"sound/live2d_test/{name}")

    keys = dict(ASSIGN)
    for key, model in ASSIGN.items():  # the legacy human portraits use the same models
        keys[key.replace("human_", "human_legacy_")] = model
    side = os.path.join(a.out, "gfx", "portraits", "live2d")
    os.makedirs(side)
    with open(os.path.join(side, "00_live2d_humans.txt"), "w", encoding="utf-8", newline="\n") as f:
        f.write("# Live2D replacements for the human portraits. Read by stellaris_live2d.dll, not by the game.\n"
                "# The keys are the ones of the `portraits = { }` entries in gfx/portraits/portraits/07_portraits_human.txt.\n\n"
                "portraits = {\n" + "".join(entry(k, m, sounds) for k, m in keys.items()) + "}\n")

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
