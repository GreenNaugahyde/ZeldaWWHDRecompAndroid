# The Legend of Zelda: The Wind Waker HD — native macOS port

A static recompilation of the Wii U version (USA) for Apple Silicon Macs. The game's PowerPC
code is translated to C ahead of time, the Cafe OS libraries the game uses are reimplemented
natively, and GX2 graphics are implemented directly on Metal or Vulkan (no Cemu runtime, no GPU
command emulation).

How it works and how it differs from running the game in Cemu: [docs/how-it-works.md](docs/how-it-works.md).

## What's new in this update

- **Vulkan renderer** (by OpenAI Codex), built into the same app next to Metal. Pick one in
  **Graphics › Renderer**; the choice is saved and used from the next start ("Restart Now"
  relaunches right away). Both share the same windows, menus, display modes, controls and mods.
  If Vulkan can't start (no Vulkan loader or MoltenVK installed), the game falls back to Metal and
  says why. Details: [docs/vulkan.md](docs/vulkan.md).
- **Full screen and GamePad screen modes** (Display menu): full screen for the TV window (⌘F),
  picture scaling (smooth, sharp, integer), and the GamePad screen as its own window, a
  picture-in-picture overlay, an automatic overlay that pops up when the GamePad picture changes,
  or off (⌘G shows/hides it).
- **Aspect ratio** (Graphics › Aspect ratio): 16:9 (original), match the window, 16:10, 21:9 or
  32:9. Wider screens see more to the sides (same vertical view); the HUD stays at the edges and
  menus stay centred.
- **Fixes**: misplaced Yes/No cursor in text boxes at 16:10, quitting with ⌘Q could hang, garbled
  characters in the window title. Community fixes from pull requests #1 and #2 (Miiverse manager
  throttling, shared shader-cache memory) are included.

## Earlier updates

- **60 fps.** Two modes in the Graphics menu:
  - **60 fps (key 6)**: frame interpolation. The game logic keeps its original 30 steps per second;
    every second frame is drawn halfway between two steps (camera, models, particles, sea, wave
    crests, grass and trees, cloth, weather, lighting). Input, sound and menus behave as at 30 fps.
  - **True 60 (key 7, experimental)**: Link and the follow camera run their logic at 60 steps per
    second (for the actions that have been converted and measured against the original); everything
    else runs at 30 and is interpolated.
- **Higher internal resolution** (1x / 1.5x / 2x / 3x, key R) and **edge smoothing** (FXAA, key 8).
- **Save states**: a Save States menu with 5 slots (Shift+F1–F5 save, F1–F5 load), kept across
  sessions in `~/Library/Application Support/wwhd/states/`.
- **Controls window** (Input › Controls…): a drawing of the Wii U GamePad or Pro Controller; click
  a button to remap it to a key or a controller input, live feedback of pressed buttons and stick
  positions, conflict warnings.
- **Optional gameplay mods** (Gameplay menu, all off by default): climb any wall, direct right-stick
  camera, mouse camera, first person on the mouse wheel, quick doors, fast scene changes.
- **Fixes**: shadow streaks, flicker after loading, doubled wave sounds at 60 fps, camera issues.
- **Tools**: function naming against the GameCube decompilation (`tools/decomp/`), a differential
  harness that verifies hand-written source against the recompiled original (`tools/verify/`), and
  the 60 fps conversion tools (`tools/true60/`). See "Optional: decompilation tools" below.

## Legal notice

This is an unofficial fan project. It is not affiliated with, endorsed or sponsored by Nintendo.
"The Legend of Zelda", "The Wind Waker", "Wii U" and related names are trademarks of their
respective owners and are used here only to describe what this software is compatible with.

This repository contains **no game code, no game assets and no keys**: no executable, no
recompiled or disassembled game code, no textures, models, audio, shaders, screenshots or other
material from the game, and no console encryption keys. It contains only the tools and the
runtime written for this project (plus the third-party code listed under Credits).

To use it you need your own, legally obtained copy of the game, dumped from your own Wii U disc
and console. Everything game-specific (the extracted files, the recompiled code in `build/gen/`,
shader caches) is generated locally on your machine from your dump, and must not be
redistributed. The `.gitignore` keeps all of it out of the repository.

## Requirements

- macOS on Apple Silicon
- Xcode command line tools (`xcode-select --install`)
- CMake 3.20 or newer
- Python 3 with `pycryptodome` (disc extraction): `pip3 install pycryptodome`
- optional: `capstone` (`pip3 install capstone`) for the disassembler helper `tools/ppcdis.py`
- optional, decompilation tools only: `ninja` and the requirements of the zeldaret/tww build
  (see below)

You also need, from your own console and disc:

- a disc image of The Wind Waker HD (USA) in `.wud` or `.wux` format;
- its disc key (16 bytes) in a `.key` file next to the image, with the same base name;
- the Wii U common key, either in a file `common.key` (16 raw bytes or 32 hex digits) next to
  the image or in the current directory, or in the `WIIU_COMMON_KEY` environment variable
  (32 hex digits).

None of these are included or will be provided.

## Building

For the Vulkan renderer also: `brew install vulkan-headers vulkan-loader molten-vk glslang` (the
build needs them; the app still runs with Metal on a Mac without them). `-DWWHD_RENDERER=METAL`
builds a Metal-only app without any Vulkan dependency.

```sh
# 1. extract the game into game/ (game.wux with game.key next to it, plus your common key)
python3 tools/wudextract.py game.wux extract game
#    -> game/code/cking.rpx, game/content/..., game/meta/...

# 2. recompile the game code to C (writes build/gen/; stays on your machine)
python3 tools/recomp/recomp.py game/code/cking.rpx build/gen

# 3. build
cmake -S . -B build/cmake && make -C build/cmake -j$(sysctl -n hw.ncpu) wwhd
```

The build fails with a clear message if `build/gen` has not been generated. The runtime checks at
startup that `game/code/cking.rpx` matches the recompiled code.

## Playing

```sh
./build/cmake/wwhd                 # options: --game DIR (default game), --save DIR (default save)
```

`--renderer=metal` or `--renderer=vulkan` (or `WWHD_RENDERER_RUNTIME=metal|vulkan`) overrides the
saved renderer choice for one start.

Two windows open: the TV and the GamePad screen (map, items, menus). Click and drag in the
GamePad window to use the touch screen. Saves go to `save/`.

### Controls

Default keyboard layout:

| Keyboard | Wii U GamePad |
|---|---|
| W A S D | left stick (move) |
| arrow keys | right stick (camera) |
| K or Space | A |
| J | B |
| L | X |
| I | Y |
| Q / E | L / R |
| Left Shift | ZL (target) |
| C | ZR |
| Enter / Tab | + / − |
| H | Home |
| 1 2 3 4 | D-pad up / down / left / right |
| X / V | left / right stick click |

**Input › Controls…** remaps everything on a drawing of the controller: click a button, stick
direction or stick click, then press a key or a controller button / stick direction (each input
has a key, an alternate key and a controller binding); Esc cancels, right-click clears. Pressed
buttons light up and the sticks show their deflection, so you can test the mapping; a key bound
twice is marked with a warning. Changes apply immediately, also while playing. A dead zone for
controller sticks and an option to invert the camera's up/down are at the bottom, with **Reset to
Defaults…**. The mapping is saved to `~/Library/Application Support/WWHD/controls.json`
(`WWHD_CONTROLS=<file>` uses another file); deleting it restores the defaults. The app's
single-key shortcuts (R, O, M, N, 6–9, P, F1–F5, F12) and Esc can't be bound.

The **Graphics** menu in the menu bar switches fixes and enhancements while playing (the TV
window title shows what is active and the current frame rate): 60 fps by frame interpolation
(**6**), true 60 fps (**7**, experimental), internal resolution 1x / 1.5x / 2x / 3x (**R**
cycles; the game renders at 1280x720, 2x renders at 2560x1440), edge smoothing (FXAA, **8**),
ambient-occlusion mode (**O** cycles), full-size occlusion depth (**M**), 16x anisotropic
filtering (**N**), the aspect ratio, the renderer (Metal or Vulkan), and a frame capture for debugging (**P** or fn+F12, written to `captures/`;
captures contain game imagery, so keep them to yourself).

The **Gameplay** menu has optional changes to how the game plays, all off by default: climb any
wall (with a stamina wheel; B or A lets go), a direct right-stick camera (no easing, adjustable
speed), a mouse camera (click the picture to capture the pointer, Esc releases it), first person
on the mouse wheel, quick doors and fast scene changes.

The **Save States** menu saves the whole running game to one of 5 slots and loads it back
(**Shift+F1–F5** save, **F1–F5** load); each slot shows its time and area. Slots are kept in
`~/Library/Application Support/wwhd/states/` (about 270 MB each) and survive restarts; a slot
made by an incompatible build is refused. Loading works once the game has reached gameplay.

Game controllers (Xbox, PlayStation, Switch Pro, MFi) work too; by default buttons map by
position (the bottom face button is the Wii U's B), and they can be remapped in the Controls window.
The **Input** menu switches whether keyboard and controllers act as the Wii U GamePad (default)
or as a Wii U Pro Controller (`WWHD_PRO_CONTROLLER=1` starts in that mode); with the Pro
Controller, the GamePad window keeps its screen and touch input.
When the game asks for text (e.g. your name), a macOS text field opens.

The **Display** menu: full screen for the TV window (**⌘F**, **⌃⌘F** or the green button; the
pointer hides after 2 s without movement), picture scaling (smooth, sharp, or integer scale) and
where the GamePad screen goes: a separate window (which can be put on another display, also in
full screen there), a picture-in-picture overlay in a corner of the TV picture (size, corner and
opacity selectable; click it to touch), an automatic overlay that appears for a few seconds when
the GamePad picture changes a lot (a page or menu switches; **⌘G** keeps it up), or off.
**⌘G** shows/hides the GamePad screen in any mode. Window positions, full screen, these choices
and the renderer are remembered in `~/Library/Application Support/wwhd/display.plist`
(delete it to reset).

## Notes

- Shaders are translated on first use and cached in `~/Library/Caches/wwhd/shaders.bin`; later
  runs replay that cache at startup.
- Useful environment variables: `WWHD_NO_AUDIO=1`, `WWHD_NO_GAMEPAD=1` (no second window),
  `WWHD_DRC_MODE=window|pip|auto|off`, `WWHD_ASPECT=16:9|window|16:10|21:9|32:9|<w:h>`,
  `WWHD_AUDIO_VOLUME=0..1`, `WWHD_SHADER_CACHE=<file>|0`, `WWHD_AO_MODE=0..2`, `WWHD_AO_HIRES=0|1`, `WWHD_ANISO=0|1`, `WWHD_RES_SCALE=1|1.5|2|3`,
  `WWHD_FXAA=0|1`, `WWHD_INTERP=1`, `WWHD_TRUE60=1` (start values for the Graphics menu);
  `WWHD_SHADOW_SCALE=n` gives the shadow maps their own resolution factor; `WWHD_STATE_DIR=<dir>`
  stores save states elsewhere.
- A game halt writes `captures/crash-<time>.log` with the guest call chain (useful for bug reports;
  it contains only addresses and function names).
- Debugging aids (frame/draw dumps, traces, scheduler statistics) are documented next to their
  code: grep for `WWHD_` in `runtime/src`.

## Optional: shader head start

The first time a shader is needed it is translated and compiled, which can cause short hitches.
A "head start" pre-translates shaders from the game's own shader archives so later sessions
start with them. It is built locally from your files and is never distributed; without it the
game simply translates shaders on first use.

Building it needs a state template (the GPU register states each shader was used with), which
is recorded from your own play: play for a while (the further you get, the more it covers),
then

```sh
python3 tools/shaderprep.py template ~/Library/Caches/wwhd/shaders.bin   # -> game/shadercache/template.bin
python3 tools/shaderprep.py build                                          # -> game/shadercache/headstart.bin
./build/cmake/wwhd --warm-shaders    # optional: compile everything once to fill the macOS shader cache
```

`--merge game/shadercache/template.bin` adds a later session to an existing template. The runtime
picks up `game/shadercache/headstart.bin` automatically (`WWHD_HEADSTART=<file>|0` overrides it).
See the comment at the top of `tools/shaderprep.py` for the file formats.

## Optional: decompilation tools

`tools/decomp/` names WWHD functions by matching them against the
[zeldaret/tww](https://github.com/zeldaret/tww) GameCube decompilation (CC0); the 60 fps features
are built on those names (`tools/recomp/hooks.txt`, `runtime/src/interp*.cpp`, `runtime/src/true60*.cpp`).
Findings are in `docs/decomp-notes.md`.

`tools/verify/` is a differential test harness for a functionally verified decompilation: it runs
hand-written C++ next to the recompiled original on generated and recorded inputs and compares
return values, memory effects and call sequences (see `tools/verify/README.md`). The verified
source itself is derived from the game and is **not** part of this repository.

```sh
git clone https://github.com/zeldaret/tww tww     # git-ignored reference checkout
python3 tools/decomp/match.py game/code/cking.rpx tww build/names.tsv
```

The assert, string and actor-profile stages only need the decompilation's sources. The
call-graph stage additionally needs the decompilation built (`tww/build/GZLE01`), which in turn
needs `ninja` and your own GameCube disc image of The Wind Waker (USA) as described in the tww
README. `build/names.tsv` is derived from the game and stays on your machine.

## Status

The opening of the game (title, file select, intro, Outset Island) is tested and matches Cemu
side by side, at a steady 30 fps (the console's frame rate) and at 60 fps with interpolation;
true 60 is experimental. Known gaps: geometry shaders and rectangle
primitives are not implemented yet (not encountered so far), shadow edges are harder than on
the console, startup sometimes sits on a black screen for up to a minute before the logo
(a timing-dependent wait during audio initialization, under investigation), and later parts of
the game are untested.

## License

The code of this project is licensed under the Mozilla Public License 2.0 (see `LICENSE`).
Vendored third-party code keeps its own license: Cemu (MPL-2.0), metal-cpp (Apache-2.0) and {fmt} (MIT); see Credits. The game itself is Nintendo's property and is not included.

## Credits

GPU address library, shader decompiler and a few reference structures are vendored from
[Cemu](https://github.com/cemu-project/Cemu) (MPL-2.0, see `runtime/third_party/cemu/LICENSE.txt`);
`tools/wudextract.py`, `runtime/src/espresso_fp.c` and parts of the OS layer are ported from or
follow Cemu as noted in those files (in the Vulkan renderer: the vertex-format table and the
shader parser glue). Also vendored: [metal-cpp](https://developer.apple.com/metal/cpp/)
(Apache-2.0, `runtime/third_party/metal-cpp/LICENSE.txt`) and [{fmt}](https://github.com/fmtlib/fmt)
(MIT, `runtime/third_party/fmt/LICENSE`).
