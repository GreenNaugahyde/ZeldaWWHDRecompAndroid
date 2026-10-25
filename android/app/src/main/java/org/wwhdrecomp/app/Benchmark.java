package org.wwhdrecomp.app;

import android.content.SharedPreferences;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.util.Log;
import android.view.Gravity;
import android.view.View;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.TextView;

import java.util.ArrayDeque;
import java.util.Locale;

/**
 * The graphics benchmark: finds the preset step (GraphicsPresets) for this device by running the game
 * itself. The app restarts into a benchmark run: the game boots to its title screen (a play scene on the
 * sea, stage sea_T), the renderer measures the GPU's busy time and the busiest thread's CPU time per
 * frame at two rendering scales there and at Outset Island (a warp from the title, no save file
 * needed), and a check at the chosen settings confirms the frame rate. With Lossless.dll, frame
 * generation is tried last (on Mali GPUs it can hang the GPU: a crash in that stage is noticed at the
 * next start and the result without it is offered).
 *
 * While it runs: the game's picture stays under a card (unless the player asked to see it), no sound,
 * no save states of any kind, the settings themselves untouched (scales are set live). Afterwards the
 * app restarts with the result applied or the old settings kept.
 *
 * GPU time is taken as fixed + k * pixels (the shadow map, the GamePad picture and some fixed-size
 * passes don't shrink with the resolution), fitted from the two scales; each step's top resolution must
 * fit the frame time with 20% to spare (dynamic resolution covers heavier scenes and a warm device).
 */
final class Benchmark {
    private static final String TAG = "wwhd";

    // prefs: the next start runs the benchmark; with the frame generation stage; game picture shown
    static final String PENDING = "bench_pending", WITH_FG = "bench_fg", SHOW = "bench_show";
    static final String WIDE = "bench_wide";  // the player's widescreen choice (render_aspect 1): measured with it
    // the stage running (a crash there is found at the next start); the step found without frame generation
    static final String STAGE = "bench_stage", TIER = "bench_tier";
    static final String DONE = "bench_done";  // offered or run once (the start offer comes once)

    // the top resolution with 10% to spare: dynamic resolution (down to the step's minimum) covers the
    // heavier views and a warm device
    private static final float BUDGET_MS = 1000f / 30 * 0.9f;
    private static final float BUDGET_60_MS = 1000f / 60 * 0.9f;
    private static final float FG_GUESS_MS = 8;  // frame generation's network before its check measured it
    private static final int HEADINGS = 4;       // camera directions measured
    private static final int TURN_FRAMES = 9;    // game frames of the camera stick held: about a quarter turn (Outset pier)
    private static final float CPU_60_MS = 1000f / 60 * 0.9f;    // the busiest thread at 60 fps
    private static final int SPEED = 3;  // the game's clock while measuring: 90 fps asked, the GPU flat out
    private static final float[] SCALES = {1f, 1.5f};

    private final MainActivity a;
    private final SharedPreferences prefs;
    private final Handler h = new Handler(Looper.getMainLooper());
    private final ArrayDeque<Step> steps = new ArrayDeque<>();
    private Step step;
    private long stepStart;
    private int done, total;
    private View cover;
    private TextView title, detail;
    private ProgressBar bar;
    private SeaView sea;
    private final StringBuilder log = new StringBuilder();

    // per scene: GPU ms per frame at SCALES[0], SCALES[1]; busiest thread ms at SCALES[0]
    private final float[][] gpu = new float[HEADINGS][2];  // per camera direction: GPU ms at SCALES[0], SCALES[1]
    private final float[] cpu = new float[1];
    private float fgMs;
    private float anisoMs;  // anisotropic filtering's GPU time per frame at 1x
    private float last1x;   // the last direction's GPU time at 1x
    private int fgGood = -1;  // with frame generation: the step whose check passed (a step higher is tried once)
    private int tier = GraphicsPresets.LOWEST, fgTier = -1;

    private static Benchmark running;  // the run in this process (interrupted when the app leaves the screen)

    private Benchmark(MainActivity a) {
        this.a = a;
        prefs = a.prefs;
    }

    static boolean pending(SharedPreferences p) { return p.getBoolean(PENDING, false); }

    /** asks to run it (with the frame generation stage or not) and restarts into it */
    static void request(MainActivity a, boolean withFg, boolean show, boolean wide) {
        a.prefs.edit().putBoolean(PENDING, true).putBoolean(WITH_FG, withFg).putBoolean(SHOW, show).putBoolean(WIDE, wide).putBoolean(DONE, true)
                .remove(STAGE).commit();
        a.restartForBenchmark();
    }

    /** the game started in a benchmark run: covers it and runs the steps */
    static void run(MainActivity a, FrameLayout root) {
        running = new Benchmark(a);
        running.start(root);
    }

    /** the app left the screen (screen off, locked, another app): the numbers would be wrong, the run stops */
    static void interrupted() {
        Benchmark b = running;
        if (b != null && b.step != null) b.fail(b.a.getString(R.string.bench_interrupted));
    }

    // ------------------------------------------------------------------ steps
    private abstract static class Step {
        final int label;
        Step(int label) { this.label = label; }
        void begin() {}
        /** true when done; ms since begin */
        abstract boolean tick(long ms);
        /** the card's second line while it runs, or null: the live frame rate */
        String detail() { return null; }
    }

    private interface Cond { boolean ok(); }

    // the title screen with the shader cache loaded. A fresh installation first turns the head start's shaders into GPU
    // code and builds its pipelines (minutes on slower phones, more with older Adreno drivers that build one at a
    // time): the wait goes on as long as that work moves (work_done), up to BOOT_MAX_MS; it fails after
    // BOOT_STALL_MS without any. (The very first start, without a save, goes to the file select with a message instead
    // of the title: the offer waits for a save, MainActivity.checkAndStart.)
    private static final long BOOT_STALL_MS = 60_000, BOOT_MAX_MS = 15 * 60_000;

    private Step bootWait() {
        return new Step(R.string.bench_step_boot) {
            int work = -1;
            long movedAt;
            long logged;
            boolean tick(long ms) {
                int front = Native.frontScreen();
                boolean title = front == 1, cache = Native.cacheProgress()[0] == 2;
                if (title && cache) return true;
                int w = Native.getOption("work_done");
                if (w != work) {
                    work = w;
                    movedAt = ms;
                }
                if (ms - logged >= 10_000) {
                    logged = ms;
                    Log.i(TAG, "[bench] waiting at boot " + ms / 1000 + " s: front screen " + front + ", title " + title + ", cache " + cache + ", shaders and pipelines done " + w);
                }
                if (ms - movedAt > BOOT_STALL_MS || ms > BOOT_MAX_MS) fail("the title screen");
                return false;
            }
            String detail() { return work > 0 ? a.getString(R.string.bench_shaders, work) : null; }
        };
    }

    private Step waitFor(int label, Cond c, long timeoutMs, String what) {
        return new Step(label) {
            boolean tick(long ms) {
                if (c.ok()) return true;
                if (ms > timeoutMs) fail(what);
                return false;
            }
        };
    }

    /** waits for `frames` game frames (the camera turns per game frame, whatever the frame rate) */
    private Step holdFrames(int label, int frames) {
        return new Step(label) {
            float from = -1;
            boolean tick(long ms) {
                float now = Native.getOption("bench_frames");  // (frames shown: game frames, no interpolation here)
                if (from < 0) from = now;
                if (ms > 20_000) fail("the camera turn");
                return now - from >= frames;
            }
        };
    }

    private Step hold(int label, long holdMs) {
        return new Step(label) {
            boolean tick(long ms) { return ms >= holdMs; }
        };
    }

    private interface Result { void got(float[] s); }

    /** settles 1.5 s at the current settings, then takes the numbers of `ms`; `fast`: with the game's
     *  clock sped up (the frame rate then shows what the device can do, up to the display's rate) */
    private Step measure(int label, long ms, boolean fast, Result r) { return measure(label, 1500, ms, fast, r); }

    private Step measure(int label, long settleMs, long ms, boolean fast, Result r) {
        return new Step(label) {
            long from = -1;  // the window's start (ms since begin), -1 while settling
            int redo;
            void begin() {
                Native.setOption("benchmark_quiet", 1);  // (no background compiles while measuring)
                if (fast && !prefs.getBoolean("bench_slow", false)) Native.setOption("benchmark_speed", SPEED);
            }
            boolean tick(long t) {
                if (from < 0) {
                    if (t < settleMs) return false;
                    Native.benchmarkSample();  // (starts the window)
                    from = t;
                    return false;
                }
                if (t < from + ms) return false;
                float[] s = Native.benchmarkSample();
                // draws left out while their pipeline compiled make frames lighter: the window again
                if (s[5] > 0 && redo < 2) {
                    redo++;
                    note(String.format(Locale.ROOT, "%.0f draws left out while compiling: measured again", s[5]));
                    from = t;
                    return false;
                }
                if (fast) Native.setOption("benchmark_speed", 1);
                Native.setOption("benchmark_quiet", 0);
                r.got(s);
                return true;
            }
        };
    }

    private Step action(int label, Runnable run) {
        return new Step(label) {
            void begin() { run.run(); }
            boolean tick(long ms) { return true; }
        };
    }

    private void start(FrameLayout root) {
        Log.i(TAG, "[bench] start (frame generation stage " + prefs.getBoolean(WITH_FG, false) + ")");
        Native.setOption("benchmark", 1);
        Native.setOption("auto_state", 0);  // (the setting itself stays)
        a.setBenchmarkControls(true);
        showCover(root);
        // a clean start: no interpolation, no dynamic resolution, no frame generation, no anisotropic
        // filtering; the widescreen choice
        Native.setOption("interp_fps", 0);
        Native.setOption("dyn_res_min", 0);
        Native.setOption("aniso", 0);
        Native.setOption("ao_hires", 1);
        Native.setOption("render_aspect", prefs.getBoolean(WIDE, false) ? 1 : 0);
        a.applyFrameGenBenchmark(false);
        // straight on from the title: left alone for ~15 s it plays the story's intro (another scene)
        steps.add(bootWait());
        // Outset Island, the end of the pier: with the camera turning around Link, the open sea one way and
        // the village the other. (One place: a second scene change after the one from the title doesn't
        // load, see warp.cpp warp_title_draw.)
        addWarp(44, 0, "Outset Island");
        addScene(0, R.string.bench_step_island);
        steps.add(action(R.string.bench_step_check, this::decide));
        // (the check steps are added by decide())
        total = steps.size() + 4;
        next();
    }

    // a warp to a room of the sea stage, then a few seconds for its pipelines and the GPU's clocks
    private void addWarp(int room, int spawn, String what) {
        steps.add(action(R.string.bench_step_warp, () -> {
            if (!Native.warp("sea", room, spawn, -1)) fail(what + " (warp refused)");
        }));
        steps.add(waitFor(R.string.bench_step_warp, () -> Native.warpStatus()[0] == 3, 45_000, what));
        steps.add(hold(R.string.bench_step_warm, 3000));
        // one full turn of the camera first, unmeasured: the place's pipelines get built for every direction
        steps.add(action(R.string.bench_step_warm, () -> a.setBenchmarkCamera(true)));
        steps.add(holdFrames(R.string.bench_step_warm, 4 * TURN_FRAMES + 6));
        steps.add(action(R.string.bench_step_warm, () -> a.setBenchmarkCamera(false)));
        steps.add(hold(R.string.bench_step_warm, 3000));
    }

    // the place from HEADINGS camera directions (the camera turns between them, then holds still): at each,
    // both scales sped up; at the last also 1x with anisotropic filtering. The directions are averaged.
    private void addScene(int scene, int label) {
        for (int h = 0; h < HEADINGS; h++) {
            if (h > 0) {
                steps.add(action(label, () -> a.setBenchmarkCamera(true)));
                steps.add(holdFrames(label, TURN_FRAMES));
                steps.add(action(label, () -> a.setBenchmarkCamera(false)));
            }
            for (int i = 0; i < SCALES.length; i++) {
                final int k = i, heading = h;
                steps.add(action(label, () -> a.setRenderScaleLive(SCALES[k])));
                steps.add(measure(label, 2000, true, s -> {
                    gpu[heading][k] = s[2];
                    if (k == 0) last1x = s[2];
                    float fps = s[1] > 0 ? s[0] / s[1] : 0;
                    // the CPU's limit: the busiest thread (Android's performance hint session), else the frame
                    // time when the GPU wasn't the limit and the display's rate wasn't reached
                    float cpuMs = s[3];
                    if (cpuMs <= 0 && fps > 0 && fps < a.maxRefreshHz() * 0.95f && s[2] < 1000f / fps * 0.85f) cpuMs = 1000f / fps;
                    cpu[scene] += cpuMs / HEADINGS;
                    note(String.format(Locale.ROOT, "scene %d direction %d %.1fx: %.1f fps, GPU %.1f ms, CPU %.1f ms, worst %.0f ms", scene,
                            heading, SCALES[k], fps, s[2], s[3], s[4]));
                }));
            }
        }
        // anisotropic filtering: 1x with it on, against 1x without in the same direction (texture
        // bandwidth: scales with the pixels)
        steps.add(action(label, () -> {
            a.setRenderScaleLive(SCALES[0]);
            Native.setOption("aniso", 1);
        }));
        steps.add(measure(label, 2000, true, s -> {
            Native.setOption("aniso", 0);
            anisoMs = Math.max(0, s[2] - last1x);
            note(String.format(Locale.ROOT, "scene %d 1x anisotropic: GPU %.1f ms (+%.1f)", scene, s[2], s[2] - last1x));
        }));
    }

    // GPU ms per frame in camera direction h at `scale`, from fixed + k * scale^2
    private float at(int h, float scale) {
        float[] g = gpu[h];
        float s0 = SCALES[0] * SCALES[0], s1 = SCALES[1] * SCALES[1];
        float k = Math.max(0, (g[1] - g[0]) / (s1 - s0)), fixed = Math.max(0, g[0] - k * s0);
        return fixed + k * scale * scale;
    }

    private float worst(float scale) {
        float v = 0;
        for (int h = 0; h < HEADINGS; h++) v = Math.max(v, at(h, scale));
        return v;
    }

    private float lightest(float scale) {
        float v = Float.MAX_VALUE;
        for (int h = 0; h < HEADINGS; h++) v = Math.min(v, at(h, scale));
        return v;
    }

    /**
     * Step t fits as dynamic resolution runs it: at its minimum the heaviest direction keeps the frame
     * rate with 10% to spare (no stutter), and its top resolution fits the frame in the lightest
     * direction (it is reached). `extraMs`: frame generation's network per frame.
     */
    private boolean fits(int t, boolean fg, float extraMs) {
        boolean sixty = GraphicsPresets.fps(t, fg) > 0;
        float frame = sixty ? 1000f / 60 : 1000f / 30, budget = sixty ? BUDGET_60_MS : BUDGET_MS;
        float top = Float.parseFloat(GraphicsPresets.res(t, fg)), floor = Math.min(top, GraphicsPresets.dyn(t, fg));
        float an = GraphicsPresets.aniso(t) ? Math.max(0, anisoMs) : 0;
        boolean ok = worst(floor) + an * floor * floor + extraMs <= budget && lightest(top) + an * top * top + extraMs <= frame;
        float cpuMs = 0;
        for (float v : cpu) cpuMs = Math.max(cpuMs, v);
        return ok && cpuMs <= (sixty ? CPU_60_MS : BUDGET_MS);
    }

    private void decide() {
        // without frame generation: the highest step that fits
        tier = GraphicsPresets.LOWEST;
        for (int t = GraphicsPresets.COUNT - 1; t > 0; t--)
            if (fits(t, false, 0)) {
                tier = t;
                break;
            }
        note(String.format(Locale.ROOT, "GPU ms heaviest / lightest direction: 0.75x %.1f / %.1f, 1x %.1f / %.1f, 1.5x %.1f / %.1f, 2x %.1f / %.1f "
                + "(anisotropic +%.1f at 1x) -> step %d", worst(0.75f), lightest(0.75f), worst(1f), lightest(1f), worst(1.5f), lightest(1.5f),
                worst(2f), lightest(2f), anisoMs, tier));
        steps.addFirst(check(false, 2));
    }

    /** runs step `tier` (or fgTier) for 6 s (at the sea, the last place); a miss tries the step below (`retries` times) */
    private Step check(boolean fg, int retries) {
        final int t = fg ? fgTier : tier;
        Step apply = action(R.string.bench_step_check, () -> {
            a.setRenderScaleLive(Float.parseFloat(GraphicsPresets.res(t, fg)));
            // as the step runs: dynamic resolution down to its minimum (it settles before the numbers)
            Native.setOption("dyn_res_min", Math.round(GraphicsPresets.dyn(t, fg) * 100));
            Native.setOption("interp_fps", GraphicsPresets.fps(t, fg));
            Native.setOption("aniso", GraphicsPresets.aniso(t) ? 1 : 0);
            Native.setOption("ao_hires", GraphicsPresets.aoHires(t) ? 1 : 0);
            if (fg) a.applyFrameGenBenchmark(true);
        });
        // (8 s first: dynamic resolution finds its level)
        steps.addFirst(measure(R.string.bench_step_check, 8000, 6000, false, s -> {
            float shown = s[1] > 0 ? s[0] / s[1] : 0;
            float[] perf = Native.perfStats();
            float target = fg ? 30 : GraphicsPresets.fps(t, false) > 0 ? 60 : 30;
            // (with frame generation the game's own frames: the generated ones come on top)
            boolean ok = shown >= target * 0.93f;
            if (fg) fgMs = perf[4];
            note(String.format(Locale.ROOT, "check step %d%s: %.1f fps shown, game %.1f, GPU %.1f ms, FG %.1f ms, scale %.2fx -> %s", t,
                    fg ? " with FG" : "", shown, perf[0], s[2], perf[4], perf[7], ok ? "ok" : "too slow"));
            if (ok && fg) {
                // the network's measured cost: room for a step higher? (tried once)
                if (fgGood < 0 && t + 1 < GraphicsPresets.COUNT && fits(t + 1, true, fgMs)) {
                    fgGood = t;
                    fgTier = t + 1;
                    steps.addFirst(check(true, 0));
                } else {
                    finish();
                }
                return;
            }
            if (ok) {
                afterCheck();
                return;
            }
            if (fg && fgGood >= 0) {  // the step higher didn't hold: the one that did
                fgTier = fgGood;
                finish();
                return;
            }
            if (t > GraphicsPresets.LOWEST && retries > 0) {
                if (fg) fgTier = t - 1;
                else tier = t - 1;
                steps.addFirst(check(fg, retries - 1));
            } else if (fg) {
                fgTier = -1;
                finish();
            } else {
                tier = GraphicsPresets.LOWEST;
                afterCheck();
            }
        }));
        return apply;
    }

    private void afterCheck() {
        prefs.edit().putInt(TIER, tier).commit();
        if (!prefs.getBoolean(WITH_FG, false) || !a.hasFrameGenDll()) {
            finish();
            return;
        }
        // frame generation: 30 fps from the game, so the step from the 30 fps budget with ~6 ms for the network
        fgTier = GraphicsPresets.LOWEST;
        for (int t = GraphicsPresets.COUNT - 1; t > 0; t--)
            if (fits(t, true, FG_GUESS_MS)) {
                fgTier = t;
                break;
            }
        prefs.edit().putString(STAGE, "fg").commit();  // a crash or hang from here on: no frame generation
        steps.addFirst(check(true, 1));
    }

    private void next() {
        step = steps.poll();
        if (step == null) return;
        done++;
        stepStart = SystemClock.uptimeMillis();
        title.setText(step.label);
        bar.setProgress(Math.min(1000, done * 1000 / Math.max(1, total)));
        if (sea != null) sea.setProgress(Math.min(1f, done / (float) Math.max(1, total)));
        step.begin();
        h.post(this::tick);
    }

    private void tick() {
        if (step == null) return;
        long ms = SystemClock.uptimeMillis() - stepStart;
        float[] p = Native.perfStats();
        String own = step.detail();
        detail.setText(own != null ? own : a.getString(R.string.bench_live, p[0], p[7], Math.round(p[8]), Math.round(p[9])));
        if (step.tick(ms)) {
            next();
            return;
        }
        h.postDelayed(this::tick, 250);
    }

    private void note(String s) {
        Log.i(TAG, "[bench] " + s);
        log.append(s).append('\n');
    }

    private void fail(String what) {
        note("failed: " + what);
        step = null;
        steps.clear();
        end();
        title.setText(R.string.bench_failed_title);
        detail.setText(a.getString(R.string.bench_failed_text, what));
        buttons(new int[] {R.string.bench_close, R.string.bench_again}, new Runnable[] {this::closeKeep,
                () -> request(a, prefs.getBoolean(WITH_FG, false), prefs.getBoolean(SHOW, false), prefs.getBoolean(WIDE, false))});
    }

    private void finish() {
        step = null;
        steps.clear();
        end();
        boolean fg = fgTier >= 0;
        int t = fg ? fgTier : tier;
        String[] names = a.getResources().getStringArray(R.array.preset_steps);
        String what = a.getString(fg ? R.string.bench_result_fg : GraphicsPresets.fps(t, false) > 0 ? R.string.bench_result_60
                : R.string.bench_result_30, names[t], GraphicsPresets.res(t, fg));
        if (prefs.getBoolean(WITH_FG, false) && a.hasFrameGenDll() && !fg) what += "\n\n" + a.getString(R.string.bench_fg_no);
        note("result: step " + t + (fg ? " with frame generation" : ""));
        title.setText(R.string.bench_result_title);
        detail.setText(what);
        bar.setProgress(1000);
        if (sea != null) sea.setProgress(1);
        table(rows(a, t, fg));
        buttons(new int[] {R.string.bench_keep, R.string.bench_apply}, new Runnable[] {this::closeKeep, () -> {
            applyResult(prefs, t, fg);
            close();
        }});
    }

    /** writes the result into the settings (read when the game starts again) */
    private static void applyResult(SharedPreferences p, int t, boolean fg) {
        GraphicsPresets.write(p, t, fg);
        SharedPreferences.Editor e = p.edit().putBoolean("fg_enabled", fg).putInt("render_aspect", p.getBoolean(WIDE, false) ? 1 : 0);
        if (fg) e.putInt("fg_multiplier", 2);  // (the multiplier the check ran)
        e.commit();
    }

    /**
     * Every graphics setting of the result as {label, value, the current value where it differs (else
     * null)}: the ones the step sets, the widescreen choice, and the ones it leaves as they are.
     */
    static java.util.List<String[]> rows(MainActivity a, int t, boolean fg) {
        SharedPreferences p = a.prefs;
        java.util.List<String[]> r = new java.util.ArrayList<>();
        String on = a.getString(R.string.opt_on), off = a.getString(R.string.opt_off);
        boolean fgNow = p.getBoolean("fg_enabled", false) && a.hasFrameGenDll();
        row(r, a.getString(R.string.opt_framegen), fg ? on + ", ×2" : off, fgNow ? on + ", ×" + p.getInt("fg_multiplier", 2) : off);
        row(r, a.getString(R.string.opt_interp), fpsLabel(a, GraphicsPresets.fps(t, fg)), fpsLabel(a, p.getInt("interp_fps", 0)));
        row(r, a.getString(R.string.opt_resolution), a.resolutionLabel(GraphicsPresets.res(t, fg)), a.resolutionLabel(p.getString("res_scale", "1")));
        row(r, a.getString(R.string.opt_dyn_res), dynLabel(a, Math.round(GraphicsPresets.dyn(t, fg) * 100)), dynLabel(a, p.getInt("dyn_res_min", 0)));
        String[] aspects = a.getResources().getStringArray(R.array.render_aspect_modes);
        row(r, a.getString(R.string.opt_render_aspect), aspects[p.getBoolean(WIDE, false) ? 1 : 0], aspects[p.getInt("render_aspect", 0) == 1 ? 1 : 0]);
        String[] ao = a.getResources().getStringArray(R.array.ao_modes);
        String aoNow = ao[Math.max(0, Math.min(ao.length - 1, p.getInt("ao_mode", Native.getOption("ao_mode"))))];
        row(r, a.getString(R.string.opt_ao), aoNow, aoNow);
        row(r, a.getString(R.string.opt_ao_hires), GraphicsPresets.aoHires(t) ? on : off, p.getBoolean("ao_hires", true) ? on : off);
        row(r, a.getString(R.string.opt_aniso), GraphicsPresets.aniso(t) ? on : off, p.getBoolean("aniso", false) ? on : off);
        String[] disp = a.getResources().getStringArray(R.array.display_res_modes);
        String dispNow = disp[Math.max(0, Math.min(disp.length - 1, p.getInt("display_res", MainActivity.DISPLAY_RES_AUTO)))];
        row(r, a.getString(R.string.opt_display_res), dispNow, dispNow);
        return r;
    }

    private static void row(java.util.List<String[]> r, String label, String value, String now) {
        r.add(new String[] {label, value, value.equals(now) ? null : now});
    }

    private static String fpsLabel(MainActivity a, int fps) {
        return fps > 0 ? a.getString(R.string.interp_fps, fps) : a.getString(R.string.interp_off);
    }

    private static String dynLabel(MainActivity a, int min) {
        return min <= 0 ? a.getString(R.string.opt_off) : a.getString(R.string.dyn_res_down_to, String.valueOf(min / 100f), Math.round(7.2f * min) + "p");
    }

    // the rows as a table in the card: label, value, and in a lighter colour what is set now where it differs
    private void table(java.util.List<String[]> rows) {
        LinearLayout box = (LinearLayout) detail.getParent();
        LinearLayout t = new LinearLayout(a);
        t.setOrientation(LinearLayout.VERTICAL);
        t.setPadding(0, GameUi.px(a, 8), 0, GameUi.px(a, 4));
        for (String[] r : rows) {
            LinearLayout line = new LinearLayout(a);
            TextView l = new TextView(a), v = new TextView(a);
            l.setText(r[0]);
            l.setTextColor(0xFF4A2C12);
            l.setTextSize(13);
            android.text.SpannableStringBuilder sb = new android.text.SpannableStringBuilder(r[1]);
            sb.setSpan(new android.text.style.StyleSpan(android.graphics.Typeface.BOLD), 0, r[1].length(), 0);
            if (r[2] != null) {
                int from = sb.length();
                sb.append("  (").append(a.getString(R.string.bench_now, r[2])).append(')');
                sb.setSpan(new android.text.style.ForegroundColorSpan(0xFF8A7058), from, sb.length(), 0);
            }
            v.setText(sb);
            v.setTextColor(0xFF4A2C12);
            v.setTextSize(13);
            line.addView(l, new LinearLayout.LayoutParams(0, -2, 0.42f));
            line.addView(v, new LinearLayout.LayoutParams(0, -2, 0.58f));
            t.addView(line);
        }
        box.addView(t);
    }

    // the numbers stop; the game stays covered until the restart
    private void end() {
        prefs.edit().remove(STAGE).putBoolean(PENDING, false).commit();
        Native.setOption("benchmark", 0);
        Native.setOption("benchmark_quiet", 0);
        a.setBenchmarkCamera(false);
        a.applyFrameGenBenchmark(false);
        Native.setPaused(true);
    }

    private void closeKeep() { close(); }

    private void close() { a.restartAfterBenchmark(); }

    // ------------------------------------------------------------------ the cover card
    private void showCover(FrameLayout root) {
        boolean show = prefs.getBoolean(SHOW, false);
        LinearLayout box = new LinearLayout(a);
        box.setOrientation(LinearLayout.VERTICAL);
        int pad = GameUi.px(a, 24);
        box.setPadding(pad, pad, pad, pad);
        title = new TextView(a);
        title.setTextSize(18);
        box.addView(title);
        bar = new ProgressBar(a, null, android.R.attr.progressBarStyleHorizontal);
        bar.setMax(1000);
        box.addView(bar);
        detail = new TextView(a);
        detail.setTextSize(14);
        box.addView(detail);
        if (show) {
            // a small card at the top: the game stays visible
            android.graphics.drawable.GradientDrawable card = new android.graphics.drawable.GradientDrawable();
            card.setColor(0xE6F6EBCB);
            card.setCornerRadius(GameUi.px(a, 14));
            box.setBackground(card);
            for (TextView t : new TextView[] {title, detail}) t.setTextColor(0xFF4A2C12);
            FrameLayout.LayoutParams lp = new FrameLayout.LayoutParams(Math.min(GameUi.px(a, 520),
                    a.getResources().getDisplayMetrics().widthPixels - GameUi.px(a, 32)), -2, Gravity.TOP | Gravity.CENTER_HORIZONTAL);
            lp.topMargin = GameUi.px(a, 12);
            lp.bottomMargin = GameUi.px(a, 12);
            android.widget.ScrollView sv = new android.widget.ScrollView(a);  // (the result's table on a low screen)
            sv.addView(box);
            cover = sv;
            root.addView(cover, lp);
        } else {
            cover = a.coverScreen(box);
            sea = a.coverSea();
            sea.setFrameInterval(250);  // (the measurement: next to nothing for the sea)
            // over the whole window, the strip beside a camera cutout too (the game's view leaves it out)
            ((FrameLayout) a.getWindow().getDecorView()).addView(cover, new FrameLayout.LayoutParams(-1, -1));
        }
    }

    private void buttons(int[] labels, Runnable[] runs) {
        LinearLayout row = new LinearLayout(a);
        row.setGravity(Gravity.END);
        for (int i = 0; i < labels.length; i++) {
            android.widget.Button b = new android.widget.Button(a);
            b.setText(labels[i]);
            b.setBackgroundTintList(android.content.res.ColorStateList.valueOf(0xFF8B5A2B));
            b.setTextColor(0xFFFFF6E0);
            final Runnable r = runs[i];
            b.setOnClickListener(v -> r.run());
            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(-2, -2);
            lp.leftMargin = GameUi.px(a, 8);
            row.addView(b, lp);
        }
        // on the hidden game's sea card: in the paper's bottom right corner, below the scrolling part (the result's
        // table is taller than a phone held sideways, and buttons at the end of the scroll were out of sight)
        LinearLayout box = (LinearLayout) detail.getParent();
        if (box.getParent() instanceof android.widget.ScrollView && box.getParent().getParent() instanceof LinearLayout) {
            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(-1, -2);
            lp.topMargin = GameUi.px(a, 8);
            ((LinearLayout) box.getParent().getParent()).addView(row, lp);  // (MainActivity.seaScreen's card)
        } else {
            box.addView(row);
        }
    }

    // ------------------------------------------------------------------ a crash in the frame generation stage
    /** at a start: the last run ended in the frame generation stage; true if this takes over the screen */
    static boolean afterCrash(MainActivity a) {
        SharedPreferences p = a.prefs;
        if (!"fg".equals(p.getString(STAGE, ""))) return false;
        p.edit().remove(STAGE).putBoolean(PENDING, false).commit();
        int t = p.getInt(TIER, GraphicsPresets.LOWEST);
        String[] names = a.getResources().getStringArray(R.array.preset_steps);
        String what = a.getString(GraphicsPresets.fps(t, false) > 0 ? R.string.bench_result_60 : R.string.bench_result_30, names[t],
                GraphicsPresets.res(t, false));
        StringBuilder all = new StringBuilder(a.getString(R.string.bench_fg_crashed)).append("\n\n").append(what).append('\n');
        for (String[] r : rows(a, t, false)) {
            all.append('\n').append(r[0]).append(": ").append(r[1]);
            if (r[2] != null) all.append("  (").append(a.getString(R.string.bench_now, r[2])).append(')');
        }
        new GameDialog(a).title(R.string.bench_result_title).message(all.toString())
                .cancelable(false)
                .button(R.string.bench_keep, a::continueStart)
                .button(R.string.bench_apply, () -> {
                    applyResult(p, t, false);
                    a.continueStart();
                }).show();
        return true;
    }

    // ------------------------------------------------------------------ the offer
    /** the start screen: what it does, frame generation, show the game; `later` runs on Not now */
    static void offer(MainActivity a, Runnable later) {
        SharedPreferences p = a.prefs;
        boolean hasDll = a.hasFrameGenDll();
        boolean[] fg = {hasDll && p.getBoolean("fg_enabled", true)}, show = {false}, wide = {p.getInt("render_aspect", 0) == 1};
        GameDialog d = new GameDialog(a).title(R.string.bench_offer_title)
                .message(a.getString(R.string.bench_offer_text) + (hasDll ? "" : "\n\n" + a.getString(R.string.bench_offer_dll)))
                .cancelable(false)
                .toggle(a.getString(R.string.bench_wide), wide[0], on -> wide[0] = on)
                .toggle(a.getString(R.string.bench_show_game), false, on -> show[0] = on);
        if (hasDll) d.toggle(a.getString(R.string.bench_with_fg), fg[0], on -> fg[0] = on);
        else d.button(R.string.bench_choose_dll, () -> a.pickDllThen(() -> offer(a, later)));
        d.button(R.string.bench_not_now, () -> {
            p.edit().putBoolean(DONE, true).commit();
            if (later != null) later.run();
        });
        d.button(R.string.bench_start, () -> {
            if (fg[0] && GraphicsPresets.glRenderer().toLowerCase(Locale.ROOT).contains("mali") && !p.getBoolean("fg_risk_ok", false)) {
                // frame generation can hang Mali GPUs: the same warning as in the menu
                new GameDialog(a).title(R.string.fg_risk_title).message(a.getString(R.string.fg_risk_text, GraphicsPresets.glRenderer()))
                        .button(R.string.fg_risk_keep_off, () -> request(a, false, show[0], wide[0]))
                        .button(R.string.fg_risk_accept, () -> {
                            p.edit().putBoolean("fg_risk_ok", true).commit();
                            request(a, true, show[0], wide[0]);
                        }).show();
                return;
            }
            request(a, fg[0], show[0], wide[0]);
        });
        d.show();
    }
}
