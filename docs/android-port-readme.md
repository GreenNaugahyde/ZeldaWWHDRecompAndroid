# TLoZ:TWW HD Recompiled — Android (the port this fork is based on)

> The README of [GreenNaugahyde/ZeldaWWHDRecompAndroid](https://github.com/GreenNaugahyde/ZeldaWWHDRecompAndroid) as of the version this fork is based on. For this fork, see the [README](../README.md).

An Android port of the [ZeldaWWHDRecomp](https://github.com/ZeldaWWHDRecomp/ZeldaWWHDRecomp) project:
The Legend of Zelda: The Wind Waker HD (Wii U, USA version) as a native app for 64-bit ARM Android
devices. The game's PowerPC code is recompiled to native ARM code, the Wii U system libraries the
game uses are reimplemented, and its graphics run directly on Vulkan. There is no emulator in
between.

**The app contains no game files.** You bring your own copy of the game: a disc image (`.wud` or
`.wux`) dumped from your own Wii U disc, with its keys. On the first start the app extracts the game
from it and builds the game code on your device.

The original project (macOS, the 60 fps modes, decompilation tools) is described in
[docs/original-readme.md](original-readme.md); how the recompilation works in
[docs/how-it-works.md](how-it-works.md).

## What this fork adds

- **Android port**: Vulkan renderer, AAudio sound, touch screen, game controllers and hardware
  keyboards.
- **An APK without game code**: on the first start the app extracts the game from your disc image
  and recompiles its code on the device with LLVM (about 5 minutes, once). Later starts load the
  compiled code in about half a second. The work continues in the background with a progress
  notification, and resumes where it stopped if the app is closed.
- **Frame generation** with Lossless Scaling's LSFG 3 (see below): 60, 90 or 120 fps.
- **An in-game options menu** in the style of the game's own menus, with tabs for saves, graphics,
  mods and controls, usable by touch and with a controller (see below).
- **Save states**: five slots with a picture, time and place of each state.
- **Import and export** of the game save and the save states to a folder of your choice.
- **Performance overlay**: frame rate, frame time, CPU and GPU load, CPU, GPU and battery
  temperatures; you choose the values and drag it where you want it.
- **Display options**: rendering resolution from 0.5× to 3×, aspect ratio (bars, stretched, or
  filling the screen), screen layouts for the TV and GamePad pictures (GamePad inset, side by side,
  TV only, GamePad with TV inset). All of them apply immediately, without a restart.
- **On-screen controls** for the whole GamePad, which hide while a controller is in use.
- **The original project's gameplay mods** on Android: climb any wall, direct right-stick camera,
  first person on R3, quick doors, fast scene changes.
- **Fixes**: correct lighting on the first visit to a scene with an empty shader cache.
- **No internet access**: the app doesn't request it, and its manifest explicitly excludes it.

## Getting started

You need:

- an Android 11 (or newer) device with a 64-bit ARM processor and Vulkan 1.1, about 2 GB of free
  storage and, for the one-time compile, about 2 GB of free memory;
- your own dump of The Wind Waker HD (USA): the disc image (`.wux` or `.wud`), its disc key (a
  `.key` file with the image's name) and the Wii U common key (`common.key`).

None of these are included or provided here.

1. Put the image and both keys in one folder on your device.
2. Install the APK and start it. Choose **Extract from your disc image…** and select that folder.
3. The app extracts the game files (a few seconds to minutes), then prepares the game code for
   your device (about 5 minutes, depends on your hardware). You can leave the app meanwhile and read a Wind Waker walkthrough guide. A notification shows the progress and keeps the process alive.
4. The game starts. The first visit to each place may stutter briefly while its shaders compile as usual.
   After that they are cached.

## Controls

The **on-screen controls** cover the Wii U GamePad: both sticks, the D-pad, A/B/X/Y, L/R/ZL/ZR,
−/+ and the stick clicks (L3/R3). The
controls hide while a game controller is in use and come back on the next touch.

**Game controllers** map by button position, as the game uses them: the bottom face button is the
Wii U's B, the right one A, the left one Y and the top one X; Select is −, Start is +.

**Keyboards**: WASD move, the arrow keys turn the camera, K or Space = A, J = B, L = X, I = Y, Q/E =
L/R, Left Shift = ZL, C = ZR, Enter = +, Tab = −, H = HOME, 1–4 = D-pad, X/V = stick clicks.

## The options menu

The game keeps running while the menu is open, and changes apply at once.

**How to open it**

- touch: the ≡ button at the bottom of the screen, or Android's Back.
- controller: **hold Select** (View, Share, −: whatever the controller calls it) for a moment, or
  press **Home / Guide** (the logo button, where the controller passes it to apps). A short press
  of Select is still the game's − button. The same buttons close the menu again.

**With a controller**: L / R switch tabs, the D-pad moves and changes values, and as in the game
the **right** face button selects and the **bottom** one goes back. (On an Xbox-style controller
that is B to select and A to go back; on a PlayStation controller Circle selects and Cross goes
back.)

**The tabs**

- **Saves**: five save state slots with Save and Load (a state can only be loaded during
  gameplay, for example the save select screen or anywhere in the game, and only with the build that saved it); exporting and importing saves. An imported game
  save restarts the game.  
- **Graphics**: rendering resolution, frame generation, aspect ratio, screen layout, ambient
  occlusion, full-size occlusion depth, 16× anisotropic filtering, the performance overlay, and
  deleting the shader cache (restarts the game as on its first start).
- **Mods**: the gameplay mods, all off by default.
- **Controls**: on-screen controls on or off, their size, and whether the controls act as a Wii U
  GamePad or a Pro Controller.

About (with the licenses) and Quit are always on the left.

## Frame generation (Lossless Scaling)

Frame generation adds frames between the game's 30 frames per second: ×2 = 60 fps, ×3 = 90 fps,
×4 = 120 fps, on a screen with that refresh rate. It uses the frame generation network of
[Lossless Scaling](https://store.steampowered.com/app/993090/Lossless_Scaling/) (LSFG 3), which is
not part of this app: it needs the usual **`Lossless.dll`** from a Lossless Scaling installation on
Windows (in the program's folder in your Steam library).

Lossless Scaling costs a few euros on Steam, and its developer has earned every one of them. **Buy
it, don't pirate it like Gonzo** would. And never share the DLL with others.

1. Copy `Lossless.dll` to your device.
2. In the menu, open Graphics › Frame generation › **Select Lossless.dll** and pick the file.
3. The app checks it, copies it into its private storage and switches frame generation on.

The app was developed and tested with a `Lossless.dll` of 7,521,280 bytes; its shaders' SHA-1
fingerprint is `fc6092a72b94003fac3d68e9a7b54954422db757`. A different version with the same shader
layout is accepted with a warning, since it may compute something else and show wrong frames; a
version with a different layout is rejected with the reason. If frame generation can't start on a
device, its page in the menu says why.

Options: multiplier, network (performance, or quality at about 40% more GPU time), flow scale
(resolution of the motion estimate; lower is faster, 50% is the default) and UI detection (keeps
menus and text from warping). Generated frames show the game about one frame later. On the tested Snapdragon
7+ Gen 3 (2560×1600, performance network, 50% flow scale): about 7 ms of GPU time per game
frame for ×2, 9 ms for ×3; ×4 needs a 25% flow scale.

`runtime/src/vk/lsfg.cpp` is an independent implementation that runs the DLL's shaders; how they
connect was worked out from the DLL itself. It contains no code from other LSFG projects.

## Building

You need the Android SDK (platform 36), NDK 27.2.12479018, JDK 17, CMake 3.20+, Ninja, git and
Python 3.

**The APK without game code** builds LLVM for Android once (about 20
minutes on a 12-core PC, 2 GB in `build/llvm`), then the app:

```sh
tools/android/build-llvm.sh          # -> build/llvm/install (LLVM 20.1.8: AArch64 code generation, ORC JIT)
cd android
echo "sdk.dir=$HOME/Android/Sdk" > local.properties
./gradlew assembleRelease -PwwhdDeviceRecomp -PwwhdVersionCode=3 -PwwhdVersionName=0.3
```

Sign it with your own release key.  

```sh
keytool -genkeypair -keystore ~/.android/wwhd-release.jks -storetype PKCS12 -alias wwhd \
        -keyalg RSA -keysize 4096 -validity 10000 -dname "CN=Your name"
cat > android/keystore.properties <<EOF
storeFile=/home/you/.android/wwhd-release.jks
storePassword=...
keyAlias=wwhd
keyPassword=...
EOF
```

`android/keystore.properties`, `*.jks` and `*.keystore` are in `.gitignore`; never commit them.
Raise the version code with every APK you share. Release builds ignore the testing launch extras
(`WWHD_*`, `gameDir`).

## Legal notice

This is an unofficial fan project. It is not affiliated with, endorsed or sponsored by Nintendo or
by the developer of Lossless Scaling. "The Legend of Zelda", "The Wind Waker", "Wii U" and related
names are trademarks of their respective owners and are used here only to describe what this
software is compatible with.

This repository and the APK contain **no game code, no game assets, no keys and
no part of Lossless Scaling**. To use the app you need your own, legally obtained copy of the game,
dumped from your own Wii U disc and console, and for frame generation your own copy of Lossless
Scaling. Everything game-specific (the extracted files, the compiled game code, shader caches) is
created on your device from your own legal dump and must not be redistributed. 

## License

The code of this project is licensed under the Mozilla Public License 2.0 (see `LICENSE`).
Third-party code keeps its own license: Cemu (MPL-2.0), {fmt} (MIT), glslang (BSD-3-Clause and
others), the Vulkan Memory Allocator (MIT), and in the APK without game code
[LLVM](https://llvm.org) and the NDK's libc++ (Apache-2.0 with LLVM Exceptions). The app shows all
of these licenses under About.

## Credits

This fork is built on [ZeldaWWHDRecomp](https://github.com/ZeldaWWHDRecomp/ZeldaWWHDRecomp), which
did the recompilation, the Wii U system libraries and the original Metal renderer for macOS. The GPU
address library, shader decompiler and a few reference structures are vendored from
[Cemu](https://github.com/cemu-project/Cemu) (MPL-2.0); `tools/wudextract.py` and parts of the OS
layer are ported from or follow Cemu as noted in those files. Frame generation uses the shaders of
[Lossless Scaling](https://store.steampowered.com/app/993090/Lossless_Scaling/) from your own copy.
