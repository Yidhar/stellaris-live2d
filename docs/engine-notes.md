# Engine notes (Stellaris 4.5.1, Windows x64)

What the plugin relies on inside `stellaris.exe` and how each fact was established. Addresses are RVAs of the exe with
timestamp `0x6ab5181d`; `tools/locate.py` finds the functions and the offsets it can by fingerprint and writes
`sdk/stellaris_sdk.hpp`; the rest are layout constants listed under "Layout constants". Tags: **[V]** verified in the Windows
disassembly, **[L]** read in the Linux decompile only, **[I]** inferred, **[G]** checked in the running game.

## One frame

`UpdatePortraits` (0xFB4320) calls `UpdatePortrait` (0xFB1280) for each visible portrait; then `UpdateSprites`; then the GUI draw
(`CGuiGraphics::Render2dObjectsToScreen` -> `Render2dTree` -> `RenderGfx2dList` -> virtual `CPortraitObject::Render`, 0xFAD5B0)
draws the portrait's render target. [V] So the plugin's paint (after `UpdatePortrait`) shows in the same frame's GUI draw, and the
object's GUI fields (position, mirror flag) are from the previous frame's draw.

## CPortraitObject

A portrait object is a GUI sprite: `CPortraitObject : CSprite : C2dObject : CGraphicalObject`, `sizeof` 0xA68. [V]

| Offset | What | |
|---|---|---|
| +0x68 | float x, y: the lower-left corner in GUI units around the middle of the screen, y up | [V][G] |
| +0xC4 | float scale | [V] |
| +0xC8 | byte: 1 = the GUI draws the portrait flipped left to right (the council does for some slots) | [V][G] |
| +0x528 / +0x52A | render target width / height (u16) | [V] |
| +0x52C | byte: set by the GUI draw each time it draws the portrait, cleared at the end of `UpdatePortrait`; the engine re-renders only while it is set | [V] |
| +0x530 | `TextureGFX*` of the render target (holds the `ID3D11Texture2D*` at +0x8) | [V][G] |
| +0x818 | engine string: the key of the `portraits = {}` entry this object shows (`human_female_01`), empty for planets | [V][G] |
| +0x878 | engine string: the selector or group name given to the setter (`human`) | [V] |
| +0x248 / +0x250 | scope type (u64: 0x100 leader, 0x800 species, 0x20 pop group) and id (u32) | [V] |
| +0xA48 | kind (0 character, 1 character_large, 2 room, 3 empty_room, 4 character_without_room, 5 planet) | [V] |

Engine string: 0x30 bytes; characters inline from +0x10 while the capacity (+0x28) is below 16, else a pointer at +0x10; the length
is at +0x20. [V]

The virtual `GetSize(this, int out[2])` (vtable slot 54) gives the width and height the GUI draws the object at (sprite type size times
scale). [V]

## CGuiGraphics

`CPortraitObject::Render` gets it as its second argument. `UpdatePortrait`'s second argument is a different object (a CGraphics). [G]

| Offset | What | |
|---|---|---|
| +0x28 / +0x2C | int: the size of the window in pixels (1920 x 1080 here) | [G][V] |
| +0x30 / +0x34 | int: the size of the GUI in GUI units, the pixels divided by the UI scale (equal to the above at UI scale 1) | [V] |
| +0x8C / +0x90 | float: the UI scale (`gui_scale`, 0.5 to 1.5 at 1080p, fixed for the session) and the safe ratio (fullscreen and borderless only) | [V] |
| +0x7C / +0x80, +0x84 / +0x88 | GUI units per pixel (about 1/scale) and pixels per GUI unit (about the scale) | [V] |
| +0x2F4 / +0x2F0, +0x300 / +0x304 | the safe viewport's width and height in pixels, and its offsets from the window's corner | [V] |
| +0x350 / +0x354 | float: the mouse pointer, in GUI units from the top-left corner (an integer-valued float: `(client pixel - viewport offset) / viewport size * GUI size`, rounded) | [G][V] |

The world space of `CPortraitObject +0x68` and of the `Render` matrix has its origin in the middle of the *window in pixels* but its units
are GUI units, y up (`ortho(-W/2, GUIw-W/2, H/2-GUIh, H/2)`): GUI x = world x + W/2 with W the pixel width (`+0x28`), not `GUIw/2`.
GUI units to client pixels: multiply by `W/GUIw` (the viewport offsets are zero unless the safe ratio is below 1). The UI scale and the
safe ratio were not changed in the running game; the formulas are from the disassembly of `CGuiGraphics::Init` (0x1C06800) and
`CGui::HandelInput` (0x1C0A340).

## Portrait definitions

- `CPortraitDatabase::ReadPortraitsFile` (0xEF5370) reads `gfx/portraits/portraits/*.txt`; `Init` (0xEF7040) drives it and builds one
  selector per key and one per `portrait_groups` entry. Duplicate keys: the last definition wins (logged).
- Accepted keys inside an entry: `textureFile`, `spriteType`, `entity`, `custom_attachment_label`, `clothes_selector`,
  `attachment_selector`, `character_textures`, `greeting_sound`, the `custom_*close_up_*` offsets and scales, `portrait_evolution`.
- An unknown key goes to `CReader::ReportUnexpected`: one `Error: ... Unexpected token` line in `error.log`, and the value (a whole
  `{ ... }` block included) is skipped; the entry still loads. An entry is dropped only without a layer and an entity, or when the
  entity does not exist.
- The definition table (key -> `SPortraitCharacterLayer*`) is at database + 0x1CE8; the global `CPortraitDatabase*` is at RVA
  0x3157538. Not used by the plugin: the object already holds the key.

## Sound

The engine has its own mixer (an SDL audio callback), not FMOD. The categories are `Weapon`, `Effects`, `Voice`, `Ambient` and `TTS`.
`CGameApplication::UpdateAudioVolume` (0x1BA0A0, found as the only function that names `Effects`, `Ambient`, `Voice` and `TTS`) copies the
sliders of the settings object into the mixer every frame: `master_eff = master/100 * dev_master/100`, a category's volume is its slider /
100. The settings object (`sdk::glob::CSettings`) holds them as floats 0..100: master, `dev_master_volume` (hidden, 75 by default), music,
`sound_fx_volume` (the `Effects` category), ambient, `voice_volume` (advisor and event speech), `tts_volume`; `locate.py` reads the offsets
from `UpdateAudioVolume`'s code. The settings file is written only on Apply, so the plugin reads the object. The game does not mute when the
window loses focus. [V]

## Greeting sounds

`CPortraitObject::GetGreetingSoundEffect` (found as the function that logs `Missing sound effect: %s`, works on the portrait key and whose
callers hand its result on at once) returns the sound of the portrait's `greeting_sound` (category `Effects`), or null; three callers play it:
the diplomacy window opening, an incoming diplomatic action, the species preview. A null result is handled by them (the original returns it
when the sound is missing), so the plugin returns null to replace the sound. It runs on the game's main thread. [V]

## What the plugin hooks

| Function | RVA | Why |
|---|---|---|
| `CPortraitObject::UpdatePortrait` | 0xFB1280 | paint the Live2D frame over the render target after the engine rendered it; found by the string `...portraitobject.cpp:467` |
| `CPortraitObject::Render` | 0xFAD5B0 | learn where the GUI draws each portrait and get the `CGuiGraphics`; found by the string `Invalid alternate sprite configuration index [%i], must be in range [%i, %i)` |
| `CPortraitObject::GetGreetingSoundEffect` | 0xFB3CE0 | the `greeting` event, and replacing the game's greeting sound; found as described under Greeting sounds |

## Layout constants (not found by a fingerprint)

`rt::CPortraitObject_pos/scale/mirrored`, `rt::CGuiGraphics_mouse_x/y/width/height`, `vt::C2dObject_GetSize` and the engine string
layout are constants in `tools/locate.py`, verified for the exe above by the disassembly of the constructors, `Render2dTree` and
`CPortraitObject::Render` and, for the rectangle and pointer, against screenshots and the log. `locate.py` warns when run against another
build. The plugin checks every value it reads for plausibility (finite, in range) and falls back to treating the portrait as being in the
middle of the game window.
