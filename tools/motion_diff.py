"""Shows what a motion group of a Live2D model does that none of the model's other motion groups do, to find the parameters of a stage
effect (a black fade-in, a camera move, a spotlight) that the model author put into one motion, usually `login`, and that should not play inside
a portrait. Prints the `ignore_parameters` line for a portrait entry.

    python tools/motion_diff.py <model3.json> [group] [--all]

`group` is `login` by default. A parameter is listed when the group moves it and every other group leaves it alone (constant, or not keyed), or
when the group holds it at a value the other groups do not. Parameters whose name looks like a stage effect are marked `*` and put in the suggested line; the others (the glass the character
raises in the login of a wedding model, say) are listed without it, since they are part of the animation. `--all` puts every listed
parameter in the suggestion.
"""
import json
import os
import re
import sys

STAGE = re.compile(r"black|dark|heimu|curtain|mask|mengban|cover|camera|cam|sun|zoom|qianjin|lens|jingtou|kaiguan|spotlight|guang|shan|stage|scene|"
                   r"fade|bg|background|frame|xiangkuang|xiangpian", re.I)


def curve_range(segments):
    """Lowest and highest value a curve's points (control points included) reach."""
    values = [segments[1]]
    i = 2
    while i < len(segments):
        kind = int(segments[i])
        if kind == 1:  # bezier: two control points and the end
            values += [segments[i + 2], segments[i + 4], segments[i + 6]]
            i += 7
        else:
            values.append(segments[i + 2])
            i += 3
    return min(values), max(values)


def group_curves(base, entries):
    """parameter id -> (lowest, highest) over all motions of a group."""
    out = {}
    for entry in entries:
        try:
            with open(os.path.join(base, entry["File"]), encoding="utf-8-sig") as f:
                curves = json.load(f)["Curves"]
        except (OSError, ValueError, KeyError):
            continue
        for c in curves:
            if c.get("Target") != "Parameter":
                continue
            lo, hi = curve_range(c["Segments"])
            old = out.get(c["Id"])
            out[c["Id"]] = (lo, hi) if old is None else (min(old[0], lo), max(old[1], hi))
    return out


def unique_to(model3, group):
    with open(model3, encoding="utf-8-sig") as f:
        motions = json.load(f)["FileReferences"].get("Motions", {})
    if group not in motions:
        raise SystemExit(f"{model3} has no motion group `{group}` (it has: {', '.join(sorted(motions))})")
    base = os.path.dirname(model3)
    mine = group_curves(base, motions[group])
    elsewhere = {}
    for name, entries in motions.items():
        if name == group:
            continue
        for pid, (lo, hi) in group_curves(base, entries).items():
            old = elsewhere.get(pid)
            elsewhere[pid] = (lo, hi) if old is None else (min(old[0], lo), max(old[1], hi))
    found = []
    eps = 1e-4
    for pid, (lo, hi) in mine.items():
        other = elsewhere.get(pid)
        if other is not None and other[1] - other[0] >= eps:
            continue  # other groups move it as well: part of the animation
        if other is None:
            if hi - lo < eps:
                continue  # nobody moves it
        elif hi - lo < eps and abs(lo - other[0]) < eps:
            continue  # the group holds it where the others do
        found.append((pid, lo, hi, bool(STAGE.search(pid))))
    return found


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    if not args:
        raise SystemExit(__doc__)
    model3, group = args[0], args[1] if len(args) > 1 else "login"
    found = unique_to(model3, group)
    if not found:
        print(f"no parameter is moved only by `{group}`: nothing to ignore")
        return
    for pid, lo, hi, stage in sorted(found, key=lambda f: (not f[3], f[0])):
        print(f"  {'*' if stage else ' '} {pid:32s} {lo:8.2f} .. {hi:8.2f}")
    chosen = [f[0] for f in found if f[3] or "--all" in sys.argv]
    if chosen:
        print("\nlive2d_ignore_parameters = { " + " ".join(f'"{p}"' for p in chosen) + " }")
    else:
        print("\nnone of them looks like a stage effect (use --all to ignore them anyway)")


if __name__ == "__main__":
    main()
