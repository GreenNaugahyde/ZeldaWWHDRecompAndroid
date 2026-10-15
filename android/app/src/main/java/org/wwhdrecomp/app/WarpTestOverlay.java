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

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileReader;
import java.io.FileWriter;
import java.io.IOException;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

/**
 * Developer: a panel over the running game to go through the warp points one after the other (Warp
 * page, "Test overlay"). ✓ / ✗ write the verdict for the spot shown and warp to the next (before a
 * warp to the spot shown they warp there first); ◀ ▶ only move (after a finished load without a
 * verdict they write "loaded, no verdict"). The status line follows the warp (Native.warpStatus): loading, loaded with its time, or no
 * end after 15 s (then the scene change is stuck: restart the app). Auto goes through the category by
 * itself: a spot that loaded is written as "auto: loaded" after the set seconds, then the next one
 * warps; it stops at the end of the category or at a warp that doesn't load. A warp that never got a verdict
 * (the app crashed or was closed) is written as "crash / stuck" at the next start and skipped.
 * Results: files/warp_tests.tsv (time, category, place, stage, room, spawn, result, load seconds,
 * what the spawn is); a spot's latest result shows under its name. Drag the panel by its title.
 */
@SuppressLint("ViewConstructor")
final class WarpTestOverlay extends LinearLayout {
    private static final int STUCK_MS = 15000;

    private final MainActivity a;
    private final float dp;
    private final TextView title, place, status;
    private final LinearLayout body;
    private final Handler handler = new Handler(Looper.getMainLooper());
    private final Map<String, String> last = new HashMap<>();  // spot key -> latest result
    private JSONArray cats;
    private int cat, idx;
    private List<WarpCatalog.Point> points;
    private boolean warped;  // a warp of this panel is under way or done (the status line follows it)
    // Auto: after a spot has loaded, wait autoSecs, write "auto: loaded" and warp to the next one; it
    // stops at the end of the category or when a warp doesn't load (that needs an app restart)
    private boolean auto;
    private int autoSecs;
    private long loadedAt;  // uptime when the spot shown had loaded and no event ran (0: not yet)
    private long eventSince;  // uptime since an event (cutscene, dialogue) runs after the load (0: none)
    private static final int EVENT_MAX_MS = 20000;  // then Auto goes on anyway (an event waiting for input)
    private TextView autoButton, autoTime;

    WarpTestOverlay(MainActivity a) {
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
        top.addView(button("✕", v -> a.setWarpTest(false)), new LayoutParams(px(40), px(36)));
        addView(top);
        dragBy(title);

        body = new LinearLayout(a);
        body.setOrientation(VERTICAL);
        place = text(15, false);
        status = text(13, false);
        body.addView(place);
        body.addView(status);
        LinearLayout row = new LinearLayout(a);
        row.setPadding(0, px(6), 0, 0);
        row.addView(button("Cat", v -> setCategory(cat + 1)), cell());
        row.addView(button("◀", v -> move(-1)), cell());
        row.addView(button("Warp", v -> warp()), cell());
        row.addView(button("▶", v -> move(1)), cell());
        row.addView(button("✓", v -> verdict("ok")), cell());
        row.addView(button("✗", v -> verdict("bad")), cell());
        body.addView(row);
        LinearLayout autoRow = new LinearLayout(a);
        autoRow.setGravity(Gravity.CENTER_VERTICAL);
        autoRow.setPadding(0, px(6), 0, 0);
        autoSecs = a.prefs.getInt("warp_test_auto_secs", 5);
        autoButton = button("", v -> setAuto(!auto));
        autoRow.addView(autoButton, new LayoutParams(0, px(44), 2));
        autoRow.addView(button("−", v -> setAutoSecs(autoSecs - 1)), new LayoutParams(px(44), px(44)));
        autoTime = text(15, true);
        autoTime.setGravity(Gravity.CENTER);
        autoRow.addView(autoTime, new LayoutParams(px(70), px(44)));
        autoRow.addView(button("+", v -> setAutoSecs(autoSecs + 1)), new LayoutParams(px(44), px(44)));
        body.addView(autoRow);
        showAuto();
        addView(body);

        readResults();
        cat = a.prefs.getInt("warp_test_cat", 0);
        idx = a.prefs.getInt("warp_test_idx", 0);
        load();
        // a warp without a verdict: the app crashed or was closed while it ran (or after)
        String pending = a.prefs.getString("warp_test_pending", null);
        if (pending != null && points != null && idx < points.size() && points.get(idx).key().equals(pending)) {
            write(points.get(idx), "crash / stuck", -1);
            move(1);
        }
        a.prefs.edit().remove("warp_test_pending").apply();
        handler.post(tick);
    }

    /** Where it sits in the game screen: top middle, or where it was dragged to. */
    FrameLayout.LayoutParams layoutParams() {
        FrameLayout.LayoutParams lp = new FrameLayout.LayoutParams(px(380), -2, Gravity.TOP | Gravity.START);
        int w = a.getResources().getDisplayMetrics().widthPixels;
        lp.leftMargin = a.prefs.getInt("warp_test_x", (w - px(380)) / 2);
        lp.topMargin = a.prefs.getInt("warp_test_y", px(8));
        return lp;
    }

    void stop() { handler.removeCallbacksAndMessages(null); }

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
                a.prefs.edit().putInt("warp_test_x", lp.leftMargin).putInt("warp_test_y", lp.topMargin).apply();
            }
            return true;
        });
    }

    private void collapse() {
        body.setVisibility(body.getVisibility() == VISIBLE ? GONE : VISIBLE);
    }

    private void load() {
        cats = WarpCatalog.categories(a, () -> { load(); show(); });
        if (cats == null || cats.length() == 0) {
            points = null;
            show();
            return;
        }
        List<WarpCatalog.Point> presets = WarpCatalog.presetPoints(a);  // the story presets: a category after the others
        cat = Math.floorMod(cat, cats.length() + (presets.isEmpty() ? 0 : 1));
        points = cat < cats.length() ? WarpCatalog.points(cats.optJSONObject(cat)) : presets;
        idx = points.isEmpty() ? 0 : Math.floorMod(idx, points.size());
        show();
    }

    private void setCategory(int c) {
        cat = c;
        idx = 0;
        warped = false;
        load();
        save();
    }

    private void move(int d) {
        if (points == null || points.isEmpty()) return;
        if (warped) {  // left without a verdict: what the status line knew
            long[] st = Native.warpStatus();
            if (st[0] == 3) write(points.get(idx), "loaded, no verdict", st[1] / 1000.0);
            a.prefs.edit().remove("warp_test_pending").apply();
        }
        idx = Math.floorMod(idx + d, points.size());
        warped = false;
        loadedAt = eventSince = 0;
        save();
        show();
    }

    private void setAuto(boolean on) {
        auto = on;
        loadedAt = 0;
        if (on && !warped) warp();  // start with the spot shown
        showAuto();
    }

    private void setAutoSecs(int secs) {
        autoSecs = Math.max(1, Math.min(30, secs));
        a.prefs.edit().putInt("warp_test_auto_secs", autoSecs).apply();
        showAuto();
    }

    private void showAuto() {
        autoButton.setText(a.getString(auto ? R.string.warp_test_auto_on : R.string.warp_test_auto_off));
        ((GradientDrawable) autoButton.getBackground()).setColor(auto ? 0xFF2E7D32 : 0xFF2E4A66);
        autoTime.setText(a.getString(R.string.warp_test_auto_secs, autoSecs));
    }

    // Auto, 4 times a second: the spot shown loaded autoSecs ago -> write it, on to the next
    private void autoStep(long[] st) {
        if (!auto || !warped || points == null) return;
        if (st[0] == 4) {  // not taken (a scene change was under way): again
            warp();
            return;
        }
        if (st[0] != 3) {
            if (st[1] > STUCK_MS) {  // doesn't load: a restart writes it as crash / stuck
                auto = false;
                showAuto();
            }
            return;
        }
        long now = android.os.SystemClock.uptimeMillis();
        // a cutscene or dialogue right after the load (a boss's intro): rate the spot after it
        if (st[2] != 0) {
            if (eventSince == 0) eventSince = now;
            if (now - eventSince < EVENT_MAX_MS) {
                loadedAt = 0;
                status.setTextColor(0xFFFFE080);
                status.setText(R.string.warp_test_auto_event);
                return;
            }
        } else {
            eventSince = 0;
        }
        if (loadedAt == 0) loadedAt = now;
        if (now - loadedAt < autoSecs * 1000L) return;
        write(points.get(idx), "auto: loaded", st[1] / 1000.0);
        a.prefs.edit().remove("warp_test_pending").apply();
        warped = false;
        loadedAt = eventSince = 0;
        if (idx == points.size() - 1) {  // the category is through
            auto = false;
            showAuto();
            show();
            return;
        }
        move(1);
        warp();
    }

    private void save() { a.prefs.edit().putInt("warp_test_cat", cat).putInt("warp_test_idx", idx).apply(); }

    private void show() {
        if (points == null) {
            title.setText(a.getString(R.string.warp_test_title));
            place.setText(cats == null ? a.getString(R.string.warp_reading) : "–");
            status.setText("");
            return;
        }
        String c = categoryName();
        title.setText(a.getString(R.string.warp_test_title) + " · " + c + " " + (idx + 1) + "/" + points.size());
        WarpCatalog.Point p = points.get(idx);
        String l = last.get(p.key());
        place.setText(p.name + "\n" + p.stage + " · " + a.getString(R.string.warp_spawn, p.room, p.spawn)
                + (p.hint.isEmpty() ? "" : " · " + p.hint) + (l == null ? "" : "\n" + a.getString(R.string.warp_test_last, l)));
    }

    private void warp() {
        if (points == null || points.isEmpty()) return;
        WarpCatalog.Point p = points.get(idx);
        if (!p.warp()) {
            status.setTextColor(0xFFFFB0B0);
            status.setText(R.string.warp_no_save);
            if (auto) {
                auto = false;
                showAuto();
            }
            return;
        }
        warped = true;
        a.prefs.edit().putString("warp_test_pending", p.key()).commit();  // written now: a crash may follow
    }

    // the verdict for the spot shown, then on to the next and warp there
    private void verdict(String result) {
        if (points == null || points.isEmpty()) return;
        if (!warped) {  // a verdict needs a look first: warp to the spot shown
            warp();
            return;
        }
        long[] st = Native.warpStatus();
        write(points.get(idx), result, st[0] == 3 ? st[1] / 1000.0 : -1);
        a.prefs.edit().remove("warp_test_pending").apply();
        warped = false;
        move(1);
        warp();
    }

    private void write(WarpCatalog.Point p, String result, double loadSeconds) {
        last.put(p.key(), result);
        String c = categoryName();
        String line = String.join("\t", new java.text.SimpleDateFormat("yyyy-MM-dd HH:mm:ss", java.util.Locale.ROOT).format(new java.util.Date()),
                c, p.name, p.stage, Integer.toString(p.room), Integer.toString(p.spawn), result,
                loadSeconds < 0 ? "" : String.format(java.util.Locale.ROOT, "%.1f", loadSeconds), p.hint) + "\n";
        try (FileWriter w = new FileWriter(new File(a.baseDir(), "warp_tests.tsv"), true)) {
            w.write(line);
        } catch (IOException ignored) {
        }
    }

    private String categoryName() {
        if (cats == null) return "";
        return cat < cats.length() ? cats.optJSONObject(cat).optString("name") : a.getString(R.string.preset_title);
    }

    private void readResults() {
        File f = new File(a.baseDir(), "warp_tests.tsv");
        if (!f.exists()) return;
        try (BufferedReader r = new BufferedReader(new FileReader(f))) {
            for (String l; (l = r.readLine()) != null; ) {
                String[] c = l.split("\t", -1);
                if (c.length >= 7) last.put(c[3] + "/" + c[4] + "/" + c[5], c[6]);
            }
        } catch (IOException ignored) {
        }
    }

    // the status line, 4 times a second
    private final Runnable tick = new Runnable() {
        @Override
        public void run() {
            boolean waiting = false;
            if (warped) {
                long[] st = Native.warpStatus();
                autoStep(st);
                waiting = auto && st[0] == 3 && st[2] != 0;  // autoStep shows it
            }
            if (warped && !waiting) {
                long[] st = Native.warpStatus();
                int ph = (int) st[0];
                double s = st[1] / 1000.0;
                if (ph == 3) {
                    status.setTextColor(0xFFA0FFA0);
                    status.setText(a.getString(R.string.warp_test_loaded, s));
                } else if (ph == 4) {
                    status.setTextColor(0xFFFFB0B0);
                    status.setText(R.string.warp_test_refused);
                } else if (st[1] > STUCK_MS) {
                    status.setTextColor(0xFFFFB0B0);
                    status.setText(a.getString(R.string.warp_test_stuck, s));
                } else {
                    status.setTextColor(0xFFFFE080);
                    status.setText(a.getString(R.string.warp_test_loading, s));
                }
            } else if (points != null) {
                status.setTextColor(0xFFC0C0C0);
                status.setText(R.string.warp_test_ready);
            }
            handler.postDelayed(this, 250);
        }
    };
}
