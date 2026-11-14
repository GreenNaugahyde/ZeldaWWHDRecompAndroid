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
import android.util.TypedValue;
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
    static final boolean NEW_TOUCH_CONTROLS = true;

    // screen layouts
    static final int LAYOUT_INSET = 0, LAYOUT_SIDE = 1, LAYOUT_TV = 2, LAYOUT_DRC_LARGE = 3, LAYOUT_ON_DEMAND = 4;
    // "TV, GamePad on demand": the GamePad picture is shown instead of the TV picture (screen button)
    private boolean drcShown;
    // the file select is shown as the GamePad picture (touchable) on a single screen, until it's left
    private boolean drcForFiles;
    private int front = -1;
    // The very first start (no game save when the game started): the game makes its save and shows "save data
    // created" with OK in its boot layout, then goes on to the file select. The app presses OK itself, once: the
    // message tells nothing the player needs (pref skip_save_message, on; hud_boot_message)
    private boolean firstStart;
    private int firstStartPresses;   // presses so far (the button takes them only once it has faded in: again until gone)
    private long firstStartSeen, firstStartPressed;
    private static final int FIRST_START_MAX_PRESSES = 6;

    private void firstStartMessage() {
        if (firstStartPresses >= FIRST_START_MAX_PRESSES || !firstStart || !started || !prefs.getBoolean("skip_save_message", true))
            return;
        long now = android.os.SystemClock.uptimeMillis();
        if (Native.getOption("boot_message") == 0) {
            if (firstStartPresses > 0) firstStartPresses = FIRST_START_MAX_PRESSES;  // gone after a press: done
            firstStartSeen = 0;
            return;
        }
        if (firstStartSeen == 0) firstStartSeen = now;
        if (now - firstStartSeen < 700 || now - firstStartPressed < 1500) return;
        firstStartPressed = now;
        firstStartPresses++;
        Log.i(TAG, "first start: the game's \"save data created\" confirmed (press " + firstStartPresses + ")");
        mapper.pulse(Native.A, true);
        pushInput();
        frontHandler.postDelayed(() -> {
            mapper.pulse(Native.A, false);
            pushInput();
        }, 150);
    }

    static final long LOW_MEMORY_MB = 4608;
    static final long VERY_LOW_MEMORY_MB = 3584;
    static final long AREA_HEADSTART_MB = 7168;

    // Android low on memory (it no longer tells running apps through onTrimMemory): the renderer frees the
    // pipelines unused lately (made again when needed); checked every 2 s, at most every 30 s
    private long memoryCheckedAt, memoryTrimmedAt;

    private void checkMemory() {
        long now = android.os.SystemClock.uptimeMillis();
        if (!started || now - memoryCheckedAt < 2000) return;
        memoryCheckedAt = now;
        android.app.ActivityManager am = (android.app.ActivityManager) getSystemService(ACTIVITY_SERVICE);
        if (am == null) return;
        android.app.ActivityManager.MemoryInfo mi = new android.app.ActivityManager.MemoryInfo();
        am.getMemoryInfo(mi);
        // save states check it before they start (savestate.cpp set_memory_headroom)
        Native.setOption("memory_headroom", (int) Math.max(0, (mi.availMem - mi.threshold) >> 20));
        // the line itself: dynamic resolution reads the free memory more often than this check (vk_dynres.cpp)
        Native.setOption("memory_threshold", (int) (mi.threshold >> 20));
        if ((mi.lowMemory || mi.availMem < mi.threshold + (256L << 20)) && now - memoryTrimmedAt > 30000) {
            memoryTrimmedAt = now;
            Log.i(TAG, "memory low: " + (mi.availMem >> 20) + " MB available (threshold " + (mi.threshold >> 20) + " MB)");
            Native.setOption("trim_memory", 1);
        }
    }

    private final Runnable frontPoll = new Runnable() {
        @Override
        public void run() {
            int f = Native.frontScreen();
            packFileSwitch(f);
            firstStartMessage();
            checkMemory();
            if (controls != null && !benchRunning && tour == null) {
                int[] ts = Native.touchState();
                controls.ringHint(ts);
                controls.windDialHint(ts);
                controls.pictoPages(ts);
            }
            boolean noPad = !controllerConnected();
            setFilesTouch(f == Native.FRONT_FILES && noPad);
            setMenuTouch(noPad && drcScreen() && Native.drcGameCursor());
            if (lastController == null) usePadMap(firstController());  // (the one the menu's buttons page edits)
            if (controls != null) {
                controls.setPadConnected(!noPad);
                setShortcutHidden(controls.dpadInRing());
                applySlotLabels(!noPad);
            }
            if (f != front && controls != null) {
                Log.i(TAG, "front screen " + f);  // (4 boot, 1 title, 2 file select, 3 opening, 0 play)
                front = f;
                controls.setFrontScreen(f);
                boolean files = f == Native.FRONT_FILES && !drcOnOwnDisplay();
                if (files && !drcShown) {
                    drcShown = drcForFiles = true;
                    updateLayout();
                } else if (!files && drcForFiles) {
                    drcShown = drcForFiles = false;
                    updateLayout();
                }
            }
            frontHandler.postDelayed(this, 250);
        }
    };
    private final android.os.Handler frontHandler = new android.os.Handler(android.os.Looper.getMainLooper());
    private static final float TV_ASPECT = 16f / 9f, DRC_ASPECT = 854f / 480f;

    SharedPreferences prefs;
    // held here: SharedPreferences keeps its listeners only weakly
    private final SharedPreferences.OnSharedPreferenceChangeListener crashInfoUpdater = (p, key) -> CrashLogs.updateInfo(this, p);
    private SurfaceView surface;
    private ControlsView controls;
    private final InputMapper mapper = new InputMapper();
    private boolean autoHidden;
    private int surfaceW, surfaceH;  // the game view's size (screen pixels)
    // Display resolution (Graphics): the surface's buffers can be smaller than the view; the display
    // hardware scales them up for free. Auto sizes them to what the picture needs: 1.5x the 3D view's
    // height (Sharp upscaling still enlarges), at least 720 lines, never more than the screen. On a 2560x1600 screen with a
    // Helio G99 / Mali-G57, presenting at full size cost ~6 ms of a 33 ms frame and Sharp another ~4.7 ms.
    // Full: the screen's own resolution (classic). WWHD_SURFACE_SCALE=<0.25..1> fixes the factor (tests).
    static final int DISPLAY_RES_AUTO = 0, DISPLAY_RES_FULL = 1;
    private float bufScale = 1;      // buffer pixels per view pixel, as the surface has it now
    private float bufRequested = 1;  // what was last asked for (setFixedSize)
    private float renderScale = 1;   // the rendering resolution setting (the 3D view is 720 lines at 1x)

    // the app's own texts in the menu language (Game > Menu language, English by default, #62;
    // res/values-xx, what isn't translated stays English)
    @Override
    protected void attachBaseContext(android.content.Context base) {
        android.content.res.Configuration c = new android.content.res.Configuration(base.getResources().getConfiguration());
        c.setLocale(new java.util.Locale(menuLanguage(base.getSharedPreferences("settings", MODE_PRIVATE))));
        super.attachBaseContext(base.createConfigurationContext(c));
    }

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        // the icon's shortcuts (res/xml/shortcuts.xml): this device as another's GamePad or screen, the game here
        // not started at all
        int padRole = BuildConfig.REMOTE_PLAY ? getIntent().getIntExtra(EXTRA_PAD_ROLE, 0) : 0;
        if (padRole != 0 && !libraryLoaded) {
            PadActivity.open(this, padRole);
            finish();
            return;
        }
        // Game > Other devices > Choose at the start: play here, the GamePad or the screen of another device (the start
        // page in PadActivity's process: nothing of the game loaded for the other two); "play here" comes back with
        // EXTRA_CHOSEN
        if (BuildConfig.REMOTE_PLAY && !libraryLoaded && state == null && !getIntent().getBooleanExtra(EXTRA_CHOSEN, false)
                && getSharedPreferences("settings", MODE_PRIVATE).getBoolean(PREF_START_CHOICE, false)) {
            PadActivity.open(this, PadActivity.ROLE_CHOOSE);
            finish();
            return;
        }
        instance = this;
        prefs = getSharedPreferences("settings", MODE_PRIVATE);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        getWindow().getAttributes().layoutInDisplayCutoutMode = WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
        // edge to edge on every Android version, as Android 15+ does anyway for targetSdk 35+ (issue #29: on Android 14
        // the decor kept the game's view clear of the camera cutout, a black strip beside it); the menus keep clear of
        // the cutout themselves (OptionsMenu, FullMap)
        getWindow().setDecorFitsSystemWindows(false);
        // testing (debug builds): --es bench "fg,show" runs the graphics benchmark at this start
        String benchExtra = debuggable() && !libraryLoaded ? getIntent().getStringExtra("bench") : null;
        // testing (not in published releases): --es shader_tour <slot> plays by itself (ShaderTour); with
        // --ez shader_tour_reset true from the start of its list
        if (!BuildConfig.PUBLIC_RELEASE && !libraryLoaded) {
            String t = getIntent().getStringExtra(ShaderTour.EXTRA);
            tourSlot = t != null ? Integer.parseInt(t) : 0;
            if (getIntent().getBooleanExtra("shader_tour_reset", false)) ShaderTour.reset(prefs);
            // --ei shader_tour_idx <n>: on from that spot (0-based; after another tour run in between)
            if (getIntent().hasExtra("shader_tour_idx")) ShaderTour.startAt(prefs, getIntent().getIntExtra("shader_tour_idx", 0));
            ShaderTour.storyName = getIntent().getStringExtra("shader_tour_story");
            ShaderTour.placesOnly = getIntent().getBooleanExtra("shader_tour_places_only", false);
            ShaderTour.fly = getIntent().getBooleanExtra("shader_tour_fly", false);
            ShaderTour.onlyStages = getIntent().getStringExtra("shader_tour_stages");
            ShaderTour.fight = getIntent().getBooleanExtra("shader_tour_fight", false);
            ShaderTour.talk = getIntent().getBooleanExtra("shader_tour_talk", false);
        }
        if (benchExtra != null)
            prefs.edit().putBoolean(Benchmark.PENDING, true).putBoolean(Benchmark.WITH_FG, benchExtra.contains("fg"))
                    .putBoolean(Benchmark.SHOW, benchExtra.contains("show")).putBoolean(Benchmark.DONE, true)
                    .putBoolean("bench_slow", benchExtra.contains("slow")).putBoolean(Benchmark.WIDE, benchExtra.contains("wide")).commit();
        // a new installation starts at the graphics preset guessed for this device (before the settings below are read)
        GraphicsPresets.firstStart(this, prefs, new File(gameDir()));
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
            if (interp != 0 && !Benchmark.pending(prefs)) setenv("WWHD_INTERP_FPS", String.valueOf(interp));
            // 60 fps (generated): in-between frames made on the GPU (vk/reproj.cpp); goes before LSFG
            if (prefs.getBoolean("fps_generated", false) && !Benchmark.pending(prefs)) setenv("WWHD_FPS_GENERATED", "1");
            // little RAM (4 GB devices report ~3.6-3.8 GB): no shader pipelines made ahead, a budget for them (vk_draw.cpp)
            if (GraphicsPresets.ramMb(this) < LOW_MEMORY_MB) setenv("WWHD_LOW_MEMORY", "1");
            // under ~3.5 GB (3 GB TV boxes; 4 GB phones report 3.6-3.8): no shader head start at all. It translated
            // ~91,000 shaders at the start and Android ended the game on a 3 GB Tegra X1 TV box; without it the game
            // ran there (the shaders come as they are first used). wwhd.env may set WWHD_HEADSTART (read after this)
            if (GraphicsPresets.ramMb(this) < VERY_LOW_MEMORY_MB) setenv("WWHD_HEADSTART", "0");
            // under 7 GB (6 GB devices report ~5.5 GB): shader pipelines made ahead only for the area the player
            // is in and its neighbours (~400 MB less than all of them; vk_headstart.cpp, area head start). A
            // shader tour builds all of them: its draws tag every recipe of a pipeline only when all were built
            if (tourSlot != 0) setenv("WWHD_AREA_HEADSTART", "0");
            else if (GraphicsPresets.ramMb(this) < AREA_HEADSTART_MB) setenv("WWHD_AREA_HEADSTART", "1");
            applyEnvironment();
            CrashLogs.deleteShared(this, prefs);  // shared in an earlier run
            // a shader dump (WWHD_DUMP_SHADERS=<files>/shaders, debugging) left from an earlier start
            if (Os.getenv("WWHD_DUMP_SHADERS") == null) Backup.deleteTree(new File(baseDir(), "shaders"));
            System.loadLibrary("wwhd");
            libraryLoaded = true;
        }
        // testing (not in published releases): WWHD_TV=1 in wwhd.env acts as a TV on a phone or tablet
        tv = isTv(this) || (!BuildConfig.PUBLIC_RELEASE && "1".equals(Os.getenv("WWHD_TV")));
        // (test only: WWHD_TV_1080=1 switches the display to 1080p; the user found the TV's mode change not nice,
        // the game's own surface at 1080 lines (updateBufferSize) is the default way)
        if (tv && GraphicsPresets.ramMb(this) < VERY_LOW_MEMORY_MB && "1".equals(Os.getenv("WWHD_TV_1080"))) displayMode1080();
        // saves a phone brought while the game ran here (PadHost): in place before the game reads them
        Backup.deleteTree(new File(baseDir(), PadLink.RECEIVING));  // (a transfer that broke off: never used)
        File syncIn = new File(baseDir(), PadLink.INCOMING);
        if (syncIn.isDirectory()) {
            try {
                PadLink.applySaves(baseDir(), syncIn);
            } catch (IOException e) {
                Log.w(TAG, "saves from the phone not applied", e);
            }
        }
        if (BuildConfig.REMOTE_PLAY && prefs.getBoolean("pad_host", tv)) startPadHost();  // (a TV by default; a phone or tablet when switched on)
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

    /** the language of the app's own texts (menus, hints, dialogs): its own setting, English unless chosen;
     *  only English or one of the release's languages (Menu language), whose letters the game's font has */
    static String menuLanguage(android.content.SharedPreferences p) {
        String l = p.getString("menu_language", "en");
        for (String s : LANGUAGES)
            if (s.equals(l)) return l;
        return "en";
    }

    String menuLanguage() { return menuLanguage(prefs); }

    void setMenuLanguage(String l) {
        prefs.edit().putString("menu_language", l).commit();
    }

    // the command recording thread applies from the next start
    void setRecordThread(int mode) {
        if (mode == prefs.getInt("record_thread", 0)) return;
        prefs.edit().putInt("record_thread", mode).commit();
        new GameDialog(this).title(R.string.opt_record_thread).message(R.string.record_thread_restart)
                .button(R.string.gpu_driver_later, null)
                .button(R.string.res_restart_now, this::restartApp).show();
    }

    /** after the options menu closed with another game language (game) or menu language than it opened with */
    void askRestartForLanguage(boolean game) {
        new GameDialog(this).title(game ? R.string.opt_language : R.string.opt_menu_language)
                .message(game ? R.string.language_restart : R.string.menu_language_restart)
                .button(R.string.gpu_driver_later, null)
                .button(R.string.res_restart_now, this::restartApp).show();
    }

    File baseDir() {
        File f = getExternalFilesDir(null);
        return f != null ? f : getFilesDir();
    }

    // ------------------------------------------------------------------ Android TV
    // On a TV (the TV launcher, or no touch screen) the game is played with a controller and the app is
    // set up with the remote: no on-screen controls by default, no touch or microphone questions, the
    // setup's buttons reached with the d-pad, and the game files also from the app's own folder (many
    // TVs have no file chooser for other apps).

    /** a TV, or another device without a touch screen */
    static boolean isTv(android.content.Context c) {
        android.content.pm.PackageManager pm = c.getPackageManager();
        android.app.UiModeManager ui = c.getSystemService(android.app.UiModeManager.class);
        return pm.hasSystemFeature(android.content.pm.PackageManager.FEATURE_LEANBACK)
                || (ui != null && ui.getCurrentModeType() == android.content.res.Configuration.UI_MODE_TYPE_TELEVISION)
                || !pm.hasSystemFeature(android.content.pm.PackageManager.FEATURE_TOUCHSCREEN);
    }

    boolean tv;  // isTv, at the start

    /** Controls > On-screen controls: on by default, off on a TV */
    boolean onscreenControls() { return prefs.getBoolean("onscreen_controls", !tv); }

    /** the folder for game files copied onto the device by hand (adb push, a file manager with access):
     *  Android/data/org.wwhdrecomp.app/files/import */
    File importDir() { return new File(baseDir(), "import"); }

    // ------------------------------------------------------------------ a phone as the GamePad
    // On a TV (PadHost): announced in the local network; a phone running the app (PadActivity) becomes the
    // GamePad: its buttons and touches come in here, the GamePad picture goes to it as video, it brings the
    // game files and saves. On a phone: Game > Play on a TV turns it into that GamePad.
    private PadHost padHost;

    void startPadHost() {
        if (padHost == null) padHost = new PadHost(this);
        padHost.start();
    }

    /** Game > Other devices' value: who is joined now, else whether others can join this device */
    String padHostState() {
        String who = padHost != null ? padHost.joined() : "";
        if (!who.isEmpty()) return who;
        return getString(prefs.getBoolean("pad_host", tv) ? R.string.opt_on : R.string.opt_off);
    }

    /** the options arrow tapped on a GamePad device: the whole options (the inventory's page slides in over this
     *  device's own GamePad picture, which isn't shown here: it would come over empty), unless a dialog is open */
    /** the GamePad device's own item screen (PadScreen): a slot put on X / Y / R */
    void padEquip(int button, int slot) {
        if (!gameStarted() || button < 0 || button > 2) return;
        Native.equipItem(button, slot);
    }

    /** ... its Save button: the game's own (the Save button of its GamePad item page, tapped there; the Items tab
     *  first where the game's GamePad screen shows another page) */
    void padSaveGame() {
        if (gameStarted()) tapDrcPart(new String[] {"L_MenuSave_00", "L_MenuSave_01"}, "L_BtnMenu_00", 0);
    }

    /** taps the game's GamePad screen on a part's touch box (Native.drcTargets: the first of `parts` shown); if none
     *  shows, `first` (a tab; or, on the page left of the items, where the console's options page slides the inventory,
     *  the game's arrow right) and again a moment later */
    private void tapDrcPart(String[] parts, String first, int tries) {
        float[] at = drcTarget(parts);
        if (at == null) {
            float[] tab = tries < 3 && first != null ? drcTarget(new String[] {first, "L_MenuArrowR_00"}) : null;
            Log.i(TAG, "GamePad host: " + parts[0] + " not on the game's GamePad screen" + (tab != null ? ", its tab first" : ""));
            if (tab != null) {
                drcTap(tab);
                keyHandler.postDelayed(() -> tapDrcPart(parts, first, tries + 1), 700);
            }
            return;
        }
        drcTap(at);
    }

    private float[] drcTarget(String[] parts) {
        for (String part : parts)
            for (String t : Native.drcTargets().split(";")) {
                String[] f = t.trim().split(" ");
                if (f.length != 6 || !f[1].equals(part)) continue;
                try {
                    float x = Float.parseFloat(f[2]), y = Float.parseFloat(f[3]);
                    if (x > 0f && x < 1f && y > 0f && y < 1f) return new float[] {x, y};
                } catch (NumberFormatException ignored) {
                }
            }
        return null;
    }

    private void drcTap(float[] at) {
        float u = ControlsView.pictureU(at[0]), v = ControlsView.pictureV(at[1]);
        Native.setDrcTouch(true, u, v);
        keyHandler.postDelayed(() -> Native.setDrcTouch(false, u, v), 100);
    }

    /** ... the Wind Waker tapped there: Link conducts (the item ring's way) */
    void padConduct(int slot) {
        if (gameStarted() && itemRing != null) itemRing.conductFromPad(slot);
    }

    void padOptions() {
        android.app.Dialog d = topDialog();
        Log.i(TAG, "GamePad host: options arrow tapped there (" + (d != null ? "a dialog is open: " + d.getClass().getSimpleName()
                : !started ? "game not started" : "options open") + ")");
        if (d == null && started) padMenu();
    }

    // the options opened from a GamePad device: shown there only (see-through here, OptionsMenu.onlyOnGamePad); here
    // again if it leaves while they are open
    private OptionsMenu padOnlyMenu;

    private void padMenu() {
        OptionsMenu m = new OptionsMenu(this);
        m.onlyOnGamePad(true);
        m.setOnDismissListener(x -> { if (padOnlyMenu == m) padOnlyMenu = null; });
        padOnlyMenu = m;
        m.show();
    }

    /** the GamePad device's picture came or went (PadHost): the targets and the arrow are followed for it */
    void padPictureChanged() {
        if (padPicture() && controls != null) startFocusTick();
    }

    /** a GamePad device shows the GamePad picture now (not the TV's behind its buttons, not away): its targets and
     *  the options arrow are followed as for the GamePad picture here */
    boolean padPicture() { return padHost != null && padHost.padPicture(); }

    void setPadHost(boolean on) {
        prefs.edit().putBoolean("pad_host", on).apply();
        if (on) startPadHost();
        else if (padHost != null) {
            padHost.stop();
            padHost = null;
            padRemote(null, 0);
            padTvOut(null);
            pushInput();
        }
    }

    boolean gameStarted() { return started; }

    boolean hasGameFiles() { return Native.checkGame(gameDir()) == null; }

    private int padButtons() { return padHost != null && topDialog() == null ? padHost.buttons : 0; }

    /** a remote GamePad's stick, none while one of the app's dialogs has its controller */
    private float padAxis(float v) { return padHost != null && topDialog() == null ? v : 0; }

    // ---- the app's dialogs (OptionsMenu, GameDialog) open on this device, the newest last: a remote GamePad's
    // controller works the top one as a controller connected here would (its game input waits meanwhile)
    private static final List<android.app.Dialog> openDialogs = new ArrayList<>();

    static void dialogShown(android.app.Dialog d) {
        openDialogs.remove(d);
        openDialogs.add(d);
        dialogsChanged();
    }

    static void dialogGone(android.app.Dialog d) {
        openDialogs.remove(d);
        dialogsChanged();
    }

    // the dialog on top goes to a remote GamePad too (PadUi), once it has settled
    private static void dialogsChanged() {
        MainActivity a = instance;
        if (a != null && a.padHost != null) a.keyHandler.post(() -> { if (a.padHost != null) a.padHost.ui.update(topDialog()); });
    }

    private static android.app.Dialog topDialog() {
        for (int i = openDialogs.size() - 1; i >= 0; i--)
            if (openDialogs.get(i).isShowing()) return openDialogs.get(i);
        return null;
    }

    private boolean padSelectDown;
    private boolean remoteController;
    private boolean remotePlay;  // that controller is what's played with (ControlsView.setPadPlay), until a touch here  // a GamePad or screen device's controller pressed a button (applySlotLabels)
    private boolean padSelectFromScreen;  // held on a screen device's controller: the options (that one shows the TV picture)
    private final Runnable padSelectHeld = () -> {
        if (padSelectDown) Log.i(TAG, "GamePad host: − held there: " + (padSelectFromScreen ? "the options" : drcScreen() ? "back" : "the options"));
        if (padSelectDown && padSelectFromScreen) onMenu();
        else if (padSelectDown) menuOrBack();
        padSelectDown = false;
    };

    /**
     * A controller key on a remote GamePad (PadLink.KEY): into the app's dialog on top if one is open (the options,
     * the start-state choice...); else Home opens the options, and Select held for 0.6 s (as here; a short press
     * is the GamePad's − like a controller's here, which went with its buttons).
     */
    void padKey(int action, int code, int repeat, boolean fromScreen) {
        long now = android.os.SystemClock.uptimeMillis();
        if (KeyEvent.isGamepadButton(code) && action == KeyEvent.ACTION_DOWN) {
            if (!remoteController) {  // a controller there: its letters on the item buttons
                remoteController = true;
                applySlotLabels(controllerConnected());
            }
            // and in use as one here is: the touch controls hide, and what they do by themselves with them (the
            // sword / leaf put away by itself...) stops
            if (started && topDialog() == null) controllerUsed();
            if (!remotePlay) {  // (as inputSource: A puts the sword away, nothing goes by itself)
                Log.i(TAG, "GamePad host: a controller there (" + KeyEvent.keyCodeToString(code) + ")");
                remotePlay = true;
                if (controls != null) controls.setPadPlay(true);
            }
        }
        android.app.Dialog d = topDialog();
        if (d != null) {
            // out of touch mode first, as a key from a controller connected here would be (else the d-pad finds no focus)
            // and with a focus in it (its window needn't have the system's focus: the keys come from here)
            View root = d.getWindow() != null ? d.getWindow().getDecorView() : null;
            if (root != null && action == KeyEvent.ACTION_DOWN) {
                View f = d.getCurrentFocus();
                if (f != null && root.isInTouchMode()) f.requestFocusFromTouch();
                else if (f == null) {
                    root.requestFocusFromTouch();
                    if (d.getCurrentFocus() == null) root.requestFocus(View.FOCUS_DOWN);
                    if (d.getCurrentFocus() != null && code >= KeyEvent.KEYCODE_DPAD_UP && code <= KeyEvent.KEYCODE_DPAD_RIGHT) return;
                }
            }
            boolean used = d.dispatchKeyEvent(new KeyEvent(now, now, action, code, repeat, 0, android.view.KeyCharacterMap.VIRTUAL_KEYBOARD,
                    0, 0, InputDevice.SOURCE_GAMEPAD));
            // the d-pad's focus moves are Android's input system's (before a window sees the key): done here
            int dir = code == KeyEvent.KEYCODE_DPAD_UP ? View.FOCUS_UP : code == KeyEvent.KEYCODE_DPAD_DOWN ? View.FOCUS_DOWN
                    : code == KeyEvent.KEYCODE_DPAD_LEFT ? View.FOCUS_LEFT : code == KeyEvent.KEYCODE_DPAD_RIGHT ? View.FOCUS_RIGHT : 0;
            View f = d.getCurrentFocus();
            if (!used && dir != 0 && action == KeyEvent.ACTION_DOWN && f != null) {
                View next = f.focusSearch(dir);
                if (next != null && next.requestFocus(dir)) MenuSounds.play(this, MenuSounds.CURSOR);
            }
            return;
        }
        if (!started) return;
        if (code == KeyEvent.KEYCODE_BUTTON_SELECT && repeat == 0)
            Log.i(TAG, "GamePad host: − " + (action == KeyEvent.ACTION_DOWN ? "down" : "up") + " on " + (fromScreen ? "the screen device" : "the GamePad device"));
        if (code == KeyEvent.KEYCODE_BUTTON_MODE && action == KeyEvent.ACTION_UP) {
            if (drcScreen()) menuOrBack();
            else padMenu();  // (opened there: shown there only)
        }
        if (code == KeyEvent.KEYCODE_BUTTON_SELECT && repeat == 0) {
            padSelectDown = action == KeyEvent.ACTION_DOWN;
            keyHandler.removeCallbacks(padSelectHeld);
            padSelectFromScreen = fromScreen;
            if (padSelectDown) {
                windPad = false;
                windPadAngle = Integer.MIN_VALUE;
                keyHandler.postDelayed(padSelectHeld, SELECT_HOLD_MS);
            } else if (windPad) {  // − held with the right stick there: the wind chosen (as selectButton here)
                windPad = false;
                int w = windPadAngle;
                windPadAngle = Integer.MIN_VALUE;
                if (controls != null) controls.miniMap.setWindPreview(Integer.MIN_VALUE);
                if (w != Integer.MIN_VALUE) setWind(w);
                pushInput();
            }
        }
    }

    /** the phone's buttons or sticks changed */
    void padInput() { if (started) pushInput(); }

    /** a touch on the phone's GamePad picture (0..1, 16:9) */
    void padTouch(boolean down, float u, float v) {
        if (!started) return;
        Native.setDrcTouch(down, u, v);  // (a wider or taller picture: its 16:9 middle is the touch screen, as DrcDisplay's)
        float pa = Native.drcPictureAspect(), kx = pa > 16f / 9f ? pa / (16f / 9f) : 1f,
                ky = pa > 0.1f && pa < 16f / 9f ? (16f / 9f) / pa : 1f;
        ControlsView.fingerX = (u - 0.5f) * kx + 0.5f;
        ControlsView.fingerY = (v - 0.5f) * ky + 0.5f;
    }

    /** the GamePad picture into another device's video (null: back here) */
    void padRemote(android.view.Surface s, float aspect) {
        if (drcDisplay == null && s == null) return;
        updateDrcDisplay();
        drcDisplay.setRemote(s, aspect);
    }

    private boolean tvOut;  // the TV picture goes to another device (its screen)

    /** the TV picture into another device's video, its own output (null: only here again) */
    void padTvOut(android.view.Surface s) {
        Native.tvOutSurfaceChanged(s);
        boolean was = tvOut;
        tvOut = s != null;
        updateLayout();
        if (was != tvOut && controls != null) applyControlsAppearance();  // (the game's buttons on the TV: hudContext)
    }

    /** another device shows the TV picture: this one shows the GamePad picture with the touch controls over it */
    boolean screenRemote() { return tvOut || (drcDisplay != null && drcDisplay.tvShown()); }

    void padGameReceived(boolean ok) {
        if (!ok) {
            padStatus(getString(R.string.pad_tv_game_failed));
            return;
        }
        padStatus(null);
        if (!started) checkAndStart();
    }

    /** another device became the GamePad or left: this one's touch controls go or come back */
    void padConnectedChanged() {
        if (remoteController && (padHost == null || (!padHost.connected() && !padHost.screenConnected()))) {
            remoteController = false;
            remotePlay = false;
            if (controls != null) controls.setPadPlay(lastController != null);
            applySlotLabels(controllerConnected());
        }
        applyControlsAppearance();
        if (padOnlyMenu != null && (padHost == null || !padHost.connected())) {
            padOnlyMenu.onlyOnGamePad(false);
            padOnlyMenu = null;
        }
        if (padHost != null) padHost.ui.update(topDialog());
    }

    private boolean padCodeArrived;

    /** whether this device needs its game code (it has the game files): a phone can compile it instead (PadActivity) */
    boolean padNeedsCode() {
        if (!hasGameFiles()) return false;
        return WorkService.Work.running() == WorkService.Work.COMPILE || Native.needsCompile(gameDir(), codeDir().getAbsolutePath());
    }

    /** the game code from a phone is in the code cache: this device's own compile stops, then the game starts */
    void padCodeReceived() {
        if (!hasGameFiles() || started) return;  // (no game files yet: they come next, padGameReceived starts it)
        if (WorkService.Work.running() == WorkService.Work.COMPILE) {
            padCodeArrived = true;
            Native.compileCancel();  // (parts in progress still finish: workFinished then)
        } else {
            checkAndStart();
        }
    }

    void padRestartForSaves() {
        padStatus(getString(R.string.pad_tv_restart));
        keyHandler.postDelayed(this::restartApp, 1500);
    }

    private TextView padStatus;
    private final Runnable padStatusHide = () -> { if (padStatus != null) padStatus.setVisibility(View.GONE); };

    /** a line at the top of the screen for a few seconds, over whatever shows (null: away) */
    void padStatus(String text) {
        if (padStatus == null) {
            padStatus = GameUi.text(this, false);
            padStatus.setTextColor(0xFFFFFFFF);
            padStatus.setTextSize(18);
            padStatus.setBackgroundColor(0xC0102040);
            int p = GameUi.px(this, 12);
            padStatus.setPadding(2 * p, p, 2 * p, p);
            FrameLayout.LayoutParams lp = new FrameLayout.LayoutParams(-2, -2, Gravity.TOP | Gravity.CENTER_HORIZONTAL);
            lp.topMargin = GameUi.px(this, 16);
            ((FrameLayout) getWindow().getDecorView()).addView(padStatus, lp);
        }
        keyHandler.removeCallbacks(padStatusHide);
        if (text == null) {
            padStatus.setVisibility(View.GONE);
            return;
        }
        padStatus.setText(text);
        padStatus.setVisibility(View.VISIBLE);
        padStatus.bringToFront();
        keyHandler.postDelayed(padStatusHide, 5000);
    }

    /** a phone or tablet: the game here ends (saved as with Quit) and this device looks for a TV to be its GamePad */
    static final String EXTRA_PAD_ROLE = "pad_role";
    static final String EXTRA_CHOSEN = "start_chosen";  // the start page chose to play here (PadActivity.startGameHere)
    static final String PREF_START_CHOICE = "start_choice";  // PadLink.ROLE_*: the icon's shortcuts

    @Override
    protected void onNewIntent(Intent i) {
        super.onNewIntent(i);
        int role = i.getIntExtra(EXTRA_PAD_ROLE, 0);  // a shortcut while the game runs: it ends first (saved as with Quit)
        if (BuildConfig.REMOTE_PLAY && role != 0 && started) becomePad(role);
    }

    void becomePad() { becomePad(PadLink.ROLE_PAD); }

    /** the same as the screen of another device: the TV picture and sound only */
    void becomeScreen() { becomePad(PadLink.ROLE_SCREEN); }

    /** Show on a TV: a device waiting to be a screen invited (Others can join this device switched on first) */
    void inviteScreen(ScreenFinder.Screen s, java.util.function.Consumer<Boolean> done) {
        if (padHost == null) setPadHost(true);
        PadHost h = padHost;
        padStatus(getString(R.string.pad_cast_sent, s.name));
        new Thread(() -> {
            int port = -1;
            for (int i = 0; i < 30 && (port = h.port()) < 0; i++) {  // (just switched on: its socket opens in a moment)
                try {
                    Thread.sleep(100);
                } catch (InterruptedException e) {
                    return;
                }
            }
            boolean ok = port > 0 && ScreenFinder.invite(s, PadHost.deviceName(this), port, h.ticket());
            runOnUiThread(() -> {
                if (!ok) padStatus(getString(R.string.pad_cast_failed, s.name));
                done.accept(ok);
            });
        }, "wwhd-invite").start();
    }

    private void becomePad(int role) {
        boolean screen = role == PadLink.ROLE_SCREEN;
        new GameDialog(this).title(screen ? R.string.pad_screen_title : R.string.pad_title)
                .message(screen ? R.string.pad_screen_become_text : R.string.pad_become_text)
                .button(R.string.opt_cancel, null)
                .button(R.string.pad_become, () -> {
                    onExit = () -> PadActivity.open(this, role);
                    quitApp();
                }).show();
    }

    /** A file chooser (ACTION_OPEN_DOCUMENT / CREATE_DOCUMENT); a device without one (many TVs) gets told
     *  where to copy files instead. False then. */
    boolean startPicker(Intent i, int request) { return startPicker(i, request, null); }

    /** as startPicker; `noChooser` runs after the notice when there is no chooser */
    boolean startPicker(Intent i, int request, Runnable noChooser) {
        try {
            startActivityForResult(i, request);
            return true;
        } catch (android.content.ActivityNotFoundException e) {
            Log.w(TAG, "no file chooser: " + e);
            new GameDialog(this).title(R.string.tv_no_chooser_title)
                    .message(getString(R.string.tv_no_chooser, importDir().getAbsolutePath()))
                    .button(R.string.opt_ok, noChooser).show();
            return false;
        }
    }

    /** whether this device has a file chooser for other apps */
    private boolean hasPicker() {
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT).addCategory(Intent.CATEGORY_OPENABLE).setType("*/*");
        return i.resolveActivity(getPackageManager()) != null;
    }

    /** the setup's "Use the files in the app's folder": the image, keys or .wua in importDir() */
    private void extractFromImportDir() {
        File dir = importDir();
        //noinspection ResultOfMethodCallIgnored
        dir.mkdirs();
        File[] list = dir.listFiles(File::isFile);
        List<String[]> files = new ArrayList<>();
        if (list != null)
            for (File f : list) files.add(new String[] {f.getName(), android.net.Uri.fromFile(f).toString()});
        if (files.isEmpty()) {
            new GameDialog(this).title(R.string.tv_import_title)
                    .message(getString(R.string.tv_import_empty, dir.getAbsolutePath()))
                    .button(R.string.opt_ok, null).show();
            return;
        }
        extractFrom(files);
    }

    // testing hooks (launch extras) only in debuggable builds: in a shared APK other apps could pass them
    boolean debuggable() { return (getApplicationInfo().flags & android.content.pm.ApplicationInfo.FLAG_DEBUGGABLE) != 0; }

    /** The folder with save/ and states/ of the game this run plays: files, or the randomized game's (Randomizer). */
    File playDir() { return Randomizer.playDir(this); }

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
            TextView t = cardText();
            t.setText(R.string.compile_running);
            box.addView(t);
            android.widget.ProgressBar bar = new android.widget.ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal);
            bar.setMax(1000);
            TextView detail = cardText();
            setContentView(seaScreen(box, workFooter(bar, detail, GameUi.barButton(this, getString(android.R.string.cancel), v -> {}, false, true))));
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

    private boolean linkColorsTried;

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
        // Game > Randomizer: a new randomized game to make first; then the game to play (its executable, its files)
        if (Randomizer.pending(prefs)) {
            runRandomizer();
            return;
        }
        // with randomized games made: which game, at each start of the app (a restart of its own keeps the one played)
        if (!gameAsked) {
            gameAsked = true;
            if (tourSlot == 0 && !Benchmark.pending(prefs) && !getIntent().getBooleanExtra(RestartActivity.EXTRA_RESTARTED, false)
                    && askGame()) return;
        }
        Native.setRandomizer(Randomizer.runtimeDir(this));
        Native.setKeepPlainCode(prefs.getBoolean(Randomizer.PREF_KEEP_PLAIN, true));
        // the save files' player models to the runtime (put in live when a file is played; the pack this game plays is the base)
        PlayerModels.prepare(this);
        // the save files' long names to the runtime (Mods > Save file names)
        SaveNames.handOver(this);
        // Link's colours in the game as it is (Mods > Link's colours): their pack made first when they changed (the game
        // starts without them if that fails)
        if (!linkColorsTried && LinkColors.needsMaking(this)) {
            linkColorsTried = true;  // (once: a failure doesn't hold the start up)
            withProgress(R.string.look_making, () -> LinkColors.make(this), r -> checkAndStart());
            return;
        }
        if (Native.needsCompile(gameDir(), codeDir().getAbsolutePath())) {
            runCompile();
            return;
        }
        // the graphics benchmark ended in its frame generation stage (a crash or GPU hang): its result
        // without frame generation, before the crash dialog
        if (!benchCrashChecked) {
            benchCrashChecked = true;
            if (Benchmark.afterCrash(this)) return;
        }
        // the last run ended unexpectedly: offer its log before the game starts (a game that closes
        // again right after loading leaves no time to share it later)
        if (!crashOffered && !Benchmark.pending(prefs)) {  // (a benchmark run: at the next normal start)
            crashOffered = true;
            CrashLogs.recordExit(this, prefs);
            if (tourSlot != 0) CrashLogs.markSeen(this, prefs);  // (the tour goes on by itself)
            else if (CrashLogs.offerNew(this, prefs, this::checkAndStart)) return;
        }
        // the GPU driver lost the device last time (it hung): offer another driver before the game starts
        if (!gpuLostOffered && tourSlot == 0) {
            gpuLostOffered = true;
            if (offerGpuDriverAfterLoss(this::checkAndStart)) return;
        }
        boolean bench = Benchmark.pending(prefs);
        // the graphics benchmark's offer, once (new installations right after the compile, existing ones at the first
        // start of a version with it). Without a game save the game shows its "save data created" message before
        // the title; firstStartMessage confirms it, in the benchmark's run too
        if (!bench && tourSlot == 0 && !prefs.getBoolean(Benchmark.DONE, false) && prefs.contains(GraphicsPresets.STATE)) {
            Benchmark.offer(this, this::checkAndStart);
            return;
        }
        // once: the new touch controls, Jump anywhere and singing the songs (StarterQuestions)
        if (!bench && tourSlot == 0 && StarterQuestions.offer(this, this::checkAndStart)) return;
        File base = baseDir();
        // testing: WWHD_CLEAR_SHADERS=1 (an intent extra) starts as with "Delete shader cache"
        if (Os.getenv("WWHD_CLEAR_SHADERS") != null) Backup.deleteTree(new File(getNoBackupFilesDir(), "shadercache"));
        installShaderTemplate();
        String failedDriver = applyGpuDriver();
        // the save state picker before the intro: the game waits for it from its first frame on
        File play = playDir();  // (the randomized game's own save and states while it is played)
        boolean offerStates = !bench && (tourSlot != 0 || startMode() != START_INTRO) && !usedStateSlots(play).isEmpty();
        if (offerStates) Native.setStartHold(true);
        // the last run's frame generation mark is read (crash dialog); frame generation switched off there
        // since the library loaded (the renderer reads WWHD_LSFG_DLL when it starts)
        //noinspection ResultOfMethodCallIgnored
        frameGenMarker(this).delete();
        if (!frameGenOn() || bench) {
            try {
                Os.unsetenv("WWHD_LSFG_DLL");
            } catch (ErrnoException ignored) {
            }
        }
        firstStart = !new File(play, "save/user/cking.sav").exists();  // (the game makes its save at this start)
        Achievements.saveDir = new File(play, "save");
        Native.start(gameDir(), new File(play, "save").getAbsolutePath(),
                new File(getNoBackupFilesDir(), "shadercache").getAbsolutePath(), base.getAbsolutePath());
        started = true;
        applyOptions();
        // Game > Skip the title screen: the logos go straight to the file select (mods/warp.cpp; B there still leads
        // to the title); not for the benchmark (it warps from the title)
        if (prefs.getBoolean("skip_title", false) && !bench) Native.setOption("boot_skip", 1);
        startMotion();
        showGame();
        if (padHost != null) padHost.gameStarted();
        frontHandler.post(frontPoll);
        if (bench) Benchmark.run(this, (FrameLayout) surface.getParent());
        else showCacheLoading();
        if (offerStates) offerStartStates();
        else if (tourSlot != 0) tour = new ShaderTour(this, tourSlot, true);  // (no states at all: a fresh installation)
        updateDrcDisplay();
        if (failedDriver != null)
            new GameDialog(this).title(R.string.opt_gpu_driver).message(getString(R.string.gpu_driver_failed, failedDriver))
                    .button(R.string.opt_ok, null).show();
    }

    private boolean crashOffered;  // once per process: the offer before the game starts
    private boolean gameAsked;  // once per process: the game as it is or a randomized one (askGame)
    private boolean benchCrashChecked;
    private boolean benchRunning;  // the graphics benchmark runs in this process: no save states
    private static int tourSlot;  // the shader tour's save state slot (0: no tour); ShaderTour
    private ShaderTour tour;

    /** continues the start after a dialog that came first */
    void continueStart() { checkAndStart(); }
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
        // scene; from pull request #8 by SSunnKing). On Adreno 6xx its own choice instead (forced GMEM
        // measured slower on an Adreno 650: 22.7 against 17.0 ms per frame), and without UBWC
        // compression: with it, Turnip 26.0 to 26.3 above 1x resolution restarted an SD865 / Adreno 650
        // phone (kernel 4.19) at the first frame (a fatal bus error, TZBSP_ERR_FATAL_NOC_ERROR); without it
        // 1.5x runs and 1x is as fast (WindWakerHD-Android-Plus d2ef429, b3db777). A TU_DEBUG in wwhd.env wins
        if (d.turnip() && Os.getenv("TU_DEBUG") == null) {
            java.util.List<String> dbg = new java.util.ArrayList<>();
            String gpu = GpuDrivers.gpuModel();
            boolean a6xx = gpu.startsWith("Adreno6");
            if (d.library.contains("freedreno") && !a6xx) dbg.add("gmem");
            if (a6xx) dbg.add("noubwc");
            Log.i(TAG, "Turnip on " + (gpu.isEmpty() ? "an unknown GPU" : gpu) + ": TU_DEBUG=" + String.join(",", dbg));
            if (!dbg.isEmpty()) setenv("TU_DEBUG", String.join(",", dbg));
        }
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
        startPicker(i, PICK_DRIVER, this::driverPickDone);
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

    // ---- the graphics benchmark (Benchmark.java)
    /** a sea card over the game for the benchmark (the game's picture stays hidden) */
    View coverScreen(LinearLayout content) {
        View v = seaScreen(content);
        coverSea = sea;
        sea = null;  // the game's screen: no work screen of its own
        return v;
    }

    private SeaView coverSea;

    SeaView coverSea() { return coverSea; }

    void restartForBenchmark() { restartApp(); }

    void restartAfterBenchmark() { restartApp(); }

    private boolean benchCamera;

    /** the benchmark: the camera turns around Link (the right stick held) */
    void setBenchmarkCamera(boolean on) {
        benchCamera = on;
        pushInput();
    }

    /** the benchmark runs: touch controls, overlay and saves out of the way */
    void setBenchmarkControls(boolean on) {
        benchRunning = on;
        if (controls != null) controls.setVisibility(on ? View.GONE : View.VISIBLE);
        if (filesBack != null && on) filesBack.setVisibility(View.GONE);
    }

    /** the rendering scale for the benchmark, live, without touching the setting */
    void setRenderScaleLive(float scale) {
        Native.setOption("res_scale", Math.round(scale * 100));
        renderScale = scale;
        updateBufferSize();
    }

    /** frame generation for the benchmark: the player's network settings, x2, without the setting */
    void applyFrameGenBenchmark(boolean on) {
        if (on) Native.setFrameGenRiskAccepted(prefs.getBoolean("fg_risk_ok", false));
        Native.applyFrameGen(on && frameGenDll().exists(), frameGenDll().getAbsolutePath(), prefs.getBoolean("fg_quality", false),
                Float.parseFloat(prefs.getString("fg_flow_scale", "0.5")), 2, prefs.getBoolean("fg_ui_detection", true));
    }

    private Runnable afterDllPick;

    /** the Lossless.dll picker; `then` runs once a good file was taken (or the pick ended) */
    void pickDllThen(Runnable then) {
        afterDllPick = then;
        pickDll();
    }

    private View seaScreen(LinearLayout content) { return seaScreen(content, null); }

    /** footer: shown below the scrolling text, always in view (the work screens' progress and Cancel) */
    private View seaScreen(LinearLayout content, LinearLayout footer) {
        float d = getResources().getDisplayMetrics().density;
        android.widget.FrameLayout root = new android.widget.FrameLayout(this);
        sea = new SeaView(this);
        root.addView(sea, new android.widget.FrameLayout.LayoutParams(-1, -1));
        // the GamePad menus' torn paper (GameUi.GamePaper; drawn parchment until the game files are there): its torn
        // edge and blue line take ~30 of its 48-unit frame
        android.graphics.Rect win = getWindowManager().getCurrentWindowMetrics().getBounds();
        float unit = Math.min(win.width() / 1280f, win.height() / 720f);
        int frame = Math.round(52 * unit);
        styleCardText(content);
        // at most the upper 60% of the screen (it scrolls inside if longer): the boat sails below it, always in view;
        // the paper behind the scrolling part, so its torn edges stay
        int maxH = (int) (win.height() * 0.6f) - 2 * frame;
        ScrollView sv = new ScrollView(this) {
            @Override
            protected void onMeasure(int wSpec, int hSpec) {
                super.onMeasure(wSpec, MeasureSpec.makeMeasureSpec(maxH, MeasureSpec.AT_MOST));
            }
        };
        sv.setFillViewport(false);
        // the card's buttons below the scrolling part, always in view (on a phone held sideways the text alone can
        // fill the card): the game's buttons at the end of the content move into rows of their own
        java.util.List<View> buttons = new java.util.ArrayList<>();
        for (int i = content.getChildCount() - 1; i >= 0 && isCardButton(content.getChildAt(i)); i--) buttons.add(0, content.getChildAt(i));
        for (View b : buttons) content.removeView(b);
        sv.addView(content, new android.widget.FrameLayout.LayoutParams(-1, -2));
        LinearLayout card = new LinearLayout(this);
        card.setOrientation(LinearLayout.VERTICAL);
        card.setBackground(new GameUi.GamePaper(this, unit));
        card.setPadding(frame, frame, frame, frame);
        card.addView(sv, new LinearLayout.LayoutParams(-1, -2, 1));  // (the part that shrinks when the card is too tall)
        if (footer != null) {
            styleCardText(footer);
            LinearLayout.LayoutParams flp = new LinearLayout.LayoutParams(-1, -2);
            flp.leftMargin = (int) (24 * d);
            flp.rightMargin = (int) (24 * d);
            card.addView(footer, flp);
        }
        if (!buttons.isEmpty()) {
            GameUi.ButtonRows rows = new GameUi.ButtonRows(this, (int) (12 * d));
            for (View b : buttons) rows.addView(b, new android.view.ViewGroup.LayoutParams(-2, (int) (54 * d)));
            LinearLayout.LayoutParams rlp = new LinearLayout.LayoutParams(-1, -2);
            rlp.leftMargin = (int) (24 * d);
            rlp.rightMargin = (int) (24 * d);
            rlp.topMargin = (int) (4 * d);
            card.addView(rows, rlp);
        }
        // a card of at most 640 dp, in the upper part so the sea stays visible
        int width = (int) Math.min(getResources().getDisplayMetrics().widthPixels - 32 * d, 640 * d);
        android.widget.FrameLayout.LayoutParams lp = new android.widget.FrameLayout.LayoutParams(width, -2,
                Gravity.CENTER_HORIZONTAL | Gravity.TOP);
        lp.topMargin = (int) (24 * d);
        lp.bottomMargin = (int) (24 * d);
        root.addView(card, lp);
        // a TV: the remote's d-pad starts on the first button (else its first press goes to the scrolling text)
        if (tv && !buttons.isEmpty()) root.post(() -> buttons.get(0).requestFocus());
        else if (tv && footer != null) root.post(() -> { java.util.List<View> f = footer.getFocusables(View.FOCUS_FORWARD); if (!f.isEmpty()) f.get(0).requestFocus(); });
        return root;
    }

    /** the work screens' footer: the progress bar, below it the progress line and a small grey Cancel beside it
     *  (issue #29: the big green button made the text scroll on 20:9 phones and hid the progress line) */
    private LinearLayout workFooter(android.widget.ProgressBar bar, TextView detail, TextView cancel) {
        float d = getResources().getDisplayMetrics().density;
        LinearLayout f = new LinearLayout(this);
        f.setOrientation(LinearLayout.VERTICAL);
        f.addView(bar, new LinearLayout.LayoutParams(-1, -2));
        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(Gravity.CENTER_VERTICAL);
        row.addView(detail, new LinearLayout.LayoutParams(0, -2, 1));
        if (cancel != null) {
            cancel.setTextSize(android.util.TypedValue.COMPLEX_UNIT_SP, 14);
            cancel.setPadding((int) (14 * d), 0, (int) (16 * d), (int) (3 * d));
            cancel.setMinWidth((int) (96 * d));
            LinearLayout.LayoutParams clp = new LinearLayout.LayoutParams(-2, (int) (38 * d));
            clp.leftMargin = (int) (12 * d);
            row.addView(cancel, clp);
        }
        LinearLayout.LayoutParams rlp = new LinearLayout.LayoutParams(-1, -2);
        rlp.topMargin = (int) (4 * d);
        f.addView(row, rlp);
        return f;
    }

    private static boolean isCardButton(View v) {
        return v.getBackground() instanceof android.graphics.drawable.StateListDrawable;  // (cardButton, as styleCardText)
    }

    /** a text of a sea card: in the game's text font (until the game files are there: the system's) */
    private TextView cardText() {
        TextView t = GameUi.text(this, true);
        t.setTextSize(13);
        return t;
    }

    /** a button of a sea card: the game's green value button */
    private TextView cardButton(CharSequence text, View.OnClickListener click) {
        TextView b = GameUi.barButton(this, text.toString(), click, true, true);
        b.setMinWidth((int) (150 * getResources().getDisplayMetrics().density));
        return b;
    }

    private LinearLayout.LayoutParams cardButtonLp() {
        float d = getResources().getDisplayMetrics().density;
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT, (int) (54 * d));
        lp.topMargin = (int) (12 * d);
        return lp;
    }

    private static void styleCardText(android.view.ViewGroup g) {
        for (int i = 0; i < g.getChildCount(); i++) {
            View v = g.getChildAt(i);
            if (v instanceof Button) {
                v.setBackgroundTintList(android.content.res.ColorStateList.valueOf(0xFF8B5A2B));
                ((Button) v).setTextColor(0xFFFFF6E0);
                continue;
            }
            if (v instanceof android.widget.ProgressBar) {  // (the game's value button green on a paper shade)
                ((android.widget.ProgressBar) v).setProgressTintList(android.content.res.ColorStateList.valueOf(0xFF4FAE1C));
                ((android.widget.ProgressBar) v).setProgressBackgroundTintList(android.content.res.ColorStateList.valueOf(0xFFB4B9A2));
                continue;
            }
            if (v.getBackground() instanceof android.graphics.drawable.StateListDrawable) continue;  // (the game's buttons: cardButton)
            if (v instanceof TextView) {
                ((TextView) v).setTextColor(GameUi.OPTION_TEXT);
                ((TextView) v).setLinkTextColor(0xFF1F5FA8);
            } else if (v instanceof android.view.ViewGroup) styleCardText((android.view.ViewGroup) v);
        }
    }

    private void showSetup(String problem) {
        LinearLayout box = new LinearLayout(this);
        box.setOrientation(LinearLayout.VERTICAL);
        int pad = (int) (24 * getResources().getDisplayMetrics().density);
        box.setPadding(pad, pad, pad, pad);
        TextView t = cardText();
        if (Native.buildsGameCode()) {  // the APK without game code: a first start is the normal case
            String text = getString(R.string.setup_welcome);
            if (new File(gameDir()).exists()) text += "\n\n" + getString(R.string.setup_problem, problem);
            t.setText(text);
        } else {
            t.setText(getString(R.string.setup_text, gameDir(), problem));
        }
        boolean picker = hasPicker();
        if (tv || !picker) {
            //noinspection ResultOfMethodCallIgnored
            importDir().mkdirs();  // (there for adb push to copy into)
            t.append("\n\n" + getString(R.string.tv_setup_text, importDir().getAbsolutePath()));
            if (tv && BuildConfig.REMOTE_PLAY) t.append("\n\n" + getString(R.string.tv_setup_screen));  // (the screen of a phone that runs the game)
        }
        t.setTextIsSelectable(!tv);  // (on a TV the remote's d-pad goes to the buttons)
        box.addView(t);
        // the files themselves, not their folder: some devices' folder picker lists nothing on the
        // internal storage (Android 16 with AOSP DocumentsUI, on a Snapdragon and a Unisoc device)
        if (picker) box.addView(cardButton(getText(R.string.setup_extract_files), v -> pickDiscFiles()), cardButtonLp());
        if (tv || !picker) box.addView(cardButton(getText(R.string.tv_import_button), v -> extractFromImportDir()), cardButtonLp());
        box.addView(cardButton(getText(R.string.setup_retry), v -> checkAndStart()), cardButtonLp());
        // without game files too: this device as another's GamePad or screen (a TV: its screen only; a phone or
        // tablet runs the game and is the GamePad, the TV needs no game files and no compile)
        if (!tv && BuildConfig.REMOTE_PLAY)
            box.addView(cardButton(getText(R.string.opt_pad_play), v -> {
                PadActivity.open(this, PadLink.ROLE_PAD);
                finish();
            }), cardButtonLp());
        if (BuildConfig.REMOTE_PLAY)
            box.addView(cardButton(getText(R.string.opt_pad_screen), v -> {
                PadActivity.open(this, PadLink.ROLE_SCREEN);
                finish();
            }), cardButtonLp());
        box.addView(cardButton(getText(R.string.menu_about), v -> showLicenses()), cardButtonLp());
        setContentView(seaScreen(box));
    }

    private void showGame() {
        sea = null;
        MenuSounds.prepare(this);  // (the options menu's sounds, from the game's sound archive)
        FrameLayout root = new FrameLayout(this);
        surface = new SurfaceView(this);
        surface.getHolder().addCallback(this);
        bufScale = bufRequested = 1;
        try {
            renderScale = Float.parseFloat(String.valueOf(Os.getenv("WWHD_RES_SCALE")));  // the setting, or wwhd.env's
        } catch (NumberFormatException e) {
            renderScale = Float.parseFloat(prefs.getString("res_scale", "1"));
        }
        root.addView(surface, new FrameLayout.LayoutParams(FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT));
        controls = new ControlsView(this, this);
        itemRing = new ItemRing(this, controls);
        controls.setItemRing(itemRing);
        controls.setPadPlay(lastController != null || remotePlay);
        root.addView(controls, new FrameLayout.LayoutParams(FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT,
                Gravity.CENTER));
        // the file select's Back (B) while no controller is connected, in the game's green button style
        filesBack = GameUi.barButton(this, getString(R.string.opt_back), v -> controls.pulse(Native.B), true);
        filesBack.setTextSize(TypedValue.COMPLEX_UNIT_SP, 20);
        filesBack.setTextColor(0xFFFFFFFF);  // white lettering outlined in dark green, as the game's buttons
        filesBack.setTypeface(android.graphics.Typeface.create("sans-serif", android.graphics.Typeface.BOLD));
        filesBack.setShadowLayer(GameUi.px(this, 2.5f), 0, 0, 0xFF1F5E14);
        filesBack.setVisibility(filesTouch && !tv ? View.VISIBLE : View.GONE);
        int m = GameUi.px(this, 12);
        FrameLayout.LayoutParams lp = new FrameLayout.LayoutParams(GameUi.px(this, 120), GameUi.px(this, 48), Gravity.TOP | Gravity.END);
        lp.setMargins(m, m, m, m);
        root.addView(filesBack, lp);
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
        startPicker(i, PICK_PRESET_SAVE);
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
        TextView t = cardText();
        t.setText(R.string.cache_loading);
        box.addView(t);
        android.widget.ProgressBar bar = new android.widget.ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal);
        bar.setMax(1000);
        box.addView(bar);
        TextView detail = cardText();
        box.addView(detail);
        View screen = seaScreen(box);
        final SeaView mySea = sea;
        mySea.setFrameInterval(66);  // pipelines are being built: the CPU is for them
        sea = null;  // the game's screen: no work screen of its own
        screen.setVisibility(View.GONE);
        root.addView(screen, new FrameLayout.LayoutParams(FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT));
        final long t0 = android.os.SystemClock.uptimeMillis();
        // the very first start (no game save): the card covers the game until its title, so the game's "save data
        // created" message (confirmed by firstStartMessage) is never seen; at most 30 s
        final boolean saveCover = firstStart && prefs.getBoolean("skip_save_message", true);
        final android.os.Handler h = new android.os.Handler(android.os.Looper.getMainLooper());
        h.post(new Runnable() {
            @Override
            public void run() {
                long[] p = Native.cacheProgress();
                long now = android.os.SystemClock.uptimeMillis();
                int f = Native.frontScreen();
                boolean saveWait = saveCover && f != Native.FRONT_TITLE && now - t0 < 30000;
                if ((p[0] == 2 && !saveWait) || screen.getParent() != root) {  // done, or the game's screen was replaced
                    root.removeView(screen);
                    return;
                }
                // only while a cache file is replayed (state 1 with a size) or the shader head start is
                // made from the game files (3), not while the game boots (but from the start at the very first)
                if (screen.getVisibility() != View.VISIBLE && (saveWait || (p[0] == 1 || p[0] == 3) && p[2] > 0
                        && now - t0 > 300))
                    screen.setVisibility(View.VISIBLE);
                if (p[0] == 2 || p[0] == 0) {  // the shaders are done (or not begun): the game boots
                    t.setText(R.string.bench_step_boot);
                    detail.setText("");
                    bar.setProgress(p[0] == 2 ? 1000 : 0);
                    h.postDelayed(this, 150);
                    return;
                }
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
                if (tourSlot != 0) {  // the shader tour: its state, then it plays (none: the tour gets there itself)
                    String[] info = Native.saveSlotInfo(tourSlot);
                    boolean load = info[0].equals("1") && info[1].equals("1");
                    if (load) Native.loadState(tourSlot);
                    else Native.setStartHold(false);
                    tour = new ShaderTour(MainActivity.this, tourSlot, !load);
                    return;
                }
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
                for (int slot : usedStateSlots(playDir())) {
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
        Native.setOption("area_blur", prefs.getBoolean("area_blur", Native.getOption("area_blur") != 0) ? 1 : 0);
        Native.setOption("bloom_strength", prefs.getInt("bloom_strength", Native.getOption("bloom_strength")));
        Native.setOption("distant_shadows", prefs.getBoolean("distant_shadows", Native.getOption("distant_shadows") != 0) ? 1 : 0);
        Native.setOption("aniso", prefs.getBoolean("aniso", Native.getOption("aniso") != 0) ? 1 : 0);
        Native.setOption("vsync", prefs.getBoolean("vsync", false) ? 1 : 0);
        Native.setOption("upscale", prefs.getInt("upscale", Native.getOption("upscale")));
        // every controller and the touch controls act as the Wii U GamePad (+ shows its screen, see
        // pushInput); the setting "Controls act as" is gone, as is the Pro Controller mode
        Native.setOption("pro_controller", 0);
        if (prefs.contains("pro_controller")) prefs.edit().remove("pro_controller").apply();
        Native.setOption("invert_camera_y", prefs.getInt("invert_camera_y_mode", 0));
        Native.setOption("steer_mode", prefs.getInt("steer_mode", 0));
        Native.setOption("steer_swing", prefs.getBoolean("steer_swing", false) ? 1 : 0);
        Native.setOption("steer_swing_delay", prefs.getInt("steer_swing_delay", 1000));
        Native.setOption("steer_resync", prefs.getInt("steer_resync", 1));
        Native.setOption("tv_aspect", prefs.getInt("tv_aspect", 0));
        Native.setOption("render_aspect", prefs.getInt("render_aspect", 0));
        Native.setOption("view_zoom", viewZoomNow());
        Native.setOption("dyn_res_min", prefs.getInt("dyn_res_min", 0));
        for (String m : MODS) Native.setOption(m, prefs.getBoolean(m, false) ? 1 : 0);
        for (String c : CHEATS_INFINITE) Native.setOption(c, prefs.getBoolean(c, false) ? 1 : 0);
        Native.setOption("cheat_moon_rise", prefs.getInt("cheat_moon_rise", 100));
        Native.setOption("cheat_moon_fly", prefs.getInt("cheat_moon_fly", 100));
        Native.setOption("mod_leaf_lift_height", prefs.getInt("mod_leaf_lift_height", 100));
        Native.setOption("mod_walk_doors_a", prefs.getBoolean("mod_walk_doors_a", false) ? 1 : 0);
        Native.setOption("mod_text_pauses_off", prefs.getBoolean("mod_text_pauses_off", false) ? 1 : 0);
        Native.setOption("mod_hit_stop_off", hitStopOff());
        Native.setOption("mod_camera_speed", prefs.getInt("mod_camera_speed", 100));
        Native.setOption("swipe_boat", prefs.getInt("swipe_boat", 100));
        Native.setOption("mod_jump", prefs.getBoolean("mod_jump", false) ? 1 : 0);
        Native.setOption("mod_jump_double", prefs.getBoolean("mod_jump_double", false) ? 1 : 0);
        Native.setOption("mod_jump_noauto", prefs.getBoolean("mod_jump_noauto", false) ? 1 : 0);
        Native.setOption("auto_state", tourSlot != 0 ? 0 : prefs.getInt("auto_state", 0));  // (no states while the tour runs)
        Native.setOption("mod_run_speed", prefs.getInt("mod_run_speed", 100));
        Native.setOption("link_clothes", prefs.getInt("link_clothes", 0));
        Native.setOption("postbox_mail", prefs.getBoolean("postbox_mail", true) ? 1 : 0);
        Native.setOption("postbox_same_day", prefs.getBoolean("postbox_same_day", false) ? 1 : 0);
        Native.setOption("gamepad_sounds", prefs.getBoolean("gamepad_sounds", true) ? 1 : 0);
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
        GameIcons.linkPoses = prefs.getBoolean("link_poses", true);  // (before the redraws below)
        // Controls > On-screen controls (back for #63; its own pref: the old switch's controls_visible stays ignored)
        // (not while another device is the GamePad: it is the controller then)
        boolean visible = onscreenControls() && !autoHidden && !(padHost != null && padHost.connected());
        controls.setMenuAlone(!onscreenControls());
        controls.setTouchless(tv);
        final boolean nw = NEW_TOUCH_CONTROLS;
        controls.setAppearance(visible, prefs.getFloat("controls_scale", 1f), nw ? prefs.getFloat("controls_opacity", 0.45f) : 0.45f);
        boolean flexible = nw && prefs.getInt("controls_style", 0) == 1, swipe = prefs.getInt("touch_camera", 0) == 1;
        controls.setStyle(flexible, prefs.getBoolean("controls_show_sticks", true), swipe);
        controls.setBlindLayout(nw && prefs.getBoolean("controls_blind", true));  // (the backdrop's controller view)
        controls.setBlindSwipe(prefs.getInt("controls_blind_face", 1) == 1);  // (its face buttons: diamond / swipe)
        controls.setClassicSwipe(nw && prefs.getInt("controls_classic_face", 0) == 1);  // (the classic style's, the same choice)
        controls.setTvBackdrop(prefs.getInt("controls_tv_backdrop", 50) / 100f);  // (that view: the TV picture there)
        controls.setSwipeSpeed(prefs.getFloat("swipe_speed", SWIPE_SPEED_DEFAULT));
        controls.setFilled(nw && prefs.getBoolean("controls_filled", false));
        // context buttons: the game's button cluster (X, Y, R, B, ZR, A's seat and floating label) is hidden.
        // The large Picto Box view (off: the game's original Picto Box and telescope screens): the finder
        // twice its size (aspect.cpp), zoomed by pinching; the zoom bar and the stick guides (Zoom / Look
        // around) of the Picto Box and the telescope are hidden, and the album's d-pad icons (swiped instead)
        // (not in the eyes-free layout: plain letters on the phone, the game's own button icons on the TV)
        boolean context = flexible && prefs.getBoolean("context_buttons", false) && !controls.blind();
        boolean pictoBig = context && prefs.getBoolean("picto_big", true);
        controls.setContext(context);
        controls.setPictoBig(pictoBig);
        controls.setMinigameTouch(nw && prefs.getBoolean("minigame_touch", true));
        controls.setTalkHold(prefs.getBoolean("talk_hold", true), prefs.getBoolean("talk_hold_ff", false));
        Native.setOption("talk_hold_pad", prefs.getBoolean("talk_hold", true) ? 1 : 0);  // (A held, mods/minigames.cpp)
        controls.setConductPads(prefs.getBoolean("conduct_pads", true));
        controls.setStickFollows(prefs.getBoolean("stick_follows", true));
        controls.setAfterDefeat(nw ? prefs.getInt("zl_after_defeat", 0) : 0);
        controls.setPadZlEvery(prefs.getBoolean("pad_zl_every", false));
        controls.setZLatch(!flexible || prefs.getBoolean("z_latch", true));
        controls.setViewPinch(prefs.getBoolean("view_pinch", true));
        controls.setItemTap(nw && prefs.getBoolean("item_tap", true));
        controls.setAimFocus(prefs.getBoolean("item_tap_focus", true));
        controls.setAutoStow(prefs.getBoolean("auto_stow", nw && prefs.getBoolean("item_tap", true)));  // (default: on by tap, off held as in the game)
        controls.miniMap.setEnabled(prefs.getBoolean("minimap", true));
        controls.miniMap.setProgressMarks(prefs.getInt("minimap_marks", MiniMap.MARKS_OFF));
        // (the progress places are the game's own: in a randomized game they'd tell the wrong things)
        controls.setChartMarks(Randomizer.playing(this) ? MiniMap.MARKS_OFF : prefs.getInt("chart_marks", MiniMap.MARKS_OFF));
        ChartMarks.I.setPictures(prefs.getInt("chart_pictures", ChartMarks.GAME_CHART), prefs.getBoolean("minimap_names", true));
        ownDisplayMarks();
        controls.progressBadge.setEnabled(prefs.getBoolean("progress_hud", false));
        controls.achievementPopup.setEnabled(prefs.getBoolean("achievement_popup", true));
        controls.achievementPopup.setLink(prefs.getBoolean("achievement_link", true));
        controls.achievementPopup.setSound(prefs.getBoolean("achievement_sound", true));
        MenuSounds.itemGetScale = prefs.getInt("achievement_sound_volume", MenuSounds.JINGLE_DEFAULT) / 100f;
        controls.miniMap.setPlace(prefs.getInt("minimap_corner", MiniMap.CORNER_BOTTOM_LEFT), prefs.getInt("minimap_size", 1),
                prefs.getBoolean("minimap_turn", false), prefs.getFloat("minimap_x", 0), prefs.getFloat("minimap_y", 1),
                prefs.getBoolean("minimap_round", false), prefs.getBoolean("minimap_outline", false));
        controls.miniMap.setMark(prefs.getBoolean("minimap_face", true), prefs.getInt("minimap_mark_size", 1),
                prefs.getInt("minimap_arrow_colour", 0), prefs.getInt("minimap_move_zoom", 0));
        controls.miniMap.setPictures(MiniMap.pictures(prefs), prefs.getBoolean("minimap_names", true), MiniMap.seaZoom(prefs));
        controls.miniMap.setExtras(prefs.getBoolean("minimap_wind", true), prefs.getInt("minimap_opacity", 100),
                prefs.getBoolean("minimap_fade", false), prefs.getInt("minimap_fade_opacity", 50),
                prefs.getBoolean("minimap_charts", true), prefs.getBoolean("minimap_buttons", false), prefs.getBoolean("minimap_hidden", false));
        controls.miniMap.setWindSwipe(prefs.getBoolean("minimap_wind_swipe", true));
        // touch controls hidden (by hand, or a controller in use): the game's own back; and while the TV picture shows on
        // another device (its screen): there the game's buttons are what's seen, the touch ones are on this one
        hudContext = context && visible && !screenRemote();
        hudPicto = pictoBig;
        applyHudHidden();
        Native.setOption("picto_big", pictoBig ? 1 : 0);
        Native.setOption("ring_no_baton", itemRingOn() ? 1 : 0);  // the Wind Waker conducts from the ring, never kept on a button
        applyRingButtons();
        Native.setHudFade(0, nw && prefs.getBoolean("hearts_combat", false));
        Native.setHudFade(2, nw && prefs.getBoolean("hearts_combat", false));  // the magic bar: on its own while used
        Native.setHudFade(1, nw && prefs.getBoolean("rupees_on_change", false));
        if (!controls.editing()) {
            controls.setLayoutString(nw ? controlsLayout() : "");
            controls.setBlindLayoutString(nw ? prefs.getString("controls_layout_blind", "") : "");
        }
        Native.setTouchSwipeCamera(controls.flexibleNow() && swipe);
        updateLayout();
    }

    // the game's button cluster (X, Y, R, B, ZR, A's seat and floating label): hidden for the context
    // buttons while they show, and on the file select while no controller is connected (its A OK / B Back icons don't
    // take touches; filesBack stands in for B), and on the GamePad picture's Options / Save while no
    // controller is connected (their A / B hints)
    private static final String HUD_BUTTONS = "N_X_00,N_Y_00,N_R_00,P_B_00,P_ZR_00,P_SetSeatASpecial_00,W_SetSeatA_00,L_CommandA_00";
    private boolean hudContext, hudPicto, filesTouch, menuTouch, shortcutHidden;
    private TextView filesBack;

    // (Large Picto Box view: its zoom bar, stick guides and d-pad icons too; the game's d-pad in play
    // (Shortcut_00: Wind Waker, cannon, salvage arm) while the item ring has them: Flexible style, no
    // controller connected, ControlsView.dpadInRing), and while the context buttons show (their d-pad has them)
    private void applyHudHidden() {
        Native.setHudHidden((hudContext || filesTouch || menuTouch ? HUD_BUTTONS : "")
                + (hudPicto ? ",N_Zoom_00,P_LStick_00,P_RStick_00,N_ControlPad_00" : "")
                + (shortcutHidden || hudContext ? ",N_Shortcut_00" : "") + slotHidden);
    }

    // the item buttons' names on the game's HUD and its drop slots, and on the touch buttons (Controls > Item button
    // names): 0 Auto (no controller: A1 A2 A3; a controller: the button that presses it, InputMapper.shortName), 1 A1 A2
    // A3, 2 the game's X Y R. A1 is Y (leftmost of the GamePad's drop slots), A2 X, A3 R. An item button a jump took off
    // (Auto with a controller): hidden there
    private static final int[] SLOT_WIIU = {2, 3, 5};  // X, Y, R in InputMapper.WIIU
    private static final String[] SLOT_LETTER = {"X", "Y", "R"}, SLOT_ACTION = {"A2", "A1", "A3"};
    private String slotLabels = "", slotHidden = "";

    void setSlotLabelMode(int mode) {
        prefs.edit().putInt("slot_labels", mode).apply();
        applySlotLabels(controllerConnected());
    }

    void applySlotLabels(boolean pad) {
        int mode = prefs.getInt("slot_labels", 0);
        // (a controller on another device, none here: the Wii U's letters, as that one's buttons go as the Wii U's)
        boolean remote = !pad && remoteController;
        String[] l = mode == 2 || (mode == 0 && remote) ? SLOT_LETTER.clone() : mode == 1 || !pad ? SLOT_ACTION.clone() : new String[3];
        StringBuilder hide = new StringBuilder();
        for (int k = 0; k < 3; k++) {
            if (l[k] != null) continue;
            l[k] = mapper.labelOf(mapper.map[SLOT_WIIU[k]]);
            if (l[k] != null) continue;
            l[k] = SLOT_LETTER[k];
            String c = SLOT_LETTER[k];  // HUD cluster; the drop slot's button, frame and highlight
            hide.append(",N_").append(c).append("_00,P_").append(c).append("_00,W_").append(c).append("DropSlide_00,W_")
                    .append(c).append("DropSlideFloat_00,W_").append(c).append("DropSlideSwich_00");
        }
        applyHudPlaces(pad);
        // the game's A and B: the letter printed on the controller button that presses them (else the game's own;
        // the game's X Y R names keep them too)
        String la = pad && mode != 2 ? mapper.printedLetter(mapper.map[0]) : null;
        String lb = pad && mode != 2 ? mapper.printedLetter(mapper.map[1]) : null;
        String s = String.join(",", l) + "," + (la != null ? la : "A") + "," + (lb != null ? lb : "B");
        if (!s.equals(slotLabels)) {
            slotLabels = s;
            Native.setSlotLabels(s);
            if (controls != null) controls.setSlotLabels(l);
        }
        if (!hide.toString().equals(slotHidden)) {
            slotHidden = hide.toString();
            applyHudHidden();
        }
    }

    // the game's A, B, X, Y on the TV in the controller's arrangement (InputMapper.hudPlaces: with the face buttons by
    // letters on a controller with Xbox's letters, its A at the bottom, the game's A is drawn at the bottom); with no
    // controller here as the Wii U has them
    private String hudPlaces = "";

    private void applyHudPlaces(boolean pad) {
        int[] p = pad ? mapper.hudPlaces() : new int[] {0, 1, 2, 3};
        String s = p[0] + "," + p[1] + "," + p[2] + "," + p[3];
        if (s.equals(hudPlaces)) return;
        hudPlaces = s;
        Native.setHudPlaces(s);
    }

    private void setShortcutHidden(boolean on) {
        if (on == shortcutHidden) return;
        shortcutHidden = on;
        applyHudHidden();
    }

    /** the game's Options on the GamePad picture (its rows: L_BtnOption_*; targets as of the last frame) */
    private boolean optionsOpen() {
        for (Target t : targets)
            if (t.key.startsWith("L_BtnOption_")) return true;
        return false;
    }

    private void setMenuTouch(boolean on) {
        if (controls != null) controls.setMenuBack(on && optionsOpen());  // (Save has its own Cancel)
        if (on == menuTouch) return;
        menuTouch = on;
        applyHudHidden();
    }

    private void setFilesTouch(boolean on) {
        if (on == filesTouch) return;
        filesTouch = on;
        applyHudHidden();
        if (filesBack != null) filesBack.setVisibility(on && !tv ? View.VISIBLE : View.GONE);
    }

    private static boolean controllerConnected() { return firstController() != null; }

    /** the first connected controller, or null */
    static InputDevice firstController() {
        for (int id : InputDevice.getDeviceIds()) {
            InputDevice d = InputDevice.getDevice(id);
            if (d != null && !d.isVirtual() && InputMapper.isController(d)) return d;
        }
        return null;
    }

    // ------------------------------------------------------------------ surface and screen layout
    @Override
    public void surfaceCreated(SurfaceHolder holder) {}

    @Override
    public void surfaceChanged(SurfaceHolder holder, int format, int w, int h) {
        boolean laidOut = surface != null && surface.getWidth() > 0 && surface.getHeight() > 0;
        surfaceW = laidOut ? surface.getWidth() : w;
        surfaceH = laidOut ? surface.getHeight() : h;
        bufScale = Math.min(1f, (float) h / surfaceH);
        updateBufferSize();  // may ask for another size: surfaceChanged again
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

    /** the second display shows the game picture (a TV by casting or an HDMI cable), this device the GamePad's */
    boolean gameOnSecondDisplay() { return prefs.getBoolean("second_display_game", false); }

    void setDrcOnSecondDisplay(boolean on) {
        prefs.edit().putBoolean("drc_second_display", on).putBoolean("second_display_game", false).apply();
        updateDrcDisplay();
    }

    void setGameOnSecondDisplay() {
        prefs.edit().putBoolean("second_display_game", true).apply();
        updateDrcDisplay();
    }

    // the GamePad picture there in the display's shape (its background reaches the edges) or 16:9
    boolean drcFill() { return prefs.getBoolean("drc_fill", true); }

    /** the TV picture covers the whole screen (rendered in the screen's shape, or stretched / filled), not 16:9 with bars */
    boolean tvFillsScreen() { return prefs.getInt("render_aspect", 0) != 0 || prefs.getInt("tv_aspect", 0) != 0; }

    void setDrcFill(boolean on) {
        prefs.edit().putBoolean("drc_fill", on).apply();
        if (drcDisplay != null) drcDisplay.setFill(on);
        updateLayout();
    }

    private void updateDrcDisplay() {
        if (drcDisplay == null) {
            drcDisplay = new DrcDisplay(this, active -> {
                updateLayout();
                ownDisplayMarks();
            });
            drcDisplay.setFill(drcFill());
        }
        drcDisplay.setTvPicture(gameOnSecondDisplay());
        drcDisplay.setWanted(started && resumed && (drcOnSecondDisplay() || gameOnSecondDisplay()));
    }

    private boolean drcOnOwnDisplay() { return drcDisplay != null && drcDisplay.active(); }

    // the GamePad on its own display: the chart's boxes followed each frame for the marks there (ChartMarks; on a
    // single screen focusTick does it while the GamePad picture shows)
    private boolean ownMarksTicking;
    private final android.view.Choreographer.FrameCallback ownMarksTick = new android.view.Choreographer.FrameCallback() {
        @Override
        public void doFrame(long now) {
            if (controls == null || !drcOnOwnDisplay() || !ChartMarks.I.active()) {
                ownMarksTicking = false;
                if (controls != null) controls.setChart(false, 0, 0, 0, 0, 0, 0, 0, 0, 0);
                ChartMarks.I.setDungeon(0, 0, 0, 0, null);
                return;
            }
            readTargets();
            trackView();
            chartMarks();
            android.view.Choreographer.getInstance().postFrameCallback(this);
        }
    };

    void ownDisplayMarks() {
        if (ownMarksTicking || !drcOnOwnDisplay() || !ChartMarks.I.active()) return;
        ownMarksTicking = true;
        android.view.Choreographer.getInstance().postFrameCallback(ownMarksTick);
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
        if (drcOnOwnDisplay()) {  // the GamePad has its own display
            layout = LAYOUT_TV;
            drcShown = false;
        }
        boolean screenRemote = screenRemote();
        if (screenRemote) drcShown = false;  // (another device shows the TV picture: the GamePad's here, controls over it)
        controls.setScreenButton(layout == LAYOUT_ON_DEMAND, drcShown);
        controls.setDrcBackdrop(screenRemote());
        controls.setMenuInInventory(menuInInventory());
        Native.setItemSheetLeft(menuInInventory());
        // the inset sits between the shoulder buttons, sized to leave them free
        float insetW = Math.min(W * 0.3f, W - 2 * (Math.min(W, H) / 7f * 3.6f));
        RectF inset = new RectF((W - insetW) / 2, 0, (W + insetW) / 2, insetW / DRC_ASPECT);
        if (drcShown) layout = -1;  // + (or the screen button): only the GamePad picture
        if (screenRemote) layout = -2;
        switch (layout) {
            case -2:  // the GamePad picture, the touch controls over it (a touch on the picture is the GamePad's)
                tv = new RectF(0, 0, -1, -1);
                drc = full;
                break;
            case -1:  // width -1: the TV picture is not drawn
                tv = new RectF(0, 0, -1, -1);
                drc = full;
                break;
            case LAYOUT_SIDE:
                tv = new RectF(0, 0, W * 0.7f, H);
                drc = new RectF(W * 0.7f, 0, W, H);
                break;
            case LAYOUT_TV:
            case LAYOUT_ON_DEMAND:
                tv = full;
                drc = null;
                break;
            case LAYOUT_DRC_LARGE:
                drc = full;
                tv = inset;
                break;
            default:
                tv = full;
                drc = inset;
                break;
        }
        float k = bufScale;  // buffer pixels
        Native.setLayout(new float[] {tv.left * k, tv.top * k, tv.right < 0 ? -1 : tv.width() * k, tv.right < 0 ? -1 : tv.height() * k},
                drc == null ? null : new float[] {drc.left * k, drc.top * k, drc.width() * k, drc.height() * k}, drc != null);
        // the GamePad picture of + takes the TV picture's shape: no jump between the two. The game lays it out in
        // that shape also while it's hidden (no GamePad picture in this layout), so + shows it ready, not 16:9 first
        boolean fillDrc = (drcShown || drc == null) && tvFillsScreen() && !drcForFiles;  // (the file select: its stretched sides flickered)
        if (!drcOnOwnDisplay()) Native.setDrcAspect(fillDrc ? W / H : 0);
        controls.setDrcRect(drc == null ? null : fillDrc ? drc : fit(drc, DRC_ASPECT));
        controls.setTvRect(tv.right < 0 ? null : fit(tv, TV_ASPECT));
        controls.setTvArea(tv.right < 0 ? null : tvFillsScreen() ? tv : fit(tv, TV_ASPECT));  // (the picture's own corners)
        controls.setClimbHud(prefs.getBoolean("mod_climb", false));
        controls.setPerfHud(prefs.getBoolean("perf_hud", false));
        controls.setPerfItems(prefs.getInt("perf_items", ControlsView.PERF_ALL));
        loadButtonMap();
        controls.perfSettings = this::perfSettingsLines;
        controls.setPerfPosition(prefs.getFloat("perf_x", -1), prefs.getFloat("perf_y", -1));
    }

    // ------------------------------------------------------------------ input
    private ItemRing itemRing;
    private boolean plusHeld;

    void repushInput() { pushInput(); }

    /** the game in play: no event, no pause menu, not the GamePad picture (as ItemRing.eligible) */
    private boolean padInPlay() {
        if (drcScreen()) return false;
        int[] s = Native.touchState();
        return s[0] != 0 && s[8] == 0 && s[9] == 0;
    }

    private void pushInput() {
        if (benchRunning) {  // the benchmark's input only (no touch, no controller; the first start's confirm press)
            Native.setPad(mapper.pulseButtons(), 0, 0, benchCamera ? 1 : 0, 0);
            return;
        }
        if (tour != null) {  // the shader tour's input only
            Native.setPad(tour.buttons, tour.lx, tour.ly, tour.rx, tour.ry);
            return;
        }
        if (controls == null) return;
        // − only swapped the TV and GamePad pictures (the GamePad is the only controller now): never sent
        int touchB = controls.buttons();
        int padRaw = mapper.padButtons | mapper.keyButtons | padButtons();
        // a controller's ZL held with ZL locks every enemy: let go for a moment for each new lock (ControlsView.relockEnemy)
        controls.setPadZl((padRaw & Native.ZL) != 0);
        int b = (touchB | (padRaw & ~controls.padZlMask())) & ~Native.MINUS;
        // an item button on d-pad down (InputMapper.setExtraItem) only in play: elsewhere it is d-pad down
        int downItem = mapper.downItemBits();
        if (downItem != 0 && !padInPlay()) b &= ~downItem;
        // a touch hint (GameHint) shows: a controller's / the keyboard's buttons and stick work its choices, the game
        // gets none of them (nor the press that closed it, until it is let go)
        if (controls.hintPad(mapper.padButtons | mapper.keyButtons, pick(0, mapper.ly, mapper.keyLY()))) {
            Native.setPad(0, 0, 0, 0, 0);
            return;
        }
        // singing the songs: L on a controller holds the mic button (not for the game while Link conducts, or when
        // no mod of ours uses L)
        if (songs != null) {
            boolean padL = songs.buttonShown() && ((mapper.padButtons | mapper.keyButtons) & Native.L) != 0;
            if (padL != padLHeld) {
                padLHeld = padL;
                songs.setHeld(false, padL);
            }
            if (padL) b &= ~(songs.conducting() ? Native.L | Native.JUMP | Native.MOON : Native.L);  // (a jump on its button still jumps)
        }
        // in play the game reads L only to turn its GamePad screen between Items and Map (with its tab sound): only with
        // Controls > Controller > L turns the GamePad page (for a GamePad picture on a second screen)
        if ((b & Native.L) != 0 && !prefs.getBoolean("l_drc_pages", false) && padInPlay()) b &= ~Native.L;
        // + (touch or controller) shows or hides the full GamePad picture, unless the GamePad has a
        // display of its own: there + goes to the game
        if (!drcOnOwnDisplay() && !screenRemote() && front != Native.FRONT_OPENING) {  // (the opening: + is the game's skip)
            boolean plus = (b & Native.PLUS) != 0, press = plus && !plusHeld;
            plusHeld = plus;
            if (press) controls.post(this::plusToggle);  // not inside this call: the switch releases the touch controls
            b &= ~Native.PLUS;
        }
        boolean focus = focusMode(), ownStick = focus && !Native.drcGameCursor();  // (the game's cursor: the stick is the game's too)
        if (focus) {  // the controller works the GamePad screen through its touch targets (focusTick)
            int pad = mapper.padButtons | mapper.keyButtons;
            // d-pad down with an item button on it: moves the highlight; with ZL held it puts the item there
            if (downItem != 0) {
                boolean zl = (pad & Native.ZL) != 0;
                pad &= zl ? ~Native.DOWN : ~downItem;
                if (zl) b &= ~Native.ZL;
            }
            if (!Native.drcGameCursor()) {  // (where the game has its own cursor, its buttons are the game's)
                int pressed = pad & ~focusPad;
                if ((pressed & Native.B) != 0 && touchAt == 0) touchAt = -2;  // B: the game's Back, else back to the game (focusTick)
                if ((pressed & Native.A) != 0 && touchAt == 0) touchAt = -1;  // A: a tap (focusTick)
                int slot = (pressed & Native.Y) != 0 ? 0 : (pressed & Native.X) != 0 ? 1 : (pressed & Native.R) != 0 ? 2 : -1;
                if (slot >= 0 && touchAt == 0) touchSlot = slot;  // Y / X / R: the item onto that button
                if ((pressed & Native.UP) != 0) focusMove(0, -1, true);
                if ((pressed & Native.DOWN) != 0) focusMove(0, 1, true);
                if ((pressed & Native.LEFT) != 0) focusMove(-1, 0, true);
                if ((pressed & Native.RIGHT) != 0) focusMove(1, 0, true);
                b &= ~(Native.A | Native.B | Native.X | Native.Y | Native.R | Native.UP | Native.DOWN | Native.LEFT | Native.RIGHT);
            }
            focusPad = pad;
        }
        if (focus || drcScreen() || screenRemote() || padPicture()) startFocusTick();
        float lx = ownStick ? 0 : pick(controls.stickX(0), mapper.lx, mapper.keyLX(), padHost != null ? padAxis(padHost.lx) : 0);
        float ly = ownStick ? 0 : pick(controls.stickY(0), mapper.ly, mapper.keyLY(), padHost != null ? padAxis(padHost.ly) : 0);
        if (itemRing != null) {  // X / Y / R held for the item ring (ItemRing)
            b = itemRing.filter(b, touchB, pick(0, mapper.lx, mapper.keyLX()), pick(0, mapper.ly, mapper.keyLY()));
            if (itemRing.takesLeftStick()) lx = ly = 0;
        }
        float rx = pick(controls.stickX(1), mapper.rx, mapper.keyRX(), padHost != null ? padAxis(padHost.rx) : 0);
        float ry = pick(controls.stickY(1), mapper.ry, mapper.keyRY(), padHost != null ? padAxis(padHost.ry) : 0);
        if (mapper.keyLX() != 0 && mapper.keyLY() != 0) { lx *= 0.7071f; ly *= 0.7071f; }
        if (controls.conductPads()) {  // conducting by the note pads: the time pad is the stick, the note pad the C stick (ControlsView)
            int beat = controls.conductTime(), dir = controls.conductNote();
            lx = pick(beat == 4 ? -1 : beat == 6 ? 1 : 0, mapper.lx, mapper.keyLX());
            rx = pick(dir == 2 ? 1 : dir == 4 ? -1 : 0, mapper.rx, mapper.keyRX());
            ry = pick(dir == 1 ? 1 : dir == 3 ? -1 : 0, mapper.ry, mapper.keyRY());
        }
        if (songs != null && songs.timeBySwitch()) {  // conducting: the time switch's time (SongListener)
            int beat = songs.beat();
            lx = beat == 4 ? -1 : beat == 6 ? 1 : 0;
            ly = 0;
            if (songs.conductsByVoice() || songs.nudging()) {  // the mic button held: the note sung (or a song handed over)
                int dir = songs.cStick();
                rx = dir == 2 ? 1 : dir == 4 ? -1 : 0;
                ry = dir == 1 ? 1 : dir == 3 ? -1 : 0;
            }
        }
        if (songs != null && songs.listTime() != 0) {  // a song from the list's play button: its time, the C stick up (SongListener)
            int beat = songs.listTime();
            lx = beat == 4 ? -1 : beat == 6 ? 1 : 0;
            ly = 0;
            rx = 0;
            ry = 1;
        }
        if (windByPad(rx, ry)) rx = ry = 0;  // − held, the right stick chooses the wind (the camera gets none of it)
        b |= autoButtons;  // (the start into a save file after a mod pack restart: A on the title)
        Native.setPad(b, lx, ly, rx, ry);
    }

    // ---- the controller on the full GamePad picture (single screen, +): the game's GamePad screen takes
    // touch only (its item pages), so a highlight sits on one of its touch targets (the game's own hit
    // boxes, Native.drcTargets) and moves with the D-pad / left stick; A taps it, Y / X / R put the
    // highlighted item onto that button (a drag to the game's drop box, B_Y_00 / B_X_00 / B_R_00, as a
    // finger does), B taps the game's Back button where it shows one (an open bag, the chart from the map)
    // and otherwise goes back to the game, + as always. Touch works the whole time; a touch hides the
    // highlight (lastController null). Where the game shows its own cursor (Options), the buttons are the game's.
    private static final class Target {
        String key, name;
        float x, y, w, h;
    }

    private final java.util.ArrayList<Target> targets = new java.util.ArrayList<>();
    private String focusKey;
    private float focusX = 0.5f, focusY = 0.5f, touchX, touchY;
    private int focusPad;
    private boolean focusTicking, focusTouch;
    private long stickRepeatAt;
    // the touch the buttons make: touchAt its start (-1: A pressed, -2: B pressed; start next frame), touchSlot the
    // Y / X / R drop box it goes to (-1: a tap where it is)
    private long touchAt;
    private int touchSlot = -1;
    private float fromX, fromY, toX, toY;
    private static final String[] SLOT_BOXES = {"B_Y_00", "B_X_00", "B_R_00"};
    private static final long TAP_MS = 100, DRAG_HOLD = 150, DRAG_MOVE = 150, DRAG_DROP = 100;

    /** the drop box of slot i on the screen now (the one inside the picture: a sliding page shows two) */
    private Target slotBox(int i) {
        Target best = null;
        for (Target t : targets)
            if (t.name.equals(SLOT_BOXES[i]) && t.x > 0f && t.x < 1f && (best == null || Math.abs(t.x - 0.5f) < Math.abs(best.x - 0.5f))) best = t;
        return best;
    }

    // a finger held on an item button's drop box (from its touch down) for SLOT_HOLD_MS empties that button: the game
    // has no way to (a controller: its button on the item already on it, focusFrame)
    private static final long SLOT_HOLD_MS = 600;
    private int holdBox = -1;
    private long holdSince;
    private boolean holdDone;

    private void slotHold(long ms) {
        int k = -1;
        for (int i = 0; i < 3 && k < 0 && ControlsView.fingerDown; i++) {
            Target b = slotBox(i);
            if (b != null && inBox(b, ControlsView.fingerX, ControlsView.fingerY) && inBox(b, ControlsView.fingerStartX, ControlsView.fingerStartY)) k = i;
        }
        if (k != holdBox) {
            holdBox = k;
            holdSince = ms;
            holdDone = false;
        } else if (k >= 0 && !holdDone && ms - holdSince >= SLOT_HOLD_MS) {
            holdDone = true;
            int[] ring = Native.itemRing();
            int btn = SLOT_BUTTON[k];
            if (ring.length > 21 + btn && ring[0] != -1 && ring[21 + btn] != 0xFF) {
                Native.equipItem(btn, 0xFF);
                controls.performHapticFeedback(android.view.HapticFeedbackConstants.LONG_PRESS);
            }
        }
    }

    private static boolean inBox(Target t, float x, float y) {
        return Math.abs(x - t.x) <= t.w / 2 && Math.abs(y - t.y) <= t.h / 2;
    }

    /** the game's Back button on the screen now (an open bag or chart: L_BtnBack_00), or null */
    private Target backButton() {
        for (Target t : targets)
            if (t.key.startsWith("L_BtnBack_") && t.x > 0f && t.x < 1f) return t;
        return null;
    }

    private void touchTo(boolean down, float x, float y) {
        touchX = x;
        touchY = y;
        focusTouch = down;
        Native.setDrcTouch(down, ControlsView.pictureU(x), ControlsView.pictureV(y));
    }

    private boolean focusMode() {
        return drcShown && !drcForFiles && front == 0 && lastController != null && !drcOnOwnDisplay();
    }

    private void startFocusTick() {
        if (focusTicking) return;
        focusTicking = true;
        android.view.Choreographer.getInstance().postFrameCallback(focusTick);
    }

    private void readTargets() {
        targets.clear();
        chart = null;
        floors.clear();
        figs.clear();
        java.util.HashMap<String, Integer> seen = new java.util.HashMap<>();
        for (String t : Native.drcTargets().split(";")) {
            String[] f = t.trim().split(" ");
            if (f.length != 6) continue;
            try {
                Target g = new Target();
                g.x = Float.parseFloat(f[2]);
                g.y = Float.parseFloat(f[3]);
                g.w = Float.parseFloat(f[4]);
                g.h = Float.parseFloat(f[5]);
                if (f[1].startsWith("@fig:")) {  // a dungeon map's figure: "@fig:<turn>:<alpha>" (ChartMarks), not a target
                    String[] q = f[1].split(":");
                    if (q.length == 3) figs.add(new ChartMarks.Fig(f[0], g.x, g.y, g.w, g.h, Float.parseFloat(q[1]), Integer.parseInt(q[2])));
                    continue;
                }
                if (f[1].equals("@floor")) {  // a dungeon map's floor: its place and scale (ChartMarks), not a target
                    g.name = f[0];
                    floors.add(g);
                    continue;
                }
                if (f[0].equals("B_Area_00") && f[1].equals("N_All_00")) {  // the sea chart: one area, 7 x 7 squares
                    chart = g;
                    for (int r = 0; r < 7; r++)
                        for (int c = 0; c < 7; c++) {
                            Target q = new Target();
                            q.w = g.w / 7f;
                            q.h = g.h / 7f;
                            q.x = g.x - g.w / 2f + (c + 0.5f) * q.w;
                            q.y = g.y - g.h / 2f + (r + 0.5f) * q.h;
                            q.key = TILE + r + c;
                            q.name = f[0];
                            targets.add(q);
                        }
                    continue;
                }
                // a page-wide swipe area is not a button; the dungeon map's floor map (N_MapArea_00) is (a tap zooms it)
                if (g.w * g.h > 0.25f && !f[1].equals("N_MapArea_00")) continue;
                String k = f[1] + "/" + f[0];
                Integer n = seen.get(k);
                seen.put(k, n == null ? 1 : n + 1);
                g.key = n == null ? k : k + "#" + n;
                g.name = f[0];
                targets.add(g);
            } catch (NumberFormatException ignored) {
            }
        }
        // a chart square's view (N_StandardPos_00; Native.drcTargets gives the box of the one shown: B_Btn_00
        // small, B_BtnFullZoom_00 magnified): the only target while it is open, as it takes the touches
        Target view = null;
        for (Target t : targets)
            if (t.key.startsWith("N_StandardPos_00/")) view = t;
        // a chart opened over the sea chart (Tingle's Chart): its Back button is the only target, unframed
        backOnly = false;
        if (view == null && chart != null) {
            view = backButton();
            backOnly = view != null;
        }
        if (view != null) {
            targets.clear();
            targets.add(view);
        }
        addOptionsArrow();
    }

    private boolean backOnly, wasBackOnly;  // readTargets: only the game's Back button (A and B tap it), no highlight
    private String keptKey;
    private float keptX, keptY;

    private Target chart;  // the sea chart's area (readTargets), or null
    private final java.util.ArrayList<Target> floors = new java.util.ArrayList<>();  // a dungeon map's floors drawn (readTargets)
    // ... and its figures (statue, ship, searchlights ...: ChartMarks.Fig) drawn
    private final java.util.ArrayList<ChartMarks.Fig> figs = new java.util.ArrayList<>();

    private static final String TILE = "N_All_00/B_Area_00/";
    // the chart square (row * 10 + column) whose view is open: the one A opened, or the one a finger
    // touched (trackView); forgotten once its view closes
    private int tile = -1;
    private boolean viewOpen;
    /** the chart square under a point, or -1 */
    private int tileAt(float x, float y) {
        if (chart == null) return -1;
        int c = (int) Math.floor((x - (chart.x - chart.w / 2f)) / (chart.w / 7f)), r = (int) Math.floor((y - (chart.y - chart.h / 2f)) / (chart.h / 7f));
        return c < 0 || c > 6 || r < 0 || r > 6 ? -1 : r * 10 + c;
    }

    private float[] tileCenter(int t) {
        float tw = chart.w / 7f, th = chart.h / 7f;
        return new float[] {chart.x - chart.w / 2f + (t % 10 + 0.5f) * tw, chart.y - chart.h / 2f + (t / 10 + 0.5f) * th};
    }

    /** where B taps to close a chart square's view: the magnified one on itself, the small one on its square again */
    private float[] viewClose(Target v) {
        if (v.key.endsWith("/B_BtnFullZoom_00") || chart == null || tile < 0) return new float[] {v.x, v.y};
        return tileCenter(tile);
    }

    /** the chart square's magnified view, when one is open (readTargets keeps only it) */
    private Target chartView() {
        return targets.size() == 1 && targets.get(0).key.startsWith("N_StandardPos_00/") ? targets.get(0) : null;
    }

    /** the Y / X / R slots (their icons, and the drop boxes items are dragged onto) are targets only for Y / X / R
     * (slotBox); the highlight doesn't go there, as in the game's own menu */
    private static boolean selectable(Target t) {
        return !t.key.startsWith("N_DropSlide_00/") && !t.key.startsWith("L_YItemIcon_") && !t.key.startsWith("L_XItemIcon_") && !t.key.startsWith("L_RItemIcon_");
    }

    private Target focused() {
        Target best = null;
        float bd = Float.MAX_VALUE;
        for (Target t : targets) {
            if (!selectable(t)) continue;
            if (t.key.equals(focusKey)) return t;
            float d = dist(t.x - focusX, t.y - focusY);
            if (d < bd) { bd = d; best = t; }
        }
        return best;  // the target gone (page changed): the nearest one
    }

    private static float dist(float dx, float dy) { return (float) Math.hypot(dx * 16f / 9f, dy); }

    /** the highlight to the next target in direction dx, dy; on a page arrow pointing that way, a fresh press (turn:
     * not a held stick's repeat, which would run on through the pages) turns the page, as A on it */
    private void focusMove(int dx, int dy, boolean turn) {
        readTargets();
        Target cur = focused();
        if (cur == null) return;
        boolean left = cur.key.equals(OPTIONS_ARROW) || cur.key.startsWith("L_MenuArrowL_"), right = cur.key.startsWith("L_MenuArrowR_");
        if (dy == 0 && ((left && dx < 0) || (right && dx > 0))) {
            if (turn && touchAt == 0) touchAt = -1;
            return;
        }
        Target best = null;
        float bs = Float.MAX_VALUE;
        for (Target t : targets) {
            if (t == cur || !selectable(t)) continue;
            float ex = (t.x - cur.x) * 16f / 9f, ey = t.y - cur.y;
            float along = ex * dx + ey * dy, across = Math.abs(ex * dy - ey * dx);
            if (along < 0.01f) continue;
            float s = along + 2.5f * across;
            if (s < bs) { bs = s; best = t; }
        }
        if (best != null) setFocused(best);
    }

    private void setFocused(Target t) {
        focusKey = t.key;
        focusX = t.x;
        focusY = t.y;
    }

    private final android.view.Choreographer.FrameCallback focusTick = new android.view.Choreographer.FrameCallback() {
        @Override
        public void doFrame(long now) {
            boolean game = Native.drcGameCursor(), screen = drcScreen();
            // the GamePad picture as the backdrop (the game picture on another device): its targets, arrow and marks
            // as in the GamePad view of +, but the game goes on (it is shown elsewhere)
            boolean backdrop = !screen && (screenRemote() || padPicture()) && front == 0;
            if (controls == null || !focusMode()) {
                if (focusTouch) touchTo(false, touchX, touchY);
                touchAt = 0;
                touchSlot = -1;
                if (controls != null) controls.setFocus(false, 0, 0, 0, 0, false);
                if (controls == null || !(screen || backdrop)) {
                    if (controls != null) controls.setChart(false, 0, 0, 0, 0, 0, 0, 0, 0, 0);
                    ChartMarks.I.setDungeon(0, 0, 0, 0, null);
                    pauseForMenu(false);
                    if (controls != null) controls.setOptionsArrow(false, 0, 0, 0, 0);
                    if (padHost != null) padHost.sendArrow(false, 0, 0, 0, 0, 0);
                    focusTicking = false;
                    return;
                }
                pauseForMenu(screen);
                readTargets();  // touch only: still follows the chart views (which square a finger opened)
                slotHold(now / 1000000);
                optionsArrow();
                strayPages();
                trackView();
                chartMarks();
                android.view.Choreographer.getInstance().postFrameCallback(this);
                return;
            }
            pauseForMenu(true);
            readTargets();
            slotHold(now / 1000000);
            optionsArrow();
            strayPages();
            trackView();
            chartMarks();
            Target cur = game ? null : focused();
            if (cur != null) setFocused(cur);
            focusFrame(game, cur);
            android.view.Choreographer.getInstance().postFrameCallback(this);
        }
    };

    // a single screen shows the GamePad picture of + instead of the game: the game pauses meanwhile (as its own TV
    // menu does with the Pro Controller); its own Options / Save pause it too and are left alone

    private void pauseForMenu(boolean on) {
        Native.setGamePaused(on && Native.pauseKind() == 0);  // (each frame: it runs out by itself; the game's Options / Save pause it their own way)
    }

    /** the GamePad picture is shown on its own (the screen the controller and the fingers work) */
    private boolean drcScreen() {
        return drcShown && !drcForFiles && front == 0 && !drcOnOwnDisplay();
    }

    /** the progress marks on the game's sea chart (ControlsView.setChart): its area, and the square view open (the
     *  square trackView knows); none while another chart lies over it (Tingle's Chart: backOnly) */
    private void chartMarks() {
        if (controls == null) return;
        // a dungeon's map: its floors drawn and the floor map's box (N_MapArea_00)
        Target box = null;
        for (Target t : targets) if (t.key.startsWith("N_MapArea_00/")) box = t;
        float[] fl = new float[floors.size() * 5];
        for (int i = 0; i < floors.size(); i++) {
            Target t = floors.get(i);
            String n = t.name;  // N_1F_00 / N_B1F_00
            int f = n.charAt(2) == 'B' ? -(n.charAt(3) - '0') : n.charAt(2) - '1';
            fl[i * 5] = f;
            fl[i * 5 + 1] = t.x;
            fl[i * 5 + 2] = t.y;
            fl[i * 5 + 3] = t.w;
            fl[i * 5 + 4] = t.h;
        }
        ChartMarks.I.setFigures(figs);
        if (box != null && fl.length > 0) ChartMarks.I.setDungeon(box.x, box.y, box.w, box.h, fl);
        else ChartMarks.I.setDungeon(0, 0, 0, 0, null);
        Target v = chartView();
        if (chart == null || backOnly || v != null && tile < 0) {
            controls.setChart(false, 0, 0, 0, 0, 0, 0, 0, 0, 0);
            return;
        }
        int sq = v != null ? tile / 10 * 7 + tile % 10 + 1 : 0;
        if (v == null) controls.setChart(true, chart.x, chart.y, chart.w, chart.h, 0, 0, 0, 0, 0);
        else controls.setChart(true, chart.x, chart.y, chart.w, chart.h, sq, v.x, v.y, v.w, v.h);
    }

    /** a chart square's view opening and closing: which square it belongs to, the highlight back there after */
    private void trackView() {
        // Tingle's Chart: the highlight comes back to where it was (its button) once it is closed
        if (backOnly && !wasBackOnly) { keptKey = focusKey; keptX = focusX; keptY = focusY; }
        if (!backOnly && wasBackOnly && keptKey != null) { focusKey = keptKey; focusX = keptX; focusY = keptY; }
        wasBackOnly = backOnly;
        Target open = chartView();
        if (viewOpen && open == null) {  // a view closed: the highlight back on its square
            if (tile >= 0 && chart != null) {
                float[] p = tileCenter(tile);
                focusKey = TILE + tile / 10 + tile % 10;
                focusX = p[0];
                focusY = p[1];
            }
            tile = -1;
        }
        if (!viewOpen && open != null && tile < 0) {  // opened by touch (A sets it before)
            tile = tileAt(ControlsView.fingerX, ControlsView.fingerY);
            if (Os.getenv("WWHD_HUD_LOG") != null)
                Log.i(TAG, String.format(java.util.Locale.ROOT, "[drc-view] opened by touch at %.3f %.3f: square %d (chart %s)", ControlsView.fingerX, ControlsView.fingerY, tile,
                        chart == null ? "-" : chart.x + " " + chart.y + " " + chart.w + " " + chart.h));
        }
        viewOpen = open != null;
    }

    // ---- the options as a page of the GamePad inventory (Controls > Menu button in the game off): an arrow left of
    // the items page, where the game's own left arrow sits on the page right of it, opens them; they slide in
    // from the left and go back to the inventory (OptionsMenu inInventory)
    private static final String OPTIONS_ARROW = "app/options";

    // (a GamePad device: its picture has the arrow too, drawn there, PadActivity)
    boolean menuInInventory() {
        return !prefs.getBoolean("menu_overlay", false) && (!drcOnOwnDisplay() || drcDisplay.remoteOnly());
    }

    void setMenuOverlay(boolean on) {
        prefs.edit().putBoolean("menu_overlay", on).apply();
        if (controls != null) controls.setMenuInInventory(menuInInventory());
        // (switched on from the options page in the inventory: the paper stays under it until it has slid away)
        Native.setItemSheetLeft(menuInInventory() || (optionsPage != null && optionsPage.isShowing()));
    }

    /** the arrow on the items page (the game's right arrow there, no left one): a target too, for the controller
     * (readTargets, each time it reads them) */
    private void addOptionsArrow() {
        Target r = null;
        boolean leftArrow = false;
        for (Target t : targets) {
            if (t.key.startsWith("L_MenuArrowR_")) r = t;
            if (t.key.startsWith("L_MenuArrowL_") || t.key.startsWith("L_BtnBack_")) leftArrow = true;
        }
        // (where the game shows its own cursor the arrow stays out; on the console's GamePad view, the game going on
        // elsewhere, + is the game's menu: the arrow is there too)
        if (r == null || leftArrow || (Native.drcGameCursor() && !screenRemote() && !padPicture()) || !menuInInventory()) return;
        Target a = new Target();
        a.key = a.name = OPTIONS_ARROW;
        a.x = 1f - r.x;  // (the game's arrows sit mirrored: L_MenuArrowL_00 at -686, R at +686)
        a.y = r.y;
        a.w = r.w;
        a.h = r.h;
        targets.add(a);
    }

    /** the pages left of the items with no options page there (a save state taken while it was out): back to the items */
    private void strayPages() {
        if (Native.itemPageX() > 0.5f && (optionsPage == null || !optionsPage.isShowing()) && !Native.itemPageGliding())
            Native.setItemPageDrag(2, 0);
    }

    private void optionsArrow() {
        for (Target a : targets)  // (it fades as the pages move: gone once the options page is out)
            if (a.key.equals(OPTIONS_ARROW) && Native.itemPageX() < 1279f) {
                float h = a.h * 148f / 128f, alpha = 1f - Math.min(1f, Math.abs(Native.itemPageX()) / 1280f);
                controls.setOptionsArrow(true, a.x, a.y, a.w, h);  // (its hit box is 128 of the part's 148 units high)
                controls.setOptionsArrowAlpha(alpha);  // (out as another page comes in)
                if (padHost != null)  // (a GamePad device draws it over its picture: PadActivity)
                    padHost.sendArrow(true, alpha, ControlsView.pictureU(a.x - a.w / 2), ControlsView.pictureV(a.y - h / 2),
                            ControlsView.pictureU(a.x + a.w / 2), ControlsView.pictureV(a.y + h / 2));
                return;
            }
        controls.setOptionsArrow(false, 0, 0, 0, 0);
        if (padHost != null) padHost.sendArrow(false, 0, 0, 0, 0, 0);
    }

    private OptionsMenu optionsPage;
    private boolean pageReady;  // optionsPage is built (prepareOptionsPage) and not shown yet

    // the page is built a moment after the inventory opens (~0.2 s of views on a phone, which held up the arrow's
    // tap): the tap then only shows it
    private final Runnable prepareOptionsPage = () -> {
        // (the GamePad view of +, or the console's GamePad view with the game picture elsewhere)
        if (!(drcScreen() || screenRemote() || padPicture()) || !menuInInventory() || pageReady || (optionsPage != null && optionsPage.isShowing())) return;
        optionsPage = new OptionsMenu(this, false, true);
        optionsPage.create();
        optionsPage.layOutAhead();
        pageReady = true;
    };

    ControlsView controlsView() { return controls; }

    private LayoutPresets layoutPresets;

    LayoutPresets layoutPresets() {
        if (layoutPresets == null) layoutPresets = new LayoutPresets(this);
        return layoutPresets;
    }

    @Override
    public void onOptionsPage() {
        if (optionsPage != null && optionsPage.isShowing()) return;
        if (focusTouch) touchTo(false, touchX, touchY);
        controls.removeCallbacks(prepareOptionsPage);
        if (!pageReady) optionsPage = new OptionsMenu(this, false, true);
        pageReady = false;
        optionsPage.show();
        optionsPage.slideIn();
    }

    /** from the options page to the game (an action that shows its effect there: a state loaded...) */
    void leaveInventory() {
        if (drcShown && !drcForFiles) onScreenToggle();
    }

    @Override
    public boolean pageSwipeFrom(float x, float y) {
        if (pageArrow("L_MenuArrowR_") == null && pageArrow("L_MenuArrowL_") == null) return false;  // not the item pages
        // its Options / Save, an open bag (the game's cursor: not on the console's GamePad view, where + is the game's menu)
        if ((Native.drcGameCursor() && !screenRemote() && !padPicture()) || backButton() != null) return false;
        for (Target t : targets)
            if (Math.abs(x - t.x) <= t.w / 2 && Math.abs(y - t.y) <= t.h / 2) return false;
        return true;
    }

    /** the game's page arrow on the screen now (its hit box), or null */
    private Target pageArrow(String prefix) {
        for (Target t : targets)
            if (t.key.startsWith(prefix)) return t;
        return null;
    }

    @Override
    public void onGamePageDrag(float dx) {
        // (left of the items only to the options page: with the menu button on there is none)
        if (!menuInInventory() && pageArrow("L_MenuArrowL_") == null) dx = Math.min(0, dx);
        Native.setItemPageDrag(1, dx);
    }

    // past a quarter of the screen or flicked, and the game has a page that way: they glide there, then a tap on its
    // arrow switches the game's state (its slide has nothing left to move); else they glide back
    @Override
    public void onGamePageDragEnd(float dx, float v) {
        boolean go = Math.abs(dx) > 320 || (Math.abs(v) > 1500 && Math.signum(v) == Math.signum(dx));
        Target t = !go ? null : pageArrow(dx > 0 ? "L_MenuArrowL_" : "L_MenuArrowR_");
        float base = pageArrow("L_MenuArrowL_") != null ? -1280 : 0;  // (the page right of the items shows the left arrow)
        Native.setItemPageDrag(2, t == null ? base : dx > 0 ? 0 : -1280);
        if (t != null) tapWhenArrived(t, android.os.SystemClock.uptimeMillis() + 1000);
    }

    private void tapWhenArrived(Target t, long until) {
        if (Native.itemPageGliding() && android.os.SystemClock.uptimeMillis() < until) {
            controls.postDelayed(() -> tapWhenArrived(t, until), 16);
            return;
        }
        Native.setItemPageDrag(0, 0);
        Native.setDrcTouch(true, ControlsView.pictureU(t.x), ControlsView.pictureV(t.y));  // a tap on it, long enough for the game to see
        controls.postDelayed(() -> Native.setDrcTouch(false, ControlsView.pictureU(t.x), ControlsView.pictureV(t.y)), TAP_MS);
    }

    // a drag to the right on the items page: the pages follow (the options page with them); let go past a quarter of
    // the screen or flicked, they glide on to it, else back (and the page closes)
    @Override
    public void onPageDrag(float dx) {
        if (optionsPage == null || !optionsPage.isShowing()) {
            controls.removeCallbacks(prepareOptionsPage);
            if (!pageReady) prepareOptionsPage.run();
            if (!pageReady) return;
            pageReady = false;
            optionsPage.show();
        }
        Native.setItemPageDrag(1, Math.max(0, dx));
    }

    @Override
    public void onPageDragEnd(float dx, float v) {
        if (optionsPage != null && optionsPage.isShowing()) optionsPage.dragEnd(dx > 0 && v >= 0 && (dx > 320 || v > 1500));
        else Native.setItemPageDrag(2, 0);
    }

    /** the back key in the + menu, as B there: the game's Options / Save close, else its Back button (an open bag or
     * chart) is tapped, else back to the game */
    private void inventoryBack() {
        if (Native.drcGameCursor()) { controls.pulse(Native.B); return; }
        Target back = backButton();
        if (back == null) { onScreenToggle(); return; }
        Native.setDrcTouch(true, ControlsView.pictureU(back.x), ControlsView.pictureV(back.y));
        controls.postDelayed(() -> Native.setDrcTouch(false, ControlsView.pictureU(back.x), ControlsView.pictureV(back.y)), TAP_MS);
    }

    /** the page slid back to the inventory: the next one is built ahead again */
    void optionsPageClosed() {
        Native.setItemSheetLeft(menuInInventory());
        controls.removeCallbacks(prepareOptionsPage);
        controls.postDelayed(prepareOptionsPage, 400);
    }

    private void focusFrame(boolean game, Target cur) {
        // the left stick: a tilt moves the highlight once, held it repeats
        float x = pick(mapper.lx, mapper.keyLX(), 0), y = pick(mapper.ly, mapper.keyLY(), 0);
        long ms = android.os.SystemClock.uptimeMillis();
        if (!game && Math.hypot(x, y) > 0.6f) {
            if (ms >= stickRepeatAt) {
                boolean first = stickRepeatAt == 0;
                stickRepeatAt = ms + (first ? 350 : 140);
                if (Math.abs(x) > Math.abs(y)) focusMove(x > 0 ? 1 : -1, 0, first);
                else focusMove(0, y > 0 ? -1 : 1, first);
                cur = focused();
            }
        } else {
            stickRepeatAt = 0;
        }
        // Y / X / R on an item: down on it, hold, slide to the drop box, hold, let go (as a finger); on the item that
        // button has already: the button is left empty
        if (touchSlot >= 0 && touchAt == 0) {
            Target box = slotBox(touchSlot);
            int btn = SLOT_BUTTON[touchSlot], inv = cur != null ? itemPageSlot(cur.key) : -1;
            int[] ring = inv >= 0 ? Native.itemRing() : null;
            if (ring != null && ring.length > 21 + btn && ring[0] != -1 && ring[21 + btn] == inv) {
                Native.equipItem(btn, 0xFF);
                touchSlot = -1;
            } else if (cur != null && box != null && cur.key.contains("L_BtnItemIcon")) {
                fromX = cur.x;
                fromY = cur.y;
                toX = box.x;
                toY = box.y;
                touchAt = ms;
                touchTo(true, fromX, fromY);
            } else {
                touchSlot = -1;
            }
        } else if (touchAt == -2) {  // B: a tap on the game's Back button, or the inventory closes
            Target view = chartView(), back = view != null ? null : backButton();
            if (view != null) {  // a chart square's view: the tap that closes it
                float[] p = viewClose(view);
                touchAt = ms;
                touchTo(true, p[0], p[1]);
            } else if (back != null) {
                touchAt = ms;
                touchTo(true, back.x, back.y);
            } else {
                touchAt = 0;
                controls.post(MainActivity.this::onScreenToggle);
            }
        } else if (touchAt == -1) {  // A: a tap on the highlight
            if (cur != null && cur.key.startsWith(TILE)) tile = Integer.parseInt(cur.key.substring(TILE.length()));
            if (cur != null && cur.key.equals(OPTIONS_ARROW)) {
                touchAt = 0;
                controls.post(() -> {
                    onOptionsPage();
                    if (optionsPage != null) optionsPage.openedByController();
                });
            } else if (cur != null) {
                touchAt = ms;
                touchTo(true, cur.x, cur.y);
            } else {
                touchAt = 0;
            }
        } else if (touchAt > 0) {
            long t = ms - touchAt;
            if (touchSlot < 0) {
                if (t >= TAP_MS) {
                    touchTo(false, touchX, touchY);
                    touchAt = 0;
                }
            } else if (t < DRAG_HOLD) {
                touchTo(true, fromX, fromY);
            } else if (t < DRAG_HOLD + DRAG_MOVE + DRAG_DROP) {
                float k = Math.min(1f, (t - DRAG_HOLD) / (float) DRAG_MOVE);
                touchTo(true, fromX + (toX - fromX) * k, fromY + (toY - fromY) * k);
            } else {
                touchTo(false, toX, toY);
                touchAt = 0;
                touchSlot = -1;
            }
        }
        // (the options page out: it has its own cursor, the arrow's would stay under it)
        if (cur == null || backOnly || (optionsPage != null && optionsPage.isShowing())) controls.setFocus(false, 0, 0, 0, 0, false);
        else controls.setFocus(true, cur.x, cur.y, cur.w, cur.h, focusTouch);
    }

    // touchSlot (Y, X, R) -> the save's item button (X, Y, R); the item page's slots L_BtnItemIcon_00..18 -> inventory
    // slots (as aspect.cpp kItemPageSlot)
    private static final int[] SLOT_BUTTON = {1, 0, 2};
    private static final int[] ITEM_PAGE_SLOT = {0, 18, 3, 6, 13, 14, 15, 7, 11, 5, 12, 19, 16, 17, 8, 4, 20, 9, 10};

    /** the inventory slot of an item page target ("L_BtnItemIcon_NN/..."), else -1 */
    private static int itemPageSlot(String key) {
        if (!key.startsWith("L_BtnItemIcon_") || key.length() < 16) return -1;
        try {
            int i = Integer.parseInt(key.substring(14, 16));
            return i < ITEM_PAGE_SLOT.length ? ITEM_PAGE_SLOT[i] : -1;
        } catch (NumberFormatException e) {
            return -1;
        }
    }

    private static float pick(float a, float b, float c) {
        float v = a;
        if (Math.abs(b) > Math.abs(v)) v = b;
        if (Math.abs(c) > Math.abs(v)) v = c;
        return v;
    }

    private static float pick(float a, float b, float c, float d) { return pick(pick(a, b, c), d, 0); }

    // ------------------------------------------------------------------ motion and rumble
    // The GamePad's gyro (MotionInput) and rumble (Rumble) follow the controller in use, or this
    // device while playing by touch.
    private MotionInput motion;
    private Rumble rumble;
    private InputDevice lastController;  // the controller in use, null while playing by touch

    // where they come from (Controls > Motion controls / Rumble, offered while a controller and this device both
    // have them): automatic (the input in use, as above), this device, a controller (the one in use, else the first
    // connected that has them; none: this device), both (rumble only)
    static final int SRC_AUTO = 0, SRC_DEVICE = 1, SRC_CONTROLLER = 2, SRC_BOTH = 3;

    private void startMotion() {
        if (motion == null) motion = new MotionInput(this);
        if (prefs.getBoolean("motion", true)) motion.start(motionController());
        else motion.stop();
    }

    /** the controller whose sensors aim, or null: this device's */
    private InputDevice motionController() {
        switch (prefs.getInt("motion_from", SRC_AUTO)) {
            case SRC_DEVICE: return null;
            case SRC_CONTROLLER: return lastController != null && MotionInput.hasMotion(lastController) ? lastController : firstWith(true);
            default: return lastController;
        }
    }

    /** the controller that rumbles (with this device too for SRC_BOTH), or null: this device */
    private InputDevice rumbleController() {
        int src = prefs.getInt("rumble_on", SRC_AUTO);
        if (src == SRC_DEVICE) return null;
        if (src == SRC_AUTO) return lastController;
        return lastController != null && ControllerInfo.hasRumble(lastController) ? lastController : firstWith(false);
    }

    /** the first connected controller with motion sensors (motion) or rumble, or null */
    private static InputDevice firstWith(boolean motion) {
        for (InputDevice d : ControllerInfo.connected(null))
            if (motion ? MotionInput.hasMotion(d) : ControllerInfo.hasRumble(d)) return d;
        return null;
    }

    /** a connected controller and this device both have motion sensors (motion) or rumble: the source is a choice */
    boolean sourceChoice(boolean motion) {
        if (firstWith(motion) == null) return false;
        if (!motion) return Rumble.deviceVibrator(this) != null;
        android.hardware.SensorManager m = (android.hardware.SensorManager) getSystemService(SENSOR_SERVICE);
        return m.getDefaultSensor(android.hardware.Sensor.TYPE_GYROSCOPE) != null && m.getDefaultSensor(android.hardware.Sensor.TYPE_ACCELEROMETER) != null;
    }

    void setMotionFrom(int src) {
        prefs.edit().putInt("motion_from", src).apply();
        if (started) startMotion();
    }

    void setRumbleOn(int src) {
        prefs.edit().putInt("rumble_on", src).apply();
        rumbler().setController(rumbleController(), src == SRC_BOTH);
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
            rumble.setController(rumbleController(), prefs.getInt("rumble_on", SRC_AUTO) == SRC_BOTH);
        }
        return rumble;
    }

    // the input source changed: a controller (or touch, null)
    private void inputSource(InputDevice d) {
        if (d == lastController || (d != null && lastController != null && d.getId() == lastController.getId())) return;
        if (debuggable()) Log.d(TAG, "input source: " + (d == null ? "touch" : d.getName() + " (motion " + MotionInput.hasMotion(d) + ")"));
        lastController = d;
        if (d != null) usePadMap(d);
        if (controls != null) controls.setPadPlay(d != null);
        rumbler().setController(rumbleController(), prefs.getInt("rumble_on", SRC_AUTO) == SRC_BOTH);
        if (started) startMotion();
    }

    private long lastMotionRecheck;

    private void controllerUsed(InputDevice d) {
        if (d != null && InputMapper.isController(d)) {
            inputSource(d);
            maybeOfferSetup(d);
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

    /** Controls > On-screen controls: off, only the menu button stays (a touch doesn't bring them back) */
    void setOnscreenControls(boolean on) {
        prefs.edit().putBoolean("onscreen_controls", on).apply();
        autoHidden = false;
        applyControlsAppearance();
    }

    private void controllerUsed() {
        if (!autoHidden && prefs.getBoolean("auto_hide", true) && controls != null && controls.controlsVisible()) {
            autoHidden = true;
            applyControlsAppearance();
        }
    }

    @Override
    public void onControlsChanged() { pushInput(); }

    /** + : back to the game; where the GamePad picture shows the game's Options or Save (its own cursor), those
     * close first (B, again until they're gone), else the game picture would show the game's "Pause" */
    private void plusToggle() {
        if (!drcScreen() || !Native.drcGameCursor()) { onScreenToggle(); return; }
        closeMenusThenToggle(6);
    }

    private void closeMenusThenToggle(int tries) {
        if (!drcScreen()) return;
        if (!Native.drcGameCursor() || tries == 0) { onScreenToggle(); return; }
        controls.pulse(Native.B);
        controls.postDelayed(() -> closeMenusThenToggle(tries - 1), 400);
    }

    @Override
    public void onScreenToggle() {
        drcShown = !drcShown;
        drcForFiles = false;
        updateLayout();
        if (controls != null) {
            controls.removeCallbacks(prepareOptionsPage);
            if (drcShown) controls.postDelayed(prepareOptionsPage, 400);
            else if (pageReady) { optionsPage = null; pageReady = false; }  // (never shown: nothing to close)
        }
        pushInput();  // the switch let go of the touch +: else it still counts as held and the next + is lost
    }

    /** the on-screen controls' layout editor (ControlsView); the menu closes for it */
    void editControls() {
        if (controls != null) controls.setEditing(true);
    }

    /** the editor on the eyes-free controller view's own layout (Controls > Touch) */
    void editBlindControls() {
        if (controls != null) controls.setEditing(true, true);
    }

    // Classic and Flexible each keep their own layout; Flexible starts from the layout both shared before
    private String controlsLayoutKey() {
        return NEW_TOUCH_CONTROLS && prefs.getInt("controls_style", 0) == 1 ? "controls_layout_flex" : "controls_layout";
    }

    private String controlsLayout() {
        String k = controlsLayoutKey();
        return prefs.getString(k, k.equals("controls_layout") ? "" : prefs.getString("controls_layout", ""));
    }

    @Override
    public void onLayoutEdited(String layout, float opacity, boolean filled, boolean blindLayout) {
        prefs.edit().putString(blindLayout ? "controls_layout_blind" : controlsLayoutKey(), layout).putFloat("controls_opacity", opacity).putBoolean("controls_filled", filled).apply();
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

    /** the overlay's settings lines: ambient occlusion and the optional effects (the resolution and the driver are in ControlsView) */
    java.util.List<String> perfSettingsLines() {
        java.util.List<String> l = new java.util.ArrayList<>();
        String[] ao = {"Wii U", "centre fix", "centre+noise"};
        int aoMode = Math.max(0, Math.min(2, Native.getOption("ao_mode")));
        l.add("AO: " + ao[aoMode]);
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
        // hidden because a controller is in use: touching the screen brings the controls back (not on the GamePad
        // view with the game elsewhere while a controller is connected: a phone in a controller clamp, its touches
        // are the GamePad screen's)
        if (autoHidden && e.getActionMasked() == MotionEvent.ACTION_DOWN && !(screenRemote() && controllerConnected())) {
            autoHidden = false;
            applyControlsAppearance();
        }
        if (e.getActionMasked() == MotionEvent.ACTION_DOWN && remotePlay) {  // playing by touch (a remote controller's too)
            remotePlay = false;
            if (controls != null && lastController == null) controls.setPadPlay(false);
        }
        if (e.getActionMasked() == MotionEvent.ACTION_DOWN && lastController != null) inputSource(null);  // playing by touch
        return super.dispatchTouchEvent(e);
    }

    @Override
    public boolean dispatchKeyEvent(KeyEvent e) {
        if (!started) {  // the start page: a controller's A / B as in the menus; its back button does nothing (it left the app)
            KeyEvent k = prefs == null ? e : InputMapper.pageKey(prefs, e, false);
            return k == null || super.dispatchKeyEvent(k);
        }
        int code = e.getKeyCode();
        boolean controller = InputMapper.isController(e.getDevice()) || KeyEvent.isGamepadButton(code);
        if (code == KeyEvent.KEYCODE_BACK && !controller) {  // the menu's shortcut, in the + menu a step back instead
            if (e.getAction() == KeyEvent.ACTION_UP) menuOrBack();
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
            if (e.getAction() == KeyEvent.ACTION_UP) menuOrBack();
            return true;
        }
        if (mapper.onKey(e)) {
            if (controller) controllerUsed(e.getDevice());
            pushInput();
            return true;
        }
        return super.dispatchKeyEvent(e);
    }

    // A controller's Select (View): held for 0.6 s it opens the menu, a shorter press is the Wii U button
    // assigned to it (sent on release, held for a few frames so the game sees it); the − (dropped in pushInput)
    // steps the mini map's size instead
    private static final long SELECT_HOLD_MS = 600;
    private final android.os.Handler keyHandler = new android.os.Handler(android.os.Looper.getMainLooper());
    private boolean selectDown, selectOpenedMenu;
    private final Runnable selectHeld = () -> {
        if (!selectDown) return;
        selectOpenedMenu = true;
        menuOrBack();
    };

    // the wind by a controller (Wind dial, pref minimap_wind_swipe): − (Select) held and the right stick tilted chooses the
    // way, shown on the mini map's wind dial (the stick's way on the screen, as a swipe from the dial; a banner arrow
    // without the dial); letting go of − sets it (setWind). The hold then opens no menu and steps no map size
    private boolean windPad;
    private int windPadAngle = Integer.MIN_VALUE;
    private static final String[] WIND_ARROWS = {"→", "↘", "↓", "↙", "←", "↖", "↑", "↗"};

    /** pushInput: true while − chooses the wind (the right stick is then the wind's alone) */
    private boolean windByPad(float rx, float ry) {
        // (− held here, or on a GamePad device's controller: padKey)
        boolean here = selectDown && !selectOpenedMenu && mapper.bitFor(KeyEvent.KEYCODE_BUTTON_SELECT) == Native.MINUS
                && prefs.getBoolean("minimap_wind", true);  // (the GamePad device shows the wind on its own dial)
        if ((!here && !padSelectDown) || !prefs.getBoolean("minimap_wind_swipe", true)) return false;
        if (!windPad) {
            if (Math.hypot(rx, ry) < 0.6f || !gameRunning()) return false;
            windPad = true;
            keyHandler.removeCallbacks(selectHeld);
            keyHandler.removeCallbacks(padSelectHeld);
        }
        if (Math.hypot(rx, ry) >= 0.6f) {
            int w = controls.miniMap.windAngle(rx, -ry);  // (stick +y is up; the screen's down)
            if (w != windPadAngle) {
                windPadAngle = w;
                controls.miniMap.setWindPreview(w);
                if (!controls.miniMap.windDialShown()) {  // no dial to see it on: the way as an arrow on the screen
                    double sx = rx, sy = -ry;
                    int k = (int) Math.round(Math.atan2(sy, sx) / (Math.PI / 4));
                    controls.showBanner("≋ " + WIND_ARROWS[(k + 8) % 8]);
                }
            }
        }
        return true;
    }

    /** the menu's shortcuts (back key, Select held, Home / Guide): the options menu; in the + menu a step back */
    private void menuOrBack() {
        if (drcScreen()) inventoryBack();
        else onMenu();
    }

    private void selectButton(KeyEvent e) {
        controllerUsed(e.getDevice());
        if (e.getAction() == KeyEvent.ACTION_DOWN) {
            if (e.getRepeatCount() > 0) return;
            selectDown = true;
            selectOpenedMenu = false;
            windPad = false;
            windPadAngle = Integer.MIN_VALUE;
            keyHandler.postDelayed(selectHeld, SELECT_HOLD_MS);
        } else if (e.getAction() == KeyEvent.ACTION_UP) {
            // the press must have been ours: holding Select also closes the menu, and that release
            // arrives here after the menu is gone
            boolean pressedHere = selectDown;
            selectDown = false;
            keyHandler.removeCallbacks(selectHeld);
            if (!pressedHere || selectOpenedMenu) return;
            int bits = mapper.bitFor(KeyEvent.KEYCODE_BUTTON_SELECT);  // the − unless assigned otherwise
            if (windPad) {  // − held with the right stick: the wind chosen
                windPad = false;
                int w = windPadAngle;
                windPadAngle = Integer.MIN_VALUE;
                if (controls != null) controls.miniMap.setWindPreview(Integer.MIN_VALUE);
                if (w != Integer.MIN_VALUE) setWind(w);
                return;
            }
            // the − (never sent to the game): the mini map's next size while it shows, as a tap on it
            if (bits == Native.MINUS) {
                if (controls != null && controls.miniMap.onScreen())
                    prefs.edit().putInt("minimap_size", controls.miniMap.nextSize()).apply();
                return;
            }
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
        // a mouse's wheel (also a touchpad's two-finger scroll): Mods > First person on the mouse wheel (camera.cpp)
        if (started && e.isFromSource(android.view.InputDevice.SOURCE_MOUSE) && e.getActionMasked() == MotionEvent.ACTION_SCROLL) {
            float v = e.getAxisValue(MotionEvent.AXIS_VSCROLL);
            if (v != 0) Native.mouseWheel(v);
            return true;
        }
        if (started && mapper.onMotion(e)) {
            if (mapper.motionActive) controllerUsed(e.getDevice());
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
        if (itemRing != null) itemRing.cancel();  // a paused game can't save
        if (songs != null) songs.pause();
        // Android may end the app any time once it's in the background, without a word: the moment it
        // leaves is the last chance for a state (taken now while the game still runs, written after)
        if (started && !benchRunning && tourSlot == 0 && prefs.getBoolean("leave_state", false)) Native.saveStateOnLeave();
        if (benchRunning) Benchmark.interrupted();
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
        if (controls != null) controls.resumed();
        if (songs == null) songs = new SongListener(this);
        songs.resume();
        hideSystemBars();
    }

    @Override
    protected void onDestroy() {
        if (padHost != null) padHost.stop();
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
        savePadMap();
    }

    /** Jump anywhere's / the moon jump's controller button (InputMapper.assignOwn) */
    void assignOwnButton(int ownIndex, int code, boolean remove) {
        mapper.assignOwn(ownIndex, code, remove);
        savePadMap();
    }

    /** L1 / d-pad down as item button `item` (WIIU index, -1: off; InputMapper.setExtraItem) */
    void setExtraItem(int code, int item) {
        mapper.setExtraItem(code, item);
        savePadMap();
        pushInput();
    }

    /** Jump anywhere / the moon jump onto `code`, its item button onto L1 / d-pad down `to` */
    void assignOwnMovingItem(int ownIndex, int code, int to) {
        mapper.assignOwnMovingItem(ownIndex, code, to);
        savePadMap();
        pushInput();
    }

    // Each controller keeps its own assignment (pad_map@<InputDevice descriptor>, its name at pad_name@...); one
    // without starts from pad_map (the one assignment of the builds before, else the default), as does editing with
    // no controller connected. The map in use is the controller's last used, else the first connected one's.
    private String padKey;  // the descriptor of the controller whose map is in use; null: pad_map

    // the map in use read again (updateLayout): the controller's own, it stays in use (it went back to pad_map here,
    // and the controller in use, unchanged, never loaded its own again: its changes were lost, issue #61).
    // (Mods > ... > Button of the test builds before, a Wii U button: forgotten, L1 again)
    private void loadButtonMap() {
        mapper.loadMap(padKey == null ? prefs.getString("pad_map", "") : prefs.getString("pad_map@" + padKey, prefs.getString("pad_map", "")));
        mapper.loadPrinted(padKey == null ? "" : prefs.getString("pad_letters@" + padKey, ""));
        mapper.loadPlaces(padKey == null ? "" : prefs.getString("pad_places@" + padKey, ""));
        if (prefs.contains("mod_jump_button") || prefs.contains("cheat_moon_button"))
            prefs.edit().remove("mod_jump_button").remove("cheat_moon_button").apply();
    }

    /** the assignment of controller d (null: none connected) in use */
    void usePadMap(InputDevice d) {
        String key = d != null ? d.getDescriptor() : null;
        if (java.util.Objects.equals(key, padKey)) return;
        padKey = key;
        mapper.loadMap(key == null ? prefs.getString("pad_map", "") : prefs.getString("pad_map@" + key, prefs.getString("pad_map", "")));
        mapper.loadPrinted(key == null ? "" : prefs.getString("pad_letters@" + key, ""));
        mapper.loadPlaces(key == null ? "" : prefs.getString("pad_places@" + key, ""));
        if (key != null) prefs.edit().putString("pad_name@" + key, d.getName()).apply();
        pushInput();
        applySlotLabels(controllerConnected());
    }

    /** the controller whose assignment is in use (its name for the Controller buttons page); null: none */
    String padName() { return padKey == null ? null : prefs.getString("pad_name@" + padKey, null); }

    void savePadMap() {
        prefs.edit().putString(padKey == null ? "pad_map" : "pad_map@" + padKey, mapper.mapString()).apply();
        applySlotLabels(controllerConnected());
    }

    void resetButtons() {
        mapper.loadMap("");
        savePadMap();
    }

    /** ControllerSetup's answer: the controller in use's printed letters (codes of A, B, X, Y; null: none / not known)
     *  and where they sit (InputMapper.SOUTH...; null: not known) */
    void setPrintedLetters(int[] p, int[] places) {
        mapper.printed = p;
        mapper.places = p != null ? places : null;
        if (padKey != null)
            prefs.edit().putString("pad_letters@" + padKey, InputMapper.printedString(p))
                    .putString("pad_places@" + padKey, InputMapper.printedString(mapper.places)).apply();
        applySlotLabels(controllerConnected());
    }

    /** ControllerSetup's choice: the face buttons by letters or position, a preset with Jump anywhere's button; a
     *  face-button preset switches Jump anywhere on */
    void applyPadPreset(boolean letters, int preset, int jump) {
        mapper.preset(letters, preset, jump);
        savePadMap();
        pushInput();
        if (preset != InputMapper.PRESET_ORIGINAL && !prefs.getBoolean("mod_jump", false)) setJumpAnywhere(true);
    }

    // a controller used for the first time (by its descriptor; pad_asked@... once set up or declined): the setup is
    // offered once per app start, in play or on the game's screens, not over the menu or another dialog
    private final java.util.Set<String> setupOffered = new java.util.HashSet<>();

    private void maybeOfferSetup(InputDevice d) {
        if (!started || ControllerSetup.active || !hasWindowFocus()) return;
        String k = d.getDescriptor();
        if (prefs.getBoolean("pad_asked@" + k, false) || !setupOffered.add(k)) return;
        new ControllerSetup(this, d, null).offer();
    }

    /** the controller in use while it is connected, else null (Controller details) */
    InputDevice controllerInUse() {
        return lastController != null && InputDevice.getDevice(lastController.getId()) != null ? lastController : null;
    }

    /** Controls > Set up controller: the controller in use, else the first connected; false: none */
    boolean startControllerSetup(Runnable done) {
        InputDevice d = lastController != null ? lastController : firstController();
        if (d == null) return false;
        new ControllerSetup(this, d, done).start();
        return true;
    }

    /** the item ring works (also with a controller): the d-pad's up / left / right may be taken off for a jump */
    /** Controls > Item ring: the Wind Waker's / the boat's own buttons kept while the ring is on */
    void applyRingButtons() {
        if (controls != null) controls.setRingButtons(prefs.getBoolean("ring_baton_button", false), prefs.getBoolean("ring_boat_buttons", false));
    }

    boolean itemRingOn() { return itemRing != null ? itemRing.enabled() : prefs.getBoolean("item_ring", true); }

    /** the game waits (each frame, while ControllerSetup shows) */
    void holdGame() { if (started) pauseForMenu(true); }

    /** inverted camera up/down: 0 off, 1 first person and aiming, 2 always, 3 third-person camera only */
    void setInvertCameraY(int mode) {
        prefs.edit().putInt("invert_camera_y_mode", mode).apply();
        Native.setOption("invert_camera_y", mode);
    }

    /** walking direction while the camera turns: 0 the game's, 1 kept when the player turns it, 2 kept while the stick is held */
    void setSteerMode(int mode) {
        prefs.edit().putInt("steer_mode", mode).apply();
        Native.setOption("steer_mode", mode);
    }

    void setBool(String key, boolean v) {
        Native.setOption(key, v ? 1 : 0);
        prefs.edit().putBoolean(key, v).apply();
    }

    @Override
    public void onMenu() {
        new OptionsMenu(this).show();
    }

    @Override
    public void onProgressTap() {
        OptionsMenu.openProgress = true;
        onMenu();
    }

    // ---- actions of the in-game menu (OptionsMenu)
    static final float[] CONTROL_SIZES = {0.75f, 0.9f, 1f, 1.15f, 1.3f};
    // swipe speed factors 0.25 to 4 in steps of 0.25, shown relative to the default 2: -175 to +200
    static final float[] SWIPE_SPEEDS = new float[16];
    static { for (int i = 0; i < SWIPE_SPEEDS.length; i++) SWIPE_SPEEDS[i] = (i + 1) * 0.25f; }
    static final float SWIPE_SPEED_DEFAULT = 2f;
    static final int[] SWIPE_BOAT = {100, 125, 150, 175, 200, 225, 250, 275, 300, 325, 350, 375, 400};  // percent

    // render targets switch to the new size as the game next draws into them
    void setResolution(int i) {
        if (RES_SCALES[i].equals(prefs.getString("res_scale", "1"))) return;
        prefs.edit().putString("res_scale", RES_SCALES[i]).commit();
        Native.setOption("res_scale", Math.round(Float.parseFloat(RES_SCALES[i]) * 100));
        renderScale = Float.parseFloat(RES_SCALES[i]);
        updateBufferSize();
        controls.resetPerfAverage();
    }

    // dynamic resolution's lowest scale in percent (0 = off): the Rendering resolution is its upper limit
    static final int[] DYN_RES_MINS = {0, 100, 75, 60, 50, 40};

    void setDynResMin(int i) {
        prefs.edit().putInt("dyn_res_min", DYN_RES_MINS[i]).apply();
        Native.setOption("dyn_res_min", DYN_RES_MINS[i]);
        controls.resetPerfAverage();
    }

    // 0 = 16:9 (original), 1 = the screen's shape
    void setRenderAspect(int i) {
        prefs.edit().putInt("render_aspect", i).apply();
        Native.setOption("render_aspect", i);
        updateLayout();  // (the GamePad picture of + follows the TV picture's shape)
    }

    void setDisplayRes(int i) {
        prefs.edit().putInt("display_res", i).apply();
        updateBufferSize();
    }

    // a TV box with little memory (Tegra X1, 3 GB) at 4K: the game's swapchain and the options' window
    // buffers at 3840x2160 cost ~250 MB more than at 1080p, and opening the options (+145 MB at once) got
    // the game killed (user 2026-11-02). While the app is in front the display runs a 1080p mode (same
    // refresh rate if there is one); the TV enlarges it, and Android goes back to its own mode on leaving.
    // Only this activity: as the screen of another device (PadActivity) the display keeps its own mode.
    private void displayMode1080() {
        android.view.Display d = getWindowManager().getDefaultDisplay();
        android.view.Display.Mode cur = d.getMode(), best = null;
        if (Math.min(cur.getPhysicalWidth(), cur.getPhysicalHeight()) <= 1080) return;
        for (android.view.Display.Mode m : d.getSupportedModes()) {
            if (Math.min(m.getPhysicalWidth(), m.getPhysicalHeight()) > 1080) continue;
            // the same shape as the display's own mode (a 4K TV also lists 2560x1080 ultrawide)
            float shape = (float) m.getPhysicalWidth() / m.getPhysicalHeight(), curShape = (float) cur.getPhysicalWidth() / cur.getPhysicalHeight();
            if (Math.abs(shape - curShape) > 0.02f * curShape) continue;
            if (best == null) {
                best = m;
                continue;
            }
            boolean sameHz = Math.abs(m.getRefreshRate() - cur.getRefreshRate()) < 0.5f;
            boolean bestSameHz = Math.abs(best.getRefreshRate() - cur.getRefreshRate()) < 0.5f;
            long px = (long) m.getPhysicalWidth() * m.getPhysicalHeight(), bestPx = (long) best.getPhysicalWidth() * best.getPhysicalHeight();
            if ((sameHz && !bestSameHz) || (sameHz == bestSameHz && (px > bestPx || (px == bestPx && m.getRefreshRate() > best.getRefreshRate()))))
                best = m;
        }
        if (best == null) return;
        android.view.WindowManager.LayoutParams lp = getWindow().getAttributes();
        lp.preferredDisplayModeId = best.getModeId();
        getWindow().setAttributes(lp);
        Log.i(TAG, "little memory: display mode " + best.getPhysicalWidth() + "x" + best.getPhysicalHeight() + " " + best.getRefreshRate()
                + " Hz instead of " + cur.getPhysicalWidth() + "x" + cur.getPhysicalHeight());
    }

    /** the surface buffer's size for the Display resolution setting (see bufScale) */
    void updateBufferSize() {
        if (surface == null || surfaceW <= 0 || surfaceH <= 0) return;
        float k = 1;
        String test = null;
        try {
            test = Os.getenv("WWHD_SURFACE_SCALE");
        } catch (Exception ignored) {
        }
        if (test != null) {
            try {
                k = Float.parseFloat(test);
            } catch (NumberFormatException ignored) {
            }
        } else if (prefs.getInt("display_res", DISPLAY_RES_AUTO) == DISPLAY_RES_AUTO && Native.getOption("upscale") != 2) {
            // 1.5x the 3D view's lines (it fills the height; wider screens add width only), at least the
            // game's own 720 lines: small screens keep their resolution (writing them is cheap). Not with
            // Upscaling "Pixel perfect": the system would enlarge the buffer filtered, blurring its blocks
            k = Math.max(720f * renderScale * 1.5f, 720f) / surfaceH;
            k = (float) Math.ceil(k * 8) / 8;          // eighths: a few sizes, not one per setting
        }
        // little memory: the game's picture at most 1080 lines (the screen enlarges it)
        if (test == null && GraphicsPresets.ramMb(this) < VERY_LOW_MEMORY_MB) k = Math.min(k, 1080f / Math.min(surfaceW, surfaceH));
        k = Math.max(0.25f, Math.min(1f, k));
        if (k == bufRequested) return;
        bufRequested = k;
        SurfaceHolder holder = surface.getHolder();
        if (k >= 1) holder.setSizeFromLayout();
        else holder.setFixedSize(Math.round(surfaceW * k / 2) * 2, Math.round(surfaceH * k / 2) * 2);
        Log.i(TAG, "display resolution: buffer " + (k >= 1 ? "full" : Math.round(surfaceW * k / 2) * 2 + "x" + Math.round(surfaceH * k / 2) * 2)
                + " for " + surfaceW + "x" + surfaceH);
    }

    void setMod(String key, boolean on) {
        prefs.edit().putBoolean(key, on).apply();
        Native.setOption(key, on ? 1 : 0);
        if (key.equals("mod_climb")) controls.setClimbHud(on);
    }

    // Mods > Jump anywhere and Mods > Moon jump: their own inputs (Native.JUMP / MOON), on touch their own buttons, on
    // a controller a button of their own (L1 by default; InputMapper OWN_JUMP / OWN_MOON). Both on one button: the
    // moon jump wins, Jump anywhere rests (mods/jump.cpp). (The moon jump keeps its cheat_ pref and option names from
    // when it was among the cheats.)
    void setJumpAnywhere(boolean on) {
        setMod("mod_jump", on);
        if (controls != null) controls.invalidate();  // (its touch button)
    }

    void setMoonJump(boolean on) {
        setMod("cheat_moon_jump", on);
        if (controls != null) controls.invalidate();
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
                File bin = stateFile(playDir(), 8, "bin"), png = stateFile(playDir(), 8, "png");
                long now = android.os.SystemClock.uptimeMillis();
                if (Native.stateWritesPending() != 0 && now < until) { h.postDelayed(this, 200); return; }
                if (pictureBy == 0) pictureBy = now + 2000;
                if (png.lastModified() + 1000 < bin.lastModified() && now < pictureBy) h.postDelayed(this, 100);
                else exitNow();
            }
        }, 200);
    }

    private Runnable onExit;  // (becomePad: the GamePad screen opens as the game ends)

    private void exitNow() {
        if (onExit != null) onExit.run();
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
                     CHEAT_KEY = 64, CHEAT_BAGS = 128, CHEAT_FIGURINES = 256;
    static final String[] CHEATS_INFINITE = {"cheat_inf_health", "cheat_inf_magic", "cheat_inf_ammo", "cheat_moon_jump", "cheat_moon_nofall", "cheat_moon_anims",
                                             "cheat_inf_rupees", "cheat_inf_bags", "cheat_inf_breath", "cheat_one_hit",
                                             "cheat_fast_push"};

    /** a one-shot cheat (label: its menu line, named on the achievements page while they are paused) */
    void cheat(int label, int bit, int done) {
        Achievements.pauseRun(getString(label));
        Native.setOption("cheat", bit);
        android.widget.Toast.makeText(this, getString(done) + " " + getString(R.string.cheat_keep), android.widget.Toast.LENGTH_LONG).show();
    }
    // the gallery's figurines only for a member (event 0x3401 in the save's events at +0x624, as cheats.cpp checks):
    // a complete gallery sends Carlov away, and membership could never be had afterwards
    void cheatFigurines() {
        byte[] save = Native.progressData();
        if (save != null && save.length > 0x624 + 0x34 && (save[0x624 + 0x34] & 1) == 0) {
            android.widget.Toast.makeText(this, R.string.cheat_figurines_member, android.widget.Toast.LENGTH_LONG).show();
            return;
        }
        cheat(R.string.cheat_figurines, CHEAT_FIGURINES, R.string.cheat_figurines_done);
    }

    static final String[] MODS = {"mod_direct_camera", "mod_first_person", "mod_climb", "mod_quick_doors", "mod_fast_scenes",
                                  "mod_ff_cutscenes", "mod_ff_dialogues", "mod_song_replay", "mod_picto_pages", "mod_leaf_lift",
                                  "mod_walk_doors", "mod_instant_text"};
    // Game > Hit freezes: the game's short stops on hits, by kind (mods::HitStop bits: hits, strong hits, defeats, bosses,
    // signposts), the kinds switched off ("hit_stop_off"; an earlier build's single switch "mod_no_hit_stop": all of them)
    static final int HIT_STOP_KINDS = 5;
    int hitStopOff() {
        return prefs.getInt("hit_stop_off", prefs.getBoolean("mod_no_hit_stop", false) ? (1 << HIT_STOP_KINDS) - 1 : 0);
    }
    void setHitStopOff(int mask) {
        prefs.edit().putInt("hit_stop_off", mask).remove("mod_no_hit_stop").apply();
        Native.setOption("mod_hit_stop_off", mask);
    }

    static final int[] CAMERA_SPEEDS = {50, 100, 150, 200};
    static final int[] RUN_SPEEDS = {100, 125, 150, 200, 250, 300, 400};  // 100: off
    static final int[] SWING_DELAYS = {500, 750, 1000, 1250, 1500, 1750, 2000, 2250, 2500, 2750, 3000};  // camera swing wait, ms
    static final int[] MOON_RISE_SPEEDS = {50, 75, 100, 150, 200, 300};  // moon jump rise, percent
    static final int[] LEAF_LIFT_HEIGHTS = {25, 50, 75, 100, 150, 200, 300, 400};  // a leaf wave's lift, percent
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

    /** Native.JUMP / MOON while Jump anywhere / the moon jump is on: their touch buttons show */
    int jumpInputsInUse() {
        return (prefs.getBoolean("mod_jump", false) ? Native.JUMP : 0) | (prefs.getBoolean("cheat_moon_jump", false) ? Native.MOON : 0);
    }

    /** fast forward (ZR) is on for this event mode (mods/turbo.cpp: 1 a dialogue, 2 / 3 a cutscene) */
    boolean ffInUse(int eventMode) {
        if (eventMode == 1) return prefs.getBoolean("mod_ff_dialogues", false);
        return (eventMode == 2 || eventMode == 3) && prefs.getBoolean("mod_ff_cutscenes", false);
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
        startPicker(i, PICK_DISC_FILES);
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
        TextView t = cardText();
        t.setText(extract ? R.string.extract_running : R.string.compile_running);
        box.addView(t);
        android.widget.ProgressBar bar = new android.widget.ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal);
        bar.setMax(1000);
        TextView detail = cardText();
        TextView[] cancel = new TextView[1];
        cancel[0] = GameUi.barButton(this, getString(android.R.string.cancel), v -> {  // a compile's parts in progress still finish (up to half a minute)
            WorkService.Work.markCancelled();
            if (extract) Native.extractCancel();
            else Native.compileCancel();
            cancel[0].setEnabled(false);
            cancel[0].setAlpha(0.5f);
            cancel[0].setText(R.string.compile_stopping);
        }, false, true);
        setContentView(seaScreen(box, workFooter(bar, detail, cancel[0])));
        final SeaView mySea = sea;
        mySea.setFrameInterval(66);  // the CPU is for the work (SeaView)
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
                } else if (p[1] > 0) {
                    detail.setText(getString(R.string.compile_detail, p[0], p[1], s / 60, s % 60));
                } else {  // (the first seconds, before the recompiler knows its parts: no "0 of 0")
                    detail.setText(getString(R.string.compile_detail_split, s / 60, s % 60));
                }
                h.postDelayed(this, 500);
            }
        });
    }

    /** A job of WorkService.Work finished (called on the main thread, possibly in a later activity). */
    void workFinished(int kind, String err, boolean cancelled) {
        if (padCodeArrived && kind == WorkService.Work.COMPILE) {  // stopped for the code a phone made: it is ready now
            padCodeArrived = false;
            checkAndStart();
            return;
        }
        if (kind == WorkService.Work.EXTRACT) {
            if (err == null) checkAndStart();  // checks the extracted executable against this build
            else showSetup(getString(R.string.extract_failed, err));
        } else if (err == null && !cancelled) {
            checkAndStart();
        } else {
            showCompileStopped(err == null || err.equals("cancelled") ? getString(R.string.compile_cancelled) : getString(R.string.compile_failed, err));
        }
    }

    // ------------------------------------------------------------------ songs by microphone (SongListener)
    private SongListener songs;
    private boolean padLHeld;

    boolean gameRunning() { return libraryLoaded && started; }

    /** Mods > Sing the songs: on (while Link holds the Wind Waker) or off; asks for the microphone first */
    /** singing the songs on, after asking Android for the microphone where it isn't allowed yet (StarterQuestions) */
    void askSingSongs() {
        if (checkSelfPermission(android.Manifest.permission.RECORD_AUDIO) == android.content.pm.PackageManager.PERMISSION_GRANTED) {
            prefs.edit().putInt("sing_songs", SongListener.ON).apply();
            return;
        }
        prefs.edit().putInt("sing_songs_wanted", SongListener.ON).apply();
        requestPermissions(new String[] {android.Manifest.permission.RECORD_AUDIO}, SongListener.PERMISSION_REQUEST);
    }

    void setSingSongs(int mode) {
        if (mode != SongListener.OFF && songs != null && !songs.permitted()) {
            prefs.edit().putInt("sing_songs_wanted", mode).apply();
            requestPermissions(new String[] {android.Manifest.permission.RECORD_AUDIO}, SongListener.PERMISSION_REQUEST);
            return;
        }
        prefs.edit().putInt("sing_songs", mode).apply();
    }

    @Override
    public void onRequestPermissionsResult(int code, String[] perms, int[] results) {
        super.onRequestPermissionsResult(code, perms, results);
        if (code != SongListener.PERMISSION_REQUEST) return;
        boolean ok = results.length > 0 && results[0] == android.content.pm.PackageManager.PERMISSION_GRANTED;
        prefs.edit().putInt("sing_songs", ok ? prefs.getInt("sing_songs_wanted", SongListener.ON) : SongListener.OFF).apply();
        if (!ok) new GameDialog(this).title(R.string.opt_sing_songs).message(R.string.sing_songs_no_mic).button(android.R.string.ok, null).show();
    }

    /** a swipe from the mini map's wind dial: the wind that way at once, no event (Native.setWind; game angle y); a
     *  short buzz when it can't now (an event, the pause menu); any stage, the Wind's Requiem known or not */
    void setWind(int angle) {
        int[] song = gameRunning() ? Native.songState() : null;
        boolean ok = song != null && song[3] == 0;
        android.util.Log.i("wwhd", "wind swipe: " + angle + (ok ? "" : " (not now)"));
        if (ok) {
            Native.setWind(angle);
            if (controls != null) controls.performHapticFeedback(android.view.HapticFeedbackConstants.CONFIRM);
        } else if (controls != null) {
            controls.performHapticFeedback(android.view.HapticFeedbackConstants.REJECT);
        }
    }

    /** the wind dial on a GamePad device (PadHost): {on, way (as MiniMap's st[70]), the dial's turn}; on while the game runs
     *  with a wind (any stage) and Mini map > Wind arrow > Change the wind (dial) is on */
    float[] windForPad() {
        if (!gameRunning() || controls == null || !prefs.getBoolean("minimap_wind_swipe", true)) return new float[] {0, 0, 0};
        float[] s = Native.miniMapState();
        boolean on = s != null && s.length > 71 && s[0] != 0 && (s[70] != 0 || s[71] != 0);
        return new float[] {on ? 1 : 0, on ? s[70] : 0, on ? controls.miniMap.windTurn() : 0};
    }

    void showSongBanner(String text) { if (controls != null) controls.showBanner(text); }

    /** the learned songs to show play buttons for on the game's song list (while Link conducts), 0 none */
    void showSongList(int learned) { if (controls != null) controls.setSongList(learned); }

    /** the song a story scene waits for (a play button for it on the TV picture), -1 none */
    void showSongWanted(int melody) { if (controls != null) controls.setSongWanted(melody); }

    /** a play button on the song list tapped: back to the TV picture, Link plays the song */
    @Override
    public void onSongListPlay(int melody) {
        if (drcShown && !drcForFiles) onScreenToggle();
        if (songs != null) songs.playFromList(melody);
    }

    void showSongListening(boolean on, int notes, boolean button, boolean held, boolean conducting) {
        if (controls != null) controls.setSongListening(on, notes, button, held, conducting);
    }

    /** the hint's question "try it first?" answered yes: practice (SongListener) */
    void startSongPractice() { if (songs != null) songs.startPractice(); }

    /** the touch hint on singing, the first time it can be used (GameHint) */
    void offerSongHint(int hint) { if (controls != null) controls.offerHint(hint); }

    /** a question about singing (GameHint.ask): each time, hints on or off */
    void askSong(int question, String text) { if (controls != null) controls.askHint(question, text); }

    /** the mic button pressed / let go on the touch controls */
    @Override
    public void onSongMic(boolean down) { if (songs != null) songs.setHeld(true, down); }

    /** the time switch under the mic button tapped */
    @Override
    public void onSongBeat() { if (songs != null) songs.nextBeat(); }

    @Override
    public int songBeat() { return songs != null ? songs.beat() : 3; }

    @Override
    public SongListener.Take songTake() { return songs != null ? songs.take() : null; }

    @Override
    public int songCount() { return songs != null ? songs.countNow() : 0; }

    @Override
    public boolean songPractice() { return songs != null && songs.practicing(); }

    @Override
    public void onSongPracticeEnd() { if (songs != null) songs.stopPractice(); }

    // ---- the 3D view's zoom (Graphics > Field of view; a pinch on the picture): the field of view's
    // tangent in percent, 100 = the game's. While the singing panel shows, its own (view_zoom_song, a bit
    // wider at first: Link conducts close to the camera) and the picture moves down below the panel.
    static final int[] VIEW_ZOOMS = {100, 115, 130, 150, 175, 200};
    private static final int VIEW_ZOOM_SONG = 150;
    private int viewZoom = -1;  // the one in use now (a pinch changes it as the fingers move)
    private boolean songView;

    /** the game's own (Graphics > Field of view) */
    int viewZoomPercent() { return prefs.getInt("view_zoom", 100); }

    private String viewZoomKey() { return songView ? "view_zoom_song" : "view_zoom"; }

    private int viewZoomNow() {
        if (viewZoom < 0) viewZoom = prefs.getInt(viewZoomKey(), songView ? VIEW_ZOOM_SONG : 100);
        return viewZoom;
    }

    void setViewZoom(int percent) {
        prefs.edit().putInt("view_zoom", percent).apply();
        if (!songView) {
            viewZoom = percent;
            Native.setOption("view_zoom", percent);
        }
    }

    /** Controls > Touch > Pinch to zoom switched off (issue #71): what pinches set goes back to the defaults
     *  (the game's view, the conducting view's own) */
    void resetViewZoom() {
        prefs.edit().putInt("view_zoom", 100).remove("view_zoom_song").apply();
        viewZoom = -1;
        Native.setOption("view_zoom", viewZoomNow());
    }

    @Override
    public float viewZoom() { return viewZoomNow() / 100f; }

    @Override
    public void onViewZoom(float z, boolean done) {
        int p = Math.round(z * 100);
        if (done) prefs.edit().putInt(viewZoomKey(), p).apply();
        if (p != viewZoom) {
            viewZoom = p;
            Native.setOption("view_zoom", p);
        }
    }

    @Override
    public void onSongView(boolean panel) {
        if (panel == songView) return;
        songView = panel;
        viewZoom = -1;
        Native.setOption("view_zoom", viewZoomNow());
        Native.setOption("view_shift", panel ? Math.round(SongPanel.VIEW_SHIFT) : 0);
    }

    /** Mods > Sing the songs > Mic button: hold it while singing (else the microphone listens by itself) */
    void setSingButton(boolean on) { prefs.edit().putBoolean("sing_button", on).apply(); }

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
    File codeDir() { return new File(getNoBackupFilesDir(), "codecache"); }

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
        d.button(R.string.game_log_share, () -> CrashLogs.askGameLog(this, prefs, Native.gameRelease(gameDir())));
        d.button(R.string.opt_ok, null).show();
    }

    // the game as it is or one of the randomized games (Randomizer.games), before the game code check: each has its own
    // code, files, save and states. The one played last has the focus (a controller's A takes it). false: none made
    private boolean askGame() {
        java.util.List<Integer> games = Randomizer.games(this);
        if (games.isEmpty()) return false;
        LinearLayout box = new LinearLayout(this);
        box.setOrientation(LinearLayout.VERTICAL);
        int pad = (int) (24 * getResources().getDisplayMetrics().density);
        box.setPadding(pad, pad, pad, pad);
        TextView t = cardText();
        t.setText(R.string.rando_pick_title);
        box.addView(t);
        int last = Randomizer.chosen(this);
        View focus = null;
        for (int i = -1; i < games.size(); i++) {
            final int n = i < 0 ? 0 : games.get(i);
            TextView b = cardButton(n == 0 ? getString(R.string.rando_pick_plain) : Randomizer.name(this, n), v -> {
                Randomizer.choose(this, n);
                checkAndStart();
            });
            box.addView(b, cardButtonLp());
            if (n == last || focus == null) focus = b;
        }
        setContentView(seaScreen(box));
        focus.requestFocus();
        return true;
    }

    // Game > Randomizer asked for a new randomized game: made here, before the game code check (Randomizer.generate)
    private void runRandomizer() {
        LinearLayout box = new LinearLayout(this);
        box.setOrientation(LinearLayout.VERTICAL);
        int pad = (int) (24 * getResources().getDisplayMetrics().density);
        box.setPadding(pad, pad, pad, pad);
        TextView t = cardText();
        t.setText(R.string.rando_running);
        box.addView(t);
        android.widget.ProgressBar bar = new android.widget.ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal);
        bar.setMax(100);
        box.addView(bar);
        TextView detail = cardText();
        box.addView(detail);
        setContentView(seaScreen(box));
        final SeaView mySea = sea;
        mySea.setFrameInterval(66);
        final boolean[] done = {false};
        final android.os.Handler h = new android.os.Handler(android.os.Looper.getMainLooper());
        h.post(new Runnable() {
            @Override
            public void run() {
                if (done[0]) return;
                String[] p = Randomizer.progress(MainActivity.this);
                if (p != null) {
                    int pc = 0;
                    try {
                        pc = Integer.parseInt(p[0]);
                    } catch (NumberFormatException ignored) {
                    }
                    bar.setProgress(pc);
                    mySea.setProgress(pc / 100f);
                    detail.setText(p[1]);
                }
                h.postDelayed(this, 250);
            }
        });
        new Thread(() -> {
            String err;
            try {
                err = Randomizer.generate(this);
            } catch (RuntimeException e) {
                err = String.valueOf(e.getMessage());
            }
            final String result = err;
            h.post(() -> {
                done[0] = true;
                prefs.edit().putBoolean(Randomizer.PREF_PENDING, false).commit();  // (made: Randomizer.generate chose it)
                if (result == null) {
                    // the randomized game runs in a new process (the randomizer ran once in this one; Randomizer.playing)
                    restartApp();
                    return;
                }
                LinearLayout b = new LinearLayout(this);
                b.setOrientation(LinearLayout.VERTICAL);
                b.setPadding(pad, pad, pad, pad);
                TextView m = cardText();
                m.setText(getString(R.string.rando_failed, result));
                b.addView(m);
                b.addView(cardButton(getText(R.string.rando_play_plain), v -> {
                    Randomizer.choose(this, 0);
                    restartApp();
                }), cardButtonLp());
                setContentView(seaScreen(b));
            });
        }, "randomizer").start();
    }

    private void showCompileStopped(String message) {
        LinearLayout box = new LinearLayout(this);
        box.setOrientation(LinearLayout.VERTICAL);
        int pad = (int) (24 * getResources().getDisplayMetrics().density);
        box.setPadding(pad, pad, pad, pad);
        TextView t = cardText();
        t.setText(message);
        box.addView(t);
        box.addView(cardButton(getText(R.string.compile_continue), v -> checkAndStart()), cardButtonLp());
        setContentView(seaScreen(box));
    }

    // ------------------------------------------------------------------ frame generation
    // Lossless Scaling's frame generation (LSFG 3), with the shaders from the user's own copy of
    // Lossless.dll, which the file picker copies into the app's private storage.
    private static final int PICK_DLL = 1;
    static final String[] FG_FLOW_SCALES = {"0.25", "0.5", "0.75", "1"};

    private File frameGenDll() { return new File(new File(getFilesDir(), "lsfg"), "Lossless.dll"); }

    // exists while the renderer has shown generated frames in this run (lsfg.cpp): read by the crash dialog
    static File frameGenMarker(android.content.Context c) { return new File(new File(c.getFilesDir(), "lsfg"), "in_use"); }

    private boolean frameGenOn() { return prefs.getBoolean("fg_enabled", false) && frameGenDll().exists() && frameGenAllowed(); }

    // Mali GPUs: frame generation can hang the GPU there; it runs only after the player accepted a
    // warning (asked each time it's switched on; before the library loads, the renderer checks it itself)
    private static boolean frameGenRiskyGpu() { return libraryLoaded && Native.frameGenRiskyGpu(); }

    private boolean frameGenAllowed() { return !frameGenRiskyGpu() || prefs.getBoolean("fg_risk_ok", false); }

    /** the setting as the menu shows it (on, and on these GPUs accepted) */
    boolean frameGenSwitchedOn() { return prefs.getBoolean("fg_enabled", false) && frameGenAllowed(); }

    /** switches frame generation on, on these GPUs after the warning; `after` runs either way */
    void enableFrameGen(Runnable after) {
        if (frameGenAllowed()) {
            setFrameGen("fg_enabled", true);
            if (after != null) after.run();
            return;
        }
        new GameDialog(this).title(R.string.fg_risk_title).message(getString(R.string.fg_risk_text, PerfStats.gpuName()))
                .button(R.string.fg_risk_keep_off, () -> {
                    setFrameGen("fg_enabled", false);
                    if (after != null) after.run();
                })
                .button(R.string.fg_risk_accept, () -> {
                    prefs.edit().putBoolean("fg_risk_ok", true).commit();
                    Native.setFrameGenRiskAccepted(true);
                    setFrameGen("fg_enabled", true);
                    if (after != null) after.run();
                }).show();
    }

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
        boolean generated = prefs.getBoolean("fps_generated", false);
        prefs.edit().putInt("interp_fps", fps).putBoolean("fps_generated", false).apply();
        if (generated) Native.setOption("fps_generated", 0);  // (LSFG comes back if it is on)
        Native.setOption("interp_fps", fps);
    }

    /** Graphics > Frame rate > 60 fps (generated): the game at 30, every other frame made on the GPU */
    void setFpsGenerated() {
        prefs.edit().putInt("interp_fps", 0).putBoolean("fps_generated", true).apply();
        Native.setOption("interp_fps", 0);
        Native.setOption("fps_generated", 1);
    }

    private void applyFrameGenSettings() {
        // the renderer generates frames only if the display can show them (also when switched on later)
        float hz = 60;
        for (android.view.Display.Mode m : getWindowManager().getDefaultDisplay().getSupportedModes())
            hz = Math.max(hz, m.getRefreshRate());
        setenv("WWHD_DISPLAY_HZ", String.valueOf(Math.round(hz)));
        setenv("WWHD_FG_MARKER", frameGenMarker(this).getAbsolutePath());
        setenv("WWHD_LSFG_RISK_OK", prefs.getBoolean("fg_risk_ok", false) ? "1" : "0");
        if (!frameGenOn() || Benchmark.pending(prefs)) return;  // (the benchmark switches it on itself)
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
        if (!frameGenSwitchedOn()) return getString(R.string.off);
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
        // a graphics preset stays the same step, with or without frame generation
        int preset = key.equals("fg_enabled") ? GraphicsPresets.current(this) : GraphicsPresets.CUSTOM;
        prefs.edit().putBoolean(key, v).commit();
        if (preset != GraphicsPresets.CUSTOM && preset != GraphicsPresets.current(this)) GraphicsPresets.apply(this, preset);
        if (key.equals("fg_enabled") && !v) {  // the warning comes again next time
            prefs.edit().remove("fg_risk_ok").commit();
            Native.setFrameGenRiskAccepted(false);
        }
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
        startPicker(i, PICK_DLL);
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
                    startPicker(i, PICK_EXPORT);
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
        startPicker(i, PICK_PACKS);
    }

    private void importPacks(List<android.net.Uri> uris) {
        withProgressLines(getString(R.string.packs_importing), progress -> {
            StringBuilder ok = new StringBuilder(), bad = new StringBuilder();
            for (android.net.Uri u : uris) {
                String name = u.getLastPathSegment();
                try (android.database.Cursor c = getContentResolver().query(u, new String[] {android.provider.OpenableColumns.DISPLAY_NAME}, null, null, null)) {
                    if (c != null && c.moveToFirst() && c.getString(0) != null) name = c.getString(0);
                }
                try {
                    progress.at(name);
                    List<String> got = ModPacks.importArchive(this, baseDir(), gameDir(), getContentResolver(), u, name, progress);
                    // (many player models: their count, not a long list)
                    int models = 0;
                    for (String f : got) if (ModPacks.isPlayerModel(f)) models++;
                    if (models > 3) ok.append("\n• ").append(getString(R.string.packs_gc_imported_n, models));
                    for (String f : got) if (models <= 3 || !ModPacks.isPlayerModel(f)) ok.append("\n• ").append(f);
                    List<String> failed = ModPacks.lastFailures;
                    ModPacks.lastFailures = null;
                    if (failed != null) for (String f : failed) bad.append("\n• ").append(f);
                } catch (IOException | RuntimeException e) {
                    bad.append("\n• ").append(name).append(": ").append(e.getMessage());
                }
            }
            // (a save file's model imported again: the runtime reads it again)
            PlayerModels.handOver(this);
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

    // mod packs per save file (ModPacks.perFile): the file select handed over a save file whose packs differ from the
    // running ones (the runtime holds the game, packs.h file_service): the game restarts with that file's; at the file
    // select after the restart a note asks for that file again
    private boolean packSwitching;

    private void packFileSwitch(int front) {
        if (prefs == null || !started || packSwitching) return;  // (this process is ending: the next one goes on)
        autoStart(front);
        int file = Native.packFileSwitch();
        if (file <= 0) return;
        packSwitching = true;  // (the game waits for the answer)
        Log.i(TAG, "save file " + file + " has other mod packs: asking");
        new GameDialog(this).title(R.string.opt_packs).message(getString(R.string.packs_switch_ask, file)).cancelable(false)
                .button(R.string.opt_cancel, () -> {
                    Log.i(TAG, "save file " + file + ": not started, back to the file select");
                    Native.packFileBack();
                    if (prefs.getInt("packs_auto_file", 0) > 0) autoEnd(false);  // (the player's own choice from here)
                    packLoadingEnd();
                    packSwitching = false;
                })
                .button(R.string.opt_ok, () -> {
                    Log.i(TAG, "save file " + file + ": restarting with its mod packs");
                    ModPacks.makeActive(baseDir(), prefs, file);
                    prefs.edit().putInt("packs_auto_file", file).commit();
                    packLoading(file, 0.1f);
                    keyHandler.postDelayed(this::restartForPacks, 300);  // (the loading screen drawn first)
                }).show();
    }

    // The loading screen over the switch (the sea card, MainActivity.coverScreen): from the player's OK through the
    // restart, the title and the file select, until the save file's stage shows; the game's sound off meanwhile.
    // The boat sails across as the start goes on.
    private View packCover;
    private SeaView packSea;

    private void packLoading(int file, float progress) {
        if (packCover == null) {
            LinearLayout box = new LinearLayout(this);
            box.setOrientation(LinearLayout.VERTICAL);
            TextView t = new TextView(this);
            t.setText(getString(R.string.packs_loading, file));
            t.setTextSize(20);
            box.addView(t);
            packCover = coverScreen(box);
            packSea = coverSea();
            ((FrameLayout) getWindow().getDecorView()).addView(packCover, new FrameLayout.LayoutParams(-1, -1));
            Native.setOption("audio_mute", 1);
        }
        packCover.bringToFront();
        if (packSea != null) packSea.setProgress(progress);
    }

    private void packLoadingEnd() {
        if (packCover == null) return;
        ((FrameLayout) getWindow().getDecorView()).removeView(packCover);
        packCover = null;
        packSea = null;
        Native.setOption("audio_mute", 0);
    }

    // After that restart the app goes on into the save file by itself (pref packs_auto_file): A on the title (the
    // controller question answers itself, controller_selection.cpp), on the file select a tap on the file's box
    // (L_SaveData_0<N-1>, Native.menuTargets), then, once only that file shows above the four buttons, a tap on the
    // rightmost (L_BtnSeqence_03, Start). Each tap only when the screen is the one expected; another file opened: B
    // (back to the list). Not there within 40 s (or the player's own file / screen): the note to choose it again.
    private int autoButtons;
    private long autoSince, autoActed;
    private int autoSteady;
    private long autoFiles, autoTitle;
    private String autoSeen = "";

    private void autoStart(int front) {
        int file = prefs.getInt("packs_auto_file", 0);
        if (file <= 0) return;
        long now = android.os.SystemClock.uptimeMillis();
        if (autoSince == 0) {
            autoSince = now;
            Native.setOption("boot_skip", 1);  // (the title skipped: straight to the file select, mods/warp.cpp)
            Native.setOption("boot_file", file);  // (and from there into the file, as its Start does)
        }
        packLoading(file, front == Native.FRONT_BOOT ? 0.25f : front == Native.FRONT_TITLE ? 0.5f : 0.75f);
        if (front == Native.FRONT_OPENING || front == 0) {  // in: done (the file select's handing over checks the file)
            Log.i(TAG, "mod pack restart: save file " + file + " started");
            if (packSea != null) packSea.setProgress(1f);
            autoEnd(false);
            keyHandler.postDelayed(this::packLoadingEnd, 1800);  // (the stage's own fade in under it)
            return;
        }
        if (now - autoSince > 40000) {
            Log.i(TAG, "mod pack restart: save file " + file + " not reached by itself, the player chooses it");
            autoEnd(true);
            return;
        }
        if (front == Native.FRONT_TITLE) {
            // (the runtime skips the title; A only when it is still there after 4 s)
            if (autoTitle == 0) autoTitle = now;
            if (now - autoTitle > 4000 && now - autoActed > 1500) {
                autoActed = now;
                autoPress(Native.A);
            }
            return;
        }
        if (front != Native.FRONT_FILES) return;
        // (the runtime goes into the file by itself; the taps only when it hasn't within 3 s: a file without a game)
        if (autoFiles == 0) autoFiles = now;
        if (now - autoFiles < 3000 || now - autoActed < 1200) return;
        // the file select's boxes now: name -> {x, y, alpha} (parent "L_SaveData_01<N_SaveData_00|v1|a255")
        java.util.Map<String, float[]> box = new java.util.HashMap<>();
        for (String t : Native.menuTargets().split(";")) {
            String[] f = t.trim().split(" ");
            if (f.length != 6 || !f[0].equals("B_Btn_00")) continue;
            String[] p = f[1].split("\\|");
            if (p.length != 3 || !p[1].equals("v1")) continue;
            try {
                // (a bottom button's box sits in N_All_00 of its own part: named by that part, L_BtnSeqence_03 = Start)
                String[] path = p[0].split("<");
                String key = path[0].equals("N_All_00") && path.length > 1 ? path[1] : path[0];
                box.put(key, new float[] {Float.parseFloat(f[2]), Float.parseFloat(f[3]), Float.parseFloat(p[2].substring(1))});
            } catch (NumberFormatException ignored) {
            }
        }
        String mine = "L_SaveData_0" + (file - 1);
        float[] start = box.get("L_BtnSeqence_03"), own = box.get(mine);
        int files = 0, others = 0;
        for (java.util.Map.Entry<String, float[]> e : box.entrySet())
            if (e.getKey().startsWith("L_SaveData_0")) {
                files++;
                if (!e.getKey().equals(mine)) others++;
            }
        // (what it sees, each time it changes: names, place, alpha)
        StringBuilder seen = new StringBuilder();
        for (java.util.Map.Entry<String, float[]> e : new java.util.TreeMap<>(box).entrySet())
            seen.append(e.getKey()).append(String.format(java.util.Locale.ROOT, "@%.2f,%.2f/%d ", e.getValue()[0], e.getValue()[1], (int) e.getValue()[2]));
        if (!seen.toString().equals(autoSeen)) {
            autoSeen = seen.toString();
            Log.i(TAG, "mod pack restart: file select " + autoSeen);
        }
        // settled: the boxes acted on fully shown (fading in / out: wait), the same for two looks
        boolean settled = start != null ? start[2] >= 255 && (own == null || own[2] >= 255) : own != null && own[2] >= 255;
        if (start == null) for (java.util.Map.Entry<String, float[]> e : box.entrySet())
            if (e.getKey().startsWith("L_SaveData_0") && e.getValue()[2] < 255) settled = false;
        if (!settled || box.isEmpty()) { autoSteady = 0; return; }
        if (++autoSteady < 2) return;
        if (start != null) {  // a file opened: its box alone above the buttons
            if (files == 1 && own != null && own[1] < 0.4f) {
                Log.i(TAG, "mod pack restart: save file " + file + " opened, Start");
                drcTap(new float[] {start[0], start[1]});
                autoActed = now;
                autoSteady = 0;
            } else if (files == 1 && own == null) {
                Log.i(TAG, "mod pack restart: another file opened, back");
                autoActed = now;
                autoSteady = 0;
                autoPress(Native.B);
            }
            return;
        }
        if (files == 3 && own != null && others == 2 && Math.abs(own[0] - 0.5f) < 0.03f) {  // the list, settled
            Log.i(TAG, "mod pack restart: save file " + file + " tapped in the list");
            drcTap(new float[] {own[0], own[1]});
            autoActed = now;
            autoSteady = 0;
        }
    }

    private void autoPress(int button) {
        autoButtons = button;
        pushInput();
        keyHandler.postDelayed(() -> { autoButtons = 0; pushInput(); }, 150);
    }

    private void autoEnd(boolean note) {
        int file = prefs.getInt("packs_auto_file", 0);
        prefs.edit().remove("packs_auto_file").apply();
        autoButtons = 0;
        if (note) packLoadingEnd();
        if (note) new GameDialog(this).title(R.string.opt_packs).message(getString(R.string.packs_switched, file)).button(R.string.opt_ok, null).show();
    }

    void chooseImport() {
        new GameDialog(this).title(R.string.opt_import).message(R.string.backup_import_how)
                .button(R.string.opt_cancel, null)
                .button(R.string.backup_choose_files, () -> {
                    Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
                    i.addCategory(Intent.CATEGORY_OPENABLE);
                    i.setType("*/*");
                    i.putExtra(Intent.EXTRA_ALLOW_MULTIPLE, true);
                    startPicker(i, PICK_IMPORT_FILES);
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
    /** work on a thread of its own with a dialog showing `message`; work's progress lines replace it while it runs */
    void withProgressLines(String message, java.util.function.Function<ModPacks.Progress, String> work, java.util.function.Consumer<String> done) {
        GameDialog progress = new GameDialog(this).message(message).cancelable(false);
        progress.show();
        // (lines come quickly: the newest one is shown)
        final String[] latest = {null};
        final boolean[] posted = {false};
        ModPacks.Progress p = text -> {
            synchronized (latest) {
                latest[0] = text;
                if (posted[0]) return;
                posted[0] = true;
            }
            runOnUiThread(() -> {
                String t;
                synchronized (latest) {
                    t = latest[0];
                    posted[0] = false;
                }
                progress.setMessage(message + "\n\n" + t);
            });
        };
        new Thread(() -> {
            String r;
            try {
                r = work.apply(p);
            } catch (Exception e) {
                r = "!" + e.getMessage();
            }
            final String res = r;
            runOnUiThread(() -> {
                progress.dismiss();
                done.accept(res);
            });
        }, "progress").start();
    }

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
        if (request == LayoutPresets.PICK_EXPORT) {
            if (result == RESULT_OK && data != null && data.getData() != null) layoutPresets().exportTo(data.getData());
            else layoutPresets().show();
            return;
        }
        if (request == LayoutPresets.PICK_IMPORT) {
            List<android.net.Uri> uris = new ArrayList<>();
            if (result == RESULT_OK && data != null) {
                if (data.getClipData() != null)
                    for (int i = 0; i < data.getClipData().getItemCount(); i++) uris.add(data.getClipData().getItemAt(i).getUri());
                else if (data.getData() != null)
                    uris.add(data.getData());
            }
            if (!uris.isEmpty()) layoutPresets().importFrom(uris);
            else layoutPresets().show();
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
        if (request == PICK_DLL && afterDllPick != null && (result != RESULT_OK || data == null || data.getData() == null)) {
            Runnable r = afterDllPick;  // no file: back to the benchmark's offer
            afterDllPick = null;
            r.run();
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
            Runnable back = afterDllPick;  // (the benchmark's offer comes back after it)
            afterDllPick = null;
            new GameDialog(this).title(R.string.fg_title).message(getString(R.string.fg_bad_dll, problem))
                    .button(R.string.opt_ok, back).show();
            return;
        }
        //noinspection ResultOfMethodCallIgnored
        tmp.renameTo(frameGenDll());
        if (afterDllPick != null) {  // the benchmark's offer: back to it
            Runnable r = afterDllPick;
            afterDllPick = null;
            prefs.edit().putBoolean("fg_enabled", true).commit();
            r.run();
            return;
        }
        enableFrameGen(() -> {
            if (!frameGenOn()) return;  // (the warning for this GPU declined)
            new GameDialog(this).title(R.string.fg_title)
                    .message(Native.frameGenDllTested(frameGenDll().getAbsolutePath()) ? R.string.fg_dll_ok : R.string.fg_dll_untested)
                    .button(R.string.opt_ok, null).show();
        });
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
            if (padHost != null) padHost.textClosed();
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
        if (padHost != null)  // a GamePad device: its own field and keyboard (PadActivity.askText)
            padHost.askText(initial, maxLen, getString(R.string.text_message, maxLen), (ok, text) -> {
                if (ok) edit.setText(text);
                dlg.press(ok ? 1 : 0);
            });
    }
}
