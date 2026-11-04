package org.wwhdrecomp.app;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.RectF;
import android.media.MediaCodec;
import android.media.MediaCodecInfo;
import android.media.MediaFormat;
import android.net.nsd.NsdManager;
import android.net.nsd.NsdServiceInfo;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.view.Surface;

import java.io.File;
import java.io.IOException;
import java.net.ServerSocket;
import java.net.Socket;
import java.security.SecureRandom;
import java.util.HashSet;
import java.util.List;
import java.util.Set;
import java.util.concurrent.atomic.AtomicBoolean;

/**
 * The TV's side of a phone as the GamePad (PadLink): announces this TV on the local network, takes one
 * phone at a time (a phone it doesn't know types the code the TV shows), plays its buttons, sticks,
 * touches and motion into the game, sends it the GamePad picture (rendered into a video encoder's surface:
 * DrcDisplay.setRemote) and takes the game files and saves it sends.
 */
final class PadHost {
    private static final String TAG = "wwhd";

    private final MainActivity a;
    private final Handler main = new Handler(Looper.getMainLooper());
    private volatile ServerSocket server;
    // one-time passes for the screens this device invited (Show on a TV, ScreenFinder): such a one joins without the code
    private final Set<String> tickets = java.util.Collections.synchronizedSet(new HashSet<>());

    String ticket() {
        String t = java.util.UUID.randomUUID().toString();
        tickets.add(t);
        return t;
    }

    /** the port others join on (-1: not open yet) */
    int port() { ServerSocket s = server; return s != null ? s.getLocalPort() : -1; }
    private NsdManager nsd;
    private NsdManager.RegistrationListener registration;
    // one GamePad and one screen at a time (each the newest); the screen takes the video window (the GamePad's
    // device is a controller only meanwhile)
    private volatile Session pad, screen;
    private final Set<Session> sessions = java.util.Collections.synchronizedSet(new HashSet<>());
    private volatile boolean running;

    // the phone's input, read by MainActivity.pushInput
    volatile int buttons;
    volatile float lx, ly, rx, ry;
    private final AtomicBoolean inputPosted = new AtomicBoolean();

    /** the GamePad device's input and the screen device's controller together (buttons both, each stick the stronger) */
    private void mergeInput() {
        Session p = pad, s = screen;
        int b = 0;
        float[] st = new float[4];
        for (Session x : new Session[] {p, s}) {
            if (x == null) continue;
            b |= x.inButtons;
            float[] v = {x.inLx, x.inLy, x.inRx, x.inRy};
            for (int i = 0; i < 4; i += 2)
                if (v[i] * v[i] + v[i + 1] * v[i + 1] > st[i] * st[i] + st[i + 1] * st[i + 1]) {
                    st[i] = v[i];
                    st[i + 1] = v[i + 1];
                }
        }
        buttons = b;
        lx = st[0];
        ly = st[1];
        rx = st[2];
        ry = st[3];
    }

    private final PadLink.WifiAwake wifi;
    final PadUi ui = new PadUi(this);  // the app's dialogs on the GamePad device

    /** the dialog picture to the GamePad device (null: no dialog any more) */
    // ---- the game's text input (a save file's name, MainActivity.showTextInput) on the GamePad device: there its own
    // field and keyboard (the console's dialog only comes as a picture); the console's own field still works
    interface TextAnswer { void answer(boolean ok, String text); }

    private String textInitial = "", textMessage = "";
    private int textMax;
    private TextAnswer textAnswer;

    void askText(String initial, int max, String message, TextAnswer answer) {
        textInitial = initial != null ? initial : "";
        textMax = max;
        textMessage = message;
        textAnswer = answer;
        sendText(pad);
    }

    /** the console's text input is gone (answered there or here) */
    void textClosed() {
        if (textAnswer == null) return;
        textAnswer = null;
        sendText(pad);
    }

    private void sendText(Session p) {
        if (p == null || !p.welcomed) return;
        boolean open = textAnswer != null;
        String i = textInitial, m = textMessage;
        int max = textMax;
        p.conn.post(PadLink.TEXT_ASK, PadLink.body(o -> {
            o.writeBoolean(open);
            if (!open) return;
            o.writeUTF(i);
            o.writeInt(max);
            o.writeUTF(m);
        }));
    }

    /** the GamePad device shows the GamePad picture now */
    boolean padPicture() {
        Session p = pad;
        return p != null && p.welcomed && p.video != null && !p.viewTv && !p.hidden;
    }

    private final float[] arrowSent = new float[6];

    /** the options arrow over the GamePad device's picture (main thread; only changes go out) */
    void sendArrow(boolean on, float alpha, float l, float t, float r, float b) {
        Session p = pad;
        if (p == null || !p.welcomed) return;
        float[] now = {on ? 1 : 0, Math.round(alpha * 20) / 20f, l, t, r, b};
        if (!on && arrowSent[0] == 0) return;
        if (java.util.Arrays.equals(now, arrowSent)) return;
        System.arraycopy(now, 0, arrowSent, 0, 6);
        p.conn.post(PadLink.ARROW, PadLink.body(o -> {
            o.writeBoolean(now[0] != 0);
            for (int i = 1; i < 6; i++) o.writeFloat(now[i]);
        }));
    }

    // ---- the wind dial on the GamePad device (Mini map > Wind arrow > Change the wind (dial)): its way and turn while
    // they change (polled here), a swipe there comes back as WIND
    private static final long WIND_POLL_MS = 250;
    private float[] windSent;
    private final Runnable windPoll = new Runnable() {
        @Override
        public void run() {
            Session p = pad;
            if (p == null || !p.welcomed || !running) return;
            float[] now = a.windForPad();
            now[1] = Math.round(now[1] * 64) / 64f;  // (the arrow eases on the TV: only steps that show)
            now[2] = Math.round(now[2]);
            if (windSent == null || !java.util.Arrays.equals(now, windSent)) {
                windSent = now;
                p.conn.post(PadLink.WIND_STATE, PadLink.body(o -> {
                    o.writeBoolean(now[0] != 0);
                    o.writeFloat(now[1]);
                    o.writeFloat(now[2]);
                }));
            }
            main.postDelayed(this, WIND_POLL_MS);
        }
    };

    private int drawCount;

    // ---- the GamePad screen drawn on the GamePad device (PadScreen): the game's state while it changes (polled here)
    private static final long STATE_POLL_MS = 150, SAVE_POLL_MS = 1000;
    private Object[] stateSent;  // {ring, touch, save}
    private long saveAt;
    private final Runnable statePoll = new Runnable() {
        @Override
        public void run() {
            Session p = pad;
            if (p == null || !p.welcomed || !p.drawn || !running) return;
            if (a.gameStarted()) {
                int[] ring = Native.itemRing(), ts = Native.touchState();
                float[] map = mapState(Native.miniMapState());
                boolean play = false;  // (a game being played: the item ring has its inventory; the title, the file select: not)
                for (int v : ring) play |= v != -1;
                special(p, Native.drcTargets(), play);
                indoorMap(p, Native.miniMapState());
                long now = android.os.SystemClock.uptimeMillis();
                byte[] save = null;
                if (now >= saveAt) {
                    saveAt = now + SAVE_POLL_MS;
                    save = Native.progressData();
                }
                Object[] last = stateSent;
                boolean ringNew = last == null || !java.util.Arrays.equals(ring, (int[]) last[0]);
                boolean tsNew = last == null || !java.util.Arrays.equals(ts, (int[]) last[1]);
                boolean saveNew = save != null && (last == null || !java.util.Arrays.equals(save, (byte[]) last[2]));
                boolean mapNew = last == null || !java.util.Arrays.equals(map, (float[]) last[3]);
                if (ringNew || tsNew || saveNew || mapNew) {
                    stateSent = new Object[] {ring, ts, saveNew ? save : last == null ? null : last[2], map};
                    byte[] sv = saveNew ? save : null;
                    p.conn.post(PadLink.GAME_STATE, PadLink.body(o -> {
                        o.writeInt(ringNew ? ring.length : 0);
                        if (ringNew) for (int v : ring) o.writeInt(v);
                        o.writeInt(tsNew ? ts.length : 0);
                        if (tsNew) for (int v : ts) o.writeInt(v);
                        o.writeInt(sv == null ? -1 : sv.length);
                        if (sv != null) o.write(sv);
                        o.writeInt(mapNew ? map.length : 0);
                        if (mapNew) for (float v : map) o.writeFloat(v);
                    }));
                }
            }
            main.postDelayed(this, STATE_POLL_MS);
        }
    };

    // ---- the game's own GamePad screens the drawn one has no page for (the Picto Box's album, the warp map,
    // conducting, Sploosh Kaboom, a treasure chart ...): while one shows, its picture goes there as video
    // (PadLink.SPECIAL), the drawn pages again once the game's GamePad menu is back (its tabs among the touch targets)
    private String lastParts = "";

    private void special(Session p, String targets, boolean play) {
        java.util.TreeSet<String> parts = new java.util.TreeSet<>();
        for (String t : targets.split(";")) {
            String[] f = t.trim().split(" ");
            if (f.length != 6) continue;
            try {
                float x = Float.parseFloat(f[2]), y = Float.parseFloat(f[3]);
                if (x <= 0f || x >= 1f || y <= 0f || y >= 1f) continue;  // (a page sliding in or out)
            } catch (NumberFormatException e) {
                continue;
            }
            parts.add(f[1].replaceAll("_[0-9]+$", "") + "/" + f[0].replaceAll("_[0-9]+$", ""));
        }
        String now = String.join(" ", parts);
        if (!now.equals(lastParts)) {
            lastParts = now;
            Log.i(TAG, "GamePad host: the game's GamePad screen: " + (now.isEmpty() ? "(no touch targets)" : now));
        }
        // its menu (the tabs ITEMS / MAP / BOTTLES) or nothing to touch: drawn there; anything else: the picture
        // (the inventory slid to its page left of the items, the console's options page there: its slide area). No game
        // played yet (the title, the file select): the game's screen there too, if it can be drawn there
        boolean own = !parts.isEmpty() && !parts.contains("L_BtnMenu/B_Btn") && !parts.contains("RootPane/B_Slide")
                || (!play && (p.caps & PadLink.CAP_MIRROR) != 0);
        specialRun = own == specialSeen ? specialRun + 1 : 0;
        specialSeen = own;
        if (specialRun >= 2 && own != p.special) {  // (the same for three looks: no flicker while pages change)
            p.special = own;
            boolean orders = (p.caps & PadLink.CAP_MIRROR) != 0;
            Log.i(TAG, "GamePad host: " + (own ? "a screen of the game's own there: " + (orders ? "drawn there as the game draws it" : "its picture goes as video")
                    : "the drawn pages there again"));
            p.conn.post(PadLink.SPECIAL, PadLink.body(o -> o.writeBoolean(own)));
            if (orders) {
                if (own) p.startMirror();
                else p.stopMirror();
            } else {
                p.stopVideo();
                p.startVideo();
            }
        }
    }

    private boolean specialSeen;

    /** a texture of the game's own screen (Native.drcMirrorTexture: width, height, RGBA) as PNG (empty: none) */
    static byte[] mirrorPng(byte[] t) {
        if (t == null || t.length < 8) return new byte[0];
        java.nio.ByteBuffer b = java.nio.ByteBuffer.wrap(t).order(java.nio.ByteOrder.LITTLE_ENDIAN);
        int w = b.getInt(), h = b.getInt();
        if (w <= 0 || h <= 0 || t.length < 8 + w * h * 4) return new byte[0];
        int[] px = new int[w * h];
        for (int i = 0, o = 8; i < px.length; i++, o += 4)
            px[i] = (t[o + 3] & 255) << 24 | (t[o] & 255) << 16 | (t[o + 1] & 255) << 8 | (t[o + 2] & 255);
        android.graphics.Bitmap bm = android.graphics.Bitmap.createBitmap(px, w, h, android.graphics.Bitmap.Config.ARGB_8888);
        java.io.ByteArrayOutputStream o = new java.io.ByteArrayOutputStream();
        bm.compress(android.graphics.Bitmap.CompressFormat.PNG, 100, o);
        bm.recycle();
        return o.toByteArray();
    }

    static final int SPECIAL_PICTURE = 2;  // (1280 x 720: the game's own screens have small text)
    private int specialRun;

    // ---- the map indoors for the drawn screen: drawn here as the full map does (MiniMap.drawIndoorChart, the Map
    // pictures chosen here) when something on it changed, at most once a second
    private static final int INDOOR_W = 1024, INDOOR_H = 768;
    private String indoorKey;
    private long indoorAt;

    private void indoorMap(Session p, float[] st) {
        if (st == null || st.length < 70 || st[0] != 2) {
            if (indoorKey != null) {
                indoorKey = null;
                p.conn.post(PadLink.INDOOR_MAP, new byte[0]);
            }
            return;
        }
        long now = android.os.SystemClock.uptimeMillis();
        if (now < indoorAt) return;
        indoorAt = now + 1000;
        MiniMap mm = a.controlsView() != null ? a.controlsView().miniMap : null;
        if (mm == null) return;
        StringBuilder k = new StringBuilder(Native.miniMapStage());
        for (int i : new int[] {62, 63, 64, 65, 66, 67, 68, 69}) k.append(',').append((int) st[i]);
        k.append(',').append(Math.round(st[1] / 150)).append(',').append(Math.round(st[2] / 150)).append(',').append(Math.round(st[3] * 8))
                .append(',').append(Math.round(st[61] / 300));
        String key = k.toString();
        if (key.equals(indoorKey)) return;
        Bitmap b = Bitmap.createBitmap(INDOOR_W, INDOOR_H, Bitmap.Config.ARGB_8888);
        android.graphics.Canvas c = new android.graphics.Canvas(b);
        int pictures = MiniMap.pictures(a.prefs);
        int floor = mm.indoorReady() ? mm.indoorFloorAt(st[61]) : -1;
        RectF box = new RectF(0, 0, INDOOR_W, INDOOR_H);
        // (first drawn to read the stage; then fitted to its bounds)
        if (!mm.drawIndoorChart(c, st, pictures, floor, box, st[1], st[2], 0.05f, INDOOR_H * 0.3f, 0xFF)) return;
        RectF r = mm.indoorBounds();
        if (r == null || r.width() <= 0 || r.height() <= 0) return;
        r.inset(-r.width() * 0.04f - 200, -r.height() * 0.04f - 200);
        r.union(st[1], st[2]);
        float scale = Math.min(INDOOR_W / r.width(), INDOOR_H / r.height());
        b.eraseColor(0);
        if (!mm.drawIndoorChart(c, st, pictures, mm.indoorFloorAt(st[61]), box, r.centerX(), r.centerY(), scale, INDOOR_H * 0.3f, 0xFF)) return;
        indoorKey = key;
        fontWork.execute(() -> {
            java.io.ByteArrayOutputStream out = new java.io.ByteArrayOutputStream();
            b.compress(Bitmap.CompressFormat.WEBP_LOSSY, 88, out);
            p.conn.post(PadLink.INDOOR_MAP, out.toByteArray());
            Log.i(TAG, "GamePad host: the map indoors there, " + out.size() + " bytes");
        });
    }

    /** the mini map's state rounded (positions to 20 units, facings to 3 degrees: no message for every step) */
    private static float[] mapState(float[] s) {
        if (s == null) return new float[0];
        float[] r = s.clone();
        for (int i : new int[] {1, 2, 6, 7, 61, 73, 74, 76}) if (i < r.length) r[i] = Math.round(r[i] / 20f) * 20f;
        for (int i : new int[] {3, 8, 75}) if (i < r.length) r[i] = Math.round(r[i] / 0.05f) * 0.05f;
        return r;
    }

    /** the names PadScreen shows, in the game's words (GameText built here first: tried again a while) */
    private void sendLabels(Session p, int tries) {
        if (p.conn.closed()) return;
        GameText.prepare(a, null);
        if (GameText.itemName(0x20) == null && tries < 30) {  // (the texts are built in the background on first use)
            main.postDelayed(() -> sendLabels(p, tries + 1), 1000);
            return;
        }
        GameProgress.Labels l = GameProgress.labels(a);
        java.util.List<Object[]> all = new java.util.ArrayList<>();
        for (int i = 0; i < 10; i++) all.add(new Object[] {PadLink.LABEL_SECTION, i, l.section(i)});
        for (int i = 0; i < 256; i++) {
            String n = GameText.itemName(i);
            if (n != null) all.add(new Object[] {PadLink.LABEL_ITEM, i, l.item(i)});
        }
        for (int i = 1; i <= GameProgress.DUNGEONS; i++) all.add(new Object[] {PadLink.LABEL_DUNGEON, i, l.dungeon(i)});
        for (int i = 1; i <= 49; i++) all.add(new Object[] {PadLink.LABEL_ISLAND, i, l.island(i)});
        for (int i = 0; i < 6; i++) all.add(new Object[] {PadLink.LABEL_SONG, i, l.song(i)});
        for (int i = 0; i < GameProgress.FIGURINES; i++) all.add(new Object[] {PadLink.LABEL_FIGURINE, i, l.figurine(i)});
        for (int i = 0; i <= GameProgress.OWN_MAGIC; i++) all.add(new Object[] {PadLink.LABEL_OWN, i, l.own(i)});
        String gameDir = a.gameDir();
        java.io.File tmp = new java.io.File(a.getCacheDir(), "pad_texts.txt");
        fontWork.execute(() -> {  // the menus' own words (tabs, buttons, the map's compass) from the game's 2D pack
            int n = 0;
            for (String layout : new String[] {"MenuBtns_00", "MenuItem_00", "MenuMap_00"}) {
                if (!Native.extractUiTexts(gameDir, tmp.getAbsolutePath(), layout)) continue;
                try {
                    for (String line : java.nio.file.Files.readAllLines(tmp.toPath(), java.nio.charset.StandardCharsets.UTF_8)) {
                        int tab = line.indexOf('\t');
                        if (tab > 0) all.add(new Object[] {PadLink.LABEL_UI, n++, layout + ":" + line.substring(0, tab) + "\t" + line.substring(tab + 1)});
                    }
                } catch (IOException e) {
                    Log.w(TAG, "GamePad host: " + layout + " texts", e);
                }
            }
            tmp.delete();
            postLabels(p, all);
        });
    }

    private static void postLabels(Session p, java.util.List<Object[]> all) {
        p.conn.post(PadLink.LABELS, PadLink.body(o -> {
            o.writeInt(all.size());
            for (Object[] e : all) {
                o.writeByte((Integer) e[0]);
                o.writeInt((Integer) e[1]);
                o.writeUTF(e[2] == null ? "" : (String) e[2]);
            }
        }));
    }

    private final java.util.concurrent.ExecutorService fontWork = java.util.concurrent.Executors.newSingleThreadExecutor();

    /** a GameFont's files to the GamePad device (built here first: tried again a while) */
    private void sendFont(Session p, String name, int tries) {
        if (p.conn.closed()) return;
        GameFont font = name.equals("CKingMain") ? GameFont.MAIN : name.equals("CKingMsg") ? GameFont.MSG : null;
        if (font == null) return;
        if (!font.isReady(a, null) && tries < 30) {
            main.postDelayed(() -> sendFont(p, name, tries + 1), 1000);
            return;
        }
        String lang = a.gameLanguage();
        java.io.File dir = GameFont.dir(a, name, lang);
        java.io.File[] files = dir.listFiles();
        java.util.List<java.io.File> send = new java.util.ArrayList<>();
        if (files != null && new java.io.File(dir, GameFont.versionFile()).exists())
            for (java.io.File f : files) if (f.isFile()) send.add(f);
        fontWork.execute(() -> {
            try {
                byte[] body = PadLink.body(o -> {
                    o.writeUTF(name);
                    o.writeUTF(lang);
                    o.writeInt(send.size());
                    for (java.io.File f : send) {
                        byte[] raw = java.nio.file.Files.readAllBytes(f.toPath());
                        java.util.zip.Deflater d = new java.util.zip.Deflater(6);
                        java.io.ByteArrayOutputStream z = new java.io.ByteArrayOutputStream();
                        try (java.util.zip.DeflaterOutputStream dz = new java.util.zip.DeflaterOutputStream(z, d)) {
                            dz.write(raw);
                        }
                        d.end();
                        o.writeUTF(f.getName());
                        o.writeInt(z.size());
                        z.writeTo(o);
                    }
                });
                p.conn.post(PadLink.FONT, body);
                Log.i(TAG, "GamePad host: font " + name + " (" + lang + ") sent, " + body.length + " bytes");
            } catch (RuntimeException e) {
                Log.w(TAG, "GamePad host: font " + name + " not sent", e);
            }
        });
    }

    /** a GameIcons texture to the GamePad device (built here from the game files on first use: tried again a while) */
    private void sendIcon(Session p, String name, int tries) {
        if (p.conn.closed()) return;
        android.graphics.Bitmap b = GameIcons.get(a, name, null);
        if (b == null && tries < 20) {
            main.postDelayed(() -> sendIcon(p, name, tries + 1), 500);
            return;
        }
        byte[] png;
        if (b != null) {
            java.io.ByteArrayOutputStream out = new java.io.ByteArrayOutputStream();
            b.compress(android.graphics.Bitmap.CompressFormat.PNG, 100, out);
            png = out.toByteArray();
        } else png = new byte[0];
        p.conn.post(PadLink.ICON, PadLink.body(o -> {
            o.writeUTF(name);
            o.write(png);
        }));
    }

    /** whether the GamePad device draws the dialogs itself (UiDraw orders) */
    boolean padDraws() {
        Session p = uiSession();
        return p != null && p.welcomed && (p.caps & PadLink.CAP_UI_DRAW) != 0;
    }

    /** the dialog's drawing orders (null: none any more), after the bitmaps they use */
    void sendDraw(List<Object[]> bitmaps, byte[] orders, int w, int h) {
        Session p = uiSession();
        if (p == null || !p.welcomed) return;
        long bmpBytes = 0;
        for (Object[] b : bitmaps) {
            byte[] data = UiDraw.encode((android.graphics.Bitmap) b[1]);
            bmpBytes += data.length;
            ((android.graphics.Bitmap) b[1]).recycle();
            int id = (Integer) b[0];
            p.conn.post(PadLink.UI_BITMAP, PadLink.body(o -> {
                o.writeInt(id);
                o.write(data);
            }));
        }
        if (!bitmaps.isEmpty() || (++drawCount % 100) == 1)
            Log.i(TAG, "GamePad host: dialog drawing " + (orders == null ? 0 : orders.length) + " bytes, " + bitmaps.size()
                    + " new bitmaps (" + bmpBytes + " bytes)");
        p.conn.post(PadLink.UI_DRAW, PadLink.body(o -> {
            o.writeInt(orders == null ? 0 : w);
            o.writeInt(orders == null ? 0 : h);
            if (orders != null) o.write(orders);
        }));
    }

    void sendUi(byte[] jpeg, int w, int h) {
        Session p = uiSession();
        if (p == null || !p.welcomed) return;
        p.conn.post(PadLink.UI_FRAME, PadLink.body(o -> {
            o.writeInt(jpeg == null ? 0 : w);
            o.writeInt(jpeg == null ? 0 : h);
            if (jpeg != null) o.write(jpeg);
        }));
    }

    PadHost(MainActivity a) {
        this.a = a;
        wifi = new PadLink.WifiAwake(a);
    }

    /** where the app's dialogs go: the GamePad device, else the screen device (its controller works them) */
    private Session uiSession() {
        Session p = pad;
        if (p != null && p.welcomed) return p;
        Session s = screen;
        return s != null && s.welcomed ? s : null;
    }

    Object uiTarget() { return uiSession(); }

    /** whether a phone is the GamePad now */
    boolean connected() { Session p = pad; return p != null && p.welcomed; }

    /** whether another device shows the TV picture now */
    boolean screenConnected() { Session s = screen; return s != null && s.welcomed; }

    /** the names of the devices joined now (GamePad, screen), "" for none */
    String joined() {
        Session p = pad, s = screen;
        StringBuilder b = new StringBuilder();
        if (p != null && p.welcomed) b.append(p.phone);
        if (s != null && s.welcomed && s != p) b.append(b.length() > 0 ? ", " : "").append(s.phone);
        return b.toString();
    }

    void start() {
        if (running) return;
        running = true;
        new Thread(this::serve, "wwhd-pad-host").start();
        new Thread(this::sound, "wwhd-pad-sound").start();
    }

    // ---- sound for the other devices: the screen gets the TV sound, the GamePad its own sounds (each unless it
    // says otherwise: SOUND); this device then leaves that sound out (the runtime, option remote_audio)
    private volatile int soundBits = -1;

    /** which streams go out now (main thread; after a device came, left or switched its sound) */
    void updateSound() {
        Session s = screen, p = pad;
        wifi.set(running && ((s != null && s.welcomed) || (p != null && p.welcomed)));  // (low-latency Wi-Fi while any is there)
        int bits = (s != null && s.welcomed && s.sound ? 1 : 0) | (p != null && p.welcomed && p.sound ? 2 : 0);
        if (!running) bits = 0;
        if (bits == soundBits) return;
        soundBits = bits;
        Native.setOption("remote_audio", bits);
    }

    private void sound() {
        short[] pcm = new short[48000 / 10 * 2];  // up to 100 ms at a time
        while (running) {
            try {
                Thread.sleep(5);
            } catch (InterruptedException e) {
                return;
            }
            int bits = soundBits;
            if (bits <= 0) continue;
            for (int stream = 1; stream <= 2; stream++) {
                if ((bits & stream) == 0) continue;
                Session to = stream == 1 ? screen : pad;
                int n = Native.takeRemoteAudio(stream, pcm);
                if (n <= 0 || to == null) continue;
                byte[] b = new byte[n * 4];
                java.nio.ByteBuffer.wrap(b).order(java.nio.ByteOrder.LITTLE_ENDIAN).asShortBuffer().put(pcm, 0, n * 2);
                to.conn.post(PadLink.AUDIO, b);
            }
        }
    }

    void stop() {
        running = false;
        synchronized (sessions) {
            for (Session s : sessions) s.conn.close();
        }
        updateSound();
        try {
            if (server != null) server.close();
        } catch (IOException ignored) {
        }
        if (nsd != null && registration != null) {
            try {
                nsd.unregisterService(registration);
            } catch (IllegalArgumentException ignored) {
            }
        }
        registration = null;
    }

    static String deviceName(Context c) {
        String n = android.provider.Settings.Global.getString(c.getContentResolver(), android.provider.Settings.Global.DEVICE_NAME);
        return n != null && !n.isEmpty() ? n : android.os.Build.MODEL;
    }

    private void serve() {
        try {
            try {
                server = new ServerSocket(PadLink.PORT);
            } catch (IOException taken) {
                server = new ServerSocket(0);
            }
        } catch (IOException e) {
            Log.w(TAG, "GamePad host: no socket", e);
            return;
        }
        NsdServiceInfo info = new NsdServiceInfo();
        info.setServiceName(deviceName(a));
        info.setServiceType(PadLink.SERVICE_TYPE);
        info.setPort(server.getLocalPort());
        nsd = a.getSystemService(NsdManager.class);
        registration = new NsdManager.RegistrationListener() {
            @Override public void onServiceRegistered(NsdServiceInfo i) { Log.i(TAG, "GamePad host: announced as " + i.getServiceName()); }
            @Override public void onRegistrationFailed(NsdServiceInfo i, int err) { Log.w(TAG, "GamePad host: announcing failed " + err); }
            @Override public void onServiceUnregistered(NsdServiceInfo i) {}
            @Override public void onUnregistrationFailed(NsdServiceInfo i, int err) {}
        };
        nsd.registerService(info, NsdManager.PROTOCOL_DNS_SD, registration);
        Log.i(TAG, "GamePad host: port " + server.getLocalPort());
        while (running) {
            Socket s;
            try {
                s = server.accept();
            } catch (IOException e) {
                break;
            }
            try {
                Session n = new Session(new PadLink.Conn(s));
                sessions.add(n);
                new Thread(n::run, "wwhd-pad-session").start();
            } catch (IOException e) {
                Log.w(TAG, "GamePad host: connection failed", e);
            }
        }
    }

    /** the game started (or was already running when the phone came): the picture goes to the phone */
    void gameStarted() {
        main.post(() -> {
            Session s = screen, p = pad;
            if (s != null && s.welcomed) s.startVideo();
            if (p != null && p.welcomed) p.startVideo();
        });
    }

    private Session slot(int role) { return role == PadLink.ROLE_SCREEN ? screen : pad; }

    private final class Session {
        final PadLink.Conn conn;
        volatile boolean welcomed;
        String phone = "";
        String phoneGame = "";  // the phone's game (PadLink.gameId): saves only between the same
        int role = PadLink.ROLE_PAD;
        volatile boolean sound = true;  // its sound goes to it (SOUND)
        volatile int caps;  // PadLink.CAP_* it said it has (CAPS)
        volatile int inButtons;  // its input (INPUT): mergeInput puts the GamePad's and the screen's together
        volatile float inLx, inLy, inRx, inRy;
        volatile boolean drawn;  // it draws the GamePad screen itself (DRAWN): the game's state goes to it
        private Video video;
        private PadLink.Receive receive;
        private File receiveDir;
        private GameDialog codeDialog;

        Session(PadLink.Conn c) { conn = c; }

        void run() {
            try {
                if (!greet()) return;
                for (;;) handle(conn.read());
            } catch (IOException e) {
                if (!conn.closed()) Log.i(TAG, "GamePad host: " + phone + " gone: " + e.getMessage());
            } finally {
                conn.close();
                sessions.remove(this);
                if (receive != null) {
                    receive.abort();
                    Backup.deleteTree(receiveDir);  // (unfinished)
                }
                main.post(() -> {
                    hideCode();
                    stopVideo();
                    stopMirror();
                    if (slot(role) == this) {
                        if (role == PadLink.ROLE_SCREEN) screen = null;
                        else pad = null;
                        mergeInput();
                        a.padInput();
                        a.padConnectedChanged();
                        updateSound();
                        if (welcomed) a.padStatus(a.getString(R.string.pad_tv_left, phone));
                    }
                });
            }
        }

        /** HELLO, and the code for a phone this TV doesn't know yet */
        private boolean greet() throws IOException {
            Set<String> trusted = new HashSet<>(a.prefs.getStringSet("pad_tokens", new HashSet<>()));
            String code = null;
            for (int tries = 0; tries < 4; ) {
                PadLink.Frame f = conn.read();
                if (f.type != PadLink.HELLO) continue;
                int version = f.in.readInt();
                phone = f.in.readUTF();
                String token = f.in.readUTF(), typed = f.in.readUTF();
                phoneGame = f.in.available() > 0 ? f.in.readUTF() : "";
                role = f.in.available() > 0 ? f.in.readInt() : PadLink.ROLE_PAD;
                if (role != PadLink.ROLE_SCREEN) role = PadLink.ROLE_PAD;
                picture = role == PadLink.ROLE_SCREEN ? PadLink.SCREEN_PICTURE_DEFAULT : PadLink.PICTURE_DEFAULT;
                if (version != PadLink.VERSION) {
                    conn.send(PadLink.BAD_CODE, PadLink.body(o -> o.writeUTF("version")));
                    return false;
                }
                if (!token.isEmpty() && trusted.contains(token)) {
                    welcome(token);
                    return true;
                }
                if (!token.isEmpty() && tickets.remove(token)) {  // invited from here: trusted from now on, as with the code
                    String t = java.util.UUID.randomUUID().toString();
                    trusted.add(t);
                    a.prefs.edit().putStringSet("pad_tokens", trusted).apply();
                    welcome(t);
                    return true;
                }
                if (code != null && code.equals(typed)) {
                    String t = java.util.UUID.randomUUID().toString();
                    trusted.add(t);
                    a.prefs.edit().putStringSet("pad_tokens", trusted).apply();
                    welcome(t);
                    return true;
                }
                if (code != null) {
                    tries++;
                    conn.send(PadLink.BAD_CODE, PadLink.body(o -> o.writeUTF("code")));
                    continue;
                }
                code = String.format(java.util.Locale.ROOT, "%04d", new SecureRandom().nextInt(10000));
                final String shown = code;
                main.post(() -> showCode(shown));
                conn.send(PadLink.NEED_CODE, new byte[0]);
            }
            return false;
        }

        private void welcome(String token) throws IOException {
            main.post(this::hideCode);
            boolean hasGame = a.hasGameFiles(), running = a.gameStarted(), needsCode = !running && a.padNeedsCode();
            conn.send(PadLink.WELCOME, o -> {
                o.writeUTF(token);
                o.writeUTF(deviceName(a));
                o.writeBoolean(hasGame);
                o.writeBoolean(running);
                o.writeUTF(PadLink.gameId(a.baseDir()));
                o.writeUTF(Native.cpuFeatures());  // (a phone can compile the game code for this CPU: no compile here)
                o.writeUTF(Native.cpuModel());
                o.writeBoolean(needsCode);
            });
            welcomed = true;
            main.post(() -> {
                Session old = slot(role);
                if (role == PadLink.ROLE_SCREEN) screen = this;
                else pad = this;
                if (old != null && old != this) old.conn.close();  // one of each: the newer one
                a.padStatus(a.getString(role == PadLink.ROLE_SCREEN ? R.string.pad_tv_screen_joined : R.string.pad_tv_joined, phone));
                a.padConnectedChanged();
                updateSound();
                if (a.gameStarted()) startVideo();
                if (role == PadLink.ROLE_PAD && textAnswer != null) sendText(this);  // (a name asked for already)
                if (role == PadLink.ROLE_PAD) {
                    arrowSent[0] = 0;  // (the arrow and the wind anew for this one)
                    windSent = null;
                    main.removeCallbacks(windPoll);
                    main.post(windPoll);
                }
            });
        }

        private void showCode(String code) {
            hideCode();
            if (conn.closed()) return;
            codeDialog = new GameDialog(a).title(role == PadLink.ROLE_SCREEN ? R.string.pad_screen_title : R.string.pad_title)
                    .message(a.getString(role == PadLink.ROLE_SCREEN ? R.string.pad_tv_code_screen : R.string.pad_tv_code, phone, code))
                    .button(R.string.opt_cancel, conn::close);
            codeDialog.show();
        }

        private void hideCode() {
            if (codeDialog != null) codeDialog.dismiss();
            codeDialog = null;
        }

        private void handle(PadLink.Frame f) throws IOException {
            switch (f.type) {
                case PadLink.INPUT:  // a GamePad device's buttons, or a screen device's controller
                    inButtons = f.in.readInt();
                    inLx = f.in.readFloat();
                    inLy = f.in.readFloat();
                    inRx = f.in.readFloat();
                    inRy = f.in.readFloat();
                    mergeInput();
                    if (!inputPosted.getAndSet(true)) main.post(() -> {
                        inputPosted.set(false);
                        a.padInput();
                    });
                    break;
                case PadLink.TOUCH: {
                    if (role != PadLink.ROLE_PAD) break;
                    boolean down = f.in.readBoolean();
                    float u = f.in.readFloat(), v = f.in.readFloat();
                    main.post(() -> a.padTouch(down, u, v));
                    break;
                }
                case PadLink.MOTION:
                    if (a.gameStarted() && role == PadLink.ROLE_PAD)
                        Native.motionSample(f.in.readFloat(), f.in.readFloat(), f.in.readFloat(), f.in.readFloat(),
                                f.in.readFloat(), f.in.readFloat(), f.in.readFloat());
                    break;
                case PadLink.PING: {  // answered at once: the phone measures the round trip and the clocks' offset
                    long t = f.in.readLong();
                    conn.send(PadLink.PONG, o -> {
                        o.writeLong(t);
                        o.writeLong(System.nanoTime());
                    });
                    break;
                }
                case PadLink.SOUND: {
                    sound = f.in.readBoolean();
                    main.post(PadHost.this::updateSound);
                    break;
                }
                case PadLink.PICTURE: {
                    int p = f.in.readInt();
                    float shape = f.in.available() >= 4 ? f.in.readFloat() : 0;
                    main.post(() -> setPicture(p, shape));
                    break;
                }
                case PadLink.OPTIONS:
                    if (role == PadLink.ROLE_PAD) main.post(a::padOptions);
                    break;
                case PadLink.WIND: {
                    int angle = f.in.readInt();
                    if (role == PadLink.ROLE_PAD) main.post(() -> a.setWind(angle));
                    break;
                }
                case PadLink.SHOWN: {
                    boolean shown = f.in.readBoolean();
                    main.post(() -> {
                        if (hidden == !shown) return;
                        hidden = !shown;
                        Log.i(TAG, "GamePad host: " + phone + (hidden ? " doesn't show the picture: none encoded" : " shows the picture again"));
                        if (hidden) stopVideo();
                        else if (a.gameStarted()) startVideo();
                    });
                    break;
                }
                case PadLink.TEXT_ANSWER: {
                    boolean ok = f.in.readBoolean();
                    String text = f.in.readUTF();
                    main.post(() -> {
                        TextAnswer t = textAnswer;
                        if (t != null && pad == this) t.answer(ok, text);
                    });
                    break;
                }
                case PadLink.VIEW: {
                    boolean tv = f.in.readBoolean();
                    main.post(() -> {
                        viewTv = tv;
                        if (role == PadLink.ROLE_PAD && pad == this && video != null) Native.setOption("drc_window_tv", tv ? 1 : 0);
                        a.padPictureChanged();
                    });
                    break;
                }
                case PadLink.DRAWN: {
                    drawn = f.in.readBoolean();
                    special = false;
                    stopMirror();
                    Log.i(TAG, "GamePad host: " + phone + (drawn ? " draws the GamePad screen itself" : " takes the picture"));
                    main.post(() -> {
                        stateSent = null;
                        main.removeCallbacks(statePoll);
                        if (drawn && pad == this) main.post(statePoll);
                        if (drawn && pad == this) sendLabels(this, 0);
                    });
                    break;
                }
                case PadLink.EQUIP: {
                    int button = f.in.readInt(), slot = f.in.readInt();
                    if (role == PadLink.ROLE_PAD) main.post(() -> a.padEquip(button, slot));
                    break;
                }
                case PadLink.CONDUCT: {
                    int slot = f.in.readInt();
                    if (role == PadLink.ROLE_PAD) main.post(() -> a.padConduct(slot));
                    break;
                }
                case PadLink.ICON_WANT: {
                    String name = f.in.readUTF();
                    main.post(() -> sendIcon(this, name, 0));
                    break;
                }
                case PadLink.FONT_WANT: {
                    String name = f.in.readUTF();
                    main.post(() -> sendFont(this, name, 0));
                    break;
                }
                case PadLink.SAVE_GAME:
                    if (role == PadLink.ROLE_PAD) main.post(() -> a.padSaveGame());
                    break;
                case PadLink.MIRROR_RESET: {
                    mirrorSent.clear();
                    break;
                }
                case PadLink.CAPS: {
                    caps = f.in.readInt();
                    Log.i(TAG, "GamePad host: " + phone + " can do " + Integer.toHexString(caps));
                    main.post(() -> { if (uiSession() == this) ui.resend(); });
                    break;
                }
                case PadLink.UI_TOUCH: {
                    int action = f.in.readInt();
                    float u = f.in.readFloat(), v = f.in.readFloat();
                    main.post(() -> { if (uiSession() == this) ui.touch(action, u, v); });
                    break;
                }
                case PadLink.KEY: {
                    int action = f.in.readInt(), code = f.in.readInt(), repeat = f.in.readInt();
                    boolean fromScreen = role == PadLink.ROLE_SCREEN;
                    main.post(() -> a.padKey(action, code, repeat, fromScreen));
                    break;
                }
                case PadLink.KEYFRAME:
                    main.post(() -> { if (video != null) video.keyframe(); });
                    break;
                case PadLink.OFFER:
                    offered(f);
                    break;
                case PadLink.FILE:
                    if (receive == null || f.in.readInt() != receive.kind) throw new IOException("unexpected file");
                    receive.file(f.in);
                    break;
                case PadLink.CHUNK:
                    if (receive == null || PadLink.chunkKind(f.data) != receive.kind) throw new IOException("unexpected data");
                    receive.chunk(f.data);
                    progress();
                    break;
                case PadLink.DONE:
                    received();
                    break;
                case PadLink.SAVES_BACK:
                    sendSavesBack();
                    break;
                case PadLink.WANT:
                    wanted(f);
                    break;
                case PadLink.BYE:
                    conn.close();
                    break;
                default:
                    break;
            }
        }

        // ---- files from the phone
        private void offered(PadLink.Frame f) throws IOException {
            int kind = f.in.readInt();
            List<PadLink.Entry> files = PadLink.readOffer(f.in);
            File base = a.baseDir();
            List<Integer> want = new java.util.ArrayList<>();
            boolean saves = false;
            if (kind == PadLink.KIND_GAME && !a.gameStarted()) {  // all of them, into a folder of their own
                receiveDir = new File(base, "game-receiving");
                Backup.deleteTree(receiveDir);
                for (int i = 0; i < files.size(); i++) want.add(i);
            } else if (kind == PadLink.KIND_CODE && !a.gameStarted()) {  // the code for this CPU (before game files, or instead of a compile)
                receiveDir = new File(a.codeDir().getParentFile(), "codecache-receiving");
                Backup.deleteTree(receiveDir);
                for (int i = 0; i < files.size(); i++) want.add(i);
            } else if (kind == PadLink.KIND_SAVES && PadLink.savesFit(phoneGame, PadLink.gameId(base))) {
                receiveDir = new File(base, PadLink.RECEIVING);
                Backup.deleteTree(receiveDir);
                want = PadLink.onlyFitting(files, PadLink.newerSaves(base, files), PadLink.statesFit(phoneGame, PadLink.gameId(base)));
                saves = true;
            }
            // room for them here? (game files and code: all or nothing; saves: what fits, PadLink.fitSpace)
            long need = PadLink.size(files, want);
            lackFree = receiveDir != null ? PadLink.freeSpace(receiveDir) : Long.MAX_VALUE;
            if (saves && !want.isEmpty() && need + PadLink.SPACE_MARGIN > lackFree) {
                // the states the last sync moved aside go anyway when states come (PadLink.applySaves): short of room, now
                boolean states = false;
                for (int i : want) states |= files.get(i).path.startsWith("states/");
                File kept = new File(base, "states-before-sync");
                if (states && kept.isDirectory()) {
                    Backup.deleteTree(kept);
                    lackFree = PadLink.freeSpace(receiveDir);
                    Log.i(TAG, "GamePad host: states-before-sync deleted for room, " + PadLink.sizeText(lackFree) + " free now");
                }
            }
            if (!want.isEmpty() && need + PadLink.SPACE_MARGIN > lackFree) {
                List<Integer> fit = saves ? PadLink.fitSpace(files, want, lackFree) : new java.util.ArrayList<>();
                lacking = need - PadLink.size(files, fit);
                Log.w(TAG, "GamePad host: " + PadLink.sizeText(need) + " offered by " + phone + ", " + PadLink.sizeText(lackFree)
                        + " free here: " + PadLink.sizeText(lacking) + " not taken");
                want = fit;
            } else lacking = 0;
            conn.send(PadLink.WANT, PadLink.want(kind, want, lacking, lackFree));  // (the phone tells what stays at once)
            if (want.isEmpty()) {
                result(kind, lacking == 0, false);  // (no room: not ok, the phone tells why)
                return;
            }
            receive = new PadLink.Receive(kind, receiveDir, files, want);
            String what = a.getString(kind == PadLink.KIND_GAME ? R.string.pad_what_game : kind == PadLink.KIND_CODE ? R.string.pad_what_code
                                                                                                         : R.string.pad_what_saves);
            main.post(() -> a.padStatus(a.getString(R.string.pad_tv_receiving, what, phone, 0)));
        }

        private long lastProgress;

        private void progress() {
            long now = android.os.SystemClock.uptimeMillis();
            if (now - lastProgress < 1000) return;
            lastProgress = now;
            PadLink.Receive r = receive;
            int pct = (int) (100 * r.done() / Math.max(1, r.total()));
            String what = a.getString(r.kind == PadLink.KIND_GAME ? R.string.pad_what_game : r.kind == PadLink.KIND_CODE ? R.string.pad_what_code
                                                                                                           : R.string.pad_what_saves);
            main.post(() -> a.padStatus(a.getString(R.string.pad_tv_receiving, what, phone, pct)));
        }

        private void received() throws IOException {
            PadLink.Receive r = receive;
            if (r == null) return;
            r.finish();
            receive = null;
            File base = a.baseDir();
            if (r.kind == PadLink.KIND_CODE) {  // into the code cache: the game files' check then finds it ready
                File code = a.codeDir();
                //noinspection ResultOfMethodCallIgnored
                code.mkdirs();
                boolean ok = true;
                File[] got = receiveDir.listFiles();
                if (got != null)
                    for (File f : got) {
                        File dst = new File(code, f.getName());
                        //noinspection ResultOfMethodCallIgnored
                        dst.delete();
                        ok &= f.renameTo(dst);
                    }
                Backup.deleteTree(receiveDir);
                Log.i(TAG, "GamePad host: game code from " + phone + (ok ? " in place" : " not all in place"));
                result(r.kind, ok, false);
                if (ok) main.post(a::padCodeReceived);
                return;
            }
            if (r.kind == PadLink.KIND_GAME) {
                File game = new File(a.gameDir());
                Backup.deleteTree(game);
                boolean ok = receiveDir.renameTo(game);
                result(r.kind, ok, false);
                main.post(() -> a.padGameReceived(ok));
            } else {
                PadLink.convertSave(receiveDir, phoneGame, PadLink.gameId(base));  // (another release's)
                // the game reads its save at its start: put in place now, or at the restart that follows
                boolean restart = a.gameStarted();
                try {
                    if (!restart) PadLink.applySaves(base, receiveDir);
                    else {  // complete: put in place at the restart (MainActivity.onCreate)
                        File in = new File(base, PadLink.INCOMING);
                        Backup.deleteTree(in);
                        if (!receiveDir.renameTo(in)) throw new IOException("cannot keep the saves for the restart");
                    }
                } catch (IOException e) {
                    Log.w(TAG, "GamePad host: the saves from " + phone + " failed", e);
                    Backup.deleteTree(receiveDir);
                    result(r.kind, false, false);
                    return;
                }
                result(r.kind, true, restart);
                if (restart) main.post(a::padRestartForSaves);
            }
        }

        // the last offer's bytes not taken for lack of room here, and the free bytes then (told in the result)
        private long lacking, lackFree;

        private void result(int kind, boolean ok, boolean restart) throws IOException {
            long lack = lacking, free = lackFree;
            lacking = 0;
            conn.send(PadLink.RESULT, o -> {
                o.writeInt(kind);
                o.writeBoolean(ok);
                o.writeBoolean(restart);
                o.writeLong(lack);  // (newer: older phones read only the above)
                o.writeLong(lack > 0 ? free : 0);
            });
        }

        // ---- the saves back to the phone
        private List<PadLink.Entry> backOffer;

        private void sendSavesBack() throws IOException {
            // another game's saves would offer nothing usable: an empty offer (the phone then ends)
            String mine = PadLink.gameId(a.baseDir());
            backOffer = PadLink.savesFit(phoneGame, mine) ? PadLink.saveFiles(a.baseDir(), PadLink.statesFit(phoneGame, mine))
                                                          : new java.util.ArrayList<>();
            conn.send(PadLink.OFFER, PadLink.offer(PadLink.KIND_SAVES, backOffer));
        }

        private void wanted(PadLink.Frame f) throws IOException {
            int kind = f.in.readInt();
            List<Integer> want = PadLink.readWant(f.in);
            if (kind != PadLink.KIND_SAVES || backOffer == null) return;
            List<PadLink.Entry> offer = backOffer;
            backOffer = null;
            new Thread(() -> {
                try {
                    PadLink.sendFiles(conn, kind, a.baseDir(), offer, want, null);
                } catch (IOException e) {
                    Log.w(TAG, "GamePad host: sending the saves failed", e);
                    conn.close();
                }
            }, "wwhd-pad-send").start();
        }

        // ---- the GamePad picture (main thread)
        private int picture = PadLink.PICTURE_DEFAULT;  // the phone's choice (PadLink.PICTURES)
        private boolean viewTv;  // the GamePad device shows the TV picture now (VIEW)
        private boolean hidden;  // the picture can't be seen there now (SHOWN): none encoded

        boolean special;  // drawn there, but the game's GamePad shows a screen of its own (PadHost.special): its picture

        // ---- the game's own GamePad screen as the game draws it (CAP_MIRROR: runtime drc_mirror.cpp), while it shows:
        // its textures once each (MIRROR_TEX, PNG), then the screen whenever it changed (MIRROR), at most 30 times a
        // second; sent from its own thread, so a slow network only makes the screen come less often
        private Thread mirror;
        private volatile boolean mirrorOn;
        private final java.util.Set<Integer> mirrorSent = java.util.Collections.synchronizedSet(new java.util.HashSet<>());

        synchronized void startMirror() {
            if (mirror != null) return;
            mirrorOn = true;
            Native.drcMirror(true);
            mirror = new Thread(() -> {
                java.util.zip.Deflater z = new java.util.zip.Deflater(1);
                byte[] buf = new byte[1 << 16];
                long frames = 0, bytes = 0, logAt = android.os.SystemClock.uptimeMillis() + 5000;
                while (mirrorOn && !conn.closed()) {
                    long t0 = android.os.SystemClock.uptimeMillis();
                    byte[] f = Native.drcMirrorFrame();
                    if (f != null) {
                        try {
                            sendMirrorTextures(f);
                            z.reset();
                            z.setInput(f);
                            z.finish();
                            java.io.ByteArrayOutputStream o = new java.io.ByteArrayOutputStream(f.length / 4 + 64);
                            while (!z.finished()) o.write(buf, 0, z.deflate(buf));
                            byte[] d = o.toByteArray();
                            if (!mirrorOn) break;
                            conn.send(PadLink.MIRROR, d);
                            frames++;
                            bytes += d.length;
                        } catch (IOException e) {
                            break;
                        }
                    }
                    long now = android.os.SystemClock.uptimeMillis();
                    if (now >= logAt) {
                        if (frames > 0) Log.i(TAG, "GamePad host: the game's own screen: " + frames + " pictures as orders, " + bytes / 1024 + " KB in 5 s");
                        frames = bytes = 0;
                        logAt = now + 5000;
                    }
                    try {
                        Thread.sleep(Math.max(1, 33 - (now - t0)));
                    } catch (InterruptedException e) {
                        break;
                    }
                }
                z.end();
            }, "wwhd-pad-mirror");
            mirror.start();
        }

        private void sendMirrorTextures(byte[] f) throws IOException {
            java.nio.ByteBuffer b = java.nio.ByteBuffer.wrap(f).order(java.nio.ByteOrder.LITTLE_ENDIAN);
            if (b.getInt() != 0x3152494D) return;
            b.getInt();
            int n = b.getInt();
            for (int i = 0; i < n; i++) {
                int id = b.getInt();
                if (!mirrorSent.add(id)) continue;
                byte[] png = mirrorPng(Native.drcMirrorTexture(id));
                conn.send(PadLink.MIRROR_TEX, PadLink.body(o -> {
                    o.writeInt(id);
                    o.write(png);
                }));
            }
        }

        synchronized void stopMirror() {
            if (mirror == null) return;
            mirrorOn = false;
            Native.drcMirror(false);
            mirror.interrupt();
            mirror = null;
            conn.post(PadLink.MIRROR, new byte[0]);
        }

        void startVideo() {
            if (video != null || conn.closed() || slot(role) != this) return;
            conn.post(PadLink.STATE, PadLink.body(o -> o.writeBoolean(true)));
            int want = picture > 0 ? picture : drawn && special && (caps & PadLink.CAP_MIRROR) == 0 ? SPECIAL_PICTURE : 0;
            if (want <= 0) return;  // a controller only: the GamePad picture stays on the TV (+)
            if (hidden) return;  // (not seen there now: SHOWN)
            boolean tv = role == PadLink.ROLE_SCREEN;  // (its own output: a GamePad device keeps its picture)
            // the picture in the other device's shape: the TV picture always (the game draws it so, as for this
            // device's own screen), the GamePad's when it fills a display here too (GamePad picture options)
            float aspect = tv || a.drcFill() ? shape : 16f / 9f;
            // a console with little memory (a 3 GB TV box): the GamePad picture at the GamePad's own size at most. A
            // larger one's encoder and window buffers took the memory the game's resolution needed (Tegra X1:
            // dynamic resolution held at 0.65x while a GamePad device took 2112 x 976)
            // and 15 pictures a second (Tegra X1: 26-29 fps with 30 of them, user); the screen's TV picture keeps its 30
            int p = want;
            boolean little = !tv && GraphicsPresets.ramMb(a) < MainActivity.VERY_LOW_MEMORY_MB;
            if (little && p > 1) p = 1;
            int fps = little ? Math.min(15, pictureFps()) : pictureFps();
            if (little) Log.i(TAG, "GamePad host: little memory here: the GamePad picture at " + PadLink.PICTURES[p][1] + " lines, " + fps + " a second");
            int[] size = PadLink.videoSize(PadLink.PICTURES[p][1], aspect);
            try {
                video = new Video(conn, size[0], size[1], (tv ? PadLink.SCREEN_BIT_RATES : PadLink.BIT_RATES)[p], fps);
            } catch (IOException | RuntimeException e) {
                Log.w(TAG, "GamePad host: no video encoder", e);
                return;
            }
            Log.i(TAG, "GamePad host: picture " + video.width + "x" + video.height + " to " + phone);
            Native.setOption(tv ? "tvout_window_fps" : "drc_window_fps", fps);
            if (tv) a.padTvOut(video.surface);
            else {
                Native.setOption("drc_window_tv", viewTv ? 1 : 0);
                a.padRemote(video.surface, video.width / (float) video.height);
                a.padPictureChanged();
            }
        }

        void stopVideo() {
            if (video == null) return;
            if (role == PadLink.ROLE_SCREEN) {
                a.padTvOut(null);  // the renderer lets go of the surface first
                if (screen == this || screen == null) Native.setOption("tvout_window_fps", 0);
            } else {
                a.padRemote(null, 0);
                if (pad == this || pad == null) {  // (a second display here: the GamePad's, every frame)
                    Native.setOption("drc_window_tv", 0);
                    Native.setOption("drc_window_fps", 0);
                }
            }
            video.release();
            video = null;
        }

        /** the phone chose another picture size: the video starts again with it */
        private float shape;  // the other device's screen shape (width / height; 0 unknown)

        void setPicture(int p, float aspect) {
            if (p < 0 || p >= PadLink.PICTURES.length || (p == picture && aspect == shape)) return;
            picture = p;
            shape = aspect;
            stopVideo();
            if (a.gameStarted()) startVideo();
        }
    }

    /** The GamePad picture as H.264: the renderer presents into the encoder's surface. */
    /** pictures a second to the other devices (test builds: WWHD_PAD_FPS, to compare the delays) */
    static int pictureFps() {
        String e = BuildConfig.PUBLIC_RELEASE ? null : System.getenv("WWHD_PAD_FPS");
        try {
            return e != null ? Math.max(10, Math.min(60, Integer.parseInt(e))) : PadLink.PICTURE_FPS;
        } catch (NumberFormatException ignored) {
            return PadLink.PICTURE_FPS;
        }
    }

    private static final class Video {
        final MediaCodec codec;
        final Surface surface;
        final int width, height;
        private final PadLink.Delays encodeDelays = new PadLink.Delays(150);
        private long lastDelayLog;
        private final Thread drain;
        private volatile boolean running = true;

        Video(PadLink.Conn conn, int w, int h, int bitRate, int fps) throws IOException {
            MediaCodec c = MediaCodec.createEncoderByType(MediaFormat.MIMETYPE_VIDEO_AVC);
            // some encoders take only sizes in 16-pixel blocks (854 -> 848); some refuse the low-latency keys
            // (a Tegra X1's Nvidia encoder: configureCodec error -38), then without them
            RuntimeException last = null;
            boolean done = false;
            for (int k = 0; k < 4 && !done; k++) {
                boolean plain = k >= 2;
                int tw = k % 2 == 1 ? w & ~15 : w, th = k % 2 == 1 ? h & ~15 : h;
                try {
                    if (k > 0) c.reset();
                    c.configure(format(tw, th, bitRate, fps, plain), null, null, MediaCodec.CONFIGURE_FLAG_ENCODE);
                    w = tw;
                    h = th;
                    done = true;
                    if (k > 0) Log.i(TAG, "GamePad host: encoder took " + tw + "x" + th + (plain ? " without the low-latency keys" : ""));
                } catch (RuntimeException e) {
                    last = e;
                }
            }
            if (!done) {
                c.release();
                throw last;
            }
            codec = c;
            width = w;
            height = h;
            surface = codec.createInputSurface();
            codec.start();
            drain = new Thread(() -> drain(conn), "wwhd-pad-video");
            drain.start();
            sender = new Thread(() -> send(conn), "wwhd-pad-video-send");
            sender.start();
        }

        private static MediaFormat format(int w, int h, int bitRate, int fps, boolean plain) {
            MediaFormat f = MediaFormat.createVideoFormat(MediaFormat.MIMETYPE_VIDEO_AVC, w, h);
            f.setInteger(MediaFormat.KEY_COLOR_FORMAT, MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface);
            f.setInteger(MediaFormat.KEY_BIT_RATE, bitRate);
            f.setInteger(MediaFormat.KEY_FRAME_RATE, fps);
            f.setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, 2);
            f.setLong(MediaFormat.KEY_REPEAT_PREVIOUS_FRAME_AFTER, 100_000);  // a still picture still reaches the phone
            if (plain) return f;
            f.setInteger(MediaFormat.KEY_PRIORITY, 0);  // real time
            f.setInteger(MediaFormat.KEY_LATENCY, 1);
            // no pictures that wait for later ones (B-frames): each leaves as soon as it is encoded. Test builds:
            // WWHD_PAD_ENC=plain leaves the encoder's own choices (to compare the delays)
            if (BuildConfig.PUBLIC_RELEASE || !"plain".equals(System.getenv("WWHD_PAD_ENC"))) {
                f.setInteger(MediaFormat.KEY_PROFILE, MediaCodecInfo.CodecProfileLevel.AVCProfileBaseline);
                f.setInteger(MediaFormat.KEY_MAX_B_FRAMES, 0);
                f.setInteger("vendor.qti-ext-enc-low-latency.enable", 1);  // (Qualcomm's encoders; others ignore it)
            }
            return f;
        }

        private void drain(PadLink.Conn conn) {
            MediaCodec.BufferInfo info = new MediaCodec.BufferInfo();
            try {
                while (running) {
                    int i = codec.dequeueOutputBuffer(info, 100_000);
                    if (i < 0) continue;
                    java.nio.ByteBuffer b = codec.getOutputBuffer(i);
                    if (b != null && info.size > 0) {
                        b.position(info.offset);
                        b.limit(info.offset + info.size);
                        if ((info.flags & MediaCodec.BUFFER_FLAG_CODEC_CONFIG) != 0) {
                            byte[] cfg = new byte[info.size];
                            b.get(cfg);
                            conn.send(PadLink.VIDEO_CONFIG, o -> {
                                o.writeInt(width);
                                o.writeInt(height);
                                o.writeInt(cfg.length);
                                o.write(cfg);
                            });
                        } else {
                            // the picture's time is when the renderer handed it over (the surface's timestamp,
                            // System.nanoTime's clock): its age here is the encoder's delay
                            encodeDelays.add((System.nanoTime() / 1000 - info.presentationTimeUs) / 1000);
                            long now = android.os.SystemClock.uptimeMillis();
                            if (now - lastDelayLog > 10000) {
                                lastDelayLog = now;
                                long[] d = encodeDelays.get();
                                Log.i(TAG, "GamePad host: " + width + "x" + height + " picture encoded " + d[0] + " ms after it was drawn (90%: "
                                        + d[1] + " ms)");
                            }
                            boolean key = (info.flags & MediaCodec.BUFFER_FLAG_KEY_FRAME) != 0;
                            byte[] data = new byte[info.size];
                            b.get(data);
                            queue(info.presentationTimeUs, key, data);
                        }
                    }
                    codec.releaseOutputBuffer(i, false);
                }
            } catch (IOException | IllegalStateException e) {
                if (running) conn.close();
            }
        }

        // ---- pictures to the network, by a thread of their own: at most a few wait. When the network stalls, the
        // waiting ones are dropped (old pictures are no use) and the next full picture is asked for: the frames
        // in between build on the dropped ones, so they go too until it comes. The picture catches up at once.
        private final java.util.concurrent.ArrayBlockingQueue<Object[]> outQ = new java.util.concurrent.ArrayBlockingQueue<>(3);
        private boolean untilKey;
        private int dropped;
        private Thread sender;

        private void queue(long pts, boolean key, byte[] data) {
            if (untilKey && !key) {
                dropped++;
                return;
            }
            untilKey = false;
            if (!outQ.offer(new Object[] {pts, key, data})) {
                dropped += outQ.size() + 1;
                outQ.clear();
                if (key) outQ.offer(new Object[] {pts, true, data});
                else {
                    untilKey = true;
                    keyframe();
                }
                Log.i(TAG, "GamePad host: the network is behind, " + dropped + " pictures dropped so far");
            }
        }

        private void send(PadLink.Conn conn) {
            try {
                while (running) {
                    Object[] f = outQ.poll(100, java.util.concurrent.TimeUnit.MILLISECONDS);
                    if (f != null) conn.sendVideo((Long) f[0], (Boolean) f[1], (byte[]) f[2]);
                }
            } catch (IOException | InterruptedException e) {
                if (running) conn.close();
            }
        }

        void keyframe() {
            android.os.Bundle b = new android.os.Bundle();
            b.putInt(MediaCodec.PARAMETER_KEY_REQUEST_SYNC_FRAME, 0);
            try {
                codec.setParameters(b);
            } catch (IllegalStateException ignored) {
            }
        }

        void release() {
            running = false;
            try {
                drain.join(500);
                sender.join(500);
            } catch (InterruptedException ignored) {
            }
            try {
                codec.stop();
            } catch (IllegalStateException ignored) {
            }
            codec.release();
            surface.release();
        }
    }
}
