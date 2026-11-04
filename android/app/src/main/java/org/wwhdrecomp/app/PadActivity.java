package org.wwhdrecomp.app;

import android.app.Activity;
import android.content.Intent;
import android.content.SharedPreferences;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.media.MediaCodec;
import android.media.MediaFormat;
import android.net.nsd.NsdManager;
import android.net.nsd.NsdServiceInfo;
import android.os.Bundle;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.Looper;
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
import android.widget.EditText;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import java.io.File;
import java.io.IOException;
import java.net.InetAddress;
import java.net.InetSocketAddress;
import java.net.Socket;
import java.util.ArrayList;
import java.util.List;

/**
 * This phone or tablet as the GamePad of a TV running the app (PadLink, PadHost on the TV). Runs in a
 * process of its own (":pad"): the game itself doesn't run here. Finds the TVs in the local network,
 * pairs with a code the TV shows, then shows the GamePad picture the TV sends and sends back the touch
 * buttons around it, touches on the picture, a controller's buttons and sticks and this device's motion.
 * Brings the game files to a TV without them, this device's saves to the TV when it joins and the TV's
 * back when it leaves (both switchable).
 */
public final class PadActivity extends Activity {
    private static final String TAG = "wwhd";
    static final String PREF_SAVES_TO = "pad_saves_to", PREF_SAVES_BACK = "pad_saves_back", PREF_TOUCH = "pad_touch_buttons",
            PREF_PICTURE = "pad_picture", PREF_SCREEN_PICTURE = "pad_screen_picture", PREF_SOUND = "pad_sound";

    private SharedPreferences prefs;
    static final String EXTRA_ROLE = "role";
    static final String EXTRA_PAGE = "page";  // the drawn GamePad screen's page (PadScreen.showPage), for tests
    static final String EXTRA_SEND = "send";  // tests (not in public releases): a message type without body to the console (51: Save)
    /** the start page's choice (MainActivity, Game > Other devices > Choose at the start): play here, the GamePad or
     *  the screen of another device */
    static final int ROLE_CHOOSE = 0;
    /** the screen-only app (module receiver): no game here at all, the screen of another device, joins one found */
    static final boolean RECEIVER = BuildConfig.RECEIVER;
    static final String EXTRA_AUTO = "auto_join";
    private boolean autoJoin;  // the first device found is joined (the screen-only app, the start page's "Wait for a device")
    private final java.util.Set<String> noAutoJoin = new java.util.HashSet<>();  // left or refused here: listed, not joined
    private SearchBoard board;
    private int role = PadLink.ROLE_PAD;  // the GamePad, or the screen (the TV picture and sound only)

    private boolean screenRole() { return role == PadLink.ROLE_SCREEN; }
    private final Handler main = new Handler(Looper.getMainLooper());
    private HandlerThread sendThread;
    private Handler send;
    private final InputMapper mapper = new InputMapper();
    private MotionInput motion;
    private PadLink.WifiAwake wifi;

    // ---- finding TVs
    private static final class Tv {
        final String name;
        final InetAddress host;
        final int port;

        Tv(String name, InetAddress host, int port) {
            this.name = name;
            this.host = host;
            this.port = port;
        }
    }

    private final List<Tv> found = new ArrayList<>();
    private NsdManager nsd;
    private NsdManager.DiscoveryListener discovery;
    private final List<NsdServiceInfo> toResolve = new ArrayList<>();
    private boolean resolving;
    private LinearLayout tvList;

    // ---- the connection
    private volatile PadLink.Conn conn;
    private Tv tv;
    private boolean ending;    // leaving: the saves come back, then the game here starts again
    private int reconnects;
    private boolean tvHasGame, tvRunning;
    private String tvGame = "";  // the TV's game (PadLink.gameId)
    private String tvFeatures = "", tvModel = "";  // its CPU: the game code can be compiled here for it
    private boolean tvNeedsCode;  // it has the game files but compiles its code (or will): this device can instead

    /** both have the same game: saves and states may go between them */
    private boolean sameGame() {
        String mine = PadLink.gameId(baseDir());
        return !mine.isEmpty() && mine.equals(tvGame);
    }

    // ---- the pad screen
    private FrameLayout padRoot;
    private SurfaceView picture;
    private Controls controls;
    private TextView status, noPicture;
    private android.widget.ImageView uiView;  // the console's dialog on top (PadUi there), over everything
    private UiDraw.Player uiDraw;  // the same drawn here from the console's drawing orders (PadLink.UI_DRAW)

    private UiDraw.Player uiDraw() {
        if (uiDraw == null) {
            uiDraw = new UiDraw.Player(this);
            uiDraw.setVisibility(View.GONE);
            uiDraw.setOnTouchListener((v, e) -> uiTouch(e));
        }
        if (padRoot != null && uiDraw.getParent() != padRoot) {
            if (uiDraw.getParent() != null) ((android.view.ViewGroup) uiDraw.getParent()).removeView(uiDraw);
            padRoot.addView(uiDraw, new FrameLayout.LayoutParams(-1, -1));
        }
        return uiDraw;
    }
    private volatile android.view.Surface pictureSurface;
    private MediaCodec decoder;
    private byte[] videoConfig;
    private int videoW, videoH;
    private boolean needKey = true;
    private final Object decoderLock = new Object();

    // the app's own texts in its menu language, as in the game (MainActivity)
    @Override
    protected void attachBaseContext(android.content.Context base) {
        android.content.res.Configuration c = new android.content.res.Configuration(base.getResources().getConfiguration());
        c.setLocale(new java.util.Locale(MainActivity.menuLanguage(base.getSharedPreferences("settings", MODE_PRIVATE))));
        super.attachBaseContext(base.createConfigurationContext(c));
    }

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        prefs = getSharedPreferences("settings", MODE_PRIVATE);
        int asked = getIntent().getIntExtra(EXTRA_ROLE, PadLink.ROLE_PAD);
        role = RECEIVER || asked == PadLink.ROLE_SCREEN ? PadLink.ROLE_SCREEN : PadLink.ROLE_PAD;
        autoJoin = RECEIVER || getIntent().getBooleanExtra(EXTRA_AUTO, false);
        mapper.loadMap(prefs.getString("pad_map", ""));
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        getWindow().getAttributes().layoutInDisplayCutoutMode = WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
        sendThread = new HandlerThread("wwhd-pad-send");
        sendThread.start();
        send = new Handler(sendThread.getLooper());
        nsd = getSystemService(NsdManager.class);
        wifi = new PadLink.WifiAwake(this);
        getSystemService(android.hardware.input.InputManager.class).registerInputDeviceListener(devices, main);
        if (!RECEIVER && asked == ROLE_CHOOSE) showChoice();
        else showSearch();
    }

    // ------------------------------------------------------------------ the start page (Choose at the start)
    private boolean choosing;

    private void showChoice() {
        choosing = true;
        LinearLayout box = new LinearLayout(this);
        box.setOrientation(LinearLayout.VERTICAL);
        box.setGravity(Gravity.CENTER);
        int pad = GameUi.px(this, 20);
        box.setPadding(pad, pad, pad, pad);
        TextView title = GameUi.text(this, true);
        title.setText(R.string.app_name);
        title.setTextSize(28);
        title.setTextColor(Color.WHITE);
        title.setGravity(Gravity.CENTER);
        box.addView(title);
        TextView text = GameUi.text(this, false);
        text.setText(R.string.choice_text);
        text.setTextColor(0xFFDDE8F0);
        text.setTextSize(15);
        text.setGravity(Gravity.CENTER);
        text.setPadding(0, GameUi.px(this, 6), 0, GameUi.px(this, 14));
        box.addView(text);
        LinearLayout cards = new LinearLayout(this);
        cards.setOrientation(LinearLayout.HORIZONTAL);
        cards.setGravity(Gravity.CENTER);
        boolean tvHere = MainActivity.isTv(this);
        View first = choiceCard(cards, ChoiceIcon.HERE, R.string.choice_here, R.string.choice_here_text, () -> {
            choosing = false;
            startGameHere();
        });
        if (!tvHere)
            choiceCard(cards, ChoiceIcon.PAD, R.string.choice_pad, R.string.choice_pad_text, () -> chose(PadLink.ROLE_PAD));
        choiceCard(cards, ChoiceIcon.SCREEN, R.string.choice_screen, R.string.choice_screen_text, () -> chose(PadLink.ROLE_SCREEN));
        box.addView(cards, new LinearLayout.LayoutParams(-1, -2));
        TextView later = GameUi.text(this, false);
        later.setText(R.string.choice_later);
        later.setTextColor(0xB0DDE8F0);
        later.setTextSize(13);
        later.setGravity(Gravity.CENTER);
        later.setPadding(0, GameUi.px(this, 14), 0, 0);
        box.addView(later);
        ScrollView sv = new ScrollView(this);
        sv.setFillViewport(true);
        box.setMinimumHeight(getResources().getDisplayMetrics().heightPixels);
        sv.addView(box);
        sv.setBackground(seaBackground());
        setContentView(sv);
        hideBars();
        first.requestFocus();
    }

    private void chose(int r) {
        choosing = false;
        role = r;
        autoJoin = r == PadLink.ROLE_SCREEN;  // (waiting for a device: the first one found is joined)
        showSearch();
    }

    /** the game here after all: the app's game process (MainActivity), told not to ask again */
    private void startGameHere() {
        Intent i = new Intent(this, MainActivity.class);
        i.putExtra(MainActivity.EXTRA_CHOSEN, true);
        i.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TASK);
        startActivity(i);
        finishAndRemoveTask();
        main.postDelayed(() -> System.exit(0), 200);
    }

    private enum ChoiceIcon { HERE, PAD, SCREEN }

    private View choiceCard(LinearLayout row, ChoiceIcon icon, int name, int text, Runnable go) {
        LinearLayout card = new LinearLayout(this);
        card.setOrientation(LinearLayout.VERTICAL);
        card.setGravity(Gravity.CENTER_HORIZONTAL);
        int pad = GameUi.px(this, 12);
        card.setPadding(pad, pad, pad, pad);
        card.setFocusable(true);
        card.setClickable(true);
        card.setOnClickListener(v -> go.run());
        android.graphics.drawable.GradientDrawable off = new android.graphics.drawable.GradientDrawable(
                android.graphics.drawable.GradientDrawable.Orientation.TOP_BOTTOM, new int[] {0xFFFFF8E4, 0xFFF0DDB0});
        off.setCornerRadius(GameUi.px(this, 14));
        off.setStroke(GameUi.px(this, 3), 0xFF9A6A3C);
        android.graphics.drawable.GradientDrawable on = new android.graphics.drawable.GradientDrawable(
                android.graphics.drawable.GradientDrawable.Orientation.TOP_BOTTOM, new int[] {0xFFFFF7B8, 0xFFF5DC4A});
        on.setCornerRadius(GameUi.px(this, 14));
        on.setStroke(GameUi.px(this, 4), 0xFFC9A200);
        android.graphics.drawable.StateListDrawable st = new android.graphics.drawable.StateListDrawable();
        st.addState(new int[] {android.R.attr.state_pressed}, on);
        st.addState(new int[] {android.R.attr.state_focused}, on);
        st.addState(new int[0], off);
        card.setBackground(st);
        View pic = new View(this) {
            private final Paint pp = new Paint(Paint.ANTI_ALIAS_FLAG);

            @Override
            protected void onDraw(Canvas c) { drawChoiceIcon(c, pp, icon, getWidth(), getHeight()); }
        };
        card.addView(pic, new LinearLayout.LayoutParams(GameUi.px(this, 120), GameUi.px(this, 78)));
        TextView n = GameUi.text(this, true);
        n.setText(name);
        n.setTextSize(19);
        n.setTextColor(0xFF3A2410);
        n.setGravity(Gravity.CENTER);
        n.setPadding(0, GameUi.px(this, 6), 0, GameUi.px(this, 2));
        card.addView(n);
        TextView t = GameUi.text(this, false);
        t.setText(text);
        t.setTextSize(13);
        t.setTextColor(0xFF5A4630);
        t.setGravity(Gravity.CENTER);
        card.addView(t);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(0, -1, 1);
        int m = GameUi.px(this, 8);
        lp.setMargins(m, 0, m, 0);
        row.addView(card, lp);
        return card;
    }

    /** the start page's pictures: a TV with the game, a GamePad, a TV showing what comes over the air */
    private static void drawChoiceIcon(Canvas c, Paint p, ChoiceIcon icon, float w, float h) {
        float cx = w / 2, cy = h / 2, u = Math.min(w / 12, h / 8);
        p.setStyle(Paint.Style.FILL);
        android.graphics.RectF r = new android.graphics.RectF();
        if (icon == ChoiceIcon.PAD) {  // a GamePad: a body, the screen, sticks and buttons
            r.set(cx - 5.4f * u, cy - 3f * u, cx + 5.4f * u, cy + 3f * u);
            p.setColor(0xFF3E4A56);
            c.drawRoundRect(r, 1.6f * u, 1.6f * u, p);
            r.set(cx - 2.8f * u, cy - 2.2f * u, cx + 2.8f * u, cy + 1.6f * u);
            p.setColor(0xFF6FD8FA);
            c.drawRoundRect(r, 0.3f * u, 0.3f * u, p);
            p.setColor(0xFFDDE4EA);
            c.drawCircle(cx - 4f * u, cy - 1.2f * u, 0.75f * u, p);
            c.drawCircle(cx + 4f * u, cy - 1.2f * u, 0.75f * u, p);
            p.setColor(0xFFE5604B);
            c.drawCircle(cx + 4.3f * u, cy + 1.2f * u, 0.4f * u, p);
            p.setColor(0xFF5DBA42);
            c.drawCircle(cx + 3.5f * u, cy + 1.8f * u, 0.4f * u, p);
            p.setColor(0xFFDDE4EA);
            c.drawRect(cx - 4.5f * u, cy + 1.3f * u, cx - 3.1f * u, cy + 1.7f * u, p);
            c.drawRect(cx - 4f * u, cy + 0.8f * u, cx - 3.6f * u, cy + 2.2f * u, p);
            return;
        }
        // a TV
        r.set(cx - 5f * u, cy - 3.4f * u, cx + 5f * u, cy + 2.6f * u);
        p.setColor(0xFF2A2A30);
        c.drawRoundRect(r, 0.6f * u, 0.6f * u, p);
        c.drawRect(cx - 0.4f * u, cy + 2.6f * u, cx + 0.4f * u, cy + 3.4f * u, p);
        c.drawRoundRect(cx - 2.4f * u, cy + 3.2f * u, cx + 2.4f * u, cy + 3.7f * u, 0.2f * u, 0.2f * u, p);
        r.set(cx - 4.4f * u, cy - 2.8f * u, cx + 4.4f * u, cy + 2f * u);
        if (icon == ChoiceIcon.HERE) {  // the sea and an island with a palm: the game here
            p.setColor(0xFF2E8BD0);
            c.drawRect(r, p);
            p.setColor(0xFF9FE0FF);
            c.drawRect(r.left, r.top, r.right, cy - 0.6f * u, p);
            p.setColor(0xFFF0D890);
            c.drawOval(cx - 2.2f * u, cy - 0.2f * u, cx + 2.2f * u, cy + 1f * u, p);
            p.setColor(0xFF2F7A22);
            c.drawOval(cx - 1.5f * u, cy - 0.7f * u, cx + 1.5f * u, cy + 0.6f * u, p);
            p.setColor(0xFFFFE070);
            c.drawCircle(cx + 2.8f * u, cy - 1.9f * u, 0.6f * u, p);
        } else {  // waves coming in: the picture from another device
            p.setColor(0xFF1A5C9E);
            c.drawRect(r, p);
            p.setStyle(Paint.Style.STROKE);
            p.setStrokeWidth(0.45f * u);
            p.setStrokeCap(Paint.Cap.ROUND);
            p.setColor(0xFFFFFFFF);
            for (int k = 1; k <= 3; k++) {
                float rad = k * 1.15f * u;
                android.graphics.RectF a = new android.graphics.RectF(cx - rad, cy + 1.2f * u - rad, cx + rad, cy + 1.2f * u + rad);
                c.drawArc(a, 225, 90, false, p);
            }
            p.setStyle(Paint.Style.FILL);
            c.drawCircle(cx, cy + 1.2f * u, 0.45f * u, p);
        }
    }

    /** the sea behind the start and search pages */
    private android.graphics.drawable.Drawable seaBackground() {
        return new android.graphics.drawable.GradientDrawable(android.graphics.drawable.GradientDrawable.Orientation.TOP_BOTTOM,
                new int[] {0xFF1B6FB8, 0xFF0F3F8C, 0xFF082A5E});
    }

    // a controller gone: the touch buttons come back (Controls.controllersChanged)
    private final android.hardware.input.InputManager.InputDeviceListener devices = new android.hardware.input.InputManager.InputDeviceListener() {
        @Override public void onInputDeviceAdded(int id) {}

        @Override public void onInputDeviceChanged(int id) { if (controls != null) controls.controllersChanged(); }

        @Override public void onInputDeviceRemoved(int id) { if (controls != null) controls.controllersChanged(); }
    };

    @Override
    protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        if (padScreen != null && intent.hasExtra(EXTRA_PAGE)) padScreen.showPage(intent.getIntExtra(EXTRA_PAGE, 0));
        if (!BuildConfig.PUBLIC_RELEASE && intent.hasExtra(EXTRA_SEND)) post((byte) intent.getIntExtra(EXTRA_SEND, 0), new byte[0]);
    }

    @Override
    protected void onDestroy() {
        PadService.stop(this);
        if (padRemote) {
            GameIcons.setRemote(null);
            GameFont.setRemote(null);
        }
        getSystemService(android.hardware.input.InputManager.class).unregisterInputDeviceListener(devices);
        stopDiscovery();
        PadLink.Conn c = conn;
        conn = null;
        if (c != null) c.close();
        if (motion != null) motion.stop();
        releaseDecoder();
        stopSound();
        wifi.set(false);
        sendThread.quitSafely();
        super.onDestroy();
    }

    private void hideBars() {
        WindowInsetsController c = getWindow().getInsetsController();
        if (c != null) {
            c.hide(WindowInsets.Type.systemBars());
            c.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
        }
    }

    // ------------------------------------------------------------------ finding a TV
    private void showSearch() {
        LinearLayout box = new LinearLayout(this);
        box.setOrientation(LinearLayout.VERTICAL);
        int pad = GameUi.px(this, 18);
        box.setPadding(pad, pad, pad, pad);
        TextView title = GameUi.text(this, true);
        title.setText(screenRole() ? R.string.pad_screen_title : R.string.pad_title);
        title.setTextSize(26);
        title.setTextColor(Color.WHITE);
        box.addView(title);
        TextView text = GameUi.text(this, false);
        text.setText(getString(R.string.pad_searching) + "\n" + getString(screenRole() ? R.string.pad_none_hint_screen : R.string.pad_none_hint));
        text.setTextColor(0xFFDDE8F0);
        text.setTextSize(15);
        text.setPadding(0, GameUi.px(this, 8), 0, GameUi.px(this, 8));
        box.addView(text);
        tvList = new LinearLayout(this);
        tvList.setOrientation(LinearLayout.VERTICAL);
        box.addView(tvList);
        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.VERTICAL);
        row.addView(GameUi.barButton(this, getString(R.string.pad_address), v -> askAddress()), buttonLp());
        row.addView(GameUi.barButton(this, getString(RECEIVER ? R.string.receiver_close : R.string.pad_back_to_game),
                v -> leaveSearchNow()), buttonLp());
        box.addView(row);
        ScrollView sv = new ScrollView(this);
        sv.addView(box);
        // the board on the left (a sea battle: misses while looking, a hit for each device found), the rest beside it
        LinearLayout page = new LinearLayout(this);
        page.setOrientation(LinearLayout.HORIZONTAL);
        board = new SearchBoard(this);
        page.addView(board, new LinearLayout.LayoutParams(0, -1, 1.05f));
        page.addView(sv, new LinearLayout.LayoutParams(0, -1, 1));
        page.setBackground(seaBackground());
        setContentView(page);
        hideBars();
        listFound();
        startDiscovery();
    }

    /** the search page's way out: the game here again, or (the screen-only app) closed */
    private void leaveSearchNow() {
        if (RECEIVER) finishAndRemoveTask();
        else backToGame();
    }

    /** back to looking (the screen-only app after a session ends: it never plays itself) */
    private void toSearch() {
        PadLink.Conn c = conn;
        conn = null;
        if (tv != null) noAutoJoin.add(tv.name);  // (left on purpose or lost: listed, joined again only by choice)
        tv = null;
        ending = false;
        reconnects = 0;
        if (c != null) send.post(c::close);
        PadService.stop(this);
        releaseDecoder();
        stopSound();
        wifi.set(false);
        controls = null;
        showSearch();
    }

    private LinearLayout.LayoutParams buttonLp() {
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(-2, GameUi.px(this, 52));
        lp.setMargins(0, GameUi.px(this, 10), GameUi.px(this, 12), 0);
        return lp;
    }

    private void listFound() {
        if (tvList == null) return;
        tvList.removeAllViews();
        List<String> names = new ArrayList<>();
        for (Tv t : found) {
            tvList.addView(GameUi.barButton(this, t.name, v -> connect(t), true), buttonLp());
            names.add(t.name);
        }
        if (board != null) board.setDevices(names);
        if (!found.isEmpty() && tvList.getChildCount() > 0 && getCurrentFocus() == null) tvList.getChildAt(0).requestFocus();
    }

    private static final long AUTO_JOIN_SHOWN_MS = 1200, AUTO_JOIN_MAX_MS = 6000;  // (the board's hits and the ship shown first)

    /** waiting for a device: the first one found is joined (not one left or refused here) */
    private void maybeAutoJoin(Tv t) {
        if (!autoJoin || noAutoJoin.contains(t.name)) return;
        joinSoon(t);
    }

    /** joined once the board has shown its ship a moment (still found then, nothing else joined meanwhile) */
    private void joinSoon(Tv t) {
        long until = android.os.SystemClock.uptimeMillis() + AUTO_JOIN_MAX_MS;
        main.postDelayed(new Runnable() {
            @Override
            public void run() {
                if (conn != null || tv != null || isFinishing() || !found.contains(t) || choosing) return;
                if (!started) return;  // (in the background: never takes a console's screen slot unseen; onStart looks again)
                SearchBoard b = board;
                if (b != null && b.shownFor(t.name) < AUTO_JOIN_SHOWN_MS && android.os.SystemClock.uptimeMillis() < until) {
                    main.postDelayed(this, 150);
                    return;
                }
                Log.i(TAG, "GamePad: joining " + t.name + " (found)");
                connect(t);
            }
        }, 300);
    }

    // a screen waits for a console's invitation too (Show on a TV there, ScreenFinder): then nothing is chosen here
    private ScreenFinder.Waiting waiting;

    /** a console invites this screen: joined with its one-time pass (no code), if nothing else is going on here */
    private boolean invited(String console, InetAddress host, int port, String ticket) {
        if (conn != null || tv != null || choosing || isFinishing() || !started) return false;
        Log.i(TAG, "GamePad: invited by " + console + ", joining");
        prefs.edit().putString("pad_token@" + console, ticket).apply();
        connect(new Tv(console, host, port));
        return true;
    }

    private void startDiscovery() {
        if (screenRole() && waiting == null) waiting = new ScreenFinder.Waiting(this, PadHost.deviceName(this), this::invited);
        if (discovery != null) return;
        discovery = new NsdManager.DiscoveryListener() {
            @Override public void onStartDiscoveryFailed(String type, int err) { Log.w(TAG, "GamePad: discovery failed " + err); }
            @Override public void onStopDiscoveryFailed(String type, int err) {}
            @Override public void onDiscoveryStarted(String type) {}
            @Override public void onDiscoveryStopped(String type) {}

            @Override
            public void onServiceFound(NsdServiceInfo info) {
                main.post(() -> {
                    toResolve.add(info);
                    resolveNext();
                });
            }

            @Override
            public void onServiceLost(NsdServiceInfo info) {
                main.post(() -> {
                    found.removeIf(t -> t.name.equals(info.getServiceName()));
                    listFound();
                });
            }
        };
        nsd.discoverServices(PadLink.SERVICE_TYPE, NsdManager.PROTOCOL_DNS_SD, discovery);
    }

    private void stopDiscovery() {
        if (waiting != null) {
            waiting.stop();
            waiting = null;
        }
        if (discovery == null) return;
        try {
            nsd.stopServiceDiscovery(discovery);
        } catch (IllegalArgumentException ignored) {
        }
        discovery = null;
    }

    // one at a time (older Androids refuse a second resolve while one runs)
    @SuppressWarnings("deprecation")
    private void resolveNext() {
        if (resolving || toResolve.isEmpty()) return;
        resolving = true;
        nsd.resolveService(toResolve.remove(0), new NsdManager.ResolveListener() {
            @Override
            public void onResolveFailed(NsdServiceInfo info, int err) {
                main.post(() -> {
                    resolving = false;
                    resolveNext();
                });
            }

            @Override
            public void onServiceResolved(NsdServiceInfo info) {
                boolean self = isOwnAddress(info.getHost());
                main.post(() -> {
                    resolving = false;
                    if (self) {  // this device's own game process announces itself (a TV hosts by default): not a console for itself
                        Log.i(TAG, "GamePad: " + info.getServiceName() + " is this device, left out");
                        resolveNext();
                        return;
                    }
                    found.removeIf(t -> t.name.equals(info.getServiceName()));
                    Tv added = new Tv(info.getServiceName(), info.getHost(), info.getPort());
                    found.add(added);
                    listFound();
                    // the TV played with last time: straight to it (also after its restart with the saves)
                    if (conn == null && tv == null && info.getServiceName().equals(prefs.getString("pad_last_tv", null))
                            && prefs.getBoolean("pad_auto", false))
                        joinSoon(added);
                    else maybeAutoJoin(added);
                    resolveNext();
                });
            }
        });
    }

    /** one of this device's own addresses (or loopback) */
    static boolean isOwnAddress(InetAddress a) {
        if (a == null) return false;
        if (a.isLoopbackAddress() || a.isAnyLocalAddress()) return true;
        try {
            for (java.net.NetworkInterface n : java.util.Collections.list(java.net.NetworkInterface.getNetworkInterfaces()))
                for (InetAddress own : java.util.Collections.list(n.getInetAddresses()))
                    if (own.equals(a)) return true;
        } catch (Exception ignored) {
        }
        return false;
    }

    private void askAddress() {
        EditText edit = new EditText(this);
        edit.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_URI);
        edit.setSingleLine(true);
        edit.setText(prefs.getString("pad_address", ""));
        edit.setTextColor(GameUi.INK);
        edit.setTextSize(20);
        new GameDialog(this).title(R.string.pad_address).message(R.string.pad_address_hint).content(edit).top()
                .button(R.string.opt_cancel, null)
                .button(R.string.opt_ok, () -> {
                    String a = edit.getText().toString().trim();
                    prefs.edit().putString("pad_address", a).apply();
                    new Thread(() -> {
                        try {
                            InetAddress host = InetAddress.getByName(a);
                            main.post(() -> connect(new Tv(a, host, PadLink.PORT)));
                        } catch (IOException e) {
                            main.post(() -> message(getString(R.string.pad_failed, e.getMessage()), null));
                        }
                    }).start();
                }).show();
    }

    // ------------------------------------------------------------------ the connection
    private void connect(Tv t) {
        tv = t;
        board = null;
        prefs.edit().putString("pad_last_tv", t.name).apply();
        stopDiscovery();
        showPad();
        setStatus(getString(R.string.pad_connecting, t.name));
        new Thread(() -> session(t, ""), "wwhd-pad").start();
    }

    private void session(Tv t, String code) {
        PadLink.Conn c;
        try {
            Socket s = new Socket();
            s.connect(new InetSocketAddress(t.host, t.port), 5000);
            c = new PadLink.Conn(s);
        } catch (IOException e) {
            main.post(() -> lost(t, e.getMessage()));
            return;
        }
        conn = c;
        try {
            hello(c, code);
            for (;;) handle(c, c.read());
        } catch (IOException e) {
            if (conn == c) main.post(() -> lost(t, e.getMessage()));
        } finally {
            c.close();
        }
    }

    private void hello(PadLink.Conn c, String code) throws IOException {
        String token = prefs.getString("pad_token@" + tv.name, "");
        c.send(PadLink.HELLO, o -> {
            o.writeInt(PadLink.VERSION);
            o.writeUTF(PadHost.deviceName(this));
            o.writeUTF(token);
            o.writeUTF(code);
            o.writeUTF(PadLink.gameId(baseDir()));
            o.writeInt(role);
        });
    }

    /** the connection broke: try again a while (the TV restarts with the saves), then back to the search */
    private void lost(Tv t, String why) {
        if (isFinishing() || tv != t) return;
        conn = null;
        releaseDecoder();
        stopSound();
        wifi.set(false);
        if (ending) {  // leaving anyway
            backToGame();
            return;
        }
        if (reconnects++ < 30) {
            setStatus(getString(R.string.pad_lost));
            main.postDelayed(() -> {
                if (tv == t && conn == null) new Thread(() -> session(t, ""), "wwhd-pad").start();
            }, 2000);
            return;
        }
        tv = null;
        if (autoJoin) {  // waiting for a device (the screen app): back to looking, no message to answer; joined again once it is back
            Log.i(TAG, "GamePad: " + t.name + " gone (" + why + "), looking again");
            PadService.stop(this);
            controls = null;
            showSearch();
            return;
        }
        message(getString(R.string.pad_failed, why), this::showSearch);
    }

    private void handle(PadLink.Conn c, PadLink.Frame f) throws IOException {
        switch (f.type) {
            case PadLink.NEED_CODE:
                main.post(() -> askCode(c, false));
                break;
            case PadLink.BAD_CODE: {
                boolean version = f.in.readUTF().equals("version");
                main.post(() -> {
                    if (version) message(getString(R.string.pad_old_version), this::backToGame);
                    else askCode(c, true);
                });
                break;
            }
            case PadLink.WELCOME: {
                String token = f.in.readUTF(), name = f.in.readUTF();
                main.post(() -> {  // (a dialog of an earlier connection: the console sends its own anew)
                    showUi(null);
                    uiDraw().clear();
                    special = false;
                    mirrorMode = false;
                    if (padScreen != null) padScreen.setVisibility(drawnNow() ? View.VISIBLE : View.GONE);
                    if (mirror != null) {
                        mirror.clear();
                        showMirror();
                    }
                    remoteAssets();
                    if (padScreen != null) padScreen.clear();
                });
                main.post(() -> PadService.start(this, name, screenRole()));  // (connected in the background too)
                tvHasGame = f.in.readBoolean();
                tvRunning = f.in.readBoolean();
                tvGame = f.in.available() > 0 ? f.in.readUTF() : "";
                tvFeatures = f.in.available() > 0 ? f.in.readUTF() : "";
                tvModel = f.in.available() > 0 ? f.in.readUTF() : "";
                tvNeedsCode = f.in.available() > 0 && f.in.readBoolean();
                prefs.edit().putString("pad_token@" + tv.name, token).putBoolean("pad_auto", true).apply();
                reconnects = 0;
                main.post(() -> welcomed(c, name));
                c.post(PadLink.CAPS, PadLink.body(o -> o.writeInt(caps())));
                break;
            }
            case PadLink.PONG: {
                long sent = f.in.readLong(), host = f.in.readLong(), now = System.nanoTime(), rtt = now - sent;
                rtts.add(rtt / 1_000_000);
                // the clocks' offset from the quickest answer (the least waiting in it): host ~ here + offset
                if (rtt < bestRtt) {
                    bestRtt = rtt;
                    clockOffset = host - (sent + rtt / 2);
                }
                break;
            }
            case PadLink.STATE:
                tvRunning = f.in.readBoolean();
                main.post(() -> {
                    setStatus(null);
                    if (controls != null) controls.sendView();  // (the console's picture choice: TV or GamePad)
                    sendShown();
                });
                break;
            case PadLink.VIDEO_CONFIG: {
                videoW = f.in.readInt();
                videoH = f.in.readInt();
                main.post(this::placePicture);
                byte[] cfg = new byte[f.in.readInt()];
                f.in.readFully(cfg);
                synchronized (decoderLock) {
                    videoConfig = cfg;
                    releaseDecoderLocked();
                    startDecoderLocked();
                }
                break;
            }
            case PadLink.VIDEO:
                decode(c, f);
                break;
            case PadLink.ARROW: {
                boolean on = f.in.readBoolean();
                float al = f.in.readFloat(), l = f.in.readFloat(), t = f.in.readFloat(), r = f.in.readFloat(), b = f.in.readFloat();
                main.post(() -> {
                    arrowOn = on;
                    arrowAlpha = al;
                    arrowBox.set(l, t, r, b);
                    if (controls != null) controls.invalidate();
                });
                break;
            }
            case PadLink.WIND_STATE: {
                boolean on = f.in.readBoolean();
                float way = f.in.readFloat(), turn = f.in.readFloat();
                main.post(() -> {
                    if (controls != null) controls.setWind(on, way, turn);
                });
                break;
            }
            case PadLink.TEXT_ASK: {
                boolean open = f.in.readBoolean();
                String init = open ? f.in.readUTF() : "";
                int max = open ? f.in.readInt() : 0;
                String message = open ? f.in.readUTF() : "";
                main.post(() -> {
                    if (open) askText(init, max, message);
                    else closeText();
                });
                break;
            }
            case PadLink.UI_FRAME: {
                int w = f.in.readInt(), h = f.in.readInt();
                android.graphics.Bitmap b = w > 0 ? android.graphics.BitmapFactory.decodeByteArray(f.data, 8, f.data.length - 8) : null;
                main.post(() -> showUi(b));
                break;
            }
            case PadLink.GAME_STATE: {
                int n = f.in.readInt();
                int[] ring = new int[n];
                for (int i = 0; i < n; i++) ring[i] = f.in.readInt();
                int m = f.in.readInt();
                int[] ts = new int[m];
                for (int i = 0; i < m; i++) ts[i] = f.in.readInt();
                int len = f.in.readInt();
                byte[] save = null;
                if (len >= 0) {
                    save = new byte[len];
                    f.in.readFully(save);
                }
                byte[] sv = save;
                int k = f.in.available() >= 4 ? f.in.readInt() : 0;
                float[] map = new float[k];
                for (int i = 0; i < k; i++) map[i] = f.in.readFloat();
                main.post(() -> { if (padScreen != null) padScreen.state(n > 0 ? ring : null, m > 0 ? ts : null, sv, k > 0 ? map : null); });
                break;
            }
            case PadLink.SPECIAL: {
                boolean on = f.in.readBoolean();
                main.post(() -> {
                    if (on && !special) takeBackdrop();
                    special = on;
                    if (padScreen != null) padScreen.setVisibility(drawnNow() ? View.VISIBLE : View.GONE);
                    showMirror();
                    placePicture();
                    if (controls != null) controls.invalidate();
                });
                break;
            }
            case PadLink.MIRROR: {
                Object q = MirrorView.parse(f.data);
                if (!BuildConfig.PUBLIC_RELEASE) MirrorView.dump(this, "frame.bin", f.data);
                main.post(() -> {
                    if (mirror == null) return;
                    mirrorMode = true;
                    mirror.frame(q);
                    showMirror();
                });
                break;
            }
            case PadLink.MIRROR_TEX: {
                int id = f.in.readInt();
                int n = f.data.length - 4;
                if (!BuildConfig.PUBLIC_RELEASE) MirrorView.dump(this, String.format(java.util.Locale.ROOT, "tex_%08x.png", id), java.util.Arrays.copyOfRange(f.data, 4, f.data.length));
                android.graphics.Bitmap b = n > 0 ? android.graphics.BitmapFactory.decodeByteArray(f.data, 4, n) : null;
                main.post(() -> { if (mirror != null) mirror.texture(id, b); });
                break;
            }
            case PadLink.INDOOR_MAP: {
                android.graphics.Bitmap b = f.data.length > 0 ? android.graphics.BitmapFactory.decodeByteArray(f.data, 0, f.data.length) : null;
                main.post(() -> { if (padScreen != null) padScreen.indoor(b); });
                break;
            }
            case PadLink.FONT: {
                String font = f.in.readUTF(), lang = f.in.readUTF();
                int files = f.in.readInt();
                java.io.File dir = GameFont.dir(this, font, lang);
                Backup.deleteTree(dir);
                if (files > 0 && (dir.mkdirs() || dir.isDirectory())) {
                    for (int i = 0; i < files; i++) {
                        String name = f.in.readUTF();
                        byte[] z = new byte[f.in.readInt()];
                        f.in.readFully(z);
                        try (java.util.zip.InflaterInputStream in = new java.util.zip.InflaterInputStream(new java.io.ByteArrayInputStream(z));
                             java.io.FileOutputStream out = new java.io.FileOutputStream(new java.io.File(dir, new java.io.File(name).getName()))) {
                            byte[] buf = new byte[65536];
                            for (int r; (r = in.read(buf)) > 0; ) out.write(buf, 0, r);
                        }
                    }
                    new java.io.File(dir, GameFont.versionFile()).createNewFile();
                }
                Log.i(TAG, "GamePad: font " + font + " (" + lang + ") from the console, " + files + " files");
                GameFont.remoteArrived(font, lang);
                main.post(() -> { if (padScreen != null) padScreen.invalidate(); });
                break;
            }
            case PadLink.LABELS: {
                int n = f.in.readInt();
                java.util.Map<String, String> labels = new java.util.HashMap<>();
                for (int i = 0; i < n; i++) {
                    int kind = f.in.readByte(), key = f.in.readInt();
                    labels.put(kind + ":" + key, f.in.readUTF());
                }
                main.post(() -> { if (padScreen != null) padScreen.labels(labels); });
                break;
            }
            case PadLink.ICON: {
                String name = f.in.readUTF();
                int off = 2 + name.getBytes(java.nio.charset.StandardCharsets.UTF_8).length;
                android.graphics.Bitmap b = f.data.length > off ? android.graphics.BitmapFactory.decodeByteArray(f.data, off, f.data.length - off) : null;
                main.post(() -> {
                    if (padRemote) GameIcons.arrived(name, b);
                    if (padScreen != null) padScreen.invalidate();
                });
                break;
            }
            case PadLink.UI_BITMAP: {
                int id = f.in.readInt();
                android.graphics.Bitmap b = UiDraw.decode(f.data, 4, f.data.length - 4);
                main.post(() -> uiDraw().put(id, b));
                break;
            }
            case PadLink.UI_DRAW: {
                int w = f.in.readInt(), h = f.in.readInt();
                byte[] z = w > 0 ? java.util.Arrays.copyOfRange(f.data, 8, f.data.length) : null;
                main.post(() -> {
                    uiDraw().show(w, h, z);
                    sendShown();
                });
                break;
            }
            case PadLink.AUDIO:
                play(f.data);
                break;
            case PadLink.WANT:
                wanted(c, f);
                break;
            case PadLink.RESULT: {
                int kind = f.in.readInt();
                boolean ok = f.in.readBoolean(), restart = f.in.readBoolean();
                long lacking = f.in.available() >= 16 ? f.in.readLong() : 0, free = lacking > 0 ? f.in.readLong() : 0;
                main.post(() -> {
                    if (lacking > 0 && !ok && kind != PadLink.KIND_CODE) {  // nothing taken for lack of room: that message alone
                        setStatus(null);
                        noSpace(kind, false, lacking, free);
                        return;
                    }
                    resultCame(kind, ok, restart);  // (the code not taken: the game files are offered next, as when it fails)
                    if (lacking > 0) noSpace(kind, ok, lacking, free);
                });
                break;
            }
            case PadLink.OFFER:
                savesOffered(c, f);
                break;
            case PadLink.FILE: {
                PadLink.Receive r = receive;
                int kind = f.in.readInt();  // (the transfer's kind first, then the file: as PadHost reads it)
                if (r != null && kind == r.kind) r.file(f.in);
                break;
            }
            case PadLink.CHUNK:
                if (receive != null && PadLink.chunkKind(f.data) == receive.kind) receive.chunk(f.data);
                lastData = android.os.SystemClock.uptimeMillis();
                break;
            case PadLink.DONE:
                savesBackReceived(c);
                break;
            default:
                break;
        }
    }

    private void askCode(PadLink.Conn c, boolean wrong) {
        EditText edit = new EditText(this);
        edit.setInputType(InputType.TYPE_CLASS_NUMBER);
        edit.setSingleLine(true);
        edit.setTextColor(GameUi.INK);
        edit.setTextSize(28);
        edit.setFilters(new android.text.InputFilter[] {new android.text.InputFilter.LengthFilter(4)});
        GameDialog[] shown = new GameDialog[1];
        boolean[] sent = new boolean[1];
        Runnable ok = () -> {
            if (sent[0]) return;
            sent[0] = true;
            String code = edit.getText().toString().trim();
            send.post(() -> {
                try {
                    hello(c, code);
                } catch (IOException ignored) {
                }
            });
        };
        // the fourth digit sends it (a TV's remote: no way down to OK through the keyboard first)
        edit.addTextChangedListener(new android.text.TextWatcher() {
            @Override public void beforeTextChanged(CharSequence t, int a, int b, int n) {}
            @Override public void onTextChanged(CharSequence t, int a, int b, int n) {}

            @Override
            public void afterTextChanged(android.text.Editable e) {
                if (e.length() == 4 && shown[0] != null) main.post(() -> {
                    if (shown[0].isShowing()) {
                        shown[0].dismiss();
                        ok.run();
                    }
                });
            }
        });
        shown[0] = new GameDialog(this).title(R.string.pad_code_title).message(wrong ? R.string.pad_code_wrong : R.string.pad_code_text)
                .content(edit).top().cancelable(false)
                .button(R.string.opt_cancel, () -> {
                    if (tv != null) noAutoJoin.add(tv.name);
                    tv = null;
                    c.close();
                    showSearch();
                })
                .button(R.string.opt_ok, ok);
        shown[0].show();
    }

    private void welcomed(PadLink.Conn c, String name) {
        wifi.set(true);
        bestRtt = Long.MAX_VALUE;
        rtts.clear();
        pictureDelays.clear();
        paceDelayNs = paceStepNs = lastWhen = lastPtsNs = 0;
        send.removeCallbacks(ping);
        send.post(ping);
        setStatus(tvRunning ? null : getString(R.string.pad_waiting, name));
        sendPicture();
        sendSound();
        if (screenRole()) return;  // the screen: no input, no files
        startMotion();
        if (!tvHasGame) {
            List<PadLink.Entry> game = PadLink.gameFiles(baseDir());
            long size = 0;
            for (PadLink.Entry e : game) size += e.size;
            if (game.isEmpty()) {
                message(getString(R.string.pad_no_game_here), null);
                return;
            }
            boolean code = !tvFeatures.isEmpty() && Native.libraryHere(this);
            new GameDialog(this).title(getString(R.string.pad_send_game_title, name))
                    .message(getString(code ? R.string.pad_send_game_code_text : R.string.pad_send_game_text, PadLink.sizeText(size), name))
                    .button(R.string.opt_cancel, null)
                    .button(R.string.pad_send, () -> {
                        if (code) sendCodeThenGame(c, game, name);
                        else offer(c, PadLink.KIND_GAME, game);
                    }).show();
            return;
        }
        // the TV has the game but no code for it yet (e.g. after an update): compiled here, it saves the TV's own compile
        if (tvNeedsCode && sameGame() && !tvFeatures.isEmpty() && Native.libraryHere(this)) {
            new GameDialog(this).title(getString(R.string.pad_code_title_other, name)).message(getString(R.string.pad_code_offer, name))
                    .button(R.string.opt_cancel, null)
                    .button(R.string.pad_code_prepare, () -> sendCodeThenGame(c, null, name)).show();
        }
        String mine = PadLink.gameId(baseDir());
        if (!PadLink.savesFit(mine, tvGame)) return;
        if (!sameGame())  // another release: the game save goes along (converted there), the save states can't
            message(getString(R.string.pad_other_release, PadLink.release(tvGame), PadLink.release(mine)), null);
        if (prefs.getBoolean(PREF_SAVES_TO, true)) offer(c, PadLink.KIND_SAVES, PadLink.saveFiles(baseDir(), sameGame()));
    }

    // ---- the game code for the TV: compiled here for its CPU (this device is faster than most TVs), sent before the
    // game files, so the TV finds it ready and starts without its own compile. The plain game (no mod packs).
    private List<PadLink.Entry> pendingGame;

    private void sendCodeThenGame(PadLink.Conn c, List<PadLink.Entry> game, String name) {
        pendingGame = game;
        File dir = new File(getNoBackupFilesDir(), "codecache-for-other");
        new Thread(() -> {
            // a cache made for another CPU goes (its objects wouldn't fit), one for the same is used again
            File mark = new File(dir, "cpu.txt");
            String want = tvFeatures + "\n" + tvModel;
            try {
                if (!mark.isFile() || !want.equals(new String(java.nio.file.Files.readAllBytes(mark.toPath())))) Backup.deleteTree(dir);
            } catch (IOException e) {
                Backup.deleteTree(dir);
            }
            //noinspection ResultOfMethodCallIgnored
            dir.mkdirs();
            boolean[] running = {true};
            new Thread(() -> {
                while (running[0]) {
                    long[] p = Native.compileProgress();
                    main.post(() -> setStatus(getString(R.string.pad_preparing_code, name, (int) p[0], (int) Math.max(p[1], p[0]))));
                    try {
                        Thread.sleep(1000);
                    } catch (InterruptedException ignored) {
                    }
                }
            }).start();
            long t0 = android.os.SystemClock.uptimeMillis();
            String err = Native.compileGameFor(new File(baseDir(), "game").getAbsolutePath(), dir.getAbsolutePath(), tvFeatures, tvModel);
            running[0] = false;
            Log.i(TAG, "GamePad: game code for " + name + (err == null ? " compiled" : " not compiled: " + err) + " in "
                    + (android.os.SystemClock.uptimeMillis() - t0) / 1000 + " s");
            if (err == null) {
                try {
                    java.nio.file.Files.write(mark.toPath(), want.getBytes());
                } catch (IOException ignored) {
                }
                List<PadLink.Entry> files = new java.util.ArrayList<>();
                PadLink.list(dir, "", files);
                files.removeIf(e -> e.path.equals("cpu.txt"));
                main.post(() -> offer(c, PadLink.KIND_CODE, files));
            } else {
                main.post(() -> {  // the TV compiles it itself then
                    pendingGame = null;
                    if (game != null) offer(c, PadLink.KIND_GAME, game);
                    else setStatus(null);
                });
            }
        }, "wwhd-pad-compile").start();
    }

    File baseDir() {
        File f = getExternalFilesDir(null);
        return f != null ? f : getFilesDir();
    }

    // ---- files to the TV
    private List<PadLink.Entry> offered;
    private int offeredKind;

    private void offer(PadLink.Conn c, int kind, List<PadLink.Entry> files) {
        offered = files;
        offeredKind = kind;
        c.post(PadLink.OFFER, PadLink.offer(kind, files));
    }

    private void wanted(PadLink.Conn c, PadLink.Frame f) throws IOException {
        int kind = f.in.readInt();
        List<Integer> want = PadLink.readWant(f.in);
        // the other device's room checked before anything is sent: what stays here is said now, not after the transfer
        long lacking = f.in.available() >= 16 ? f.in.readLong() : 0, free = lacking > 0 ? f.in.readLong() : 0;
        if (lacking > 0 && kind == offeredKind) {
            boolean partly = !want.isEmpty();
            main.post(() -> {
                if (!partly && kind != PadLink.KIND_CODE) setStatus(null);
                noSpace(kind, partly, lacking, free);
            });
        }
        if (kind != offeredKind || offered == null || want.isEmpty()) return;
        List<PadLink.Entry> files = offered;
        String what = getString(kind == PadLink.KIND_GAME ? R.string.pad_what_game : kind == PadLink.KIND_CODE ? R.string.pad_what_code
                                                                                                    : R.string.pad_what_saves);
        File root = kind == PadLink.KIND_GAME ? new File(baseDir(), "game")
                  : kind == PadLink.KIND_CODE ? new File(getNoBackupFilesDir(), "codecache-for-other") : baseDir();
        new Thread(() -> {
            long[] last = {0};
            try {
                PadLink.sendFiles(c, kind, root, files, want, (done, total) -> {
                    long now = android.os.SystemClock.uptimeMillis();
                    if (now - last[0] < 500) return;
                    last[0] = now;
                    int pct = (int) (100 * done / Math.max(1, total));
                    main.post(() -> setStatus(getString(R.string.pad_sending, what, pct)));
                });
            } catch (IOException e) {
                Log.w(TAG, "GamePad: sending failed", e);
                c.close();
            }
        }, "wwhd-pad-files").start();
    }

    private void resultCame(int kind, boolean ok, boolean restart) {
        if (ending) return;
        if (kind == PadLink.KIND_CODE) {  // the code is there (or not: the TV compiles it then): now the game files
            List<PadLink.Entry> g = pendingGame;
            pendingGame = null;
            PadLink.Conn c = conn;
            if (g != null && c != null) offer(c, PadLink.KIND_GAME, g);
            else setStatus(tvRunning ? null : getString(R.string.pad_waiting, tv != null ? tv.name : ""));  // (code only: it starts now)
            return;
        }
        if (!ok) {
            setStatus(null);
            message(getString(R.string.pad_send_failed), null);
            return;
        }
        String other = tv != null ? tv.name : "";
        if (kind == PadLink.KIND_GAME) setStatus(getString(R.string.pad_tv_preparing, other));
        else if (restart) setStatus(getString(R.string.pad_saves_restart, other));
        else setStatus(tvRunning ? null : getString(R.string.pad_waiting, tv != null ? tv.name : ""));
    }

    private boolean toldNoSpace;  // (once per visit: the next connection offers the same again)

    /** the other device had no room for (some of) what was offered: `lacking` bytes stayed here */
    private void noSpace(int kind, boolean partly, long lacking, long free) {
        if (ending || toldNoSpace) return;
        toldNoSpace = true;
        String other = tv != null ? tv.name : "";
        if (kind == PadLink.KIND_SAVES && partly)
            message(getString(R.string.pad_space_states, other, PadLink.sizeText(free), PadLink.sizeText(lacking)), null);
        else message(getString(R.string.pad_space_none, other, PadLink.sizeText(free), PadLink.sizeText(lacking + PadLink.SPACE_MARGIN)), null);
    }

    // ---- the saves back from the TV (leaving)
    private volatile PadLink.Receive receive;  // (set on the connection's thread, watched from the main thread)
    private volatile long lastData;              // uptime of the last file data from the TV

    private void endSession() {
        PadLink.Conn c = conn;
        ending = true;
        if (c == null || screenRole() || !prefs.getBoolean(PREF_SAVES_BACK, true) || !PadLink.savesFit(PadLink.gameId(baseDir()), tvGame)) {
            bye(c);
            return;
        }
        setStatus(getString(R.string.pad_end_saving));
        c.post(PadLink.SAVES_BACK, new byte[0]);
        lastData = android.os.SystemClock.uptimeMillis();
        main.postDelayed(new Runnable() {  // a TV that stops answering: leave after 15 s without anything from it
            @Override
            public void run() {
                if (isFinishing() || conn != c) return;
                if (android.os.SystemClock.uptimeMillis() - lastData < 15000) main.postDelayed(this, 2000);
                else {
                    Log.w(TAG, "GamePad: no saves from the TV for 15 s, leaving without them");
                    PadLink.Receive r = receive;
                    receive = null;
                    if (r != null) {
                        r.abort();
                        Backup.deleteTree(r.dir);
                    }
                    bye(c);
                }
            }
        }, 2000);
    }

    private void savesOffered(PadLink.Conn c, PadLink.Frame f) throws IOException {
        int kind = f.in.readInt();
        List<PadLink.Entry> files = PadLink.readOffer(f.in);
        if (kind != PadLink.KIND_SAVES || !ending) return;
        File base = baseDir(), dir = new File(base, PadLink.RECEIVING);
        lastData = android.os.SystemClock.uptimeMillis();
        Backup.deleteTree(dir);
        List<Integer> want = PadLink.onlyFitting(files, PadLink.newerSaves(base, files), sameGame());
        long need = PadLink.size(files, want), free = PadLink.freeSpace(dir);
        if (!want.isEmpty() && need + PadLink.SPACE_MARGIN > free) {  // (no room here: what fits, the rest stays there)
            want = PadLink.fitSpace(files, want, free);
            Log.w(TAG, "GamePad: " + PadLink.sizeText(need) + " of saves offered back, " + PadLink.sizeText(free) + " free: "
                    + PadLink.sizeText(need - PadLink.size(files, want)) + " not taken");
        }
        c.send(PadLink.WANT, PadLink.want(kind, want));
        if (want.isEmpty()) {
            main.post(() -> bye(c));
            return;
        }
        receive = new PadLink.Receive(kind, dir, files, want);
    }

    private void savesBackReceived(PadLink.Conn c) {
        PadLink.Receive r = receive;
        receive = null;
        if (r == null) return;
        Log.i(TAG, "GamePad: saves from the TV received (" + r.done() / 1024 + " KiB)");
        try {
            r.finish();
            PadLink.convertSave(r.dir, tvGame, PadLink.gameId(baseDir()));  // (another release's)
            PadLink.applySaves(baseDir(), r.dir);
        } catch (IOException e) {
            Log.w(TAG, "GamePad: the saves from the TV failed", e);
            Backup.deleteTree(r.dir);
        }
        main.post(() -> bye(c));
    }

    private void bye(PadLink.Conn c) {
        conn = null;
        if (c != null) {
            send.post(() -> {
                try {
                    c.send(PadLink.BYE, new byte[0]);
                } catch (IOException ignored) {
                }
                c.close();
            });
        }
        prefs.edit().putBoolean("pad_auto", false).apply();
        main.postDelayed(this::backToGame, 300);
    }

    /** this device plays the game itself again */
    private void backToGame() {
        if (RECEIVER) {  // (never a game here: looking again)
            toSearch();
            return;
        }
        Log.i(TAG, "GamePad: back to the game", new Throwable("from"));  // (which way: the log tells)
        Intent i = new Intent(this, MainActivity.class);
        i.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TASK);
        startActivity(i);
        finishAndRemoveTask();
        main.postDelayed(() -> System.exit(0), 200);
    }

    private void message(String text, Runnable then) {
        new GameDialog(this).title(R.string.pad_title).message(text).button(R.string.opt_ok, then).show();
    }

    // ------------------------------------------------------------------ the pad screen
    private void showPad() {
        padRoot = new FrameLayout(this);
        padRoot.setBackgroundColor(Color.BLACK);
        picture = new SurfaceView(this);
        picture.getHolder().addCallback(new SurfaceHolder.Callback() {
            @Override public void surfaceCreated(SurfaceHolder h) {}

            @Override
            public void surfaceChanged(SurfaceHolder h, int format, int w, int hh) {
                synchronized (decoderLock) {
                    if (pictureSurface == h.getSurface() && decoder != null) return;
                    releaseDecoderLocked();
                    pictureSurface = h.getSurface();
                    startDecoderLocked();
                }
            }

            @Override
            public void surfaceDestroyed(SurfaceHolder h) {
                synchronized (decoderLock) {
                    pictureSurface = null;
                    releaseDecoderLocked();
                }
            }
        });
        padRoot.addView(picture, new FrameLayout.LayoutParams(0, 0));
        // the game's own GamePad screens drawn here as the game draws them (PadLink.MIRROR), over the picture's place
        mirror = new MirrorView(this, () -> post(PadLink.MIRROR_RESET, new byte[0]), () -> backdrop);
        mirror.setVisibility(View.GONE);
        padRoot.addView(mirror, new FrameLayout.LayoutParams(0, 0));
        remoteAssets();
        padScreen = new PadScreen(this, (type, body) -> {
            PadLink.Conn c = conn;
            if (c != null) c.post(type, body);
        });
        padScreen.setVisibility(drawn() ? View.VISIBLE : View.GONE);
        padScreen.showPage(getIntent().getIntExtra(EXTRA_PAGE, 0));
        padRoot.addView(padScreen, new FrameLayout.LayoutParams(-1, -1));
        controls = new Controls();
        controls.touchButtons = !screenRole() && prefs.getBoolean(PREF_TOUCH, true);
        padRoot.addView(controls, new FrameLayout.LayoutParams(-1, -1));
        status = GameUi.text(this, false);
        status.setTextColor(Color.WHITE);
        status.setTextSize(16);
        status.setGravity(Gravity.CENTER);
        status.setBackgroundColor(0xB0000000);
        int p = GameUi.px(this, 12);
        status.setPadding(p, p, p, p);
        FrameLayout.LayoutParams slp = new FrameLayout.LayoutParams(-2, -2, Gravity.CENTER);
        padRoot.addView(status, slp);
        uiView = new android.widget.ImageView(this);
        uiView.setScaleType(android.widget.ImageView.ScaleType.FIT_CENTER);
        uiView.setBackgroundColor(0xFF0C1622);
        uiView.setVisibility(View.GONE);
        uiView.setOnTouchListener((v, e) -> uiTouch(e));
        padRoot.addView(uiView, new FrameLayout.LayoutParams(-1, -1));
        uiDraw();
        noPicture = GameUi.text(this, false);
        noPicture.setText(R.string.pad_picture_on_tv);
        noPicture.setTextColor(0xFFB0C4D8);
        noPicture.setTextSize(15);
        noPicture.setGravity(Gravity.CENTER);
        noPicture.setVisibility(picture() == 0 && !drawn() ? View.VISIBLE : View.GONE);
        padRoot.addView(noPicture, new FrameLayout.LayoutParams(GameUi.px(this, 320), -2, Gravity.CENTER));
        padRoot.addOnLayoutChangeListener((v, l, t, r, b, ol, ot, or, ob) -> placePicture());
        setContentView(padRoot);
        hideBars();
    }

    /** the picture in the video's shape (16:9 until it is known), as large as fits, in the middle */
    private void placePicture() {
        int w = padRoot.getWidth(), h = padRoot.getHeight();
        if (w == 0 || h == 0) return;
        float a = videoW > 0 && videoH > 0 ? videoW / (float) videoH : 16f / 9f;
        int pw = w, ph = h;
        if (pw > ph * a) pw = Math.round(ph * a);
        else ph = Math.round(pw / a);
        FrameLayout.LayoutParams lp = (FrameLayout.LayoutParams) picture.getLayoutParams();
        if (lp.width == pw && lp.height == ph && (mirror == null || mirror.getLayoutParams().width == pw)) return;
        lp.width = pw;
        lp.height = ph;
        lp.gravity = Gravity.CENTER;
        picture.setLayoutParams(lp);
        if (mirror != null) {
            FrameLayout.LayoutParams ml = (FrameLayout.LayoutParams) mirror.getLayoutParams();
            ml.width = pw;
            ml.height = ph;
            ml.gravity = Gravity.CENTER;
            mirror.setLayoutParams(ml);
        }
        if (controls != null) controls.postInvalidate();  // (its dimming follows the picture)
    }

    private void setStatus(String text) {
        if (status == null) return;
        status.setText(text);
        status.setVisibility(text == null ? View.GONE : View.VISIBLE);
    }

    // ---- the game's text input (a save file's name): the console asks (PadLink.TEXT_ASK), this device's own field
    // and keyboard answer (TEXT_ANSWER); the console's own field answered first closes this one
    private GameDialog textDialog;

    private void askText(String initial, int maxLen, String message) {
        closeText();
        android.widget.EditText edit = new android.widget.EditText(this);
        edit.setInputType(android.text.InputType.TYPE_CLASS_TEXT);
        edit.setSingleLine(true);
        if (maxLen > 0) edit.setFilters(new android.text.InputFilter[] {new android.text.InputFilter.LengthFilter(maxLen)});
        edit.setText(initial);
        edit.setSelection(edit.getText().length());
        edit.setTextColor(GameUi.INK);
        edit.setTextSize(24);
        int pad = GameUi.px(this, 14);
        edit.setPadding(pad, pad / 2, pad, pad / 2);
        android.graphics.drawable.GradientDrawable field = new android.graphics.drawable.GradientDrawable();
        field.setColor(0xFFFFFBF0);
        field.setCornerRadius(GameUi.px(this, 8));
        field.setStroke(GameUi.px(this, 3), 0xFF8B5A2B);
        edit.setBackground(field);
        edit.setImeOptions(android.view.inputmethod.EditorInfo.IME_ACTION_DONE | android.view.inputmethod.EditorInfo.IME_FLAG_NO_EXTRACT_UI);
        LinearLayout box = new LinearLayout(this);
        box.setPadding(0, GameUi.px(this, 14), 0, 0);
        box.addView(edit, new LinearLayout.LayoutParams(-1, -2));
        GameDialog d = new GameDialog(this).top().title(R.string.text_title).message(message).content(box)
                .button(R.string.opt_cancel, () -> answerText(false, ""))
                .button(R.string.opt_ok, () -> answerText(true, edit.getText().toString()));
        edit.setOnEditorActionListener((v, action, e) -> {  // the keyboard's ✓: OK
            d.press(1);
            return true;
        });
        textDialog = d;
        d.getWindow().setSoftInputMode(WindowManager.LayoutParams.SOFT_INPUT_STATE_ALWAYS_VISIBLE
                | WindowManager.LayoutParams.SOFT_INPUT_ADJUST_NOTHING);
        d.show();
        edit.requestFocus();
    }

    private void answerText(boolean ok, String text) {
        textDialog = null;
        PadLink.Conn c = conn;
        if (c != null) c.post(PadLink.TEXT_ANSWER, PadLink.body(o -> {
            o.writeBoolean(ok);
            o.writeUTF(text);
        }));
    }

    private void closeText() {
        GameDialog d = textDialog;
        textDialog = null;
        if (d != null) d.dismiss();
    }

    private void openMenu() {
        GameDialog d = new GameDialog(this).title(tv != null ? tv.name : getString(R.string.pad_title));
        long[] r = rtts.get(), p = pictureDelays.get(), s = soundDelays.get();
        if (r[0] >= 0) d.message(getString(R.string.pad_delays, p[0] < 0 ? "–" : String.valueOf(p[0]), String.valueOf(r[0]),
                s[0] < 0 ? "–" : String.valueOf(s[0] + r[0] / 2)));
        if (!screenRole()) d.toggle(getString(R.string.pad_touch_buttons), controls.touchButtons, on -> {
                    controls.touchButtons = on;
                    prefs.edit().putBoolean(PREF_TOUCH, on).apply();
                    controls.invalidate();
                })
                .toggle(getString(R.string.opt_controls_blind), prefs.getBoolean("controls_blind", true), on -> {
                    prefs.edit().putBoolean("controls_blind", on).apply();  // (the console's own option: the same file)
                    controls.invalidate();
                })
                .toggle(getString(R.string.opt_blind_face) + ": " + getString(R.string.blind_face_swipe),
                        prefs.getInt("controls_blind_face", 1) == 1, on -> {
                    prefs.edit().putInt("controls_blind_face", on ? 1 : 0).apply();
                    controls.invalidate();
                })
                .toggle(getString(R.string.pad_saves_to), prefs.getBoolean(PREF_SAVES_TO, true),
                        on -> prefs.edit().putBoolean(PREF_SAVES_TO, on).apply())
                .toggle(getString(R.string.pad_saves_back), prefs.getBoolean(PREF_SAVES_BACK, true),
                        on -> prefs.edit().putBoolean(PREF_SAVES_BACK, on).apply());
        d.toggle(getString(screenRole() ? R.string.pad_sound_screen : R.string.pad_sound_pad), prefs.getBoolean(PREF_SOUND, true), on -> {
            prefs.edit().putBoolean(PREF_SOUND, on).apply();
            sendSound();
        });
        d.button(screenRole() ? R.string.pad_screen_picture : R.string.pad_picture, this::choosePicture);
        if (!screenRole() && conn != null)  // the console's options open on its screen; this controller works them
            d.button(R.string.pad_console_options, () -> {
                long now = android.os.SystemClock.uptimeMillis();
                sendKey(new KeyEvent(now, now, KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_BUTTON_MODE, 0));
                sendKey(new KeyEvent(now, now, KeyEvent.ACTION_UP, KeyEvent.KEYCODE_BUTTON_MODE, 0));
            });
        d.button(R.string.pad_other_tv, () -> {
            PadLink.Conn c = conn;
            if (tv != null) noAutoJoin.add(tv.name);
            conn = null;
            tv = null;
            prefs.edit().putBoolean("pad_auto", false).apply();
            if (c != null) send.post(c::close);
            PadService.stop(this);
            releaseDecoder();
            showSearch();
        });
        d.button(R.string.pad_end, this::endSession);
        d.button(R.string.opt_back, null);
        d.show();
    }

    // ---- the GamePad picture's size (PadLink.PICTURES): the TV encodes it so; none: a controller only, the TV
    // keeps the GamePad picture (+ shows it there) and encodes nothing
    // (the screen's own choice: the TV picture, never none)
    private int picture() {
        int def = screenRole() ? PadLink.SCREEN_PICTURE_DEFAULT : PadLink.PICTURE_DEFAULT;
        int p = prefs.getInt(screenRole() ? PREF_SCREEN_PICTURE : PREF_PICTURE, def);
        if ((p == PICTURE_DRAWN || p == PICTURE_DRAWN_VIDEO) && !screenRole()) return 0;  // (no video: drawn here)
        return p >= (screenRole() ? 1 : 0) && p < PadLink.PICTURES.length ? p : def;
    }

    // the GamePad screen drawn here from the game's state (PadScreen), no video: the picture choice after the sizes
    private static final int PICTURE_DRAWN = 4;
    // ... with the game's own screens (the album, the warp map ...) as video, the way before they were drawn here too
    private static final int PICTURE_DRAWN_VIDEO = 5;
    private PadScreen padScreen;

    // the game's textures and fonts for the drawn GamePad screen (PadScreen, GameUi's game-look pieces): from the
    // console, when this device runs no game itself
    private boolean padRemote;

    private void remoteAssets() {
        if (MainActivity.instance != null || screenRole()) return;
        padRemote = true;
        GameIcons.setRemote(tex -> post(PadLink.ICON_WANT, PadLink.body(o -> o.writeUTF(tex))));
        GameFont.setRemote(font -> post(PadLink.FONT_WANT, PadLink.body(o -> o.writeUTF(font))));
    }

    private void post(byte type, byte[] body) {
        PadLink.Conn c = conn;
        if (c != null) c.post(type, body);
    }

    // the game's GamePad shows a screen of its own (PadLink.SPECIAL): drawn here as the game draws it (MirrorView) or its
    // picture (an older console) meanwhile, touches to it
    private boolean special;
    private MirrorView mirror;

    // the drawn page as it was when a screen of the game's own came, blurred: what the game shows behind its questions
    // (a picture it draws on the GPU, not in its memory: MirrorView)
    private android.graphics.Bitmap backdrop;

    private void takeBackdrop() {
        backdrop = null;
        if (mirror != null) mirror.dropBackdrop();
        if (padScreen == null || padScreen.getWidth() == 0 || picture == null || picture.getWidth() == 0) return;
        // the picture's place on the drawn page, small (then stretched: soft)
        int w = 160, h = 90;
        android.graphics.Bitmap b = android.graphics.Bitmap.createBitmap(w, h, android.graphics.Bitmap.Config.ARGB_8888);
        android.graphics.Canvas c = new android.graphics.Canvas(b);
        c.drawColor(Color.BLACK);
        c.scale(w / (float) picture.getWidth(), h / (float) picture.getHeight());
        c.translate(padScreen.getLeft() - picture.getLeft(), padScreen.getTop() - picture.getTop());
        padScreen.draw(c);
        // twice down and up again: a soft blur
        android.graphics.Bitmap s = android.graphics.Bitmap.createScaledBitmap(b, w / 4, h / 4, true);
        backdrop = android.graphics.Bitmap.createScaledBitmap(s, w, h, true);
    }

    // the console draws the game's own screens here (MIRROR came): while one has nothing to draw (the title), the drawn
    // pages stay (drawnNow)
    private boolean mirrorMode;

    private void showMirror() {
        if (mirror == null) return;
        boolean on = special && drawn() && mirror.hasFrame();
        mirror.setVisibility(on ? View.VISIBLE : View.GONE);
        if (padScreen != null) padScreen.setVisibility(drawnNow() ? View.VISIBLE : View.GONE);
    }

    /** the drawn pages show now (not a screen of the game's own) */
    private boolean drawnNow() { return drawn() && !(special && (!mirrorMode || mirror == null || mirror.hasFrame())); }

    private boolean drawn() {
        int p = prefs.getInt(PREF_PICTURE, PadLink.PICTURE_DEFAULT);
        return !screenRole() && (p == PICTURE_DRAWN || p == PICTURE_DRAWN_VIDEO);
    }

    /** what this device can do (PadLink.CAPS): the game's own screens drawn here unless they are wanted as video */
    private int caps() {
        return PadLink.CAP_UI_DRAW | (prefs.getInt(PREF_PICTURE, PadLink.PICTURE_DEFAULT) == PICTURE_DRAWN_VIDEO ? 0 : PadLink.CAP_MIRROR);
    }

    /** a GamePad screen here: the video or the drawn one */
    private boolean hasScreen() { return picture() > 0 || drawn(); }

    private String pictureName(int p) {
        if (p == PICTURE_DRAWN) return getString(R.string.pad_picture_drawn);
        if (p == PICTURE_DRAWN_VIDEO) return getString(R.string.pad_picture_drawn_video);
        return p == 0 ? getString(R.string.pad_picture_none) : PadLink.PICTURES[p][0] + " × " + PadLink.PICTURES[p][1];
    }

    private void choosePicture() {
        GameDialog d = new GameDialog(this).title(screenRole() ? R.string.pad_screen_picture : R.string.pad_picture)
                .message(getString(screenRole() ? R.string.pad_screen_picture_text : R.string.pad_picture_text,
                        pictureName(drawn() ? prefs.getInt(PREF_PICTURE, PICTURE_DRAWN) : picture())));
        if (!screenRole())
            for (int dp : new int[] {PICTURE_DRAWN, PICTURE_DRAWN_VIDEO})
                d.button(pictureName(dp), () -> {
                    prefs.edit().putInt(PREF_PICTURE, dp).apply();
                    sendPicture();
                });
        for (int i = screenRole() ? 1 : 0; i < PadLink.PICTURES.length; i++) {
            final int p = i;
            d.button(pictureName(p), () -> {
                prefs.edit().putInt(screenRole() ? PREF_SCREEN_PICTURE : PREF_PICTURE, p).apply();
                sendPicture();
            });
        }
        d.show();
    }

    private void sendPicture() {
        int p = picture();
        boolean dr = drawn();
        if (noPicture != null) noPicture.setVisibility(p == 0 && !dr ? View.VISIBLE : View.GONE);
        // (drawn: the picture's surface stays, under the drawn pages, for the game's own screens: SPECIAL)
        special = false;
        showMirror();
        picture.setVisibility(p == 0 && !dr ? View.INVISIBLE : View.VISIBLE);
        if (padScreen != null) padScreen.setVisibility(dr ? View.VISIBLE : View.GONE);
        PadLink.Conn c = conn;
        // (a screen device: it shows the console's dialogs too while no GamePad device is there, drawn here)
        if (c != null) c.post(PadLink.CAPS, PadLink.body(o -> o.writeInt(screenRole() ? PadLink.CAP_UI_DRAW : caps())));
        if (c != null && !screenRole()) c.post(PadLink.DRAWN, PadLink.body(o -> o.writeBoolean(dr)));
        // this screen's shape (landscape): the picture comes in it
        android.graphics.Rect win = getWindowManager().getCurrentWindowMetrics().getBounds();
        float shape = Math.max(win.width(), win.height()) / (float) Math.max(1, Math.min(win.width(), win.height()));
        if (c != null) c.post(PadLink.PICTURE, PadLink.body(o -> {
            o.writeInt(p);
            o.writeFloat(shape);
        }));
    }

    // ---- sound from the other device: the TV sound (the screen) or the GamePad's own (the GamePad), raw 48 kHz
    // stereo; about 40 ms gathered before it starts, more than 100 ms waiting is dropped (the delay stays short)
    private android.media.AudioTrack track;
    private int trackBuffered;  // bytes written before playing
    private long trackFrames;   // frames written

    private void sendSound() {
        PadLink.Conn c = conn;
        boolean on = prefs.getBoolean(PREF_SOUND, true);
        if (c != null) c.post(PadLink.SOUND, PadLink.body(o -> o.writeBoolean(on)));
        if (!on) stopSound();
    }

    private static final int SOUND_MAX_MS = 50;  // more waiting in the track: this piece is dropped (stays in step with the picture)

    private synchronized void play(byte[] pcm) {
        if (!prefs.getBoolean(PREF_SOUND, true)) return;
        if (track != null && track.getPlayState() == android.media.AudioTrack.PLAYSTATE_PLAYING
                && (trackFrames - (track.getPlaybackHeadPosition() & 0xFFFFFFFFL)) * 1000 / 48000 > SOUND_MAX_MS) return;
        if (track == null) {
            int bytes = 48000 / 10 * 4;  // 100 ms
            track = new android.media.AudioTrack.Builder()
                    .setAudioAttributes(new android.media.AudioAttributes.Builder().setUsage(android.media.AudioAttributes.USAGE_GAME)
                            .setContentType(android.media.AudioAttributes.CONTENT_TYPE_MUSIC).build())
                    .setAudioFormat(new android.media.AudioFormat.Builder().setSampleRate(48000)
                            .setEncoding(android.media.AudioFormat.ENCODING_PCM_16BIT)
                            .setChannelMask(android.media.AudioFormat.CHANNEL_OUT_STEREO).build())
                    .setBufferSizeInBytes(Math.max(bytes, android.media.AudioTrack.getMinBufferSize(48000,
                            android.media.AudioFormat.CHANNEL_OUT_STEREO, android.media.AudioFormat.ENCODING_PCM_16BIT)))
                    .setPerformanceMode(android.media.AudioTrack.PERFORMANCE_MODE_LOW_LATENCY)
                    .setTransferMode(android.media.AudioTrack.MODE_STREAM).build();
            trackBuffered = 0;
            trackFrames = 0;
        }
        int written = track.write(pcm, 0, pcm.length, android.media.AudioTrack.WRITE_NON_BLOCKING);  // (a full buffer: the rest is dropped)
        if (written > 0) trackFrames += written / 4;
        if (track.getPlayState() == android.media.AudioTrack.PLAYSTATE_PLAYING)  // what waits in it: the sound's delay here
            soundDelays.add((trackFrames - (track.getPlaybackHeadPosition() & 0xFFFFFFFFL)) * 1000 / 48000);
        if (track.getPlayState() != android.media.AudioTrack.PLAYSTATE_PLAYING) {
            trackBuffered += pcm.length;
            if (trackBuffered >= 48000 / 25 * 4) track.play();
        }
    }

    private synchronized void stopSound() {
        android.media.AudioTrack t = track;
        track = null;
        if (t != null) {
            try {
                t.stop();
            } catch (IllegalStateException ignored) {
            }
            t.release();
        }
    }

    // ---- the console's dialogs (options, questions) shown here: touches and keys go to it there
    private void showUi(android.graphics.Bitmap b) {
        if (uiView == null) return;
        uiView.setImageBitmap(b);
        uiView.setVisibility(b != null ? View.VISIBLE : View.GONE);
        sendShown();
    }

    // ---- the items page's options arrow (PadLink.ARROW: the console's app draws it, it isn't in the picture): drawn
    // over the GamePad picture here, a tap opens the console's options page (PadLink.OPTIONS; it comes as a dialog)
    private boolean arrowOn;
    private float arrowAlpha = 1;
    private final android.graphics.RectF arrowBox = new android.graphics.RectF(), arrowRect = new android.graphics.RectF();
    private GameUi.MenuArrow arrowIcon;

    /** the arrow's place on the screen now, false when it isn't shown */
    private boolean placeArrow() {
        if (!arrowOn || arrowAlpha < 0.05f || picture == null || picture.getVisibility() != View.VISIBLE || uiShown() || drawnNow()
                || picture.getWidth() <= 0 || (controls != null && controls.tvBack())) return false;
        float w = picture.getWidth(), h = picture.getHeight(), x = picture.getLeft(), y = picture.getTop();
        arrowRect.set(x + arrowBox.left * w, y + arrowBox.top * h, x + arrowBox.right * w, y + arrowBox.bottom * h);
        return true;
    }

    private void drawArrow(Canvas cv) {
        if (!placeArrow()) return;
        if (arrowIcon == null) arrowIcon = new GameUi.MenuArrow(this, true);
        arrowIcon.setAlpha(Math.round(255 * arrowAlpha));
        arrowIcon.setBounds(Math.round(arrowRect.left), Math.round(arrowRect.top), Math.round(arrowRect.right), Math.round(arrowRect.bottom));
        arrowIcon.draw(cv);
    }

    /** a finger landing on the arrow: the options */
    private boolean arrowTouch(MotionEvent e, int action, int idx) {
        if ((action != MotionEvent.ACTION_DOWN && action != MotionEvent.ACTION_POINTER_DOWN) || !placeArrow()) return false;
        float x = e.getX(idx), y = e.getY(idx), slack = arrowRect.width() * 0.25f;
        if (x < arrowRect.left - slack || x > arrowRect.right + slack || y < arrowRect.top - slack || y > arrowRect.bottom + slack) return false;
        PadLink.Conn c = conn;
        Log.i(TAG, "GamePad: options arrow tapped" + (c == null ? " (no connection)" : ""));
        if (c != null) c.post(PadLink.OPTIONS, new byte[0]);
        controls.performHapticFeedback(android.view.HapticFeedbackConstants.VIRTUAL_KEY);
        return true;
    }

    // ---- whether the picture can be seen here (PadLink.SHOWN): not while the app is away or the screen off, nor
    // under a console dialog (it covers all of it); the console encodes none meanwhile
    private boolean started;
    private PadLink.Conn shownConn;
    private boolean shownSent;

    @Override
    protected void onStart() {
        super.onStart();
        started = true;
        sendShown();
        if (conn == null && tv == null && !choosing)  // back in front while waiting: a device found meanwhile is joined now
            for (Tv t : new ArrayList<>(found)) maybeAutoJoin(t);
    }

    @Override
    protected void onStop() {
        started = false;
        sendShown();
        super.onStop();
    }

    private void sendShown() {
        PadLink.Conn c = conn;
        boolean shown = started && !uiShown();
        if (c == null || (c == shownConn && shown == shownSent)) return;
        // (always said on a new connection too: one made while this was away, then shown, lost the "shown" once)
        shownConn = c;
        shownSent = shown;
        Log.i(TAG, "GamePad: the picture " + (shown ? "is seen" : "can't be seen") + " here");
        c.post(PadLink.SHOWN, PadLink.body(o -> o.writeBoolean(shown)));
    }

    @Override
    protected void onResume() {
        super.onResume();
        started = true;  // (again: a connection made in between gets it, sendShown)
        sendShown();
    }

    private boolean uiShown() {
        return (uiView != null && uiView.getVisibility() == View.VISIBLE) || (uiDraw != null && uiDraw.showing());
    }

    /** where the picture lies in the view (fit center), as u, v 0..1 of the dialog's window there */
    private boolean uiTouch(MotionEvent e) {
        PadLink.Conn c = conn;
        if (c == null) return true;
        int a = e.getActionMasked();
        if (a != MotionEvent.ACTION_DOWN && a != MotionEvent.ACTION_MOVE && a != MotionEvent.ACTION_UP && a != MotionEvent.ACTION_CANCEL) return true;
        float bw, bh, k, ox, oy;
        if (uiDraw != null && uiDraw.showing()) {
            float[] ft = uiDraw.fit();
            bw = uiDraw.dialogW();
            bh = uiDraw.dialogH();
            k = ft[0];
            ox = ft[1];
            oy = ft[2];
        } else {
            android.graphics.drawable.Drawable d = uiView.getDrawable();
            if (d == null) return true;
            bw = d.getIntrinsicWidth();
            bh = d.getIntrinsicHeight();
            float vw = uiView.getWidth(), vh = uiView.getHeight();
            k = Math.min(vw / bw, vh / bh);
            ox = (vw - bw * k) / 2;
            oy = (vh - bh * k) / 2;
        }
        float u = Math.max(0, Math.min(1, (e.getX() - ox) / (bw * k))), v = Math.max(0, Math.min(1, (e.getY() - oy) / (bh * k)));
        c.post(PadLink.UI_TOUCH, PadLink.body(o -> {
            o.writeInt(a);
            o.writeFloat(u);
            o.writeFloat(v);
        }));
        return true;
    }

    @Override
    public void onBackPressed() {
        if (uiShown()) {  // the device's Back: the console's dialog goes back (as Back there)
            long now = android.os.SystemClock.uptimeMillis();
            sendKey(new KeyEvent(now, now, KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_BACK, 0));
            sendKey(new KeyEvent(now, now, KeyEvent.ACTION_UP, KeyEvent.KEYCODE_BACK, 0));
            return;
        }
        if (choosing) {  // the start page: Back leaves the app
            finishAndRemoveTask();
            return;
        }
        if (conn != null || tv != null) openMenu();
        else leaveSearch();
    }

    /** Back on the search page: asks first. A controller's B is Android's Back, so a B pressed out of habit
     *  left the page, and a second one (on the start page) the app; "Keep looking" is the preselected button */
    private void leaveSearch() {
        new GameDialog(this).title(screenRole() ? R.string.pad_screen_title : R.string.pad_title)
                .message(RECEIVER ? R.string.receiver_leave : R.string.pad_leave_search)
                .button(RECEIVER ? R.string.receiver_close : R.string.pad_back_to_game, this::leaveSearchNow)
                .button(R.string.pad_keep_looking, null).show();
    }

    // ---- the picture: H.264 from the TV, decoded onto the surface
    private void startDecoderLocked() {
        if (decoder != null || pictureSurface == null || videoConfig == null) return;
        try {
            MediaFormat f = MediaFormat.createVideoFormat(MediaFormat.MIMETYPE_VIDEO_AVC, videoW, videoH);
            f.setInteger(MediaFormat.KEY_LOW_LATENCY, 1);
            f.setInteger(MediaFormat.KEY_PRIORITY, 0);
            decoder = MediaCodec.createDecoderByType(MediaFormat.MIMETYPE_VIDEO_AVC);
            decoder.configure(f, pictureSurface, null, 0);
            decoder.start();
            final MediaCodec d = decoder;
            new Thread(() -> showDecoded(d), "wwhd-pad-decoded").start();
            int i = decoder.dequeueInputBuffer(100_000);
            if (i >= 0) {
                java.nio.ByteBuffer b = decoder.getInputBuffer(i);
                b.clear();
                b.put(videoConfig);
                decoder.queueInputBuffer(i, 0, videoConfig.length, 0, MediaCodec.BUFFER_FLAG_CODEC_CONFIG);
            }
            needKey = true;
            requestKey();
        } catch (IOException | RuntimeException e) {
            Log.w(TAG, "GamePad: no video decoder", e);
            releaseDecoderLocked();
        }
    }

    private void releaseDecoder() {
        synchronized (decoderLock) {
            releaseDecoderLocked();
        }
    }

    private void releaseDecoderLocked() {
        if (decoder == null) return;
        try {
            decoder.stop();
        } catch (IllegalStateException ignored) {
        }
        decoder.release();
        decoder = null;
    }

    private void requestKey() {
        PadLink.Conn c = conn;
        if (c != null) c.post(PadLink.KEYFRAME, new byte[0]);
    }

    /**
     * Each decoded picture shown the moment it is ready (its own thread: waiting for the next frame's arrival to
     * collect it cost up to a frame), and its delay noted: from when the game handed it over on the other device
     * (its presentation time, that device's clock) to now, by the clocks' offset the pings found.
     */
    private void showDecoded(MediaCodec d) {
        MediaCodec.BufferInfo info = new MediaCodec.BufferInfo();
        while (decoder == d) {
            try {
                int o = d.dequeueOutputBuffer(info, 20_000);
                if (o < 0) continue;
                long now = System.nanoTime();
                boolean timed = bestRtt != Long.MAX_VALUE && info.presentationTimeUs > 0;
                if (timed) pictureDelays.add(((now + clockOffset) / 1000 - info.presentationTimeUs) / 1000);
                if (lastPts > 0) {  // the game's own steps between the pictures that came (for the log)
                    long step = (info.presentationTimeUs - lastPts + 8_000) / 16_667;  // in 60 Hz frames
                    ptsSteps[(int) Math.max(0, Math.min(ptsSteps.length - 1, step))]++;
                }
                lastPts = info.presentationTimeUs;
                // the screen device: each picture at its game time + a steady delay (a little over the slowest tenth's),
                // so the pictures come evenly as the game made them (shown at once they came as the network brought
                // them: some close together, some late, the picture uneven); the GamePad device: at once (touch)
                long when = 0;
                if (timed && screenRole()) {
                    if (++paceCount % 30 == 1) {
                        long[] p = pictureDelays.get();
                        // (the slowest tenth and as much again as they are behind the middle: few come later)
                        if (p[2] >= 30) paceDelayNs = Math.min(p[1] + Math.max(8, p[1] - p[0]), p[0] + 50) * 1_000_000L;
                    }
                    if (paceDelayNs > 0) when = info.presentationTimeUs * 1000 - clockOffset + paceDelayNs;
                    // in a steady rhythm: the game hands its pictures over a few ms early or late (its own screen
                    // shows them at its display's beats all the same); near the beat the last one gave: on it
                    long step = info.presentationTimeUs * 1000 - lastPtsNs;
                    if (lastPtsNs > 0 && step > 5_000_000L && step < 100_000_000L)
                        paceStepNs = paceStepNs == 0 ? step : paceStepNs + (step - paceStepNs) / 16;
                    lastPtsNs = info.presentationTimeUs * 1000;
                    if (when > 0 && lastWhen > 0 && paceStepNs > 0) {
                        long beat = lastWhen + Math.round((double) (when - lastWhen) / paceStepNs) * paceStepNs;
                        if (Math.abs(when - beat) < 12_000_000L && beat > lastWhen) when = beat;
                    }
                    if (when > 0) lastWhen = when;
                }
                if (when > now && when - now < 150_000_000L) d.releaseOutputBuffer(o, when);
                else d.releaseOutputBuffer(o, true);
            } catch (IllegalStateException e) {
                return;  // (released meanwhile)
            }
        }
    }

    // ---- the delays (PadLink.Delays): pings each second; written to the log every 10 s and shown in the menu
    private final PadLink.Delays rtts = new PadLink.Delays(30), pictureDelays = new PadLink.Delays(300), soundDelays = new PadLink.Delays(100);
    private volatile long bestRtt = Long.MAX_VALUE, clockOffset;
    private long paceDelayNs, paceCount, lastPts, lastPtsNs, paceStepNs, lastWhen;
    private final int[] ptsSteps = new int[8];  // the pictures' game-time steps (0..7 frames at 60 Hz), for the log  // showDecoded: the screen device's steady picture delay
    private long lastDelayLog;
    private final Runnable ping = new Runnable() {
        @Override
        public void run() {
            PadLink.Conn c = conn;
            if (c == null || isFinishing()) return;
            long t = System.nanoTime();
            c.post(PadLink.PING, PadLink.body(o -> o.writeLong(t)));
            if (android.os.SystemClock.uptimeMillis() - lastDelayLog > 10000) {
                lastDelayLog = android.os.SystemClock.uptimeMillis();
                Log.i(TAG, "GamePad: delays " + delaysText() + "; picture steps in 60 Hz frames " + java.util.Arrays.toString(ptsSteps));
                java.util.Arrays.fill(ptsSteps, 0);
            }
            send.postDelayed(this, 1000);
        }
    };

    /** for the log: medians and 90th percentiles */
    private String delaysText() {
        long[] r = rtts.get(), p = pictureDelays.get(), s = soundDelays.get();
        return "network round trip " + r[0] + " ms (90%: " + r[1] + "), picture " + p[0] + " ms (90%: " + p[1] + ", " + p[2]
                + " pictures" + (paceDelayNs > 0 ? ", shown evenly at " + paceDelayNs / 1_000_000 + " ms" : "") + "), sound " + (s[0] < 0 ? "-" : (s[0] + r[0] / 2) + " ms");
    }

    private void decode(PadLink.Conn c, PadLink.Frame f) throws IOException {
        long pts = f.in.readLong();
        boolean key = f.in.readBoolean();
        int n = f.data.length - 9;
        synchronized (decoderLock) {
            if (decoder == null) return;
            if (needKey && !key) return;  // until a full picture: nothing to build on
            try {
                int i = decoder.dequeueInputBuffer(20_000);
                if (i < 0) {  // the decoder is behind: this frame is lost, so the next full one is needed
                    needKey = true;
                    requestKey();
                    return;
                }
                needKey = false;
                java.nio.ByteBuffer b = decoder.getInputBuffer(i);
                b.clear();
                b.put(f.data, 9, n);
                decoder.queueInputBuffer(i, 0, n, pts, 0);
            } catch (IllegalStateException e) {
                releaseDecoderLocked();
                startDecoderLocked();
            }
        }
    }

    // ---- input to the TV
    private int lastButtons, lastRaw, swallowed;
    private float lastLx, lastLy, lastRx, lastRy;

    private void sendInput() {
        PadLink.Conn c = conn;
        if (c == null || controls == null) return;
        int raw = controls.buttons | mapper.padButtons | mapper.keyButtons;
        // an item chosen on the drawn items page: X / Y / R pressed put it on that button (as the game's GamePad: tap
        // an item, press the button), the press goes no further
        int fresh = raw & ~lastRaw;
        lastRaw = raw;
        if (fresh != 0 && drawnNow() && padScreen != null && padScreen.choosing()) {
            int[][] keys = {{Native.X, 0}, {Native.Y, 1}, {Native.R, 2}};
            for (int[] k : keys) if ((fresh & k[0]) != 0 && padScreen.equipFromButton(k[1])) swallowed |= k[0];
        }
        swallowed &= raw;
        int b = raw & ~swallowed;
        float lx = pick(controls.stick[0], mapper.lx, mapper.keyLX()), ly = pick(controls.stick[1], mapper.ly, mapper.keyLY());
        float rx = pick(controls.stick[2], mapper.rx, mapper.keyRX()), ry = pick(controls.stick[3], mapper.ry, mapper.keyRY());
        if (b == lastButtons && lx == lastLx && ly == lastLy && rx == lastRx && ry == lastRy) return;
        lastButtons = b;
        lastLx = lx;
        lastLy = ly;
        lastRx = rx;
        lastRy = ry;
        byte[] m = PadLink.body(o -> {
            o.writeInt(b);
            o.writeFloat(lx);
            o.writeFloat(ly);
            o.writeFloat(rx);
            o.writeFloat(ry);
        });
        c.post(PadLink.INPUT, m);
    }

    private static float pick(float a, float b, float c) {
        float v = a;
        if (Math.abs(b) > Math.abs(v)) v = b;
        if (Math.abs(c) > Math.abs(v)) v = c;
        return v;
    }

    private void sendKey(KeyEvent e) {
        PadLink.Conn c = conn;
        if (c == null) return;
        byte[] m = PadLink.body(o -> {
            o.writeInt(e.getAction());
            o.writeInt(e.getKeyCode());
            o.writeInt(e.getRepeatCount());
        });
        c.post(PadLink.KEY, m);
    }

    private void sendTouch(boolean down, float u, float v) {
        PadLink.Conn c = conn;
        if (c == null) return;
        byte[] m = PadLink.body(o -> {
            o.writeBoolean(down);
            o.writeFloat(u);
            o.writeFloat(v);
        });
        c.post(PadLink.TOUCH, m);
    }

    private void startMotion() {
        if (motion != null) return;
        motion = new MotionInput(this, (dt, gx, gy, gz, ax, ay, az) -> {
            PadLink.Conn c = conn;
            if (c != null) c.post(PadLink.MOTION, PadLink.body(o -> {
                o.writeFloat(dt);
                o.writeFloat(gx);
                o.writeFloat(gy);
                o.writeFloat(gz);
                o.writeFloat(ax);
                o.writeFloat(ay);
                o.writeFloat(az);
            }));
        });
        motion.start(null);
    }

    @Override
    public boolean dispatchKeyEvent(KeyEvent e) {
        int code = e.getKeyCode();
        // a controller's own Back (some send it for View / Create, a Tegra X1 TV box's DualSense too): its − (Select), as
        // InputMapper's; the remote's Back (and a B's fallback Back) stays this app's
        if (code == KeyEvent.KEYCODE_BACK && conn != null && InputMapper.isController(e.getDevice())
                && (e.getFlags() & KeyEvent.FLAG_FALLBACK) == 0) {
            e = new KeyEvent(e.getDownTime(), e.getEventTime(), e.getAction(), KeyEvent.KEYCODE_BUTTON_SELECT, e.getRepeatCount(),
                    e.getMetaState(), e.getDeviceId(), e.getScanCode(), e.getFlags(), e.getSource());
            code = KeyEvent.KEYCODE_BUTTON_SELECT;
        }
        boolean controller = InputMapper.isController(e.getDevice()) || KeyEvent.isGamepadButton(code);
        if (e.getRepeatCount() == 0 && (code == KeyEvent.KEYCODE_BUTTON_SELECT || code == KeyEvent.KEYCODE_BACK))
            Log.i(TAG, "GamePad device: key " + KeyEvent.keyCodeToString(code) + (e.getAction() == KeyEvent.ACTION_DOWN ? " down" : " up")
                    + " (controller " + controller + ", connected " + (conn != null) + ", controls " + (controls != null) + ", dialog " + uiShown() + ")");
        if (conn == null && tv == null && controller) {  // the search page: a controller's A / B as in the app's menus
            KeyEvent k = InputMapper.pageKey(prefs, e, true);
            if (k != e) return k == null || super.dispatchKeyEvent(k);
        }
        boolean dpad = code >= KeyEvent.KEYCODE_DPAD_UP && code <= KeyEvent.KEYCODE_DPAD_CENTER;
        if (uiShown() && !controller && (dpad || code == KeyEvent.KEYCODE_ENTER)) {  // a keyboard's or remote's on the dialog
            sendKey(code == KeyEvent.KEYCODE_ENTER ? new KeyEvent(e.getAction(), KeyEvent.KEYCODE_DPAD_CENTER) : e);
            return true;
        }
        if (controls != null && controller && code != KeyEvent.KEYCODE_BACK) {
            // (a screen device's controller too: it plays the game there as one connected to the console)
            // to the console too: its options and dialogs take a controller's keys (MainActivity.padKey); Home only
            // goes there (it opens the options, as a controller's Home on the console does)
            sendKey(e);
            if (code == KeyEvent.KEYCODE_BUTTON_MODE) return true;
        }
        if (controls != null && controller && code != KeyEvent.KEYCODE_BACK && mapper.onKey(e)) {
            controls.usedController();
            sendInput();
            return true;
        }
        return super.dispatchKeyEvent(e);
    }

    private volatile int uiDir, uiRepeats;  // the d-pad way a controller's hat / stick holds on the console's dialog here
    private final Runnable uiRepeat = new Runnable() {
        @Override public void run() {
            if (uiDir == 0) return;
            long now = android.os.SystemClock.uptimeMillis();
            sendKey(new KeyEvent(now, now, KeyEvent.ACTION_DOWN, uiDir, ++uiRepeats));
            send.postDelayed(this, 120);
        }
    };

    @Override
    public boolean dispatchGenericMotionEvent(MotionEvent e) {
        // the console's dialog here (its options...): a controller's hat or left stick moves its cursor, as d-pad keys
        // (a hat is no key on many controllers, the DualSense's too); its buttons go as keys already
        if (uiShown() && (e.getSource() & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK
                && e.getActionMasked() == MotionEvent.ACTION_MOVE) {
            float x = e.getAxisValue(MotionEvent.AXIS_HAT_X), y = e.getAxisValue(MotionEvent.AXIS_HAT_Y);
            if (Math.abs(x) < 0.5f && Math.abs(y) < 0.5f) {
                x = e.getAxisValue(MotionEvent.AXIS_X);
                y = e.getAxisValue(MotionEvent.AXIS_Y);
            }
            float lim = uiDir != 0 ? 0.35f : 0.6f;  // (a little hold: no flutter at the edge)
            int dir = Math.abs(x) >= Math.abs(y) ? (x > lim ? KeyEvent.KEYCODE_DPAD_RIGHT : x < -lim ? KeyEvent.KEYCODE_DPAD_LEFT : 0)
                    : (y > lim ? KeyEvent.KEYCODE_DPAD_DOWN : y < -lim ? KeyEvent.KEYCODE_DPAD_UP : 0);
            if (dir != uiDir) {
                send.removeCallbacks(uiRepeat);
                if (uiDir != 0) sendKey(new KeyEvent(KeyEvent.ACTION_UP, uiDir));
                uiDir = dir;
                uiRepeats = 0;
                if (dir != 0) {
                    sendKey(new KeyEvent(KeyEvent.ACTION_DOWN, dir));
                    send.postDelayed(uiRepeat, 400);
                }
            }
            return true;
        }
        if (uiDir != 0) {  // (the dialog went while held)
            send.removeCallbacks(uiRepeat);
            sendKey(new KeyEvent(KeyEvent.ACTION_UP, uiDir));
            uiDir = 0;
        }
        if (controls != null && mapper.onMotion(e)) {
            if (mapper.motionActive) controls.usedController();
            sendInput();
            return true;
        }
        return super.dispatchGenericMotionEvent(e);
    }

    /**
     * The touch buttons around and over the picture, in the GamePad's places: L / ZL and the left stick
     * with the d-pad below on the left, R / ZR, A B X Y and the right stick on the right, − Menu ▭ + at the
     * top. A double tap on a stick holds its click. Two views without a controller: the controller view
     * (default: the picture dimmed, every touch is the buttons', one beside them on the left half is the
     * left stick where it lands, on the right half the right stick; nothing reaches the GamePad's touch
     * screen, where the inventory would take any touch as an item) and the touch screen view (▭: only
     * ☰ and ▭ stay, a touch on the picture is the GamePad's touch screen). Hidden while a controller is
     * used, the touch screen view then (a phone in a controller clamp); a touch brings them back only when
     * no controller is connected any more, and so does the controller's disconnecting.
     */
    private final class Controls extends View {
        boolean touchButtons;
        private boolean hiddenForController;
        private boolean screenTouch;  // the touch screen view (▭)
        int buttons;
        final float[] stick = new float[4];

        private static final int K_BUTTON = 0, K_STICK = 1, K_DPAD = 2, K_MENU = 3, K_SCREEN = 4;

        private final class Ctl {
            final int kind, bit;
            final String label;
            float x, y, r;  // middle and radius (px)
            float w, h;     // a rounded box instead of a circle (the eyes-free layout's shoulder buttons; 0: a circle)
            float homeX, homeY;  // a stick's own place (it follows a touch beside it in the controller view)
            int pointer = -1;
            float kx, ky;   // a stick's knob offset (-1..1)
            long lastUp;
            boolean clickHeld;

            Ctl(int kind, int bit, String label) {
                this.kind = kind;
                this.bit = bit;
                this.label = label;
            }
        }

        private final List<Ctl> ctls = new ArrayList<>();
        private final Ctl lStick = new Ctl(K_STICK, Native.STICK_L, ""), rStick = new Ctl(K_STICK, Native.STICK_R, "");
        private final Ctl dpad = new Ctl(K_DPAD, 0, "");
        private final Paint fill = new Paint(Paint.ANTI_ALIAS_FLAG), line = new Paint(Paint.ANTI_ALIAS_FLAG),
                text = new Paint(Paint.ANTI_ALIAS_FLAG);
        private int touchPointer = -1;  // the finger on the GamePad's touch screen

        Controls() {
            super(PadActivity.this);
            ctls.add(new Ctl(K_BUTTON, Native.ZL, "ZL"));
            ctls.add(new Ctl(K_BUTTON, Native.L, "L"));
            ctls.add(lStick);
            ctls.add(dpad);
            ctls.add(new Ctl(K_BUTTON, Native.ZR, "ZR"));
            ctls.add(new Ctl(K_BUTTON, Native.R, "R"));
            ctls.add(new Ctl(K_BUTTON, Native.A, "A"));
            ctls.add(new Ctl(K_BUTTON, Native.B, "B"));
            ctls.add(new Ctl(K_BUTTON, Native.X, "X"));
            ctls.add(new Ctl(K_BUTTON, Native.Y, "Y"));
            ctls.add(rStick);
            ctls.add(new Ctl(K_BUTTON, Native.MINUS, "−"));
            ctls.add(new Ctl(K_MENU, 0, "☰"));
            ctls.add(new Ctl(K_BUTTON, Native.PLUS, "+"));
            ctls.add(new Ctl(K_SCREEN, 0, ""));
            ctls.add(new Ctl(K_BUTTON, Native.STICK_L, "L3"));  // 15, 16: the eyes-free layout's stick clicks
            ctls.add(new Ctl(K_BUTTON, Native.STICK_R, "R3"));
            line.setStyle(Paint.Style.STROKE);
            text.setTextAlign(Paint.Align.CENTER);
            text.setFakeBoldText(true);
        }

        void usedController() {
            if (hiddenForController) return;
            hiddenForController = true;
            invalidate();
        }

        private boolean shown() { return touchButtons && !hiddenForController; }

        /** all buttons, no touches to the picture */
        private boolean controllerView() { return shown() && !screenTouch; }

        void controllersChanged() {
            if (hiddenForController && MainActivity.firstController() == null) {
                hiddenForController = false;
                invalidate();
            }
        }

        @Override
        protected void onSizeChanged(int w, int h, int ow, int oh) {
            laidBlind = blind();
            layout(w, h);
        }

        private void layout(int w, int h) {
            for (Ctl c : ctls) c.w = c.h = 0;
            if (laidBlind) {
                layoutBlind(w, h);
                return;
            }
            float u = h / 100f;
            place(0, 12, 10, 7);   // ZL
            place(1, 12, 27, 7);   // L
            place(2, 17, 60, 14);  // left stick
            place(3, 37, 85, 11);  // d-pad
            placeR(4, 12, 10, 7);  // ZR
            placeR(5, 12, 27, 7);  // R
            placeR(6, 8, 60, 6.5f);   // A (right)
            placeR(7, 19, 72, 6.5f);  // B (bottom)
            placeR(8, 19, 48, 6.5f);  // X (top)
            placeR(9, 30, 60, 6.5f);  // Y (left)
            placeR(10, 37, 85, 11);   // right stick
            Ctl minus = ctls.get(11), menu = ctls.get(12), plus = ctls.get(13), screen = ctls.get(14);
            minus.x = w / 2f - 21 * u;
            menu.x = w / 2f - 7 * u;
            screen.x = w / 2f + 7 * u;
            plus.x = w / 2f + 21 * u;
            minus.y = menu.y = plus.y = screen.y = 7 * u;
            minus.r = menu.r = plus.r = screen.r = 5 * u;
            homes(u);
        }

        private void homes(float u) {
            Ctl screen = ctls.get(14);
            bgX = screen.x;  // the shortcut button below the screen button (controller view)
            bgY = screen.y + screen.r * 2.3f;
            bgR = screen.r * 0.85f;
            for (Ctl c : ctls) {
                c.homeX = c.x;
                c.homeY = c.y;
            }
            text.setTextSize(5 * u);
            line.setStrokeWidth(Math.max(2, 0.5f * u));
        }

        // ---- the eyes-free layout (the console's Controls > Touch > Eyes-free controller view, the same option: the
        // same places as there, ControlsView.layoutBlind): ZL / ZR in the top corners, L / R below them on the edges,
        // the d-pad in the bottom left corner, − ☰ ▭ + at the top middle, L3 / R3 at the bottom middle, the face
        // buttons a zone on the right (a swipe from where the thumb lands, or the nearest of the four), every button
        // its own vibration; elsewhere the floating sticks
        private boolean laidBlind;
        private float faceX, faceY, faceZone, faceU;
        private int facePtr = -1, faceBit, pulseBits;
        private float faceHx, faceHy;
        private boolean faceAHeld;
        private final Runnable faceHoldA = () -> {
            if (facePtr < 0 || faceBit != 0) return;
            faceAHeld = true;
            facePick(Native.A);
        };

        private boolean blind() { return controllerView() && prefs.getBoolean("controls_blind", true); }

        private boolean faceSwipe() { return laidBlind && prefs.getInt("controls_blind_face", 1) == 1; }

        private void layoutBlind(int W, int H) {
            float u = H / 100f;
            float cw = 24 * u, ch = 11 * u, lh = 9 * u, gap = u;
            box(0, cw / 2, ch / 2, cw, ch);                    // ZL: the top left corner
            box(1, cw / 2, ch + gap + lh / 2, cw, lh);         // L: below it, on the edge
            box(4, W - cw / 2, ch / 2, cw, ch);                // ZR
            box(5, W - cw / 2, ch + gap + lh / 2, cw, lh);     // R
            place(3, 10, 90, 9);                               // d-pad: the bottom left corner
            place(2, 25, 60, 11);                              // (floating sticks: where a thumb lands)
            placeR(10, 25, 60, 11);
            float d = 10 * u, r = 8 * u;
            faceU = 10 * u;
            faceX = W - 19 * u;
            faceY = H * 0.57f;
            faceZone = d + r + 2.5f * u;
            Ctl a = ctls.get(6), b = ctls.get(7), x = ctls.get(8), y = ctls.get(9);
            a.x = faceX + d; a.y = faceY;
            b.x = faceX; b.y = faceY + d;
            x.x = faceX; x.y = faceY - d;
            y.x = faceX - d; y.y = faceY;
            a.r = b.r = x.r = y.r = r;
            Ctl minus = ctls.get(11), menu = ctls.get(12), plus = ctls.get(13), screen = ctls.get(14);
            minus.x = W / 2f - 19 * u;
            menu.x = W / 2f - 6.5f * u;
            screen.x = W / 2f + 6.5f * u;
            plus.x = W / 2f + 19 * u;
            minus.y = menu.y = plus.y = screen.y = 7 * u;
            minus.r = menu.r = plus.r = screen.r = 5 * u;
            Ctl l3 = ctls.get(15), r3 = ctls.get(16);
            l3.x = W / 2f - 9 * u;
            r3.x = W / 2f + 9 * u;
            l3.y = r3.y = H - 7 * u;
            l3.r = r3.r = 5 * u;
            homes(u);
        }

        private void box(int i, float x, float y, float w, float h) {
            Ctl c = ctls.get(i);
            c.x = x;
            c.y = y;
            c.w = w;
            c.h = h;
            c.r = Math.min(w, h) / 2;
        }

        /** the layout follows the view: eyes-free or not (the screen button, a controller, the option) */
        private void checkLayout() {
            boolean b = blind();
            if (b == laidBlind) return;
            releaseAll();
            laidBlind = b;
            layout(getWidth(), getHeight());
            recompute();
        }

        private void releaseAll() {
            for (Ctl c : ctls) {
                c.pointer = -1;
                c.kx = c.ky = 0;
                c.clickHeld = false;
            }
            faceRelease();
            bgPtr = -1;
            removeCallbacks(bgStep);
        }

        private void facePick(int bit) {
            if (bit == faceBit) return;
            faceBit = bit;
            buzz(bit, false);
            recompute();
        }

        private void faceRelease() {
            removeCallbacks(faceHoldA);
            facePtr = -1;
            faceBit = 0;
            faceAHeld = false;
        }

        private boolean inFaceZone(float x, float y) { return laidBlind && Math.hypot(x - faceX, y - faceY) <= faceZone; }

        /** the swipe's own touches: true when the event is used up */
        private boolean faceTouch(MotionEvent e, int action, int idx) {
            if (action == MotionEvent.ACTION_CANCEL) {
                if (facePtr >= 0) faceRelease();
                return false;
            }
            if (action == MotionEvent.ACTION_DOWN || action == MotionEvent.ACTION_POINTER_DOWN) {
                float x = e.getX(idx), y = e.getY(idx);
                if (facePtr >= 0 || !faceSwipe() || !inFaceZone(x, y)) return false;
                Ctl other = at(x, y);
                if (other != null && !isFace(other)) return false;  // another button right there keeps its touch
                facePtr = e.getPointerId(idx);
                faceHx = x;
                faceHy = y;
                faceBit = 0;
                faceAHeld = false;
                postDelayed(faceHoldA, 220);
                invalidate();
                return true;
            }
            if (facePtr < 0) return false;
            if (action == MotionEvent.ACTION_MOVE) {
                int pi = e.findPointerIndex(facePtr);
                if (pi < 0) return false;
                float dx = e.getX(pi) - faceHx, dy = e.getY(pi) - faceHy;
                if (Math.hypot(dx, dy) >= faceU * 0.3f) {
                    removeCallbacks(faceHoldA);
                    facePick(Math.abs(dx) > Math.abs(dy) ? (dx > 0 ? Native.A : Native.Y) : (dy < 0 ? Native.X : Native.B));
                }
                return false;
            }
            if ((action == MotionEvent.ACTION_UP || action == MotionEvent.ACTION_POINTER_UP) && e.getPointerId(idx) == facePtr) {
                boolean tap = faceBit == 0;
                faceRelease();
                if (tap) {  // a tap: A
                    buzz(Native.A, false);
                    pulseBits |= Native.A;
                    postDelayed(() -> {
                        pulseBits &= ~Native.A;
                        recompute();
                    }, 80);
                }
                recompute();
                return true;
            }
            return false;
        }

        private boolean isFace(Ctl c) {
            return c.kind == K_BUTTON && (c.bit == Native.A || c.bit == Native.B || c.bit == Native.X || c.bit == Native.Y);
        }

        private android.os.Vibrator vibrator;

        /** a button's own vibration (eyes-free layout, as on the console): A one short, B two short, X one long,
         *  Y long and short, the shoulder buttons a light tick, the d-pad a tick, the others three ticks */
        private void buzz(int bit, boolean dpad) {
            if (!laidBlind) return;
            if (vibrator == null) vibrator = (android.os.Vibrator) getSystemService(VIBRATOR_SERVICE);
            if (vibrator == null || !vibrator.hasVibrator()) return;
            long[] w;
            if (dpad) w = new long[] {0, 10};
            else if (bit == Native.A) w = new long[] {0, 18};
            else if (bit == Native.B) w = new long[] {0, 14, 60, 14};
            else if (bit == Native.X) w = new long[] {0, 45};
            else if (bit == Native.Y) w = new long[] {0, 40, 60, 14};
            else if (bit == Native.ZL || bit == Native.ZR || bit == Native.L || bit == Native.R) w = new long[] {0, 7};
            else w = new long[] {0, 8, 40, 8, 40, 8};
            try {
                vibrator.vibrate(android.os.VibrationEffect.createWaveform(w, -1));
            } catch (RuntimeException ignored) {
            }
        }

        // ---- the TV picture behind the buttons (the console's Controls > Touch > TV picture in the controller view,
        // the same option): the console sends the TV picture instead of the GamePad's while the controller view is
        // on (PadLink.VIEW); the shortcut below ▭ switches it for now, a long press steps its brightness
        private Boolean bgTv;
        private float bgX, bgY, bgR;
        private int bgPtr = -1;
        private boolean bgStepped;
        private long bgLevelShownUntil;
        private PadLink.Conn viewConn;
        private boolean viewSent;
        private final Runnable bgStep = new Runnable() {
            @Override
            public void run() {
                if (bgPtr < 0) return;
                bgStepped = true;
                int now = tvLevel(), next = !bgTvNow() ? now : now >= 75 ? 25 : now + 25;
                prefs.edit().putInt("controls_tv_backdrop", next).apply();
                bgTv = true;
                performHapticFeedback(android.view.HapticFeedbackConstants.LONG_PRESS);
                bgLevelShownUntil = android.os.SystemClock.uptimeMillis() + 1200;
                invalidate();
                postInvalidateDelayed(1250);
                postDelayed(this, 700);
            }
        };

        private int tvOption() { return prefs.getInt("controls_tv_backdrop", 50); }

        private int tvLevel() { int l = tvOption(); return l > 0 ? l : 50; }

        private boolean bgTvNow() { return bgTv != null ? bgTv : tvOption() > 0; }

        private boolean bgButton() { return controllerView() && picture() > 0; }

        /** the TV picture is the background now */
        boolean tvBack() { return bgButton() && bgTvNow(); }

        /** tell the console which picture to send (again on a new connection) */
        private void sendView() {
            PadLink.Conn c = conn;
            boolean want = tvBack();
            if (c == null || (c == viewConn && want == viewSent)) return;
            viewConn = c;
            viewSent = want;
            c.post(PadLink.VIEW, PadLink.body(o -> o.writeBoolean(want)));
        }

        private void drawBgButton(Canvas cv) {
            if (!bgButton()) return;
            float r = bgR, x = bgX, y = bgY;
            fill.setColor(Color.argb(bgPtr >= 0 ? 160 : 70, 255, 255, 255));
            cv.drawCircle(x, y, r, fill);
            line.setColor(Color.argb(150, 255, 255, 255));
            cv.drawCircle(x, y, r, line);
            float sw = line.getStrokeWidth();
            line.setStrokeWidth(Math.max(1.5f, sw * 0.7f));
            line.setColor(Color.argb(230, 255, 255, 255));
            android.graphics.RectF t = new android.graphics.RectF();
            if (bgTvNow()) {  // a tap shows the GamePad's: a GamePad
                float bw = r * 1.3f, bh = r * 0.75f;
                t.set(x - bw / 2, y - bh / 2, x + bw / 2, y + bh / 2);
                cv.drawRoundRect(t, bh * 0.3f, bh * 0.3f, line);
                float iw = bw * 0.46f, ih = bh * 0.56f;
                cv.drawRect(x - iw / 2, y - ih / 2, x + iw / 2, y + ih / 2, line);
            } else {  // a tap shows the TV's: a TV
                float tw = r * 1.1f, th = r * 0.7f, top = y - r * 0.45f;
                t.set(x - tw / 2, top, x + tw / 2, top + th);
                cv.drawRoundRect(t, th * 0.1f, th * 0.1f, line);
                cv.drawLine(x, top + th, x, top + th + r * 0.2f, line);
                cv.drawLine(x - tw * 0.25f, top + th + r * 0.2f, x + tw * 0.25f, top + th + r * 0.2f, line);
            }
            line.setStrokeWidth(sw);
            if (android.os.SystemClock.uptimeMillis() < bgLevelShownUntil) {  // the step just taken, beside it
                String l = tvLevel() + " %";
                float tx = x + r * 2.6f;
                fill.setColor(Color.argb(176, 0, 0, 0));
                float hw = text.measureText("75 %") / 2 + r * 0.3f;
                cv.drawRoundRect(tx - hw, y - r * 0.8f, tx + hw, y + r * 0.8f, r * 0.3f, r * 0.3f, fill);
                text.setColor(Color.WHITE);
                cv.drawText(l, tx, y + text.getTextSize() * 0.35f, text);
            }
        }

        // ---- the wind dial (the console's Mini map > Wind arrow > Change the wind (dial)): the way the wind blows, turned
        // as the TV's mini map (PadLink.WIND_STATE); a swipe from it sets the wind that way at once (one of 8, shown on the
        // dial while the finger is down; PadLink.WIND). Only in the controller view, while the console says there is a wind
        private boolean windOn;
        private float windWay, windTurn, windX, windY, windR;
        private int windPtr = -1, windSwipe = Integer.MIN_VALUE;
        private float windDownX, windDownY;
        private final android.graphics.Path windArrow = new android.graphics.Path();

        void setWind(boolean on, float way, float turn) {
            if (on == windOn && way == windWay && turn == windTurn) return;
            windOn = on;
            windWay = way;
            windTurn = turn;
            if (!on && windPtr >= 0) {
                windPtr = -1;
                windSwipe = Integer.MIN_VALUE;
            }
            invalidate();
        }

        private boolean windShown() { return windOn && controllerView(); }

        private void placeWind() {
            float u = getHeight() / 100f;
            windX = getWidth() - (laidBlind ? 34 : 31) * u;
            windY = 12 * u;
            windR = 7 * u;
        }

        private void drawWind(Canvas cv) {
            if (!windShown()) return;
            placeWind();
            float x = windX, y = windY, r = windR;
            boolean swiping = windSwipe != Integer.MIN_VALUE;
            fill.setColor(Color.argb(swiping ? 200 : 150, 40, 28, 12));
            cv.drawCircle(x, y, r, fill);
            float sw = line.getStrokeWidth();
            line.setStrokeWidth(Math.max(1.5f, r * 0.07f));
            line.setColor(Color.argb(200, 255, 244, 216));
            cv.drawCircle(x, y, r, line);
            // the compass points, turned as the TV's map: N a small triangle outside the ring, the others ticks
            cv.save();
            cv.rotate(windTurn, x, y);
            for (int k = 0; k < 8; k++) {
                float t = (float) Math.toRadians(k * 45), dx = (float) Math.sin(t), dy = (float) -Math.cos(t);
                float in = k % 2 == 0 ? 0.78f : 0.86f;
                cv.drawLine(x + dx * r * in, y + dy * r * in, x + dx * r * 0.97f, y + dy * r * 0.97f, line);
            }
            windArrow.reset();
            windArrow.moveTo(x, y - r * 1.32f);
            windArrow.lineTo(x - r * 0.2f, y - r * 1.04f);
            windArrow.lineTo(x + r * 0.2f, y - r * 1.04f);
            windArrow.close();
            fill.setColor(Color.argb(230, 230, 70, 50));
            cv.drawPath(windArrow, fill);
            cv.restore();
            // the arrow the way it blows (MiniMap.drawCompass's turn: 180 - way + the map's turn; tip up before turning)
            float way = swiping ? PadLink.windWay(windSwipe) : windWay;
            float deg = 180f - (float) Math.toDegrees(way) + windTurn;
            cv.save();
            cv.rotate(deg, x, y);
            windArrow.reset();
            windArrow.moveTo(x, y - r * 0.8f);
            windArrow.lineTo(x + r * 0.38f, y - r * 0.18f);
            windArrow.lineTo(x + r * 0.14f, y - r * 0.18f);
            windArrow.lineTo(x + r * 0.14f, y + r * 0.72f);
            windArrow.lineTo(x - r * 0.14f, y + r * 0.72f);
            windArrow.lineTo(x - r * 0.14f, y - r * 0.18f);
            windArrow.lineTo(x - r * 0.38f, y - r * 0.18f);
            windArrow.close();
            fill.setColor(swiping ? Color.argb(255, 255, 214, 90) : Color.argb(255, 90, 160, 230));
            cv.drawPath(windArrow, fill);
            line.setColor(Color.argb(230, 255, 255, 255));
            line.setStrokeWidth(Math.max(1, r * 0.05f));
            cv.drawPath(windArrow, line);
            cv.restore();
            line.setStrokeWidth(sw);
        }

        /** a finger landing on the dial: the swipe's way while it moves, the wind set when it lets go (a tap: nothing) */
        private boolean windTouch(MotionEvent e, int action, int idx) {
            if (action == MotionEvent.ACTION_DOWN || action == MotionEvent.ACTION_POINTER_DOWN) {
                if (windPtr >= 0 || !windShown()) return false;
                placeWind();
                if (Math.hypot(e.getX(idx) - windX, e.getY(idx) - windY) > windR * 1.3f) return false;
                windPtr = e.getPointerId(idx);
                windDownX = e.getX(idx);
                windDownY = e.getY(idx);
                windSwipe = Integer.MIN_VALUE;
                return true;
            }
            if (windPtr < 0) return false;
            if (action == MotionEvent.ACTION_CANCEL) {
                windPtr = -1;
                windSwipe = Integer.MIN_VALUE;
                invalidate();
                return true;
            }
            int i = e.findPointerIndex(windPtr);
            if (i < 0) return false;
            float dx = e.getX(i) - windDownX, dy = e.getY(i) - windDownY;
            float slop = android.view.ViewConfiguration.get(getContext()).getScaledTouchSlop();
            if (Math.hypot(dx, dy) > slop) {
                int w = PadLink.windAngle(dx, dy, windTurn);
                if (w != windSwipe) {
                    if (windSwipe != Integer.MIN_VALUE) performHapticFeedback(android.view.HapticFeedbackConstants.CLOCK_TICK);
                    windSwipe = w;
                    invalidate();
                }
            }
            boolean up = (action == MotionEvent.ACTION_UP || action == MotionEvent.ACTION_POINTER_UP) && e.getPointerId(idx) == windPtr;
            if (up) {
                int w = windSwipe;
                windPtr = -1;
                windSwipe = Integer.MIN_VALUE;
                PadLink.Conn c = conn;
                if (w != Integer.MIN_VALUE && c != null) {
                    Log.i(TAG, "GamePad: wind swipe " + w);
                    c.post(PadLink.WIND, PadLink.body(o -> o.writeInt(w)));
                    performHapticFeedback(android.view.HapticFeedbackConstants.CONFIRM);
                    windWay = PadLink.windWay(w);  // (until the console's own comes)
                }
                invalidate();
                return true;
            }
            return false;  // (a move: the other fingers' too)
        }

        /** the face buttons' zone: a faint ring; with the swipe the four letters round it (round the thumb's home
         *  while it is on), the side taken filled */
        private void drawFace(Canvas cv) {
            line.setColor(Color.argb(60, 255, 255, 255));
            cv.drawCircle(faceX, faceY, faceZone, line);
            if (!faceSwipe()) return;
            String[] ls = {"X", "A", "B", "Y"};
            int[] bits = {Native.X, Native.A, Native.B, Native.Y};
            float[][] dir = {{0, -1}, {1, 0}, {0, 1}, {-1, 0}};
            boolean on = facePtr >= 0;
            float hx = on ? faceHx : faceX, hy = on ? faceHy : faceY, off = on ? faceU * 0.75f : faceZone * 0.62f;
            for (int k = 0; k < 4; k++) {
                float cx = hx + dir[k][0] * off, cy = hy + dir[k][1] * off;
                boolean taken = faceBit == bits[k];
                if (taken) {
                    fill.setColor(Color.argb(230, 255, 255, 255));
                    cv.drawCircle(cx, cy, faceU * 0.36f, fill);
                }
                text.setColor(taken ? Color.BLACK : Color.argb(220, 255, 255, 255));
                cv.drawText(ls[k], cx, cy + text.getTextSize() * 0.35f, text);
            }
            if (on) {
                line.setColor(Color.argb(200, 255, 255, 255));
                cv.drawCircle(hx, hy, faceU * 0.3f, line);
            }
        }

        private void place(int i, float x, float y, float r) {
            float u = getHeight() / 100f;
            Ctl c = ctls.get(i);
            c.x = x * u;
            c.y = y * u;
            c.r = r * u;
        }

        private void placeR(int i, float x, float y, float r) {
            place(i, 0, y, r);
            Ctl c = ctls.get(i);
            c.x = getWidth() - x * getHeight() / 100f;
        }

        @Override
        protected void onDraw(Canvas cv) {
            if (padScreen != null) padScreen.setSides(shown() ? 0.13f : 0.04f, shown() ? 0.75f : 0.96f);
            checkLayout();
            sendView();
            drawArrow(cv);
            if (!shown()) {  // the menu button stays (faint), so the menu is always there
                // (the screen: on a TV none, the remote's Back opens the menu; elsewhere for a few seconds after a touch)
                if (screenRole() && (tvHere || android.os.SystemClock.uptimeMillis() - lastTouch > MENU_SHOWN_MS)) return;
                Ctl m = ctls.get(12);
                drawButton(cv, m, false, 0.25f);
                return;
            }
            if (screenTouch) {  // the touch screen view: only the menu and the way back
                drawButton(cv, ctls.get(12), false, 0.5f);
                drawButton(cv, ctls.get(14), true, 0.6f);
                return;
            }
            // dimmed: it doesn't take touches here (the TV picture: as bright as chosen); drawn here, the game's own screens
            // too take them (MirrorView)
            if (picture.getVisibility() == View.VISIBLE && !drawn()) {
                fill.setColor(tvBack() ? Color.argb(Math.round((100 - tvLevel()) * 2.55f), 0, 0, 0) : Color.argb(110, 0, 0, 0));
                cv.drawRect(picture.getLeft(), picture.getTop(), picture.getRight(), picture.getBottom(), fill);
            }
            if (laidBlind) drawFace(cv);
            drawBgButton(cv);
            drawWind(cv);
            for (Ctl c : ctls) {
                if (c.kind == K_SCREEN && !hasScreen()) continue;
                if ((c.bit == Native.STICK_L || c.bit == Native.STICK_R) && c.kind == K_BUTTON && !laidBlind) continue;
                if (faceSwipe() && isFace(c)) continue;
                if (c.kind == K_STICK) {
                    if (laidBlind && c.pointer < 0) continue;  // (floating: only under the thumb)
                    fill.setColor(Color.argb(60, 255, 255, 255));
                    cv.drawCircle(c.x, c.y, c.r, fill);
                    line.setColor(Color.argb(140, 255, 255, 255));
                    cv.drawCircle(c.x, c.y, c.r, line);
                    fill.setColor(Color.argb(c.pointer >= 0 ? 200 : 140, 230, 240, 255));
                    cv.drawCircle(c.x + c.kx * c.r * 0.6f, c.y - c.ky * c.r * 0.6f, c.r * 0.45f, fill);
                } else if (c.kind == K_DPAD) {
                    float a = c.r * 0.38f;
                    for (int d = 0; d < 4; d++) {
                        int bit = d == 0 ? Native.UP : d == 1 ? Native.RIGHT : d == 2 ? Native.DOWN : Native.LEFT;
                        float dx = d == 1 ? 1 : d == 3 ? -1 : 0, dy = d == 0 ? -1 : d == 2 ? 1 : 0;
                        fill.setColor(Color.argb((buttons & bit) != 0 ? 200 : 90, 255, 255, 255));
                        cv.drawRect(c.x + dx * c.r * 0.62f - a, c.y + dy * c.r * 0.62f - a, c.x + dx * c.r * 0.62f + a,
                                c.y + dy * c.r * 0.62f + a, fill);
                    }
                } else {
                    drawButton(cv, c, c.kind == K_BUTTON && (buttons & c.bit) != 0, 1);
                }
            }
        }

        private void drawButton(Canvas cv, Ctl c, boolean down, float alpha) {
            fill.setColor(Color.argb((int) ((down ? 200 : 80) * alpha), 255, 255, 255));
            line.setColor(Color.argb((int) (150 * alpha), 255, 255, 255));
            if (c.w > 0) {
                float rr = c.h * 0.22f;
                cv.drawRoundRect(c.x - c.w / 2, c.y - c.h / 2, c.x + c.w / 2, c.y + c.h / 2, rr, rr, fill);
                cv.drawRoundRect(c.x - c.w / 2, c.y - c.h / 2, c.x + c.w / 2, c.y + c.h / 2, rr, rr, line);
            } else {
                cv.drawCircle(c.x, c.y, c.r, fill);
                cv.drawCircle(c.x, c.y, c.r, line);
            }
            text.setColor(Color.argb((int) (230 * alpha), down ? 20 : 255, down ? 40 : 255, down ? 60 : 255));
            if (c.kind == K_SCREEN) {  // a GamePad screen with a finger's dot on it
                float hw = c.r * 0.55f, hh = c.r * 0.36f;
                line.setColor(text.getColor());
                cv.drawRoundRect(c.x - hw, c.y - hh, c.x + hw, c.y + hh, c.r * 0.08f, c.r * 0.08f, line);
                int keep = fill.getColor();
                fill.setColor(text.getColor());
                cv.drawCircle(c.x + hw * 0.3f, c.y + hh * 0.15f, c.r * 0.12f, fill);
                fill.setColor(keep);
                return;
            }
            cv.drawText(c.label, c.x, c.y + text.getTextSize() * 0.35f, text);
        }

        private Ctl at(float x, float y) {
            Ctl best = null;
            float bestD = Float.MAX_VALUE;
            for (Ctl c : ctls) {
                if (!shown() && c.kind != K_MENU) continue;
                if (shown() && screenTouch && c.kind != K_MENU && c.kind != K_SCREEN) continue;
                if (c.kind == K_SCREEN && !hasScreen()) continue;
                if (c.kind == K_BUTTON && (c.bit == Native.STICK_L || c.bit == Native.STICK_R) && !laidBlind) continue;
                if (laidBlind && (c.kind == K_STICK || (faceSwipe() && isFace(c)))) continue;  // (floating; the swipe)
                float d = c.w > 0 ? (Math.abs(x - c.x) <= c.w * 0.6f && Math.abs(y - c.y) <= c.h * 0.65f ? 0 : Float.MAX_VALUE)
                        : (float) Math.hypot(x - c.x, y - c.y);
                if (d < c.r * 1.25f && d < bestD) {
                    best = c;
                    bestD = d;
                }
            }
            if (best == null && laidBlind && !faceSwipe() && inFaceZone(x, y)) {  // the diamond: the nearest of the four
                for (Ctl c : ctls) {
                    if (!isFace(c)) continue;
                    float d = (float) Math.hypot(x - c.x, y - c.y);
                    if (d < bestD) {
                        best = c;
                        bestD = d;
                    }
                }
            }
            return best;
        }

        private final boolean tvHere = MainActivity.isTv(PadActivity.this);
        private static final long MENU_SHOWN_MS = 4000;
        private long lastTouch;

        @Override
        public boolean onTouchEvent(MotionEvent e) {
            int a = e.getActionMasked(), idx = e.getActionIndex();
            checkLayout();
            if (arrowTouch(e, a, idx)) return true;
            if (shown() && bgTouch(e, a, idx)) return true;
            if (windTouch(e, a, idx)) return true;
            if (shown() && faceTouch(e, a, idx)) return true;
            if (screenRole() && a == MotionEvent.ACTION_DOWN) {
                boolean wasHidden = android.os.SystemClock.uptimeMillis() - lastTouch > MENU_SHOWN_MS;
                lastTouch = android.os.SystemClock.uptimeMillis();
                invalidate();
                postInvalidateDelayed(MENU_SHOWN_MS + 50);
                if (wasHidden) return true;  // (the first touch only shows the menu button)
            }
            if (a == MotionEvent.ACTION_DOWN && hiddenForController) {
                Ctl c = at(e.getX(), e.getY());
                // a touch brings the buttons back (and still counts) once no controller is connected; with one
                // (a phone in a controller clamp) touches stay the GamePad screen's
                if ((c == null || c.kind != K_MENU) && MainActivity.firstController() == null) {
                    hiddenForController = false;
                    invalidate();
                }
            }
            switch (a) {
                case MotionEvent.ACTION_DOWN:
                case MotionEvent.ACTION_POINTER_DOWN: {
                    int id = e.getPointerId(idx);
                    float x = e.getX(idx), y = e.getY(idx);
                    Ctl c = at(x, y);
                    if (c == null && controllerView()) {  // beside the buttons: that half's stick, here
                        Ctl s = x < getWidth() / 2f ? lStick : rStick;
                        if (s.pointer < 0) {
                            c = s;
                            s.x = x;
                            s.y = y;
                        }
                    }
                    if (c != null) {
                        c.pointer = id;
                        if (c.kind == K_BUTTON) buzz(c.bit, false);
                        else if (c.kind == K_DPAD) buzz(0, true);
                        if (c.kind == K_STICK && android.os.SystemClock.uptimeMillis() - c.lastUp < 300) c.clickHeld = true;
                        if (c.kind == K_MENU) openMenu();
                        if (c.kind == K_SCREEN) screenTouch = !screenTouch;
                        moved(c, x, y);
                    } else if (touchPointer < 0 && !screenRole() && !controllerView() && hasScreen() && onPicture(x, y)) {
                        touchPointer = id;
                        if (drawnNow()) padScreen.touch(MotionEvent.ACTION_DOWN, x - padScreen.getLeft(), y - padScreen.getTop());
                        else pictureTouch(true, x, y);
                    }
                    break;
                }
                case MotionEvent.ACTION_MOVE:
                    for (int i = 0; i < e.getPointerCount(); i++) {
                        int id = e.getPointerId(i);
                        if (id == touchPointer) {
                            if (drawnNow()) padScreen.touch(MotionEvent.ACTION_MOVE, e.getX(i) - padScreen.getLeft(), e.getY(i) - padScreen.getTop());
                            else pictureTouch(true, e.getX(i), e.getY(i));
                        }
                        for (Ctl c : ctls) if (c.pointer == id) moved(c, e.getX(i), e.getY(i));
                    }
                    break;
                case MotionEvent.ACTION_UP:
                case MotionEvent.ACTION_POINTER_UP:
                case MotionEvent.ACTION_CANCEL: {
                    boolean all = a == MotionEvent.ACTION_CANCEL;
                    int id = e.getPointerId(idx);
                    if (all || id == touchPointer) {
                        if (touchPointer >= 0) {
                            if (drawnNow()) padScreen.touch(all ? MotionEvent.ACTION_CANCEL : MotionEvent.ACTION_UP,
                                    e.getX(idx) - padScreen.getLeft(), e.getY(idx) - padScreen.getTop());
                            else pictureTouch(false, e.getX(idx), e.getY(idx));
                        }
                        touchPointer = -1;
                    }
                    for (Ctl c : ctls)
                        if (c.pointer >= 0 && (all || c.pointer == id)) {
                            c.pointer = -1;
                            c.kx = c.ky = 0;
                            c.x = c.homeX;
                            c.y = c.homeY;
                            c.clickHeld = false;
                            c.lastUp = android.os.SystemClock.uptimeMillis();
                        }
                    break;
                }
                default:
                    break;
            }
            recompute();
            return true;
        }

        private void moved(Ctl c, float x, float y) {
            if (c.kind == K_STICK || c.kind == K_DPAD) {
                float dx = (x - c.x) / (c.r * 0.8f), dy = (c.y - y) / (c.r * 0.8f), len = (float) Math.hypot(dx, dy);
                if (len > 1) {
                    dx /= len;
                    dy /= len;
                }
                c.kx = dx;
                c.ky = dy;
            }
        }

        /** the shortcut button's touches: a tap switches the picture, a long press steps the TV picture's brightness */
        private boolean bgTouch(MotionEvent e, int action, int idx) {
            if (action == MotionEvent.ACTION_DOWN || action == MotionEvent.ACTION_POINTER_DOWN) {
                if (bgPtr >= 0 || !bgButton() || Math.hypot(e.getX(idx) - bgX, e.getY(idx) - bgY) > bgR * 1.25f) return false;
                bgPtr = e.getPointerId(idx);
                bgStepped = false;
                postDelayed(bgStep, android.view.ViewConfiguration.getLongPressTimeout());
                invalidate();
                return true;
            }
            if (bgPtr < 0) return false;
            if (action == MotionEvent.ACTION_CANCEL || ((action == MotionEvent.ACTION_UP || action == MotionEvent.ACTION_POINTER_UP)
                    && e.getPointerId(idx) == bgPtr)) {
                removeCallbacks(bgStep);
                bgPtr = -1;
                if (action != MotionEvent.ACTION_CANCEL && !bgStepped) bgTv = !bgTvNow();
                invalidate();
                return true;
            }
            return false;
        }

        private void recompute() {
            int b = faceBit | pulseBits;
            stick[0] = stick[1] = stick[2] = stick[3] = 0;
            for (Ctl c : ctls) {
                if (c.pointer < 0) continue;
                if (c.kind == K_BUTTON) b |= c.bit;
                else if (c.kind == K_STICK) {
                    int s = c == lStick ? 0 : 2;
                    stick[s] = c.kx;
                    stick[s + 1] = c.ky;
                    if (c.clickHeld) b |= c.bit;
                } else if (c.kind == K_DPAD) {
                    if (c.ky > 0.4f) b |= Native.UP;
                    if (c.ky < -0.4f) b |= Native.DOWN;
                    if (c.kx > 0.4f) b |= Native.RIGHT;
                    if (c.kx < -0.4f) b |= Native.LEFT;
                }
            }
            buttons = b;
            invalidate();
            sendInput();
        }

        private boolean onPicture(float x, float y) {
            if (drawnNow()) return x >= padScreen.getLeft() && x < padScreen.getRight() && y >= padScreen.getTop() && y < padScreen.getBottom();
            return x >= picture.getLeft() && x < picture.getRight() && y >= picture.getTop() && y < picture.getBottom();
        }

        private void pictureTouch(boolean down, float x, float y) {
            float w = Math.max(1, picture.getWidth()), h = Math.max(1, picture.getHeight());
            float u = Math.max(0, Math.min(1, (x - picture.getLeft()) / w)), v = Math.max(0, Math.min(1, (y - picture.getTop()) / h));
            sendTouch(down, u, v);
        }
    }

    // (the phone's game process starts this one: MainActivity.becomePad)
    static void open(Activity from, int role) {
        Intent i = new Intent(from, PadActivity.class);
        i.putExtra(EXTRA_ROLE, role);
        i.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_MULTIPLE_TASK);
        from.startActivity(i);
    }
}
