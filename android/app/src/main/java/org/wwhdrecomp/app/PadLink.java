package org.wwhdrecomp.app;

import java.io.BufferedInputStream;
import java.io.BufferedOutputStream;
import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.io.Closeable;
import java.io.DataInputStream;
import java.io.DataOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.OutputStream;
import java.net.Socket;
import java.util.ArrayList;
import java.util.List;

/**
 * A phone or tablet as the TV's Wii U GamePad, over the local network: PadHost on the TV, PadActivity on
 * the phone. The TV announces itself by DNS-SD (SERVICE_TYPE); the phone connects over TCP. One
 * connection carries framed messages both ways, [type: byte][length: int][payload]: the phone's buttons,
 * sticks, touches and motion to the TV, the GamePad picture (H.264) to the phone, and files either way
 * (the game files from the phone, the saves to the TV and back).
 */
final class PadLink {
    private PadLink() {}

    static final String SERVICE_TYPE = "_wwhdpad._tcp";
    // a device waiting to be a screen (ScreenFinder): the console finds it and invites it (INVITE, then the usual join)
    static final String SCREEN_SERVICE_TYPE = "_wwhdscreen._tcp";
    static final String INVITE = "wwhd-invite";
    static final int VERSION = 4;
    static final int PORT = 24680;  // the TV's (another if taken; the address typed on the phone needs it)

    // phone -> TV
    static final byte HELLO = 1;     // version, phone name, token ("" the first time), code ("" until asked), game (gameId), role
    // what the other device is: the GamePad (input, the GamePad picture and sounds), or the screen (the TV picture
    // and sound; this device then shows the GamePad picture and is the controller)
    static final int ROLE_PAD = 1, ROLE_SCREEN = 2;
    static final byte INPUT = 2;     // buttons, lx, ly, rx, ry
    static final byte TOUCH = 3;     // down, u, v (0..1 on the GamePad picture)
    static final byte MOTION = 4;    // dt, gyro x y z, accelerometer x y z (SDL's controller axes, MotionInput)
    static final byte KEYFRAME = 5;  // the picture needs a full frame (decoder started or lost data)
    static final byte SAVES_BACK = 6;  // the TV offers its saves (the phone takes the newer ones)
    static final byte BYE = 7;
    static final byte SOUND = 9;     // the sound here (on) or left on the TV (off)
    static final byte KEY = 11;      // a controller key on the GamePad device: action, key code, repeat (the console's
                                     // dialogs and options get it: MainActivity.padKey)
    static final byte UI_TOUCH = 12; // a touch on the console's dialog shown here: action, u, v (0..1, PadUi)
    static final byte VIEW = 13;     // the GamePad device wants the TV picture in its video (its controller view's
                                     // background) instead of the GamePad's: on / off (older consoles ignore it)
    static final byte SHOWN = 15;    // the picture can be seen there now (off: the app away or the screen off, a console
                                     // dialog over all of it): the console encodes and sends none meanwhile
    static final byte WIND = 17;     // a swipe from the wind dial there: the game's wind angle y (MainActivity.setWind)
    static final byte OPTIONS = 16;  // the options arrow tapped there: the console's options page (shown there as a dialog)
    static final byte TEXT_ANSWER = 14;  // the game's text input answered on the GamePad device: OK, the text
    // the GamePad screen drawn there (PadScreen, picture "Drawn here"): no video, the game's state instead
    static final byte DRAWN = 46;     // on / off: the console sends GAME_STATE (and no picture)
    static final byte EQUIP = 47;     // button (0 X, 1 Y, 2 R), inventory slot: put on it (Native.equipItem)
    static final byte ICON_WANT = 48; // a GameIcons texture's name: the console sends it (ICON)
    static final byte CONDUCT = 49;   // the Wind Waker's slot: Link conducts (the item ring's way)
    static final byte CAPS = 18;     // what this device can do beyond the version (CAP_*): sent after WELCOME
    static final int CAP_UI_DRAW = 1;  // the console's dialogs as drawing orders (UI_DRAW) instead of pictures (UI_FRAME)
    static final int CAP_MIRROR = 2;   // the game's own GamePad screens as drawing orders (MIRROR) instead of video
    static final byte PING = 10;     // the phone's System.nanoTime: the TV answers at once (PONG)
    static final byte PICTURE = 8;   // the picture's size wanted (PICTURES; 0: none, a controller only), the device's screen shape
    // TV -> phone
    static final byte WELCOME = 20;    // token, TV name, has game files, game running, game (gameId), its CPU features and model,
                                       // needs its game code (compiling, or not yet)
    static final byte NEED_CODE = 21;  // the TV shows a code to type
    static final byte BAD_CODE = 22;
    static final byte VIDEO_CONFIG = 23;  // width, height, codec config (SPS / PPS)
    static final byte VIDEO = 24;         // presentation time (µs), key frame, data
    static final byte STATE = 25;         // game running (the picture comes)
    static final byte UI_FRAME = 28;      // the console's dialog on top as a picture (width, height of the dialog's
                                          // window there; 0: none any more), then the JPEG (PadUi)
    static final byte UI_DRAW = 32;       // the console's dialog on top as drawing orders (UiDraw; width, height of its
                                          // window there, 0: none any more), then the deflated orders
    static final byte UI_BITMAP = 33;     // a bitmap the orders use: its number, then UiDraw.encode's bytes (each once
                                          // per connection; sent before the orders that use it)
    static final byte GAME_STATE = 34;    // for PadScreen: the item ring's ints (count, values), the touch state's
                                          // (count, values), the save data (length, -1: unchanged), the mini map's
                                          // floats (count, values: Native.miniMapState), each only when it changed
    static final byte ICON = 35;          // a GameIcons texture: its name, PNG (empty: the console has none)
    static final byte FONT = 36;          // a GameFont's files (FONT_WANT): its name, the game language, the count, then
                                          // each file's name, length and deflated bytes (count 0: the console has none)
    static final byte LABELS = 37;        // the names PadScreen shows (GameProgress.Labels in the game's words): the
                                          // count, then each: kind (LABEL_*), key, text
    static final byte SPECIAL = 38;       // on / off: the game's GamePad shows a screen of its own the drawn one has no page
                                          // for (album, warp map, conducting, minigames ...): its picture comes as video
    static final byte INDOOR_MAP = 39;    // the map indoors (MiniMap.drawIndoorChart on the console): PNG (empty: none)
    static final byte FONT_WANT = 50;     // a GameFont's name (CKingMain, CKingMsg): the console sends it (FONT)
    static final byte MIRROR = 52;        // the game's own GamePad screen as the game draws it (runtime drc_mirror.cpp's frame,
                                          // deflated; empty: none any more), its textures sent before (MIRROR_TEX)
    static final byte MIRROR_TEX = 53;    // a texture of those: its id, then PNG (empty: the console couldn't read it)
    static final byte MIRROR_RESET = 54;  // the GamePad device dropped those textures (memory): send them anew
    static final byte SAVE_GAME = 51;     // the Save button of the drawn items page: the game's own Save (its GamePad
                                          // item page's button, tapped there)
    static final int LABEL_SECTION = 0, LABEL_ITEM = 1, LABEL_DUNGEON = 2, LABEL_ISLAND = 3, LABEL_SONG = 4,
            LABEL_FIGURINE = 5, LABEL_OWN = 6,
            LABEL_UI = 7;  // (the menus' own words: "<layout>:<pane>\t<text>")
    static final byte ARROW = 30;         // the items page's options arrow (drawn by the app, not in the picture): on, its
                                          // opacity, left top right bottom (0..1 of the GamePad picture)
    static final byte WIND_STATE = 31;    // the wind dial there: on (the game runs, a wind, Change the wind on), the way
                                          // it blows (MiniMap's st[70]), the dial's turn (degrees, the TV's mini map)
    static final byte TEXT_ASK = 29;      // the game asks for text (a name): open, then the text there, its most
                                          // letters, the message (closed: the console's own field was answered)
    static final byte PONG = 27;          // the PING's time, the TV's System.nanoTime when it answered
    static final byte AUDIO = 26;         // sound: 48 kHz stereo s16 little-endian frames (the screen: the TV sound,
                                          // the GamePad: its own sounds)
    // files, either way: OFFER (sender) -> WANT (receiver) -> FILE / CHUNK ... DONE (sender) -> RESULT (receiver)
    static final byte OFFER = 40, WANT = 41, FILE = 42, CHUNK = 43, DONE = 44, RESULT = 45;
    static final int KIND_GAME = 1, KIND_SAVES = 2;
    static final int KIND_CODE = 3;  // the game code compiled by the phone for the TV's CPU: its code cache's files

    // the GamePad picture's sizes (PadActivity's choice; the first: none) and their video bit rates (30 pictures a second)
    static final int[][] PICTURES = {{0, 0}, {854, 480}, {1280, 720}, {1920, 1080}};
    static final int[] BIT_RATES = {0, 3_000_000, 6_000_000, 10_000_000};
    static final int[] SCREEN_BIT_RATES = {0, 4_000_000, 8_000_000, 15_000_000};  // the TV picture: where you play
    static final int SCREEN_PICTURE_DEFAULT = 3;  // 1920 x 1080
    static final int PICTURE_DEFAULT = 1;  // the GamePad's own 854 x 480
    static final int PICTURE_FPS = 30;

    /**
     * The video's size for a picture of `height` lines in the shape `aspect` (width / height; 16:9 when unknown):
     * in 16-pixel blocks, and no more blocks than 1920 x 1080 has (the most many H.264 encoders take, level 4.1):
     * a wider shape gets fewer lines.
     */
    static int[] videoSize(int height, float aspect) {
        if (!(aspect > 0.5f && aspect < 4f)) aspect = 16f / 9f;
        int h = height;
        for (;;) {
            int w = Math.round(h * aspect / 16f) * 16, hh = (h + 15) / 16 * 16;
            if ((w / 16) * (hh / 16) <= 8160 || h <= 360) return new int[] {w, h & ~1};
            h -= 8;
        }
    }

    /** a swipe (dx, dy on the screen) on a wind dial turned by `turn` degrees (the mini map's, MiniMap.windAngle): the
     *  game's wind angle y (s16), one of the compass's 8 */
    static int windAngle(float dx, float dy, float turn) {
        double t = Math.toRadians(turn);
        double wx = Math.cos(t) * dx + Math.sin(t) * dy, wz = -Math.sin(t) * dx + Math.cos(t) * dy;
        return (short) (Math.round(Math.atan2(wz, wx) / (Math.PI / 4)) * 0x2000);
    }

    /** the game's wind angle y as the way MiniMap's st[70] gives it (atan2(x, z)) */
    static float windWay(int angle) { return (float) (Math.PI / 2 - angle * Math.PI / 0x8000); }

    /**
     * Wi-Fi in its low-latency mode while devices are connected (both ends): otherwise the radio dozes between
     * transmissions and single pictures arrive a beacon interval (~100 ms) late. Held only while the app is in
     * front with the screen on (Android's rule for this mode).
     */
    static final class WifiAwake {
        private final android.net.wifi.WifiManager.WifiLock lock;

        WifiAwake(android.content.Context c) {
            android.net.wifi.WifiManager w = c.getApplicationContext().getSystemService(android.net.wifi.WifiManager.class);
            lock = w == null ? null : w.createWifiLock(android.net.wifi.WifiManager.WIFI_MODE_FULL_LOW_LATENCY, "wwhd:pad");
            if (lock != null) lock.setReferenceCounted(false);
        }

        void set(boolean on) {
            if (lock == null) return;
            if (on && !lock.isHeld()) lock.acquire();
            else if (!on && lock.isHeld()) lock.release();
        }
    }

    /** Recent delays (ms) for the median and the 90th percentile: the delays between the two devices. */
    static final class Delays {
        private final long[] v;
        private int n, next;

        Delays(int size) { v = new long[size]; }

        synchronized void add(long ms) {
            v[next] = ms;
            next = (next + 1) % v.length;
            if (n < v.length) n++;
        }

        /** {median, 90th percentile, count}; median -1 without any */
        synchronized long[] get() {
            if (n == 0) return new long[] {-1, -1, 0};
            long[] s = java.util.Arrays.copyOf(v, n);
            java.util.Arrays.sort(s);
            return new long[] {s[n / 2], s[Math.min(n - 1, n * 9 / 10)], n};
        }

        synchronized void clear() { n = next = 0; }
    }

    private static final int MAX_FRAME = 16 << 20;
    static final int CHUNK_SIZE = 256 << 10;
    // saves arrive into RECEIVING; only a finished transfer becomes INCOMING (put in place at the next start where
    // the game runs meanwhile). A RECEIVING left over (the connection broke) is deleted, never used
    static final String RECEIVING = "sync-receiving", INCOMING = "sync-incoming";

    // ------------------------------------------------------------------ messages
    interface Body { void write(DataOutputStream o) throws IOException; }

    static byte[] body(Body b) {
        ByteArrayOutputStream bytes = new ByteArrayOutputStream();
        try {
            b.write(new DataOutputStream(bytes));
        } catch (IOException e) {
            throw new IllegalStateException(e);  // (in memory: never)
        }
        return bytes.toByteArray();
    }

    static final class Frame {
        final byte type;
        final byte[] data;
        final DataInputStream in;

        Frame(byte type, byte[] data) {
            this.type = type;
            this.data = data;
            in = new DataInputStream(new ByteArrayInputStream(data));
        }
    }

    /** One connection; sends from any thread (whole frames), reads on one. */
    static final class Conn implements Closeable {
        final Socket socket;
        private final DataInputStream in;
        private final DataOutputStream out;
        private volatile boolean closed;

        Conn(Socket s) throws IOException {
            socket = s;
            s.setTcpNoDelay(true);
            // a small send buffer: when the network stalls, the sender sees it at once and drops old pictures (PadHost)
            // instead of the system queueing seconds of them
            s.setSendBufferSize(256 << 10);
            in = new DataInputStream(new BufferedInputStream(s.getInputStream(), 1 << 16));
            out = new DataOutputStream(new BufferedOutputStream(s.getOutputStream(), 1 << 16));
        }

        void send(byte type, byte[] data) throws IOException { send(type, data, 0, data.length); }

        void send(byte type, Body b) throws IOException { send(type, body(b)); }

        synchronized void send(byte type, byte[] data, int off, int len) throws IOException {
            out.writeByte(type);
            out.writeInt(len);
            out.write(data, off, len);
            out.flush();
        }

        /** a picture frame without copying it twice */
        synchronized void sendVideo(long pts, boolean key, byte[] data) throws IOException {
            out.writeByte(VIDEO);
            out.writeInt(data.length + 9);
            out.writeLong(pts);
            out.writeBoolean(key);
            out.write(data);
            out.flush();
        }

        // post's own thread: callable from the main thread (Android allows no network there)
        private final java.util.concurrent.ExecutorService poster = java.util.concurrent.Executors.newSingleThreadExecutor();

        /** sends quietly and in order, from any thread (input, touches): a broken connection shows at the next read */
        void post(byte type, byte[] data) {
            if (closed) return;
            try {
                poster.execute(() -> {
                    try {
                        send(type, data);
                    } catch (IOException ignored) {
                    }
                });
            } catch (java.util.concurrent.RejectedExecutionException ignored) {
            }
        }

        Frame read() throws IOException {
            byte type = in.readByte();
            int len = in.readInt();
            if (len < 0 || len > MAX_FRAME) throw new IOException("bad frame length " + len);
            byte[] data = new byte[len];
            in.readFully(data);
            return new Frame(type, data);
        }

        boolean closed() { return closed; }

        @Override
        public void close() {
            closed = true;
            poster.shutdown();
            try {
                socket.close();
            } catch (IOException ignored) {
            }
        }
    }

    // ------------------------------------------------------------------ files
    /** a file offered: its path below the root, size and time */
    static final class Entry {
        final String path;
        final long size, time;

        Entry(String path, long size, long time) {
            this.path = path;
            this.size = size;
            this.time = time;
        }
    }

    interface Progress { void at(long done, long total); }

    /** every file below `dir` ("" for none), paths with `prefix` */
    static void list(File dir, String prefix, List<Entry> out) {
        File[] files = dir.listFiles();
        if (files == null) return;
        for (File f : files) {
            String p = prefix + f.getName();
            if (f.isDirectory()) list(f, p + "/", out);
            else out.add(new Entry(p, f.length(), f.lastModified()));
        }
    }

    /** the game files (files/game) */
    static List<Entry> gameFiles(File base) {
        List<Entry> l = new ArrayList<>();
        list(new File(base, "game"), "", l);
        return l;
    }

    /**
     * Which game this device has: its title ID and version (game/meta/meta.xml), "" without one. Saves and
     * above all save states (a picture of the game's memory) go only between devices with the same.
     */
    static String gameId(File base) {
        File meta = new File(base, "game/meta/meta.xml");
        try {
            String x = new String(java.nio.file.Files.readAllBytes(meta.toPath()), java.nio.charset.StandardCharsets.UTF_8);
            java.util.regex.Matcher id = java.util.regex.Pattern.compile("<title_id[^>]*>([0-9A-Fa-f]+)<").matcher(x),
                    v = java.util.regex.Pattern.compile("<title_version[^>]*>(\\d+)<").matcher(x);
            if (!id.find()) return "";
            return id.group(1).toUpperCase(java.util.Locale.ROOT) + "v" + (v.find() ? v.group(1) : "0");
        } catch (IOException e) {
            return "";
        }
    }

    /** the release a game ID belongs to, for the message when they differ */
    static String release(String gameId) {
        if (gameId.startsWith("0005000010143400")) return "JPN";
        if (gameId.startsWith("0005000010143500")) return "USA";
        if (gameId.startsWith("0005000010143600")) return "EUR";
        return gameId.isEmpty() ? "?" : gameId;
    }

    /**
     * Saves between devices: the game save goes between any two (another release's converted: convertSave), the
     * save states only between the same release and version (a picture of the game's memory, made by that game's
     * code: no conversion possible).
     */
    static boolean statesFit(String a, String b) { return !a.isEmpty() && a.equals(b); }

    static boolean savesFit(String a, String b) { return !a.isEmpty() && !b.isEmpty(); }

    /** the plain game's save and (`states`) its states (slots 1-5; the last session's stays with its device), paths below files/ */
    static List<Entry> saveFiles(File base, boolean states) {
        List<Entry> l = new ArrayList<>();
        list(new File(base, "save"), "save/", l);
        if (!states) return l;
        File[] stateFiles = new File(base, "states").listFiles((d, n) -> n.matches("slot[1-5]\\.(bin|png)"));
        if (stateFiles != null)
            for (File f : stateFiles) l.add(new Entry("states/" + f.getName(), f.length(), f.lastModified()));
        return l;
    }

    /** the wanted saves without states when they don't fit (the receiver's own check) */
    static List<Integer> onlyFitting(List<Entry> offered, List<Integer> want, boolean states) {
        if (states) return want;
        List<Integer> l = new ArrayList<>();
        for (int i : want) if (offered.get(i).path.startsWith("save/")) l.add(i);
        return l;
    }

    /** room kept free on the receiver beyond the files (the system and the game need some too) */
    static final long SPACE_MARGIN = 64L << 20;

    /** free bytes where `dir` is (or would be made); unknown: plenty */
    static long freeSpace(File dir) {
        File d = dir;
        while (d != null && !d.exists()) d = d.getParentFile();
        try {
            return d != null ? new android.os.StatFs(d.getAbsolutePath()).getAvailableBytes() : Long.MAX_VALUE;
        } catch (IllegalArgumentException e) {
            return Long.MAX_VALUE;
        }
    }

    static long size(List<Entry> offered, List<Integer> indices) {
        long t = 0;
        for (int i : indices) t += offered.get(i).size;
        return t;
    }

    /**
     * Of the wanted saves what fits into `free` bytes (less SPACE_MARGIN): the game save as a whole first (small),
     * then the state slots, newest first, each state with its picture. A slot that doesn't fit stays with the
     * sender (it is offered again next time). Before, a full receiver broke off mid-transfer (ENOSPC), the
     * connection ended, and the sender came back with the same offer, again and again.
     */
    static List<Integer> fitSpace(List<Entry> offered, List<Integer> want, long free) {
        long room = free - SPACE_MARGIN;
        List<Integer> save = new ArrayList<>();
        java.util.Map<String, List<Integer>> slots = new java.util.HashMap<>();
        java.util.Map<String, Long> slotTime = new java.util.HashMap<>();
        for (int i : want) {
            Entry e = offered.get(i);
            if (e.path.startsWith("save/")) save.add(i);
            else {
                int dot = e.path.lastIndexOf('.');
                String slot = dot > 0 ? e.path.substring(0, dot) : e.path;
                slots.computeIfAbsent(slot, k -> new ArrayList<>()).add(i);
                slotTime.merge(slot, e.time, Math::max);
            }
        }
        List<Integer> l = new ArrayList<>();
        long s = size(offered, save);
        if (s > room) return l;  // not even the game save: nothing
        l.addAll(save);
        room -= s;
        List<String> order = new ArrayList<>(slots.keySet());
        order.sort((a, b) -> Long.compare(slotTime.get(b), slotTime.get(a)));
        for (String slot : order) {
            long n = size(offered, slots.get(slot));
            if (n > room) continue;
            l.addAll(slots.get(slot));
            room -= n;
        }
        java.util.Collections.sort(l);
        return l;
    }

    /**
     * A game save received from another release made this one's: the file is the same in all three (a European
     * save played in the USA game), but the USA and European fonts have Latin letters only, so a Japanese
     * player name becomes "Link" there (GcSave.forLatinFont). Into Japanese: as it is. Keeps the sender's time.
     */
    static void convertSave(File incoming, String from, String to) throws IOException {
        File f = new File(incoming, "save/user/cking.sav");
        if (!f.isFile() || from.equals(to) || release(to).equals("JPN")) return;
        byte[] d = java.nio.file.Files.readAllBytes(f.toPath());
        byte[] c = GcSave.forLatinFont(d);
        if (c == null || c == d) return;
        long time = f.lastModified();
        java.nio.file.Files.write(f.toPath(), c);
        //noinspection ResultOfMethodCallIgnored
        f.setLastModified(time);
    }

    static byte[] offer(int kind, List<Entry> files) {
        return body(o -> {
            o.writeInt(kind);
            o.writeInt(files.size());
            for (Entry e : files) {
                o.writeUTF(e.path);
                o.writeLong(e.size);
                o.writeLong(e.time);
            }
        });
    }

    static List<Entry> readOffer(DataInputStream in) throws IOException {
        int n = in.readInt();
        List<Entry> l = new ArrayList<>(n);
        for (int i = 0; i < n; i++) l.add(new Entry(in.readUTF(), in.readLong(), in.readLong()));
        return l;
    }

    /**
     * The saves the receiver wants. The game save as a whole: its files belong together (a quest log's picto
     * album, its play log), so all of save/ when the sender's cking.sav is newer (or this device has none), else
     * none of it. The states per slot, its state and picture together, when the sender's state is newer. A
     * second's slack for the file systems' times.
     */
    static List<Integer> newerSaves(File base, List<Entry> offered) {
        List<Integer> want = new ArrayList<>();
        File mine = new File(base, "save/user/cking.sav");
        boolean save = false;
        for (Entry e : offered)
            if (e.path.equals("save/user/cking.sav")) save = !mine.isFile() || e.time > mine.lastModified() + 1000;
        java.util.Set<String> slots = new java.util.HashSet<>();
        for (Entry e : offered) {
            if (!e.path.startsWith("states/") || !e.path.endsWith(".bin")) continue;
            File f = safe(base, e.path);
            if (f != null && (!f.exists() || e.time > f.lastModified() + 1000)) slots.add(e.path.substring(0, e.path.length() - 4));
        }
        for (int i = 0; i < offered.size(); i++) {
            String p = offered.get(i).path;
            if (p.startsWith("save/") ? save : p.startsWith("states/") && slots.contains(p.substring(0, p.lastIndexOf('.'))))
                want.add(i);
        }
        return want;
    }

    static byte[] want(int kind, List<Integer> indices) { return want(kind, indices, 0, 0); }

    /** with the bytes not taken for lack of room and the free bytes then: the sender tells it before sending (older read only the list) */
    static byte[] want(int kind, List<Integer> indices, long lacking, long free) {
        return body(o -> {
            o.writeInt(kind);
            o.writeInt(indices.size());
            for (int i : indices) o.writeInt(i);
            o.writeLong(lacking);
            o.writeLong(lacking > 0 ? free : 0);
        });
    }

    static List<Integer> readWant(DataInputStream in) throws IOException {
        int n = in.readInt();
        List<Integer> l = new ArrayList<>(n);
        for (int i = 0; i < n; i++) l.add(in.readInt());
        return l;
    }

    /** Sends the wanted files from `root` (blocks; the connection's other messages go between the chunks). */
    static void sendFiles(Conn c, int kind, File root, List<Entry> offered, List<Integer> wanted, Progress p) throws IOException {
        long total = 0, done = 0, t0 = android.os.SystemClock.uptimeMillis();
        for (int i : wanted) total += offered.get(i).size;
        byte[] buf = new byte[CHUNK_SIZE + 4];
        for (int i : wanted) {
            Entry e = offered.get(i);
            final int index = i;
            c.send(FILE, o -> {
                o.writeInt(kind);
                o.writeInt(index);
            });
            try (FileInputStream in = new FileInputStream(new File(root, e.path))) {
                buf[0] = (byte) (kind >> 24);
                buf[1] = (byte) (kind >> 16);
                buf[2] = (byte) (kind >> 8);
                buf[3] = (byte) kind;
                for (int n; (n = in.read(buf, 4, CHUNK_SIZE)) > 0; ) {
                    c.send(CHUNK, buf, 0, n + 4);
                    done += n;
                    if (p != null) p.at(done, total);
                }
            }
        }
        c.send(DONE, o -> o.writeInt(kind));
        long ms = Math.max(1, android.os.SystemClock.uptimeMillis() - t0);
        android.util.Log.i("wwhd", "GamePad: sent " + wanted.size() + " files, " + total / 1024 + " KiB in " + ms + " ms ("
                + total * 1000 / 1024 / ms + " KiB/s)");
    }

    /** a path below `root`, or null if it would leave it */
    static File safe(File root, String path) {
        if (path.isEmpty() || path.startsWith("/") || path.contains("\\")) return null;
        for (String part : path.split("/"))
            if (part.isEmpty() || part.equals(".") || part.equals("..")) return null;
        return new File(root, path);
    }

    /** The receiving side of one transfer: files into `dir` with the sender's times. */
    static final class Receive {
        final int kind;
        final File dir;
        final List<Entry> offered;
        private final long total;
        private long done;
        private OutputStream out;
        private Entry current;
        private File currentFile;

        Receive(int kind, File dir, List<Entry> offered, List<Integer> wanted) {
            this.kind = kind;
            this.dir = dir;
            this.offered = offered;
            long t = 0;
            for (int i : wanted) t += offered.get(i).size;
            total = t;
        }

        long total() { return total; }

        long done() { return done; }

        void file(DataInputStream in) throws IOException {
            finishFile();
            int i = in.readInt();
            if (i < 0 || i >= offered.size()) throw new IOException("bad file index " + i);
            current = offered.get(i);
            currentFile = safe(dir, current.path);
            if (currentFile == null) throw new IOException("bad path " + current.path);
            //noinspection ResultOfMethodCallIgnored
            currentFile.getParentFile().mkdirs();
            out = new FileOutputStream(currentFile);
        }

        void chunk(byte[] data) throws IOException {
            if (out == null) throw new IOException("data without a file");
            out.write(data, 4, data.length - 4);
            done += data.length - 4;
        }

        void finish() throws IOException { finishFile(); }

        void abort() {
            try {
                finishFile();
            } catch (IOException ignored) {
            }
        }

        private void finishFile() throws IOException {
            if (out == null) return;
            out.close();
            out = null;
            //noinspection ResultOfMethodCallIgnored
            currentFile.setLastModified(current.time);
        }
    }

    static int chunkKind(byte[] data) {
        return (data[0] & 0xFF) << 24 | (data[1] & 0xFF) << 16 | (data[2] & 0xFF) << 8 | data[3] & 0xFF;
    }

    /**
     * Puts saves received into `incoming` (save/..., states/...) in place under `base`. The game save
     * as a whole: the one replaced stays in save-before-sync. Not while the game runs on this device.
     */
    static void applySaves(File base, File incoming) throws IOException {
        List<Entry> got = new ArrayList<>();
        list(incoming, "", got);
        android.util.Log.i("wwhd", "GamePad: putting " + got.size() + " saved files from " + incoming.getName() + " in place");
        if (got.isEmpty()) {
            Backup.deleteTree(incoming);
            return;
        }
        boolean save = false, states = false;
        for (Entry e : got) {
            save |= e.path.startsWith("save/");
            states |= e.path.startsWith("states/");
        }
        if (save) {  // the game save as a whole (newerSaves): the current one moves aside, kept once; the received one is it
            File cur = new File(base, "save"), keep = new File(base, "save-before-sync"), in = new File(incoming, "save");
            Backup.deleteTree(keep);
            if (cur.exists() && !cur.renameTo(keep)) throw new IOException("cannot move the save aside");
            if (!in.renameTo(cur)) {
                //noinspection ResultOfMethodCallIgnored
                keep.renameTo(cur);
                throw new IOException("cannot put the received save in place");
            }
        }
        // the states replaced are moved aside (no copy: a quarter of a gigabyte each), those of the last sync go
        File keptStates = new File(base, "states-before-sync");
        if (states) Backup.deleteTree(keptStates);
        for (Entry e : got) {
            if (e.path.startsWith("save/")) continue;  // (in place as a whole above)
            File src = new File(incoming, e.path), dst = safe(base, e.path);
            if (dst == null) continue;
            //noinspection ResultOfMethodCallIgnored
            dst.getParentFile().mkdirs();
            if (e.path.startsWith("states/") && dst.exists()) {
                //noinspection ResultOfMethodCallIgnored
                keptStates.mkdirs();
                //noinspection ResultOfMethodCallIgnored
                dst.renameTo(new File(keptStates, dst.getName()));
            }
            //noinspection ResultOfMethodCallIgnored
            dst.delete();
            if (!src.renameTo(dst)) throw new IOException("cannot write " + dst);
            //noinspection ResultOfMethodCallIgnored
            dst.setLastModified(e.time);
        }
        android.util.Log.i("wwhd", "GamePad: " + got.size() + " saved files put in place" + (save ? " (the save before: save-before-sync)" : ""));
        Backup.deleteTree(incoming);
    }

    static String sizeText(long bytes) {
        return bytes >= 1L << 30 ? String.format(java.util.Locale.ROOT, "%.1f GB", bytes / (double) (1L << 30))
                                 : String.format(java.util.Locale.ROOT, "%d MB", Math.round(bytes / (double) (1L << 20)));
    }
}
