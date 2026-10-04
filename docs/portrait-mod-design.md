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
		live2d_view_character_large = { auto = yes  body = 0.6 }   # another framing for one kind of portrait, or for one size (live2d_view_800x400)
		live2d_actions = {
			mouse_follow = { enabled = yes  strength = 0.6 }
			click      = { motion_group = "touch*" }                           # the left button went down on the picture
			click_head = { motion_group = "touch*"  expression = "smile" }     # ... on a hit area of the model (click_<Name>)
			hover      = { expression = "smile"  expression_hold = 1.5 }       # the pointer came onto the picture
			appear     = { motion_group = "wait*"  expression = "smile" }       # the portrait shows up
			idle       = { motion_group = "wait*"  interval = { 15 30 } }      # now and then while it is shown
			greeting   = { motion_group = "touch*"  replace_engine_sound = no }   # the game plays the portrait's greeting sound
		}
	}
}
```

| Key | Meaning |
|---|---|
| `live2d`, `spine` | booleans: which runtime draws the portrait. Neither: the game draws it |
| `live2d_model` | path of the `model3.json` relative to the mod root |
| `live2d_view` | the part of the model canvas shown, as fractions of the canvas. `auto = yes` works it out from the model's geometry: it starts a little above the head and covers `body` (default `0.46`) of the figure's height. Otherwise `x`, `y` (centre from the left and from the top) and `height` |
| `live2d_view_<kind>`, `live2d_view_<W>x<H>` | another framing for one kind of portrait (`character`, `character_large`, `room`, `empty_room`, `character_without_room`: what the engine says the portrait is for; the plugin log shows it for every key) or for one size of picture. A size beats a kind beats `live2d_view`. A block takes `auto`, `body`, `x`, `y`, `height` and `scale`; what it leaves out is the default's |
| `live2d_scale` | a fixed magnification of the framed part, around its centre (0.1 to 10, default 1): the framed height is divided by it, so 1.3 shows a bit more than three quarters of what `live2d_view` frames. A model is loaded once however many portrait keys use it, with whatever views and scales they give it; it also has one animation state, so two keys of one model that are on screen together show the same pose. There is no interactive zoom: it was left out as not needed |
| `live2d_unmirror` | the GUI mirrors some portraits (the council does for some slots), which reverses text and logos in a model. With this on (the default) the plugin draws the picture flipped in those portraits, so the GUI's mirror turns it back; the mouse follow accounts for it |
| `live2d_actions.mouse_follow` | the model looks towards the mouse pointer (`strength` 0..1): head turn (`ParamAngleX/Y/Z`), a little body lean (`ParamBodyAngleX`) and the eyes (`ParamEyeBallX/Y`), the values Cubism's own samples drive for a drag, eased over about 0.15 s and added on top of the motion every frame. Back to the middle while the game is not the foreground window. The ini key `interactions=0` turns all interactions and events off |

### What every model gets

For parameters no playing motion keys itself (a model whose idle loop keys them, like the test models, is left alone): Cubism's default
**breath** (head, body lean and `ParamBreath`), an **eye blink** for the parameters of the model's `EyeBlink` group (`model3.json` `Groups`),
and the **mouth** of the `LipSync` group opening with the loudness of the voice line the model is saying. Pictures are drawn at twice the
size and averaged down (ini `supersample`, default 2): measured against a 4x render it is within 51 dB where the plain picture is at 36
(multisampling was tried and changes under 0.1% of the pixels: the art's edges are texture alpha, not mesh edges).

### Loading

Models are loaded on a background thread, never on the game's render thread, and their textures go to the GPU there too (the pixel
data is freed afterwards). The memory the loaded models may take is the ini key `model_cache_mb` (default 512): within it every model
a mod names is loaded in advance, so a portrait finds its model ready; beyond it a model is loaded when a portrait first needs it,
and the game's own portrait shows until it is ready (a short pop-in the first time, then none), pushing out the model unused for
longest (never one used in the last three seconds, so a limit that is too small for what is on screen is exceeded rather than
flickering). Editing a mod's script reloads no model: only the settings of the portrait keys are rebuilt.

### Events

`live2d_actions` maps events to what happens. The motion groups, expressions and hit areas are **the model's own**, named in its
`model3.json` (`Motions`, `Expressions`, `HitAreas`): the mod does not register them, it only says which one an event plays. Models
name their groups differently (`touch_1`, `touch_01`, `Tap`...), so a group can be given as a prefix ending in `*`.

| Event | When |
|---|---|
| `click` | the left button goes down on the portrait's picture: inside the rectangle the GUI draws it in and inside its clip area, the nearest centre where portraits overlap. Only mouse messages that land there are looked at; every message still reaches the game, so a button drawn over the portrait is pressed as well, and a window drawn over it does not stop the click from counting |
| `click_<Name>` | the same, when the click is on the hit area `<Name>` of the model (`click_head`, `click_body`, `click_leg` for the test models; the comparison ignores case). Falls back to `click` when the model has no such area or the area has no action |
| `hover` | the pointer comes onto the picture |
| `appear` | the portrait shows up: the first time, or again after not being drawn for a while (a screen opened). Pick a motion that keeps the background transparent: the `login` motion of many models is a stage entrance that fades in from a black backdrop (the test models' does, for about six seconds), which looks like a dark box in the middle of a screen |
| `idle` | every `interval = { min max }` seconds (random in between) while the portrait is shown, not over a motion an event started |
| `greeting` | the game plays the portrait's own greeting sound (`greeting_sound` of its `portraits` entry: the diplomacy window opening, an incoming proposal, a species being previewed). The plugin hooks the engine function that fetches that sound; with `replace_engine_sound = yes` the game's sound is not played, so the action's line takes its place |

Each action may have:

| Key | Meaning |
|---|---|
| `motion_group = "touch*"` or `motion_groups = { a b }` | the groups to pick a motion from, at random but never the one played last; `*` at the end matches by prefix (never the `Idle` group). `motion_index` -1 = a random motion of the group. A motion that cannot be loaded makes the next group be tried |
| `expression = "smile"` | an expression of the model (the `Name` of an `Expressions` entry, an `exp3.json` file); it fades in, stays `expression_hold` seconds (default 3; 0 = until another is set) and fades out |
| `voices = { touch_1 = "sound/a.wav"  touch_2 = { "sound/b.wav" "sound/c.wav" } }` | the lines a motion group says: the key is a group name (exact names are tried before prefix patterns ending in `*`); several lines are taken in turn; paths are inside the mod |
| `sounds = { "sound/d.wav" "sound/e.ogg" }` or `sound = "..."` | the lines any other motion of the action says, in turn. A motion with neither says the `Sound` of the motion in `model3.json` (the standard key of a `Motions` entry, relative to the model's folder: the way to bind a line to one particular motion file) |
| `volume = 1` | scales the lines against the master volume |

WAV, MP3, FLAC and Ogg Vorbis lines are played (not Opus or AAC); a new line cuts off the portrait's previous one. The ini keys `audio`
(default 1) and `volume` (default 0.8) switch the voice off and set the plugin's own volume, which is then multiplied by the game's: master
volume, the hidden `dev_master_volume` and the slider named by `volume_channel` (`voice`, the default: the advisor and event speech slider;
`effects`: the sound effects slider, where the portraits' greeting sounds are; `none`: only `volume`), read from the game's settings object
every couple of seconds, so moving the sliders in the game's settings moves the voice. The game does not mute in the background and neither
does the plugin unless `mute_in_background=1`.

**Status:** registration, background loading, drawing, supersampling, `mouse_follow`, `live2d_unmirror`, `live2d_scale`, the per-kind
views, the events above, expressions, hit areas, breath, blink and lip sync are implemented. Not yet: Spine; poses (`pose3.json`: no test model has one).

## A portrait group mod

The engine's own syntax already says which portraits leaders, rulers, species and pops get: `portrait_groups` in `gfx/portraits/portraits/*.txt`
(`human = { default = ...  game_setup = { ... }  species = { ... }  pop = { ... }  leader = { ... }  ruler = { ... } }`, each scope a list of
`add = { trigger = { ... } portraits = { ... } }`). A mod that registers its own portraits the usual way (a copy of a vanilla entry, so a
game without the plugin shows something ordinary) and overrides a group with them chooses *per scope* which of them leaders and which pops
get; the engine picks one per leader or pop, and the plugin binds a model to the key the engine reports. Nothing of this needs the plugin to
know about leaders: more portraits and a group that lists them is all it takes to have a different model per leader.
`scripts/make_human_mod.py` builds exactly that for the humans (ten new portraits, the `human` group overridden: leaders and rulers pick
among the first three of each gender, pops among the last two, species among all five).

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
