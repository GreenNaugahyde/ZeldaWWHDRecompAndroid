package org.wwhdrecomp.app;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Intent;
import android.content.SharedPreferences;
import android.graphics.RectF;
import android.os.Bundle;
import android.system.ErrnoException;
import android.system.Os;
import android.text.InputFilter;
import android.text.InputType;
import android.util.Log;
import android.view.Gravity;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;
import android.widget.Button;
import android.widget.EditText;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileReader;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.ArrayList;
import java.util.List;

/**
 * The game's window: a SurfaceView the native renderer presents into (TV and GamePad images
 * composed as the layout setting says), with the on-screen GamePad on top.
 *
 * Game files: the extracted game (code/, content/, meta/ as produced by tools/wudextract.py) goes
 * to Android/data/org.wwhdrecomp.app/files/game, e.g. with `adb push game /sdcard/Android/data/org.wwhdrecomp.app/files/`.
 * Debugging: WWHD_* environment variables (see README) can be set in files/wwhd.env (KEY=VALUE per
 * line) or as intent extras (`adb shell am start -n org.wwhdrecomp.app/.MainActivity --es WWHD_LOG_FRAME 100`).
 */
public final class MainActivity extends Activity implements SurfaceHolder.Callback, ControlsView.Listener {
    private static final String TAG = "wwhd";
    static volatile MainActivity instance;
    private static boolean libraryLoaded, started;

    // The redesigned touch controls (style "Flexible", context buttons, layout editor, HUD fades) and the
    // screen layout "TV, GamePad on demand" are unfinished: hidden from the options, and saved settings
    // of them are ignored, until this is true.
    static final boolean NEW_TOUCH_CONTROLS = false;

    // screen layouts
    static final int LAYOUT_INSET = 0, LAYOUT_SIDE = 1, LAYOUT_TV = 2, LAYOUT_DRC_LARGE = 3, LAYOUT_ON_DEMAND = 4;
    // "TV, GamePad on demand": the GamePad picture is shown instead of the TV picture (screen button)
    private boolean drcShown;
    private static final float TV_ASPECT = 16f / 9f, DRC_ASPECT = 854f / 480f;

    SharedPreferences prefs;
    // held here: SharedPreferences keeps its listeners only weakly
    private final SharedPreferences.OnSharedPreferenceChangeListener crashInfoUpdater = (p, key) -> CrashLogs.updateInfo(this, p);
    private SurfaceView surface;
    private ControlsView controls;
    private final InputMapper mapper = new InputMapper();
    private boolean autoHidden;
    private int surfaceW, surfaceH;

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        instance = this;
        prefs = getSharedPreferences("settings", MODE_PRIVATE);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        getWindow().getAttributes().layoutInDisplayCutoutMode = WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
        if (!libraryLoaded) {
            // static initializers in the library read these, so they must be set before it loads
            setenv("WWHD_RES_SCALE", prefs.getString("res_scale", "1"));  // wwhd.env / intent extras below override
            setenv("WWHD_LANGUAGE", gameLanguage());
            applyFrameGenSettings();
            setenv("WWHD_APP_VERSION", CrashLogs.appVersion(this));  // the crash log's name
            // command recording thread: automatic (the renderer decides), on or off (wwhd.env still wins)
            int recordThread = prefs.getInt("record_thread", 0);
            if (recordThread != 0) setenv("WWHD_RECORD_THREAD", recordThread == 1 ? "1" : "0");
            // frame interpolation (Graphics > Frame rate): 0 off, 60 or 120 fps
            int interp = prefs.getInt("interp_fps", 0);
            if (interp != 0) setenv("WWHD_INTERP_FPS", String.valueOf(interp));
            applyEnvironment();
            CrashLogs.deleteShared(this, prefs);  // shared in an earlier run
            // a shader dump (WWHD_DUMP_SHADERS=<files>/shaders, debugging) left from an earlier start
            if (Os.getenv("WWHD_DUMP_SHADERS") == null) Backup.deleteTree(new File(baseDir(), "shaders"));
            System.loadLibrary("wwhd");
            libraryLoaded = true;
        }
        // debug: WWHD_DUMP_UI=1 writes every 2D pack texture to captures/ui (to look at them on a PC)
        if (Os.getenv("WWHD_DUMP_UI") != null) {
            File out = new File(baseDir(), "captures/ui");
            out.mkdirs();
            new Thread(() -> Native.dumpUiTextures(gameDir(), out.getAbsolutePath())).start();
        }
        // the crash log's app section, kept current while settings change
        CrashLogs.updateInfo(this, prefs);
        prefs.registerOnSharedPreferenceChangeListener(crashInfoUpdater);
        if (started) showGame();
        else checkAndStart();
    }

    // ------------------------------------------------------------------ game language
    // The console language the game sees (runtime: UCReadSysConfig), from the release's languages;
    // by default the device's, else English. Applies at the next start.
    static final String[] LANGUAGES = {"en", "fr", "de", "it", "es", "ja"};
    static final String[] LANGUAGE_NAMES = {"English", "Français", "Deutsch", "Italiano", "Español", "日本語"};

    /** the release's languages (indexes into LANGUAGES): USA English, French, Spanish; EUR the first
     *  five; JPN Japanese */
    int[] gameLanguages() {
        String r = Native.gameRelease(gameDir());
        if ("EUR".equals(r)) return new int[] {0, 1, 2, 3, 4};
        if ("JPN".equals(r)) return new int[] {5};
        return new int[] {0, 1, 4};
    }

    String gameLanguage() {
        String l = prefs.getString("language", "");
        if (l.isEmpty()) l = java.util.Locale.getDefault().getLanguage();
        for (String s : LANGUAGES)
            if (s.equals(l)) return l;
        return "en";
    }

    void setGameLanguage(String l) {
        prefs.edit().putString("language", l).commit();
    }

    // the command recording thread applies from the next start
    void setRecordThread(int mode) {
        if (mode == prefs.getInt("record_thread", 0)) return;
        prefs.edit().putInt("record_thread", mode).commit();
        new GameDialog(this).title(R.string.opt_record_thread).message(R.string.record_thread_restart)
                .button(R.string.gpu_driver_later, null)
                .button(R.string.res_restart_now, this::restartApp).show();
    }

    /** after the options menu closed with another language than it opened with */
    void askRestartForLanguage() {
        new GameDialog(this).title(R.string.opt_language).message(R.string.language_restart)
                .button(R.string.gpu_driver_later, null)
                .button(R.string.res_restart_now, this::restartApp).show();
    }

    File baseDir() {
        File f = getExternalFilesDir(null);
        return f != null ? f : getFilesDir();
    }

    // testing hooks (launch extras) only in debuggable builds: in a shared APK other apps could pass them
    boolean debuggable() { return (getApplicationInfo().flags & android.content.pm.ApplicationInfo.FLAG_DEBUGGABLE) != 0; }

    String gameDir() {
        String extra = debuggable() ? getIntent().getStringExtra("gameDir") : null;
        if (extra != null) return extra;
        return new File(baseDir(), "game").getAbsolutePath();
    }

    private void applyEnvironment() {
        File env = new File(baseDir(), "wwhd.env");
        if (env.exists()) {
            try (BufferedReader r = new BufferedReader(new FileReader(env))) {
                for (String line; (line = r.readLine()) != null; ) {
                    line = line.trim();
                    int eq = line.indexOf('=');
                    if (line.isEmpty() || line.startsWith("#") || eq <= 0) continue;
                    setenv(line.substring(0, eq).trim(), line.substring(eq + 1).trim());
                }
            } catch (IOException e) {
                Log.w(TAG, "cannot read " + env, e);
            }
        }
        Bundle extras = debuggable() ? getIntent().getExtras() : null;
        if (extras != null)
            for (String k : extras.keySet())
                if (k.startsWith("WWHD_")) setenv(k, String.valueOf(extras.get(k)));
    }

    private static void setenv(String k, String v) {
        try {
            Os.setenv(k, v, true);
            Log.i(TAG, "env " + k + "=" + v);
        } catch (ErrnoException e) {
            Log.w(TAG, "setenv " + k, e);
        }
    }

    // ------------------------------------------------------------------ startup
    // testing the setup screens' look: WWHD_PREVIEW=setup or =compile (simulated progress, nothing runs)
    private boolean preview() {
        String p = Os.getenv("WWHD_PREVIEW");
        if (p == null) return false;
        if (p.equals("compile")) {
            LinearLayout box = new LinearLayout(this);
            box.setOrientation(LinearLayout.VERTICAL);
            int pad = (int) (24 * getResources().getDisplayMetrics().density);
            box.setPadding(pad, pad, pad, pad);
            TextView t = new TextView(this);
            t.setTextSize(16);
            t.setText(R.string.compile_running);
            box.addView(t);
            android.widget.ProgressBar bar = new android.widget.ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal);
            bar.setMax(1000);
            box.addView(bar);
            TextView detail = new TextView(this);
            box.addView(detail);
            setContentView(seaScreen(box));
            final long t0 = android.os.SystemClock.elapsedRealtime();
            final android.os.Handler h = new android.os.Handler(android.os.Looper.getMainLooper());
            h.post(new Runnable() {
                @Override
                public void run() {
                    long ms = (android.os.SystemClock.elapsedRealtime() - t0) % 60000;
                    int done = (int) (ms * 27 / 60000);
                    bar.setProgress((int) (ms * 1000 / 60000));
                    detail.setText(getString(R.string.compile_detail, done, 27, ms / 60000, ms / 1000 % 60));
                    if (sea != null) sea.setProgress(ms / 60000f);
                    h.postDelayed(this, 250);
                }
            });
        } else {
            showSetup("preview");
        }
        return true;
    }

    private void checkAndStart() {
        if (preview()) return;
        if (WorkService.Work.running() != 0) {  // opened again while extracting or compiling
            showWorkScreen(WorkService.Work.running());
            return;
        }
        if (WorkService.Work.takeFinished(this)) return;
        // WWHD_SELFTEST=1: renderer test without game files (runtime/src/android/selftest.cpp)
        String problem = Os.getenv("WWHD_SELFTEST") != null ? null : Native.checkGame(gameDir());
        if (problem != null) {
            showSetup(problem);
            return;
        }
        if (Native.needsCompile(gameDir(), codeDir().getAbsolutePath())) {
            runCompile();
            return;
        }
        // the last run ended unexpectedly: offer its log before the game starts (a game that closes
        // again right after loading leaves no time to share it later)
        if (!crashOffered) {
            crashOffered = true;
            CrashLogs.recordExit(this, prefs);
            if (CrashLogs.offerNew(this, prefs, this::checkAndStart)) return;
        }
        // the GPU driver lost the device last time (it hung): offer another driver before the game starts
        if (!gpuLostOffered) {
            gpuLostOffered = true;
            if (offerGpuDriverAfterLoss(this::checkAndStart)) return;
        }
        File base = baseDir();
        // testing: WWHD_CLEAR_SHADERS=1 (an intent extra) starts as with "Delete shader cache"
        if (Os.getenv("WWHD_CLEAR_SHADERS") != null) Backup.deleteTree(new File(getNoBackupFilesDir(), "shadercache"));
        installShaderTemplate();
        String failedDriver = applyGpuDriver();
        // the save state picker before the intro: the game waits for it from its first frame on
        boolean offerStates = startMode() != START_INTRO && !usedStateSlots(base).isEmpty();
        if (offerStates) Native.setStartHold(true);
        Native.start(gameDir(), new File(base, "save").getAbsolutePath(),
                new File(getNoBackupFilesDir(), "shadercache").getAbsolutePath(), base.getAbsolutePath());
        started = true;
        applyOptions();
        startMotion();
        showGame();
        showCacheLoading();
        if (offerStates) offerStartStates();
        updateDrcDisplay();
        if (failedDriver != null)
            new GameDialog(this).title(R.string.opt_gpu_driver).message(getString(R.string.gpu_driver_failed, failedDriver))
                    .button(R.string.opt_ok, null).show();
    }

    private boolean crashOffered;  // once per process: the offer before the game starts
    private boolean gpuLostOffered;

    // `gpu_lost` (vk_device.cpp, vk_note_device_lost): the GPU driver hung or crashed in the last run.
    // Old Qualcomm drivers (v502 on Snapdragon 855/865) do that with this game; a Turnip driver
    // runs it (issue #25). On Adreno GPUs: a driver file can be chosen right away, or the game
    // starts as it is. `then` continues the start (also after the picker, unless the new driver
    // restarts the app).
    private boolean offerGpuDriverAfterLoss(Runnable then) {
        File note = new File(baseDir(), "gpu_lost");
        if (!note.exists()) return false;
        // the note: GPU, the driver that ran (as Vulkan names it), the installed driver package's
        // folder if one was loaded (empty: the system driver)
        String driver = "", packageDir = "";
        try (BufferedReader r = new BufferedReader(new FileReader(note))) {
            r.readLine();  // GPU
            String d = r.readLine();
            if (d != null) driver = d.trim();
            String p = r.readLine();
            if (p != null) packageDir = p.trim();
        } catch (IOException ignored) {
        }
        //noinspection ResultOfMethodCallIgnored
        note.delete();
        if (!GpuDrivers.supported()) return false;
        if (driver.isEmpty()) driver = getString(R.string.gpu_lost_unknown_driver);
        GameDialog d = new GameDialog(this).title(R.string.gpu_lost_title).cancelable(false);
        if (packageDir.isEmpty()) {  // the system driver hung
            d.message(getString(R.string.gpu_lost_system, driver))
                    .button(R.string.gpu_lost_start, then)
                    .button(R.string.gpu_lost_choose, () -> pickDriverThen(then));
        } else {  // an installed driver hung: going back to the system driver comes first
            d.message(getString(R.string.gpu_lost_custom, driver))
                    .button(R.string.gpu_lost_start, then)
                    .button(R.string.gpu_lost_choose_other, () -> pickDriverThen(then))
                    .button(R.string.gpu_lost_system_driver, () -> {
                        prefs.edit().putString("gpu_driver", "").commit();
                        then.run();
                    });
        }
        d.show();
        return true;
    }

    // the driver picker, and `then` once it is done without a restart (cancelled, failed, "Later")
    private Runnable afterDriverPick;
    private void pickDriverThen(Runnable then) {
        afterDriverPick = then;
        pickDriver();
    }
    private void driverPickDone() {
        Runnable r = afterDriverPick;
        afterDriverPick = null;
        if (r != null) r.run();
    }

    // ------------------------------------------------------------------ GPU driver (Adreno)
    // A driver package the user installed (GpuDrivers), used instead of the system driver from the
    // next start on. Returns the name of a driver that failed at its last start (then the system
    // driver runs and the choice is reset), else null.
    private static final int PICK_DRIVER = 5;

    private String applyGpuDriver() {
        if (!GpuDrivers.supported()) return null;
        GpuDrivers.Driver d = GpuDrivers.find(this, prefs.getString("gpu_driver", ""));
        if (d == null) return null;
        File probe = new File(d.dir, ".probe");
        if (probe.exists()) {  // crashed, hung or couldn't be loaded at its last start
            //noinspection ResultOfMethodCallIgnored
            probe.delete();
            prefs.edit().putString("gpu_driver", "").commit();
            return d.name;
        }
        if (!GpuDrivers.prepareHooks(this)) return null;
        setenv("WWHD_GPU_DRIVER_DIR", d.dir.getAbsolutePath() + "/");
        setenv("WWHD_GPU_DRIVER_LIB", d.library);
        // Turnip: always render in tiles (GMEM). Its automatic choice falls back to direct rendering for
        // many of the game's passes, which measured slower on an Adreno 740 (25 -> 28.5 fps in a heavy
        // scene; from pull request #8 by SSunnKing); a TU_DEBUG in wwhd.env wins
        if (d.library.contains("freedreno") && Os.getenv("TU_DEBUG") == null) setenv("TU_DEBUG", "gmem");
        setenv("WWHD_GPU_HOOK_DIR", GpuDrivers.hookDir(this).getAbsolutePath() + "/");
        try {
            //noinspection ResultOfMethodCallIgnored
            probe.createNewFile();
            setenv("WWHD_GPU_DRIVER_PROBE", probe.getAbsolutePath());
        } catch (IOException e) {
            Log.w(TAG, "cannot create " + probe, e);
        }
        return null;
    }

    String gpuDriverLabel() {
        GpuDrivers.Driver d = GpuDrivers.find(this, prefs.getString("gpu_driver", ""));
        return d != null ? d.name : getString(R.string.gpu_driver_system);
    }

    String gpuDriverId() { return prefs.getString("gpu_driver", ""); }

    /** Uses driver `id` ("" = the system's) from a restart on, after asking. */
    void chooseGpuDriver(String id, String name) {
        if (id.equals(gpuDriverId())) return;
        new GameDialog(this).title(R.string.opt_gpu_driver).message(getString(R.string.gpu_driver_switch, name))
                .button(R.string.opt_cancel, null)
                .button(R.string.res_restart_now, () -> {
                    prefs.edit().putString("gpu_driver", id).commit();
                    restartApp();
                }).show();
    }

    /** Deletes an installed driver; the one in use only with a restart onto the system driver. */
    void removeGpuDriver(String id, Runnable refresh) {
        if (!id.equals(gpuDriverId())) {
            GpuDrivers.remove(this, id);
            refresh.run();
            return;
        }
        new GameDialog(this).title(R.string.opt_gpu_driver).message(R.string.gpu_driver_remove_active)
                .button(R.string.opt_cancel, null)
                .button(R.string.res_restart_now, () -> {
                    prefs.edit().putString("gpu_driver", "").commit();
                    // the game writes this driver's pipeline cache once more while it closes: the
                    // restart deletes it once the game's process has ended
                    File cache = GpuDrivers.pipelineCache(this, id);
                    GpuDrivers.remove(this, id);  // the running process keeps its mapping
                    restartApp(false, cache);
                }).show();
    }

    void pickDriver() {
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        i.addCategory(Intent.CATEGORY_OPENABLE);
        i.setType("*/*");
        startActivityForResult(i, PICK_DRIVER);
    }

    private void installDriver(android.net.Uri uri) {
        withProgress(R.string.gpu_driver_installing, () -> GpuDrivers.install(this, uri), res -> {
            if (res.startsWith("!")) {
                new GameDialog(this).title(R.string.opt_gpu_driver).message(getString(R.string.gpu_driver_install_failed, res.substring(1)))
                        .button(R.string.opt_ok, this::driverPickDone).show();
                return;
            }
            GpuDrivers.Driver d = GpuDrivers.find(this, res);
            String name = d != null ? d.name + (d.version.isEmpty() ? "" : " (" + d.version + ")") : res;
            new GameDialog(this).title(R.string.opt_gpu_driver).message(getString(R.string.gpu_driver_installed, name))
                    .button(R.string.gpu_driver_later, this::driverPickDone)
                    .button(R.string.gpu_driver_use_now, () -> {
                        prefs.edit().putString("gpu_driver", res).commit();
                        restartApp();
                    }).show();
        });
    }

    // ------------------------------------------------------------------ setup screens
    // The setup, extraction and compile screens: their content on a parchment card over an animated
    // seascape (SeaView), whose boat sails across as the work progresses.
    private SeaView sea;

    private View seaScreen(LinearLayout content) {
        float d = getResources().getDisplayMetrics().density;
        android.widget.FrameLayout root = new android.widget.FrameLayout(this);
        sea = new SeaView(this);
        root.addView(sea, new android.widget.FrameLayout.LayoutParams(-1, -1));
        android.graphics.drawable.GradientDrawable card = new android.graphics.drawable.GradientDrawable();
        card.setColor(0xFBF6EBCB);
        card.setCornerRadius(18 * d);
        card.setStroke((int) (3 * d), 0xFF8B5A2B);
        content.setBackground(card);
        content.setElevation(8 * d);
        styleCardText(content);
        ScrollView sv = new ScrollView(this);
        sv.setFillViewport(false);
        sv.addView(content, new android.widget.FrameLayout.LayoutParams(-1, -2));
        // a card of at most 640 dp, in the upper part so the sea stays visible
        int width = (int) Math.min(getResources().getDisplayMetrics().widthPixels - 32 * d, 640 * d);
        android.widget.FrameLayout.LayoutParams lp = new android.widget.FrameLayout.LayoutParams(width, -2,
                Gravity.CENTER_HORIZONTAL | Gravity.TOP);
        lp.topMargin = (int) (24 * d);
        lp.bottomMargin = (int) (24 * d);
        root.addView(sv, lp);
        return root;
    }

    private static void styleCardText(android.view.ViewGroup g) {
        for (int i = 0; i < g.getChildCount(); i++) {
            View v = g.getChildAt(i);
            if (v instanceof Button) {
                v.setBackgroundTintList(android.content.res.ColorStateList.valueOf(0xFF8B5A2B));
                ((Button) v).setTextColor(0xFFFFF6E0);
                continue;
            }
            if (v instanceof android.widget.ProgressBar) {
                ((android.widget.ProgressBar) v).setProgressTintList(android.content.res.ColorStateList.valueOf(0xFF8B5A2B));
                ((android.widget.ProgressBar) v).setProgressBackgroundTintList(android.content.res.ColorStateList.valueOf(0xFFD9C79E));
                continue;
            }
            if (v instanceof TextView) {
                ((TextView) v).setTextColor(0xFF4A2C12);
                ((TextView) v).setLinkTextColor(0xFF1F5FA8);
            } else if (v instanceof android.view.ViewGroup) styleCardText((android.view.ViewGroup) v);
        }
    }

    private void showSetup(String problem) {
        LinearLayout box = new LinearLayout(this);
        box.setOrientation(LinearLayout.VERTICAL);
        int pad = (int) (24 * getResources().getDisplayMetrics().density);
        box.setPadding(pad, pad, pad, pad);
        TextView t = new TextView(this);
        t.setTextSize(16);
        if (Native.buildsGameCode()) {  // the APK without game code: a first start is the normal case
            String text = getString(R.string.setup_welcome);
            if (new File(gameDir()).exists()) text += "\n\n" + getString(R.string.setup_problem, problem);
            t.setText(text);
        } else {
            t.setText(getString(R.string.setup_text, gameDir(), problem));
        }
        t.setTextIsSelectable(true);
        box.addView(t);
        // the files themselves, not their folder: some devices' folder picker lists nothing on the
        // internal storage (Android 16 with AOSP DocumentsUI: iPlay 70 mini Ultra, K70 Pro)
        Button extractFiles = new Button(this);
        extractFiles.setText(R.string.setup_extract_files);
        extractFiles.setOnClickListener(v -> pickDiscFiles());
        box.addView(extractFiles, new LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT));
        Button retry = new Button(this);
        retry.setText(R.string.setup_retry);
        retry.setOnClickListener(v -> checkAndStart());
        box.addView(retry, new LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT));
        Button about = new Button(this);
        about.setText(R.string.menu_about);
        about.setOnClickListener(v -> showLicenses());
        box.addView(about, new LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT));
        setContentView(seaScreen(box));
    }

    private void showGame() {
        sea = null;
        FrameLayout root = new FrameLayout(this);
        surface = new SurfaceView(this);
        surface.getHolder().addCallback(this);
        root.addView(surface, new FrameLayout.LayoutParams(FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT));
        controls = new ControlsView(this, this);
        root.addView(controls, new FrameLayout.LayoutParams(FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT,
                Gravity.CENTER));
        setContentView(root);
        applyControlsAppearance();
        hideSystemBars();
        warpTest = null;
        if (prefs.getBoolean("warp_test", false)) setWarpTest(true);  // again after a restart (a crash while testing)
    }

    // developer: story presets from a WWHD save file (StoryPresets): the file, then a name for its slots
    private static final int PICK_PRESET_SAVE = 7;

    void pickPresetSave() {
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        i.addCategory(Intent.CATEGORY_OPENABLE);
        i.setType("*/*");
        i.putExtra(Intent.EXTRA_ALLOW_MULTIPLE, true);
        startActivityForResult(i, PICK_PRESET_SAVE);
    }

    // one file: its slots get a name asked for (a WWHD save is always cking.sav); several (e.g. a
    // collection of GameCube saves): each is named after its file
    private void importPresetSaves(List<android.net.Uri> uris) {
        List<String> names = new ArrayList<>();
        List<byte[]> files = new ArrayList<>();
        for (android.net.Uri u : uris) {
            String name = "save";
            try {
                files.add(readAll(getContentResolver(), u));
                try (android.database.Cursor c = getContentResolver().query(u, new String[] {android.provider.OpenableColumns.DISPLAY_NAME}, null, null, null)) {
                    if (c != null && c.moveToFirst() && c.getString(0) != null) name = c.getString(0).replaceAll("\\.(sav|gci|wwp)$", "");
                }
                names.add(name);
            } catch (IOException e) {
                android.widget.Toast.makeText(this, e.getMessage(), android.widget.Toast.LENGTH_LONG).show();
                return;
            }
        }
        if (files.size() == 1) askName(getString(R.string.preset_import), names.get(0), base -> importPresets(List.of(base), files));
        else importPresets(names, files);
    }

    private void importPresets(List<String> names, List<byte[]> files) {
        int n = 0, failed = 0, why = R.string.preset_import_none;
        for (int i = 0; i < files.size(); i++) {
            try {
                n += StoryPresets.importFile(this, names.get(i), files.get(i));
            } catch (StoryPresets.ImportError e) {
                failed++;
                why = e.message;
            }
        }
        String msg = n > 0 ? getString(R.string.preset_imported, n) : getString(why);
        if (n > 0 && failed > 0) msg += " (" + getString(R.string.preset_import_skipped, failed) + ")";
        android.widget.Toast.makeText(this, msg, android.widget.Toast.LENGTH_LONG).show();
    }

    interface NameDone { void done(String name); }

    /** A dialog with a text field (a preset's name); `done` gets the text when confirmed. */
    void askName(String title, String initial, NameDone done) {
        EditText edit = new EditText(this);
        edit.setInputType(InputType.TYPE_CLASS_TEXT);
        edit.setSingleLine(true);
        edit.setText(initial);
        edit.setSelection(edit.getText().length());
        edit.setTextColor(GameUi.INK);
        edit.setTextSize(20);
        new GameDialog(this).title(title).content(edit).button(android.R.string.cancel, null)
                .button(R.string.opt_ok, () -> done.done(edit.getText().toString().trim())).top().show();
    }

    // developer: the warp test panel over the game (WarpTestOverlay)
    private WarpTestOverlay warpTest;

    void setWarpTest(boolean on) {
        prefs.edit().putBoolean("warp_test", on).apply();
        FrameLayout root = surface != null && surface.getParent() instanceof FrameLayout ? (FrameLayout) surface.getParent() : null;
        if (warpTest != null) {
            warpTest.stop();
            if (warpTest.getParent() != null) ((FrameLayout) warpTest.getParent()).removeView(warpTest);
            warpTest = null;
        }
        if (on && root != null) {
            warpTest = new WarpTestOverlay(this);
            root.addView(warpTest, warpTest.layoutParams());
        }
    }

    // The shader head start's template (assets/shader_template.bin, when the build has one) next to the
    // shader cache, where the runtime reads it (vk_headstart.cpp); a build without one removes an old copy
    private void installShaderTemplate() {
        File dir = new File(getNoBackupFilesDir(), "shadercache");
        File out = new File(dir, "headstart-template.bin");
        try (java.io.InputStream in = getAssets().open("shader_template.bin")) {
            long updated = getPackageManager().getPackageInfo(getPackageName(), 0).lastUpdateTime;
            if (out.exists() && out.lastModified() >= updated) return;
            dir.mkdirs();
            File tmp = new File(dir, "headstart-template.tmp");
            try (java.io.OutputStream o = new java.io.FileOutputStream(tmp)) {
                byte[] buf = new byte[1 << 16];
                for (int n; (n = in.read(buf)) > 0;) o.write(buf, 0, n);
            }
            if (!tmp.renameTo(out)) Log.w(TAG, "cannot install " + out);
        } catch (java.io.FileNotFoundException e) {
            out.delete();
        } catch (Exception e) {
            Log.w(TAG, "shader template", e);
        }
    }

    // The shader cache's replay before the first frame (Native.cacheProgress; seconds with a large
    // cache): the sea card with its progress over the game picture instead of a black screen. Shown
    // only when there is a cache file and 300 ms have passed; gone once the replay is done.
    private void showCacheLoading() {
        if (surface == null || !(surface.getParent() instanceof FrameLayout)) return;
        FrameLayout root = (FrameLayout) surface.getParent();
        LinearLayout box = new LinearLayout(this);
        box.setOrientation(LinearLayout.VERTICAL);
        int pad = (int) (24 * getResources().getDisplayMetrics().density);
        box.setPadding(pad, pad, pad, pad);
        TextView t = new TextView(this);
        t.setTextSize(16);
        t.setText(R.string.cache_loading);
        box.addView(t);
        android.widget.ProgressBar bar = new android.widget.ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal);
        bar.setMax(1000);
        box.addView(bar);
        TextView detail = new TextView(this);
        box.addView(detail);
        View screen = seaScreen(box);
        final SeaView mySea = sea;
        sea = null;  // the game's screen: no work screen of its own
        screen.setVisibility(View.GONE);
        root.addView(screen, new FrameLayout.LayoutParams(FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT));
        final long t0 = android.os.SystemClock.uptimeMillis();
        final android.os.Handler h = new android.os.Handler(android.os.Looper.getMainLooper());
        h.post(new Runnable() {
            @Override
            public void run() {
                long[] p = Native.cacheProgress();
                if (p[0] == 2 || screen.getParent() != root) {  // done, or the game's screen was replaced
                    root.removeView(screen);
                    return;
                }
                // only while a cache file is replayed (state 1 with a size) or the shader head start is
                // made from the game files (3), not while the game boots
                if (screen.getVisibility() != View.VISIBLE && (p[0] == 1 || p[0] == 3) && p[2] > 0
                        && android.os.SystemClock.uptimeMillis() - t0 > 300)
                    screen.setVisibility(View.VISIBLE);
                if (p[2] > 0) {
                    bar.setProgress((int) Math.min(1000, p[1] * 1000 / p[2]));
                    mySea.setProgress(Math.min(1f, (float) p[1] / p[2]));
                }
                t.setText(p[0] == 3 ? R.string.headstart_building : R.string.cache_loading);
                detail.setText(p[0] == 3 ? getString(R.string.headstart_building_detail, (int) p[3])
                                         : getString(R.string.cache_loading_detail, (int) p[3]));
                h.postDelayed(this, 150);
            }
        });
    }

    // savestate.cpp slot_path: slot 1..5, auto 6..7, last session 8
    static File stateFile(File base, int slot, String ext) {
        String name = slot == 8 ? "last" : slot > 5 ? "auto" + (slot - 5) : "slot" + slot;
        return new File(new File(base, "states"), name + "." + ext);
    }

    // the slots with a state file, the latest first
    static java.util.List<Integer> usedStateSlots(File base) {
        java.util.List<Integer> used = new java.util.ArrayList<>();
        for (int slot = 1; slot <= 8; slot++) if (stateFile(base, slot, "bin").exists()) used.add(slot);
        used.sort((x, y) -> Long.compare(stateFile(base, y, "bin").lastModified(), stateFile(base, x, "bin").lastModified()));
        return used;
    }

    // Saves > At start: the intro as usual, the save state picker, or the last session loaded at once
    static final int START_INTRO = 0, START_STATES = 1, START_LAST = 2;

    int startMode() { return prefs.getInt("start_mode", START_INTRO); }

    // Once the game waits before its intro (after the shader cache replay): the last session loads
    // right away (Saves > At start), else the save states to load, or Start
    // game. Without a state this game can load, it starts right away.
    private void offerStartStates() {
        final android.os.Handler h = new android.os.Handler(android.os.Looper.getMainLooper());
        h.post(new Runnable() {
            @Override
            public void run() {
                int st = Native.startHoldState();
                if (st == 0) return;
                if (st == 1) { h.postDelayed(this, 100); return; }
                // an automatic load that didn't keep running for 20 s (crashed): the picker this time
                boolean lastFailed = prefs.getBoolean("start_last_trying", false);
                prefs.edit().putBoolean("start_last_trying", false).commit();
                String[] last = Native.saveSlotInfo(8);
                if (startMode() == START_LAST && !lastFailed && last[0].equals("1") && last[1].equals("1")) {
                    prefs.edit().putBoolean("start_last_trying", true).commit();
                    Native.loadState(8);
                    android.widget.Toast.makeText(MainActivity.this, R.string.start_last_loading, android.widget.Toast.LENGTH_SHORT).show();
                    showStateResult();
                    h.postDelayed(() -> prefs.edit().putBoolean("start_last_trying", false).apply(), 20000);
                    return;
                }
                boolean any = false;
                for (int slot : usedStateSlots(baseDir())) {
                    String[] info = Native.saveSlotInfo(slot);
                    any |= info[0].equals("1") && info[1].equals("1");
                }
                boolean picker = startMode() == START_STATES || lastFailed;
                if (any && picker && !isFinishing()) new OptionsMenu(MainActivity.this, true).show();
                else Native.setStartHold(false);
            }
        });
    }

    private void hideSystemBars() {
        WindowInsetsController c = getWindow().getInsetsController();
        if (c == null) return;
        c.hide(WindowInsets.Type.systemBars());
        c.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
    }

    private void applyOptions() {
        Native.setOption("ao_mode", prefs.getInt("ao_mode", Native.getOption("ao_mode")));
        Native.setOption("ao_hires", prefs.getBoolean("ao_hires", Native.getOption("ao_hires") != 0) ? 1 : 0);
        Native.setOption("aniso", prefs.getBoolean("aniso", Native.getOption("aniso") != 0) ? 1 : 0);
        Native.setOption("pro_controller", prefs.getBoolean("pro_controller", true) ? 1 : 0);  // default: as a Pro Controller
        Native.setOption("invert_camera_y", prefs.getInt("invert_camera_y_mode", 0));
        Native.setOption("tv_aspect", prefs.getInt("tv_aspect", 0));
        Native.setOption("render_aspect", prefs.getInt("render_aspect", 0));
        for (String m : MODS) Native.setOption(m, prefs.getBoolean(m, false) ? 1 : 0);
        for (String c : CHEATS_INFINITE) Native.setOption(c, prefs.getBoolean(c, false) ? 1 : 0);
        Native.setOption("cheat_moon_rise", prefs.getInt("cheat_moon_rise", 100));
        Native.setOption("cheat_moon_fly", prefs.getInt("cheat_moon_fly", 100));
        Native.setOption("mod_camera_speed", prefs.getInt("mod_camera_speed", 100));
        Native.setOption("mod_jump", prefs.getBoolean("mod_jump", false) ? 1 : 0);
        Native.setOption("mod_jump_double", prefs.getBoolean("mod_jump_double", false) ? 1 : 0);
        Native.setOption("mod_jump_noauto", prefs.getBoolean("mod_jump_noauto", false) ? 1 : 0);
        Native.setOption("auto_state", prefs.getInt("auto_state", 0));
        Native.setOption("mod_run_speed", prefs.getInt("mod_run_speed", 100));
        Native.setOption("mod_swim_speed", prefs.getInt("mod_swim_speed", 100));
        Native.setOption("mod_run_mode", moveMode("mod_run"));
        Native.setOption("mod_swim_mode", moveMode("mod_swim"));
        Native.setOption("mod_climb_speed", prefs.getInt("mod_climb_speed", 100));
        Native.setOption("mod_climb_mode", moveMode("mod_climb"));
        Native.setOption("mod_sidle_speed", prefs.getInt("mod_sidle_speed", 100));
        Native.setOption("mod_sidle_mode", moveMode("mod_sidle"));
    }

    void applyControlsAppearance() {
        if (controls == null) return;
        boolean visible = prefs.getBoolean("controls_visible", true) && !autoHidden;
        final boolean nw = NEW_TOUCH_CONTROLS;
        controls.setAppearance(visible, prefs.getFloat("controls_scale", 1f), nw ? prefs.getFloat("controls_opacity", 0.45f) : 0.45f);
        boolean flexible = nw && prefs.getInt("controls_style", 0) == 1, swipe = prefs.getInt("touch_camera", 0) == 1;
        controls.setStyle(flexible, prefs.getBoolean("controls_show_sticks", true), swipe);
        controls.setSwipeSpeed(prefs.getFloat("swipe_speed", SWIPE_SPEED_DEFAULT));
        controls.setFilled(nw && prefs.getBoolean("controls_filled", false));
        // context buttons: the game's button cluster (X, Y, R, B, ZR, A's seat and floating label) is hidden
        boolean context = flexible && prefs.getBoolean("context_buttons", false);
        controls.setContext(context);
        Native.setHudHidden(context ? "N_X_00,N_Y_00,N_R_00,P_B_00,P_ZR_00,P_SetSeatASpecial_00,W_SetSeatA_00,L_CommandA_00" : "");
        Native.setHudFade(0, nw && prefs.getBoolean("hearts_combat", false));
        Native.setHudFade(1, nw && prefs.getBoolean("rupees_on_change", false));
        if (!controls.editing()) controls.setLayoutString(nw ? prefs.getString("controls_layout", "") : "");
        Native.setTouchSwipeCamera(flexible && swipe);
        updateLayout();
    }

    // ------------------------------------------------------------------ surface and screen layout
    @Override
    public void surfaceCreated(SurfaceHolder holder) {}

    @Override
    public void surfaceChanged(SurfaceHolder holder, int format, int w, int h) {
        surfaceW = w;
        surfaceH = h;
        updateLayout();
        Native.surfaceChanged(holder.getSurface());
    }

    @Override
    public void surfaceDestroyed(SurfaceHolder holder) {
        Native.surfaceDestroyed();
    }

    /** a rect of aspect `a` letterboxed in r (as the renderer draws it) */
    private static RectF fit(RectF r, float a) {
        RectF o = new RectF(r);
        if (r.width() / r.height() > a) {
            float w = r.height() * a;
            o.left += (r.width() - w) / 2;
            o.right = o.left + w;
        } else {
            float h = r.width() / a;
            o.top += (r.height() - h) / 2;
            o.bottom = o.top + h;
        }
        return o;
    }

    // ------------------------------------------------------------------ GamePad on a second display
    // dual-screen devices: the GamePad picture on the second screen (DrcDisplay), the TV picture
    // alone in the main window; the option "GamePad screen" in Graphics (on by default)
    private DrcDisplay drcDisplay;

    boolean hasSecondDisplay() { return drcDisplay != null && drcDisplay.available() != null; }

    boolean drcOnSecondDisplay() { return prefs.getBoolean("drc_second_display", true); }

    void setDrcOnSecondDisplay(boolean on) {
        prefs.edit().putBoolean("drc_second_display", on).apply();
        updateDrcDisplay();
    }

    // the GamePad picture there in the display's shape (its background reaches the edges) or 16:9
    boolean drcFill() { return prefs.getBoolean("drc_fill", true); }

    void setDrcFill(boolean on) {
        prefs.edit().putBoolean("drc_fill", on).apply();
        if (drcDisplay != null) drcDisplay.setFill(on);
    }

    private void updateDrcDisplay() {
        if (drcDisplay == null) {
            drcDisplay = new DrcDisplay(this, active -> updateLayout());
            drcDisplay.setFill(drcFill());
        }
        drcDisplay.setWanted(started && resumed && drcOnSecondDisplay());
    }

    // Graphics > Screen layout: TV only by default; with a second display (dual-screen devices) the
    // GamePad inset, used when the GamePad is set to show in the layout instead of on that display
    int layoutSetting() { return prefs.getInt("layout", hasSecondDisplay() ? LAYOUT_INSET : LAYOUT_TV); }

    void updateLayout() {
        if (surfaceW == 0 || surfaceH == 0 || controls == null) return;
        float W = surfaceW, H = surfaceH;
        RectF full = new RectF(0, 0, W, H);
        RectF tv, drc;
        int layout = layoutSetting();
        if (layout == LAYOUT_ON_DEMAND && !NEW_TOUCH_CONTROLS) layout = LAYOUT_INSET;
        if (drcDisplay != null && drcDisplay.active()) layout = LAYOUT_TV;  // the GamePad has its own display
        if (layout != LAYOUT_ON_DEMAND) drcShown = false;
        controls.setScreenButton(layout == LAYOUT_ON_DEMAND, drcShown);
        // the inset sits between the shoulder buttons, sized to leave them free
        float insetW = Math.min(W * 0.3f, W - 2 * (Math.min(W, H) / 7f * 3.6f));
        RectF inset = new RectF((W - insetW) / 2, 0, (W + insetW) / 2, insetW / DRC_ASPECT);
        switch (layout) {
            case LAYOUT_SIDE:
                tv = new RectF(0, 0, W * 0.7f, H);
                drc = new RectF(W * 0.7f, 0, W, H);
                break;
            case LAYOUT_TV:
                tv = full;
                drc = null;
                break;
            case LAYOUT_DRC_LARGE:
                drc = full;
                tv = inset;
                break;
            case LAYOUT_ON_DEMAND:  // the TV picture, or only the GamePad picture (width -1: not drawn)
                tv = drcShown ? new RectF(0, 0, -1, -1) : full;
                drc = drcShown ? full : null;
                break;
            default:
                tv = full;
                drc = inset;
                break;
        }
        Native.setLayout(new float[] {tv.left, tv.top, tv.right < 0 ? -1 : tv.width(), tv.right < 0 ? -1 : tv.height()},
                drc == null ? null : new float[] {drc.left, drc.top, drc.width(), drc.height()}, drc != null);
        controls.setDrcRect(drc == null ? null : fit(drc, DRC_ASPECT));
        controls.setTvRect(tv.right < 0 ? null : fit(tv, TV_ASPECT));
        controls.setClimbHud(prefs.getBoolean("mod_climb", false));
        controls.setPerfHud(prefs.getBoolean("perf_hud", false));
        controls.setPerfItems(prefs.getInt("perf_items", ControlsView.PERF_ALL));
        mapper.loadMap(prefs.getString("pad_map", ""));
        controls.perfSettings = this::perfSettingsLines;
        controls.setPerfPosition(prefs.getFloat("perf_x", -1), prefs.getFloat("perf_y", -1));
    }

    // ------------------------------------------------------------------ input
    private void pushInput() {
        if (controls == null) return;
        int b = controls.buttons() | mapper.padButtons | mapper.keyButtons;
        float lx = pick(controls.stickX(0), mapper.lx, mapper.keyLX());
        float ly = pick(controls.stickY(0), mapper.ly, mapper.keyLY());
        float rx = pick(controls.stickX(1), mapper.rx, mapper.keyRX());
        float ry = pick(controls.stickY(1), mapper.ry, mapper.keyRY());
        if (mapper.keyLX() != 0 && mapper.keyLY() != 0) { lx *= 0.7071f; ly *= 0.7071f; }
        Native.setPad(b, lx, ly, rx, ry);
    }

    private static float pick(float a, float b, float c) {
        float v = a;
        if (Math.abs(b) > Math.abs(v)) v = b;
        if (Math.abs(c) > Math.abs(v)) v = c;
        return v;
    }

    // ------------------------------------------------------------------ motion and rumble
    // The GamePad's gyro (MotionInput) and rumble (Rumble) follow the controller in use, or this
    // device while playing by touch.
    private MotionInput motion;
    private Rumble rumble;
    private InputDevice lastController;  // the controller in use, null while playing by touch

    private void startMotion() {
        if (motion == null) motion = new MotionInput(this);
        if (prefs.getBoolean("motion", true)) motion.start(lastController);
        else motion.stop();
    }

    void setMotion(boolean on) {
        prefs.edit().putBoolean("motion", on).apply();
        if (started) startMotion();
    }

    void setRumble(boolean on) {
        prefs.edit().putBoolean("rumble", on).apply();
        rumbler().enabled = on;
        if (!on) rumbler().stop();
    }

    Rumble rumbler() {
        if (rumble == null) {
            rumble = new Rumble(this);
            rumble.enabled = prefs.getBoolean("rumble", true);
        }
        return rumble;
    }

    // the input source changed: a controller (or touch, null)
    private void inputSource(InputDevice d) {
        if (d == lastController || (d != null && lastController != null && d.getId() == lastController.getId())) return;
        if (debuggable()) Log.d(TAG, "input source: " + (d == null ? "touch" : d.getName() + " (motion " + MotionInput.hasMotion(d) + ")"));
        lastController = d;
        rumbler().setController(d);
        if (started) startMotion();
    }

    private long lastMotionRecheck;

    private void controllerUsed(InputDevice d) {
        if (d != null && InputMapper.isController(d)) {
            inputSource(d);
            // a controller's sensors can come up after its first input (just connected): check
            // again now and then while the device's sensors stand in for them
            long now = android.os.SystemClock.uptimeMillis();
            if (started && motion != null && !motion.fromController() && now - lastMotionRecheck > 1000) {
                lastMotionRecheck = now;
                if (MotionInput.hasMotion(d)) startMotion();
            }
        }
        controllerUsed();
    }

    private void controllerUsed() {
        if (!autoHidden && prefs.getBoolean("auto_hide", true) && controls != null && controls.controlsVisible()) {
            autoHidden = true;
            applyControlsAppearance();
        }
    }

    @Override
    public void onControlsChanged() { pushInput(); }

    @Override
    public void onScreenToggle() {
        drcShown = !drcShown;
        updateLayout();
    }

    /** the on-screen controls' layout editor (ControlsView); the menu closes for it */
    void editControls() {
        if (controls != null) controls.setEditing(true);
    }

    @Override
    public void onLayoutEdited(String layout, float opacity, boolean filled) {
        prefs.edit().putString("controls_layout", layout).putFloat("controls_opacity", opacity).putBoolean("controls_filled", filled).apply();
        applyControlsAppearance();
    }

    @Override
    public void onOverlayMoved(float fx, float fy) {
        prefs.edit().putFloat("perf_x", fx).putFloat("perf_y", fy).apply();
    }

    // ------------------------------------------------------------------ performance overlay
    // shown or not, which values, and where (dragged; "Move" also lets it move over the controls)
    static final int[] PERF_BITS = {ControlsView.PERF_FPS, ControlsView.PERF_FRAME, ControlsView.PERF_CPU,
            ControlsView.PERF_GPU, ControlsView.PERF_TEMP_CPU, ControlsView.PERF_TEMP_GPU, ControlsView.PERF_TEMP_BAT,
            ControlsView.PERF_SETTINGS};
    static final int[] PERF_LABELS = {R.string.perf_fps, R.string.perf_frame, R.string.perf_cpu, R.string.perf_gpu,
            R.string.perf_temp_cpu, R.string.perf_temp_gpu, R.string.perf_temp_bat, R.string.perf_settings};

    /** the overlay's settings lines: resolution, ambient occlusion and the optional effects (the driver is in ControlsView) */
    java.util.List<String> perfSettingsLines() {
        java.util.List<String> l = new java.util.ArrayList<>();
        String scale = prefs.getString("res_scale", "1");
        float f;
        try {
            f = Float.parseFloat(scale);
        } catch (NumberFormatException e) {
            f = 1;
        }
        String[] ao = {"Wii U", "centre fix", "centre+noise"};
        int aoMode = Math.max(0, Math.min(2, Native.getOption("ao_mode")));
        l.add("Res: " + scale + "× " + Math.round(720 * f) + "p  AO: " + ao[aoMode]);
        StringBuilder fx = new StringBuilder();
        if (Native.getOption("aniso") != 0) fx.append("Aniso: 16×  ");
        if (Native.getOption("ao_hires") != 0) fx.append("AO depth: full size");
        if (fx.length() > 0) l.add(fx.toString().trim());
        return l;
    }

    // "Qualcomm Adreno driver v762.24 (07/17/24)" -> "Qualcomm v762.24",
    // "turnip Mesa driver Mesa 26.3.0-devel (git-...)" -> "Turnip Mesa 26.3.0-devel"
    static String shortDriverName(String d) {
        int p = d.indexOf(" (");
        if (p > 0) d = d.substring(0, p);
        d = d.replace(" Adreno driver", "").replace("turnip Mesa driver Mesa", "Turnip Mesa").replace("Mesa driver Mesa", "Mesa");
        return d.length() > 32 ? d.substring(0, 31) + "…" : d;
    }

    // ---- used by the performance overlay page of OptionsMenu
    void setPerfHud(boolean on) {
        prefs.edit().putBoolean("perf_hud", on).apply();
        controls.setPerfHud(on);
    }

    void setPerfItem(int bit, boolean on) {
        int b = prefs.getInt("perf_items", ControlsView.PERF_ALL);
        b = on ? b | bit : b & ~bit;
        prefs.edit().putInt("perf_items", b).apply();
        controls.setPerfItems(b);
    }

    // drag mode: the overlay follows the finger until a tap elsewhere
    void movePerfOverlay() {
        if (!prefs.getBoolean("perf_hud", false)) setPerfHud(true);
        controls.setPerfMoveMode(true);
    }

    void resetPerfOverlay() {
        prefs.edit().remove("perf_x").remove("perf_y").apply();
        controls.setPerfPosition(-1, -1);
    }

    @Override
    public boolean dispatchTouchEvent(MotionEvent e) {
        // hidden because a controller is in use: touching the screen brings the controls back
        if (autoHidden && e.getActionMasked() == MotionEvent.ACTION_DOWN) {
            autoHidden = false;
            applyControlsAppearance();
        }
        if (e.getActionMasked() == MotionEvent.ACTION_DOWN && lastController != null) inputSource(null);  // playing by touch
        return super.dispatchTouchEvent(e);
    }

    @Override
    public boolean dispatchKeyEvent(KeyEvent e) {
        if (!started) return super.dispatchKeyEvent(e);
        int code = e.getKeyCode();
        boolean controller = InputMapper.isController(e.getDevice()) || KeyEvent.isGamepadButton(code);
        if (code == KeyEvent.KEYCODE_BACK && !controller) {
            if (e.getAction() == KeyEvent.ACTION_UP) onMenu();
            return true;
        }
        if (code == KeyEvent.KEYCODE_VOLUME_UP || code == KeyEvent.KEYCODE_VOLUME_DOWN || code == KeyEvent.KEYCODE_VOLUME_MUTE)
            return super.dispatchKeyEvent(e);
        if (!controller && e.getAction() == KeyEvent.ACTION_DOWN && e.getRepeatCount() == 0 && hotkey(code)) return true;
        if (controller && (code == KeyEvent.KEYCODE_BUTTON_SELECT || code == KeyEvent.KEYCODE_BACK)) {
            selectButton(e);
            return true;
        }
        if (controller && code == KeyEvent.KEYCODE_BUTTON_MODE) {  // Home / Guide, where the system passes it on
            if (e.getAction() == KeyEvent.ACTION_UP) onMenu();
            return true;
        }
        if (mapper.onKey(e)) {
            if (controller) controllerUsed(e.getDevice());
            pushInput();
            return true;
        }
        return super.dispatchKeyEvent(e);
    }

    // A controller's Select (View): held for 0.6 s it opens the menu, a shorter press is the game's
    // - button (sent on release, held for a few frames so the game sees it)
    private static final long SELECT_HOLD_MS = 600;
    private final android.os.Handler keyHandler = new android.os.Handler(android.os.Looper.getMainLooper());
    private boolean selectDown, selectOpenedMenu;
    private final Runnable selectHeld = () -> {
        if (!selectDown) return;
        selectOpenedMenu = true;
        onMenu();
    };

    private void selectButton(KeyEvent e) {
        controllerUsed(e.getDevice());
        if (e.getAction() == KeyEvent.ACTION_DOWN) {
            if (e.getRepeatCount() > 0) return;
            selectDown = true;
            selectOpenedMenu = false;
            keyHandler.postDelayed(selectHeld, SELECT_HOLD_MS);
        } else if (e.getAction() == KeyEvent.ACTION_UP) {
            // the press must have been ours: holding Select also closes the menu, and that release
            // arrives here after the menu is gone
            boolean pressedHere = selectDown;
            selectDown = false;
            keyHandler.removeCallbacks(selectHeld);
            if (!pressedHere || selectOpenedMenu) return;
            int bits = mapper.bitFor(KeyEvent.KEYCODE_BUTTON_SELECT);  // the − unless assigned otherwise
            mapper.pulse(bits, true);
            pushInput();
            keyHandler.postDelayed(() -> {
                mapper.pulse(bits, false);
                pushInput();
            }, 80);
        }
    }

    // single-key shortcuts as in the macOS build: O cycles ambient occlusion, M full-size occlusion
    // depth, N anisotropic filtering, P capture
    private boolean hotkey(int code) {
        switch (code) {
            case KeyEvent.KEYCODE_O: setAo((Native.getOption("ao_mode") + 1) % 3); return true;
            case KeyEvent.KEYCODE_M: setBool("ao_hires", Native.getOption("ao_hires") == 0); return true;
            case KeyEvent.KEYCODE_N: setBool("aniso", Native.getOption("aniso") == 0); return true;
            case KeyEvent.KEYCODE_P: case KeyEvent.KEYCODE_F12: Native.setOption("capture", 1); return true;
            default: return false;
        }
    }

    @Override
    public boolean dispatchGenericMotionEvent(MotionEvent e) {
        if (started && mapper.onMotion(e)) {
            controllerUsed(e.getDevice());
            pushInput();
            return true;
        }
        return super.dispatchGenericMotionEvent(e);
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) hideSystemBars();
        else if (started) {
            mapper.reset();
            pushInput();
        }
    }

    // ------------------------------------------------------------------ lifecycle
    private boolean resumed;

    @Override
    protected void onPause() {
        super.onPause();
        resumed = false;
        // Android may end the app any time once it's in the background, without a word: the moment it
        // leaves is the last chance for a state (taken now while the game still runs, written after)
        if (started && prefs.getBoolean("leave_state", false)) Native.saveStateOnLeave();
        updateDrcDisplay();
        if (started) Native.setPaused(true);
        if (motion != null) motion.stop();
        if (rumble != null) rumble.stop();
    }

    @Override
    protected void onResume() {
        super.onResume();
        resumed = true;
        updateDrcDisplay();
        if (started) {
            Native.setPaused(false);
            startMotion();
        }
        hideSystemBars();
    }

    @Override
    protected void onDestroy() {
        if (drcDisplay != null) drcDisplay.release();
        if (instance == this) instance = null;
        super.onDestroy();
    }

    // ------------------------------------------------------------------ menu
    /** Auto save states: every `minutes` of play into the two auto slots, 0 off. */
    void setAutoState(int minutes) {
        Native.setOption("auto_state", minutes);
        prefs.edit().putInt("auto_state", minutes).apply();
    }

    void setAo(int mode) {
        Native.setOption("ao_mode", mode);
        prefs.edit().putInt("ao_mode", mode).apply();
    }

    // ---- controller buttons (OptionsMenu's Controller buttons page)
    InputMapper inputMapper() { return mapper; }

    void assignButton(int wiiuIndex, int code) {
        mapper.assign(wiiuIndex, code);
        prefs.edit().putString("pad_map", mapper.mapString()).apply();
    }

    void resetButtons() {
        mapper.loadMap("");
        prefs.edit().remove("pad_map").apply();
    }

    /** inverted camera up/down: 0 off, 1 first person and aiming, 2 always */
    void setInvertCameraY(int mode) {
        prefs.edit().putInt("invert_camera_y_mode", mode).apply();
        Native.setOption("invert_camera_y", mode);
    }

    void setBool(String key, boolean v) {
        Native.setOption(key, v ? 1 : 0);
        prefs.edit().putBoolean(key, v).apply();
    }

    @Override
    public void onMenu() {
        new OptionsMenu(this).show();
    }

    // ---- actions of the in-game menu (OptionsMenu)
    static final float[] CONTROL_SIZES = {0.75f, 0.9f, 1f, 1.15f, 1.3f};
    // swipe speed factors 0.25 to 4 in steps of 0.25, shown relative to the default 2: -175 to +200
    static final float[] SWIPE_SPEEDS = new float[16];
    static { for (int i = 0; i < SWIPE_SPEEDS.length; i++) SWIPE_SPEEDS[i] = (i + 1) * 0.25f; }
    static final float SWIPE_SPEED_DEFAULT = 2f;

    // render targets switch to the new size as the game next draws into them
    void setResolution(int i) {
        if (RES_SCALES[i].equals(prefs.getString("res_scale", "1"))) return;
        prefs.edit().putString("res_scale", RES_SCALES[i]).commit();
        Native.setOption("res_scale", Math.round(Float.parseFloat(RES_SCALES[i]) * 100));
        controls.resetPerfAverage();
    }

    void setMod(String key, boolean on) {
        prefs.edit().putBoolean(key, on).apply();
        Native.setOption(key, on ? 1 : 0);
        if (key.equals("mod_climb")) controls.setClimbHud(on);
    }

    // Mods > Jump anywhere and Mods > Moon jump both use L, so one switches the other off (the moon jump
    // keeps its cheat_ pref and option names from when it was among the cheats)
    void setJumpAnywhere(boolean on) {
        setMod("mod_jump", on);
        if (on && prefs.getBoolean("cheat_moon_jump", false)) {
            setMod("cheat_moon_jump", false);
            android.widget.Toast.makeText(this, R.string.cheat_moon_jump_off, android.widget.Toast.LENGTH_SHORT).show();
        }
    }

    void setMoonJump(boolean on) {
        setMod("cheat_moon_jump", on);
        if (on && prefs.getBoolean("mod_jump", false)) {
            setMod("mod_jump", false);
            android.widget.Toast.makeText(this, R.string.opt_mod_jump_off, android.widget.Toast.LENGTH_SHORT).show();
        }
    }

    void setControlsVisible(boolean on) {
        prefs.edit().putBoolean("controls_visible", on).apply();
        autoHidden = false;
        applyControlsAppearance();
    }

    void askClearShaders() {
        new GameDialog(this).title(R.string.opt_shaders).message(R.string.clear_shaders_text)
                .button(R.string.opt_cancel, null)
                .button(R.string.clear_shaders_restart, () -> restartApp(true)).show();
    }

    // with "Save state when leaving the app" the last session is saved first (as onPause would, which
    // never runs here) and the app waits until the file is written (seconds: it is written gently)
    void quitApp() {
        if (!started || !prefs.getBoolean("leave_state", false)) {
            exitNow();
            return;
        }
        Native.saveStateOnLeave();
        if (Native.stateWritesPending() == 0) {
            exitNow();
            return;
        }
        new GameDialog(this).title(R.string.opt_quit).message(R.string.quit_saving).cancelable(false).show();
        final long until = android.os.SystemClock.uptimeMillis() + 60000;
        final android.os.Handler h = new android.os.Handler(android.os.Looper.getMainLooper());
        h.postDelayed(new Runnable() {
            long pictureBy;

            @Override
            public void run() {
                // the state, then its picture (written on its own, a moment later; at most 2 s more)
                File bin = stateFile(baseDir(), 8, "bin"), png = stateFile(baseDir(), 8, "png");
                long now = android.os.SystemClock.uptimeMillis();
                if (Native.stateWritesPending() != 0 && now < until) { h.postDelayed(this, 200); return; }
                if (pictureBy == 0) pictureBy = now + 2000;
                if (png.lastModified() + 1000 < bin.lastModified() && now < pictureBy) h.postDelayed(this, 100);
                else exitNow();
            }
        }, 200);
    }

    private void exitNow() {
        finishAndRemoveTask();
        System.exit(0);
    }

    String onOff(boolean on) { return getString(on ? R.string.on : R.string.off); }

    // ------------------------------------------------------------------ resolution
    static final String[] RES_SCALES = {"0.5", "0.75", "1", "1.5", "2", "3"};

    String resolutionLabel(String scale) {
        float f;
        try {
            f = Float.parseFloat(scale);
        } catch (NumberFormatException e) {
            f = 1;
        }
        // the game draws its 3D view at 1280x720
        String view = Math.round(720 * f) + "p";
        if (f == 1) return getString(R.string.res_original, view);
        return getString(f > 1 ? R.string.res_scaled_up : R.string.res_scaled_down, scale, view);
    }

    // ------------------------------------------------------------------ save states
    // 5 slots (runtime/src/savestate.cpp): the whole running game, kept in files/states
    void saveState(int slot) {
        Native.saveState(slot);
        showStateResult();
    }

    // the save or load happens at the next frame boundary: show its result when it's there
    void showStateResult() {
        final String before = Native.saveStateMessage();
        final long until = android.os.SystemClock.uptimeMillis() + 15000;
        final android.os.Handler h = new android.os.Handler(android.os.Looper.getMainLooper());
        h.postDelayed(new Runnable() {
            @Override
            public void run() {
                String m = Native.saveStateMessage();
                if (!m.isEmpty() && !m.equals(before)) {
                    // a loaded state brings its controls along: keep the setting in step
                    prefs.edit().putBoolean("pro_controller", Native.getOption("pro_controller") != 0).apply();
                    android.widget.Toast.makeText(MainActivity.this, m, android.widget.Toast.LENGTH_SHORT).show();
                } else if (android.os.SystemClock.uptimeMillis() < until) {
                    h.postDelayed(this, 200);
                }
            }
        }, 200);
    }

    // ------------------------------------------------------------------ gameplay mods
    // optional changes to how the game plays (runtime/src/mods), all off by default
    // cheats (mods/cheats.cpp): the one-shot ones' bits (mods.h Cheat) and the switches kept as settings
    static final int CHEAT_ITEMS = 1, CHEAT_SWORD = 2, CHEAT_STATS = 4, CHEAT_SONGS = 8, CHEAT_TRIFORCE = 16, CHEAT_DUNGEON = 32,
                     CHEAT_KEY = 64, CHEAT_BAGS = 128;
    static final String[] CHEATS_INFINITE = {"cheat_inf_health", "cheat_inf_magic", "cheat_inf_ammo", "cheat_moon_jump", "cheat_moon_nofall", "cheat_moon_anims",
                                             "cheat_inf_rupees", "cheat_inf_bags", "cheat_inf_breath"};

    void cheat(int bit, int done) {
        Native.setOption("cheat", bit);
        android.widget.Toast.makeText(this, getString(done) + " " + getString(R.string.cheat_keep), android.widget.Toast.LENGTH_LONG).show();
    }

    static final String[] MODS = {"mod_direct_camera", "mod_first_person", "mod_climb", "mod_quick_doors", "mod_fast_scenes",
                                  "mod_ff_cutscenes", "mod_ff_dialogues"};
    static final int[] CAMERA_SPEEDS = {50, 100, 150, 200};
    static final int[] RUN_SPEEDS = {100, 125, 150, 200, 250, 300, 400};  // 100: off
    static final int[] MOON_RISE_SPEEDS = {50, 75, 100, 150, 200, 300};  // moon jump rise, percent
    static final int[] MOON_FLY_SPEEDS = new int[20];  // moon jump flight, percent: 50..1000 in steps of 50
    static { for (int i = 0; i < MOON_FLY_SPEEDS.length; i++) MOON_FLY_SPEEDS[i] = 50 * (i + 1); }

    // faster running ("mod_run"), swimming ("mod_swim"), climbing ("mod_climb_speed"; "mod_climb" itself is
    // Climb any wall) and sidling ("mod_sidle") each apply always, or with L3: held, or one press switches
    // it on and off
    /** one of them is on and works with L3 (the context buttons then show L3) */
    boolean l3InUse() {
        for (String mod : new String[] {"mod_run", "mod_swim", "mod_climb", "mod_sidle"})
            if (prefs.getInt(mod + "_speed", 100) != 100 && prefs.getBoolean(mod + "_l3", false)) return true;
        return false;
    }

    int moveMode(String mod) {
        if (!prefs.getBoolean(mod + "_l3", false)) return 0;
        return prefs.getBoolean(mod + "_l3_hold", false) ? 1 : 2;
    }

    void setMoveL3(String mod, boolean withL3, boolean hold) {
        prefs.edit().putBoolean(mod + "_l3", withL3).putBoolean(mod + "_l3_hold", hold).apply();
        Native.setOption(mod + "_mode", moveMode(mod));
    }

    // ------------------------------------------------------------------ game from a disc image
    // The user picks their .wux/.wud image, its disc key (same name, .key) and the console's common
    // key (common.key), or a Cemu .wua archive (decrypted: no keys; used first); the game is
    // extracted into files/game on the device.

    void pickDiscFiles() {
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        i.addCategory(Intent.CATEGORY_OPENABLE);
        i.setType("*/*");
        i.putExtra(Intent.EXTRA_ALLOW_MULTIPLE, true);
        startActivityForResult(i, PICK_DISC_FILES);
    }

    // the files the user picked (instead of a folder)
    private void startExtraction(List<android.net.Uri> uris) {
        List<String[]> files = new ArrayList<>();
        for (android.net.Uri u : uris)
            try (android.database.Cursor c = getContentResolver().query(u, new String[] {android.provider.OpenableColumns.DISPLAY_NAME}, null, null, null)) {
                if (c != null && c.moveToFirst() && c.getString(0) != null) files.add(new String[] {c.getString(0), u.toString()});
            }
        extractFrom(files);
    }

    private void extractFrom(List<String[]> files) {
        android.content.ContentResolver cr = getContentResolver();
        android.net.Uri image = null, discKey = null, commonKey = null, archive = null;
        String imageName = null;
        List<String[]> keys = new ArrayList<>();  // other .key files: {name, uri}
        for (String[] f : files) {
            String name = f[0], lower = name.toLowerCase(java.util.Locale.ROOT);
            android.net.Uri u = android.net.Uri.parse(f[1]);
            if (lower.endsWith(".wua") && archive == null) {
                archive = u;
            } else if ((lower.endsWith(".wux") || lower.endsWith(".wud")) && image == null) {
                image = u;
                imageName = name.substring(0, name.length() - 4);
            } else if (lower.equals("common.key")) {
                commonKey = u;
            } else if (lower.endsWith(".key")) {
                keys.add(f);
            }
        }
        if (image != null)
            for (String[] k : keys)  // the disc key: the image's name with .key, else the only other key
                if (k[0].substring(0, k[0].length() - 4).equalsIgnoreCase(imageName))
                    discKey = android.net.Uri.parse(k[1]);
        if (discKey == null && keys.size() == 1)
            discKey = android.net.Uri.parse(keys.get(0)[1]);
        if (archive != null) {
            image = archive;
            discKey = commonKey = null;
        } else if (image == null || discKey == null || commonKey == null) {
            new AlertDialog.Builder(this).setTitle(R.string.setup_extract)
                    .setMessage(getString(R.string.extract_missing, image == null ? "✗" : "✓", discKey == null ? "✗" : "✓",
                            commonKey == null ? "✗" : "✓"))
                    .setPositiveButton(android.R.string.ok, null).show();
            return;
        }
        byte[] dk = null, ck = null;
        int fd;
        try {
            if (archive == null) {
                dk = Native.parseKey(readAll(cr, discKey));
                ck = Native.parseKey(readAll(cr, commonKey));
                if (dk == null || ck == null) throw new IOException(getString(R.string.extract_bad_key));
            }
            android.os.ParcelFileDescriptor pfd = cr.openFileDescriptor(image, "r");
            if (pfd == null) throw new IOException("cannot open the image");
            fd = pfd.detachFd();
        } catch (IOException e) {
            new AlertDialog.Builder(this).setTitle(R.string.setup_extract).setMessage(e.getMessage())
                    .setPositiveButton(android.R.string.ok, null).show();
            return;
        }
        if (!enoughSpace(baseDir(), 2000L << 20)) {
            try {
                android.os.ParcelFileDescriptor.adoptFd(fd).close();
            } catch (IOException ignored) {
            }
            return;
        }
        runExtraction(fd, dk, ck);
    }

    private static byte[] readAll(android.content.ContentResolver cr, android.net.Uri u) throws IOException {
        try (InputStream in = cr.openInputStream(u)) {
            if (in == null) throw new IOException("cannot read " + u);
            java.io.ByteArrayOutputStream out = new java.io.ByteArrayOutputStream();
            byte[] buf = new byte[4096];
            for (int n; (n = in.read(buf)) > 0; ) out.write(buf, 0, n);
            return out.toByteArray();
        }
    }

    // dk == null: fd is a .wua archive
    private void runExtraction(int fd, byte[] dk, byte[] ck) {
        File base = baseDir(), work = new File(base, "game-extracting"), game = new File(gameDir());
        askNotifications();
        boolean ok = WorkService.Work.start(this, WorkService.Work.EXTRACT, () -> {
            Backup.deleteTree(work);
            String err = dk == null ? Native.extractArchive(fd, work.getAbsolutePath())
                                    : Native.extractGame(fd, dk, ck, work.getAbsolutePath());
            if (err == null) {  // complete: swap it in for the old game folder
                Native.buildWarps(work.getAbsolutePath());  // the developer warp menu's places (a few seconds; logged if not)
                Backup.deleteTree(game);
                if (!work.renameTo(game)) err = "cannot move the extracted files into place";
            } else {
                Backup.deleteTree(work);
            }
            return err;
        });
        if (ok) showWorkScreen(WorkService.Work.EXTRACT);
    }

    // ------------------------------------------------------------------ extraction and compile screens
    // The work runs in WorkService.Work (with a notification, so it continues in the background);
    // this screen shows its progress, also when the app is opened again while it runs.
    private void showWorkScreen(int kind) {
        boolean extract = kind == WorkService.Work.EXTRACT;
        LinearLayout box = new LinearLayout(this);
        box.setOrientation(LinearLayout.VERTICAL);
        int pad = (int) (24 * getResources().getDisplayMetrics().density);
        box.setPadding(pad, pad, pad, pad);
        TextView t = new TextView(this);
        t.setTextSize(16);
        t.setText(extract ? R.string.extract_running : R.string.compile_running);
        box.addView(t);
        android.widget.ProgressBar bar = new android.widget.ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal);
        bar.setMax(1000);
        box.addView(bar);
        TextView detail = new TextView(this);
        box.addView(detail);
        Button cancel = new Button(this);
        cancel.setText(android.R.string.cancel);
        cancel.setOnClickListener(v -> {  // a compile's parts in progress still finish (up to half a minute)
            WorkService.Work.markCancelled();
            if (extract) Native.extractCancel();
            else Native.compileCancel();
            cancel.setEnabled(false);
            cancel.setText(R.string.compile_stopping);
        });
        box.addView(cancel, new LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT));
        setContentView(seaScreen(box));
        final SeaView mySea = sea;
        final android.os.Handler h = new android.os.Handler(android.os.Looper.getMainLooper());
        h.post(new Runnable() {
            @Override
            public void run() {
                if (WorkService.Work.running() != kind || sea != mySea) return;  // finished, or another screen
                long[] p = extract ? Native.extractProgress() : Native.compileProgress();
                long s = WorkService.Work.elapsedSeconds();
                if (p[1] > 0) {
                    bar.setProgress((int) (p[0] * 1000 / p[1]));
                    mySea.setProgress((float) p[0] / p[1]);
                }
                if (extract) {
                    if (p[1] > 0) detail.setText(getString(R.string.extract_detail, p[0] / 1048576, p[1] / 1048576, s > 0 ? p[0] / 1048576.0 / s : 0));
                } else {
                    detail.setText(getString(R.string.compile_detail, p[0], p[1], s / 60, s % 60));
                }
                h.postDelayed(this, 500);
            }
        });
    }

    /** A job of WorkService.Work finished (called on the main thread, possibly in a later activity). */
    void workFinished(int kind, String err, boolean cancelled) {
        if (kind == WorkService.Work.EXTRACT) {
            if (err == null) checkAndStart();  // checks the extracted executable against this build
            else showSetup(getString(R.string.extract_failed, err));
        } else if (err == null && !cancelled) {
            checkAndStart();
        } else {
            showCompileStopped(err == null || err.equals("cancelled") ? getString(R.string.compile_cancelled) : getString(R.string.compile_failed, err));
        }
    }

    // the progress notification needs the permission on Android 13+ (the work runs without it); asked once
    private void askNotifications() {
        if (android.os.Build.VERSION.SDK_INT < 33 || prefs.getBoolean("asked_notifications", false)) return;
        prefs.edit().putBoolean("asked_notifications", true).apply();
        if (checkSelfPermission(android.Manifest.permission.POST_NOTIFICATIONS) != android.content.pm.PackageManager.PERMISSION_GRANTED)
            requestPermissions(new String[] {android.Manifest.permission.POST_NOTIFICATIONS}, 7);
    }

    // free space in `dir` is at least `need` bytes; else explains and returns false
    private boolean enoughSpace(File dir, long need) {
        long free = new android.os.StatFs(dir.getAbsolutePath()).getAvailableBytes();
        if (free >= need) return true;
        new AlertDialog.Builder(this).setTitle(R.string.space_title)
                .setMessage(getString(R.string.space_text, need / 1048576, free / 1048576))
                .setPositiveButton(android.R.string.ok, null).show();
        return false;
    }

    // ------------------------------------------------------------------ game code
    // Builds that contain no game code recompile it on the device (once per app version and game
    // files); the runtime loads it from here at every start.
    private File codeDir() { return new File(getNoBackupFilesDir(), "codecache"); }

    private void runCompile() {
        String game = gameDir(), dir = codeDir().getAbsolutePath();
        if (!enoughSpace(getNoBackupFilesDir(), 300L << 20)) {
            showCompileStopped(getString(R.string.compile_failed, getString(R.string.space_title)));
            return;
        }
        askNotifications();
        if (WorkService.Work.start(this, WorkService.Work.COMPILE, () -> Native.compileGame(game, dir)))
            showWorkScreen(WorkService.Work.COMPILE);
    }

    void showLicenses() {
        TextView t = new TextView(this);
        t.setTextSize(12);
        t.setTextColor(GameUi.INK);
        t.setLinkTextColor(0xFF1F5FA8);
        t.setTextIsSelectable(true);
        t.setText(Native.licenses());
        android.text.util.Linkify.addLinks(t, android.text.util.Linkify.WEB_URLS);
        GameDialog d = new GameDialog(this).title(R.string.menu_about).content(t);
        java.util.List<File> logs = CrashLogs.list(this, prefs);
        d.button(R.string.crash_share_all, () -> CrashLogs.share(this, prefs, logs), !logs.isEmpty());
        d.button(R.string.opt_ok, null).show();
    }

    private void showCompileStopped(String message) {
        LinearLayout box = new LinearLayout(this);
        box.setOrientation(LinearLayout.VERTICAL);
        int pad = (int) (24 * getResources().getDisplayMetrics().density);
        box.setPadding(pad, pad, pad, pad);
        TextView t = new TextView(this);
        t.setTextSize(16);
        t.setText(message);
        t.setTextIsSelectable(true);
        box.addView(t);
        Button resume = new Button(this);
        resume.setText(R.string.compile_continue);
        resume.setOnClickListener(v -> checkAndStart());
        box.addView(resume, new LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT));
        setContentView(seaScreen(box));
    }

    // ------------------------------------------------------------------ frame generation
    // Lossless Scaling's frame generation (LSFG 3), with the shaders from the user's own copy of
    // Lossless.dll, which the file picker copies into the app's private storage.
    private static final int PICK_DLL = 1;
    static final String[] FG_FLOW_SCALES = {"0.25", "0.5", "0.75", "1"};

    private File frameGenDll() { return new File(new File(getFilesDir(), "lsfg"), "Lossless.dll"); }

    private boolean frameGenOn() { return prefs.getBoolean("fg_enabled", false) && frameGenDll().exists(); }

    // the frame rates the Frame rate setting offers: off, 60, and 120 where the display shows it
    int[] interpRates() {
        return maxRefreshHz() >= 118 ? new int[] {0, 60, 120} : new int[] {0, 60};
    }

    // the display's highest refresh rate (at least 60)
    int maxRefreshHz() {
        float hz = 60;
        for (android.view.Display.Mode m : getWindowManager().getDefaultDisplay().getSupportedModes())
            hz = Math.max(hz, m.getRefreshRate());
        return Math.round(hz);
    }

    // frames shown per second without frame generation: 30, or the interpolated rate
    int shownFps() {
        int f = Native.getOption("interp_fps");
        return f > 0 ? f : 30;
    }

    void setInterpFps(int fps) {
        prefs.edit().putInt("interp_fps", fps).apply();
        Native.setOption("interp_fps", fps);
    }

    private void applyFrameGenSettings() {
        // the renderer generates frames only if the display can show them (also when switched on later)
        float hz = 60;
        for (android.view.Display.Mode m : getWindowManager().getDefaultDisplay().getSupportedModes())
            hz = Math.max(hz, m.getRefreshRate());
        setenv("WWHD_DISPLAY_HZ", String.valueOf(Math.round(hz)));
        if (!frameGenOn()) return;
        setenv("WWHD_LSFG_DLL", frameGenDll().getAbsolutePath());
        setenv("WWHD_LSFG_MULTIPLIER", String.valueOf(prefs.getInt("fg_multiplier", 2)));
        setenv("WWHD_LSFG_QUALITY", prefs.getBoolean("fg_quality", false) ? "1" : "0");
        setenv("WWHD_LSFG_FLOW_SCALE", prefs.getString("fg_flow_scale", "0.5"));
        setenv("WWHD_LSFG_UI_DETECTION", prefs.getBoolean("fg_ui_detection", true) ? "1" : "0");
    }

    /** the frame generation settings as they are now, applied from the game's next frame on */
    private void applyFrameGenNow() {
        Native.applyFrameGen(frameGenOn(), frameGenDll().getAbsolutePath(), prefs.getBoolean("fg_quality", false),
                Float.parseFloat(prefs.getString("fg_flow_scale", "0.5")), prefs.getInt("fg_multiplier", 2),
                prefs.getBoolean("fg_ui_detection", true));
    }

    String frameGenLabel() {
        if (!frameGenDll().exists()) return getString(R.string.fg_no_dll);
        if (!prefs.getBoolean("fg_enabled", false)) return getString(R.string.off);
        if (!Native.frameGenError().isEmpty()) return getString(R.string.fg_not_working);
        return "×" + prefs.getInt("fg_multiplier", 2);
    }

    // ---- used by the frame generation page of OptionsMenu
    boolean hasFrameGenDll() { return frameGenDll().exists(); }

    boolean frameGenDllTested() { return Native.frameGenDllTested(frameGenDll().getAbsolutePath()); }

    void setFrameGenMultiplier(int m) {
        if (m == prefs.getInt("fg_multiplier", 2)) return;
        prefs.edit().putInt("fg_multiplier", m).commit();
        frameGenChanged();
    }

    void setFrameGenFlow(String scale) {
        if (scale.equals(prefs.getString("fg_flow_scale", "0.5"))) return;
        prefs.edit().putString("fg_flow_scale", scale).commit();
        frameGenChanged();
    }

    void removeFrameGenDll() {
        //noinspection ResultOfMethodCallIgnored
        frameGenDll().delete();
        prefs.edit().putBoolean("fg_enabled", false).commit();
        frameGenChanged();
    }

    void setFrameGen(String key, boolean v) {
        prefs.edit().putBoolean(key, v).commit();
        frameGenChanged();
    }

    // applied while the game runs (the renderer rebuilds the network at its next frame)
    private void frameGenChanged() {
        applyFrameGenNow();
    }

    void pickDll() {
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        i.addCategory(Intent.CATEGORY_OPENABLE);
        i.setType("*/*");
        startActivityForResult(i, PICK_DLL);
    }

    // ------------------------------------------------------------------ import / export
    // the game save (files/save) and the save states (files/states) to and from a folder the user picks
    static final int PICK_EXPORT = 2, PICK_DISC_FILES = 6;
    private boolean exportSave = true, exportStates = true;

    void chooseExport() {
        long states = 0;
        File[] files = new File(baseDir(), "states").listFiles((dir, n) -> Backup.STATE_FILE.matcher(n).matches());
        if (files != null)
            for (File f : files) states += f.length();
        boolean[] checked = {true, states > 0};
        new GameDialog(this).title(R.string.backup_export_title)
                .toggle(getString(R.string.backup_game_save), checked[0], on -> checked[0] = on)
                .toggle(getString(R.string.backup_states, Math.round(states / 1048576.0)), checked[1], on -> checked[1] = on)
                .button(R.string.opt_cancel, null)
                .button(R.string.backup_save_file, () -> {
                    exportSave = checked[0];
                    exportStates = checked[1];
                    if (!exportSave && !exportStates) return;
                    // one file ("save as"): some devices' folder picker can't choose or make folders
                    Intent i = new Intent(Intent.ACTION_CREATE_DOCUMENT);
                    i.addCategory(Intent.CATEGORY_OPENABLE);
                    i.setType("application/zip");
                    i.putExtra(Intent.EXTRA_TITLE, Backup.exportName());
                    startActivityForResult(i, PICK_EXPORT);
                }).show();
    }

    // the backup's files picked themselves (some devices' folder picker shows no folders)
    private static final int PICK_IMPORT_FILES = 8;

    // ---- mod packs (ModPacks): zip files with Cemu graphic packs; changes apply after a restart
    private static final int PICK_PACKS = 9;
    boolean packsChanged;  // enabled packs or presets changed since the start (the menu offers a restart)
    boolean packsAsked;  // the restart was offered for the current changes ("Later": not again until the next change)

    void markPacksChanged() {
        packsChanged = true;
        packsAsked = false;
    }

    // leaving the mod packs or the menu with changes that need a restart: offer it once per change;
    // returns false (and runs nothing) when there is nothing to ask, else `later` runs on "Later"
    boolean askRestartForPacks(Runnable later) {
        if (!packsChanged || packsAsked || isFinishing() || isDestroyed()) return false;
        packsAsked = true;
        new GameDialog(this).title(R.string.opt_packs).message(R.string.packs_restart_ask).cancelable(false)
                .button(R.string.res_restart_later, later)
                .button(R.string.packs_restart, this::restartForPacks).show();
        return true;
    }
    private Runnable afterPackImport;

    void pickPacks(Runnable after) {
        afterPackImport = after;
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        i.addCategory(Intent.CATEGORY_OPENABLE);
        i.setType("*/*");
        i.putExtra(Intent.EXTRA_ALLOW_MULTIPLE, true);
        startActivityForResult(i, PICK_PACKS);
    }

    private void importPacks(List<android.net.Uri> uris) {
        withProgress(R.string.packs_importing, () -> {
            StringBuilder ok = new StringBuilder(), bad = new StringBuilder();
            for (android.net.Uri u : uris) {
                String name = u.getLastPathSegment();
                try (android.database.Cursor c = getContentResolver().query(u, new String[] {android.provider.OpenableColumns.DISPLAY_NAME}, null, null, null)) {
                    if (c != null && c.moveToFirst() && c.getString(0) != null) name = c.getString(0);
                }
                try {
                    for (String f : ModPacks.importArchive(baseDir(), gameDir(), getContentResolver(), u, name)) ok.append("\n• ").append(f);
                } catch (IOException | RuntimeException e) {
                    bad.append("\n• ").append(name).append(": ").append(e.getMessage());
                }
            }
            ModPacks.writeEnabled(baseDir(), prefs);
            String msg = "";
            if (ok.length() > 0) msg += getString(R.string.packs_imported) + ok;
            if (bad.length() > 0) msg += (msg.isEmpty() ? "" : "\n\n") + getString(R.string.packs_import_failed) + bad;
            return msg;
        }, res -> {
            new GameDialog(this).title(R.string.opt_packs).message(res.startsWith("!") ? res.substring(1) : res).button(R.string.opt_ok, null).show();
            if (afterPackImport != null) afterPackImport.run();
        });
    }

    void restartForPacks() { restartApp(); }

    void chooseImport() {
        new GameDialog(this).title(R.string.opt_import).message(R.string.backup_import_how)
                .button(R.string.opt_cancel, null)
                .button(R.string.backup_choose_files, () -> {
                    Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
                    i.addCategory(Intent.CATEGORY_OPENABLE);
                    i.setType("*/*");
                    i.putExtra(Intent.EXTRA_ALLOW_MULTIPLE, true);
                    startActivityForResult(i, PICK_IMPORT_FILES);
                }).show();
    }

    private void importFiles(List<android.net.Uri> uris) {
        List<String> names = new ArrayList<>();
        List<android.net.Uri> kept = new ArrayList<>();
        for (android.net.Uri u : uris)
            try (android.database.Cursor c = getContentResolver().query(u, new String[] {android.provider.OpenableColumns.DISPLAY_NAME}, null, null, null)) {
                if (c != null && c.moveToFirst() && c.getString(0) != null) {
                    names.add(c.getString(0));
                    kept.add(u);
                }
            }
        // a GameCube save (.gci): converted into a WWHD game save, its three files become the three
        // quest logs (GcSave, as the upstream gc2hd.py)
        for (int i = 0; i < names.size(); i++) {
            if (!names.get(i).toLowerCase(java.util.Locale.ROOT).endsWith(".gci")) continue;
            try {
                byte[] sav = GcSave.toCkingSav(readAll(getContentResolver(), kept.get(i)));
                File tmp = new File(getCacheDir(), "gc-import/cking.sav");
                //noinspection ResultOfMethodCallIgnored
                tmp.getParentFile().mkdirs();
                try (java.io.FileOutputStream out = new java.io.FileOutputStream(tmp)) {
                    out.write(sav);
                }
                Backup.Found f = Backup.fromFiles(List.of("cking.sav"), List.of(android.net.Uri.fromFile(tmp)),
                        getString(R.string.backup_from_gc, names.get(i)));
                chooseWhatToImport(f);
            } catch (GcSave.Unsupported | IOException e) {
                info(getString(R.string.backup_failed, e.getMessage()));
            }
            return;
        }
        // a backup zip (Export): read it through for what it holds
        for (int i = 0; i < names.size(); i++) {
            if (!names.get(i).toLowerCase(java.util.Locale.ROOT).endsWith(".zip")) continue;
            android.net.Uri zip = kept.get(i);
            String name = names.get(i);
            final Backup.Found[] found = {null};
            withProgress(R.string.backup_scanning, () -> {
                found[0] = Backup.scanZip(getContentResolver(), zip, name);
                return "";
            }, r -> {
                if (r.startsWith("!")) info(getString(R.string.backup_failed, r.substring(1)));
                else if (found[0].empty()) info(getString(R.string.backup_nothing_files));
                else chooseWhatToImport(found[0]);
            });
            return;
        }
        Backup.Found f = Backup.fromFiles(names, kept, getString(R.string.backup_picked_files));
        if (f.empty()) {
            info(getString(R.string.backup_nothing_files));
            return;
        }
        chooseWhatToImport(f);
    }

    /** runs `work` off the UI thread behind a progress dialog; `done` gets null or an error */
    private void withProgress(int message, java.util.concurrent.Callable<String> work, java.util.function.Consumer<String> done) {
        GameDialog progress = new GameDialog(this).message(message).cancelable(false);
        progress.show();
        new Thread(() -> {
            String r;
            try {
                r = work.call();
            } catch (Exception e) {
                r = "!" + e.getMessage();
            }
            final String res = r;
            runOnUiThread(() -> {
                progress.dismiss();
                done.accept(res);
            });
        }, "backup").start();
    }

    private void info(String msg) {
        new GameDialog(this).title(R.string.backup_title).message(msg).button(R.string.opt_ok, null).show();
    }

    private void doExport(android.net.Uri file) {
        withProgress(R.string.backup_exporting, () -> {
            Backup.export(getContentResolver(), file, baseDir(), exportSave, exportStates);
            return displayName(file);
        }, r -> info(r.startsWith("!") ? getString(R.string.backup_failed, r.substring(1)) : getString(R.string.backup_exported, r)));
    }

    private String displayName(android.net.Uri u) {
        try (android.database.Cursor c = getContentResolver().query(u, new String[] {android.provider.OpenableColumns.DISPLAY_NAME}, null, null, null)) {
            if (c != null && c.moveToFirst() && c.getString(0) != null) return c.getString(0);
        }
        return u.getLastPathSegment();
    }

    // what to import: the game save and/or the save states found
    private void chooseWhatToImport(Backup.Found f) {
        List<String> items = new ArrayList<>();
        List<Boolean> isSave = new ArrayList<>();
        if (!f.saveUser.isEmpty()) {
            items.add(getString(R.string.backup_import_save, f.saveFrom));
            isSave.add(true);
        }
        if (!f.slots().isEmpty()) {
            items.add(getString(R.string.backup_import_states, android.text.TextUtils.join(", ", f.slots())));
            isSave.add(false);
        }
        boolean[] checked = new boolean[items.size()];
        java.util.Arrays.fill(checked, true);
        GameDialog d = new GameDialog(this).title(R.string.backup_import_title);
        for (int i = 0; i < items.size(); i++) {
            final int k = i;
            d.toggle(items.get(i), true, on -> checked[k] = on);
        }
        d.button(R.string.opt_cancel, null).button(R.string.backup_import_go, () -> {
            boolean save = false, states = false;
            for (int i = 0; i < checked.length; i++) {
                if (!checked[i]) continue;
                if (isSave.get(i)) save = true;
                else states = true;
            }
            runImport(f, save, states);
        }).show();
    }

    private void runImport(Backup.Found f, boolean save, boolean states) {
        if (!save && !states) return;
        withProgress(R.string.backup_importing, () -> {
            if (f.zip != null) {
                Backup.importZip(getContentResolver(), f, baseDir(), save, states);
            } else {
                if (states) Backup.importStates(getContentResolver(), f, baseDir());
                if (save) Backup.importSave(getContentResolver(), f, baseDir());
            }
            return "";
        }, r -> {
            if (r.startsWith("!")) {
                info(getString(R.string.backup_failed, r.substring(1)));
                return;
            }
            if (!save) {
                info(getString(R.string.backup_imported_states));
                return;
            }
            // the running game has the old save in memory and would write it back: restart right away
            new GameDialog(this).title(R.string.backup_title).message(R.string.backup_imported_save).cancelable(false)
                    .button(R.string.res_restart_now, this::restartApp).show();
        });
    }

    @Override
    protected void onActivityResult(int request, int result, Intent data) {
        super.onActivityResult(request, result, data);
        if (request == PICK_DRIVER) {
            if (result == RESULT_OK && data != null && data.getData() != null) installDriver(data.getData());
            else driverPickDone();  // cancelled: continue (the start after a GPU hang)
            return;
        }
        if (request == PICK_PRESET_SAVE) {
            if (result == RESULT_OK && data != null) {
                List<android.net.Uri> uris = new ArrayList<>();
                if (data.getClipData() != null)
                    for (int i = 0; i < data.getClipData().getItemCount(); i++) uris.add(data.getClipData().getItemAt(i).getUri());
                else if (data.getData() != null)
                    uris.add(data.getData());
                if (!uris.isEmpty()) importPresetSaves(uris);
            }
            return;
        }
        if (request == PICK_DISC_FILES && result == RESULT_OK && data != null) {
            List<android.net.Uri> uris = new ArrayList<>();
            if (data.getClipData() != null)
                for (int i = 0; i < data.getClipData().getItemCount(); i++) uris.add(data.getClipData().getItemAt(i).getUri());
            else if (data.getData() != null)
                uris.add(data.getData());
            startExtraction(uris);
            return;
        }
        if (request == PICK_IMPORT_FILES) {
            if (result == RESULT_OK && data != null) {
                List<android.net.Uri> uris = new ArrayList<>();
                if (data.getClipData() != null)
                    for (int i = 0; i < data.getClipData().getItemCount(); i++) uris.add(data.getClipData().getItemAt(i).getUri());
                else if (data.getData() != null)
                    uris.add(data.getData());
                if (!uris.isEmpty()) importFiles(uris);
            }
            return;
        }
        if (request == PICK_PACKS) {
            if (result == RESULT_OK && data != null) {
                List<android.net.Uri> uris = new ArrayList<>();
                if (data.getClipData() != null)
                    for (int i = 0; i < data.getClipData().getItemCount(); i++) uris.add(data.getClipData().getItemAt(i).getUri());
                else if (data.getData() != null)
                    uris.add(data.getData());
                if (!uris.isEmpty()) importPacks(uris);
            }
            return;
        }
        if (request == PICK_EXPORT && result == RESULT_OK && data != null && data.getData() != null) {
            doExport(data.getData());
            return;
        }
        if (request != PICK_DLL || result != RESULT_OK || data == null || data.getData() == null) return;
        File dir = frameGenDll().getParentFile();
        //noinspection ResultOfMethodCallIgnored
        dir.mkdirs();
        File tmp = new File(dir, "Lossless.dll.tmp");
        String problem;
        try (InputStream in = getContentResolver().openInputStream(data.getData());
             OutputStream out = new FileOutputStream(tmp)) {
            if (in == null) throw new IOException("no data");
            byte[] buf = new byte[1 << 16];
            for (int n; (n = in.read(buf)) > 0; ) out.write(buf, 0, n);
            out.close();
            problem = Native.checkFrameGenDll(tmp.getAbsolutePath());
        } catch (IOException e) {
            problem = e.getMessage();
        }
        if (problem != null) {
            //noinspection ResultOfMethodCallIgnored
            tmp.delete();
            new GameDialog(this).title(R.string.fg_title).message(getString(R.string.fg_bad_dll, problem))
                    .button(R.string.opt_ok, null).show();
            return;
        }
        //noinspection ResultOfMethodCallIgnored
        tmp.renameTo(frameGenDll());
        prefs.edit().putBoolean("fg_enabled", true).commit();
        applyFrameGenNow();
        new GameDialog(this).title(R.string.fg_title)
                .message(Native.frameGenDllTested(frameGenDll().getAbsolutePath()) ? R.string.fg_dll_ok : R.string.fg_dll_untested)
                .button(R.string.opt_ok, null).show();
    }

    // the resolution applies to render targets as the game creates them: start a fresh process
    private void restartApp() { restartApp(false); }

    private void restartApp(boolean clearShaders) { restartApp(clearShaders, null); }

    /** restarts the game; `deleteAfter`: a file to delete once this process has ended */
    private void restartApp(boolean clearShaders, File deleteAfter) {
        Native.setPaused(true);  // also writes the pipeline cache
        android.content.Intent i = new android.content.Intent(this, RestartActivity.class);
        i.putExtra(RestartActivity.EXTRA_PID, android.os.Process.myPid());
        i.putExtra(RestartActivity.EXTRA_CLEAR_SHADERS, clearShaders);
        if (deleteAfter != null) i.putExtra(RestartActivity.EXTRA_DELETE, deleteAfter.getAbsolutePath());
        startActivity(i);
    }

    // ------------------------------------------------------------------ software keyboard
    void showTextInput(String initial, int maxLen) {
        mapper.reset();
        pushInput();
        EditText edit = new EditText(this);
        edit.setInputType(InputType.TYPE_CLASS_TEXT);
        edit.setSingleLine(true);
        if (maxLen > 0) edit.setFilters(new InputFilter[] {new InputFilter.LengthFilter(maxLen)});
        edit.setText(initial);
        edit.setSelection(edit.getText().length());
        // a field in the menus' style: light paper with a wooden frame
        edit.setTextColor(GameUi.INK);
        edit.setTextSize(24);
        edit.setTypeface(android.graphics.Typeface.create("sans-serif-medium", android.graphics.Typeface.NORMAL));
        int pad = GameUi.px(this, 14);
        edit.setPadding(pad, pad / 2, pad, pad / 2);
        android.graphics.drawable.GradientDrawable field = new android.graphics.drawable.GradientDrawable();
        field.setColor(0xFFFFFBF0);
        field.setCornerRadius(GameUi.px(this, 8));
        field.setStroke(GameUi.px(this, 3), 0xFF8B5A2B);
        edit.setBackground(field);
        // cursor and selection in the field's colours (not the theme's, which may vanish on paper)
        android.graphics.drawable.GradientDrawable cursor = new android.graphics.drawable.GradientDrawable();
        cursor.setColor(0xFF8B5A2B);
        cursor.setSize(GameUi.px(this, 2), GameUi.px(this, 24));
        edit.setTextCursorDrawable(cursor);
        edit.setHighlightColor(0x668B5A2B);
        edit.setImeOptions(android.view.inputmethod.EditorInfo.IME_ACTION_DONE | android.view.inputmethod.EditorInfo.IME_FLAG_NO_EXTRACT_UI);
        LinearLayout box = new LinearLayout(this);
        box.setPadding(0, GameUi.px(this, 14), 0, 0);
        box.addView(edit, new LinearLayout.LayoutParams(-1, -2));
        final boolean[] answered = {false};
        GameDialog dlg = new GameDialog(this).top().title(R.string.text_title).message(getString(R.string.text_message, maxLen))
                .content(box)
                .button(R.string.opt_cancel, () -> {
                    answered[0] = true;
                    Native.textInputDone(false, "");
                })
                .button(R.string.opt_ok, () -> {
                    answered[0] = true;
                    Native.textInputDone(true, edit.getText().toString());
                });
        dlg.setOnDismissListener(d -> {
            if (!answered[0]) Native.textInputDone(false, "");
            hideSystemBars();
        });
        edit.setOnEditorActionListener((v, action, e) -> {  // the keyboard's ✓: OK
            dlg.press(1);
            return true;
        });
        dlg.getWindow().setSoftInputMode(WindowManager.LayoutParams.SOFT_INPUT_STATE_ALWAYS_VISIBLE
                | WindowManager.LayoutParams.SOFT_INPUT_ADJUST_NOTHING);
        dlg.show();
        edit.requestFocus();
    }
}
