package org.wwhdrecomp.app;

import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.BlendMode;
import android.graphics.BlendModeColorFilter;
import android.graphics.Canvas;
import android.graphics.ColorFilter;
import android.graphics.ColorMatrixColorFilter;
import android.graphics.LightingColorFilter;
import android.graphics.Matrix;
import android.graphics.NinePatch;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.PorterDuff;
import android.graphics.Rect;
import android.graphics.RectF;
import android.graphics.Region;
import android.graphics.Typeface;
import android.view.View;

import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.io.DataInputStream;
import java.io.DataOutputStream;
import java.io.IOException;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.WeakHashMap;

/**
 * The app's dialogs on the GamePad device as drawing orders instead of pictures (PadUi, PadLink.UI_DRAW): the console
 * draws the dialog's views into a Recorder, a Canvas that keeps each call (rectangles, rounded boxes, text with its
 * font and size, paths, bitmaps by number) instead of painting pixels; the GamePad device plays them back on its own
 * screen at its own size (Player), sharp at any scale. Bitmaps (icons, thumbnails, the game font's glyph sheets) go
 * over once each (PadLink.UI_BITMAP, by content) and are kept there. What can't be described (a gradient, a blur, a
 * dash pattern, a nine-patch) is painted into a small bitmap on the console for that one call.
 */
final class UiDraw {
    private UiDraw() {}

    // the orders
    private static final int SAVE = 1, SAVE_LAYER = 2, RESTORE_TO = 3, TRANSLATE = 4, SCALE = 5, ROTATE = 6, SKEW = 7,
            CONCAT = 8, SET_MATRIX = 9, CLIP_RECT = 10, CLIP_PATH = 11, PAINT = 12, COLOR = 13, RECT = 14, RRECT = 15,
            OVAL = 16, CIRCLE = 17, ARC = 18, LINES = 19, POINTS = 20, PATH = 21, BITMAP = 22, BITMAP_MATRIX = 23,
            TEXT = 24, TEXT_RUN = 25, DEVICE_IMAGE = 26, DRAW_PAINT = 27, DRRECT = 28, END = 0;

    /** a PorterDuff mode as the BlendMode that paints the same */
    static BlendMode blendOf(PorterDuff.Mode m) {
        if (m == null) return null;
        switch (m) {
            case MULTIPLY: return BlendMode.MODULATE;
            case ADD: return BlendMode.PLUS;
            default:
                try {
                    return BlendMode.valueOf(m.name());
                } catch (IllegalArgumentException e) {
                    return null;
                }
        }
    }

    // ------------------------------------------------------------------ fonts by number (the same list both ends)
    private static final String[] FAMILIES = {null, "sans-serif", "sans-serif-medium", "sans-serif-black",
            "sans-serif-light", "sans-serif-condensed", "serif", "monospace"};
    private static Typeface[] faces;

    private static synchronized Typeface[] faces() {
        if (faces == null) {
            List<Typeface> l = new ArrayList<>();
            l.add(Typeface.DEFAULT);
            l.add(Typeface.DEFAULT_BOLD);
            l.add(Typeface.SANS_SERIF);
            l.add(Typeface.SERIF);
            l.add(Typeface.MONOSPACE);
            for (String f : FAMILIES)
                for (int s = 0; s < 4; s++) l.add(f == null ? Typeface.create(Typeface.DEFAULT, s) : Typeface.create(f, s));
            faces = l.toArray(new Typeface[0]);
        }
        return faces;
    }

    // ------------------------------------------------------------------ bitmaps by content
    /** The console's bitmaps by number: each content once; the ones the GamePad device doesn't have yet wait in `fresh`. */
    static final class Images {
        private final WeakHashMap<Bitmap, long[]> known = new WeakHashMap<>();  // bitmap -> {generation, number}
        private final Map<Long, Integer> byContent = new HashMap<>();
        private final java.util.Set<Integer> sent = new java.util.HashSet<>();
        final List<Object[]> fresh = new ArrayList<>();  // {number, Bitmap} to send before the orders
        private int next = 1;
        private java.nio.ByteBuffer buf;

        /** a new GamePad device: it has none */
        synchronized void reset() { sent.clear(); }

        synchronized int id(Bitmap b) {
            if (b == null || b.isRecycled()) return 0;
            long[] k = known.get(b);
            int gen = b.getGenerationId();
            int id;
            if (k != null && k[0] == gen) id = (int) k[1];
            else {
                Bitmap c = b.getConfig() == Bitmap.Config.HARDWARE ? b.copy(Bitmap.Config.ARGB_8888, false) : b;
                if (c == null) return 0;
                int n = c.getByteCount();
                if (buf == null || buf.capacity() < n) buf = java.nio.ByteBuffer.allocate(n);
                buf.clear();
                c.copyPixelsToBuffer(buf);
                java.util.zip.CRC32 crc = new java.util.zip.CRC32();
                crc.update(buf.array(), 0, n);
                long key = (crc.getValue() << 24) ^ ((long) c.getWidth() << 12) ^ c.getHeight()
                        ^ ((long) (c.getConfig() == null ? 9 : c.getConfig().ordinal()) << 56);
                if (c != b) c.recycle();
                Integer have = byContent.get(key);
                if (have == null) {
                    have = next++;
                    byContent.put(key, have);
                }
                id = have;
                known.put(b, new long[] {gen, id});
            }
            if (sent.add(id)) {
                // a copy: the view may change or recycle its bitmap before it is encoded (alpha masks stay masks:
                // the paint's colour tints them, the game font's glyph sheets)
                Bitmap.Config cf = b.getConfig() == Bitmap.Config.ALPHA_8 ? Bitmap.Config.ALPHA_8 : Bitmap.Config.ARGB_8888;
                Bitmap copy = b.copy(cf, false);
                if (copy != null) fresh.add(new Object[] {id, copy});
                else sent.remove(id);
            }
            return id;
        }

        synchronized List<Object[]> takeFresh() {
            List<Object[]> l = new ArrayList<>(fresh);
            fresh.clear();
            return l;
        }
    }

    /** a bitmap for the wire: an alpha mask raw (deflated), else PNG (alpha, small) or JPEG (large opaque ones) */
    static byte[] encode(Bitmap b) {
        ByteArrayOutputStream out = new ByteArrayOutputStream(Math.max(1024, b.getByteCount() / 8));
        if (b.getConfig() == Bitmap.Config.ALPHA_8) {
            java.nio.ByteBuffer px = java.nio.ByteBuffer.allocate(b.getByteCount());
            b.copyPixelsToBuffer(px);
            out.write(1);
            DataOutputStream d = new DataOutputStream(out);
            try {
                d.writeInt(b.getWidth());
                d.writeInt(b.getHeight());
                d.writeInt(b.getRowBytes());
                java.util.zip.Deflater z = new java.util.zip.Deflater(1);
                try (java.util.zip.DeflaterOutputStream dz = new java.util.zip.DeflaterOutputStream(out, z)) {
                    dz.write(px.array(), 0, px.position());
                } finally {
                    z.end();
                }
            } catch (IOException e) {
                throw new IllegalStateException(e);
            }
            return out.toByteArray();
        }
        out.write(0);
        boolean jpeg = !b.hasAlpha() && b.getWidth() * b.getHeight() > 160 * 160;
        b.compress(jpeg ? Bitmap.CompressFormat.JPEG : Bitmap.CompressFormat.PNG, 90, out);
        return out.toByteArray();
    }

    /** encode's bitmap back */
    static Bitmap decode(byte[] data, int off, int len) {
        if (len < 1) return null;
        if (data[off] == 0) return BitmapFactory.decodeByteArray(data, off + 1, len - 1);
        try {
            DataInputStream d = new DataInputStream(new ByteArrayInputStream(data, off + 1, len - 1));
            int w = d.readInt(), h = d.readInt(), row = d.readInt();
            byte[] px = new byte[row * h];
            new DataInputStream(new java.util.zip.InflaterInputStream(d)).readFully(px);
            Bitmap b = Bitmap.createBitmap(w, h, Bitmap.Config.ALPHA_8);
            if (b.getRowBytes() != row) return null;
            b.copyPixelsFromBuffer(java.nio.ByteBuffer.wrap(px));
            return b;
        } catch (IOException | RuntimeException e) {
            return null;
        }
    }

    // ------------------------------------------------------------------ the console: recording
    static final class Recorder extends Canvas {
        private final Images images;
        private ByteArrayOutputStream bytes;
        private DataOutputStream o;
        private int guard;  // inside a recorded call: Canvas's own calls of its other methods aren't kept twice
        private final Map<String, Integer> paints = new HashMap<>();
        private final ByteArrayOutputStream paintBytes = new ByteArrayOutputStream(128);
        private final DataOutputStream po = new DataOutputStream(paintBytes);
        private Bitmap backing;
        private int w, h;
        private final Matrix m = new Matrix();
        private final RectF tmp = new RectF();

        Recorder(Images images) { this.images = images; }

        /** the view's drawing as orders (deflated), w x h its size */
        byte[] record(View root) {
            w = root.getWidth();
            h = root.getHeight();
            // the clip and the matrix are kept by Canvas itself (views ask it what is visible): a bitmap of the size,
            // never painted (the draw calls don't reach it)
            if (backing == null || backing.getWidth() != w || backing.getHeight() != h) {
                if (backing != null) backing.recycle();
                backing = Bitmap.createBitmap(Math.max(1, w), Math.max(1, h), Bitmap.Config.ALPHA_8);
            }
            setBitmap(backing);
            bytes = new ByteArrayOutputStream(1 << 15);
            o = new DataOutputStream(bytes);
            paints.clear();
            guard = 0;
            recDepth = getSaveCount();
            try {
                root.draw(this);
                o.writeByte(END);
            } catch (IOException e) {
                throw new IllegalStateException(e);
            } finally {
                setBitmap(null);
            }
            ByteArrayOutputStream z = new ByteArrayOutputStream(bytes.size() / 3 + 64);
            java.util.zip.Deflater d = new java.util.zip.Deflater(1);
            try (java.util.zip.DeflaterOutputStream dz = new java.util.zip.DeflaterOutputStream(z, d)) {
                bytes.writeTo(dz);
            } catch (IOException e) {
                throw new IllegalStateException(e);
            } finally {
                d.end();
            }
            return z.toByteArray();
        }

        @Override public boolean isHardwareAccelerated() { return false; }

        private boolean enter() { return guard++ == 0; }

        private void leave() { guard--; }

        private void op(int code) throws IOException { o.writeByte(code); }

        private void f(float... v) throws IOException { for (float x : v) o.writeFloat(x); }

        // ---- the state (kept by Canvas too)
        @Override public int save() {
            if (enter()) {
                rec(() -> op(SAVE));
                recDepth++;
            }
            try { return super.save(); } finally { leave(); }
        }

        private int layer(float l, float t, float r, float b, int alpha) {
            if (enter()) {
                rec(() -> { op(SAVE_LAYER); f(l, t, r, b); o.writeByte(alpha); });
                recDepth++;
            }
            try { return super.saveLayerAlpha(l, t, r, b, alpha); } finally { leave(); }
        }

        private static int alphaOf(Paint p) { return p == null ? 255 : p.getAlpha(); }

        private final Rect clipNow = new Rect();

        private RectF clipF() {
            getClipBounds(clipNow);
            return new RectF(clipNow);
        }

        @Override public int saveLayer(RectF b, Paint p) {
            if (guard > 0) return super.saveLayer(b, p);
            if (b == null) b = clipF();
            return layer(b.left, b.top, b.right, b.bottom, alphaOf(p));
        }
        @Override public int saveLayer(RectF b, Paint p, int flags) {
            if (guard > 0) return super.saveLayer(b, p, flags);
            return saveLayer(b, p);
        }
        @Override public int saveLayer(float l, float t, float r, float b, Paint p) {
            if (guard > 0) return super.saveLayer(l, t, r, b, p);
            return layer(l, t, r, b, alphaOf(p));
        }
        @Override public int saveLayer(float l, float t, float r, float b, Paint p, int flags) {
            if (guard > 0) return super.saveLayer(l, t, r, b, p, flags);
            return layer(l, t, r, b, alphaOf(p));
        }
        @Override public int saveLayerAlpha(RectF b, int a) {
            if (guard > 0) return super.saveLayerAlpha(b, a);
            if (b == null) b = clipF();
            return layer(b.left, b.top, b.right, b.bottom, a);
        }
        @Override public int saveLayerAlpha(RectF b, int a, int flags) {
            if (guard > 0) return super.saveLayerAlpha(b, a, flags);
            return saveLayerAlpha(b, a);
        }
        @Override public int saveLayerAlpha(float l, float t, float r, float b, int a) {
            if (guard > 0) return super.saveLayerAlpha(l, t, r, b, a);
            return layer(l, t, r, b, a);
        }
        @Override public int saveLayerAlpha(float l, float t, float r, float b, int a, int flags) {
            if (guard > 0) return super.saveLayerAlpha(l, t, r, b, a, flags);
            return layer(l, t, r, b, a);
        }

        @Override public void restore() {
            if (enter()) {
                int to = getSaveCount() - 1;
                rec(() -> { op(RESTORE_TO); o.writeShort(to); });
                recDepth = Math.min(recDepth, to);
            }
            try { super.restore(); } finally { leave(); }
        }

        @Override public void restoreToCount(int n) {
            if (enter()) {
                rec(() -> { op(RESTORE_TO); o.writeShort(n); });
                recDepth = Math.min(recDepth, n);
            }
            try { super.restoreToCount(n); } finally { leave(); }
        }

        @Override public void translate(float dx, float dy) {
            if (dx == 0 && dy == 0) return;
            if (enter()) rec(() -> { op(TRANSLATE); f(dx, dy); });
            try { super.translate(dx, dy); } finally { leave(); }
        }

        @Override public void scale(float sx, float sy) {
            if (enter()) rec(() -> { op(SCALE); f(sx, sy); });
            try { super.scale(sx, sy); } finally { leave(); }
        }

        @Override public void rotate(float d) {
            if (enter()) rec(() -> { op(ROTATE); f(d); });
            try { super.rotate(d); } finally { leave(); }
        }

        @Override public void skew(float sx, float sy) {
            if (enter()) rec(() -> { op(SKEW); f(sx, sy); });
            try { super.skew(sx, sy); } finally { leave(); }
        }

        @Override public void concat(Matrix mx) {
            if (mx == null || mx.isIdentity()) return;
            if (enter()) rec(() -> { op(CONCAT); matrix(mx); });
            try { super.concat(mx); } finally { leave(); }
        }

        @Override public void setMatrix(Matrix mx) {
            if (enter()) rec(() -> { op(SET_MATRIX); matrix(mx == null ? new Matrix() : mx); });
            try { super.setMatrix(mx); } finally { leave(); }
        }

        private void matrix(Matrix mx) throws IOException {
            float[] v = new float[9];
            mx.getValues(v);
            f(v);
        }

        private void clip(float l, float t, float r, float b, boolean out) {
            rec(() -> { op(CLIP_RECT); f(l, t, r, b); o.writeBoolean(out); });
        }

        @Override public boolean clipRect(RectF r) {
            if (enter()) clip(r.left, r.top, r.right, r.bottom, false);
            try { return super.clipRect(r); } finally { leave(); }
        }
        @Override public boolean clipRect(Rect r) {
            if (enter()) clip(r.left, r.top, r.right, r.bottom, false);
            try { return super.clipRect(r); } finally { leave(); }
        }
        @Override public boolean clipRect(float l, float t, float r, float b) {
            if (enter()) clip(l, t, r, b, false);
            try { return super.clipRect(l, t, r, b); } finally { leave(); }
        }
        @Override public boolean clipRect(int l, int t, int r, int b) {
            if (enter()) clip(l, t, r, b, false);
            try { return super.clipRect(l, t, r, b); } finally { leave(); }
        }
        @SuppressWarnings("deprecation")
        @Override public boolean clipRect(RectF r, Region.Op op) {
            if (enter()) clip(r.left, r.top, r.right, r.bottom, op == Region.Op.DIFFERENCE);
            try { return super.clipRect(r, op); } finally { leave(); }
        }
        @SuppressWarnings("deprecation")
        @Override public boolean clipRect(Rect r, Region.Op op) {
            if (enter()) clip(r.left, r.top, r.right, r.bottom, op == Region.Op.DIFFERENCE);
            try { return super.clipRect(r, op); } finally { leave(); }
        }
        @SuppressWarnings("deprecation")
        @Override public boolean clipRect(float l, float t, float r, float b, Region.Op op) {
            if (enter()) clip(l, t, r, b, op == Region.Op.DIFFERENCE);
            try { return super.clipRect(l, t, r, b, op); } finally { leave(); }
        }
        @Override public boolean clipOutRect(RectF r) {
            if (enter()) clip(r.left, r.top, r.right, r.bottom, true);
            try { return super.clipOutRect(r); } finally { leave(); }
        }
        @Override public boolean clipOutRect(Rect r) {
            if (enter()) clip(r.left, r.top, r.right, r.bottom, true);
            try { return super.clipOutRect(r); } finally { leave(); }
        }
        @Override public boolean clipOutRect(float l, float t, float r, float b) {
            if (enter()) clip(l, t, r, b, true);
            try { return super.clipOutRect(l, t, r, b); } finally { leave(); }
        }
        @Override public boolean clipOutRect(int l, int t, int r, int b) {
            if (enter()) clip(l, t, r, b, true);
            try { return super.clipOutRect(l, t, r, b); } finally { leave(); }
        }
        @Override public boolean clipPath(Path p) {
            if (enter()) rec(() -> { op(CLIP_PATH); path(p); o.writeBoolean(false); });
            try { return super.clipPath(p); } finally { leave(); }
        }
        @Override public boolean clipOutPath(Path p) {
            if (enter()) rec(() -> { op(CLIP_PATH); path(p); o.writeBoolean(true); });
            try { return super.clipOutPath(p); } finally { leave(); }
        }
        @SuppressWarnings("deprecation")
        @Override public boolean clipPath(Path p, Region.Op op) {
            if (enter()) rec(() -> { op(CLIP_PATH); path(p); o.writeBoolean(op == Region.Op.DIFFERENCE); });
            try { return super.clipPath(p, op); } finally { leave(); }
        }

        private void path(Path p) throws IOException {
            float[] pts = p.approximate(0.2f);
            o.writeByte(p.getFillType().ordinal());
            o.writeInt(pts.length / 3);
            for (int i = 0; i < pts.length; i += 3) {
                o.writeBoolean(i == 0 || pts[i] == pts[i - 3]);  // a jump (a new contour)
                f(pts[i + 1], pts[i + 2]);
            }
        }

        // ---- paints
        private interface Rec { void run() throws IOException; }

        // the saves the GamePad device has made (its depth counted as here): Canvas's hidden saves (View's own,
        // save(flags), unclipped layers) aren't seen; before each order the missing ones are added
        private int recDepth;

        private void sync() throws IOException {
            int now = getSaveCount();
            while (recDepth < now) {
                o.writeByte(SAVE);
                recDepth++;
            }
        }

        private void rec(Rec r) {
            try {
                sync();
                r.run();
            } catch (IOException e) {
                throw new IllegalStateException(e);
            }
        }

        /** whether the paint can be described (else its call is painted into a bitmap here) */
        private static boolean plain(Paint p) {
            if (p.getShader() != null || p.getMaskFilter() != null || p.getPathEffect() != null) return false;
            ColorFilter cf = p.getColorFilter();
            return cf == null || cf instanceof ColorMatrixColorFilter || cf instanceof BlendModeColorFilter || cf instanceof LightingColorFilter
                    || (cf instanceof android.graphics.PorterDuffColorFilter && tint(cf) != null);
        }

        private static java.lang.reflect.Method pdColor, pdMode;
        private static boolean pdTried;

        /** a PorterDuffColorFilter's colour and mode as a BlendMode filter's {color, BlendMode ordinal}, null if unreadable */
        private static int[] tint(ColorFilter cf) {
            if (!pdTried) {
                pdTried = true;
                try {
                    pdColor = android.graphics.PorterDuffColorFilter.class.getDeclaredMethod("getColor");
                    pdMode = android.graphics.PorterDuffColorFilter.class.getDeclaredMethod("getMode");
                    pdColor.setAccessible(true);
                    pdMode.setAccessible(true);
                } catch (ReflectiveOperationException | RuntimeException e) {
                    pdColor = pdMode = null;
                }
            }
            if (pdColor == null) return null;
            try {
                int color = (Integer) pdColor.invoke(cf);
                PorterDuff.Mode m = (PorterDuff.Mode) pdMode.invoke(cf);
                BlendMode b = blendOf(m);
                return b == null ? null : new int[] {color, b.ordinal()};
            } catch (ReflectiveOperationException | RuntimeException e) {
                return null;
            }
        }


        private int paint(Paint p) throws IOException {
            paintBytes.reset();
            po.writeInt(p.getFlags());
            po.writeInt(p.getColor());
            po.writeByte(p.getStyle().ordinal());
            po.writeFloat(p.getStrokeWidth());
            po.writeByte(p.getStrokeCap().ordinal());
            po.writeByte(p.getStrokeJoin().ordinal());
            po.writeFloat(p.getStrokeMiter());
            po.writeFloat(p.getTextSize());
            po.writeFloat(p.getTextScaleX());
            po.writeFloat(p.getTextSkewX());
            po.writeFloat(p.getLetterSpacing());
            po.writeByte(p.getTextAlign().ordinal());
            Typeface tf = p.getTypeface();
            int face = 0;
            if (tf != null) {
                Typeface[] all = faces();
                face = -1;
                for (int i = 0; i < all.length; i++) if (all[i] == tf) { face = i; break; }
            }
            po.writeShort(face);
            if (face < 0) {  // (another font: its weight and slant on the default)
                po.writeShort(tf.getWeight());
                po.writeBoolean(tf.isItalic());
            }
            float sr = p.getShadowLayerRadius();
            po.writeFloat(sr);
            if (sr > 0) {
                po.writeFloat(p.getShadowLayerDx());
                po.writeFloat(p.getShadowLayerDy());
                po.writeInt(p.getShadowLayerColor());
            }
            BlendMode bm = p.getBlendMode();
            po.writeByte(bm == null ? -1 : bm.ordinal());
            ColorFilter cf = p.getColorFilter();
            if (cf instanceof ColorMatrixColorFilter) {
                po.writeByte(1);
                android.graphics.ColorMatrix cm = new android.graphics.ColorMatrix();
                ((ColorMatrixColorFilter) cf).getColorMatrix(cm);
                for (float v : cm.getArray()) po.writeFloat(v);
            } else if (cf instanceof BlendModeColorFilter) {
                po.writeByte(2);
                po.writeInt(((BlendModeColorFilter) cf).getColor());
                po.writeByte(((BlendModeColorFilter) cf).getMode().ordinal());
            } else if (cf instanceof android.graphics.PorterDuffColorFilter) {
                int[] t = tint(cf);
                po.writeByte(2);
                po.writeInt(t[0]);
                po.writeByte(t[1]);
            } else if (cf instanceof LightingColorFilter) {
                po.writeByte(3);
                po.writeInt(((LightingColorFilter) cf).getColorMultiply());
                po.writeInt(((LightingColorFilter) cf).getColorAdd());
            } else po.writeByte(0);
            String key = paintBytes.toString("ISO-8859-1");
            Integer n = paints.get(key);
            if (n == null) {
                n = paints.size();
                paints.put(key, n);
                op(PAINT);
                o.writeShort(paintBytes.size());
                paintBytes.writeTo(o);
            }
            return n;
        }

        // ---- calls painted here (a paint that can't be described): into a bitmap over the call's place on the screen
        private interface Paint2 { void draw(Canvas c); }

        private void raster(float l, float t, float r, float b, Paint p, Paint2 d) {
            float pad = p == null ? 2 : p.getStrokeWidth() / 2 + 2 + (p.getMaskFilter() != null ? 24 : 0);
            tmp.set(l - pad, t - pad, r + pad, b + pad);
            getMatrix(m);
            m.mapRect(tmp);
            tmp.intersect(0, 0, w, h);
            int bw = (int) Math.ceil(tmp.width()), bh = (int) Math.ceil(tmp.height());
            if (bw <= 0 || bh <= 0) return;
            int x0 = (int) Math.floor(tmp.left), y0 = (int) Math.floor(tmp.top);
            Bitmap bmp = Bitmap.createBitmap(bw, bh, Bitmap.Config.ARGB_8888);
            Canvas c = new Canvas(bmp);
            c.translate(-x0, -y0);
            c.concat(m);
            // painted alone, without its blend mode (on an empty bitmap SRC_IN and the like leave nothing): the
            // picture then goes onto what lies there with that mode
            BlendMode bm = p == null ? null : p.getBlendMode();
            if (bm != null) p.setBlendMode(null);
            try {
                d.draw(c);
            } finally {
                if (bm != null) p.setBlendMode(bm);
            }
            int id = images.id(bmp);
            bmp.recycle();
            rec(() -> { op(DEVICE_IMAGE); o.writeInt(id); f(x0, y0, x0 + bw, y0 + bh); o.writeByte(bm == null ? -1 : bm.ordinal()); });
        }

        // ---- drawing
        @Override public void drawColor(int color) { if (enter()) rec(() -> { op(COLOR); o.writeInt(color); o.writeByte(BlendMode.SRC_OVER.ordinal()); }); leave(); }
        @Override public void drawColor(int color, PorterDuff.Mode mode) {
            if (enter()) rec(() -> { op(COLOR); o.writeInt(color); o.writeByte(mode == PorterDuff.Mode.CLEAR ? BlendMode.CLEAR.ordinal() : mode == PorterDuff.Mode.SRC ? BlendMode.SRC.ordinal() : BlendMode.SRC_OVER.ordinal()); });
            leave();
        }
        @Override public void drawColor(int color, BlendMode mode) { if (enter()) rec(() -> { op(COLOR); o.writeInt(color); o.writeByte(mode.ordinal()); }); leave(); }
        @Override public void drawColor(long color) { drawColor(android.graphics.Color.toArgb(color)); }
        @Override public void drawColor(long color, BlendMode mode) { drawColor(android.graphics.Color.toArgb(color), mode); }
        @Override public void drawARGB(int a, int r, int g, int b) { drawColor(android.graphics.Color.argb(a, r, g, b)); }
        @Override public void drawRGB(int r, int g, int b) { drawColor(android.graphics.Color.rgb(r, g, b)); }

        @Override public void drawPaint(Paint p) {
            if (enter()) {
                if (plain(p)) rec(() -> { int n = paint(p); op(DRAW_PAINT); o.writeShort(n); });
                else {
                    getClipBounds(rect);
                    raster(rect.left, rect.top, rect.right, rect.bottom, p, c -> c.drawPaint(p));
                }
            }
            leave();
        }

        private final Rect rect = new Rect();

        private void shape(int code, float l, float t, float r, float b, Paint p, Rec extra, Paint2 d) {
            if (!enter()) { leave(); return; }
            try {
                if (plain(p)) rec(() -> { int n = paint(p); op(code); o.writeShort(n); f(l, t, r, b); if (extra != null) extra.run(); });
                else raster(Math.min(l, r), Math.min(t, b), Math.max(l, r), Math.max(t, b), p, d);
            } finally {
                leave();
            }
        }

        @Override public void drawRect(float l, float t, float r, float b, Paint p) { shape(RECT, l, t, r, b, p, null, c -> c.drawRect(l, t, r, b, p)); }
        @Override public void drawRect(RectF r, Paint p) { drawRect(r.left, r.top, r.right, r.bottom, p); }
        @Override public void drawRect(Rect r, Paint p) { drawRect(r.left, r.top, r.right, r.bottom, p); }
        @Override public void drawRoundRect(float l, float t, float r, float b, float rx, float ry, Paint p) {
            shape(RRECT, l, t, r, b, p, () -> f(rx, ry), c -> c.drawRoundRect(l, t, r, b, rx, ry, p));
        }
        @Override public void drawRoundRect(RectF r, float rx, float ry, Paint p) { drawRoundRect(r.left, r.top, r.right, r.bottom, rx, ry, p); }
        @Override public void drawOval(float l, float t, float r, float b, Paint p) { shape(OVAL, l, t, r, b, p, null, c -> c.drawOval(l, t, r, b, p)); }
        @Override public void drawOval(RectF r, Paint p) { drawOval(r.left, r.top, r.right, r.bottom, p); }
        @Override public void drawCircle(float x, float y, float rad, Paint p) {
            shape(CIRCLE, x - rad, y - rad, x + rad, y + rad, p, null, c -> c.drawCircle(x, y, rad, p));
        }
        @Override public void drawArc(float l, float t, float r, float b, float start, float sweep, boolean center, Paint p) {
            shape(ARC, l, t, r, b, p, () -> { f(start, sweep); o.writeBoolean(center); }, c -> c.drawArc(l, t, r, b, start, sweep, center, p));
        }
        @Override public void drawArc(RectF r, float start, float sweep, boolean center, Paint p) { drawArc(r.left, r.top, r.right, r.bottom, start, sweep, center, p); }
        @Override public void drawDoubleRoundRect(RectF out, float ox, float oy, RectF in, float ix, float iy, Paint p) {
            shape(DRRECT, out.left, out.top, out.right, out.bottom, p, () -> f(ox, oy, in.left, in.top, in.right, in.bottom, ix, iy),
                    c -> c.drawDoubleRoundRect(out, ox, oy, in, ix, iy, p));
        }
        @Override public void drawDoubleRoundRect(RectF out, float[] or, RectF in, float[] ir, Paint p) {
            if (!enter()) { leave(); return; }
            try {
                raster(out.left, out.top, out.right, out.bottom, p, c -> c.drawDoubleRoundRect(out, or, in, ir, p));
            } finally {
                leave();
            }
        }

        private void lines(int code, float[] pts, int off, int count, Paint p) {
            if (!enter()) { leave(); return; }
            try {
                if (plain(p)) rec(() -> { int n = paint(p); op(code); o.writeShort(n); o.writeInt(count); for (int i = 0; i < count; i++) o.writeFloat(pts[off + i]); });
                else {
                    float l = Float.MAX_VALUE, t = Float.MAX_VALUE, r = -Float.MAX_VALUE, b = -Float.MAX_VALUE;
                    for (int i = 0; i + 1 < count; i += 2) {
                        l = Math.min(l, pts[off + i]); r = Math.max(r, pts[off + i]);
                        t = Math.min(t, pts[off + i + 1]); b = Math.max(b, pts[off + i + 1]);
                    }
                    if (code == LINES) raster(l, t, r, b, p, c -> c.drawLines(pts, off, count, p));
                    else raster(l, t, r, b, p, c -> c.drawPoints(pts, off, count, p));
                }
            } finally {
                leave();
            }
        }

        @Override public void drawLine(float x0, float y0, float x1, float y1, Paint p) { lines(LINES, new float[] {x0, y0, x1, y1}, 0, 4, p); }
        @Override public void drawLines(float[] pts, int off, int count, Paint p) { lines(LINES, pts, off, count, p); }
        @Override public void drawLines(float[] pts, Paint p) { lines(LINES, pts, 0, pts.length, p); }
        @Override public void drawPoint(float x, float y, Paint p) { lines(POINTS, new float[] {x, y}, 0, 2, p); }
        @Override public void drawPoints(float[] pts, int off, int count, Paint p) { lines(POINTS, pts, off, count, p); }
        @Override public void drawPoints(float[] pts, Paint p) { lines(POINTS, pts, 0, pts.length, p); }

        @Override public void drawPath(Path path, Paint p) {
            if (!enter()) { leave(); return; }
            try {
                if (plain(p)) rec(() -> { int n = paint(p); op(PATH); o.writeShort(n); path(path); });
                else {
                    path.computeBounds(tmp, true);
                    RectF b = new RectF(tmp);
                    raster(b.left, b.top, b.right, b.bottom, p, c -> c.drawPath(path, p));
                }
            } finally {
                leave();
            }
        }

        @Override public void drawRegion(Region r, Paint p) {
            Path path = r.getBoundaryPath();
            drawPath(path, p);
        }

        private void bitmap(Bitmap b, Rect src, float l, float t, float r, float bo, Paint p) {
            if (!enter()) { leave(); return; }
            try {
                if (p != null && !plain(p)) {  // (a shader or a filter not described: painted here)
                    RectF d = new RectF(l, t, r, bo);
                    raster(l, t, r, bo, p, c -> c.drawBitmap(b, src, d, p));
                    return;
                }
                int id = images.id(b);
                if (id == 0) return;
                rec(() -> {
                    int n = p == null ? -1 : paint(p);
                    op(BITMAP);
                    o.writeInt(id);
                    o.writeShort(n);
                    o.writeBoolean(src != null);
                    if (src != null) f(src.left, src.top, src.right, src.bottom);
                    f(l, t, r, bo);
                });
            } finally {
                leave();
            }
        }

        @Override public void drawBitmap(Bitmap b, float l, float t, Paint p) { bitmap(b, null, l, t, l + b.getWidth(), t + b.getHeight(), p); }
        @Override public void drawBitmap(Bitmap b, Rect src, RectF dst, Paint p) { bitmap(b, src, dst.left, dst.top, dst.right, dst.bottom, p); }
        @Override public void drawBitmap(Bitmap b, Rect src, Rect dst, Paint p) { bitmap(b, src, dst.left, dst.top, dst.right, dst.bottom, p); }
        @Override public void drawBitmap(Bitmap b, Matrix mx, Paint p) {
            if (!enter()) { leave(); return; }
            try {
                if (p != null && !plain(p)) {
                    RectF d = new RectF(0, 0, b.getWidth(), b.getHeight());
                    mx.mapRect(d);
                    raster(d.left, d.top, d.right, d.bottom, p, c -> c.drawBitmap(b, mx, p));
                    return;
                }
                int id = images.id(b);
                if (id == 0) return;
                rec(() -> {
                    int n = p == null ? -1 : paint(p);
                    op(BITMAP_MATRIX);
                    o.writeInt(id);
                    o.writeShort(n);
                    matrix(mx);
                });
            } finally {
                leave();
            }
        }

        @Override public void drawPatch(NinePatch patch, Rect dst, Paint p) {
            if (!enter()) { leave(); return; }
            try { raster(dst.left, dst.top, dst.right, dst.bottom, null, c -> patch.draw(c, dst, p)); } finally { leave(); }
        }
        @Override public void drawPatch(NinePatch patch, RectF dst, Paint p) {
            if (!enter()) { leave(); return; }
            try { raster(dst.left, dst.top, dst.right, dst.bottom, null, c -> c.drawPatch(patch, dst, p)); } finally { leave(); }
        }

        private void text(String s, float x, float y, Paint p) {
            if (!enter()) { leave(); return; }
            try {
                if (s.isEmpty()) return;
                rec(() -> { int n = paint(p); op(TEXT); o.writeShort(n); f(x, y); o.writeUTF(s); });
            } finally {
                leave();
            }
        }

        @Override public void drawText(String s, float x, float y, Paint p) { text(s, x, y, p); }
        @Override public void drawText(String s, int start, int end, float x, float y, Paint p) { text(s.substring(start, end), x, y, p); }
        @Override public void drawText(CharSequence s, int start, int end, float x, float y, Paint p) { text(s.subSequence(start, end).toString(), x, y, p); }
        @Override public void drawText(char[] s, int index, int count, float x, float y, Paint p) { text(new String(s, index, count), x, y, p); }

        private void run(String ctx, int start, int end, float x, float y, boolean rtl, Paint p) {
            if (!enter()) { leave(); return; }
            try {
                if (end <= start) return;
                rec(() -> { int n = paint(p); op(TEXT_RUN); o.writeShort(n); f(x, y); o.writeUTF(ctx); o.writeShort(start); o.writeShort(end); o.writeBoolean(rtl); });
            } finally {
                leave();
            }
        }

        @Override public void drawTextRun(char[] s, int index, int count, int ctxIndex, int ctxCount, float x, float y, boolean rtl, Paint p) {
            run(new String(s, ctxIndex, ctxCount), index - ctxIndex, index - ctxIndex + count, x, y, rtl, p);
        }
        @Override public void drawTextRun(CharSequence s, int start, int end, int ctxStart, int ctxEnd, float x, float y, boolean rtl, Paint p) {
            run(s.subSequence(ctxStart, ctxEnd).toString(), start - ctxStart, end - ctxStart, x, y, rtl, p);
        }
        @Override public void drawTextRun(android.graphics.text.MeasuredText t, int start, int end, int ctxStart, int ctxEnd, float x, float y, boolean rtl, Paint p) {
            // (the text itself isn't readable from a MeasuredText: painted here)
            if (!enter()) { leave(); return; }
            try {
                float wd = p.measureText("M") * (end - start) * 2;
                raster(x - wd, y - p.getTextSize() * 2, x + wd, y + p.getTextSize(), null, c -> c.drawTextRun(t, start, end, ctxStart, ctxEnd, x, y, rtl, p));
            } finally {
                leave();
            }
        }
    }

    // ------------------------------------------------------------------ the GamePad device: playing back
    /** the console's dialog drawn from its orders, fit into this view (its own size kept: w x h there) */
    static final class Player extends View {
        private final Map<Integer, Bitmap> bitmaps = new HashMap<>();
        private byte[] orders;  // inflated
        private int w, h;
        private final List<Paint> paints = new ArrayList<>();
        private final Matrix base = new Matrix(), mx = new Matrix();
        private final float[] mv = new float[9];
        private final RectF r1 = new RectF(), r2 = new RectF();
        private final Rect src = new Rect();
        private final Paint bmpPaint = new Paint(Paint.FILTER_BITMAP_FLAG | Paint.ANTI_ALIAS_FLAG);
        private final Paint devPaint = new Paint(Paint.FILTER_BITMAP_FLAG | Paint.ANTI_ALIAS_FLAG);
        private final Path path = new Path();
        int background = 0xFF0C1622;

        Player(android.content.Context c) { super(c); }

        boolean showing() { return orders != null; }

        int dialogW() { return w; }

        int dialogH() { return h; }

        /** a bitmap that came (main thread; UiDraw.decode) */
        void put(int id, Bitmap b) {
            if (b == null) return;
            bitmaps.put(id, b);
            invalidate();
        }

        /** new orders (deflated), or none (w 0): main thread */
        void show(int w, int h, byte[] deflated) {
            if (w <= 0 || deflated == null) {
                orders = null;
                setVisibility(GONE);
                return;
            }
            try {
                java.util.zip.Inflater inf = new java.util.zip.Inflater();
                inf.setInput(deflated);
                ByteArrayOutputStream out = new ByteArrayOutputStream(deflated.length * 4);
                byte[] buf = new byte[1 << 15];
                while (!inf.finished()) {
                    int n = inf.inflate(buf);
                    if (n == 0 && (inf.needsInput() || inf.needsDictionary())) break;
                    out.write(buf, 0, n);
                }
                inf.end();
                orders = out.toByteArray();
            } catch (java.util.zip.DataFormatException e) {
                return;
            }
            this.w = w;
            this.h = h;
            setVisibility(VISIBLE);
            invalidate();
        }

        /** a connection's end: its bitmaps go */
        void clear() {
            bitmaps.clear();
            orders = null;
            setVisibility(GONE);
        }

        /** the dialog's place here: scale, x, y */
        float[] fit() {
            float vw = getWidth(), vh = getHeight();
            if (w <= 0 || h <= 0) return new float[] {1, 0, 0};
            float k = Math.min(vw / w, vh / h);
            return new float[] {k, (vw - w * k) / 2, (vh - h * k) / 2};
        }

        @Override
        protected void onDraw(Canvas c) {
            c.drawColor(background);
            byte[] ord = orders;
            if (ord == null) return;
            float[] ft = fit();
            base.setScale(ft[0], ft[0]);
            base.postTranslate(ft[1], ft[2]);
            int start = c.save();
            c.concat(base);
            // the console counted its saves from 1 (its canvas's own): that depth here is start + 1
            int shift = start;
            paints.clear();
            try {
                play(c, new DataInputStream(new ByteArrayInputStream(ord)), shift);
            } catch (IOException | RuntimeException e) {
                android.util.Log.w("WWHD", "GamePad: dialog drawing broke: " + e);
            }
            c.restoreToCount(start);
        }

        private float rf(DataInputStream in) throws IOException { return in.readFloat(); }

        private Paint paintAt(int n) { return n >= 0 && n < paints.size() ? paints.get(n) : null; }

        private void play(Canvas c, DataInputStream in, int shift) throws IOException {
            for (;;) {
                int op = in.readUnsignedByte();
                switch (op) {
                    case END: return;
                    case SAVE: c.save(); break;
                    case SAVE_LAYER: {
                        float l = rf(in), t = rf(in), r = rf(in), b = rf(in);
                        int a = in.readUnsignedByte();
                        c.saveLayerAlpha(l, t, r, b, a);
                        break;
                    }
                    case RESTORE_TO: {
                        int n = in.readShort() + shift;
                        if (n > shift && n < c.getSaveCount()) c.restoreToCount(n);
                        break;
                    }
                    case TRANSLATE: c.translate(rf(in), rf(in)); break;
                    case SCALE: c.scale(rf(in), rf(in)); break;
                    case ROTATE: c.rotate(rf(in)); break;
                    case SKEW: c.skew(rf(in), rf(in)); break;
                    case CONCAT: readMatrix(in); c.concat(mx); break;
                    case SET_MATRIX: readMatrix(in); mx.postConcat(base); c.setMatrix(mx); break;
                    case CLIP_RECT: {
                        float l = rf(in), t = rf(in), r = rf(in), b = rf(in);
                        if (in.readBoolean()) c.clipOutRect(l, t, r, b);
                        else c.clipRect(l, t, r, b);
                        break;
                    }
                    case CLIP_PATH: {
                        readPath(in);
                        if (in.readBoolean()) c.clipOutPath(path);
                        else c.clipPath(path);
                        break;
                    }
                    case PAINT: {
                        int len = in.readUnsignedShort();
                        byte[] pb = new byte[len];
                        in.readFully(pb);
                        paints.add(readPaint(new DataInputStream(new ByteArrayInputStream(pb))));
                        break;
                    }
                    case COLOR: {
                        int color = in.readInt();
                        int mode = in.readByte();
                        BlendMode[] all = BlendMode.values();
                        c.drawColor(color, mode >= 0 && mode < all.length ? all[mode] : BlendMode.SRC_OVER);
                        break;
                    }
                    case DRAW_PAINT: { Paint p = paintAt(in.readShort()); if (p != null) c.drawPaint(p); break; }
                    case RECT: case RRECT: case OVAL: case CIRCLE: case ARC: case DRRECT: {
                        Paint p = paintAt(in.readShort());
                        float l = rf(in), t = rf(in), r = rf(in), b = rf(in);
                        if (op == RRECT) { float rx = rf(in), ry = rf(in); if (p != null) c.drawRoundRect(l, t, r, b, rx, ry, p); }
                        else if (op == ARC) { float s = rf(in), sw = rf(in); boolean ctr = in.readBoolean(); if (p != null) c.drawArc(l, t, r, b, s, sw, ctr, p); }
                        else if (op == DRRECT) {
                            float ox = rf(in), oy = rf(in);
                            r2.set(rf(in), rf(in), rf(in), rf(in));
                            float ix = rf(in), iy = rf(in);
                            r1.set(l, t, r, b);
                            if (p != null) c.drawDoubleRoundRect(r1, ox, oy, r2, ix, iy, p);
                        } else if (p != null) {
                            if (op == RECT) c.drawRect(l, t, r, b, p);
                            else if (op == OVAL) c.drawOval(l, t, r, b, p);
                            else c.drawCircle((l + r) / 2, (t + b) / 2, (r - l) / 2, p);
                        }
                        break;
                    }
                    case LINES: case POINTS: {
                        Paint p = paintAt(in.readShort());
                        int n = in.readInt();
                        float[] pts = new float[n];
                        for (int i = 0; i < n; i++) pts[i] = rf(in);
                        if (p != null) {
                            if (op == LINES) c.drawLines(pts, p);
                            else c.drawPoints(pts, p);
                        }
                        break;
                    }
                    case PATH: {
                        Paint p = paintAt(in.readShort());
                        readPath(in);
                        if (p != null) c.drawPath(path, p);
                        break;
                    }
                    case BITMAP: {
                        Bitmap b = bitmaps.get(in.readInt());
                        Paint p = paintAt(in.readShort());
                        boolean hasSrc = in.readBoolean();
                        if (hasSrc) src.set(Math.round(rf(in)), Math.round(rf(in)), Math.round(rf(in)), Math.round(rf(in)));
                        r1.set(rf(in), rf(in), rf(in), rf(in));
                        if (b != null) c.drawBitmap(b, hasSrc ? src : null, r1, bitmapPaint(p));
                        break;
                    }
                    case BITMAP_MATRIX: {
                        Bitmap b = bitmaps.get(in.readInt());
                        Paint p = paintAt(in.readShort());
                        readMatrix(in);
                        if (b != null) c.drawBitmap(b, mx, bitmapPaint(p));
                        break;
                    }
                    case DEVICE_IMAGE: {
                        Bitmap b = bitmaps.get(in.readInt());
                        r1.set(rf(in), rf(in), rf(in), rf(in));
                        int bm = in.readByte();
                        BlendMode[] modes = BlendMode.values();
                        devPaint.setBlendMode(bm >= 0 && bm < modes.length ? modes[bm] : null);
                        if (b != null) {
                            int s = c.save();
                            c.setMatrix(base);
                            c.drawBitmap(b, null, r1, devPaint);
                            c.restoreToCount(s);
                        }
                        break;
                    }
                    case TEXT: {
                        Paint p = paintAt(in.readShort());
                        float x = rf(in), y = rf(in);
                        String s = in.readUTF();
                        if (p != null) c.drawText(s, x, y, p);
                        break;
                    }
                    case TEXT_RUN: {
                        Paint p = paintAt(in.readShort());
                        float x = rf(in), y = rf(in);
                        String s = in.readUTF();
                        int st = in.readShort(), en = in.readShort();
                        boolean rtl = in.readBoolean();
                        if (p != null && st >= 0 && en <= s.length() && st < en) c.drawTextRun(s, st, en, 0, s.length(), x, y, rtl, p);
                        break;
                    }
                    default:
                        throw new IOException("unknown order " + op);
                }
            }
        }

        private Paint bitmapPaint(Paint p) {
            if (p == null) return bmpPaint;
            p.setFilterBitmap(true);
            return p;
        }

        private void readMatrix(DataInputStream in) throws IOException {
            for (int i = 0; i < 9; i++) mv[i] = in.readFloat();
            mx.setValues(mv);
        }

        private void readPath(DataInputStream in) throws IOException {
            path.reset();
            int ft = in.readByte();
            Path.FillType[] fts = Path.FillType.values();
            path.setFillType(ft >= 0 && ft < fts.length ? fts[ft] : Path.FillType.WINDING);
            int n = in.readInt();
            for (int i = 0; i < n; i++) {
                boolean jump = in.readBoolean();
                float x = in.readFloat(), y = in.readFloat();
                if (jump) path.moveTo(x, y);
                else path.lineTo(x, y);
            }
        }

        private static Paint readPaint(DataInputStream in) throws IOException {
            Paint p = new Paint();
            p.setFlags(in.readInt());
            p.setColor(in.readInt());
            p.setStyle(Paint.Style.values()[in.readByte()]);
            p.setStrokeWidth(in.readFloat());
            p.setStrokeCap(Paint.Cap.values()[in.readByte()]);
            p.setStrokeJoin(Paint.Join.values()[in.readByte()]);
            p.setStrokeMiter(in.readFloat());
            p.setTextSize(in.readFloat());
            p.setTextScaleX(in.readFloat());
            p.setTextSkewX(in.readFloat());
            p.setLetterSpacing(in.readFloat());
            p.setTextAlign(Paint.Align.values()[in.readByte()]);
            int face = in.readShort();
            if (face >= 0) {
                Typeface[] all = faces();
                p.setTypeface(face < all.length ? all[face] : Typeface.DEFAULT);
            } else {
                int weight = in.readShort();
                boolean italic = in.readBoolean();
                p.setTypeface(Typeface.create(Typeface.DEFAULT, Math.max(1, Math.min(1000, weight)), italic));
            }
            float sr = in.readFloat();
            if (sr > 0) p.setShadowLayer(sr, in.readFloat(), in.readFloat(), in.readInt());
            int bm = in.readByte();
            BlendMode[] modes = BlendMode.values();
            if (bm >= 0 && bm < modes.length) p.setBlendMode(modes[bm]);
            switch (in.readByte()) {
                case 1: {
                    float[] v = new float[20];
                    for (int i = 0; i < 20; i++) v[i] = in.readFloat();
                    p.setColorFilter(new ColorMatrixColorFilter(v));
                    break;
                }
                case 2: {
                    int color = in.readInt();
                    int mode = in.readByte();
                    p.setColorFilter(new BlendModeColorFilter(color, mode >= 0 && mode < modes.length ? modes[mode] : BlendMode.SRC_IN));
                    break;
                }
                case 3:
                    p.setColorFilter(new LightingColorFilter(in.readInt(), in.readInt()));
                    break;
                default:
                    break;
            }
            return p;
        }
    }
}
