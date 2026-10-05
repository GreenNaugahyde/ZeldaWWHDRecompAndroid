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

    /** Null if gameDir holds the executable this build was recompiled from, else a message. */
    static native String checkGame(String gameDir);
    /** Extracts the game from a disc image (fd: the open image, closed here) into outDir: null, or why not. Blocks. */
    static native String extractGame(int fd, byte[] discKey, byte[] commonKey, String outDir);
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
    /** The licenses of everything in the app, as text. */
    static native String licenses();
    /** Boots the runtime and starts the game (once per process). */
    static native void start(String gameDir, String saveDir, String cacheDir, String workDir);
    /** Null if path is a Lossless.dll whose frame generation shaders this build can run, else why not. */
    static native String checkFrameGenDll(String path);
    /** True if path is a usable Lossless.dll with a shader version this build was tested with. */
    static native boolean frameGenDllTested(String path);
    /** Why frame generation couldn't start, or "" (it runs, or it is off). */
    static native String frameGenError();

    static native void surfaceChanged(Object surface);
    static native void surfaceDestroyed();
    /** Screen rectangles in surface pixels: {x, y, w, h}. */
    static native void setLayout(float[] tv, float[] drc, boolean drcVisible);

    static native void setPad(int buttons, float lx, float ly, float rx, float ry);
    /** Touch on the GamePad screen, 0..1 from the top left. */
    static native void setTouch(boolean down, float x, float y);
    static native void textInputDone(boolean ok, String text);
    static native void setPaused(boolean paused);

    /**
     * ao_mode (0..2), ao_hires, aniso, pro_controller (0/1); capture (any value); gameplay mods:
     * mod_direct_camera, mod_camera_speed (percent), mod_first_person, mod_climb, mod_quick_doors, mod_fast_scenes.
     */
    static native void setOption(String name, int value);
    static native int getOption(String name);
    /** game state for the touch controls: {flags, A action, B action, ZR action, X, Y, R item} */
    static native int[] hudState();
    /** gyro aiming: a sensor sample in GamePad axes (rad/s, m/s^2, ns), on/off, recentre */
    static native void setMotion(float gx, float gy, float gz, float ax, float ay, float az, long timestampNs);
    static native void setMotionEnabled(boolean on);
    static native void recalibrateMotion();
    /** decodes game textures to outDir/<texture>.rgba (IconForge); returns how many */
    static native int extractUiTextures(String gameDir, String outDir, String[] layouts, String[] textures);

    /** Save state slot 1..5: {used "1"/"0", compatible "1"/"0", time, area}. */
    static native String[] saveSlotInfo(int slot);
    /** Saved / loaded at the next frame boundary; the result shows in saveStateMessage(). */
    static native void saveState(int slot);
    static native void loadState(int slot);
    /** The latest save state result, "" when stale. */
    static native String saveStateMessage();
    /** Frame generation settings; applied from the next frame on (the DLL is read again). */
    static native void applyFrameGen(boolean on, String dll, boolean quality, float flowScale, int multiplier, boolean uiDetection);
    /** Performance overlay: {game fps, frame time avg ms, worst ms, presented fps, frame generation GPU ms}. */
    static native float[] perfStats();
    /** Climb mod stamina wheel: {stamina 0..1, alpha 0..1 (0 = hidden), exhausted 0/1}. */
    static native float[] climbHud();

    /** Called by the game (software keyboard) on one of its threads. */
    @SuppressWarnings("unused")
    /** the game's rumble (GamePad motor): the phone vibrates, strength 0..1 for ms; 0 stops it */
    static void rumble(float strength, int ms) {
        MainActivity a = MainActivity.instance;
        if (a != null) a.rumble(strength, ms);
    }

    static void requestTextInput(String initial, int maxLen) {
        MainActivity a = MainActivity.instance;
        if (a == null) {
            textInputDone(false, "");
            return;
        }
        a.runOnUiThread(() -> a.showTextInput(initial, maxLen));
    }
}
