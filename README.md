# Wind Waker HD Recompiled — Android Enhanced

**Better touch controls, graphics and performance** for the Android port of
[ZeldaWWHDRecomp](https://github.com/ZeldaWWHDRecomp/ZeldaWWHDRecomp): The Legend of Zelda: The Wind
Waker HD (Wii U, USA) as a native app for 64-bit ARM phones and tablets, recompiled from the game's
PowerPC code to ARM, with its graphics running directly on Vulkan. No emulator.

This is a fork of [GreenNaugahyde/ZeldaWWHDRecompAndroid](https://github.com/GreenNaugahyde/ZeldaWWHDRecompAndroid).
Everything that port does still applies: you bring your own game dump, the app extracts it and
compiles the game code on your device on the first start. Its full README (getting started,
controllers, frame generation, building) is in [docs/android-port-readme.md](docs/android-port-readme.md).

**The app contains no game files**, no game code and no keys. You need your own Wii U disc image
(`.wud`/`.wux`) with its keys.

## What this fork adds

### Touch controls made for a phone
- **Floating left stick**: it appears where your thumb lands on the left side.
- **Camera by swiping** on the right side of the screen, with an adjustable speed; double tap to
  center it behind Link.
- **Context buttons that follow your game**: the app reads your save data, so
  - **B** shows the sword (or nothing until you have one), **ZR** the shield (or crouch);
  - **X / Y / R** show the item you assigned, with the game's own item artwork;
  - the **combat buttons** (jump attack, spin attack, vertical slash, dodge / backflip) appear once
    you have a sword;
  - the **D-pad** buttons (Wind Waker, cannon, salvage hook) appear once you have those items.
- **One-touch combat moves**: each button performs the GamePad combination for you.
- **Layout editor** (✎ next to the menu button): move and resize every control on a grid, remove the
  ones you don't want and put them back from a panel.
- **Gyro aiming** with the phone's sensors (and a recalibrate button), **rumble** as the phone's
  vibration, and vibration on touch.
- **Icons in the art style of the game**. Item, equipment and HUD icons are built **on your device
  from your own game files** the first time it starts; nothing of the game is shipped.

### Graphics
- **Native widescreen**: the game renders at your screen's shape (21:9, 20:9...) instead of
  stretching 16:9. The camera sees more to the sides with the same vertical view (Hor+), the
  culling follows, and the HUD keeps its proportions with hearts, rupees and buttons at the new
  screen edges (Options → *Widescreen*, switchable while playing). Ported from the official
  project's aspect-ratio support.
- **Shadows fixed**: no more flickering between the near (detailed) and far (basic) shading when
  moving the camera, and no more grid / moire pattern on sand and other ground. The causes were
  dynamic depth bias left over from the previous render pass and work submitted in chunks in the
  middle of a frame.
- Renderer fixes ported from the official Vulkan backend: invariant vertex positions for multipass
  depth tests, depth-compare samplers bound to depth surfaces, `GX2CopySurface` of depth and array
  slices, uniform blocks compared instead of trusted to write tracking.
- Ambient occlusion modes, including **Off**.

### Camera
- **Pinch to zoom** on the camera side: spread two fingers to bring the camera closer, pinch to
  move it further away (half to twice the game's distance). Zoomed out, the camera uses the game's
  own collision check and stays in front of walls. Not applied in first person.

### Performance
The renderer's CPU work per frame was cut to a fraction in busy scenes, measured with simpleperf on a
Snapdragon 8 Gen 2:
- a **fast path for repeated draws** (grass, trees, crowds: thousands per frame), which reuses
  everything resolved while only uniforms or vertex data change;
- **identical vertex and uniform copies reused** within a submission (often 20+ MB per frame);
- redundant Vulkan state skipped, cheaper shader and texture lookups, context switches that copy
  only the registers in use, descriptor sets reused between draws;
- **ADPF performance hints** and a choice of **CPU cores** (automatic, performance cores, or the
  prime core for the game or the renderer);
- the guest vsync clock **locked to the display**, the screen asked for 60 Hz instead of 120, a
  pre-rotated swapchain, and a scheduler tick that sleeps when idle;
- from the original project: the Miiverse thread that spun a whole core is throttled, the pipeline
  queue is checked in slices, and compile waits wake on completion;
- the game code compiled at `-O3`.

### More
- **60 fps** by frame interpolation (game logic stays at 30 steps a second, nothing speeds up),
  holding full speed anywhere between 30 and 60 fps, an **adaptive** mode that falls back to 30 where the device can't hold 60, and the original
  project's redesigned **True 60** (experimental).
- **Game language** selector: English, Spanish, French (the USA disc's languages).
- **Cheats** from the original project: items, sword and shield, hearts, songs, Triforce, dungeon
  items, infinite health / magic / ammo.
- A **GameCube to HD save converter** (`tools/savegame/`, run on a PC).
- Performance overlay with a **pacing diagnostic** in the log (which frames missed their vsync, and
  whether the game, the renderer or the GPU was late).

## Recommended settings

On a recent phone: **Frame rate** 60 fps (or adaptive), **rendering resolution** 1×,
**CPU cores** Automatic or *Prime core (game)*, **GX2DrawDone** *Wait*. The options marked
*Experimental* are for testing.

Most of the game holds 60 fps on a Snapdragon 8 Gen 2. Very busy views (the title screen flight
over the island, big islands seen from afar) can still drop, and a hot phone lowers its clocks.

## Getting started

1. Put your disc image (`.wux` or `.wud`), its disc key (`.key`, same name as the image) and the
   Wii U common key (`common.key`) in one folder on your device.
2. Install the APK from the Releases page and start it. Choose **Extract from your disc image…**
   and select that folder.
3. The app extracts the game and compiles its code for your device (about 5 minutes, once).

Requirements: Android 11 or newer, 64-bit ARM, Vulkan 1.1, about 2 GB of free storage and memory.

## Building

As in the [Android port](docs/android-port-readme.md#building): Android SDK 36, NDK 27.2.12479018,
JDK 17, CMake, Ninja, Python 3, and LLVM built once for Android (`tools/android/build-llvm.sh`), then

```sh
cd android
./gradlew assembleRelease -PwwhdDeviceRecomp -PwwhdVersionCode=<n> -PwwhdVersionName=<name>
```

Sign release APKs with your own key (`android/keystore.properties`, never committed).

## Legal notice

This is an unofficial fan project. It is not affiliated with, endorsed or sponsored by Nintendo.
"The Legend of Zelda", "The Wind Waker", "Wii U" and related names are trademarks of their
respective owners and are used here only to describe what this software is compatible with.

This repository and the APK contain **no game code, no game assets and no keys**. The icons in
`android/app/src/main/res/drawable-nodpi` are original artwork in the style of the game; the game's
own artwork is read from your copy on your device and never leaves it. You need your own, legally
obtained copy of the game, dumped from your own Wii U disc and console. Everything game-specific
created on your device must not be redistributed.

## License

Mozilla Public License 2.0 (see `LICENSE`), as the projects it is based on. Third-party code keeps
its own license (see the [Android port's README](docs/android-port-readme.md#license)).

## Credits

- [ZeldaWWHDRecomp](https://github.com/ZeldaWWHDRecomp/ZeldaWWHDRecomp): the recompilation, the
  Wii U system libraries, the 60 fps modes, the cheats, rumble, the aspect-ratio support and the
  renderer and performance fixes ported here.
- [GreenNaugahyde/ZeldaWWHDRecompAndroid](https://github.com/GreenNaugahyde/ZeldaWWHDRecompAndroid):
  the Android port (Vulkan renderer, on-device compilation, options menu, frame generation).
- [Cemu](https://github.com/cemu-project/Cemu): the GPU address library and shader decompiler (MPL-2.0).
- The [zeldaret/tww](https://github.com/zeldaret/tww) decompilation, for the save data and actor layouts.
