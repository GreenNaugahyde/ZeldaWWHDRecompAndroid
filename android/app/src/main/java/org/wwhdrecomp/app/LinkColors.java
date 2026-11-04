package org.wwhdrecomp.app;

import android.graphics.Bitmap;
import android.graphics.BitmapFactory;

import java.io.File;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

/**
 * Game > Randomizer > Link's colours: the randomizer's customizer (customizer/model.cpp, RandoNative.colors) as read
 * from its preferences, and its preview picture recoloured as its PC program does (gui/desktop/player_customization.cpp
 * update_preview: the parts' masks in customizer/Link/color_preview, utility/color.cpp colorExchange in 16-bit colour).
 */
final class LinkColors {
    static final class Part {
        final String name, def, now;  // name as the customizer has it ("Belt Buckle"); colours hex RRGGBB

        Part(String name, String def, String now) {
            this.name = name;
            this.def = def;
            this.now = now;
        }
    }

    boolean casual;
    final List<String> presets = new ArrayList<>();  // [0] = the game's colours ("Default")
    int preset = -1;  // the preset the colours are now, -1 own ones
    final List<Part> parts = new ArrayList<>();

    /** null when the settings can't be read; `work` the randomizer's (Randomizer.work) or a save file's (fileWork) */
    static LinkColors read(MainActivity a, String work) {
        String s = RandoNative.colors(Randomizer.data(a), work);
        if (s == null || s.isEmpty()) return null;
        LinkColors c = new LinkColors();
        for (String line : s.split("\n")) {
            String[] f = line.split("\t");
            if (f[0].equals("casual") && f.length > 1) c.casual = f[1].equals("1");
            else if (f[0].equals("preset") && f.length > 2) {
                if (f[2].equals("1") && c.preset < 0) c.preset = c.presets.size();
                c.presets.add(f[1]);
            } else if (f[0].equals("color") && f.length > 3) c.parts.add(new Part(f[1], f[2], f[3]));
        }
        return c;
    }

    // ---- Mods > Link's colours: the game as it is, with colours of its own (apart from the randomizer's) per save file,
    // for both clothes. Per file N: its colours in <base>/link-colours/file<N>/work, Link's textures in them in
    // file<N>.bin (RandoNative.applyColors, made after each change and at start when the game's pack changed), which the
    // runtime puts into the game's memory while that file is played (runtime/src/mods/link_colors.cpp; no randomized game,
    // no player model)
    static final int FILES = 3;
    static final String[] SHARED = {"Skin", "Mouth", "Eyes", "Sclera"};

    static String prefOn(int file) { return "link_colors_on_" + file; }

    // skin, mouth and eyes of each clothes their own (else alike: one texture each for both; swapped with the clothes)
    static String prefSeparate(int file) { return "link_colors_separate_" + file; }

    static boolean on(MainActivity a, int file) { return a.prefs.getBoolean(prefOn(file), false); }

    static boolean anyOn(MainActivity a) {
        for (int f = 1; f <= FILES; f++) if (on(a, f)) return true;
        return false;
    }

    static boolean separate(MainActivity a, int file) { return a.prefs.getBoolean(prefSeparate(file), false); }

    static File gameDir(MainActivity a) { return new File(a.baseDir(), "link-colours"); }

    static String fileWork(MainActivity a, int file) {
        migrate(a);
        File w = new File(gameDir(a), "file" + file + "/work");
        //noinspection ResultOfMethodCallIgnored
        w.mkdirs();
        return w.getAbsolutePath();
    }

    // (once) the colours from before there were save files: into each file's
    private static void migrate(MainActivity a) {
        File d = gameDir(a), old = new File(d, "work");
        if (!old.isDirectory()) return;
        boolean on = a.prefs.getBoolean("link_colors_on", false), sep = a.prefs.getBoolean("link_colors_separate", false);
        android.content.SharedPreferences.Editor e = a.prefs.edit();
        for (int f = 1; f <= FILES; f++) {
            File w = new File(d, "file" + f + "/work");
            //noinspection ResultOfMethodCallIgnored
            w.mkdirs();
            for (String n : new String[] {"config.yaml", "preferences.yaml"})
                try {
                    File from = new File(old, n);
                    if (from.isFile()) java.nio.file.Files.copy(from.toPath(), new File(w, n).toPath(), java.nio.file.StandardCopyOption.REPLACE_EXISTING);
                } catch (java.io.IOException ignored) {
                }
            e.putBoolean(prefOn(f), on).putBoolean(prefSeparate(f), sep);
        }
        e.remove("link_colors_on").remove("link_colors_separate").apply();
        for (String n : new String[] {"work", "content", "shared.bin", "stamp"}) ModPacks.deleteTree(new File(d, n));
    }

    /** the game's own colours can be played now: the game as it is */
    static boolean active(MainActivity a) {
        return Randomizer.runtimeDir(a) == null;  // (a save file with a player model: the runtime leaves that file alone)
    }

    private static File bin(MainActivity a, int file) { return new File(gameDir(a), "file" + file + ".bin"); }

    private static File stampFile(MainActivity a, int file) { return new File(gameDir(a), "file" + file + ".stamp"); }

    private static File from(MainActivity a) { return new File(a.gameDir(), "content/Common/Pack/permanent_3d.pack"); }

    // file `file`'s textures are as its colours are now (or it has none and no file)
    private static boolean current(MainActivity a, int file) {
        if (!on(a, file)) return !bin(a, file).exists();
        String want = stamp(a, file);
        try {
            return want != null && bin(a, file).isFile()
                    && new String(java.nio.file.Files.readAllBytes(stampFile(a, file).toPath()), StandardCharsets.UTF_8).equals(want);
        } catch (java.io.IOException e) {
            return false;
        }
    }

    // the runtime's folder (it reads the files again)
    private static void handOver(MainActivity a) {
        Native.setLinkColors(active(a) && anyOn(a) ? gameDir(a).getAbsolutePath() : null);
    }

    /**
     * Before the game starts: true when a save file's textures have to be made first (the game's pack changed, or a
     * change wasn't made yet; make, not on the main thread); else they are handed to the runtime at once.
     */
    static boolean needsMaking(MainActivity a) {
        migrate(a);
        if (active(a))
            for (int f = 1; f <= FILES; f++)
                if (!current(a, f)) return true;
        handOver(a);
        return false;
    }

    /** the save files' textures made where needed (about a second each) and handed over. Null when done, else why not. */
    static synchronized String make(MainActivity a) {
        String err = null;
        for (int f = 1; f <= FILES; f++) {
            String e = makeFile(a, f);
            if (e != null) err = e;
        }
        handOver(a);
        return err;
    }

    private static String makeFile(MainActivity a, int file) {
        if (current(a, file)) return null;
        File bin = bin(a, file), stamp = stampFile(a, file);
        //noinspection ResultOfMethodCallIgnored
        stamp.delete();
        if (!on(a, file)) {
            //noinspection ResultOfMethodCallIgnored
            bin.delete();
            return null;
        }
        long t = System.currentTimeMillis();
        String err = RandoNative.applyColors(Randomizer.data(a), fileWork(a, file), a.gameDir(), bin.getAbsolutePath(), separate(a, file));
        android.util.Log.i("wwhd", "Link's colours, save file " + file + ": " + (err == null ? "made" : err) + " (" + (System.currentTimeMillis() - t) + " ms)");
        if (err != null) {
            //noinspection ResultOfMethodCallIgnored
            bin.delete();
            return err;
        }
        try {
            String want = stamp(a, file);
            if (want != null) java.nio.file.Files.write(stamp.toPath(), want.getBytes(StandardCharsets.UTF_8));
        } catch (java.io.IOException ignored) {
        }
        return null;
    }

    private static final java.util.concurrent.ScheduledExecutorService maker =
            java.util.concurrent.Executors.newSingleThreadScheduledExecutor(r -> new Thread(r, "link-colours"));
    private static java.util.concurrent.ScheduledFuture<?> pending;

    /** After a change on the page: the textures made again shortly (changes close together: once) and shown in the game. */
    static synchronized void remakeSoon(MainActivity a) {
        if (pending != null) pending.cancel(false);
        pending = maker.schedule(() -> make(a), 400, java.util.concurrent.TimeUnit.MILLISECONDS);
    }

    // what a file's textures are made from: the game's pack and its colours (preferences.yaml); null before there are any
    private static String stamp(MainActivity a, int file) {
        File from = from(a);
        try {
            return "v3\t" + from.length() + "\t" + from.lastModified() + "\t" + (separate(a, file) ? "separate" : "alike") + "\n"
                    + new String(java.nio.file.Files.readAllBytes(new File(fileWork(a, file), "preferences.yaml").toPath()), StandardCharsets.UTF_8);
        } catch (java.io.IOException e) {
            return null;
        }
    }

    Part part(String name) {
        for (Part p : parts) if (p.name.equals(name)) return p;
        return null;
    }

    static int rgb(String hex) {
        try {
            return 0xFF000000 | Integer.parseInt(hex, 16);
        } catch (NumberFormatException e) {
            return 0xFF000000;
        }
    }

    // ---- the preview

    private static final Map<String, Bitmap> bases = new HashMap<>();
    private static final Map<String, int[]> masks = new HashMap<>();  // per "<type>_<part>": the pixels it covers

    private static File previewFile(MainActivity a, String type, String part) {
        return new File(Randomizer.dataDir(a), "customizer/Link/color_preview/preview_" + type + (part == null ? "" : "_" + part) + ".png");
    }

    private static int[] mask(MainActivity a, String type, String part) {
        String key = type + "_" + part;
        int[] m = masks.get(key);
        if (m != null) return m;
        Bitmap b = BitmapFactory.decodeFile(previewFile(a, type, part).getPath());
        if (b == null) return new int[0];
        int w = b.getWidth(), h = b.getHeight();
        int[] px = new int[w * h];
        b.getPixels(px, 0, w, 0, 0, w, h);
        b.recycle();
        int n = 0;
        for (int p : px) if ((p >> 16 & 0xFF) == 255 && (p & 0xFF) == 0) n++;
        m = new int[n];
        n = 0;
        for (int i = 0; i < px.length; i++) if ((px[i] >> 16 & 0xFF) == 255 && (px[i] & 0xFF) == 0) m[n++] = i;
        masks.put(key, m);
        return m;
    }

    /** Link in these colours (the hero's or the casual clothes), or null without the pictures */
    Bitmap preview(MainActivity a) {
        String type = casual ? "casual" : "hero";
        Bitmap base = bases.get(type);
        if (base == null) {
            base = BitmapFactory.decodeFile(previewFile(a, type, null).getPath());
            if (base == null) return null;
            bases.put(type, base);
        }
        int w = base.getWidth(), h = base.getHeight();
        int[] px = new int[w * h];
        base.getPixels(px, 0, w, 0, 0, w, h);
        for (Part p : parts) {
            if (p.now.equalsIgnoreCase(p.def)) continue;
            int from = hexTo16(p.def), to = hexTo16(p.now);
            Map<Integer, Integer> done = new HashMap<>();
            for (int i : mask(a, type, p.name)) {
                int c = px[i];
                double[] hsv = rgbToHsv((c >> 16 & 0xFF) / 255.0, (c >> 8 & 0xFF) / 255.0, (c & 0xFF) / 255.0);
                int cur = hsvTo16(hsv);
                Integer got = done.get(cur);
                int n = got != null ? got : exchange(from, to, cur);
                if (got == null) done.put(cur, n);
                int r = (int) Math.round((n >> 11 & 0x1F) / 31.0 * 255), g = (int) Math.round((n >> 5 & 0x3F) / 63.0 * 255),
                        bl = (int) Math.round((n & 0x1F) / 31.0 * 255);
                px[i] = (c & 0xFF000000) | r << 16 | g << 8 | bl;
            }
        }
        // only Link (the pictures have much empty room round him)
        int x0 = w, y0 = h, x1 = -1, y1 = -1;
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
                if ((px[y * w + x] >>> 24) != 0) {
                    x0 = Math.min(x0, x);
                    x1 = Math.max(x1, x);
                    y0 = Math.min(y0, y);
                    y1 = Math.max(y1, y);
                }
        Bitmap all = Bitmap.createBitmap(px, w, h, Bitmap.Config.ARGB_8888);
        if (x1 < x0) return all;
        return Bitmap.createBitmap(all, x0, y0, x1 - x0 + 1, y1 - y0 + 1);
    }

    // ---- utility/color.cpp, as the randomizer recolours the textures

    static double[] rgbToHsv(double r, double g, double b) {
        double min = Math.min(r, Math.min(g, b)), max = Math.max(r, Math.max(g, b)), delta = max - min;
        double h, s = max > 0 ? delta / max : 0;
        if (delta < 0.001) h = 0;
        else {
            if (r >= max) h = (g - b) / delta;
            else if (g >= max) h = 2 + (b - r) / delta;
            else h = 4 + (r - g) / delta;
            h *= 60;
            if (h < 0) h += 360;
        }
        return new double[] {h, s, max};
    }

    static double[] hsvToRgb(double h, double s, double v) {
        if (s <= 0) return new double[] {v, v, v};
        double hh = h >= 360 ? 0 : h / 60;
        int i = (int) hh;
        double ff = hh - i, p = v * (1 - s), q = v * (1 - s * ff), t = v * (1 - s * (1 - ff));
        switch (i) {
            case 0: return new double[] {v, t, p};
            case 1: return new double[] {q, v, p};
            case 2: return new double[] {p, v, t};
            case 3: return new double[] {p, q, v};
            case 4: return new double[] {t, p, v};
            default: return new double[] {v, p, q};
        }
    }

    static double[] hsv16(int c) { return rgbToHsv((c >> 11 & 0x1F) / 31.0, (c >> 5 & 0x3F) / 63.0, (c & 0x1F) / 31.0); }

    static int hsvTo16(double[] hsv) {
        double[] rgb = hsvToRgb(hsv[0], hsv[1], hsv[2]);
        return (int) Math.round(rgb[0] * 31) << 11 | (int) Math.round(rgb[1] * 63) << 5 | (int) Math.round(rgb[2] * 31);
    }

    static int hexTo16(String hex) {
        int c = rgb(hex);
        return hsvTo16(rgbToHsv((c >> 16 & 0xFF) / 255.0, (c >> 8 & 0xFF) / 255.0, (c & 0xFF) / 255.0));
    }

    static int exchange(int base, int replacement, int cur) {
        double[] b = hsv16(base), r = hsv16(replacement), c = hsv16(cur);
        double sChange = r[1] - b[1], vChange = r[2] - b[2];
        if (c[1] == 0) c[1] = b[1];
        double s = Math.max(0, Math.min(1, c[1] + sChange)), v = Math.max(0, Math.min(1, c[2] + vChange));
        return hsvTo16(new double[] {r[0], s, v});
    }
}
