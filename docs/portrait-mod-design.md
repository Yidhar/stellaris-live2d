# Portrait-group mods

A mod carries the art; the plugin (`stellaris_live2d.dll`) only supplies the runtime. A mod says which of its portraits are
Live2D (or, later, Spine) and how they react, in the same script syntax the game uses to register portraits. Without the
plugin the mod does nothing and the game draws the portraits it always drew.

See also [engine-notes.md](engine-notes.md) for the engine facts the plugin relies on.

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
		live2d_unmirror = yes      # (default) where the GUI mirrors the portrait, draw it flipped so it comes out the right way round
		spine  = no                # or as a Spine skeleton (read, not drawn yet)
		live2d_model = "gfx/live2d/pa15/model.model3.json"     # inside the mod
		live2d_view = { auto = yes  body = 0.46 }               # or { x = 0.44  y = 0.19  height = 0.26 }
		live2d_scale = 1.0         # magnifies the framed part around its centre: 1.25 shows a quarter less of the model
		live2d_actions = {
			mouse_follow = { enabled = yes  strength = 0.6 }
			click        = { enabled = yes  motion_group = "touch*"  motion_index = -1 }   # or motion_groups = { touch_1 touch_2 }
			                                                                              # + voices = { touch_1 = "sound/a.wav" }  sounds = { "sound/b.wav" }  volume = 1.0
			drag         = { enabled = no   strength = 1.0 }
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
| `live2d_unmirror` | the GUI mirrors some portraits (the council does for some slots), which reverses text and logos in a model. With this on (the default) the plugin draws the picture flipped in those portraits, so the GUI's mirror turns it back; the mouse follow accounts for it |
| `live2d_actions.click` | a left click that lands on the portrait (inside the rectangle the GUI draws it in and its clip area) starts a motion from one of the groups named by `motion_group` or `motion_groups`, picked at random but never the one played last; `motion_index` -1 = a random motion of the group. A name ending in `*` matches every group that starts with the rest (`touch*` = `touch_1`, `touch_2`, ..., never `Idle`). The click is not swallowed: a button the GUI draws over the portrait is pressed too. `expression` is read but not implemented |
| voice lines | what the portrait says when a click starts a motion. Per motion group: `voices = { touch_1 = "sound/a.wav"  touch_2 = { "sound/b.wav" "sound/c.wav" }  wait* = "..." }` (the key is the motion's group name, exact names are tried before prefix patterns ending in `*`; several lines are taken in turn; paths are inside the mod). A motion with no entry there says one of `sounds = { "sound/d.wav" "sound/e.ogg" }` (or `sound = "..."`, taken in turn), if the click names any, else the `Sound` of the motion in `model3.json` (the standard `"Sound": "voice/touch_1.wav"` key of a `Motions` entry, relative to the model's folder; it is how to bind a line to one particular motion file, not just a group). WAV, MP3, FLAC and Ogg Vorbis are played (not Opus or AAC). `click.volume` (default 1) scales a line; the ini keys `audio` (default 1) and `volume` (default 0.8) switch the voice off and set the master volume. A new line cuts off the portrait's previous one. Only motions started by a click speak; the idle and wait motions do not |
| `live2d_actions.drag` | dragging on the portrait moves the model's look/body parameters |
| `live2d_scale` | a fixed magnification of the framed part, around its centre (0.1 to 10, default 1): the framed height is divided by it, so 1.3 shows a bit more than three quarters of what `live2d_view` frames. A model is loaded once however many portrait keys use it, with whatever views and scales they give it; it also has one animation state, so two keys of one model that are on screen together show the same pose. There is no interactive zoom: it was left out as not needed |

**Status:** the registration, model loading and drawing work (checked in the game with a mod that replaces the human
portraits). `mouse_follow` works (checked in the game: the head and eyes of the large leader portrait follow the pointer to the
four screen edges, and in the council with the pointer around the four portraits, where the mirrored ones turn the right way).
The look target is the pointer relative to the portrait's own place on the screen. `click` (motion and voice line) and `live2d_unmirror` are implemented
(the click polls the left button once per frame, so a press shorter than a frame can be missed; a window drawn over the portrait does
not stop the click from counting). `drag` is parsed and kept but not implemented.

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
- Clicks do not know what the GUI has drawn over a portrait (a window, a tooltip); only the portrait's own clip area is respected.
- Two portraits that share a model share its frame and its look target (the first one asked decides).
