# stellaris-live2d

[English](README.md) | [简体中文](README.zh-CN.md)

Live2D portraits for **Stellaris 4.5.1** (Windows x64, the `-dx11` build): a DLL that is loaded into `stellaris.exe` and
puts a rendered frame into the game's own portrait frames. The game keeps doing the layout, the masks and the holographic
shader, and only draws portraits that are on screen.

**Status: milestone 1, a proof of concept.** The hook point and the texture write work: a test pattern replaces the portraits
in the council screen, inside the game's own masks. There is no Live2D rendering yet.

## How it works

Stellaris portraits are skeletal 2D figures rendered into a render-target texture, which the GUI shows through a masked
sprite. Once per frame, for each portrait that is visible, the engine calls `CPortraitObject::UpdatePortrait`. The plugin
hooks that function; after the original returns it overwrites the portrait's render target.

- **Finding the code.** `tools/locate.py` finds `UpdatePortrait` and the object fields it needs from fingerprints taken from the
  engine's own code, writes `sdk/stellaris_sdk.hpp`, and fails instead of guessing if a fingerprint is not unique. No address
  is typed in by hand. Run it after every game patch.
- **Finding the texture.** The engine wraps its D3D11 textures in a `TextureGFX` object. The plugin scans that object for a
  COM pointer that is a D3D11 texture of exactly the portrait's size, bound as a render target, remembers the offset and
  re-validates it on every use. It then writes with `UpdateSubresource` on the texture's own device context.
- **Visibility.** Only portraits that are on screen are updated: with the council screen closed the hook sees no calls at all.
- **Cleanup.** Turning the effect off or unloading the DLL flags every portrait in the engine's portrait array for
  re-rendering, so none keeps a stale picture. The DLL unloads itself: hooks are removed, in-flight calls drained, then it
  frees itself.
- **Multiplayer.** The plugin only changes what this client draws and never touches the simulation, so it cannot put a game
  out of sync. A player without the plugin sees the normal portrait.

## What the proof of concept shows

With `test_pattern=1`, every visible portrait's render target gets a pattern (a colour per portrait, a grid, a moving bar, a red
marker top left, a blue one bottom right). In the game the six councillor frames showed the patterns with the right
orientation, under the game's own mask and fade; turning it off, or unloading while it was on, brought the original portraits
back. The game's portrait render targets are 575×380, format `B8G8R8A8_UNORM`, bound as render target and shader resource.

## Install and use

Build (see below), start the game, and run

```
python scripts\l2dctl.py load        # inject; unload / reload / status work too
```

`stellaris_live2d.ini` is created next to `stellaris.exe` and re-read every 2 seconds; `stellaris_live2d.log` shows the hook
and counters. The injection lasts for that game session only.

| Key | Default | Meaning |
|---|---|---|
| `test_pattern` | `0` | paint the test pattern into the render target of every visible portrait |
| `only_width`, `only_height` | `0` | only paint render targets of exactly this size (`0` = every size) |

`python scripts\capture_game.py out.png` saves the game window's client area (the window must be visible and uncovered).

## Building

Visual Studio 2022 (MSVC, x64) and CMake 3.20+. MinHook is fetched by CMake.

```
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release        # build\Release\stellaris_live2d.dll
```

The DLL only works with the `stellaris.exe` its SDK was located in (checked at load time; with any other build it logs the
mismatch and installs nothing). After a game patch: `pip install pefile capstone`, `python tools/locate.py`, rebuild.

## Plan

1. **Done:** hook, texture write, restore, unload.
2. Load a `moc3` model through the Cubism Core C API (loaded at run time from `Live2DCubismCore.dll`), play its motions and
   physics, draw it on the GPU with its own D3D11 device, and hand the frame to the hook.
3. Bind portraits to models by name, crop the full-body canvas to a portrait, state variants (e.g. wounded).
4. A small control interface for parameters, motions and expressions.

## Licensing

The code here is MIT. It does not include or download any Live2D code or any model:

- **Cubism Core** is Live2D's proprietary library and is not redistributed here. The plan is to load `Live2DCubismCore.dll`
  at run time, from either Live2D's official SDK (free for individuals and small enterprises under Live2D's terms, which
  you accept yourself) or a compatible replacement such as [Purism Core](https://github.com/SakuraMotion/PurismCore) (MIT).
  Check the terms of whichever you use; Live2D's license has special rules for applications that let third parties add content.
- **Models** are the work of their authors and carry their own licenses.

## License

MIT, see [LICENSE](LICENSE).
