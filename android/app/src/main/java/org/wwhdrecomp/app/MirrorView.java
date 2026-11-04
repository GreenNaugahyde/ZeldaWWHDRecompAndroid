package org.wwhdrecomp.app;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.BitmapShader;
import android.graphics.BlendMode;
import android.graphics.Canvas;
import android.graphics.ColorMatrix;
import android.graphics.ColorMatrixColorFilter;
import android.graphics.ComposeShader;
import android.graphics.Matrix;
import android.graphics.Paint;
import android.graphics.RectF;
import android.graphics.Shader;
import android.view.View;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.HashMap;
import java.util.Map;

/**
 * The game's own GamePad screens (the Picto Box's album, the warp map, Sploosh Kaboom, the wind's choice, the save
 * question ...) on a GamePad device, drawn here as the game draws them: the console sends the GamePad layouts' panes
 * as textured quads (PadLink.MIRROR, runtime drc_mirror.cpp) and the game's textures once each (MIRROR_TEX). Laid over
 * the picture's place (1280 x 720 layout units to its size); touches go to the game as on the picture (PadActivity).
 *
 * Colours as the game's layout shaders make them (their GLSL, 2026-11-03): the texture t (two textures: modulated,
 * t0 t1; added, t0 a0 + t1 a1, the TEV modes 1 and 2; the first alone, mode 0) goes from the material's
 * black (0) to its white (1), times the vertex colour (with the pane's alpha). A glyph of a font with outlines (its
 * sheet: 0.5 the outline, 1 the letter) is the black colour as far as the outline reaches, the white one inside.
 * A texture the game draws into on the GPU (the blurred menu behind a question: empty in its memory) is this
 * device's own drawn screen, blurred (backdrop).
 */
final class MirrorView extends View {
    private static final class Q {
        int ntex, blend, tevC;
        boolean texOnly, font;
        final float[] pos = new float[8];
        final int[] col = new int[8];  // (drawVertices wants room for one per value)
        int black, white;
        final int[] id = new int[2], wrapS = new int[2], wrapT = new int[2];
        final float[][] uv = new float[2][8];
    }

    interface Backdrop { Bitmap get(); }

    private Q[] quads = new Q[0];
    private final Map<Integer, Bitmap> textures = new HashMap<>();
    private final Map<Long, BitmapShader> shaders = new HashMap<>();
    private final java.util.Set<Integer> empty = new java.util.HashSet<>();  // drawn into on the GPU: the backdrop
    private final Map<Integer, Bitmap[]> fontMasks = new HashMap<>();         // {outline, letter} (null: one level)
    private long textureBytes;
    private final Runnable tooMany;  // the textures here take too much memory: dropped, the console sends them anew
    private final Backdrop backdrop;
    private static final long MAX_TEXTURE_BYTES = 192L << 20;

    MirrorView(Context c, Runnable tooMany, Backdrop backdrop) {
        super(c);
        this.tooMany = tooMany;
        this.backdrop = backdrop;
    }

    boolean hasFrame() { return quads.length > 0; }

    /** tests: what came, as files in Android/data/.../files/mirror_dump (only while mirror_dump.on is there) */
    static void dump(Context c, String name, byte[] data) {
        java.io.File base = c.getExternalFilesDir(null), d = new java.io.File(base, "mirror_dump");
        if (!new java.io.File(base, "mirror_dump.on").exists() || (!d.isDirectory() && !d.mkdirs())) return;
        try (java.io.FileOutputStream o = new java.io.FileOutputStream(new java.io.File(d, name))) {
            o.write(data);
        } catch (java.io.IOException ignored) {
        }
    }

    // ---- from the console (any thread: parsing; the main thread: taking them)
    /** a deflated frame (empty: none) to quads; null if it isn't one */
    static Object parse(byte[] deflated) {
        if (deflated.length == 0) return new Q[0];
        byte[] f;
        try {
            java.util.zip.Inflater z = new java.util.zip.Inflater();
            z.setInput(deflated);
            java.io.ByteArrayOutputStream o = new java.io.ByteArrayOutputStream(deflated.length * 4);
            byte[] buf = new byte[1 << 16];
            while (!z.finished()) {
                int n = z.inflate(buf);
                if (n == 0 && (z.needsInput() || z.needsDictionary())) break;
                o.write(buf, 0, n);
            }
            z.end();
            f = o.toByteArray();
        } catch (java.util.zip.DataFormatException e) {
            return null;
        }
        ByteBuffer b = ByteBuffer.wrap(f).order(ByteOrder.LITTLE_ENDIAN);
        if (f.length < 12 || b.getInt() != 0x3152494D) return null;
        int n = b.getInt(), nt = b.getInt();
        b.position(b.position() + 4 * nt);
        Q[] qs = new Q[n];
        for (int i = 0; i < n; i++) {
            Q q = new Q();
            int fl = b.get() & 255;
            q.ntex = fl & 3;
            q.texOnly = (fl & 4) != 0;
            q.font = (fl & 8) != 0;
            q.blend = b.get() & 255;
            q.tevC = b.get() & 255;
            b.get();
            for (int k = 0; k < 8; k++) q.pos[k] = b.getFloat();
            for (int k = 0; k < 4; k++) q.col[k] = b.getInt();
            q.black = b.getInt();
            q.white = b.getInt();
            for (int t = 0; t < q.ntex; t++) {
                q.id[t] = b.getInt();
                q.wrapS[t] = b.get() & 255;
                q.wrapT[t] = b.get() & 255;
                b.getShort();
                for (int k = 0; k < 8; k++) q.uv[t][k] = b.getFloat();
            }
            qs[i] = q;
        }
        return qs;
    }

    void frame(Object parsed) {
        if (!(parsed instanceof Q[])) return;
        quads = (Q[]) parsed;
        dirty = true;
        invalidate();
    }

    void texture(int id, Bitmap b) {
        Bitmap old = textures.put(id, b);
        if (old != null) textureBytes -= old.getByteCount();
        if (b != null) {
            textureBytes += b.getByteCount();
            if (isEmpty(b)) empty.add(id);
        }
        shaders.keySet().removeIf(k -> (int) (k >>> 32) == id);
        fontMasks.remove(id);
        dirty = true;
        if (textureBytes > MAX_TEXTURE_BYTES) {
            clear();
            if (tooMany != null) tooMany.run();
        }
        invalidate();
    }

    private static boolean isEmpty(Bitmap b) {
        int w = b.getWidth(), h = b.getHeight();
        if (w < 256 || h < 128) return false;
        for (int y = 0; y < h; y += Math.max(1, h / 24))
            for (int x = 0; x < w; x += Math.max(1, w / 24))
                if ((b.getPixel(x, y) >>> 24) != 0) return false;
        return true;
    }

    void clear() {
        textures.clear();
        shaders.clear();
        empty.clear();
        fontMasks.clear();
        textureBytes = 0;
        quads = new Q[0];
        back = null;
        dirty = true;
        invalidate();
    }

    // ---- drawing
    private final Paint paint = new Paint(Paint.FILTER_BITMAP_FLAG);
    private final Matrix m = new Matrix();
    private final float[] src = new float[6], dst = new float[6];
    private final Map<Long, ColorMatrixColorFilter> filters = new HashMap<>();
    private final Map<Integer, BitmapShader> solids = new HashMap<>();

    private static Shader.TileMode tile(int gx2) {  // GX2 clamp modes: 0 wrap, 1 mirror, 2 clamp (others: clamp)
        return gx2 == 0 ? Shader.TileMode.REPEAT : gx2 == 1 ? Shader.TileMode.MIRROR : Shader.TileMode.CLAMP;
    }

    /** bitmap b with texture t's coordinates of q, placed on the screen (null: they don't span an area) */
    private BitmapShader placed(Q q, int t, Bitmap b, long key) {
        int w = b.getWidth(), h = b.getHeight();
        float[] uv = q.uv[t];
        src[0] = uv[0] * w; src[1] = uv[1] * h;  // top left, top right, bottom left
        src[2] = uv[2] * w; src[3] = uv[3] * h;
        src[4] = uv[4] * w; src[5] = uv[5] * h;
        float area = (src[2] - src[0]) * (src[5] - src[1]) - (src[3] - src[1]) * (src[4] - src[0]);
        if (Math.abs(area) < 1e-6f) return null;
        dst[0] = q.pos[0]; dst[1] = q.pos[1];
        dst[2] = q.pos[2]; dst[3] = q.pos[3];
        dst[4] = q.pos[4]; dst[5] = q.pos[5];
        if (!m.setPolyToPoly(src, 0, dst, 0, 3)) return null;
        BitmapShader s = shaders.get(key);
        if (s == null) shaders.put(key, s = new BitmapShader(b, tile(q.wrapS[t]), tile(q.wrapT[t])));
        s.setLocalMatrix(m);
        return s;
    }

    /** texture t of q as a shader on the screen (null: not here yet); coordinates on one texel: that colour */
    private Shader shader(Q q, int t) {
        Bitmap b = textures.get(q.id[t]);
        if (b == null) return null;
        Shader s = placed(q, t, b, (long) q.id[t] << 32 | q.wrapS[t] << 8 | q.wrapT[t]);
        if (s != null) return s;
        int w = b.getWidth(), h = b.getHeight();
        return solid(b.getPixel(clampWrap(q.uv[t][0] * w, w, q.wrapS[t]), clampWrap(q.uv[t][1] * h, h, q.wrapT[t])));
    }

    private Shader solid(int argb) {
        BitmapShader s = solids.get(argb);
        if (s == null) {
            Bitmap one = Bitmap.createBitmap(1, 1, Bitmap.Config.ARGB_8888);
            one.setPixel(0, 0, argb);
            if (solids.size() > 256) solids.clear();
            solids.put(argb, s = new BitmapShader(one, Shader.TileMode.CLAMP, Shader.TileMode.CLAMP));
        }
        return s;
    }

    private static int clampWrap(float v, int n, int wrap) {
        int i = (int) Math.floor(v);
        if (wrap == 0) i = ((i % n) + n) % n;
        return Math.max(0, Math.min(n - 1, i));
    }

    /** colour matrix: each channel black + (white - black) t (texture only: t), all times a colour (ARGB) */
    private ColorMatrixColorFilter filter(int black, int white, int times, boolean texOnly) {
        long key = ((long) black * 31 + white) * 31 + times + (texOnly ? 7 : 0);
        ColorMatrixColorFilter f = filters.get(key);
        if (f != null) return f;
        float[] a = new float[20];
        int[] sh = {16, 8, 0, 24};
        for (int r = 0; r < 4; r++) {
            float k = ((times >>> sh[r]) & 255) / 255f;
            float b0 = texOnly ? 0 : (black >>> sh[r]) & 255, w0 = texOnly ? 255 : (white >>> sh[r]) & 255;
            a[r * 5 + r] = (w0 - b0) / 255f * k;
            a[r * 5 + 4] = b0 * k;
        }
        return put(key, a);
    }

    /** colour matrix: rgb = c, alpha = c's alpha times the texture's (a mask in a colour); times a colour */
    private ColorMatrixColorFilter maskFilter(int c, int times) {
        long key = 0x5A5A5A5A00000000L ^ ((long) c * 31 + times);
        ColorMatrixColorFilter f = filters.get(key);
        if (f != null) return f;
        float[] a = new float[20];
        int[] sh = {16, 8, 0};
        for (int r = 0; r < 3; r++) a[r * 5 + 4] = ((c >>> sh[r]) & 255) * (((times >>> sh[r]) & 255) / 255f);
        a[18] = ((c >>> 24) & 255) / 255f * (((times >>> 24) & 255) / 255f);
        return put(key, a);
    }

    /** colour matrix: rgb = (white - black) t, alpha as is; times a colour (the second texture added) */
    private ColorMatrixColorFilter addFilter(int black, int white, int times) {
        long key = 0x3C3C3C3C00000000L ^ (((long) black * 31 + white) * 31 + times);
        ColorMatrixColorFilter f = filters.get(key);
        if (f != null) return f;
        float[] a = new float[20];
        int[] sh = {16, 8, 0};
        for (int r = 0; r < 3; r++)
            a[r * 5 + r] = Math.max(0, (((white >>> sh[r]) & 255) - ((black >>> sh[r]) & 255)) / 255f) * (((times >>> sh[r]) & 255) / 255f);
        a[18] = ((times >>> 24) & 255) / 255f;
        return put(key, a);
    }

    private ColorMatrixColorFilter put(long key, float[] a) {
        ColorMatrixColorFilter f = new ColorMatrixColorFilter(new ColorMatrix(a));
        if (filters.size() > 1024) filters.clear();
        filters.put(key, f);
        return f;
    }

    /** a font sheet's two masks {as far as the outline reaches, the letter} (white, alpha), or null: one level */
    private Bitmap[] masks(int id) {
        if (fontMasks.containsKey(id)) return fontMasks.get(id);
        Bitmap b = textures.get(id);
        Bitmap[] r = null;
        if (b != null) {
            int w = b.getWidth(), h = b.getHeight();
            int[] px = new int[w * h];
            b.getPixels(px, 0, w, 0, 0, w, h);
            int mid = 0, any = 0;
            for (int p : px) {
                int a = p >>> 24;
                if (a > 8) any++;
                if (a > 100 && a < 156) mid++;
            }
            if (any > 0 && mid * 5 > any) {  // (a good part half covered: the outline's level)
                int[] o = new int[px.length], l = new int[px.length];
                for (int i = 0; i < px.length; i++) {
                    int a = px[i] >>> 24;
                    o[i] = Math.min(255, a * 2) << 24 | 0xFFFFFF;
                    l[i] = Math.max(0, a * 2 - 255) << 24 | 0xFFFFFF;
                }
                r = new Bitmap[] {Bitmap.createBitmap(o, w, h, Bitmap.Config.ARGB_8888), Bitmap.createBitmap(l, w, h, Bitmap.Config.ARGB_8888)};
            }
        }
        fontMasks.put(id, r);
        return r;
    }

    private static int mulColor(int x, int y) {
        int o = 0;
        for (int s = 0; s < 32; s += 8) o |= ((((x >>> s) & 255) * ((y >>> s) & 255) + 127) / 255) << s;
        return o;
    }

    private Bitmap back;  // this device's own screen, blurred (for the textures the game draws into on the GPU)

    private boolean dirty = true;  // (a new frame or texture: the test dump writes what is shown)
    private long dumpAt, drawNs;
    private int drawn;

    @Override
    protected void onDraw(Canvas c) {
        int w = getWidth(), h = getHeight();
        if (w <= 0 || h <= 0) return;
        long t0 = System.nanoTime();
        c.drawColor(0xFF000000);
        c.save();
        c.clipRect(0, 0, w, h);
        c.scale(w / 1280f, h / 720f);
        for (Q q : quads) draw(c, q);
        c.restore();
        drawNs += System.nanoTime() - t0;
        if (++drawn % 120 == 0) {
            android.util.Log.i("wwhd", "GamePad device: the game's own screen recorded in " + drawNs / drawn / 100000 / 10f + " ms (" + quads.length + " quads)");
            drawNs = 0;
            drawn = 0;
        }
        long now = android.os.SystemClock.uptimeMillis();
        if (dirty && !BuildConfig.PUBLIC_RELEASE && now - dumpAt > 3000 && quads.length > 0) {  // tests (dump): it in software
            java.io.File base = getContext().getExternalFilesDir(null);
            if (base != null && new java.io.File(base, "mirror_dump.on").exists()) {
                dumpAt = now;
                dirty = false;
                Bitmap b = Bitmap.createBitmap(1280, 720, Bitmap.Config.ARGB_8888);
                Canvas sc = new Canvas(b);
                sc.drawColor(0xFF000000);
                for (Q q : quads) draw(sc, q);
                java.io.ByteArrayOutputStream o = new java.io.ByteArrayOutputStream();
                b.compress(Bitmap.CompressFormat.PNG, 100, o);
                dump(getContext(), "shown.png", o.toByteArray());
            }
        }
    }

    /** the backdrop is taken anew when next needed */
    void dropBackdrop() {
        back = null;
        dirty = true;
    }

    private final RectF box = new RectF();

    private void draw(Canvas c, Q q) {
        paint.reset();
        paint.setFlags(Paint.FILTER_BITMAP_FLAG);
        if (q.blend == 1) paint.setBlendMode(BlendMode.PLUS);
        else if (q.blend == 2) paint.setBlendMode(BlendMode.MULTIPLY);
        boolean same = q.col[0] == q.col[1] && q.col[0] == q.col[2] && q.col[0] == q.col[3];
        int times = same ? q.col[0] : 0xFFFFFFFF;
        int[] cols = same ? null : q.col;
        if (q.ntex == 0) {  // no texture: the material's white
            int[] cs = new int[8];
            for (int k = 0; k < 4; k++) cs[k] = mulColor(q.col[k], q.white);
            c.drawVertices(Canvas.VertexMode.TRIANGLE_STRIP, 8, q.pos, 0, null, 0, cs, 0, null, 0, 0, paint);
            return;
        }
        if (empty.contains(q.id[0])) {  // drawn into on the GPU: the whole screen this device's own one, blurred; else none
            box.set(Math.min(q.pos[0], q.pos[4]), Math.min(q.pos[1], q.pos[3]), Math.max(q.pos[2], q.pos[6]), Math.max(q.pos[5], q.pos[7]));
            if (box.width() < 1200 || box.height() < 680) return;
            if (back == null && backdrop != null) back = backdrop.get();
            if (back != null) c.drawBitmap(back, null, box, paint);
            return;
        }
        if (q.font) {
            Bitmap[] mk = masks(q.id[0]);
            if (mk != null) {  // the outline in the black colour, the letter in the white one
                if ((q.black >>> 24) != 0) {
                    BitmapShader so = placed(q, 0, mk[0], (long) q.id[0] << 32 | 0xF0);
                    if (so != null) {
                        paint.setShader(so);
                        paint.setColorFilter(maskFilter(q.black, times));
                        c.drawVertices(Canvas.VertexMode.TRIANGLE_STRIP, 8, q.pos, 0, q.pos, 0, cols, 0, null, 0, 0, paint);
                    }
                }
                BitmapShader sl = placed(q, 0, mk[1], (long) q.id[0] << 32 | 0xF1);
                if (sl != null) {
                    paint.setShader(sl);
                    paint.setColorFilter(maskFilter(q.white, times));
                    c.drawVertices(Canvas.VertexMode.TRIANGLE_STRIP, 8, q.pos, 0, q.pos, 0, cols, 0, null, 0, 0, paint);
                }
                return;
            }
        }
        Shader s0 = shader(q, 0);
        if (s0 == null) return;  // (not here yet)
        Shader s1 = q.ntex > 1 ? shader(q, 1) : null;
        boolean add = s1 != null && (q.tevC == 1 || q.tevC == 2);
        paint.setShader(s1 == null || add || q.tevC == 0 ? s0 : new ComposeShader(s0, s1, BlendMode.MODULATE));
        paint.setColorFilter(filter(q.black, q.white, times, q.texOnly));
        c.drawVertices(Canvas.VertexMode.TRIANGLE_STRIP, 8, q.pos, 0, q.pos, 0, cols, 0, null, 0, 0, paint);
        if (add) {  // the second texture added where the first one is: (white - black) t1, on top
            paint.setShader(new ComposeShader(s0, s1, BlendMode.SRC_IN));
            paint.setColorFilter(addFilter(q.black, q.white, times));
            paint.setBlendMode(BlendMode.PLUS);
            c.drawVertices(Canvas.VertexMode.TRIANGLE_STRIP, 8, q.pos, 0, q.pos, 0, cols, 0, null, 0, 0, paint);
        }
    }
}
