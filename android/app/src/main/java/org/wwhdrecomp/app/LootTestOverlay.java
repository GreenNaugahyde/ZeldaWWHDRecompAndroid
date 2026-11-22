package org.wwhdrecomp.app;

import android.annotation.SuppressLint;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.os.Handler;
import android.os.Looper;
import android.view.Gravity;
import android.view.MotionEvent;
import android.view.View;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.TextView;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileReader;
import java.io.FileWriter;
import java.io.IOException;
import java.util.ArrayList;
import java.util.List;

/**
 * A panel over the running game to go through the found things (Mods > Loot swords / Loot shields, "In-game switcher";
 * user: for testing them): sword or shield, one step or ten, for the save file in play, in the order of the Found
 * things pages. A thing is asked for only once the taps have stopped for a moment (each one is built when asked for).
 * Good / Needs refinement / Bad note the verdict for the thing shown and go on to the next. Results: files/loot_verdicts.tsv
 * (time, sword or shield, id, name, verdict; a thing's latest line counts); its verdict shows under its name.
 * Auto: once the thing shown is on Link, 5 s, then the next (stops at the end of the list, or when one doesn't come).
 * Unrated only: the list without what has a verdict. Drag the panel by its title.
 */
@SuppressLint("ViewConstructor")
final class LootTestOverlay extends LinearLayout {
    private static final int SETTLE_MS = 300, AUTO_MS = 5000, AUTO_GIVE_UP_MS = 20000, TICK_MS = 250;
    private static final int OFF = 0xFF2E4A66, ON = 0xFF1565C0;

    private final MainActivity a;
    private final float dp;
    private final TextView title, name, verdict, kindButton, autoButton, unratedButton;
    private final TextView[] categoryButtons = new TextView[3];
    private final LinearLayout categoryRow;
    private final TextView[] actionButtons = new TextView[6];
    private final LinearLayout[] actionRows = new LinearLayout[2];
    private boolean auto, unrated;
    private long requestedAt, loadedAt;  // uptime: the thing shown was asked for / is on Link (0: not yet)
    private final java.util.Map<String, String> verdicts = new java.util.HashMap<>();  // "sword/id" -> good, refine, bad
    private final LinearLayout body;
    private final Handler handler = new Handler(Looper.getMainLooper());
    private Loot kind;
    private List<String> ids;
    private int idx;

    LootTestOverlay(MainActivity a) {
        super(a);
        this.a = a;
        dp = a.getResources().getDisplayMetrics().density;
        setOrientation(VERTICAL);
        GradientDrawable bg = new GradientDrawable();
        bg.setColor(0xD0101820);
        bg.setCornerRadius(px(10));
        bg.setStroke(px(1), 0x80FFFFFF);
        setBackground(bg);
        setPadding(px(10), px(6), px(10), px(8));

        LinearLayout top = new LinearLayout(a);
        top.setGravity(Gravity.CENTER_VERTICAL);
        title = text(14, true);
        top.addView(title, new LayoutParams(0, -2, 1));
        top.addView(button("–", v -> collapse()), new LayoutParams(px(40), px(36)));
        top.addView(button("✕", v -> a.setLootTest(false)), new LayoutParams(px(40), px(36)));
        addView(top);
        dragBy(title);

        body = new LinearLayout(a);
        body.setOrientation(VERTICAL);
        name = text(15, false);
        body.addView(name);
        verdict = text(13, true);
        body.addView(verdict);
        kindButton = button("", v -> setKind(kind == Loot.SWORDS ? Loot.SHIELDS : kind == Loot.SHIELDS ? Loot.OUTFITS : Loot.SWORDS));
        LayoutParams kp = new LayoutParams(-1, px(40));
        kp.topMargin = px(6);
        body.addView(kindButton, kp);
        LinearLayout row = new LinearLayout(a);
        row.setPadding(0, px(6), 0, 0);
        row.addView(button("−10", v -> move(-10)), cell());
        row.addView(button("◀", v -> move(-1)), cell());
        row.addView(button("▶", v -> move(1)), cell());
        row.addView(button("+10", v -> move(10)), cell());
        body.addView(row);
        LinearLayout rate = new LinearLayout(a);
        rate.setPadding(0, px(6), 0, 0);
        rate.addView(rateButton(R.string.loot_v_good, 0xFF2E7D32, "good"), cell());
        rate.addView(rateButton(R.string.loot_v_refine, 0xFFB26A00, "refine"), cell());
        rate.addView(rateButton(R.string.loot_v_bad, 0xFFB3261E, "bad"), cell());
        body.addView(rate);
        categoryRow = new LinearLayout(a);
        categoryRow.setPadding(0, px(6), 0, 0);
        final String[] cats = {Loot.SWORD, Loot.SHIELD, Loot.OTHER};
        final int[] catWords = {R.string.loot_menu_sword, R.string.loot_menu_shield, R.string.loot_cat_other};
        for (int i = 0; i < cats.length; i++) {
            final String c = cats[i];
            TextView b = button(a.getString(catWords[i]), v -> setCategory(c));
            b.setTextSize(13);
            categoryButtons[i] = b;
            categoryRow.addView(b, cell());
        }
        body.addView(categoryRow);
        LinearLayout worldRow = new LinearLayout(a);  // (loot in the world: LootWorld)
        worldRow.setPadding(0, px(6), 0, 0);
        TextView chestHere = button(a.getString(R.string.loot_t_chest), v -> a.lootWorld.putChest());
        chestHere.setTextSize(13);
        worldRow.addView(chestHere, cell());
        TextView dropHere = button(a.getString(R.string.loot_t_drop), v -> a.lootWorld.testDrop());
        dropHere.setTextSize(13);
        worldRow.addView(dropHere, cell());
        TextView enemyHere = button(a.getString(R.string.loot_t_enemy), v -> a.lootWorld.testEnemy());
        enemyHere.setTextSize(13);
        worldRow.addView(enemyHere, cell());
        body.addView(worldRow);
        final int[] acts = {R.string.loot_sword_boko, R.string.loot_sword_machete, R.string.loot_sword_mace, R.string.loot_sword_darknut, R.string.loot_sword_spear, R.string.loot_sword_pgsword};
        for (int rowNo = 0; rowNo < 2; rowNo++) {
            LinearLayout r = new LinearLayout(a);
            r.setPadding(0, px(6), 0, 0);
            for (int i = 0; i < 3; i++) {
                final int act = rowNo * 3 + i;
                TextView b = button(a.getString(acts[act]), v -> setAction(act));
                b.setTextSize(11);
                actionButtons[act] = b;
                r.addView(b, cell());
            }
            actionRows[rowNo] = r;
            body.addView(r);
        }
        LinearLayout modes = new LinearLayout(a);
        modes.setPadding(0, px(6), 0, 0);
        autoButton = button("", v -> setAuto(!auto));
        unratedButton = button(a.getString(R.string.loot_unrated), v -> setUnrated(!unrated));
        unratedButton.setTextSize(13);
        modes.addView(autoButton, cell());
        modes.addView(unratedButton, cell());
        body.addView(modes);
        addView(body);
        readVerdicts();
        unrated = a.prefs.getBoolean("loot_test_unrated", false);
        paint(unratedButton, unrated);
        showAuto(-1);
        setKind(a.prefs.getBoolean("loot_test_outfit", false) ? Loot.OUTFITS : a.prefs.getBoolean("loot_test_shield", false) ? Loot.SHIELDS : Loot.SWORDS);
    }

    FrameLayout.LayoutParams layoutParams() {
        FrameLayout.LayoutParams lp = new FrameLayout.LayoutParams(px(300), -2);
        lp.gravity = Gravity.TOP | Gravity.START;
        lp.leftMargin = a.prefs.getInt("loot_test_x", px(60));
        lp.topMargin = a.prefs.getInt("loot_test_y", px(20));
        return lp;
    }

    void stop() {
        handler.removeCallbacks(apply);
        handler.removeCallbacks(tick);
        auto = false;
    }

    private void paint(TextView b, boolean on) { ((GradientDrawable) b.getBackground()).setColor(on ? ON : OFF); }

    private void showAuto(int secondsLeft) {
        autoButton.setText(a.getString(R.string.loot_auto) + (secondsLeft >= 0 ? " " + secondsLeft : ""));
        paint(autoButton, auto);
    }

    private void setAuto(boolean on) {
        handler.removeCallbacks(tick);
        auto = on && !ids.isEmpty() && PlayerModels.playedFile() != 0;
        showAuto(-1);
        if (!auto) return;
        requestedAt = android.os.SystemClock.uptimeMillis();
        loadedAt = 0;
        handler.removeCallbacks(apply);
        if (!ids.get(idx).equals(inMemory())) applyNow();
        handler.postDelayed(tick, TICK_MS);
    }

    private final Runnable tick = this::autoTick;

    private void autoTick() {
        if (!auto) return;
        if (ids.isEmpty() || PlayerModels.playedFile() == 0) {
            setAuto(false);
            return;
        }
        handler.postDelayed(tick, TICK_MS);
        long now = android.os.SystemClock.uptimeMillis();
        if (!ids.get(idx).equals(inMemory())) {
            loadedAt = 0;
            showAuto(-1);
            if (now - requestedAt > AUTO_GIVE_UP_MS) setAuto(false);  // (it doesn't come: stop on it)
            return;
        }
        if (loadedAt == 0) loadedAt = now;
        long left = AUTO_MS - (now - loadedAt);
        if (left > 0) {
            showAuto((int) ((left + 999) / 1000));
        } else if (idx >= ids.size() - 1) {
            setAuto(false);  // (the end of the list)
        } else {
            move(1);
        }
    }

    private void setUnrated(boolean on) {
        unrated = on;
        a.prefs.edit().putBoolean("loot_test_unrated", on).apply();
        paint(unratedButton, on);
        String keep = ids.isEmpty() ? null : ids.get(Math.min(idx, ids.size() - 1));
        rebuild(keep);
        show();
        requestShown();
    }

    // the list: the found things of this kind (without the rated ones with Unrated only), at `keep` if it is in it
    private void rebuild(String keep) {
        final Loot k = kind;
        ids = new ArrayList<>();
        for (String id : k.ids())
            if (Loot.found(id) && !(unrated && verdicts.containsKey(key(id)))) ids.add(id);
        ids.sort((x, y) -> k.name(a, x).compareToIgnoreCase(k.name(a, y)));
        idx = Math.max(0, ids.indexOf(keep));
    }

    // the thing shown asked for, after the taps have stopped
    private void requestShown() {
        requestedAt = android.os.SystemClock.uptimeMillis();
        loadedAt = 0;
        handler.removeCallbacks(apply);
        if (!ids.isEmpty()) handler.postDelayed(apply, SETTLE_MS);
    }

    private int px(float v) { return Math.round(v * dp); }

    private LayoutParams cell() {
        LayoutParams lp = new LayoutParams(0, px(44), 1);
        lp.rightMargin = px(4);
        return lp;
    }

    private TextView text(float size, boolean bold) {
        TextView t = new TextView(a);
        t.setTextColor(Color.WHITE);
        t.setTextSize(size);
        if (bold) t.setTypeface(Typeface.DEFAULT_BOLD);
        return t;
    }

    private TextView button(String label, View.OnClickListener click) {
        TextView b = text(16, true);
        b.setText(label);
        b.setGravity(Gravity.CENTER);
        GradientDrawable bg = new GradientDrawable();
        bg.setColor(0xFF2E4A66);
        bg.setCornerRadius(px(6));
        b.setBackground(bg);
        b.setOnClickListener(click);
        return b;
    }

    private TextView rateButton(int label, int colour, String value) {
        TextView b = button(a.getString(label), v -> {
            if (value.equals("refine")) askComment(value);
            else rate(value);
        });
        b.setTextSize(13);
        ((GradientDrawable) b.getBackground()).setColor(colour);
        return b;
    }

    private String kindWord() { return kind == Loot.SWORDS ? "sword" : kind == Loot.SHIELDS ? "shield" : "outfit"; }

    private String key(String id) { return kindWord() + "/" + id; }

    /** what of this kind is on Link now */
    private String inMemory() { return kind == Loot.OUTFITS ? Native.lootOutfitInMemory() : Native.lootInMemory(kind == Loot.SHIELDS); }

    private File verdictFile() { return new File(a.baseDir(), "loot_verdicts.tsv"); }

    private void readVerdicts() {
        File f = verdictFile();
        if (!f.exists()) return;
        try (BufferedReader r = new BufferedReader(new FileReader(f))) {
            for (String l; (l = r.readLine()) != null; ) {
                String[] c = l.split("\t", -1);
                if (c.length >= 5) verdicts.put(c[1] + "/" + c[2], c[4]);
            }
        } catch (IOException ignored) {
        }
    }

    // "needs refinement": what is wrong with it, in a few words (optional; Auto waits meanwhile)
    private void askComment(String value) {
        if (ids.isEmpty() || PlayerModels.playedFile() == 0) return;
        final boolean wasAuto = auto;
        setAuto(false);
        android.widget.EditText e = new android.widget.EditText(a);
        e.setHint(R.string.loot_comment_hint);
        e.setSingleLine(true);
        e.setTextColor(GameUi.INK);
        e.setFilters(new android.text.InputFilter[] {new android.text.InputFilter.LengthFilter(200)});
        // the usual problems, one tap each (kept in the file in English, whatever the language shown)
        final String[] tags = {"feet", "arms", "hands", "head", "eyes", "body", "size", "colour"};
        final int[] names = {R.string.loot_tag_feet, R.string.loot_tag_arms, R.string.loot_tag_hands, R.string.loot_tag_head, R.string.loot_tag_eyes, R.string.loot_tag_body,
                R.string.loot_tag_size, R.string.loot_tag_colour};
        final boolean[] on = new boolean[tags.length];
        LinearLayout box = new LinearLayout(a);
        box.setOrientation(LinearLayout.VERTICAL);
        LinearLayout row = null;
        for (int i = 0; i < tags.length; i++) {
            if (i % 4 == 0) {
                row = new LinearLayout(a);
                row.setPadding(0, 0, 0, px(4));
                box.addView(row);
            }
            final int n = i;
            final TextView[] self = new TextView[1];
            self[0] = button(a.getString(names[i]), v -> {
                on[n] = !on[n];
                paint(self[0], on[n]);
            });
            self[0].setTextSize(13);
            row.addView(self[0], cell());
        }
        box.addView(e);
        new GameDialog(a).top().title(kind.name(a, ids.get(idx))).content(box).button(R.string.opt_cancel, () -> {
            if (wasAuto) setAuto(true);
        }).button(R.string.opt_ok, () -> {
            StringBuilder c = new StringBuilder();
            for (int i = 0; i < tags.length; i++)
                if (on[i]) c.append(c.length() > 0 ? "," : "").append(tags[i]);
            String text = e.getText().toString().replace('\t', ' ').replace('\n', ' ').trim();
            if (!text.isEmpty()) c.append(c.length() > 0 ? ": " : "").append(text);
            rate(value, c.toString());
            if (wasAuto) setAuto(true);
        }).show();
    }

    private void rate(String value) { rate(value, ""); }

    // the verdict for the thing shown (only with a file in play: nothing is shown without one), then the next one; a comment as a sixth column
    private void rate(String value, String comment) {
        if (ids.isEmpty() || PlayerModels.playedFile() == 0) return;
        String id = ids.get(idx);
        verdicts.put(key(id), value);
        String line = String.join("\t", new java.text.SimpleDateFormat("yyyy-MM-dd HH:mm:ss", java.util.Locale.ROOT).format(new java.util.Date()),
                kindWord(), id, kind.name(a, id), value, comment) + "\n";
        try (FileWriter w = new FileWriter(verdictFile(), true)) {
            w.write(line);
        } catch (IOException ignored) {
        }
        if (unrated) {  // (gone from the list; the next one has its place)
            ids.remove(idx);
            if (idx >= ids.size()) idx = 0;
            show();
            requestShown();
            if (ids.isEmpty()) setAuto(false);
        } else {
            move(1);
        }
    }

    @SuppressLint("ClickableViewAccessibility")
    private void dragBy(View handle) {
        float[] down = new float[4];
        handle.setOnTouchListener((v, e) -> {
            FrameLayout.LayoutParams lp = (FrameLayout.LayoutParams) getLayoutParams();
            if (e.getActionMasked() == MotionEvent.ACTION_DOWN) {
                down[0] = e.getRawX();
                down[1] = e.getRawY();
                down[2] = lp.leftMargin;
                down[3] = lp.topMargin;
            } else if (e.getActionMasked() == MotionEvent.ACTION_MOVE) {
                lp.leftMargin = Math.max(0, (int) (down[2] + e.getRawX() - down[0]));
                lp.topMargin = Math.max(0, (int) (down[3] + e.getRawY() - down[1]));
                setLayoutParams(lp);
            } else if (e.getActionMasked() == MotionEvent.ACTION_UP) {
                a.prefs.edit().putInt("loot_test_x", lp.leftMargin).putInt("loot_test_y", lp.topMargin).apply();
            }
            return true;
        });
    }

    private void collapse() { body.setVisibility(body.getVisibility() == VISIBLE ? GONE : VISIBLE); }

    private void setKind(Loot k) {
        stop();
        kind = k;
        a.prefs.edit().putBoolean("loot_test_shield", k == Loot.SHIELDS).putBoolean("loot_test_outfit", k == Loot.OUTFITS).apply();
        // (from the thing the save file in play has, else from the top)
        int file = PlayerModels.playedFile();
        String has = file > 0 ? k.get(a, file) : null;
        rebuild(has);
        showAuto(-1);
        kindButton.setText(k == Loot.SWORDS ? R.string.loot_title : k == Loot.SHIELDS ? R.string.loot_shield_title : R.string.loot_outfit_title);
        show();
        if (unrated && file > 0 && !ids.isEmpty() && !ids.get(idx).equals(has)) requestShown();  // (what it had is rated: the first unrated one)
    }

    private void move(int by) {
        if (ids.isEmpty()) return;
        idx = Math.floorMod(idx + by, ids.size());
        show();
        requestShown();
    }

    private final Runnable apply = this::applyNow;

    private void applyNow() {
        int file = PlayerModels.playedFile();
        if (file > 0 && !ids.isEmpty()) kind.set(a, file, ids.get(idx));
        show();
    }

    // where the thing shown can be equipped (swords and shields; an outfit has its own slot)
    private void setCategory(String c) {
        if (ids.isEmpty() || kind == Loot.OUTFITS) return;
        String id = ids.get(idx);
        Loot.setCategory(a, id, kind.name(a, id), c);
        show();
    }

    // an Other thing: which of the game's carried weapons it acts as on X / Y / R
    private void setAction(int act) {
        if (ids.isEmpty()) return;
        String id = ids.get(idx);
        Loot.setAction(a, id, kind.name(a, id), act);
        show();
    }

    private void show() {
        final boolean isOther = !ids.isEmpty() && kind != Loot.OUTFITS && Loot.OTHER.equals(kind.category(a, ids.get(idx)));
        for (LinearLayout r : actionRows) r.setVisibility(isOther ? VISIBLE : GONE);
        if (isOther) {
            final int act = Loot.action(a, ids.get(idx));
            for (int i = 0; i < 6; i++) paint(actionButtons[i], i == act);
        }
        categoryRow.setVisibility(kind == Loot.OUTFITS ? GONE : VISIBLE);
        final String cat = ids.isEmpty() || kind == Loot.OUTFITS ? "" : kind.category(a, ids.get(idx));
        final String[] cats = {Loot.SWORD, Loot.SHIELD, Loot.OTHER};
        for (int i = 0; i < 3; i++) paint(categoryButtons[i], cats[i].equals(cat));
        title.setText(ids.isEmpty() ? a.getString(R.string.loot_found) : a.getString(R.string.loot_found) + "  " + (idx + 1) + " / " + ids.size());
        name.setText(PlayerModels.playedFile() == 0 ? a.getString(R.string.loot_test_no_file)
                : ids.isEmpty() ? a.getString(R.string.loot_all_rated) : kind.name(a, ids.get(idx)));
        String v = ids.isEmpty() ? null : verdicts.get(key(ids.get(idx)));
        verdict.setText(v == null ? "" : a.getString(v.equals("good") ? R.string.loot_v_good : v.equals("bad") ? R.string.loot_v_bad : R.string.loot_v_refine));
        verdict.setVisibility(v == null ? GONE : VISIBLE);
    }
}
