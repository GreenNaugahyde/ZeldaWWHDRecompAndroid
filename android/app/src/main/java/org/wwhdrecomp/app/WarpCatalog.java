package org.wwhdrecomp.app;

import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.util.ArrayList;
import java.util.List;

/**
 * The developer warp menu's places: <game>/warps.json (warp_catalog.cpp, made at the end of the
 * extraction; for a game extracted before, here once in the background). Shared by the Warp page
 * (OptionsMenu) and the test overlay (WarpTestOverlay).
 */
final class WarpCatalog {
    static int layer = -1;  // -1: the story's layer
    private static final int VERSION = 2;  // warp_catalog.cpp's "version": an older file is made again

    private static JSONArray categories;
    private static String key;  // path and time of the file read (a new extraction replaces it)
    private static boolean building;

    /** A spot to warp to. */
    static final class Point {
        final String name, hint, stage;
        final int room, spawn;
        final StoryPresets.Preset preset;  // a story preset to load with the warp, or null

        Point(String name, String hint, String stage, int room, int spawn) {
            this(name, hint, stage, room, spawn, null);
        }

        Point(String name, String hint, String stage, int room, int spawn, StoryPresets.Preset preset) {
            this.name = name;
            this.hint = hint;
            this.stage = stage;
            this.room = room;
            this.spawn = spawn;
            this.preset = preset;
        }

        String key() { return (preset != null ? "preset:" + preset.name + ":" : "") + stage + "/" + room + "/" + spawn; }

        /** Warps there (with the preset's story); false without a loaded save file. */
        boolean warp() {
            if (preset != null)
                return Native.warpPreset(preset.story, stage, room, spawn, preset.layer >= 0 ? preset.layer : layer);
            return Native.warp(stage, room, spawn, layer);
        }
    }

    /** The story presets as warp points. */
    static List<Point> presetPoints(MainActivity a) {
        List<Point> out = new ArrayList<>();
        for (StoryPresets.Preset p : StoryPresets.list(a))
            out.add(new Point(p.name, "story preset", p.stage, p.room, p.spawn, p));
        return out;
    }

    /** The categories, or null while the file is being made (onReady then runs on the UI thread). */
    static JSONArray categories(MainActivity a, Runnable onReady) {
        File f = new File(a.gameDir(), "warps.json");
        // made with the stages' save tables (story presets) since; an older one is made again
        if (!f.exists() || !new File(a.gameDir(), "warp_tables.txt").exists()) {
            //noinspection ResultOfMethodCallIgnored
            f.delete();
            if (!building) {
                building = true;
                String dir = a.gameDir();
                new Thread(() -> {
                    String err = Native.buildWarps(dir);
                    a.runOnUiThread(() -> {
                        building = false;
                        if (err != null) android.widget.Toast.makeText(a, err, android.widget.Toast.LENGTH_LONG).show();
                        if (onReady != null) onReady.run();
                    });
                }, "warp-catalog").start();
            }
            return null;
        }
        String k = f.getPath() + "@" + f.lastModified();
        if (!k.equals(key)) {
            key = k;
            try (InputStream in = new FileInputStream(f)) {
                ByteArrayOutputStream out = new ByteArrayOutputStream();
                byte[] buf = new byte[16384];
                for (int n; (n = in.read(buf)) > 0; ) out.write(buf, 0, n);
                JSONObject root = new JSONObject(out.toString("UTF-8"));
                if (root.optInt("version", 1) < VERSION) {  // from an older app: made again
                    key = null;
                    //noinspection ResultOfMethodCallIgnored
                    f.delete();
                    return categories(a, onReady);
                }
                categories = root.getJSONArray("categories");
            } catch (IOException | JSONException e) {
                categories = new JSONArray();
            }
        }
        return categories;
    }

    /** Every spot of a category: an entry's own spot, or each spawn point of a place. */
    static List<Point> points(JSONObject cat) {
        List<Point> out = new ArrayList<>();
        JSONArray items = cat.optJSONArray("items");
        for (int i = 0; items != null && i < items.length(); i++) {
            JSONObject p = items.optJSONObject(i);
            String name = p.optString("name"), stage = p.optString("stage");
            JSONArray spawns = p.optJSONArray("spawns");
            if (spawns == null) {
                out.add(new Point(name, "", stage, p.optInt("room"), p.optInt("spawn")));
                continue;
            }
            for (int j = 0; j < spawns.length(); j++) {
                JSONArray s = spawns.optJSONArray(j);  // room, spawn, what it is
                out.add(new Point(name, s.optString(2), stage, s.optInt(0), s.optInt(1)));
            }
        }
        return out;
    }
}
