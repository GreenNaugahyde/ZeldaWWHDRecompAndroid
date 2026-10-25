package org.wwhdrecomp.app;

/** Entry points into libwwhd.so (runtime/src/android/jni_main.cpp). */
final class Native {
    private Native() {}

    // Wii U GamePad button bits (VPAD), as in runtime/src/input.h
    static final int A = 0x8000, B = 0x4000, X = 0x2000, Y = 0x1000;
    static final int L = 0x0020, R = 0x0010, ZL = 0x0080, ZR = 0x0040;
    static final int PLUS = 0x0008, MINUS = 0x0004, HOME = 0x0002;
    static final int UP = 0x0200, DOWN = 0x0100, LEFT = 0x0800, RIGHT = 0x0400;
    static final int STICK_R = 0x00020000, STICK_L = 0x00040000;
    // not GamePad buttons: Jump anywhere's and the moon jump's own inputs (input.h kJump / kMoonJump; never the game's)
    static final int JUMP = 0x00100000, MOON = 0x00200000;

    /** Null if gameDir holds the executable this build was recompiled from, else a message. */
    static native String checkGame(String gameDir);
    /** Extracts the game from a disc image (fd: the open image, closed here) into outDir: null, or why not. Blocks. */
    static native String extractGame(int fd, byte[] discKey, byte[] commonKey, String outDir);
    /** Touch controls: the right side turns the camera by swiping (on) or is a stick (off). */
    static native void setTouchSwipeCamera(boolean on);
    /** A mouse wheel's movement in notches (+ forward / up): first person on the mouse wheel (mods/camera.cpp). */
    static native void mouseWheel(float dy);
    /** A swipe on the right side, in dp (+y down). */
    static native void cameraSwipe(float dx, float dy);
    /** {valid, A action, sword, X item, Y item, R item, B action, ZR action, event mode, pause menu open, has the Wind Waker, shield} (0xFF: none; all 0 outside a game). */
    static native int[] touchState();
    /** Songs by microphone (songs.cpp): {conducting, learned songs (bits by melody; -1 unknown), has the Wind Waker, another event runs, songs handed to the game}. */
    static native int[] songState();
    /** The game's judge answers this melody (0 Wind's Requiem .. 5 Song of Passing) at the next beat while Link conducts. */
    static native void playSong(int melody);
    /** a beat of the singing count-in: the game's metronome click (and on the last, its upbeat) */
    static native void countBeat(int time, boolean last);
    /** The HUD's text for each button action code: NN of T_ACT_Command_NN (0 none); empty until the game filled its table (touch_state.cpp). */
    static native int[] actionTexts();
    /** The item ring: {21 inventory item ids (0xFF empty), X / Y / R inventory slots, item in Link's hand (0x100 none, 0x103 sword), the bags' 3 x 8 items}; all -1 outside the game. */
    static native int[] itemRing();
    /** Puts an inventory slot (0..20, a bag's 24.., 36.., 48..) on X / Y / R (0..2) at the next logic step (an item on another button swaps). */
    static native void equipItem(int button, int slot);
    /** The Wind Waker's slot on X / Y / R (0..2) only until Link has conducted with it (the caller presses the button), then the button's item comes back. */
    static native void conductWith(int button, int slot);
    /** The game's pace while the item ring is open: 0 normal, 1 slow motion (a third), 2 paused. */
    static native void setGamePace(int mode);
    /** The minigame played by touch now (runtime/src/mods/minigames.cpp): 0 none, else which (MINIGAME_*). */
    static native int minigameMode();
    /** A finger on the TV picture during a minigame: action 0 down, 1 moved, 2 up; x, y in the game's layout units
     *  (720 across the picture's height, 0 at its centre, +y up). */
    static native void minigameTouch(int action, float x, float y);
    /** Debug (WWHD_MG_WATCH): points to mark on the TV picture, x, y pairs in layout units; empty without. */
    static native float[] minigameMarks();
    /** The message window's box: {shown, x, y, half width, half height} in the TV's layout units (720 across the
     *  picture's height, 0 at its centre, +y up). */
    static native int[] msgWindow();
    /** the wind compass after the Wind's Requiem: {shown, centre x, y} in layout units */
    static native int[] windCompass();
    /** HUD panes not to draw, "name,name,..." (runtime/src/hud.h). */
    static native void setHudHidden(String names);
    /** HUD elements that fade out when not needed: 0 hearts (sword in hand, damage), 1 rupees (count changed),
     *  2 magic bar (magic used / refilled). */
    static native void setHudFade(int which, boolean on);
    /** Debug: every texture of the 2D pack as outDir/layout__texture.rgba; how many. */
    static native int dumpUiTextures(String gameDir, String outDir);
    /** Textures from the game's 2D pack, written as outDir/texture.rgba (uint32 w, h, RGBA8): how many. */
    static native int extractUiTextures(String gameDir, String outDir, String[] layouts, String[] textures);
    /** Link in his own poses (runtime/src/android/link_icons.cpp) as outDir/link_*.rgba, like the textures: how many. */
    static native int renderLinkIcons(String gameDir, String outDir);
    /** One section of the crash log's context (runtime/src/crash_info.h). */
    static native void setCrashInfo(String section, String text);
    /** The same from a Cemu .wua archive (decrypted, no keys). */
    static native String extractArchive(int fd, String outDir);
    /** {bytes written, total} of the running extraction. */
    static native long[] extractProgress();
    static native void extractCancel();
    /** A key file's contents ("16 raw bytes or 32 hex digits") as 16 bytes, or null. */
    static native byte[] parseKey(byte[] data);
    /** True if this build contains no game code and compiles it on the device. */
    static native boolean buildsGameCode();
    /** True if the game code has to be compiled first (builds that recompile on the device). */
    static native boolean needsCompile(String gameDir, String codeDir);
    /** Compiles the game code into codeDir (resuming an interrupted compile): null, or why not. Blocks. */
    static native String compileGame(String gameDir, String codeDir);
    /** {modules done, modules in total (0 until known)} of the running compile. */
    static native long[] compileProgress();
    static native void compileCancel();
    /** The game release in gameDir: "USA", "EUR", "JPN", or "" (none or unknown). */
    static native String gameRelease(String gameDir);
    /** The licenses of everything in the app, as text. */
    static native String licenses();
    /** Boots the runtime and starts the game (once per process). */
    static native void start(String gameDir, String saveDir, String cacheDir, String workDir);
    /** Null if path is a Lossless.dll whose frame generation shaders this build can run, else why not. */
    static native String checkFrameGenDll(String path);
    /** True if path is a usable Lossless.dll with a shader version this build was tested with. */
    static native boolean frameGenDllTested(String path);
    /** The running GPU driver's name and version, "" until the renderer has started. */
    static native String gpuDriverInfo();
    /** The GPU's name (e.g. "Adreno (TM) 640"), "" until the renderer has started. */
    static native String gpuName();
    /** True if the chosen installed driver couldn't be loaded and the system's runs instead. */
    static native boolean gpuDriverFellBack();

    /** The shader cache's replay at start: {state (0 not started, 1 loading, 2 done), bytes read, file size, shaders prepared}. */
    static native long[] cacheProgress();
    /** Why frame generation couldn't start, or "" (it runs, or it is off). */
    static native String frameGenError();
    // GPUs where frame generation can hang the GPU (lsfg.cpp risky_gpu; false before the renderer starts),
    // and whether the player accepted the warning for them
    static native boolean frameGenRiskyGpu();
    static native void setFrameGenRiskAccepted(boolean on);

    static native void surfaceChanged(Object surface);
    static native void surfaceDestroyed();
    /** The GamePad picture's own display (a second screen): its surface, or null. */
    static native void drcSurfaceChanged(Object surface);
    /** Screen rectangles in surface pixels: {x, y, w, h}. */
    static native void setLayout(float[] tv, float[] drc, boolean drcVisible);

    /** the item buttons' letters on the HUD and the GamePad's drop slots: "X,Y,R" order ("" each: the game's) */
    static native void setSlotLabels(String labels);
    static native void setPad(int buttons, float lx, float ly, float rx, float ry);
    /** Touch on the GamePad screen, 0..1 from the top left. */
    static native void setTouch(boolean down, float x, float y);
    // the GamePad picture on a second display: its shape (0 = 16:9, else the display's), the shape of
    // the picture the game sends now, and touches on it (0..1 of the picture)
    // the graphics benchmark since the last call: {frames shown, seconds, GPU ms per frame, busiest thread ms
    // per frame (0 unknown), worst frame interval ms, draws left out while compiling} (setOption "benchmark" switches it on)
    static native float[] benchmarkSample();
    static native void setDrcAspect(float aspect);
    /** the inventory's pages (items x 0, the page right -1280, options +1280): 1 follow (v layout units to the right of
     * where the drag began), 2 glide to x = v, 0 leave them (aspect.h) */
    static native void setItemPageDrag(int mode, float dx);
    static native boolean itemPageGliding();
    /** a font of the game's 2D pack: glyph sheets and metrics into outDir (ui_icons.cpp extract_font) */
    static native boolean extractUiFont(String gameDir, String outDir, String font);
    /** a layout's texts of the game's 2D pack as "label<TAB>text" lines in outFile (ui_icons.cpp extract_texts) */
    static native boolean extractUiTexts(String gameDir, String outFile, String layout);
    static native float itemPageX();
    static native void setItemSheetLeft(boolean on);
    static native float drcPictureAspect();
    static native void setDrcTouch(boolean down, float u, float v);
    static native void textInputDone(boolean ok, String text);
    static native void setPaused(boolean paused);

    /**
     * ao_mode (0..2), ao_hires, aniso, pro_controller (0/1); capture (any value); gameplay mods:
     * mod_direct_camera, mod_camera_speed (percent), mod_first_person, mod_climb, mod_quick_doors, mod_fast_scenes, mod_ff_cutscenes, mod_ff_dialogues,
     * mod_run_speed (percent), mod_run_mode (0 always, 1 hold L3, 2 L3 switches), mod_swim_speed (percent), mod_swim_mode (as run),
     * mod_climb_speed / mod_climb_mode, mod_sidle_speed / mod_sidle_mode (as run);
     * auto_state (auto save state interval in minutes, 0 off); vsync (1: frames wait for the display refresh).
     */
    static native void setOption(String name, int value);

    /** a mod pack's description for the menu (runtime/src/packs.h, packs::describe) */
    static native String packDescribe(String packDir, String selection, String gameDir);

    /** Unpacks a mod archive (zip, RAR; open file descriptor) into destDir: null when done, else why not. */
    static native String extractModArchive(int fd, String destDir);
    static native int getOption(String name);

    /** Warp (developer menu): to a stage's room and spawn at the next frame; layer -1 = the story's. False without a loaded save file. */
    static native boolean warp(String stage, int room, int spawn, int layer);
    /** The warp menu's places from the game files: gameDir/warps.json (a few seconds). Null when done, else why not. */
    static native String buildWarps(String gameDir);
    /** Warp with a story preset (the save's dSv_save_c, 0x778 bytes) written into the live save first. */
    static native boolean warpPreset(byte[] story, String stage, int room, int spawn, int layer);
    /** The next warp puts Link at x, y, z facing angle (s16, 0 = +z) once it has loaded and no cutscene runs. */
    static native void warpPlace(float x, float y, float z, int angle);
    /** The live save's story part (for a preset), null outside the game. */
    static native byte[] storyBlock();
    /** The latest warp: {0 none, 1 requested, 2 changing scene, 3 loaded, 4 refused; ms since the request, or the load time; event mode (0 none, 1 dialogue, 2 / 3 cutscene)}. */
    static native long[] warpStatus();
    /** "stage,room,spawn,layer" of the current scene, "" outside the game. */
    static native String warpWhere();

    /** Save state slot 1..5, auto slots 6..7, last session 8: {used "1"/"0", compatible "1"/"0", time, area, controller}. */
    static native String[] saveSlotInfo(int slot);
    /** Saved (1..5) / loaded (1..8) at the next frame boundary; the result shows in saveStateMessage(). */
    static native void saveState(int slot);
    static native void loadState(int slot);
    /** Before start(): the game waits before its intro (after the shader cache replay) until a state loads or this is released. */
    static native void setStartHold(boolean on);
    /** 0 no hold, 1 armed, 2 the game is waiting for the choice. */
    static native int startHoldState();
    /** The stage loaded: FRONT_TITLE (title screen), FRONT_FILES (file select), FRONT_OPENING (on from the
     *  file select: a new game's opening, or loading), FRONT_BOOT (no stage yet) or 0. */
    static native int frontScreen();
    /** the GamePad screen's touch targets drawn now: "name parent x y w h;" (16:9 screen, 0..1, y down) */
    static native String drcTargets();
    /** the game shows its own button cursor on the GamePad screen (it takes the buttons there) */
    static native boolean drcGameCursor();
    static native void setGamePaused(boolean on);
    static native int pauseKind();
    static final int FRONT_TITLE = 1, FRONT_FILES = 2, FRONT_OPENING = 3, FRONT_BOOT = 4;
    /** The app goes to the background: saves the last session's state (slot 8); returns once it is taken (at most ~1 s). */
    static native void saveStateOnLeave();
    /** Save states taken but not yet written (Quit waits for them). */
    static native int stateWritesPending();
    /** The latest save state result, "" when stale. */
    static native String saveStateMessage();
    /** Frame generation settings; applied from the next frame on (the DLL is read again). */
    static native void applyFrameGen(boolean on, String dll, boolean quality, float flowScale, int multiplier, boolean uiDetection);
    /** Performance overlay: {game fps, frame time avg ms, worst ms, presented fps, frame generation GPU ms}. */
    static native float[] perfStats();
    /** Climb mod stamina wheel: {stamina 0..1, alpha 0..1 (0 = hidden), exhausted 0/1}. */
    static native float[] climbHud();

    /** One motion sample in SDL controller axes (gyro rad/s, acceleration m/s²), dt in seconds. */
    static native void motionSample(float dt, float gx, float gy, float gz, float ax, float ay, float az);
    /** Motion sensors stopped: the GamePad reports resting values. */
    static native void motionReset();

    /** Called by the game (VPADControlMotor): a rumble pattern of `bits` 1/120 s steps, or stop (0). */
    @SuppressWarnings("unused")
    static void rumble(byte[] pattern, int bits) {
        MainActivity a = MainActivity.instance;
        if (a != null) a.rumbler().pattern(pattern, bits);
    }

    /** Called by the game (Pro Controller): rumble motor on or off. */
    @SuppressWarnings("unused")
    static void rumbleHold(boolean on) {
        MainActivity a = MainActivity.instance;
        if (a != null) a.rumbler().hold(on);
    }

    /** Called by the game (software keyboard) on one of its threads. */
    @SuppressWarnings("unused")
    static void requestTextInput(String initial, int maxLen) {
        MainActivity a = MainActivity.instance;
        if (a == null) {
            textInputDone(false, "");
            return;
        }
        a.runOnUiThread(() -> a.showTextInput(initial, maxLen));
    }
}
