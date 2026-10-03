# stellaris-live2d

[English](README.md) | [简体中文](README.zh-CN.md)

Live2D portraits for **Stellaris 4.5.1** (Windows x64, the `-dx11` build): a DLL that is loaded into `stellaris.exe` and
draws a Live2D model into the game's own portrait frames. The game keeps doing the layout, the masks and the shaders, and
only draws portraits that are on screen.

**Status: milestone 2.** A `moc3` model is loaded, animated (idle loop, motions, physics) and drawn into the portraits of
the council and leader screens, in the real game. What is still missing: choosing a model per portrait (for now one model
is drawn into every portrait of the chosen size), crops per screen, state variants, and a control interface. See the
[plan](#plan).

## How it works

Stellaris portraits are skeletal 2D figures rendered into a render-target texture, which the GUI shows through a masked
sprite. Once per frame, for each portrait that is visible, the engine calls `CPortraitObject::UpdatePortrait`. The plugin
hooks that function; after the original returns it replaces the portrait's render target with a frame of the model.

- **The Live2D side** (`src/cubism_core.cpp`, `live2d_model.cpp`, `live2d_motion.cpp`, `live2d_physics.cpp`,
  `live2d_character.cpp`, `live2d_renderer.cpp`) knows nothing about the game and builds into a library that the DLL and
  an offscreen viewer share.
  - **Cubism Core** is loaded at run time from a `Live2DCubismCore.dll` you point the plugin at; its C API is declared in
    `include/cubism_core.hpp`. Nothing from Live2D is in this repository.
  - **Motions** are `motion3.json` curves (linear, bezier, stepped, inverse-stepped) applied the way Cubism does it: the
    last frame's parameters are restored, every active motion is blended in by its fade-in/out weight, the result is saved,
    then effects that must not accumulate (physics) go on top. The `Idle` group loops; other groups play once.
  - **Physics** (`physics3.json`) is an independent implementation written from the meaning of the file's fields. It has
    not been compared with Live2D's own runtime, so hair and clothes move in the same way but not necessarily identically.
  - **The renderer** draws with Direct3D 11: premultiplied alpha, additive and multiplicative drawables, clipping masks, the
    model's multiply and screen colours, mip-mapped textures.
- **Drawing into the game.** The renderer records into a *deferred context* on the game's own device and the commands are
  run with `ExecuteCommandList(…, RestoreContextState = TRUE)`, so the engine's pipeline state is put back exactly as it
  was. The model is advanced and drawn at most `fps` times a second into one texture per portrait size, which is then
  copied into each portrait's render target.
- **Finding the code.** `tools/locate.py` finds `UpdatePortrait`, the object fields it needs and the engine's array of all
  portraits from fingerprints taken from the engine's own code, writes `sdk/stellaris_sdk.hpp`, and fails instead of
  guessing if a fingerprint is not unique. No address is typed in by hand. Run it after every game patch.
- **Finding the texture.** The engine wraps its D3D11 textures in a `TextureGFX` object. The plugin scans it for a COM
  pointer that is a D3D11 texture of exactly the portrait's size, bound as a render target, remembers the offset and
  re-validates it on every use.
- **Cleanup.** Turning the effect off or unloading the DLL flags every portrait in the engine's portrait array for
  re-rendering, so none keeps a stale picture. The DLL unloads itself: hooks are removed, in-flight calls drained, GPU
  objects and the Cubism Core released, then it frees itself.
- **Multiplayer.** The plugin only changes what this client draws and never touches the simulation, so it cannot put a game
  out of sync. A player without the plugin sees the normal portrait.

## Install and use

You need a Cubism Core library and a model. Neither is included.

1. **Cubism Core.** Either Live2D's official `Live2DCubismCore.dll` (from the Cubism SDK for Native on live2d.com, under
   Live2D's terms), or a compatible one such as [Purism Core](https://github.com/SakuraMotion/PurismCore) (MIT). The plugin
   uses the v5 API (`csmGetDrawableRenderOrders` and the colour functions); a library with only the newer API is not
   supported yet.
2. **A model**: a folder with `model3.json`, the `moc3`, PNG or JPEG textures, and optionally `physics3.json` and motions.
   Models carry their authors' licenses.
3. Build the plugin (below), start the game, and run `python scripts\l2dctl.py load` (`unload`, `reload` and `status` work
   too). The injection lasts for that game session only.
4. Edit `stellaris_live2d.ini` next to `stellaris.exe` (created on first run, re-read every 2 seconds), open a screen with
   portraits (the council, the leaders list), and look at `stellaris_live2d.log` if nothing shows.

| Key | Default | Meaning |
|---|---|---|
| `live2d` | `0` | draw the model into the render target of every visible portrait |
| `core_dll` | | path of `Live2DCubismCore.dll` |
| `model` | | path of the model's `model3.json` (one model, shown with the `view_*` values) |
| `models` | | several models separated by `;`: `path`, `path\|x,y,h` (that view) or `path\|auto` / `path\|auto:0.5` (a view worked out from the model: from the head down, that fraction of the figure's height, default `0.46`). Each portrait gets one of them, handed out in turn the first time the portrait is seen |
| `only_width`, `only_height` | `0` | only portraits whose render target has exactly this size (`0` = every size); the game's character portraits are 575×380 |
| `view_x`, `view_y`, `view_h` | `0.44`, `0.19`, `0.26` | the part of the model canvas shown: centre from the left, centre from the top, and height, as fractions of the canvas |
| `fps` | `30` | how often the model is advanced and redrawn |
| `physics` | `1` | secondary motion from the model's `physics3.json` |
| `test_pattern` | `0` | paint a test pattern instead (a check that the hook works) |

### The offscreen viewer

`l2d_view.exe` draws one frame to a PNG without the game, to check a model and choose `view_*`:

```
build\Release\l2d_view.exe --core Live2DCubismCore.dll --model model3.json --out out.png ^
    --size 575x380 --view 0.44,0.19,0.26 [--motion touch_1 --time 2.0] [--no-physics] [--param ParamAngleX=20]
```

`--view auto` (or `auto:0.5`) prints the view the plugin would work out for the model. `python scripts\capture_game.py out.png`
saves the game window's client area (the window must be visible and uncovered).

### Cost of many models at once

`l2d_bench.exe` draws N different models per step the way the plugin does (advance, record on a deferred context, execute)
and reports CPU time per stage and GPU time from timestamp queries:

```
build\Release\l2d_bench.exe --core Live2DCubismCore.dll --size 575x380 --frames 300 --counts 1,6,12,24 ^
    --model a\model3.json^|auto --model b\model3.json^|auto [--no-physics]
```

`python scripts\ingame_multi_bench.py` runs several model sets in the real game (a screen with portraits open, and
`stellaris_bench.dll` from the stellaris-perf repo loaded for the frame counter) and compares frames per second with the
plugin off.

## Building

Visual Studio 2022 (MSVC, x64) and CMake 3.20+. MinHook is fetched by CMake; `stb_image` and `nlohmann/json` are single
headers in `third_party/`.

```
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release        # build\Release\stellaris_live2d.dll and l2d_view.exe
```

The DLL only works with the `stellaris.exe` its SDK was located in (checked at load time; with any other build it logs the
mismatch and installs nothing). After a game patch: `pip install pefile capstone`, `python tools/locate.py`, rebuild.

## Plan

1. **Done:** hook, texture write, restore, unload.
2. **Done:** Core loading, model, motions, physics, renderer, drawing into the game.
3. **Done:** several models at once, one per portrait, handed out in turn.
4. Portrait-group mods: a mod registers its portraits the usual way and says in an extra file which of them are Live2D (or
   Spine) models and how they react (mouse follow, click, drag, zoom); the DLL provides the runtime. See
   `docs/portrait-mod-design.md`.
5. Crop per screen, state variants (for example wounded).
6. Release builds by CI.

## Licensing

The code here is MIT. It does not include or download any Live2D code or any model:

- **Cubism Core** is Live2D's proprietary library and is not redistributed here (see step 1 above). Check the terms of
  whichever you use; Live2D's license has special rules for applications that let third parties add content.
- **Models** are the work of their authors and carry their own licenses.
- `third_party/` holds `stb_image` (public domain) and `nlohmann/json` (MIT), see `third_party/README.md`.

## License

MIT, see [LICENSE](LICENSE).
