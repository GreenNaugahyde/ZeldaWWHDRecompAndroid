package org.wwhdrecomp.app;

import android.app.ActivityManager;
import android.content.Context;
import android.content.SharedPreferences;
import android.opengl.EGL14;
import android.opengl.EGLConfig;
import android.opengl.EGLContext;
import android.opengl.EGLDisplay;
import android.opengl.EGLSurface;
import android.opengl.GLES20;
import android.util.Log;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileReader;
import java.util.Locale;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

/**
 * Graphics presets (Graphics > Preset): rendering resolution, dynamic resolution's lowest scale, the
 * frame rate, anisotropic filtering and the full-size occlusion depth as one choice. Two sets of steps: with frame generation (the game
 * at 30 fps, Lossless.dll doubles it, so the GPU has room for more pixels) and without (60 fps by
 * interpolation on the upper steps, which needs the CPU for it). A setting changed by hand makes it
 * Custom: the preset is only what the settings match, nothing else is stored.
 *
 * A new installation starts at a guess from the GPU's name, the CPU's clock and the RAM; the
 * benchmark (later) replaces it with measured numbers. Existing installations keep their settings.
 */
final class GraphicsPresets {
    private static final String TAG = "wwhd";

    static final int LOWEST = 0, LOW = 1, MEDIUM = 2, HIGH = 3, CUSTOM = -1;
    static final int COUNT = 4;

    // per step, Lowest to High: rendering resolution, dynamic resolution's lowest scale (percent),
    // frame interpolation (0 = 30 fps)
    private static final String[] RES_FG = {"0.75", "1", "1.5", "2"};
    private static final int[] DYN_FG = {50, 60, 75, 100};
    private static final String[] RES = {"0.75", "1", "1", "1.5"};
    private static final int[] DYN = {50, 60, 60, 75};
    private static final int[] FPS = {0, 0, 60, 60};
    // 16x anisotropic filtering (texture bandwidth: the benchmark measures it) and the full-size
    // occlusion depth (one more depth draw; off on Lowest only). Widescreen is the player's choice: the
    // steps leave it, the benchmark measures with it.
    private static final boolean[] ANISO = {false, false, true, true};
    private static final boolean[] AO_HIRES = {false, true, true, true};

    // pref "preset_state": unset before the first start of a version with presets; "guess" = the
    // starting guess was applied; "existing" = an installation from before (settings kept)
    static final String STATE = "preset_state";

    private GraphicsPresets() {}

    /** the step the current settings match (with frame generation if it is on), or CUSTOM */
    static int current(MainActivity a) { return current(a.prefs, a.frameGenSwitchedOn() && a.hasFrameGenDll()); }

    static int current(SharedPreferences p, boolean fg) {
        String res = p.getString("res_scale", "1");
        int dyn = p.getInt("dyn_res_min", 0), fps = p.getInt("interp_fps", 0);
        boolean aniso = p.getBoolean("aniso", false), aoHires = p.getBoolean("ao_hires", true);
        for (int t = 0; t < COUNT; t++) {
            if (!res.equals(fg ? RES_FG[t] : RES[t]) || dyn != (fg ? DYN_FG[t] : DYN[t]) || fps != (fg ? 0 : FPS[t])) continue;
            if (aniso != ANISO[t] || aoHires != AO_HIRES[t]) continue;
            return t;
        }
        return CUSTOM;
    }

    /** sets step t while the game runs (through the settings' own setters) */
    static void apply(MainActivity a, int t) {
        boolean fg = a.frameGenSwitchedOn() && a.hasFrameGenDll();
        a.setResolution(indexOf(MainActivity.RES_SCALES, fg ? RES_FG[t] : RES[t]));
        a.setDynResMin(indexOf(MainActivity.DYN_RES_MINS, fg ? DYN_FG[t] : DYN[t]));
        a.setInterpFps(fg ? 0 : FPS[t]);
        a.setBool("aniso", ANISO[t]);
        a.setBool("ao_hires", AO_HIRES[t]);
    }

    /** step t has 16x anisotropic filtering */
    static boolean aniso(int t) { return ANISO[t]; }

    /** step t has the full-size occlusion depth */
    static boolean aoHires(int t) { return AO_HIRES[t]; }

    /** step t's rendering resolution, with frame generation or without */
    static String res(int t, boolean fg) { return fg ? RES_FG[t] : RES[t]; }

    /** step t's dynamic resolution minimum (scale) */
    static float dyn(int t, boolean fg) { return (fg ? DYN_FG[t] : DYN[t]) / 100f; }

    /** step t's frame interpolation (0 = 30 fps) */
    static int fps(int t, boolean fg) { return fg ? 0 : FPS[t]; }

    /** writes step t into the settings before the game starts (the renderer reads them when it starts) */
    static void write(SharedPreferences p, int t, boolean fg) {
        SharedPreferences.Editor e = p.edit().putString("res_scale", res(t, fg)).putInt("dyn_res_min", fg ? DYN_FG[t] : DYN[t])
                .putInt("interp_fps", fps(t, fg)).putBoolean("aniso", ANISO[t]).putBoolean("ao_hires", AO_HIRES[t]);
        e.commit();
    }

    /**
     * Once per installation, before the library loads: a new installation (no game files yet) starts
     * at the guess; an existing one keeps its settings.
     */
    static void firstStart(Context c, SharedPreferences p, File gameDir) {
        if (p.contains(STATE)) return;
        String gpu = glRenderer();
        int mhz = maxCpuMhz();
        long ramMb = ramMb(c);
        int t = guess(gpu, mhz, ramMb);
        boolean existing = gameDir.exists();
        Log.i(TAG, "graphics preset: starting guess " + t + " for GPU '" + gpu + "', CPU " + mhz + " MHz, RAM " + ramMb + " MB"
                + (existing ? " (existing installation: settings kept)" : ""));
        if (!existing) write(p, t, false);
        // the guess is kept for the menu's hint either way
        p.edit().putString(STATE, existing ? "existing" : "guess").putInt("preset_guess", t).commit();
    }

    /**
     * A rough step from the GPU's name (OpenGL ES renderer string), refined later by the benchmark.
     * From the devices tested: Adreno 732 holds 60 fps, Adreno 640 30 fps at 1x, Mali-G52 / G57
     * stay below 30 fps at 1x. The steps with 60 fps need a fast CPU core and RAM for it.
     */
    static int guess(String gpu, int cpuMhz, long ramMb) {
        String g = gpu.toLowerCase(Locale.ROOT);
        int n = number(g);
        int t;
        if (g.contains("adreno")) t = n >= 730 ? HIGH : n >= 650 ? MEDIUM : n >= 616 ? LOW : LOWEST;
        else if (g.contains("immortalis")) t = HIGH;
        else if (g.contains("mali")) t = n >= 710 ? MEDIUM : n >= 610 || n == 68 || n == 77 || n == 78 ? LOW : LOWEST;
        else if (g.contains("xclipse")) t = MEDIUM;
        else if (g.contains("powervr") || g.contains("img ")) t = LOWEST;
        else t = LOW;
        if (t >= MEDIUM && (cpuMhz > 0 && cpuMhz < 2400 || ramMb > 0 && ramMb < 5000)) t = LOW;
        return t;
    }

    // the model number in a GPU name: "adreno (tm) 640" 640, "mali-g52 mc2" 52, "mali-g710" 710
    private static int number(String g) {
        Matcher m = Pattern.compile("(?:adreno\\D*|mali-g|immortalis-g)(\\d+)").matcher(g);
        return m.find() ? Integer.parseInt(m.group(1)) : 0;
    }

    /** the GPU's name from a throwaway OpenGL ES context ("" if none): Vulkan isn't up before the game */
    static String glRenderer() {
        EGLDisplay d = EGL14.eglGetDisplay(EGL14.EGL_DEFAULT_DISPLAY);
        if (d == EGL14.EGL_NO_DISPLAY || !EGL14.eglInitialize(d, new int[1], 0, new int[1], 0)) return "";
        String name = "";
        EGLContext ctx = EGL14.EGL_NO_CONTEXT;
        EGLSurface surf = EGL14.EGL_NO_SURFACE;
        try {
            EGLConfig[] cfg = new EGLConfig[1];
            int[] num = new int[1];
            int[] attrs = {EGL14.EGL_RENDERABLE_TYPE, EGL14.EGL_OPENGL_ES2_BIT, EGL14.EGL_SURFACE_TYPE, EGL14.EGL_PBUFFER_BIT, EGL14.EGL_NONE};
            if (!EGL14.eglChooseConfig(d, attrs, 0, cfg, 0, 1, num, 0) || num[0] < 1) return "";
            ctx = EGL14.eglCreateContext(d, cfg[0], EGL14.EGL_NO_CONTEXT, new int[] {EGL14.EGL_CONTEXT_CLIENT_VERSION, 2, EGL14.EGL_NONE}, 0);
            surf = EGL14.eglCreatePbufferSurface(d, cfg[0], new int[] {EGL14.EGL_WIDTH, 1, EGL14.EGL_HEIGHT, 1, EGL14.EGL_NONE}, 0);
            if (ctx == EGL14.EGL_NO_CONTEXT || surf == EGL14.EGL_NO_SURFACE || !EGL14.eglMakeCurrent(d, surf, surf, ctx)) return "";
            String r = GLES20.glGetString(GLES20.GL_RENDERER);
            if (r != null) name = r;
        } finally {
            EGL14.eglMakeCurrent(d, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_CONTEXT);
            if (surf != EGL14.EGL_NO_SURFACE) EGL14.eglDestroySurface(d, surf);
            if (ctx != EGL14.EGL_NO_CONTEXT) EGL14.eglDestroyContext(d, ctx);
            EGL14.eglTerminate(d);
        }
        return name;
    }

    /** the fastest core's highest clock in MHz (0 if unreadable) */
    static int maxCpuMhz() {
        int best = 0;
        File[] cpus = new File("/sys/devices/system/cpu").listFiles((dir, n) -> n.matches("cpu\\d+"));
        if (cpus == null) return 0;
        for (File cpu : cpus) {
            try (BufferedReader r = new BufferedReader(new FileReader(new File(cpu, "cpufreq/cpuinfo_max_freq")))) {
                best = Math.max(best, Integer.parseInt(r.readLine().trim()) / 1000);
            } catch (Exception ignored) {
            }
        }
        return best;
    }

    static long ramMb(Context c) {
        ActivityManager am = (ActivityManager) c.getSystemService(Context.ACTIVITY_SERVICE);
        if (am == null) return 0;
        ActivityManager.MemoryInfo mi = new ActivityManager.MemoryInfo();
        am.getMemoryInfo(mi);
        return mi.totalMem >> 20;
    }

    private static int indexOf(String[] a, String v) {
        for (int i = 0; i < a.length; i++) if (a[i].equals(v)) return i;
        throw new IllegalArgumentException(v);
    }

    private static int indexOf(int[] a, int v) {
        for (int i = 0; i < a.length; i++) if (a[i] == v) return i;
        throw new IllegalArgumentException(String.valueOf(v));
    }
}
