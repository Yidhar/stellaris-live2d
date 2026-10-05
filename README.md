# stellaris-live2d

[English](README.md) | [简体中文](README.zh-CN.md)

Live2D portraits for **Stellaris 4.5.1** (Windows x64, the `-dx11` build): a DLL that is loaded into `stellaris.exe` and
draws a Live2D model into the game's own portrait frames. The game keeps doing the layout, the masks and the shaders, and
only draws portraits that are on screen.

**Status.** Live2D `moc3` models are drawn into the portraits of the game (leaders, pops, species, the council, the planet
view), animated (motions, physics, expressions, blinking, breathing, lip sync) and interactive (mouse follow, click on the
picture or on a hit area of the model, hover, appear, idle, greeting sound), with voice lines that follow the game's volume.
A mod declares all of it in the portrait script syntax, and once `scripts\deploy.py` has installed the loader the game loads
the plugin by itself. Not done: Spine, `pose3.json`, a test with a UI scale other than 1, a test with Live2D's official Core
(only Purism Core was used), multiplayer. Built for and tested on Stellaris 4.5.1 only. See the [plan](#plan).

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
   Live2D's terms), or a compatible reimplementation: the plugin was tested with [Purism Core](https://github.com/SakuraMotion/PurismCore), which its author
   publishes under the MIT license. Whether using such a reimplementation is allowed under Live2D's terms has not been checked here, so
   the official library is the safe choice. The plugin
   uses the v5 API (`csmGetDrawableRenderOrders` and the colour functions); a library with only the newer API is not
   supported yet.
2. **A model**: a folder with `model3.json`, the `moc3`, textures (PNG, JPEG, or DDS in DXT1/DXT3/DXT5, the format of the game's
   own textures: a quarter of the video memory, with the mip chain stored in the file) and optionally `physics3.json` and
   motions. `l2d_pack` converts a model's PNG textures to DXT5. Models carry their authors' licenses.
3. Unpack a zip from the [Releases](https://github.com/Yidhar/stellaris-live2d/releases) page (or build the plugin, below), and run
   `python scripts\deploy.py` from that folder: it copies `stellaris_live2d.dll` and the loader `d3dx9_43.dll` next to
   `stellaris.exe`, and from then on the game loads the plugin by itself a few seconds after it starts (`deploy.py --remove` takes
   both away; a file `stellaris_live2d.disabled` next to the exe stops the loader for a session). The loader is a stand-in for a system
   DLL that only the game's exe imports: the folder of the exe is searched first, so the game picks it up, it passes every call on to the
   real `d3dx9_43.dll` and loads the plugin (it does nothing in any other program). Without it, `python scripts\l2dctl.py load` injects
   the plugin into a running game for that session (`unload`, `reload` and `status` work too).
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
| `model_cache_mb` | `512` | memory the loaded models may take: within it all models are loaded in advance (in the background), beyond it on demand, dropping the one unused longest |
| `supersample` | `2` | `2` draws the models at twice the size and averages down (crisper fine lines, costs little); `1` is off |
| `fps` | `30` | how often the model is advanced and redrawn |
| `physics` | `1` | secondary motion from the model's `physics3.json` |
| `interactions` | `1` | the events mods declare (mouse follow, click, hover, ...); `0` = the models only play their own motions |
| `audio`, `volume` | `1`, `0.8` | the voice lines of events on or off, and the plugin's own volume (multiplied by the game's) |
| `volume_channel` | `voice` | which slider of the game's sound settings the voice follows: `voice`, `effects` or `none` |
| `mute_in_background` | `0` | `1` = silent while the game window is not in front |
| `extra_mod_dirs` | | mod root folders read as if they were enabled (development) |
| `test_pattern` | `0` | paint a test pattern instead (a check that the hook works) |

### Portrait mods

The intended way to use the plugin is a mod that carries the models and says which portraits they replace: the mod registers
the portrait keys it wants drawn by the plugin, in the script syntax the game uses for portraits, with a few extra keys
(`live2d = yes`, `spine = yes`, `live2d_model`, `live2d_view`, `live2d_scale`, `live2d_unmirror`, and `live2d_actions` for
what happens on mouse follow, click, hover, appear, idle and the game's greeting sound). The plugin reads them from the enabled
mods' `gfx/portraits/live2d/*.txt` (a folder the engine ignores) or from their `gfx/portraits/portraits/*.txt`, and binds each
model to the portrait key the engine reports for a portrait. A game without the plugin keeps drawing the normal portraits.
Which portraits leaders, rulers, species and pops get is the game's own `portrait_groups` syntax: a mod lists its keys in a group
and gets a different model per leader or pop, with no extra syntax. The motion groups, expressions and hit areas come from the
model's own `model3.json`; the mod only says which to play on which event.

- Syntax, events, views, loading and the group rules: [docs/portrait-mod-design.md](docs/portrait-mod-design.md).
- **A demo mod** lives in its own repository, [stellaris-live2d-demo-mod](https://github.com/Yidhar/stellaris-live2d-demo-mod): the `human` portrait group
  replaced with ten Live2D portraits (a `set` for each scope, then `add`s), the vanilla keys bound too, and every event, voice line,
  view and scale option used once. It ships no models, since the ones it was made with are other people's art: bring your own.
- The `login` motion of many models is a stage entrance (black curtain, camera zoom); `live2d_ignore_parameters` skips the curves of the
  parameters you name, and `python tools/motion_diff.py <model3.json>` finds them (see the design doc, *Stage effects in motions*).
- Two rules worth knowing: a portrait group defined in several files is *merged*, so the first entry of each scope has to be a
  `set` to drop what vanilla listed; and a portrait a script or the empire designer named outright (the ruler's) is not drawn from
  a group, so bind the vanilla key as well.
- `python scripts\make_human_mod.py --enable` builds the test mod from local models (`--export-demo <folder>` writes the demo
  repository's text files); `python scripts\load_save.py <save> --folder <folder>` restarts the game on a save.
- `interactions=0` in the ini turns the interactions off. Dragging and wheel zoom are not offered; `live2d_scale` is a fixed
  magnification of the framed part.

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

## Building and releases

CI (`.github/workflows/build-release.yml`) builds every push and pull request on a Windows runner, runs the offline tests and keeps the packaged
zip as a workflow artifact. Pushing a tag `v*` publishes a GitHub Release with the zip (the plugin, the loader, `scripts/`, the command line tools,
the docs and a `GAME_BUILD.txt` naming the game build) and its SHA-256 file; a tag with a hyphen (`v0.2.0-rc1`) is a pre-release.
`pwsh scripts/package_release.ps1` makes the same zip from a local Release build.

Visual Studio 2022 (MSVC, x64) and CMake 3.20+. MinHook is fetched by CMake; `stb_image` and `nlohmann/json` are single
headers in `third_party/`.

```
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release        # build\Release\stellaris_live2d.dll and l2d_view.exe
```

The DLL only works with the `stellaris.exe` its SDK was located in (checked at load time; with any other build it logs the
mismatch and installs nothing). After a game patch: `pip install pefile capstone`, `python tools/locate.py`, rebuild, then check:

- `python tools/validate.py`: the locator still finds what the header says, every value that was verified by hand or in the game is
  unchanged, and the layout invariants hold; it says when the constants no fingerprint finds (the GUI object layout) were verified for
  another build. This is what the main repo's SDK dumper does with its `validate.py`.
- `python tools/live_verify.py` with the game running and a portrait on screen: the plugin checks the live objects itself (portrait key and
  kind, the rectangle, the window and GUI sizes, the game's volumes, and the pointer once the game window is in front) and writes the result to
  its log; this prints it.
- `build\Release\l2d_tests.exe` (or `ctest -C Release` in `build`): offline checks of the script reader, the portrait registry, DDS reading,
  mip chains and the framing maths.

## Known limits

- **The loader looks like malware to a scanner.** `d3dx9_43.dll` next to `stellaris.exe` is a stand-in for a system DLL that passes every
  call on and loads the plugin: the same technique DLL hijacking uses, and antivirus software may flag it. It is built from `loader/` in
  this repository and does nothing in any program but `stellaris.exe`. Verifying the game's files in Steam removes it; so does
  `python scripts\deploy.py --remove`. `l2dctl.py load` injects without it.
- **One game build.** Addresses are located in the installed `stellaris.exe` by `tools/locate.py`; after a game patch the DLL logs the
  mismatch and installs nothing (it cannot crash the game over it). Rerun the locator and `tools/validate.py` and rebuild.
- **Tested on one machine** (Windows 11, one AMD GPU, 1920x1080, UI scale 1), with Direct3D 11 (`-dx11`) only. Other GPUs, other
  resolutions and UI scales are untested; multiplayer is untested (the plugin only changes what this client draws).
- Not implemented: Spine, `pose3.json`, dragging and wheel zoom. The physics is an independent implementation, not compared with Live2D's.

## Plan

1. **Done:** hook, texture write, restore, unload; Core loading, model, motions, physics, renderer; several models at once, DXT5
   textures, portrait-group mods.
2. **Done:** mouse follow, click, click on hit areas, hover, appear, idle, greeting; expressions, blinking, breathing and lip sync;
   voice lines that follow the game's volume; views per portrait kind and size; supersampling; background loading within a memory budget.
3. **Done:** the loader that makes the game load the plugin by itself; SDK checks (`tools/validate.py`, the plugin's self-test) and
   offline tests; CI that builds, tests and packages releases.
4. A checker for mods (what is wrong with a portrait file before the game is started), Spine, `pose3.json`.

## Licensing

The code here is MIT. It does not include or download any Live2D code or any model:

- **Cubism Core** is Live2D's proprietary library and is not redistributed here (see step 1 above). Check the terms of
  whichever you use; Live2D's license has special rules for applications that let third parties add content.
- **Models** are the work of their authors and carry their own licenses.
- `third_party/` holds single-header libraries (miniaudio, the stb libraries, `nlohmann/json`), see `third_party/README.md`;
  MinHook (BSD-2-Clause) is fetched by CMake.

## License

MIT, see [LICENSE](LICENSE).
