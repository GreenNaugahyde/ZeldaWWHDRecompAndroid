package org.wwhdrecomp.app;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.Rect;
import android.graphics.RectF;

import java.io.BufferedReader;
import java.io.DataInputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileReader;
import java.io.IOException;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.HashMap;
import java.util.Map;

/**
 * The game's fonts (CKingMain: the GamePad menus' tabs; CKingMsg: their texts and buttons), taken from the player's
 * own game files (runtime/src/android/ui_icons.cpp extract_font) like GameIcons: glyph sheets as alpha masks and
 * the character map. Built once in the background on first use; the app ships none of it. Tab labels are drawn as
 * the game draws them (letters and the glyphs' own outline coloured); texts plain in their colour (GameText).
 */
final class GameFont {
    private static final String DIR = "game_font";
    private static final int VERSION = 1;
    static final GameFont MAIN = new GameFont("CKingMain"), MSG = new GameFont("CKingMsg");

    private static final class Glyph { int sheet, x, y, left, w, advance; }

    private final String FONT;
    private Bitmap[] sheets;
    private final Map<Character, Glyph> glyphs = new HashMap<>();
    private Glyph fallback;
    private int cellW, cellH, baseline;
    private volatile boolean building;
    private boolean failed;

    private GameFont(String font) { FONT = font; }

    // the tab font (CKingMain), as before
    static boolean ready(Context c, Runnable whenReady) { return MAIN.isReady(c, whenReady); }
    static float width(String s) { return MAIN.widthOf(s); }
    static int cellHeight() { return MAIN.cellH; }
    static Bitmap label(String s, float scale, int fill, int outline) { return MAIN.labelOf(s, scale, fill, outline); }
    static float labelBaseline(float scale, float ring2) { return (int) Math.ceil(ring2) + 2 + MAIN.baseline * scale; }

    /** loaded (else it starts building in the background and whenReady runs on the main thread after) */
    synchronized boolean isReady(Context c, Runnable whenReady) {
        if (sheets != null) return true;
        if (failed) return false;
        // per game language: its 2D pack's font has that language's letters (ui_icons.cpp find_pack)
        String lang = MainActivity.instance == null ? "en" : MainActivity.instance.gameLanguage();
        File dir = new File(new File(new File(c.getFilesDir(), DIR), lang), FONT);
        if (new File(dir, "version" + VERSION).exists()) {
            if (load(dir)) return true;
            failed = true;
            return false;
        }
        if (!building && MainActivity.instance != null) {
            building = true;
            String gameDir = MainActivity.instance.gameDir();
            new Thread(() -> {
                build(dir, gameDir);
                synchronized (this) { building = false; }
                if (whenReady != null) new android.os.Handler(android.os.Looper.getMainLooper()).post(whenReady);
            }, "game-font").start();
        }
        return false;
    }

    private void build(File dir, String gameDir) {
        Backup.deleteTree(dir);
        if (!dir.mkdirs() && !dir.isDirectory()) return;
        if (!Native.extractUiFont(gameDir, dir.getAbsolutePath(), FONT)) return;
        try {
            new File(dir, "version" + VERSION).createNewFile();
        } catch (IOException ignored) {
        }
    }

    private boolean load(File dir) {
        try (BufferedReader r = new BufferedReader(new FileReader(new File(dir, FONT + ".txt")))) {
            String[] cell = r.readLine().split(" "), def = r.readLine().split(" ");
            cellW = Integer.parseInt(cell[1]);
            cellH = Integer.parseInt(cell[2]);
            baseline = Integer.parseInt(cell[3]);
            int cols = Integer.parseInt(cell[4]), rows = Integer.parseInt(cell[5]), n = Integer.parseInt(cell[8]);
            Bitmap[] sh = new Bitmap[n];
            for (int i = 0; i < n; i++) sh[i] = loadAlpha(new File(dir, FONT + "_" + i + ".rgba"));
            String line;
            while ((line = r.readLine()) != null) {
                String[] f = line.split(" ");
                int code = Integer.parseInt(f[0]), index = Integer.parseInt(f[1]);
                Glyph g = new Glyph();
                int per = cols * rows, k = index % per;
                g.sheet = index / per;
                // cells are cellW x cellH with a 1-pixel border between them
                g.x = (k % cols) * (cellW + 1) + 1;
                g.y = (k / cols) * (cellH + 1) + 1;
                g.left = Integer.parseInt(f[2]);
                g.w = Integer.parseInt(f[3]);
                g.advance = Integer.parseInt(f[4]);
                if (g.sheet < n) glyphs.put((char) code, g);
            }
            fallback = glyphs.get('?');
            sheets = sh;
            return true;
        } catch (IOException | RuntimeException e) {
            android.util.Log.w("wwhd", "game font: " + e);
            return false;
        }
    }

    private static Bitmap loadAlpha(File f) throws IOException {
        try (DataInputStream in = new DataInputStream(new FileInputStream(f))) {
            byte[] hdr = new byte[8];
            in.readFully(hdr);
            ByteBuffer h = ByteBuffer.wrap(hdr).order(ByteOrder.LITTLE_ENDIAN);
            int w = h.getInt(), ht = h.getInt();
            byte[] px = new byte[w * ht * 4];
            in.readFully(px);
            byte[] a = new byte[w * ht];
            for (int y = 0; y < ht; y++)  // (sheets are stored bottom row first)
                for (int x = 0; x < w; x++) a[y * w + x] = px[((ht - 1 - y) * w + x) * 4 + 3];
            Bitmap b = Bitmap.createBitmap(w, ht, Bitmap.Config.ALPHA_8);
            b.copyPixelsFromBuffer(ByteBuffer.wrap(a));
            b.setHasMipMap(true);  // (drawn much smaller than the sheets' cells)
            return b;
        }
    }

    private Glyph glyph(char ch) {
        Glyph g = glyphs.get(ch);
        if (g == null) {  // a letter the font lacks (the title font has no capital Ä or Á): its plain letter
            String base = java.text.Normalizer.normalize(String.valueOf(ch), java.text.Normalizer.Form.NFD);
            if (base.length() > 1) g = glyphs.get(base.charAt(0));
        }
        return g != null ? g : fallback;
    }

    /** the tab font has every letter of s (else it would show '?': draw s in the system's font instead) */
    static boolean covers(String s) { return MAIN.hasAll(s); }

    synchronized boolean hasAll(String s) {
        if (sheets == null) return false;
        for (int i = 0; i < s.length(); i++) {
            char ch = s.charAt(i);
            if (ch != '?' && glyph(ch) == fallback) return false;
        }
        return true;
    }

    /** the text's width in font pixels */
    float widthOf(String s) {
        float w = 0;
        for (int i = 0; i < s.length(); i++) {
            Glyph g = glyph(s.charAt(i));
            if (g != null) w += g.advance;
        }
        return w;
    }

    private final Map<String, Bitmap> labels = new HashMap<>();

    /**
     * the text as the game's tabs show it, at scale (screen pixels per font pixel): the glyphs carry their outline
     * (coverage ~128 outline, 255 letter), which the game colours: the letters in `fill`, the outline in `outline`
     */
    synchronized Bitmap labelOf(String s, float scale, int fill, int outline) {
        float ring2 = 0;
        String key = s + "|" + scale + "|" + fill + "|" + outline;
        Bitmap cached = labels.get(key);
        if (cached != null) return cached;
        int pad = (int) Math.ceil(ring2) + 2;
        int w = (int) Math.ceil(widthOf(s) * scale) + 2 * pad, h = (int) Math.ceil(cellH * scale) + 2 * pad;
        Bitmap mask = Bitmap.createBitmap(w, h, Bitmap.Config.ALPHA_8);
        Canvas mc = new Canvas(mask);
        Paint p = new Paint(Paint.FILTER_BITMAP_FLAG | Paint.ANTI_ALIAS_FLAG);
        Rect src = new Rect();
        RectF dst = new RectF();
        float x = pad;
        for (int i = 0; i < s.length(); i++) {
            Glyph g = glyph(s.charAt(i));
            if (g == null) continue;
            src.set(g.x, g.y, g.x + g.w, g.y + cellH);
            dst.set(x + g.left * scale, pad, x + (g.left + g.w) * scale, pad + cellH * scale);
            mc.drawBitmap(sheets[g.sheet], src, dst, p);
            x += g.advance * scale;
        }
        // the glyphs coloured: up to 128 the outline fading in, above it the outline turning into the letter
        ByteBuffer mb = ByteBuffer.allocate(mask.getRowBytes() * h);
        mask.copyPixelsToBuffer(mb);
        byte[] m = mb.array();
        int stride = mask.getRowBytes();
        int[] argb = new int[w * h];
        int or = (outline >> 16) & 0xFF, og = (outline >> 8) & 0xFF, ob = outline & 0xFF;
        int fr = (fill >> 16) & 0xFF, fg = (fill >> 8) & 0xFF, fb = fill & 0xFF;
        for (int py = 0; py < h; py++)
            for (int px = 0; px < w; px++) {
                int v = m[py * stride + px] & 0xFF;
                if (v == 0) continue;
                int a = Math.min(255, v * 2), t = Math.max(0, v - 128) * 2;
                int r = or + (fr - or) * t / 255, g = og + (fg - og) * t / 255, bl = ob + (fb - ob) * t / 255;
                argb[py * w + px] = Math.min(255, a) << 24 | Math.min(255, r) << 16 | Math.min(255, g) << 8 | Math.min(255, bl);
            }
        Bitmap glyphs = Bitmap.createBitmap(argb, w, h, Bitmap.Config.ARGB_8888);
        Bitmap out = Bitmap.createBitmap(w, h, Bitmap.Config.ARGB_8888);
        Canvas oc = new Canvas(out);
        oc.drawBitmap(glyphs, 0, 0, null);
        glyphs.recycle();
        mask.recycle();
        if (labels.size() > 32) labels.clear();
        labels.put(key, out);
        return out;
    }

    // ---- plain text (GameText): glyph by glyph, in the paint's colour
    int height() { return cellH; }
    int baselinePx() { return baseline; }
    boolean has(char ch) { return glyphs.containsKey(ch); }
    float advance(char ch) { Glyph g = glyphs.get(ch); return g == null ? 0 : g.advance; }

    private final Rect src = new Rect();
    private final RectF dst = new RectF();

    /** a glyph with its cell's top at y, its origin at x; the advance (screen pixels) */
    float draw(Canvas c, char ch, float x, float y, float scale, Paint p) {
        Glyph g = glyphs.get(ch);
        if (g == null) return 0;
        src.set(g.x, g.y, g.x + g.w, g.y + cellH);
        dst.set(x + g.left * scale, y, x + (g.left + g.w) * scale, y + cellH * scale);
        c.drawBitmap(sheets[g.sheet], src, dst, p);
        return g.advance * scale;
    }
}
