package org.wwhdrecomp.app;

import android.content.SharedPreferences;
import android.graphics.BitmapShader;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.PorterDuff;
import android.graphics.PorterDuffColorFilter;
import android.graphics.RectF;
import android.graphics.Shader;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.util.Log;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.widget.FrameLayout;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.TextView;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.Map;
import java.util.Set;
import java.util.regex.Pattern;

/**
 * Pages of save files on the file select: each page is a set of the game's three files, for the game as it is
 * (randomized and online games have as many games of their own as one likes).
 *
 * The page in use is always in the usual places (save/, states/, the per-file settings): everything that reads a save
 * stays as it was. The other pages are parked in save-pages/<n>/. A turn: the game holds at a frame start while the file
 * list shows (mods.h save_page_*), the folders and per-file settings change places here, then the game reads the save
 * again with its own load and makes the three panels again. Page 1 is what was there before pages existed.
 *
 * A journal (save-pages/turn.txt) covers a crash in a turn: while the folders move it is undone at the next start,
 * after that it is finished.
 */
final class SavePages {
    static final int MAX_PAGES = 99;  // (as many as made with the bar's +)
    private static final String PREF_COUNT = "save_pages_count";
    private static final String PREF_ON = "save_pages_on", PREF_PAGE = "save_page", PREF_VIEW = "save_page_view", TAG = "wwhd";
    private static volatile boolean viewingOther;
    // the settings kept per save file 1..3 (online_files_p: the marks of the game as it is)
    private static final Pattern PER_FILE = Pattern.compile("(player_model|save_name_long|link_colors_on|link_colors_separate|loot_tier"
            + "|loot_chests_open|loot_quick_\\d+|loot_owned_[a-z]+|loot_[a-z]+_tier|loot_(?!test)[a-z]+)_[123]|loot_(w|broken)_[123]_.*"
            + "|pack_f[123]_.*|online_files_p|packs_slots_used|online_slots_used_p");  // (the last two: which files hold a game)

    private final MainActivity a;
    private final Handler handler = new Handler(Looper.getMainLooper());
    private LinearLayout bar;
    private ImageView prev, next;
    private TextView label, plus;
    private boolean busy, starting;
    private int front = -1;
    // the pages the file select turns through: the game as it is (its pages; with pages off only the one in use), then each
    // online game and each randomized game (their own save each). {kind, n}
    private static final int BASE = 0, ONLINE = 1, RANDO = 2;
    private int[] shownEntry;  // another game's page showing, null: the running game's own
    private long shownAt;  // (when: the runtime takes the folder at its next frame)

    SavePages(MainActivity a) { this.a = a; }

    private static File root(MainActivity a) { return new File(a.baseDir(), "save-pages"); }
    private static File journal(MainActivity a) { return new File(root(a), "turn.txt"); }
    private static SharedPreferences store(MainActivity a, int page) { return a.getSharedPreferences("save_page_" + page, 0); }
    private static SharedPreferences goals(MainActivity a, String suffix) { return a.getSharedPreferences("achievements_files" + suffix, 0); }

    static int page(MainActivity a) { return Math.max(1, Math.min(MAX_PAGES, a.prefs.getInt(PREF_PAGE, 1))); }

    /** the pages made so far: 1 at first, one more with each + (from before: up to the last one with a game, or in use) */
    static int count(MainActivity a) {
        int n = a.prefs.getInt(PREF_COUNT, 0);
        if (n > 0) return Math.max(n, Math.max(page(a), viewed(a)));
        n = Math.max(page(a), viewed(a));
        for (int p = n + 1; p <= MAX_PAGES; p++) {
            String[] names = GcSave.fileNames(readSave(a, p));
            if (names != null && (names[0] != null || names[1] != null || names[2] != null)) n = p;
        }
        a.prefs.edit().putInt(PREF_COUNT, n).apply();
        return n;
    }

    private boolean available() { return a.prefs != null && (a.prefs.getBoolean(PREF_ON, true) || entries().size() > 1); }

    private java.util.List<int[]> entries() {
        java.util.List<int[]> r = new java.util.ArrayList<>();
        if (a.prefs.getBoolean(PREF_ON, true)) for (int p = 1; p <= count(a); p++) r.add(new int[] {BASE, p});
        else r.add(new int[] {BASE, page(a)});
        for (int n : OnlineGames.games(a)) r.add(new int[] {ONLINE, n});
        for (int n : Randomizer.games(a)) r.add(new int[] {RANDO, n});
        return r;
    }

    private int[] running() {
        if (OnlineGames.playing(a)) return new int[] {ONLINE, OnlineGames.playingGame(a)};
        if (Randomizer.playing(a)) return new int[] {RANDO, Randomizer.playingGame(a)};
        return new int[] {BASE, page(a)};
    }

    private int[] showing() { return shownEntry != null ? shownEntry : running(); }

    private String entryLabel(int[] e) {
        if (e[0] == ONLINE) return a.getString(R.string.online_game_pick, OnlineGames.name(a, e[1]));
        if (e[0] == RANDO) return Randomizer.name(a, e[1]);
        return a.prefs.getBoolean(PREF_ON, true) ? a.getString(R.string.save_page, e[1], count(a)) : a.getString(R.string.rando_pick_plain);
    }

    /** the save folder of a page */
    private File saveDir(int[] e) {
        if (e[0] == ONLINE) return new File(OnlineGames.gameDir(a, e[1]), "save");
        if (e[0] == RANDO) return new File(Randomizer.gameDir(a, e[1]), "save");
        return e[1] == page(a) ? new File(a.baseDir(), "save") : new File(new File(root(a), String.valueOf(e[1])), "save");
    }

    static boolean shown(MainActivity a) {
        return a.prefs != null && a.prefs.getBoolean(PREF_ON, true) && !Randomizer.playing(a) && !OnlineGames.playing(a);
    }

    // ---- what the per-file settings pages say: "File 1 \u00B7 Lunk", "Page 5 \u00B7 File 1 \u00B7 Lunk"

    /** the labels of the files of the page the settings show; a file without a game says so */
    static String[] fileLabels(MainActivity a, int n) {
        String[] names = names(a);
        String[] r = new String[n];
        for (int i = 0; i < n; i++) r[i] = label(a, i + 1, names);
        return r;
    }

    /** what a setting of save file `file` changes */
    static String target(MainActivity a, int file) {
        String l = label(a, file, names(a));
        return shown(a) ? a.getString(R.string.file_page, viewed(a)) + " \u00B7 " + l : l;
    }

    private static String label(MainActivity a, int file, String[] names) {
        String f = a.getString(R.string.look_file_n, file);
        if (names == null || file > names.length) return f;
        String longName = a.prefs.getString(SaveNames.pref(file), null);
        String n = names[file - 1] == null ? a.getString(R.string.file_empty) : longName != null ? longName : names[file - 1];
        return f + " \u00B7 " + n;
    }

    private static String[] names(MainActivity a) {
        int v = viewed(a);
        String[] n = GcSave.fileNames(readSave(a, v));
        return n == null && v != page(a) ? new String[3] : n;  // (a page not used yet: three empty files)
    }

    // ---- Game > Save file pages > the pages: what each holds; a page deleted

    /** "Lunk · empty · empty": the page's three files */
    static String pageSummary(MainActivity a, int p) {
        String[] names = GcSave.fileNames(readSave(a, p));
        if (names == null) names = new String[3];
        StringBuilder b = new StringBuilder();
        for (int i = 0; i < 3; i++) {
            String n = names[i] == null ? a.getString(R.string.file_empty) : p == page(a) && a.prefs.getString(SaveNames.pref(i + 1), null) != null
                    ? a.prefs.getString(SaveNames.pref(i + 1), null) : names[i];
            b.append(i > 0 ? " \u00B7 " : "").append(n);
        }
        return b.toString();
    }

    /** why page p can't be deleted now (a text id), 0: it can */
    static int whyNotDelete(MainActivity a, int p) {
        if (p == page(a)) return R.string.pages_in_use;
        if (viewing() || Native.savePageForeign()) return R.string.pages_busy;
        return 0;
    }

    /** page p deleted with its save files, save states and per-file settings; the pages after it move up by one */
    static boolean deletePage(MainActivity a, int p) {
        if (whyNotDelete(a, p) != 0) return false;
        final int n = count(a);
        File profiles = new File(new File(a.getFilesDir(), "online"), "profiles");
        Backup.deleteTree(new File(root(a), String.valueOf(p)));
        store(a, p).edit().clear().commit();
        goals(a, "_p" + p).edit().clear().commit();
        for (int f = 1; f <= 3; f++) new File(profiles, "p-f" + f + ".ini.page" + p).delete();
        for (int q = p + 1; q <= n; q++) {  // (each after it one down)
            File from = new File(root(a), String.valueOf(q));
            if (from.exists() && !from.renameTo(new File(root(a), String.valueOf(q - 1)))) Log.w(TAG, "save pages: page " + q + " not moved");
            moveStore(store(a, q), store(a, q - 1));
            moveStore(goals(a, "_p" + q), goals(a, "_p" + (q - 1)));
            for (int f = 1; f <= 3; f++) {
                File ini = new File(profiles, "p-f" + f + ".ini.page" + q);
                if (ini.exists()) ini.renameTo(new File(profiles, "p-f" + f + ".ini.page" + (q - 1)));
            }
        }
        SharedPreferences.Editor e = a.prefs.edit().putInt(PREF_COUNT, Math.max(1, n - 1));
        if (page(a) > p) e.putInt(PREF_PAGE, page(a) - 1);
        e.commit();
        Log.i(TAG, "save pages: page " + p + " deleted, " + Math.max(1, n - 1) + " left");
        return true;
    }

    private static void moveStore(SharedPreferences from, SharedPreferences to) {
        SharedPreferences.Editor e = to.edit().clear();
        for (Map.Entry<String, ?> k : from.getAll().entrySet()) put(e, k.getKey(), k.getValue());
        e.commit();
        from.edit().clear().commit();
    }

    /** the labels of the Page row of the per-file settings */
    static String[] pageLabels(MainActivity a) {
        String[] r = new String[count(a)];
        for (int p = 1; p <= r.length; p++)
            r[p - 1] = a.getString(R.string.file_page, p) + (p == page(a) ? " \u00B7 " + a.getString(R.string.file_in_use) : "");
        return r;
    }

    // ---- the per-file settings of another page, the game staying on its own: only those settings change places (as in a
    // turn) while the settings page shows, and nothing of them goes to the runtime meanwhile (viewing())

    /** the page whose per-file settings are in place now */
    static int viewed(MainActivity a) {
        int v = a.prefs.getInt(PREF_VIEW, 0);
        return v > 0 ? v : page(a);
    }

    static boolean viewing() { return viewingOther; }

    static void view(MainActivity a, int to) {
        boolean back;
        synchronized (LinkColors.class) {  // (not while Link's colours are being made)
            int holder = viewed(a);
            if (to == holder) return;
            try {
                root(a).mkdirs();
                try (FileOutputStream o = new FileOutputStream(journal(a))) {
                    o.write((holder + " " + to + " 3").getBytes(StandardCharsets.US_ASCII));
                }
            } catch (IOException e) {
                Log.w(TAG, "save pages: " + e);
                return;
            }
            moveSettings(a, holder, to, true);
            journal(a).delete();
            back = to == page(a);
            viewingOther = !back;
        }
        if (back) handOverAll(a);
    }

    static void endView(MainActivity a) {
        if (a.prefs != null && viewed(a) != page(a)) view(a, page(a));
    }

    private static void handOverAll(MainActivity a) {
        PlayerModels.handOver(a);
        SaveNames.handOver(a);
        Loot.handOver(a);
        LinkColors.remakeSoon(a);
    }

    private static byte[] readSave(MainActivity a, int p) {
        try {
            File dir = p == page(a) ? a.playDir() : new File(root(a), String.valueOf(p));
            return Files.readAllBytes(new File(dir, "save/user/cking.sav").toPath());
        } catch (IOException e) {
            return null;
        }
    }

    // ---- the bar on the file select

    void addTo(FrameLayout root) {
        bar = new LinearLayout(a);
        bar.setOrientation(LinearLayout.HORIZONTAL);
        bar.setGravity(Gravity.CENTER_VERTICAL);
        bar.setBackground(new SlotStrip(a));
        prev = arrow(true, v -> turn(-1));
        next = arrow(false, v -> turn(1));
        plus = GameUi.text(a, true);
        plus.setText("+");
        plus.setGravity(Gravity.CENTER);
        plus.setTextColor(0xFF1F5E14);
        plus.setTextSize(TypedValue.COMPLEX_UNIT_SP, 26);
        plus.setBackground(GameUi.states(new GameUi.GameValueButton(a, true, false), new GameUi.GameValueButton(a, true, true)));
        plus.setOnClickListener(v -> addPage());
        plus.setClickable(true);
        label = GameUi.text(a, true);
        label.setGravity(Gravity.CENTER);
        label.setTextColor(0xFF5E7F3F);  // (as the file slots' writing)
        label.setTextSize(TypedValue.COMPLEX_UNIT_SP, 20);
        bar.addView(prev, new LinearLayout.LayoutParams(0, -1));
        bar.addView(label, new LinearLayout.LayoutParams(0, -2, 1));
        bar.addView(next, new LinearLayout.LayoutParams(0, -1));
        bar.addView(plus, new LinearLayout.LayoutParams(0, -1));
        bar.setVisibility(View.GONE);
        root.addView(bar, new FrameLayout.LayoutParams(0, 0, Gravity.BOTTOM | Gravity.CENTER_HORIZONTAL));
    }

    private ImageView arrow(boolean left, View.OnClickListener click) {
        ImageView v = new ImageView(a);
        v.setBackground(GameUi.states(new GameUi.GameValueButton(a, true, false), new GameUi.GameValueButton(a, true, true)));
        v.setImageDrawable(new GameUi.OptionArrow(a, left, 0xFF1F5E14));
        v.setOnClickListener(click);
        v.setClickable(true);
        return v;
    }

    // the strip below the three files: as wide as they are, in the picture's units (any screen shape)
    private void place() {
        View root = (View) bar.getParent();
        int w = root.getWidth(), h = root.getHeight();
        if (w <= 0 || h <= 0) return;
        float pw = Math.min(w, h * 16f / 9f), ph = pw * 9f / 16f;
        int bw = Math.round(pw * 0.636f), bh = Math.round(ph * 0.112f), m = Math.round((h - ph) / 2 + ph * 0.05f);
        FrameLayout.LayoutParams lp = (FrameLayout.LayoutParams) bar.getLayoutParams();
        if (lp.width == bw && lp.height == bh && lp.bottomMargin == m) return;
        lp.width = bw;
        lp.height = bh;
        lp.bottomMargin = m;
        bar.setLayoutParams(lp);
        int pad = Math.round(bh * 0.14f), arrowW = Math.round(bh * 1.7f);
        bar.setPadding(pad * 2, pad, pad * 2, pad);
        for (View v : new View[] {prev, next, plus}) {
            LinearLayout.LayoutParams alp = (LinearLayout.LayoutParams) v.getLayoutParams();
            alp.width = v == plus ? Math.round(bh * 1.1f) : arrowW;
            alp.leftMargin = v == plus ? pad : 0;
            v.setLayoutParams(alp);
            if (v != plus) v.setPadding(arrowW / 3, pad, arrowW / 3, pad * 2);
            else v.setPadding(0, 0, 0, pad);
        }
    }

    /** every quarter second with the front screen; the bar shows while the three files are being chosen from */
    void poll(int f, boolean controller) {
        front = f;
        if (bar == null) return;
        if (shownEntry != null && !Native.savePageForeign() && !busy && SystemClock.uptimeMillis() - shownAt > 2000)
            shownEntry = null;  // (a save state loaded: the game's own)
        if (!starting && shownEntry != null) {
            int file = Native.savePageStart();
            if (file > 0) startOther(file);
        }
        boolean show = f == Native.FRONT_FILES && available() && Native.savePageReady();
        bar.setVisibility(show ? View.VISIBLE : View.GONE);
        if (!show) return;
        plus.setVisibility(a.prefs.getBoolean(PREF_ON, true) && count(a) < MAX_PAGES ? View.VISIBLE : View.GONE);
        place();
        bar.bringToFront();
        String page = entryLabel(showing());
        label.setTextSize(TypedValue.COMPLEX_UNIT_SP, page.length() > 18 ? 16 : 20);
        label.setText(controller ? "L    " + page + "    R" : page);
        plus.setText(controller ? "+ X" : "+");
        plus.setTextSize(TypedValue.COMPLEX_UNIT_SP, controller ? 20 : 26);
        LinearLayout.LayoutParams plp = (LinearLayout.LayoutParams) plus.getLayoutParams();
        int pw = Math.round(bar.getLayoutParams().height * (controller ? 1.6f : 1.1f));
        if (plp.width != pw) {
            plp.width = pw;
            plus.setLayoutParams(plp);
        }
    }

    /** the file slots' look: their green paper with its torn white edge */
    private static final class SlotStrip extends android.graphics.drawable.Drawable {
        private final MainActivity a;
        private final Paint fill = new Paint(Paint.ANTI_ALIAS_FLAG | Paint.FILTER_BITMAP_FLAG), edge = new Paint(Paint.ANTI_ALIAS_FLAG),
                shadow = new Paint(Paint.ANTI_ALIAS_FLAG);

        SlotStrip(MainActivity a) { this.a = a; }

        private Path outline(RectF r, float step, float rough) {
            java.util.Random rnd = new java.util.Random(7);
            Path p = new Path();
            p.moveTo(r.left, r.top);
            for (float x = r.left + step; x < r.right; x += step) p.lineTo(x, r.top + rnd.nextFloat() * rough);
            p.lineTo(r.right, r.top);
            for (float y = r.top + step; y < r.bottom; y += step) p.lineTo(r.right - rnd.nextFloat() * rough, y);
            p.lineTo(r.right, r.bottom);
            for (float x = r.right - step; x > r.left; x -= step) p.lineTo(x, r.bottom - rnd.nextFloat() * rough);
            p.lineTo(r.left, r.bottom);
            for (float y = r.bottom - step; y > r.top; y -= step) p.lineTo(r.left + rnd.nextFloat() * rough, y);
            p.close();
            return p;
        }

        @Override
        public void draw(Canvas c) {
            float d = GameUi.px(a, 1);
            RectF r = new RectF(getBounds());
            r.inset(3 * d, 3 * d);
            Path p = outline(r, 5 * d, 2.2f * d);
            shadow.setColor(0x40000000);
            c.save();
            c.translate(1.5f * d, 2.5f * d);
            c.drawPath(p, shadow);
            c.restore();
            android.graphics.Bitmap paper = GameIcons.get(a, "ItemBasePattern_01^r", () -> a.runOnUiThread(this::invalidateSelf));
            fill.setShader(null);
            fill.setColorFilter(null);
            fill.setColor(0xFFB0DA77);  // (measured on the slots)
            c.drawPath(p, fill);
            if (paper != null) {
                BitmapShader bs = new BitmapShader(paper, Shader.TileMode.MIRROR, Shader.TileMode.MIRROR);
                fill.setShader(bs);
                fill.setColor(0xFFFFFFFF);
                fill.setColorFilter(new PorterDuffColorFilter(0xFFC2F183, PorterDuff.Mode.MULTIPLY));  // (the paper darkens by ~0.9: B0DA77 on the screen)
                c.drawPath(p, fill);
            }
            edge.setStyle(Paint.Style.STROKE);
            edge.setStrokeWidth(2 * d);
            edge.setColor(0xFFEEF2DB);
            c.drawPath(p, edge);
        }

        @Override public void setAlpha(int alpha) {}
        @Override public void setColorFilter(android.graphics.ColorFilter cf) {}
        @Override public int getOpacity() { return android.graphics.PixelFormat.TRANSLUCENT; }
    }

    /** a controller's L / R on the file select: true when taken */
    boolean key(int code, int bits, boolean down, boolean first) {
        // (the Wii U buttons the controller's map gives; L / R turn, X makes a page as + does)
        boolean l = (bits & Native.L) != 0 || bits == 0 && code == android.view.KeyEvent.KEYCODE_BUTTON_L1;
        boolean r = (bits & Native.R) != 0 || bits == 0 && code == android.view.KeyEvent.KEYCODE_BUTTON_R1;
        boolean x = (bits & Native.X) != 0 && plus.getVisibility() == View.VISIBLE;
        if (!l && !r && !x || front != Native.FRONT_FILES || !available() || !Native.savePageReady()) return false;
        if (down && first) {
            if (x) addPage();
            else turn(l ? -1 : 1);
        }
        return true;
    }

    // ---- a turn

    private void turn(int step) {
        if (busy || starting || viewing() || front != Native.FRONT_FILES || !available()) return;
        java.util.List<int[]> list = entries();
        int[] cur = showing();
        int at = 0;
        for (int i = 0; i < list.size(); i++) if (list.get(i)[0] == cur[0] && list.get(i)[1] == cur[1]) at = i;
        turnTo(list.get((at + step + list.size()) % list.size()));
    }

    /** the bar's +: a new page of the game as it is, turned to at once */
    private void addPage() {
        if (busy || starting || viewing() || front != Native.FRONT_FILES || a.prefs == null || !a.prefs.getBoolean(PREF_ON, true)) return;
        if (count(a) >= MAX_PAGES) return;
        int n = count(a) + 1;
        a.prefs.edit().putInt(PREF_COUNT, n).apply();
        Log.i(TAG, "save pages: page " + n + " made");
        turnTo(new int[] {BASE, n});
    }

    private void turnTo(final int[] to) {
        if (busy || starting || viewing() || front != Native.FRONT_FILES) return;
        final int[] cur = showing();
        busy = true;
        final long asked = SystemClock.uptimeMillis();
        Native.savePageAsk();
        handler.post(new Runnable() {
            @Override
            public void run() {
                int state = Native.savePageState();
                if (state == 2) {  // the game holds
                    int[] run = running();
                    boolean ok = true;
                    String dir = "";
                    if (to[0] == BASE && run[0] == BASE) {  // (the game as it is, its own pages: they change places)
                        if (to[1] != page(a)) ok = change(page(a), to[1]);
                    } else if (to[0] != run[0] || to[1] != run[1]) {  // (another game's: read where it is)
                        File d = saveDir(to);
                        ok = new File(d, "user/cking.sav").isFile() || blankSave(a, d, homeSave());
                        dir = d.getAbsolutePath();
                    }
                    Native.savePageMoved(ok, dir);
                    if (ok) shownEntry = dir.isEmpty() ? null : to;
                    shownAt = SystemClock.uptimeMillis();
                    busy = false;
                    Log.i(TAG, "save pages: " + cur[0] + "/" + cur[1] + " -> " + to[0] + "/" + to[1] + (ok ? "" : " failed"));
                    poll(front, false);
                } else if (state != 1) busy = false;
                else if (SystemClock.uptimeMillis() - asked > 1500 && Native.savePageCancel()) busy = false;  // (the list isn't showing)
                else handler.postDelayed(this, 8);
            }
        });
    }

    private byte[] homeSave() {
        try {
            return Files.readAllBytes(new File(a.playDir(), "save/user/cking.sav").toPath());
        } catch (IOException e) {
            return null;
        }
    }

    /** another game's file chosen on its page: that game started, straight into the file (as for mod packs per file) */
    private void startOther(int file) {
        int[] e = shownEntry;
        if (e == null) {
            Native.savePageStartBack();
            return;
        }
        starting = true;
        Log.i(TAG, "save pages: file " + file + " of " + e[0] + "/" + e[1] + ": restarting into it");
        if (e[0] == ONLINE) OnlineGames.choose(a, e[1]);
        else if (e[0] == RANDO) Randomizer.choose(a, e[1]);
        else {
            Randomizer.choose(a, 0);
            if (e[1] != page(a) && !change(page(a), e[1])) {  // (its page into place first; the runtime doesn't read it now)
                starting = false;
                Native.savePageStartBack();
                return;
            }
        }
        a.restartIntoFile(file, entryLabel(e));
    }

    private boolean change(int from, int to) {
        try {
            root(a).mkdirs();
            writeJournal(from, to, 1);
            if (!moveFolders(from, to)) {
                journal(a).delete();
                return false;
            }
            writeJournal(from, to, 2);
            moveSettings(a, from, to, false);
            journal(a).delete();
        } catch (IOException e) {
            Log.w(TAG, "save pages: " + e);
            return false;
        }
        handOverAll(a);
        return true;
    }

    private void writeJournal(int from, int to, int stage) throws IOException {
        try (FileOutputStream o = new FileOutputStream(journal(a))) {
            o.write((from + " " + to + " " + stage).getBytes(StandardCharsets.US_ASCII));
            o.getFD().sync();
        }
    }

    private boolean moveFolders(int from, int to) {
        File base = a.baseDir(), f = new File(root(a), String.valueOf(from)), t = new File(root(a), String.valueOf(to));
        f.mkdirs();
        t.mkdirs();
        File save = new File(base, "save"), states = new File(base, "states");
        File parkedSave = new File(f, "save"), parkedStates = new File(f, "states"), inSave = new File(t, "save"), inStates = new File(t, "states");
        if (parkedSave.exists() || parkedStates.exists()) return false;  // (never over a parked page)
        byte[] like = null;
        try {
            like = Files.readAllBytes(new File(save, "user/cking.sav").toPath());
        } catch (IOException ignored) {
        }
        if (!save.renameTo(parkedSave)) return false;
        if (states.exists() && !states.renameTo(parkedStates)) {
            parkedSave.renameTo(save);
            return false;
        }
        boolean ok = inSave.exists() ? inSave.renameTo(save) : blankSave(a, save, like);
        if (ok) ok = inStates.exists() ? inStates.renameTo(states) : states.mkdirs() || states.isDirectory();
        if (!ok) {
            if (save.exists() && !inSave.exists()) save.renameTo(inSave);
            if (states.exists() && !inStates.exists()) states.renameTo(inStates);
            parkedSave.renameTo(save);
            parkedStates.renameTo(states);
        }
        return ok;
    }

    private static boolean blankSave(MainActivity a, File save, byte[] like) {
        File user = new File(save, "user");
        user.mkdirs();
        try (FileOutputStream o = new FileOutputStream(new File(user, "cking.sav")); java.io.InputStream in = a.getAssets().open("save_blank_slot.bin")) {
            byte[] template = new byte[0xA94];
            new java.io.DataInputStream(in).readFully(template);
            o.write(GcSave.blankCkingSav(like, template));
            o.getFD().sync();
            return true;
        } catch (IOException e) {
            Log.w(TAG, "save pages: no new page: " + e);
            //noinspection ResultOfMethodCallIgnored
            new File(user, "cking.sav").delete();  // (only what was made: another game's pictures stay)
            return false;
        }
    }

    private static void moveSettings(MainActivity a, int from, int to, boolean view) {
        File base = a.baseDir(), profiles = new File(new File(a.getFilesDir(), "online"), "profiles");
        File f = new File(new File(root(a), String.valueOf(from)), "extra"), t = new File(new File(root(a), String.valueOf(to)), "extra");
        f.mkdirs();
        t.mkdirs();
        for (int n = 1; n <= 3; n++) {
            extra(new File(base, "packs/enabled_" + n + ".txt"), new File(f, "packs.enabled_" + n), new File(t, "packs.enabled_" + n));
            extra(new File(base, "link-colours/file" + n), new File(f, "colours.file" + n), new File(t, "colours.file" + n));
            extra(new File(base, "link-colours/file" + n + ".bin"), new File(f, "colours.bin" + n), new File(t, "colours.bin" + n));
            extra(new File(base, "link-colours/file" + n + ".stamp"), new File(f, "colours.stamp" + n), new File(t, "colours.stamp" + n));
            String ini = "p-f" + n + ".ini";  // (another file system than save-pages/: parked beside it)
            extra(new File(profiles, ini), new File(profiles, ini + ".page" + from), new File(profiles, ini + ".page" + to));
        }
        if ((view ? viewed(a) : a.prefs.getInt(PREF_PAGE, 1)) == to) return;  // (finished at a start: the settings had changed places)
        SharedPreferences.Editor park = store(a, from).edit().clear(), e = a.prefs.edit();
        for (Map.Entry<String, ?> k : a.prefs.getAll().entrySet())
            if (PER_FILE.matcher(k.getKey()).matches()) {
                put(park, k.getKey(), k.getValue());
                e.remove(k.getKey());
            }
        for (Map.Entry<String, ?> k : store(a, to).getAll().entrySet()) put(e, k.getKey(), k.getValue());
        park.commit();
        if (view) e.putInt(PREF_VIEW, to == page(a) ? 0 : to).commit();
        else e.putInt(PREF_PAGE, to).commit();
        store(a, to).edit().clear().commit();
        if (view) return;  // (the goals belong to the game being played)
        // the goals earned per save file ("2:Link/goal"): the page's own
        SharedPreferences now = goals(a, ""), keep = goals(a, "_p" + from), back = goals(a, "_p" + to);
        SharedPreferences.Editor kept = keep.edit().clear(), cur = now.edit().clear();
        for (Map.Entry<String, ?> k : now.getAll().entrySet()) put(kept, k.getKey(), k.getValue());
        for (Map.Entry<String, ?> k : back.getAll().entrySet()) put(cur, k.getKey(), k.getValue());
        kept.commit();
        cur.commit();
        back.edit().clear().commit();
    }

    private static void extra(File active, File parked, File incoming) {
        if (active.exists()) {
            Backup.deleteTree(parked);
            if (!active.renameTo(parked)) Log.w(TAG, "save pages: " + active + " not parked");
        }
        if (incoming.exists()) {
            File dir = active.getParentFile();
            if (dir != null) dir.mkdirs();
            if (!incoming.renameTo(active)) Log.w(TAG, "save pages: " + incoming + " not brought back");
        }
    }

    @SuppressWarnings("unchecked")
    private static void put(SharedPreferences.Editor e, String k, Object v) {
        if (v instanceof String) e.putString(k, (String) v);
        else if (v instanceof Integer) e.putInt(k, (Integer) v);
        else if (v instanceof Boolean) e.putBoolean(k, (Boolean) v);
        else if (v instanceof Long) e.putLong(k, (Long) v);
        else if (v instanceof Float) e.putFloat(k, (Float) v);
        else if (v instanceof Set) e.putStringSet(k, (Set<String>) v);
    }

    // ---- a turn a crash cut short (before the game starts)

    static void recover(MainActivity a) {
        recoverTurn(a);
        if (a.prefs != null && viewed(a) != page(a)) moveSettings(a, viewed(a), page(a), true);  // (the app ended on another page's settings)
    }

    private static void recoverTurn(MainActivity a) {
        File j = journal(a);
        if (!j.isFile()) return;
        try {
            String[] p = new String(Files.readAllBytes(j.toPath()), StandardCharsets.US_ASCII).trim().split(" ");
            int from = Integer.parseInt(p[0]), to = Integer.parseInt(p[1]), stage = Integer.parseInt(p[2]);
            Log.w(TAG, "save pages: the turn " + from + " -> " + to + " was cut short (stage " + stage + ")");
            if (stage >= 2) moveSettings(a, from, to, stage == 3);
            else undoFolders(a, from, to);
        } catch (IOException | RuntimeException e) {
            Log.w(TAG, "save pages: no recovery: " + e);
            return;  // (the journal stays for a look)
        }
        j.delete();
    }

    private static void undoFolders(MainActivity a, int from, int to) {
        File base = a.baseDir(), f = new File(root(a), String.valueOf(from)), t = new File(root(a), String.valueOf(to));
        undoOne(new File(base, "save"), new File(f, "save"), new File(t, "save"));
        undoOne(new File(base, "states"), new File(f, "states"), new File(t, "states"));
    }

    private static void undoOne(File current, File parkedFrom, File parkedTo) {
        if (!parkedFrom.exists()) return;  // (it never left)
        if (current.exists() && !current.renameTo(parkedTo)) return;  // (the other page's: back where it was)
        parkedFrom.renameTo(current);
    }
}
