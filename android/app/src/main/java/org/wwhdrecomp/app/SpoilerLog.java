package org.wwhdrecomp.app;

import java.io.File;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.HashSet;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.Set;

/**
 * The randomizer's spoiler log (Game > Randomizer > Spoiler log, OptionsMenu.randoSpoilerPage) read into its parts: the
 * starting items, the way through (the randomizer's playthrough spheres: what can be got with what came before), every
 * location's item, where the charts lead and the hints. Each location gets a place on the progress map
 * (ProgressMapView): the square and spot of tools/rando/spoiler_places.py's table (assets rando/wwhd/places.tsv), the
 * item's icon (the game's own, by the randomizer's item data's game item ids), important when it is on the way through.
 */
final class SpoilerLog {
    static final class Entry {
        final String location, item;
        int sphere = -1;  // its step on the way through, -1: not needed
        int id = -1;      // the item's game item id
        ProgressPlaces.Place place;

        Entry(String location, String item) {
            this.location = location;
            this.item = item;
        }

        /** the island or dungeon part of the location's name ("Windfall Island"), "" without one */
        String area() {
            int i = location.indexOf(" - ");
            return i > 0 ? location.substring(0, i) : "";
        }

        /** the rest of the location's name ("Tingle First Gift") */
        String spot() {
            int i = location.indexOf(" - ");
            return i > 0 ? location.substring(i + 3) : location;
        }
    }

    String hash = "", seed = "";
    final List<String> starting = new ArrayList<>();
    final List<List<Entry>> spheres = new ArrayList<>();
    final List<Entry> all = new ArrayList<>();
    final Map<String, String> charts = new LinkedHashMap<>();  // chart -> island
    final List<String> hints = new ArrayList<>();
    final List<String> entrances = new ArrayList<>();
    final List<ProgressPlaces.Place> places = new ArrayList<>();
    String language = "English";  // the item names' (the randomizer's own, in all six of the game's languages)
    private final Map<String, String[]> itemNames = new HashMap<>();  // English -> {Spanish, French, German, Italian, Japanese}
    private final Map<String, Integer> itemIds = new HashMap<>();

    /** Randomized game n's log (Randomizer.spoilerLog), or null when there is none or it can't be read. */
    static SpoilerLog read(MainActivity a, int n) {
        File f = Randomizer.spoilerLog(a, n);
        if (f == null) return null;
        try {
            SpoilerLog s = new SpoilerLog();
            String data = Randomizer.data(a);
            s.readItems(new File(data, "logic/item_data.yaml"));
            s.parse(new String(Files.readAllBytes(f.toPath()), StandardCharsets.UTF_8));
            s.placeAll(new File(data, "wwhd/places.tsv"));
            String hash = Randomizer.lastInfo(a, n, "hash"), seed = Randomizer.lastInfo(a, n, "seed");
            if (hash != null) s.hash = hash;
            if (seed != null) s.seed = seed;
            String lang = a.gameLanguage();
            s.language = lang.startsWith("es") ? "Spanish" : lang.startsWith("fr") ? "French" : lang.startsWith("de") ? "German"
                    : lang.startsWith("it") ? "Italian" : lang.startsWith("ja") ? "Japanese" : "English";
            return s;
        } catch (IOException | RuntimeException e) {
            android.util.Log.w("wwhd", "spoiler log", e);
            return null;
        }
    }

    /** an item's name in the menu's language */
    String itemName(String english) {
        String[] t = itemNames.get(english);
        if (t == null) return english;
        int i = java.util.Arrays.asList(OTHER_LANGUAGES).indexOf(language);
        String n = i >= 0 ? t[i] : null;
        return n != null && !n.isEmpty() ? n : english;
    }

    // ---- reading

    private static final String[] OTHER_LANGUAGES = {"Spanish", "French", "German", "Italian", "Japanese"};

    // item_data.yaml: "- Names:" / "    English: ..." / "    Spanish: ..." ... "  Game Item Id: 0x.."
    private void readItems(File f) throws IOException {
        String en = null;
        String[] other = new String[OTHER_LANGUAGES.length];
        boolean inNames = false;
        for (String line : Files.readAllLines(f.toPath(), StandardCharsets.UTF_8)) {
            String t = line.trim();
            if (line.startsWith("- Names:")) {
                en = null;
                other = new String[OTHER_LANGUAGES.length];
                inNames = true;
            } else if (line.startsWith("  ") && !line.startsWith("    ")) {
                inNames = false;
                if (t.startsWith("Game Item Id:") && en != null) {
                    try {
                        itemIds.put(en, Integer.decode(t.substring(13).trim()));
                    } catch (NumberFormatException ignored) {
                    }
                    itemNames.put(en, other);
                }
            } else if (inNames) {
                if (t.startsWith("English:")) en = unquote(t.substring(8));
                for (int i = 0; i < OTHER_LANGUAGES.length; i++)
                    if (t.startsWith(OTHER_LANGUAGES[i] + ":")) other[i] = unquote(t.substring(OTHER_LANGUAGES[i].length() + 1));
            }
        }
    }

    private static String unquote(String s) {
        s = s.trim();
        if (s.length() >= 2 && (s.startsWith("\"") && s.endsWith("\"") || s.startsWith("'") && s.endsWith("'"))) s = s.substring(1, s.length() - 1);
        return s;
    }

    private void parse(String text) {
        String section = "";
        Map<String, Entry> byLocation = new HashMap<>();
        List<Entry> sphere = null;
        for (String line : text.split("\n")) {
            if (line.trim().isEmpty()) continue;
            if (!line.startsWith(" ") && !line.startsWith("\t")) {
                section = line.trim();
                if (section.startsWith("Seed:") || section.startsWith("seed:")) seed = section.substring(5).trim();
                continue;
            }
            String t = line.trim();
            if (section.startsWith("Starting Inventory")) {
                starting.add(t);
            } else if (section.startsWith("Playthrough")) {
                if (t.startsWith("Sphere ") && t.endsWith(":")) {
                    sphere = new ArrayList<>();
                    spheres.add(sphere);
                } else if (sphere != null) {
                    Entry e = entry(t);
                    if (e != null) {
                        e.sphere = spheres.size() - 1;
                        sphere.add(e);
                    }
                }
            } else if (section.startsWith("All Locations")) {
                Entry e = entry(t);
                if (e != null) {
                    all.add(e);
                    byLocation.put(e.location, e);
                }
            } else if (section.startsWith("Charts")) {
                int c = t.indexOf(':');
                if (c > 0) charts.put(t.substring(0, c).trim(), t.substring(c + 1).trim());
            } else if (section.startsWith("Hints")) {
                hints.add(t);
            } else if (section.startsWith("Entrances")) {
                entrances.add(t);
            }
        }
        // the way through's steps on the locations
        for (int i = 0; i < spheres.size(); i++)
            for (Entry s : spheres.get(i)) {
                Entry e = byLocation.get(s.location);
                if (e != null && e.sphere < 0) e.sphere = i;
            }
        for (Entry e : all) {
            Integer id = itemIds.get(e.item);
            if (id != null) e.id = id;
        }
        for (List<Entry> sp : spheres)
            for (Entry e : sp) {
                Integer id = itemIds.get(e.item);
                if (id != null) e.id = id;
            }
    }

    // "Location Name:   Item Name" (the name has " - ", the item none of ": ")
    private static Entry entry(String t) {
        int c = t.lastIndexOf(": ");
        if (c <= 0) c = t.indexOf(":\t");
        if (c <= 0) return null;
        String loc = t.substring(0, c).trim(), item = t.substring(c + 1).trim();
        return loc.isEmpty() || item.isEmpty() ? null : new Entry(loc, item);
    }

    private void placeAll(File table) throws IOException {
        Map<String, String[]> rows = new HashMap<>();
        for (String line : Files.readAllLines(table.toPath(), StandardCharsets.UTF_8)) {
            if (line.startsWith("#")) continue;
            String[] f = line.split("\t");
            if (f.length >= 6) rows.put(f[0], f);
        }
        int index = 0;
        for (Entry e : all) {
            String[] f = rows.get(e.location);
            int square = 0;
            String stage = null;
            float x = Float.NaN, y = Float.NaN, z = Float.NaN;
            if (f != null) {
                square = Integer.parseInt(f[1]);
                stage = "-".equals(f[2]) ? null : f[2];
                x = number(f[3]);
                y = number(f[4]);
                z = number(f[5]);
            }
            e.place = ProgressPlaces.spoiler(index++, kind(e.item), square, stage, x, y, z, icon(e.item, e.id), e.sphere >= 0, e.location);
            places.add(e.place);
        }
    }

    private static float number(String s) {
        try {
            return Float.parseFloat(s);
        } catch (NumberFormatException e) {
            return Float.NaN;
        }
    }

    // ---- icons: the game's own, by the item's name and game item id

    // the progress map's groups (hearts, charts, dungeons) for the map's choice
    static int kind(String item) {
        if (item.equals("Piece of Heart")) return ProgressPlaces.HEART;
        if (item.equals("Heart Container")) return ProgressPlaces.CONTAINER;
        if (item.contains("Chart")) return ProgressPlaces.CHART;
        if (item.endsWith("Dungeon Map")) return ProgressPlaces.MAP;
        if (item.endsWith("Compass")) return ProgressPlaces.COMPASS;
        if (item.endsWith("Big Key") || item.endsWith("Small Key")) return ProgressPlaces.BIGKEY;
        return ProgressPlaces.ITEM;
    }

    static String icon(String item, int id) {
        String n = item.toLowerCase(Locale.ROOT);
        if (n.equals("progressive sword") || n.contains("sword")) return "CollectIcon118_00^l";
        if (n.equals("progressive shield") || n.contains("shield")) return "CollectIcon118_04^l";
        if (n.equals("power bracelets")) return "CollectIcon118_09^l";
        if (n.equals("hero's charm")) return "CollectIcon118_08^l";
        if (n.equals("progressive bow")) return GameIcons.itemTexture(0x27);
        if (n.equals("progressive wallet") || n.endsWith("rupee") || n.endsWith("rupees")) return "Rupy_01^l";
        if (n.equals("progressive bomb bag") || n.equals("bombs")) return GameIcons.itemTexture(0x31);
        if (n.equals("progressive quiver")) return "NormalArrow_00^l";
        if (n.equals("progressive picto box")) return GameIcons.itemTexture(0x23);
        if (n.equals("progressive sail")) return GameIcons.itemTexture(0x78);
        if (n.equals("piece of heart")) return "HeartPiece_02^l";
        if (n.equals("heart container")) return "HeartReceptacle_00^l";
        if (n.contains("chart")) return "MapBtn_00^l";
        if (n.startsWith("triforce shard")) return "TriforceShards_00^l";
        if (n.equals("nayru's pearl")) return "Pearl_02^l";
        if (n.equals("din's pearl")) return "Pearl_00^l";
        if (n.equals("farore's pearl")) return "Pearl_01^l";
        if (n.endsWith("dungeon map")) return "MapItemIcon_00^l";
        if (n.endsWith("compass")) return "MapItemIcon_02^l";
        if (n.endsWith("big key")) return "MapItemIcon_01^l";
        if (n.equals("wind's requiem") || n.equals("ballad of gales") || n.equals("command melody") || n.equals("earth god's lyric")
                || n.equals("wind god's aria") || n.equals("song of passing") || n.equals("wind waker")) return "CollectIcon118_15^l";
        if (n.contains("bottle")) return GameIcons.itemTexture(0x50);
        if (id >= 0) {
            String t = GameIcons.itemTexture(id);
            if (t != null) return t;
        }
        return null;  // (ProgressIcon: a plain disc)
    }

    // ---- what the list shows

    /** the locations by area (island or dungeon), areas in name order */
    Map<String, List<Entry>> byArea(boolean importantOnly) {
        Map<String, List<Entry>> m = new java.util.TreeMap<>();
        for (Entry e : all) {
            if (importantOnly && e.sphere < 0) continue;
            String area = e.area().isEmpty() ? "-" : e.area();
            List<Entry> l = m.get(area);
            if (l == null) m.put(area, l = new ArrayList<>());
            l.add(e);
        }
        return m;
    }

    /** the important items' names (on the way through) */
    Set<String> importantItems() {
        Set<String> s = new HashSet<>();
        for (List<Entry> sp : spheres) for (Entry e : sp) s.add(e.item);
        return s;
    }
}
