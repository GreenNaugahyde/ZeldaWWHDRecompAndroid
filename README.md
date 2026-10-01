# The Legend of Zelda: The Wind Waker HD — native macOS port

A static recompilation of the Wii U version (USA) for Apple Silicon Macs. The game's PowerPC
code is translated to C ahead of time, the Cafe OS libraries the game uses are reimplemented
natively, and GX2 graphics are implemented directly on Metal (no Cemu runtime, no GPU command
emulation).

How it works and how it differs from running the game in Cemu: [docs/how-it-works.md](docs/how-it-works.md).

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

Two windows open: the TV and the GamePad screen (map, items, menus). Click and drag in the
GamePad window to use the touch screen. Saves go to `save/`.

### Controls

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
| 1 2 3 4 | D-pad up / down / left / right |
| X / V | left / right stick click |

The **Graphics** menu in the menu bar switches fixes and enhancements while playing (the TV
window title shows what is active): ambient-occlusion mode (**O** cycles), full-size occlusion
depth (**M**), 16x anisotropic filtering (**N**), and a frame capture for debugging (**P** or
fn+F12, written to `captures/`; captures contain game imagery, so keep them to yourself).

Game controllers (Xbox, PlayStation, Switch Pro, MFi) work too; buttons map by position.
The **Input** menu switches whether keyboard and controllers act as the Wii U GamePad (default)
or as a Wii U Pro Controller (`WWHD_PRO_CONTROLLER=1` starts in that mode); with the Pro
Controller, the GamePad window keeps its screen and touch input.
When the game asks for text (e.g. your name), a macOS text field opens.

## Notes

- Shaders are translated on first use and cached in `~/Library/Caches/wwhd/shaders.bin`; later
  runs replay that cache at startup.
- Useful environment variables: `WWHD_NO_AUDIO=1`, `WWHD_NO_GAMEPAD=1` (no second window),
  `WWHD_AUDIO_VOLUME=0..1`, `WWHD_SHADER_CACHE=<file>|0`, `WWHD_AO_MODE=0..2`, `WWHD_AO_HIRES=0|1`, `WWHD_ANISO=0|1` (start values for the Graphics menu).
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

## Optional: decompilation tools (`decomp` branch)

The `decomp` branch adds `tools/decomp/`, which names WWHD functions by matching them against
the [zeldaret/tww](https://github.com/zeldaret/tww) GameCube decompilation (CC0), and a frame
interpolation prototype built on those names (`tools/recomp/hooks.txt`, `runtime/src/interp.cpp`).
Findings are in `docs/decomp-notes.md`.

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
side by side, at a steady 30 fps (the console's frame rate). Known gaps: geometry shaders and rectangle
primitives are not implemented yet (not encountered so far), shadow edges are harder than on
the console, startup sometimes sits on a black screen for up to a minute before the logo
(a timing-dependent wait during audio initialization, under investigation), and later parts of
the game are untested.

## License

The code of this project is licensed under the Mozilla Public License 2.0 (see `LICENSE`).
Vendored third-party code keeps its own license: Cemu (MPL-2.0), metal-cpp (Apache-2.0) and
{fmt} (MIT); see Credits. The game itself is Nintendo's property and is not included.

## Credits

GPU address library, shader decompiler and a few reference structures are vendored from
[Cemu](https://github.com/cemu-project/Cemu) (MPL-2.0, see `runtime/third_party/cemu/LICENSE.txt`);
`tools/wudextract.py`, `runtime/src/espresso_fp.c` and parts of the OS layer are ported from or
follow Cemu as noted in those files. Also vendored: [metal-cpp](https://developer.apple.com/metal/cpp/)
(Apache-2.0, `runtime/third_party/metal-cpp/LICENSE.txt`) and [{fmt}](https://github.com/fmtlib/fmt)
(MIT, `runtime/third_party/fmt/LICENSE`).
