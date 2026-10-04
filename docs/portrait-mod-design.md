# Portrait-group mods

A mod carries the art; the plugin (`stellaris_live2d.dll`) only supplies the runtime. A mod says which of its portraits are
Live2D (or, later, Spine) and how they react, in the same script syntax the game uses to register portraits. Without the
plugin the mod does nothing and the game draws the portraits it always drew.

## What the engine tells us (Stellaris 4.5.1, found by static analysis, checked in the game)

- A `portraits = { <key> = { entity, clothes_selector, attachment_selector, character_textures, ... } }` entry in
  `gfx/portraits/portraits/*.txt` defines a portrait. `portrait_groups = { <name> = { ... } }` (same folder) chooses a key by
  scope and trigger, and `common/portrait_sets` puts groups and keys on species classes. For the humans the group `human`
  picks one of `human_male_01..05` / `human_female_01..05`.
- Every `CPortraitObject` (the thing that renders one portrait into a render target every frame) holds the key it resolved, as
  an engine string at `+0x818` (found by `tools/locate.py` from the lookup in `UpdatePortrait`: `lea r8,[this+KEY]` before the
  database `Find`). The key is what the plugin binds on, so no lookup into the definition database is needed. Objects that show
  no portrait (planets) have an empty key.
- The parser of `portraits = {}` entries (`CPortraitDatabase::ReadPortraitsFile`) skips unknown keys, **including whole
  `{ ... }` values**, and logs one `Unexpected token` line per statement in `error.log`. It is not fatal and the entry still
  loads. Because of that log noise the plugin's keys are best kept in a file the engine never reads (below), but the plugin
  reads them from either place.
- Duplicate keys: the last definition wins. Files load in name order, mods after the game.
- Scope of an object (leader, species, pop group, event) is stored next to the key (`+0x248` type, `+0x250` id) and the sprite
  type says what kind of widget it is; neither is used yet. They are the way to bind per leader or per screen later.

## Where the plugin looks

For every mod in the playset (`Documents\Paradox Interactive\Stellaris\dlc_load.json`, `enabled_mods`; the mod's `.mod` file
gives its `path`), and for the folders in the ini key `extra_mod_dirs` (development):

1. `gfx/portraits/live2d/*.txt`: the plugin's own folder. The engine does not read it, so nothing is logged about the extra keys.
2. `gfx/portraits/portraits/*.txt`, only files that mention `live2d` or `spine`.

In both, entries of a `portraits = { }` block (or of a block named `live2d_portraits`) that have `live2d = yes` or `spine = yes`
register their key. The scan repeats when the playset or one of these files changes.

## The keys

```
portraits = {
	human_female_01 = {            # a key the game already has, or a new one the mod registered the usual way
		live2d = yes               # draw it as a Live2D model
		spine  = no                # or as a Spine skeleton (read, not drawn yet)
		live2d_model = "gfx/live2d/pa15/model.model3.json"     # inside the mod
		live2d_view = { auto = yes  body = 0.46 }               # or { x = 0.44  y = 0.19  height = 0.26 }
		live2d_actions = {
			mouse_follow = { enabled = yes  strength = 0.6 }
			click        = { enabled = yes  motion_group = "TapBody"  motion_index = -1  expression = "" }
			drag         = { enabled = no   strength = 1.0 }
			scale        = { enabled = yes  min = 0.8  max = 1.6 }
		}
	}
}
```

| Key | Meaning |
|---|---|
| `live2d`, `spine` | booleans: which runtime draws the portrait. Neither: the game draws it |
| `live2d_model` | path of the `model3.json` relative to the mod root |
| `live2d_view` | the part of the model canvas shown, as fractions of the canvas. `auto = yes` works it out from the model's geometry: it starts a little above the head and covers `body` (default `0.46`) of the figure's height. Otherwise `x`, `y` (centre from the left and from the top) and `height` |
| `live2d_actions.mouse_follow` | the model looks towards the mouse pointer (`strength` 0..1): head turn (`ParamAngleX/Y/Z`), a little body lean (`ParamBodyAngleX`) and the eyes (`ParamEyeBallX/Y`), the values Cubism's own samples drive for a drag, eased over about 0.15 s and added on top of the motion every frame. Back to the middle while the game is not the foreground window. The ini key `interactions=0` turns all interactions off |
| `live2d_actions.click` | clicking the portrait starts a motion from `motion_group` (`motion_index` -1 = random) and/or an expression |
| `live2d_actions.drag` | dragging on the portrait moves the model's look/body parameters |
| `live2d_actions.scale` | the mouse wheel over the portrait zooms between `min` and `max` |

**Status:** the registration, model loading and drawing work (checked in the game with a mod that replaces the human
portraits). `mouse_follow` works (checked in the game: the head and eyes of the large leader portrait follow the pointer to the
four screen edges); until the screen rectangle of each portrait is known the target is the pointer's place relative to the
middle of the game window, not relative to the portrait. `click`, `drag` and `scale` are parsed and kept but not implemented:
they need the screen rectangle of each portrait, which the engine does not keep on the portrait object (the GUI sprite that
shows the render target knows it), plus mouse input.

## Making a mod

1. Pack each model's textures to DXT5 (the format of the game's own textures; a quarter of the video memory, no mip building at
   load): `l2d_pack --in <model folder> --out <folder>` (any tool that writes DXT5 `.dds` with mips works as well).
2. Put the packed model folders in the mod under `gfx/live2d/`.
3. Register the keys in `gfx/portraits/live2d/*.txt` as above. To replace an existing portrait use its key; to add a portrait
   register it the usual way (`entity` etc., which is also what a game without the plugin shows) and give the same key the
   plugin's keys.
4. `scripts/make_human_mod.py` does all of this for the test models and the human portrait keys.

## Open points

- Spine: nothing draws yet.
- Binding on more than the key: per leader, per species, per screen (the object's scope and sprite type are available).
- Interactions (above) and a way for events or scripts to trigger a motion or expression.
- Council slots are narrow and the engine may mirror a portrait depending on the slot, so text or logos in a model can appear
  reversed there; a per-portrait flip option is a candidate.
