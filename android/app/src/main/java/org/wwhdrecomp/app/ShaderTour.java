package org.wwhdrecomp.app;

import android.content.SharedPreferences;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.util.Log;

import org.json.JSONArray;

import java.util.ArrayList;
import java.util.HashSet;
import java.util.List;
import java.util.Set;

/**
 * Developer (not in published releases): plays by itself to fill the shader cache for the head-start
 * template. Started with the intent extra shader_tour=<save state slot> (adb shell am start ... --es
 * shader_tour 8): the game loads that state at its start, then the tour warps to the story presets (each
 * one's story and place) and then to every room of every place (one spawn per stage and room) with the
 * furthest preset's story (the one named 100%, else the most hearts; also after a restart, which loads
 * the slot's story again), and at each walks Link about and turns the camera all round. Extras for further
 * rounds: shader_tour_story=<part of a preset's name> plays the places with that preset's story instead,
 * shader_tour_places_only=true leaves the presets' own spots out. Without a state in that slot this build can
 * load (a fresh installation) the tour presses A through the title and the file select, and once the save file
 * runs it saves a state there for the restarts. Dialogue after a load gets A presses; a
 * cutscene is waited out (at most EVENT_MAX_MS).
 *
 * No save states of any kind while it runs, no dialogs at the start. Progress lives in the prefs
 * (tour_idx); a spot whose warp never finished (a crash, or a stuck load: the tour then ends the
 * process) is skipped at the next start, so a script that starts the app again whenever it isn't
 * running gets through the list. Log lines: "[tour] ..." (tag wwhd); "[tour] done" at the end.
 */
final class ShaderTour {
    private static final String TAG = "wwhd";
    static final String EXTRA = "shader_tour";
    static String storyName;      // shader_tour_story: the places' story preset (null: the furthest)
    static boolean placesOnly;    // shader_tour_places_only: no preset spots
    private static final String IDX = "tour_idx", PENDING = "tour_pending";
    private static final int LOAD_MAX_MS = 40000;   // a warp that hasn't loaded by then is stuck
    private static final int EVENT_MAX_MS = 25000;  // then the moves start anyway
    private static final int SETTLE_MS = 1500;      // after the load, before moving

    // the moves at each spot: {milliseconds, left stick x, y, right stick x, y}
    private static final float[][] MOVES = {
            {3200, 0, 0, 1, 0},       // the camera once all round
            {2200, 0.5f, 1, 0, 0},    // walk ahead, bearing right
            {1600, 0, 0, -1, 0},      // the camera back half way
            {2200, -1, 0.6f, 0, 0},   // walk on, to the left
            {900, 0, 0, 0, 1},        // the camera up
            {900, 0, 0, 0, -1},       // and down
            {2000, 1, -1, 0.6f, 0},   // walk back, the camera turning
            {1200, 0, 0, 0, 0},
    };

    private final MainActivity a;
    private final SharedPreferences prefs;
    private final Handler h = new Handler(Looper.getMainLooper());
    private List<WarpCatalog.Point> points;
    private int idx;
    private long stepAt, eventSince, aAt;
    private int phase;  // 0 waiting for the game, 1 warping, 2 events, 3 settling, 4 moving
    private int move;
    private final int slot;
    private final boolean boot;  // no state to load: A through the title and file select, then a state saved
    private long bootA;
    private int refused;  // warps refused in a row (a scene change that never ends: game over and the like)
    // the input the tour holds (MainActivity.pushInput sends it while the tour runs)
    int buttons;
    float lx, ly, rx, ry;

    ShaderTour(MainActivity a, int slot, boolean boot) {
        this.a = a;
        this.slot = slot;
        this.boot = boot;
        if (boot) Log.i(TAG, "[tour] no state in slot " + slot + ": through the title and file select by A");
        prefs = a.prefs;
        idx = prefs.getInt(IDX, 0);
        String pending = prefs.getString(PENDING, null);
        if (pending != null) {  // the last run ended in this spot's warp: on past it
            Log.i(TAG, "[tour] skipped (crash / stuck): " + pending);
            idx++;
            prefs.edit().putInt(IDX, idx).remove(PENDING).commit();
        }
        h.post(tick);
    }

    /** Starts the list from the beginning (a new tour). */
    static void reset(SharedPreferences prefs) { prefs.edit().remove(IDX).remove(PENDING).commit(); }

    private List<WarpCatalog.Point> list() {
        JSONArray cats = WarpCatalog.categories(a, null);
        if (cats == null) return null;
        List<WarpCatalog.Point> presets = WarpCatalog.presetPoints(a);
        List<WarpCatalog.Point> out = new ArrayList<>(placesOnly ? new ArrayList<>() : presets);
        StoryPresets.Preset story = null;
        for (WarpCatalog.Point p : presets)
            if (storyName != null ? p.preset.name.contains(storyName) && story == null
                    : story == null || p.preset.name.contains("100%")
                    || (!story.name.contains("100%") && p.preset.maxHearts() > story.maxHearts())) story = p.preset;
        Log.i(TAG, "[tour] places with the story of " + (story != null ? story.name : "the save state"));
        Set<String> seen = new HashSet<>();
        for (int c = 0; c < cats.length(); c++)
            for (WarpCatalog.Point p : WarpCatalog.points(cats.optJSONObject(c))) {
                if (!seen.add(p.stage + "/" + p.room)) continue;
                out.add(story == null ? p : new WarpCatalog.Point(p.name, p.hint, p.stage, p.room, p.spawn,
                        new StoryPresets.Preset(null, story.name, p.stage, p.room, p.spawn, -1, story.story)));
            }
        return out;
    }

    private void hold(int b, float lx, float ly, float rx, float ry) {
        buttons = b;
        this.lx = lx;
        this.ly = ly;
        this.rx = rx;
        this.ry = ry;
        a.repushInput();
    }

    private void next() {
        prefs.edit().remove(PENDING).putInt(IDX, ++idx).commit();
        warp();
    }

    private void warp() {
        hold(0, 0, 0, 0, 0);
        if (idx >= points.size()) {
            Log.i(TAG, "[tour] done: " + points.size() + " spots");
            phase = 5;
            return;
        }
        WarpCatalog.Point p = points.get(idx);
        Log.i(TAG, "[tour] " + (idx + 1) + "/" + points.size() + " " + p.name + " (" + p.key() + ")");
        prefs.edit().putString(PENDING, p.key()).commit();  // written now: a crash may follow
        if (!p.warp()) {
            Log.i(TAG, "[tour] warp refused (no game loaded)");
            prefs.edit().remove(PENDING).commit();
            phase = 0;
            return;
        }
        phase = 1;
        stepAt = SystemClock.uptimeMillis();
    }

    private final Runnable tick = new Runnable() {
        @Override
        public void run() {
            step();
            if (phase != 5) h.postDelayed(this, 100);
        }
    };

    private void step() {
        long now = SystemClock.uptimeMillis();
        if (phase == 0) {  // the state loaded and the game in play
            if (Native.warpWhere().isEmpty()) {
                if (boot && now - bootA > 1500) {  // the title and the file select: A
                    bootA = now;
                    hold(Native.A, 0, 0, 0, 0);
                    h.postDelayed(() -> hold(0, 0, 0, 0, 0), 150);
                }
                return;
            }
            if (boot && stepAt == 0) {
                Log.i(TAG, "[tour] save file running: state into slot " + slot);
                Native.saveState(slot);
            }
            if (points == null) points = list();
            if (points == null) return;  // the catalog is being made
            if (stepAt == 0) stepAt = now;
            if (now - stepAt < (boot ? 8000 : 3000)) return;
            warp();
            return;
        }
        long[] st = Native.warpStatus();
        if (phase == 1) {
            if (st[0] == 4) {  // another scene change was under way: again, for a while
                if (++refused > 100) {
                    Log.i(TAG, "[tour] refused 100 times: ending the process (the next start skips this spot)");
                    android.os.Process.killProcess(android.os.Process.myPid());
                }
                warp();
            } else if (st[0] == 3) {
                Log.i(TAG, String.format(java.util.Locale.ROOT, "[tour] loaded in %.1f s", st[1] / 1000.0));
                refused = 0;
                phase = 2;
                stepAt = now;
                eventSince = aAt = 0;
            } else if (now - stepAt > LOAD_MAX_MS) {
                Log.i(TAG, "[tour] stuck: ending the process (the next start skips this spot)");
                android.os.Process.killProcess(android.os.Process.myPid());
            }
            return;
        }
        if (phase == 2) {  // dialogue: A now and then; a cutscene: wait
            if (st[2] != 0 && now - stepAt < EVENT_MAX_MS) {
                if (st[2] == 1 && now - aAt > 1200) {
                    aAt = now;
                    hold(Native.A, 0, 0, 0, 0);
                } else if (now - aAt > 150) {
                    hold(0, 0, 0, 0, 0);
                }
                return;
            }
            hold(0, 0, 0, 0, 0);
            phase = 3;
            stepAt = now;
            return;
        }
        if (phase == 3) {
            if (now - stepAt < SETTLE_MS) return;
            phase = 4;
            move = -1;
            stepAt = 0;
        }
        if (phase == 4) {
            if (move >= 0 && now - stepAt < MOVES[move][0]) return;
            if (++move >= MOVES.length) {
                next();
                return;
            }
            float[] m = MOVES[move];
            hold(0, m[1], m[2], m[3], m[4]);
            stepAt = now;
        }
    }
}
