"""Builds a test portrait-group mod that gives the humans Live2D portraits.

What it makes, the way a real portrait mod would:
  * gfx/portraits/portraits/zz_live2d_humans.txt registers ten new portraits (l2d_human_female_01..05, l2d_human_male_01..05) the usual way,
    each a copy of a vanilla human portrait (so a game without the plugin shows an ordinary human), and overrides the portrait group `human`
    so that leaders and rulers pick from one subset of them, pops from another and species from all (the group syntax of the game:
    `add = { trigger = ... portraits = { ... } }` per scope; the first entry of each scope is a `set`, which drops what vanilla listed:
    a group defined in several files is merged, so with `add` alone the vanilla portraits would still be picked);
  * gfx/portraits/live2d/00_live2d_humans.txt (read by the plugin, not by the game) says which of the new portraits are Live2D, with
    which model, framing, events, voice lines and expression;
  * gfx/live2d/ holds the models (DXT5 textures, see l2d_pack), sound/ the voice lines.

    python make_human_mod.py [--out <mod folder>] [--models <folder with packed models>] [--enable] [--export-demo <folder>] [--ignore-stage]

By default every portrait plays the model's `login` motion as the author made it when it shows up, stage effects included (a black curtain
that fades away, a camera move). --ignore-stage lists the parameters of those effects in `live2d_ignore_parameters` (found by
tools/motion_diff.py) so only the character's own animation plays, and gives the one model whose login is a whole scene a wait motion.

--enable also adds the mod to the playset (dlc_load.json, the original is kept as dlc_load.json.live2d_backup).
--export-demo copies only the text of the mod (descriptor.mod and the two portrait files, no models, no sound) into a folder: the demo mod
repository is kept that way, since the test models are third-party art.
"""
import argparse
import json
import os
import re
import shutil
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "tools"))
import motion_diff  # noqa: E402

DOCS = os.path.join(os.path.expanduser("~"), "Documents", "Paradox Interactive", "Stellaris")
GAME = os.environ.get("STELLARIS_DIR", r"E:\Program Files (x86)\Steam\steamapps\common\Stellaris")
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MOD_NAME = "live2d_humans"

# vanilla portrait -> model folder. The test models are all female characters; the male portraits get the ones left over.
ASSIGN = {
    "human_female_01": "pa15_5802", "human_female_02": "d243_s2901", "human_female_03": "d261_s3801",
    "human_female_04": "d119_s3001", "human_female_05": "d95_s50001",
    "human_male_01": "d307_s4703", "human_male_02": "d253_s6901", "human_male_03": "d351_s9001",
    "human_male_04": "d316_s11603", "human_male_05": "d243_s2901",
}
# which of the new portraits each scope of the group picks from (numbers 01..05): leaders and rulers the first three, pops the last two,
# species all of them
SCOPES = {"game_setup": [1, 2, 3], "species": [1, 2, 3, 4, 5], "pop": [4, 5], "leader": [1, 2, 3], "ruler": [1, 2, 3]}
# parameters of stage effects that every motion of a model plays besides the one found by tools/motion_diff.py in `login` (the black
# curtain of this model is also keyed by its touch motions)
EXTRA_IGNORE = {"d316_s11603": ["ParamHeiMu*"]}
# the motion the portrait plays when it shows up: the model's login, except for a model whose login is a whole staged scene (photo frames, light
# sweeps and a character switch, all keyed by parameters that cannot be told apart from the character's own), which plays a wait motion
APPEAR = {"d351_s9001": "wait*"}
# a model whose automatic crop is wrong gets its own
VIEWS = {"d307_s4703": "x = 0.48 y = 0.49 height = 0.17"}
# live2d_scale per portrait, to show the option: this one is magnified by 30 percent
SCALES = {"l2d_human_female_04": 1.3}
# lines bound to single motion groups, to show `voices`: this portrait says line1 for touch_1, line2 for touch_2, line3 for touch_3, and
# nothing for its other touch motions (the others say any of the lines in turn)
VOICES = {"l2d_human_female_01": {"touch_1": "line1.wav", "touch_2": "line2.wav", "touch_3": "line3.wav"}}


def new_key(vanilla):
    return "l2d_" + vanilla


def vanilla_entry(text, key):
    """The text of `key = { ... }` in the top-level portraits block of a vanilla portraits file."""
    m = re.search(r"^[ \t]*" + re.escape(key) + r"[ \t]*=[ \t]*\{", text, re.M)
    if not m:
        raise SystemExit(f"{key} is not in the vanilla file")
    depth, i = 0, m.end() - 1
    while True:
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return text[m.start():i + 1]
        i += 1


def group_text():
    def gender_adds(trigger_female, trigger_male, numbers):
        def entry(verb, trigger, gender):
            keys = "\n".join(f"\t\t\t\t\t{new_key('human_' + gender + '_%02d' % n)}" for n in numbers)
            return f"\t\t\t{verb} = {{\n\t\t\t\ttrigger = {{\n{trigger}\n\t\t\t\t}}\n\t\t\t\tportraits = {{\n{keys}\n\t\t\t\t}}\n\t\t\t}}\n"
        # `set` in front: the game drops the entries earlier files (vanilla) gave this scope, then the `add` that follows adds to ours only
        return entry("set", trigger_male, "male") + entry("add", trigger_female, "female")

    ruler_male = "\t\t\t\t\truler = { OR = { gender = male gender = indeterminable } }"
    ruler_female = "\t\t\t\t\truler = { OR = { gender = female gender = indeterminable } }"
    own_male = "\t\t\t\t\tOR = { gender = male gender = indeterminable }"
    own_female = "\t\t\t\t\tOR = { gender = female gender = indeterminable }"
    species_male = "\t\t\t\t\texists = species\n\t\t\t\t\tNOT = { species = { species_gender = female } }"
    species_female = "\t\t\t\t\texists = species\n\t\t\t\t\tNOT = { species = { species_gender = male } }"
    pop_male = "\t\t\t\t\tNOT = { species = { species_gender = female } }"
    pop_female = "\t\t\t\t\tNOT = { species = { species_gender = male } }"
    triggers = {"game_setup": (ruler_female, ruler_male), "species": (species_female, species_male), "pop": (pop_female, pop_male),
                "leader": (own_female, own_male), "ruler": (own_female, own_male)}
    out = "portrait_groups = {\n\thuman = {\n\t\tdefault = " + new_key("human_male_01") + "\n"
    for scope, numbers in SCOPES.items():
        female, male = triggers[scope]
        out += f"\t\t{scope} = {{\n" + gender_adds(female, male, numbers) + "\t\t}\n"
    return out + "\t}\n}\n"


def ignored_parameters(models_folder, model):
    """The stage parameters of the model's login motion (found by tools/motion_diff.py) and the extra ones, for `live2d_ignore_parameters`."""
    found = motion_diff.unique_to(os.path.join(models_folder, model, "model.model3.json"), "login")
    return [pid for pid, _, _, stage in found if stage] + EXTRA_IGNORE.get(model, [])


def live2d_entry(key, model, sounds, ignore):
    view = VIEWS.get(model)
    view = f"{{ {view} }}" if view else "{ auto = yes  body = 0.46 }"
    lines = " ".join(f'"{s}"' for s in sounds)
    say = f"  sounds = {{ {lines} }}" if sounds else ""
    if key in VOICES:
        bound = " ".join(f'{group} = "sound/live2d_test/{name}"' for group, name in VOICES[key].items())
        say = f"  voices = {{ {bound} }}"
    appear = APPEAR.get(model, "login")
    scale = f"\t\tlive2d_scale = {SCALES[key]}\n" if key in SCALES else ""
    ignore_line = ("\t\tlive2d_ignore_parameters = { " + " ".join(f'"{p}"' for p in ignore) + " }\n") if ignore else ""
    return f"""\t{key} = {{
\t\tlive2d = yes
\t\tlive2d_unmirror = yes
{scale}{ignore_line}\t\tlive2d_model = "gfx/live2d/{model}/model.model3.json"
\t\tlive2d_view = {view}
\t\tlive2d_actions = {{
\t\t\tmouse_follow = {{ enabled = yes  strength = 0.6 }}
\t\t\tclick = {{ motion_group = "touch*"{say} }}
\t\t\tclick_head = {{ motion_group = "touch*" expression = "smile"{say} }}
\t\t\thover = {{ expression = "smile"  expression_hold = 1.5 }}
\t\t\tappear = {{ motion_group = "{appear}"  expression = "smile"  expression_hold = 2 }}
\t\t\tidle = {{ motion_group = "wait*"  interval = {{ 15 30 }} }}
\t\t\tgreeting = {{ motion_group = "touch*"{say} }}
\t\t}}
\t}}
"""


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", default=os.path.join(DOCS, "mod", MOD_NAME))
    ap.add_argument("--models", default=os.path.join(ROOT, "models_dxt5"))
    ap.add_argument("--enable", action="store_true")
    ap.add_argument("--ignore-stage", action="store_true", help="skip the stage parameters of each model's login motion (see above)")
    ap.add_argument("--export-demo", metavar="FOLDER", help="also copy the mod's text files (no art) into FOLDER")
    ap.add_argument("--voices", default=os.path.join(ROOT, "scratch", "voice"),
                    help="folder of WAV/MP3/FLAC/OGG lines every portrait says when clicked (see make_test_voices.ps1); none if missing")
    a = ap.parse_args()

    if os.path.isdir(a.out):
        shutil.rmtree(a.out)
    for model in sorted(set(ASSIGN.values())):
        folder = os.path.join(a.out, "gfx", "live2d", model)
        shutil.copytree(os.path.join(a.models, model), folder)
        # the test models have no expressions: give each a small one, the way a model author would (exp3.json + the entry in model3.json)
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

    # the engine's side: the new portraits (copies of the vanilla ones) and the overridden group
    with open(os.path.join(GAME, "gfx", "portraits", "portraits", "07_portraits_human.txt"), encoding="utf-8-sig") as f:
        vanilla = f.read()
    entries = []
    for key in ASSIGN:
        text = vanilla_entry(vanilla, key)
        entries.append(re.sub(r"^([ \t]*)" + re.escape(key), r"\1" + new_key(key), text, count=1))
    engine_dir = os.path.join(a.out, "gfx", "portraits", "portraits")
    os.makedirs(engine_dir)
    with open(os.path.join(engine_dir, "zz_live2d_humans.txt"), "w", encoding="utf-8", newline="\n") as f:
        f.write("# Ten portraits that look like the vanilla human ones to a game without the plugin, and the human portrait group choosing among them.\n"
                "# The Live2D side of them is in gfx/portraits/live2d/.\n\nportraits = {\n" + "\n".join(entries) + "\n}\n\n" + group_text())

    # the plugin's side
    ignore = {m: ignored_parameters(a.models, m) if a.ignore_stage else [] for m in set(ASSIGN.values())}
    global APPEAR
    if not a.ignore_stage:
        APPEAR = {}
    side = os.path.join(a.out, "gfx", "portraits", "live2d")
    os.makedirs(side)
    with open(os.path.join(side, "00_live2d_humans.txt"), "w", encoding="utf-8", newline="\n") as f:
        f.write("# Which of this mod's portraits are Live2D, and how. Read by stellaris_live2d.dll, not by the game.\n\n"
                "portraits = {\n" + "".join(live2d_entry(new_key(k), m, sounds, ignore[m]) for k, m in ASSIGN.items())
                + "\n\t# a leader a script or the empire designer gave one of the vanilla portraits by name is not drawn from the group, so the\n"
                  "\t# vanilla keys get the same models\n" + "".join(live2d_entry(k, m, sounds, ignore[m]) for k, m in ASSIGN.items()) + "}\n")

    descriptor = ('version="0.2.0"\ntags={\n\t"Graphics"\n\t"Species"\n}\nname="Live2D Human Portraits (test)"\n'
                  'supported_version="v4.5.*"\n')
    with open(os.path.join(a.out, "descriptor.mod"), "w", encoding="utf-8", newline="\n") as f:
        f.write(descriptor)
    outer = os.path.join(os.path.dirname(a.out), MOD_NAME + ".mod")
    with open(outer, "w", encoding="utf-8", newline="\n") as f:
        f.write(descriptor + f'path="{a.out.replace(os.sep, "/")}"\n')
    size = sum(os.path.getsize(os.path.join(d, n)) for d, _, ns in os.walk(a.out) for n in ns)
    print(f"mod written to {a.out} ({size / 1048576:.1f} MB), descriptor {outer}; {len(ASSIGN)} portraits, "
          f"{len(set(ASSIGN.values()))} models")

    if a.export_demo:
        # the demo does not name the test models (their folder names are ids of the third-party art): model_01, model_02, ...
        names = {m: f"model_{i:02d}" for i, m in enumerate(sorted(set(ASSIGN.values())), 1)}
        for rel in ("descriptor.mod", "gfx/portraits/portraits/zz_live2d_humans.txt", "gfx/portraits/live2d/00_live2d_humans.txt"):
            dst = os.path.join(a.export_demo, *rel.split("/"))
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            with open(os.path.join(a.out, *rel.split("/")), encoding="utf-8") as f:
                text = f.read()
            with open(dst, "w", encoding="utf-8", newline="\n") as f:
                text = text.replace("Live2D Human Portraits (test)", "Live2D Human Portraits (demo)").replace("sound/live2d_test/", "sound/demo/")
                for model, name in names.items():
                    text = text.replace(f"gfx/live2d/{model}/", f"gfx/live2d/{name}/")
                f.write(text)
        print(f"text files exported to {a.export_demo}")
        for key, model in ASSIGN.items():
            print(f"  {new_key(key)} (and {key}) -> gfx/live2d/{names[model]}/")

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
