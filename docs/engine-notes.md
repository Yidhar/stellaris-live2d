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
| +0x28 / +0x2C | int: the size of the GUI in GUI units (1920 x 1080 at UI scale 1 in a 1080p window) | [G] |
| +0x350 / +0x354 | float: the mouse pointer, in GUI units measured from the top-left corner (equal to client pixels at UI scale 1) | [G] |

Not checked: a UI scale other than 1 (the pointer is assumed to be in GUI units there too).

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

## What the plugin hooks

| Function | RVA | Why |
|---|---|---|
| `CPortraitObject::UpdatePortrait` | 0xFB1280 | paint the Live2D frame over the render target after the engine rendered it; found by the string `...portraitobject.cpp:467` |
| `CPortraitObject::Render` | 0xFAD5B0 | learn where the GUI draws each portrait and get the `CGuiGraphics`; found by the string `Invalid alternate sprite configuration index [%i], must be in range [%i, %i)` |

## Layout constants (not found by a fingerprint)

`rt::CPortraitObject_pos/scale/mirrored`, `rt::CGuiGraphics_mouse_x/y/width/height`, `vt::C2dObject_GetSize` and the engine string
layout are constants in `tools/locate.py`, verified for the exe above by the disassembly of the constructors, `Render2dTree` and
`CPortraitObject::Render` and, for the rectangle and pointer, against screenshots and the log. `locate.py` warns when run against another
build. The plugin checks every value it reads for plausibility (finite, in range) and falls back to treating the portrait as being in the
middle of the game window.
