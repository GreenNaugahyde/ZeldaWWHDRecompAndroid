# TLoZ:TWW HD Recompiled — Android

An Android port of the [ZeldaWWHDRecomp](https://github.com/ZeldaWWHDRecomp/ZeldaWWHDRecomp) project:
The Legend of Zelda: The Wind Waker HD (Wii U, USA and European versions) as a native app for 64-bit ARM Android
devices. The game's PowerPC code is recompiled to native ARM code, the Wii U system libraries the
game uses are reimplemented, and its graphics run directly on Vulkan. There is no emulator in
between.

**The app contains no game files.** You bring your own copy of the game: a disc image (`.wud` or
`.wux`) dumped from your own Wii U disc, with its keys. On the first start the app extracts the game
from it and builds the game code on your device.

The original project (macOS, the 60 fps modes, decompilation tools) is described in
[docs/original-readme.md](docs/original-readme.md); how the recompilation works in
[docs/how-it-works.md](docs/how-it-works.md).

## What this fork adds

- **Android port**: Vulkan renderer, AAudio sound, touch screen, game controllers and hardware
  keyboards.
- **An APK without game code**: on the first start the app extracts the game from your disc image
  and recompiles its code on the device with LLVM (once; a few minutes on a fast phone, longer on
  slower ones). Later starts load the
  compiled code in about half a second. The work continues in the background with a progress
  notification, and resumes where it stopped if the app is closed.
- **Frame generation** with Lossless Scaling's LSFG 3 (see below): 60, 90 or 120 fps.
- **An in-game options menu** in the style of the game's own menus, with tabs for saves, the game,
  graphics, mods and controls, usable by touch and with a controller (see below).
- **Save states**: five slots with a picture, time and place of each state, and the controls it was
  saved with (GamePad or Pro Controller), which loading it switches back to.
- **Import and export** of the game save and the save states to a folder of your choice.
- **Performance overlay**: frame rate with its average, frame time, CPU and GPU load, CPU, GPU and
  battery temperatures, the graphics settings in use, the SoC, GPU and GPU driver; you choose the
  values and drag it where you want it.
- **The European version** as well as the USA one, with all features: the game code of both is
  the same program at shifted addresses, and the app's hooks and mods find their places in either
  (`tools/recomp/port_addresses.py` matches the two executables; the address map it writes is in
  `tools/recomp/release_eur.txt`).
- **Game language**: the languages of your copy (USA: English, French, Spanish; Europe: also German
  and Italian), by default the device's language setting is used.
- **Custom GPU drivers** on Adreno GPUs (see below): Mesa Turnip or newer Qualcomm drivers.
- **Display options**: rendering resolution from 0.5× to 3×, aspect ratio (bars, stretched, or
  filling the screen), screen layouts for the TV and GamePad pictures (GamePad inset, side by side,
  TV only, GamePad with TV inset). All of them apply immediately, without a restart.
- **Dual-screen devices** (AYN Thor and the like): the GamePad picture goes to the second screen by
  itself, with touch, and the main screen shows the TV picture; Graphics › GamePad screen switches
  back to the screen layouts.
- **On-screen controls** for the whole GamePad, which hide while a controller is in use.
- **Gyro aiming** with the device's motion sensors or a controller's (DualSense, DualShock 4, Switch
  Pro and others that Android reports with sensors), also with "Wii U Pro Controller" selected,
  where the original game has no gyro aiming.
- **Rumble** on the controller in use, or on the device while playing by touch.
- **The original project's gameplay mods** on Android: climb any wall, direct right-stick camera,
  first person on R3, quick doors, fast scene changes.
- **Performance work for weaker devices**: BC textures unpacked on the GPU where it can't sample
  them (most Mali and PowerVR GPUs), precise Vulkan barriers (on a Mali-G52 the GPU time per frame
  drops from 52 to 40 ms), less work on the render thread, Android performance hints and game
  modes, and SVE where the processor has it.
- **Fixes**: correct lighting on the first visit to a scene with an empty shader cache; game files
  found on devices whose storage tells upper and lower case apart.
- **No internet access**: the app doesn't request it, and its manifest explicitly excludes it.

## Version history

**0.3**

- The European version of the game is now supported. A Game tab to choose the game language was added in the overlay options menu.
- Custom GPU drivers on Adreno GPUs (Turnip, newer Qualcomm drivers). Install them in the overlay options menu in the graphics settings.
- Gyro aiming with the device or a controller motion sensors, also in Pro Controller mode which is useful for single screen devices.
- Force feedback is working on the device in touchscreen mode or with a connected controller.
- The WiiU GamePad picture on the second screen of dual-screen devices.
- Save states keep the controller mode and switch back to it when loaded.
- Performance overlay: average frame rate, the graphics settings in use, SoC, GPU and GPU driver,
  with labelled values.
- Faster on Mali and other GPUs without BC textures; less CPU time on the render thread; Android
  performance hints, game modes and SVE.
- Fixes:
  - A crash at boot where the device's storage tells upper and lower case apart
  - The Saves tab shows a new state as soon as it is written
  - Removing a GPU driver removes its pipeline cache

**0.2**: the first release (USA version).

## Getting started

You need:

- an Android 11 (or newer) device with a 64-bit ARM processor and Vulkan 1.1, about 2 GB of free
  storage and, for the one-time compile, about 2 GB of free memory;
- your own dump of The Wind Waker HD (USA or Europe): the disc image (`.wux` or `.wud`), its disc key (a
  `.key` file with the image's name) and the Wii U common key (`common.key`).

None of these are included or provided here.

1. Put the image and both keys in one folder on your device.
2. Install the APK and start it. Choose **Extract from your disc image…** and select that folder.
3. The app extracts the game files (a few seconds to minutes), then prepares the game code for
   your device (once; how long depends on the processor: about 6 minutes on a Snapdragon 7+ Gen 3,
   7.5 minutes on a Snapdragon 855, 16 minutes on a Helio G85). You can leave the app meanwhile and read a Wind Waker walkthrough guide. A notification shows the progress and keeps the process alive.
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
  gameplay, for example the save select screen or anywhere in the game, and only with the build that saved it).
  A state also keeps whether the controls acted as GamePad or Pro Controller; loading it switches
  back to that, since the game only listens to the controller it was saved with. Exporting and importing saves. An imported game
  save restarts the game.  
- **Game**: the game language (the languages of your copy; applies after a restart).
- **Graphics**: rendering resolution, frame generation, aspect ratio, the GamePad screen (with a
  second display), screen layout, ambient
  occlusion, full-size occlusion depth, 16× anisotropic filtering, the performance overlay, the GPU
  driver (Adreno only), and deleting the shader cache (restarts the game as on its first start).
- **Mods**: the gameplay mods, all off by default.
- **Controls**: on-screen controls on or off, their size, whether the controls act as a Wii U
  GamePad or a Pro Controller, motion controls (gyro aiming) and rumble.

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

## GPU drivers (Adreno)

On devices with an Adreno GPU the game can run on another Vulkan driver than the one the device came
with, for example a Mesa Turnip build or a newer Qualcomm driver. The app loads it with
[libadrenotools](https://github.com/bylaws/libadrenotools) and takes the
usual driver packages: a `.zip` with a `meta.json` and the driver's `.so` file.

1. Copy the driver package to your device.
2. In the menu, open Graphics › GPU driver › **Install driver package…** and pick the file.
3. Choose **Use and restart**. The first start with a driver takes a little longer while it compiles
   the game's pipelines; each driver keeps its own pipeline cache.

The page shows which driver is running. If the game doesn't start with a driver (it crashes, hangs or
can't be loaded), the next start goes back to the system driver and says so. Drivers differ a lot in
speed for this game: try a few, and compare with the performance overlay.

On some older Snapdragon phones the system driver makes the GPU hang after a few seconds to minutes
of play and the game closes (seen on a Snapdragon 855 / Adreno 640 with a Qualcomm driver from
2021). A Turnip driver runs the game there without these crashes, so if the game keeps closing on
such a device, install one.

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
others), the Vulkan Memory Allocator (MIT), volk (MIT), libadrenotools (BSD-2-Clause), and in the
APK without game code
[LLVM](https://llvm.org) and the NDK's libc++ (Apache-2.0 with LLVM Exceptions). The app shows all
of these licenses under About.

## Credits

This fork is built on [ZeldaWWHDRecomp](https://github.com/ZeldaWWHDRecomp/ZeldaWWHDRecomp), which
did the recompilation, the Wii U system libraries and the original Metal renderer for macOS. The GPU
address library, shader decompiler and a few reference structures are vendored from
[Cemu](https://github.com/cemu-project/Cemu) (MPL-2.0); `tools/wudextract.py` and parts of the OS
layer are ported from or follow Cemu as noted in those files. Frame generation uses the shaders of
[Lossless Scaling](https://store.steampowered.com/app/993090/Lossless_Scaling/) from your own copy.
