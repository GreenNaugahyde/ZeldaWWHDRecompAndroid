package org.wwhdrecomp.app;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.ColorFilter;
import android.graphics.LinearGradient;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.PixelFormat;
import android.graphics.RectF;
import android.graphics.Shader;
import android.graphics.Typeface;
import android.graphics.drawable.Drawable;
import android.graphics.drawable.GradientDrawable;
import android.graphics.drawable.StateListDrawable;
import android.text.Layout;
import android.text.TextPaint;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.widget.TextView;

import java.util.Random;

/**
 * The look of the in-game menu (OptionsMenu) and its dialogs (GameDialog), after the game's pause
 * menus and drawn in code: parchment with a torn edge, cyan title plates with outlined lettering,
 * light-blue bar buttons, slots, ON/OFF switches and the yellow highlight.
 */
final class GameUi {
    private GameUi() {}

    static final int SLATE = 0xEE3D5E7C, PAPER = 0xFFEDEAD7, PAPER_EDGE = 0xFFD8D1B4, FRINGE = 0xFFA9DDF0;
    static final int CYAN = 0xFF2FC0F2, CYAN_DARK = 0xFF1C82B6, INK = 0xFF2E3D47, HINT = 0xFF6B7780;
    static final int YELLOW = 0xFFF2D21C, GLOW = 0xFFFFF4AE;

    /** A plate colour: gradient top and bottom, and the dark outline of its lettering. */
    static final class Palette {
        final int light, mid, dark;

        Palette(int light, int mid, int dark) {
            this.light = light;
            this.mid = mid;
            this.dark = dark;
        }

        // the muted version (an unselected tab): mixed with a grey-blue
        Palette muted() { return new Palette(mix(light, 0xFFB4C6CE, 0.6f), mix(mid, 0xFF8FA9B5, 0.6f), mix(dark, 0xFF5E7884, 0.6f)); }

        static int mix(int a, int b, float t) {
            int r = Math.round(((a >> 16) & 0xFF) * (1 - t) + ((b >> 16) & 0xFF) * t);
            int g = Math.round(((a >> 8) & 0xFF) * (1 - t) + ((b >> 8) & 0xFF) * t);
            int bl = Math.round((a & 0xFF) * (1 - t) + (b & 0xFF) * t);
            return 0xFF000000 | (r << 16) | (g << 8) | bl;
        }
    }

    // the game's menu colours: its green cards, the blue buttons, plus amber and coral for variety
    static final Palette GREEN = new Palette(0xFFB4EA86, 0xFF5DBA42, 0xFF2F7A22);
    static final Palette BLUE = new Palette(0xFF6FD8FA, CYAN, CYAN_DARK);
    static final Palette AMBER = new Palette(0xFFFFDA82, 0xFFF0A630, 0xFF9E5A0C);
    static final Palette CORAL = new Palette(0xFFFFA796, 0xFFE5604B, 0xFF94291C);
    static final Palette VIOLET = new Palette(0xFFD7BBFF, 0xFFA06AE0, 0xFF5A2D91);

    static int px(Context c, float v) { return Math.round(v * c.getResources().getDisplayMetrics().density); }

    /** a text view: in the game's text font (GameText) or the system's */
    static TextView text(Context c, boolean game) { return game ? new GameText(c) : new TextView(c); }

    // the game's light-blue bar buttons
    static TextView barButton(Context c, String text, View.OnClickListener click) { return barButton(c, text, click, false); }

    // green: the confirming button of a dialog
    static TextView barButton(Context c, String text, View.OnClickListener click, boolean green) { return barButton(c, text, click, green, false); }

    static TextView barButton(Context c, String text, View.OnClickListener click, boolean green, boolean game) {
        if (game) {  // the game's Options button: green confirms, the others grey
            TextView b = text(c, true);
            b.setText(text);
            b.setGravity(Gravity.CENTER);
            b.setTextColor(green ? OPTION_BUTTON_TEXT : 0xFF3A3A32);
            b.setTextSize(TypedValue.COMPLEX_UNIT_SP, 18);
            b.setPadding(px(c, 18), 0, px(c, 22), px(c, 4));
            b.setFocusable(true);
            b.setOnClickListener(click);
            b.setBackground(states(new GameValueButton(c, green, false), new GameValueButton(c, green, true)));
            return b;
        }
        TextView b = text(c, game);
        b.setText(text);
        b.setGravity(Gravity.CENTER);
        b.setTextColor(0xFF203A49);
        b.setTextSize(TypedValue.COMPLEX_UNIT_SP, 18);
        b.setTypeface(Typeface.create("sans-serif-medium", Typeface.NORMAL));
        b.setPadding(px(c, 16), 0, px(c, 16), 0);
        b.setFocusable(true);
        b.setOnClickListener(click);
        b.setBackground(states(green ? greenBar(c) : bar(c, false), bar(c, true)));
        return b;
    }

    private static Drawable bar(Context c, boolean lit) {
        GradientDrawable g = new GradientDrawable(GradientDrawable.Orientation.TOP_BOTTOM,
                lit ? new int[] {0xFFFFF7B8, 0xFFF5DC4A} : new int[] {0xFFA6E4FB, 0xFF4AB6EC});
        g.setCornerRadius(px(c, 6));
        g.setStroke(px(c, 3), lit ? 0xFFC9A200 : 0xFF2A86BD);
        return g;
    }

    private static Drawable greenBar(Context c) {
        GradientDrawable g = new GradientDrawable(GradientDrawable.Orientation.TOP_BOTTOM, new int[] {0xFFC4EE9C, 0xFF62BE45});
        g.setCornerRadius(px(c, 6));
        g.setStroke(px(c, 3), 0xFF3A8A2A);
        return g;
    }

    static Drawable slot(Context c, int fill, int stroke) {
        GradientDrawable g = new GradientDrawable();
        g.setCornerRadius(px(c, 14));
        g.setColor(fill);
        if (stroke != 0) g.setStroke(px(c, 3), stroke);
        return g;
    }

    // a setting's row: a slot that lights up yellow when pressed or selected with a controller
    static Drawable slotStates(Context c) { return states(slot(c, 0x1F000000, 0), slot(c, GLOW, YELLOW)); }

    // pressed or focused (touch or controller): the game's yellow highlight
    static StateListDrawable states(Drawable normal, Drawable lit) {
        StateListDrawable s = new StateListDrawable();
        s.addState(new int[] {android.R.attr.state_pressed}, lit);
        s.addState(new int[] {android.R.attr.state_focused}, lit);
        s.addState(new int[] {}, normal);
        return s;
    }

    // an ON / OFF switch
    static TextView pill(Context c, boolean on) { return pill(c, on, false); }

    static TextView pill(Context c, boolean on, boolean game) {
        if (game) {  // the game's Options button, grey when off
            TextView b = text(c, true);
            b.setText(on ? R.string.opt_on : R.string.opt_off);
            b.setGravity(Gravity.CENTER);
            b.setTextSize(16);
            b.setTextColor(on ? OPTION_BUTTON_TEXT : 0xFF45453C);
            b.setPadding(0, 0, px(c, 4), px(c, 4));
            b.setBackground(new GameValueButton(c, on, false));
            return b;
        }
        TextView pill = text(c, game);
        pill.setText(on ? R.string.opt_on : R.string.opt_off);
        pill.setGravity(Gravity.CENTER);
        pill.setTextSize(16);
        pill.setTypeface(Typeface.create("sans-serif-black", Typeface.NORMAL));
        pill.setTextColor(on ? Color.WHITE : 0xFF6B6658);
        GradientDrawable g = new GradientDrawable();
        g.setCornerRadius(px(c, 18));
        g.setColor(on ? GREEN.mid : 0xFFCBC8B6);
        g.setStroke(px(c, 2), on ? GREEN.dark : 0xFFA9A595);
        pill.setBackground(g);
        return pill;
    }

    /** A panel: parchment with a rough, torn edge and a light-blue fringe behind it. */
    static final class Parchment extends Drawable {
        private final Paint p = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Context c;

        Parchment(Context c) { this.c = c; }

        private Path edge(RectF r, float jitter, long seed) {
            Random rnd = new Random(seed);
            Path path = new Path();
            float step = px(c, 16);
            path.moveTo(r.left, r.top);
            for (float x = r.left + step; x < r.right; x += step) path.lineTo(x, r.top + (rnd.nextFloat() - 0.5f) * jitter);
            path.lineTo(r.right, r.top);
            for (float y = r.top + step; y < r.bottom; y += step) path.lineTo(r.right + (rnd.nextFloat() - 0.5f) * jitter, y);
            path.lineTo(r.right, r.bottom);
            for (float x = r.right - step; x > r.left; x -= step) path.lineTo(x, r.bottom + (rnd.nextFloat() - 0.5f) * jitter);
            path.lineTo(r.left, r.bottom);
            for (float y = r.bottom - step; y > r.top; y -= step) path.lineTo(r.left + (rnd.nextFloat() - 0.5f) * jitter, y);
            path.close();
            return path;
        }

        @Override
        public void draw(Canvas canvas) {
            RectF b = new RectF(getBounds());
            RectF outer = new RectF(b), inner = new RectF(b);
            inner.inset(px(c, 7), px(c, 7));
            p.setStyle(Paint.Style.FILL);
            p.setColor(FRINGE);
            canvas.drawPath(edge(outer, px(c, 6), 1), p);
            p.setShader(new LinearGradient(0, inner.top, 0, inner.bottom, PAPER, 0xFFE4E0CA, Shader.TileMode.CLAMP));
            canvas.drawPath(edge(inner, px(c, 5), 2), p);
            p.setShader(null);
            p.setStyle(Paint.Style.STROKE);
            p.setStrokeWidth(px(c, 1.5f));
            p.setColor(PAPER_EDGE);
            canvas.drawPath(edge(inner, px(c, 5), 2), p);
        }

        @Override public void setAlpha(int alpha) {}
        @Override public void setColorFilter(ColorFilter cf) {}
        @Override public int getOpacity() { return PixelFormat.TRANSLUCENT; }
    }

    /** A dialog's frame: wooden boat planks with nails around a sheet of paper. */
    static final class WoodFrame extends Drawable {
        static final float THICK_DP = 20;
        private static final int[] WOODS = {0xFFAE7140, 0xFFB97C49, 0xFFA2663A, 0xFFB37645};
        private final Context c;
        private final Paint p = new Paint(Paint.ANTI_ALIAS_FLAG);

        WoodFrame(Context c) { this.c = c; }

        // one side: planks of varying length along it (horizontal or vertical), seams, grain, nails
        private void side(Canvas cv, RectF r, boolean horizontal, long seed) {
            Random rnd = new Random(seed);
            float len = horizontal ? r.width() : r.height(), t = horizontal ? r.height() : r.width();
            float pos = -rnd.nextFloat() * px(c, 120);  // the first seam somewhere along the side
            while (pos < len) {
                float plank = px(c, 150) + rnd.nextFloat() * px(c, 130);
                float a = Math.max(0, pos), b = Math.min(len, pos + plank);
                RectF pr = horizontal ? new RectF(r.left + a, r.top, r.left + b, r.bottom) : new RectF(r.left, r.top + a, r.right, r.top + b);
                int base = WOODS[rnd.nextInt(WOODS.length)];
                // light edge on the outer side, shadow towards the paper (opaque: the paint's alpha
                // also applies to a gradient, and the grain and nails leave it translucent)
                p.setStyle(Paint.Style.FILL);
                p.setColor(0xFFFFFFFF);
                p.setShader(horizontal
                        ? new LinearGradient(0, pr.top, 0, pr.bottom, lighten(base, 1.18f), lighten(base, 0.82f), Shader.TileMode.CLAMP)
                        : new LinearGradient(pr.left, 0, pr.right, 0, lighten(base, 1.18f), lighten(base, 0.82f), Shader.TileMode.CLAMP));
                cv.drawRect(pr, p);
                p.setShader(null);
                // grain: a few thin wavy lines along the plank
                p.setStyle(Paint.Style.STROKE);
                p.setStrokeWidth(px(c, 1));
                p.setColor(0x665A3418);
                for (int g = 0; g < 3; g++) {
                    float off = t * (0.25f + 0.25f * g) + (rnd.nextFloat() - 0.5f) * px(c, 2);
                    float phase = rnd.nextFloat() * 6f, amp = px(c, 0.6f + rnd.nextFloat());
                    Path path = new Path();
                    for (float u = a; u <= b; u += px(c, 6)) {
                        float v = off + (float) Math.sin(u / px(c, 30) + phase) * amp;
                        float x = horizontal ? r.left + u : r.left + v, y = horizontal ? r.top + v : r.top + u;
                        if (u == a) path.moveTo(x, y);
                        else path.lineTo(x, y);
                    }
                    cv.drawPath(path, p);
                }
                // the seam to the next plank
                if (b < len) {
                    p.setStrokeWidth(px(c, 2));
                    p.setColor(0xFF4A2E17);
                    if (horizontal) cv.drawLine(r.left + b, r.top, r.left + b, r.bottom, p);
                    else cv.drawLine(r.left, r.top + b, r.right, r.top + b, p);
                }
                // a nail near each end of the plank
                for (float n : new float[] {a + px(c, 9), b - px(c, 9)}) {
                    if (n - a < px(c, 6) || b - n < px(c, 6)) continue;
                    float x = horizontal ? r.left + n : r.centerX(), y = horizontal ? r.centerY() : r.top + n;
                    p.setStyle(Paint.Style.FILL);
                    p.setColor(0xFF3E2A1C);
                    cv.drawCircle(x, y, px(c, 2.6f), p);
                    p.setColor(0x99D8C0A0);
                    cv.drawCircle(x - px(c, 0.8f), y - px(c, 0.8f), px(c, 0.9f), p);
                }
                pos += plank;
            }
        }

        private static int lighten(int color, float f) {
            int r = Math.min(255, Math.round(((color >> 16) & 0xFF) * f));
            int g = Math.min(255, Math.round(((color >> 8) & 0xFF) * f));
            int b = Math.min(255, Math.round((color & 0xFF) * f));
            return 0xFF000000 | (r << 16) | (g << 8) | b;
        }

        @Override
        public void draw(Canvas cv) {
            RectF b = new RectF(getBounds());
            float t = px(c, THICK_DP);
            // paper inside
            RectF paper = new RectF(b.left + t - 1, b.top + t - 1, b.right - t + 1, b.bottom - t + 1);
            p.setStyle(Paint.Style.FILL);
            p.setColor(0xFFFFFFFF);
            p.setShader(new LinearGradient(0, paper.top, 0, paper.bottom, PAPER, 0xFFE4E0CA, Shader.TileMode.CLAMP));
            cv.drawRect(paper, p);
            p.setShader(null);
            // planks: top and bottom run the full width, the sides fit between them
            side(cv, new RectF(b.left, b.top, b.right, b.top + t), true, 11);
            side(cv, new RectF(b.left, b.bottom - t, b.right, b.bottom), true, 12);
            side(cv, new RectF(b.left, b.top + t, b.left + t, b.bottom - t), false, 13);
            side(cv, new RectF(b.right - t, b.top + t, b.right, b.bottom - t), false, 14);
            // joints at the corners, a shadow onto the paper, and the outline
            p.setStyle(Paint.Style.STROKE);
            p.setStrokeWidth(px(c, 2));
            p.setColor(0xFF4A2E17);
            cv.drawLine(b.left, b.top + t, b.left + t, b.top + t, p);
            cv.drawLine(b.right - t, b.top + t, b.right, b.top + t, p);
            cv.drawLine(b.left, b.bottom - t, b.left + t, b.bottom - t, p);
            cv.drawLine(b.right - t, b.bottom - t, b.right, b.bottom - t, p);
            p.setColor(0x553A2412);
            p.setStrokeWidth(px(c, 3));
            cv.drawRect(paper, p);
            p.setColor(0xFF3A2412);
            p.setStrokeWidth(px(c, 2.5f));
            RectF outer = new RectF(b);
            outer.inset(px(c, 1.25f), px(c, 1.25f));
            cv.drawRoundRect(outer, px(c, 4), px(c, 4), p);
        }

        @Override public void setAlpha(int alpha) {}
        @Override public void setColorFilter(ColorFilter cf) {}
        @Override public int getOpacity() { return PixelFormat.TRANSLUCENT; }
    }

    /** A title or tab: a slanted cyan plate (muted when not selected). */
    static final class TabPlate extends Drawable {
        private final boolean on;
        private final Context c;
        private final Palette pal;
        private final Paint p = new Paint(Paint.ANTI_ALIAS_FLAG);
        // > 0: the game tabs' scratch pattern (ScratchPattern_00, repeated) over the chosen plate, at this many pixels per texel
        float scratch;

        TabPlate(Context c, boolean on) { this(c, on, BLUE); }

        TabPlate(Context c, boolean on, Palette pal) {
            this.c = c;
            this.on = on;
            this.pal = on ? pal : pal.muted();
        }

        @Override
        public void draw(Canvas canvas) {
            RectF b = new RectF(getBounds());
            float s = px(c, 10);
            Path path = new Path();
            path.moveTo(b.left + s, b.top);
            path.lineTo(b.right, b.top);
            path.lineTo(b.right - s, b.bottom);
            path.lineTo(b.left, b.bottom);
            path.close();
            p.setStyle(Paint.Style.FILL);
            p.setColor(0xFFFFFFFF);  // opaque (the outline below leaves the paint translucent)
            p.setShader(new LinearGradient(0, b.top, 0, b.bottom, pal.light, pal.mid, Shader.TileMode.CLAMP));
            canvas.drawPath(path, p);
            p.setShader(null);
            android.graphics.Bitmap sc = scratch > 0 && on ? GameIcons.get(c, "ScratchPattern_00^t", () -> new android.os.Handler(android.os.Looper.getMainLooper()).post(this::invalidateSelf)) : null;
            if (sc != null) {  // light scratches on the chosen tab, as the game's
                android.graphics.BitmapShader bs = new android.graphics.BitmapShader(sc, Shader.TileMode.REPEAT, Shader.TileMode.REPEAT);
                android.graphics.Matrix m = new android.graphics.Matrix();
                m.setScale(scratch, scratch);
                m.postTranslate(b.left, b.top);
                bs.setLocalMatrix(m);
                Paint sp = new Paint(Paint.ANTI_ALIAS_FLAG | Paint.FILTER_BITMAP_FLAG);
                sp.setShader(bs);
                float k = 0.5f;
                // the pattern is in its luminance or (an alpha texture: white throughout) in its alpha
                boolean inAlpha = (sc.getPixel(0, 0) & 0xFFFFFF) == 0xFFFFFF && (sc.getPixel(sc.getWidth() / 2, sc.getHeight() / 3) & 0xFFFFFF) == 0xFFFFFF;
                sp.setColorFilter(new android.graphics.ColorMatrixColorFilter(new float[] {
                        0, 0, 0, 0, 255,
                        0, 0, 0, 0, 255,
                        0, 0, 0, 0, 255,
                        inAlpha ? 0 : k, 0, 0, inAlpha ? k : 0, 0}));
                canvas.drawPath(path, sp);
            }
            p.setStyle(Paint.Style.STROKE);
            p.setStrokeWidth(px(c, 3));
            p.setColor(on ? 0xFFFFFFFF : 0xCCFFFFFF);
            canvas.drawPath(path, p);
        }

        @Override public void setAlpha(int alpha) {}
        @Override public void setColorFilter(ColorFilter cf) {}
        @Override public int getOpacity() { return PixelFormat.TRANSLUCENT; }
    }

    /** White title lettering with a dark outline, as on the game's menu plates. */
    /**
     * The game's page arrow (BtnMenuArrow_00 of the GamePad menus: its shadow and base, tinted as the layout's
     * materials do), from the player's game files (GameIcons); a plain yellow arrow until they are built.
     * Laid out in the arrow part's 80 x 148 layout units; left points it to the left.
     */
    static final class MenuArrow extends Drawable {
        private final Context c;
        private final boolean left;
        private final Paint base = new Paint(Paint.FILTER_BITMAP_FLAG | Paint.ANTI_ALIAS_FLAG), shadow = new Paint(Paint.FILTER_BITMAP_FLAG);
        private final Paint spec = new Paint(Paint.FILTER_BITMAP_FLAG | Paint.ANTI_ALIAS_FLAG);
        private final Paint plain = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final RectF dst = new RectF();
        private final Path tri = new Path();
        static final boolean TEXTURE_LEFT = false;  // which way ArrowBase_00 points as stored

        MenuArrow(Context c, boolean left) {
            this.c = c;
            this.left = left;
            // P_Btn_00 is black d3d300 .. white e8e8e8 over the luminance, but the game's Valid animation and
            // second (spec) texture light it up: measured on the game's right arrow, ebeb1b .. f7f7ff
            base.setColorFilter(new android.graphics.ColorMatrixColorFilter(new float[] {
                    (0xF7 - 0xEB) / 255f, 0, 0, 0, 0xEB,
                    0, (0xF7 - 0xEB) / 255f, 0, 0, 0xEB,
                    0, 0, (0xFF - 0x1B) / 255f, 0, 0x1B,
                    0, 0, 0, 1, 0}));
            // the gloss (ArrowSpec_00): white by its luminance, only over the arrow itself
            spec.setColorFilter(new android.graphics.ColorMatrixColorFilter(new float[] {
                    0, 0, 0, 0, 255,
                    0, 0, 0, 0, 255,
                    0, 0, 0, 0, 255,
                    0.6f, 0, 0, 0, 0}));
            spec.setXfermode(new android.graphics.PorterDuffXfermode(android.graphics.PorterDuff.Mode.SRC_ATOP));
            shadow.setColorFilter(new android.graphics.ColorMatrixColorFilter(new float[] {
                    (255 - 0x1E) / 255f, 0, 0, 0, 0x1E,
                    0, (255 - 0x1E) / 255f, 0, 0, 0x1E,
                    0, 0, (255 - 0x1E) / 255f, 0, 0x1E,
                    0, 0, 0, 1, 0}));
        }

        @Override
        public void draw(Canvas canvas) {
            android.graphics.Rect b = getBounds();
            float k = Math.min(b.width() / 80f, b.height() / 148f), cx = b.exactCenterX(), cy = b.exactCenterY();
            android.graphics.Bitmap bb = GameIcons.get(c, "ArrowBase_00^t", () -> new android.os.Handler(android.os.Looper.getMainLooper()).post(this::invalidateSelf)), sb = GameIcons.get(c, "ArrowShadow_00^t", null);
            canvas.save();
            if (left != TEXTURE_LEFT) canvas.scale(-1, 1, cx, cy);
            if (bb == null) {
                tri.reset();  // (pointing the texture's way)
                float s = TEXTURE_LEFT ? -1 : 1;
                tri.moveTo(cx - s * 26 * k, cy - 52 * k);
                tri.lineTo(cx + s * 30 * k, cy);
                tri.lineTo(cx - s * 26 * k, cy + 52 * k);
                tri.close();
                plain.setStyle(Paint.Style.FILL);
                plain.setColor(0xFFE8D300);
                canvas.drawPath(tri, plain);
                plain.setStyle(Paint.Style.STROKE);
                plain.setStrokeWidth(5 * k);
                plain.setStrokeJoin(Paint.Join.ROUND);
                plain.setColor(0xC0302010);
                canvas.drawPath(tri, plain);
            } else {
                if (sb != null) {
                    dst.set(cx + (2 - 39) * k, cy + (12 - 67) * k, cx + (2 + 39) * k, cy + (12 + 67) * k);
                    canvas.drawBitmap(sb, null, dst, shadow);
                }
                dst.set(cx + (-1 - 35) * k, cy - 62 * k, cx + (-1 + 35) * k, cy + 62 * k);
                android.graphics.Bitmap gb = GameIcons.get(c, "ArrowSpec_00^t", null);
                int layer = gb == null ? -1 : canvas.saveLayer(dst, null);
                canvas.drawBitmap(bb, null, dst, base);
                if (gb != null) {
                    canvas.drawBitmap(gb, null, dst, spec);
                    canvas.restoreToCount(layer);
                }
            }
            canvas.restore();
        }

        @Override public void setAlpha(int a) { base.setAlpha(a); shadow.setAlpha(a); spec.setAlpha(a); plain.setAlpha(a); }
        @Override public void setColorFilter(ColorFilter f) {}
        @Override public int getOpacity() { return PixelFormat.TRANSLUCENT; }
    }

    /**
     * A tab plate as the game's GamePad menus draw theirs (BtnMenu_00, from the player's game files via GameIcons):
     * the torn paper plate (MenuBtnBase_00, its rim in the tab's dark colour, inside its middle or, chosen, its light
     * colour), the white paper edge and drop shadow (MenuBtnShadow_00), the gloss (MenuBtnSpec_00); the chosen one
     * also the scratches (ScratchPattern_00) and a soft light glow. The plate fills the bounds less a margin for the
     * edge, shadow and glow. Until the textures are built: TabPlate.
     */
    static final class GameTabPlate extends Drawable {
        private final Context c;
        private final boolean on;
        private final Palette pal;
        private final float scratch;  // pixels per scratch texel
        private final TabPlate plain;
        private final Paint base = new Paint(Paint.FILTER_BITMAP_FLAG | Paint.ANTI_ALIAS_FLAG), shadow = new Paint(Paint.FILTER_BITMAP_FLAG);
        private final Paint glow = new Paint(Paint.FILTER_BITMAP_FLAG), spec = new Paint(Paint.FILTER_BITMAP_FLAG);
        private final RectF r = new RectF();

        GameTabPlate(Context c, boolean on, Palette pal, float scratch) {
            this.c = c;
            this.on = on;
            this.pal = pal;
            this.scratch = scratch;
            plain = new TabPlate(c, on, pal);
            int dark = pal.dark, in = on ? Palette.mix(pal.light, 0xFFFFFFFF, 0.25f) : pal.mid;  // (the chosen one lit up)
            base.setColorFilter(lerp(dark, in));
            shadow.setColorFilter(lerp(0xFF1E1E1E, 0xFFE8E8E8));
            glow.setColorFilter(new android.graphics.ColorMatrixColorFilter(new float[] {
                    0, 0, 0, 0, 255, 0, 0, 0, 0, 255, 0, 0, 0, 0, 255, 0, 0, 0, 0.32f, 0}));
        }

        static android.graphics.ColorMatrixColorFilter lerpFilter(int a, int b) { return lerp(a, b); }

        // the texture's luminance from colour a (black) to b (white), its alpha kept: as the layout's materials
        private static android.graphics.ColorMatrixColorFilter lerp(int a, int b) {
            float[] m = new float[20];
            for (int i = 0; i < 3; i++) {
                int sh = 16 - 8 * i, ca = (a >> sh) & 0xFF, cb = (b >> sh) & 0xFF;
                m[i * 5 + i] = (cb - ca) / 255f;
                m[i * 5 + 4] = ca;
            }
            m[18] = 1;
            return new android.graphics.ColorMatrixColorFilter(m);
        }

        @Override
        public void draw(Canvas canvas) {
            Runnable again = () -> new android.os.Handler(android.os.Looper.getMainLooper()).post(this::invalidateSelf);
            android.graphics.Bitmap bb = GameIcons.get(c, "MenuBtnBase_00^t", again), sb = GameIcons.get(c, "MenuBtnShadow_00^t", null);
            android.graphics.Bitmap gb = GameIcons.get(c, "MenuBtnSpec_00^t", null), sc = on ? GameIcons.get(c, "ScratchPattern_00^t", null) : null;
            if (bb == null) {
                plain.setBounds(getBounds());
                plain.draw(canvas);
                return;
            }
            android.graphics.Rect b = getBounds();
            float m = b.height() * 0.07f;
            r.set(b.left + m, b.top, b.right - m, b.bottom - m * 1.4f);  // the plate (its top hangs off the screen's edge, as the game's)
            float u = r.height() / 116f;  // layout units: the plate is 256 x 116
            if (on)  // a soft light around it
                for (float rad = m; rad > 0; rad -= m / 3f)
                    for (int k = 0; k < 16; k++) {
                        double an = 2 * Math.PI * k / 16;
                        float dx = (float) Math.cos(an) * rad, dy = (float) Math.sin(an) * rad;
                        canvas.drawBitmap(bb, null, new RectF(r.left + dx, r.top + dy, r.right + dx, r.bottom + dy), glow);
                    }
            if (sb != null) {  // the paper edge and the shadow: 264 x 129, 3 right and 5 down of the plate's middle
                float cx = r.centerX() + 3 * u, cy = r.centerY() + 5 * u, hw = r.width() / 2 * 264f / 256f, hh = 129 * u / 2;
                canvas.drawBitmap(sb, null, new RectF(cx - hw, cy - hh, cx + hw, cy + hh), shadow);
            }
            int layer = canvas.saveLayer(b.left - m, b.top - m, b.right + m, b.bottom + m, null);
            canvas.drawBitmap(bb, null, r, base);
            Paint atop = new Paint(Paint.FILTER_BITMAP_FLAG | Paint.ANTI_ALIAS_FLAG);
            atop.setXfermode(new android.graphics.PorterDuffXfermode(android.graphics.PorterDuff.Mode.SRC_ATOP));
            if (sc != null) {  // the scratches, light, over the plate only
                android.graphics.BitmapShader bs = new android.graphics.BitmapShader(sc, Shader.TileMode.REPEAT, Shader.TileMode.REPEAT);
                android.graphics.Matrix mx = new android.graphics.Matrix();
                mx.setScale(scratch, scratch);
                mx.postTranslate(r.left, r.top);
                bs.setLocalMatrix(mx);
                atop.setShader(bs);
                atop.setColorFilter(new android.graphics.ColorMatrixColorFilter(new float[] {
                        0, 0, 0, 0, 255, 0, 0, 0, 0, 255, 0, 0, 0, 0, 255, 0.75f, 0, 0, 0, 0}));
                canvas.drawRect(r, atop);
                atop.setShader(null);
                atop.setColorFilter(null);
            }
            if (gb != null) {  // the gloss: its light only (its dark lower part made the plates dull)
                atop.setColorFilter(new android.graphics.ColorMatrixColorFilter(new float[] {
                        0, 0, 0, 0, 255, 0, 0, 0, 0, 255, 0, 0, 0, 0, 255, 0.4f, 0, 0, 0, 0}));
                canvas.drawBitmap(gb, null, r, atop);
            }
            canvas.restoreToCount(layer);
        }

        @Override public void setAlpha(int alpha) {}
        @Override public void setColorFilter(ColorFilter cf) {}
        @Override public int getOpacity() { return PixelFormat.TRANSLUCENT; }
    }

    /**
     * The GamePad menus' paper sheet (MenuItem_00 W_Base_00, from the player's game files via GameIcons) drawn by the
     * app where the game doesn't (the in-game menu): a window of 48-unit frame pieces (a torn corner and two torn
     * edges, turned per side) filled with the paper pattern, a soft white edge under it (W_BaseEdgeShadow_00, alpha
     * 190) and a blue line along its edge over it (W_BaseGlowLine_00, 007bc7, alpha 110). The bounds are the sheet;
     * unit = pixels per layout unit. Tints measured on the game's screen. Until the textures are built: Parchment.
     */
    /** Buttons in rows, right-aligned, as many to a row as fit, in order (a dialog's buttons on a narrow screen). */
    static final class ButtonRows extends android.view.ViewGroup {
        private final int gap;

        ButtonRows(android.content.Context c, int gap) {
            super(c);
            this.gap = gap;
        }

        @Override
        protected void onMeasure(int wSpec, int hSpec) {
            int w = MeasureSpec.getSize(wSpec);
            int x = 0, rowH = 0, h = 0;
            for (int i = 0; i < getChildCount(); i++) {
                View v = getChildAt(i);
                LayoutParams lp = v.getLayoutParams();
                v.measure(MeasureSpec.makeMeasureSpec(w, MeasureSpec.AT_MOST),
                        lp.height > 0 ? MeasureSpec.makeMeasureSpec(lp.height, MeasureSpec.EXACTLY) : MeasureSpec.UNSPECIFIED);
                int cw = v.getMeasuredWidth();
                if (x > 0 && x + gap + cw > w) {  // a new row
                    h += rowH + gap;
                    x = 0;
                    rowH = 0;
                }
                x += (x > 0 ? gap : 0) + cw;
                rowH = Math.max(rowH, v.getMeasuredHeight());
            }
            setMeasuredDimension(w, h + rowH);
        }

        @Override
        protected void onLayout(boolean changed, int l, int t, int r, int b) {
            int w = r - l, i = 0, y = 0;
            while (i < getChildCount()) {
                // the row: from i while it fits
                int j = i, rowW = 0, rowH = 0;
                while (j < getChildCount()) {
                    int cw = getChildAt(j).getMeasuredWidth();
                    if (j > i && rowW + gap + cw > w) break;
                    rowW += (j > i ? gap : 0) + cw;
                    rowH = Math.max(rowH, getChildAt(j).getMeasuredHeight());
                    j++;
                }
                int x = w - rowW;
                for (int k = i; k < j; k++) {
                    View v = getChildAt(k);
                    v.layout(x, y, x + v.getMeasuredWidth(), y + v.getMeasuredHeight());
                    x += v.getMeasuredWidth() + gap;
                }
                y += rowH + gap;
                i = j;
            }
        }
    }

    static final class GamePaper extends Drawable {
        private final Context c;
        private final float unit;
        private final Parchment plain;
        private final Paint p = new Paint(Paint.FILTER_BITMAP_FLAG | Paint.ANTI_ALIAS_FLAG);

        GamePaper(Context c, float unit) {
            this.c = c;
            this.unit = unit;
            plain = new Parchment(c);
        }

        private android.graphics.Bitmap tex(String n, Runnable again) { return GameIcons.get(c, n + "^t", again); }

        // the frame: corners LT RT LB RB, edges L R T B; turns (clockwise quarter turns) as the layout's frame flips
        private static final int[] TURN = {0, 1, 3, 2, 3, 1, 0, 2};

        /** piece i of the window in r with thickness f: its rectangle (before turning) */
        private static RectF piece(int i, RectF r, float f) {
            switch (i) {
                case 0: return new RectF(r.left, r.top, r.left + f, r.top + f);
                case 1: return new RectF(r.right - f, r.top, r.right, r.top + f);
                case 2: return new RectF(r.left, r.bottom - f, r.left + f, r.bottom);
                case 3: return new RectF(r.right - f, r.bottom - f, r.right, r.bottom);
                case 4: return new RectF(r.left, r.top + f, r.left + f, r.bottom - f);
                case 5: return new RectF(r.right - f, r.top + f, r.right, r.bottom - f);
                case 6: return new RectF(r.left + f, r.top, r.right - f, r.top + f);
                default: return new RectF(r.left + f, r.bottom - f, r.right - f, r.bottom);
            }
        }

        /** a frame piece's texture into dst, turned `turn` quarter turns; edges repeat along their length (256 units) */
        private void drawPiece(Canvas canvas, android.graphics.Bitmap b, RectF dst, int turn, Paint paint) {
            if (b == null || dst.width() <= 0 || dst.height() <= 0) return;
            boolean side = turn % 2 == 1;  // the texture's length runs down the screen
            float len = side ? dst.height() : dst.width(), thick = side ? dst.width() : dst.height();
            float tl = b.getWidth() * unit;  // one texture's length on the screen
            canvas.save();
            canvas.clipRect(dst);
            canvas.translate(dst.centerX(), dst.centerY());
            canvas.rotate(90 * turn);
            for (float x = -len / 2; x < len / 2; x += tl)
                canvas.drawBitmap(b, null, new RectF(x, -thick / 2, x + tl, thick / 2), paint);
            canvas.restore();
        }

        @Override
        public void draw(Canvas canvas) {
            Runnable again = () -> new android.os.Handler(android.os.Looper.getMainLooper()).post(this::invalidateSelf);
            android.graphics.Bitmap lt = tex("RepeatWindowLT_00", again), tb = tex("RepeatWindowTB_00", null), lr = tex("RepeatWindowLR_00", null);
            android.graphics.Bitmap paper = GameIcons.get(c, "ItemBasePattern_01^r", null);
            if (lt == null || tb == null || lr == null || paper == null) {
                plain.setBounds(getBounds());
                plain.draw(canvas);
                return;
            }
            RectF r = new RectF(getBounds());
            float f = 48 * unit;
            // the soft white edge under the sheet (its own eight pieces, unturned)
            String[] shadow = {"LT", "RT", "LB", "RB", "L", "R", "T", "B"};
            p.setColorFilter(null);
            p.setAlpha(190);
            for (int i = 0; i < 8; i++) {
                android.graphics.Bitmap b = tex("RepeatWindowEdgeShadow" + shadow[i] + "_00", null);
                if (b == null) continue;
                canvas.drawBitmap(b, null, piece(i, r, f), p);  // (soft: stretched along the edges)
            }
            p.setAlpha(255);
            // the sheet: the pieces' shape filled with the paper (measured tint: ebf0da)
            int layer = canvas.saveLayer(r, null);
            for (int i = 0; i < 8; i++) drawPiece(canvas, i < 4 ? lt : i < 6 ? lr : tb, piece(i, r, f), TURN[i], p);
            canvas.drawRect(r.left + f - 1, r.top + f - 1, r.right - f + 1, r.bottom - f + 1, p);
            Paint fill = new Paint(Paint.FILTER_BITMAP_FLAG);
            // (mirrored where it repeats: a wide window's sheet is more than one texture wide, plain repeats showed a seam)
            android.graphics.BitmapShader bs = new android.graphics.BitmapShader(paper, Shader.TileMode.MIRROR, Shader.TileMode.MIRROR);
            android.graphics.Matrix m = new android.graphics.Matrix();
            m.setScale(1280f / paper.getWidth() * unit, 1280f / paper.getWidth() * unit);
            m.postTranslate(r.left, r.top);
            bs.setLocalMatrix(m);
            fill.setShader(bs);
            fill.setColorFilter(new android.graphics.PorterDuffColorFilter(0xFFEBF0DA, android.graphics.PorterDuff.Mode.MULTIPLY));
            fill.setXfermode(new android.graphics.PorterDuffXfermode(android.graphics.PorterDuff.Mode.SRC_IN));
            canvas.drawRect(r, fill);
            canvas.restoreToCount(layer);
            // the blue line along the edge
            android.graphics.Bitmap glt = tex("RepeatWindowGlowLineLT_00", null), gtb = tex("RepeatWindowGlowLineTB_00", null), glr = tex("RepeatWindowGlowLineLR_00", null);
            if (glt != null && gtb != null && glr != null) {
                p.setColorFilter(new android.graphics.PorterDuffColorFilter(0xFF007BC7, android.graphics.PorterDuff.Mode.SRC_IN));
                p.setAlpha(110);
                for (int i = 0; i < 8; i++) drawPiece(canvas, i < 4 ? glt : i < 6 ? glr : gtb, piece(i, r, f), TURN[i], p);
                p.setColorFilter(null);
                p.setAlpha(255);
            }
        }

        @Override public void setAlpha(int alpha) {}
        @Override public void setColorFilter(ColorFilter cf) {}
        @Override public int getOpacity() { return PixelFormat.TRANSLUCENT; }
    }

    /** the GamePad menus' blue background (P_BgPattern_00: ItemBasePattern_00 by its luminance, tint measured on the
     * game's screen), or the slate colour until the textures are built */
    static final class GameBackground extends Drawable {
        private final Context c;
        private final float unit;
        private final Paint p = new Paint(Paint.FILTER_BITMAP_FLAG);

        GameBackground(Context c, float unit) {
            this.c = c;
            this.unit = unit;
        }

        @Override
        public void draw(Canvas canvas) {
            android.graphics.Bitmap b = GameIcons.get(c, "ItemBasePattern_00^r", () -> new android.os.Handler(android.os.Looper.getMainLooper()).post(this::invalidateSelf));
            if (b == null) {
                canvas.drawColor(SLATE);
                return;
            }
            android.graphics.BitmapShader bs = new android.graphics.BitmapShader(b, Shader.TileMode.MIRROR, Shader.TileMode.MIRROR);
            android.graphics.Matrix m = new android.graphics.Matrix();
            float k = 1280f / b.getWidth() * unit;
            m.setScale(k, k);
            bs.setLocalMatrix(m);
            p.setShader(bs);
            // luminance from (40,70,85) to (188,231,242)
            p.setColorFilter(new android.graphics.ColorMatrixColorFilter(new float[] {
                    (188 - 40) / 255f, 0, 0, 0, 40, (231 - 70) / 255f, 0, 0, 0, 70, (242 - 85) / 255f, 0, 0, 0, 85, 0, 0, 0, 1, 0}));
            canvas.drawRect(getBounds(), p);
        }

        @Override public void setAlpha(int alpha) {}
        @Override public void setColorFilter(ColorFilter cf) {}
        @Override public int getOpacity() { return PixelFormat.OPAQUE; }
    }

    /**
     * Text in the game's text font (CKingMsg, GameFont.MSG), laid out here: words wrapped to the width, maxLines with
     * an ellipsis, the gravity's alignment; characters the font lacks (arrows, ticks) in the system font on the same
     * line. Until the font is built (first use): a plain TextView.
     */
    static final class GameText extends TextView {
        private final java.util.ArrayList<String> lines = new java.util.ArrayList<>();
        private int laidOutFor = -1;
        private CharSequence laidOutText;
        private final Paint glyphPaint = new Paint(Paint.FILTER_BITMAP_FLAG | Paint.ANTI_ALIAS_FLAG);

        GameText(Context c) {
            super(c);
            // the letters only: the glyphs carry an outline (coverage ~128) that blurred them at text sizes
            glyphPaint.setColorFilter(new android.graphics.ColorMatrixColorFilter(new float[] {
                    1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 2.2f, -300}));
        }

        private boolean game() {
            return GameFont.MSG.isReady(getContext(), () -> { laidOutFor = -1; requestLayout(); invalidate(); });
        }

        // a new text is measured and drawn again: TextView's setText does that only through its own layout, which
        // this view never makes (the preparation card stayed at "0 of 0 parts (0:00)" while its bar moved)
        @Override
        protected void onTextChanged(CharSequence text, int start, int lengthBefore, int lengthAfter) {
            super.onTextChanged(text, start, lengthBefore, lengthAfter);
            requestLayout();
            invalidate();
        }

        // the font's cell (50 high) to the text size: a little larger than the system font's (its letters are small
        // in their cell: at the system font's size it read too small)
        private float scale() { return getTextSize() * 1.62f / GameFont.MSG.height(); }

        private float lineHeight() { return GameFont.MSG.height() * scale() * 0.92f * getLineSpacingMultiplier() + getLineSpacingExtra(); }

        private float charWidth(char ch, float k) {
            return GameFont.MSG.has(ch) ? GameFont.MSG.advance(ch) * k : getPaint().measureText(String.valueOf(ch));
        }

        private float widthOf(String s, float k) {
            float w = 0;
            for (int i = 0; i < s.length(); i++) w += charWidth(s.charAt(i), k);
            return w;
        }

        /** the lines for this width (cached) */
        private void layOut(int width) {
            CharSequence t = getText();
            if (width == laidOutFor && t.equals(laidOutText)) return;
            laidOutFor = width;
            laidOutText = t;
            lines.clear();
            float k = scale();
            for (String para : t.toString().split("\n", -1)) {
                String[] words = para.split(" ", -1);
                StringBuilder line = new StringBuilder();
                for (String w : words) {
                    String next = line.length() == 0 ? w : line + " " + w;
                    if (line.length() > 0 && widthOf(next, k) > width) {
                        lines.add(line.toString());
                        line.setLength(0);
                        line.append(w);
                    } else {
                        line.setLength(0);
                        line.append(next);
                    }
                }
                lines.add(line.toString());
            }
            int max = getMaxLines();
            if (max > 0 && lines.size() > max) {  // the last one shown ends with an ellipsis
                String last = lines.get(max - 1);
                String dots = GameFont.MSG.has('…') ? "…" : "...";
                while (last.length() > 0 && widthOf(last + dots, k) > width) last = last.substring(0, last.length() - 1);
                while (lines.size() > max) lines.remove(lines.size() - 1);
                lines.set(max - 1, last.trim() + dots);
            }
        }

        @Override
        protected void onMeasure(int wSpec, int hSpec) {
            if (!game()) { super.onMeasure(wSpec, hSpec); return; }
            int wMode = MeasureSpec.getMode(wSpec), wSize = MeasureSpec.getSize(wSpec);
            int pad = getCompoundPaddingLeft() + getCompoundPaddingRight();  // (with a drawable at an end, e.g. a submenu's arrow)
            int avail = wMode == MeasureSpec.UNSPECIFIED ? Integer.MAX_VALUE / 2 : wSize - pad;
            layOut(Math.max(1, avail));
            float k = scale(), widest = 0;
            for (String l : lines) widest = Math.max(widest, widthOf(l, k));
            int w = wMode == MeasureSpec.EXACTLY ? wSize : (int) Math.ceil(widest) + pad;
            if (wMode == MeasureSpec.AT_MOST) w = Math.min(w, wSize);
            w = Math.max(w, Math.max(getSuggestedMinimumWidth(), getMinWidth()));
            if (w - pad != avail) layOut(Math.max(1, w - pad));
            int hMode = MeasureSpec.getMode(hSpec), hSize = MeasureSpec.getSize(hSpec);
            int h = (int) Math.ceil(lines.size() * lineHeight()) + getPaddingTop() + getPaddingBottom();
            if (hMode == MeasureSpec.EXACTLY) h = hSize;
            else if (hMode == MeasureSpec.AT_MOST) h = Math.min(h, hSize);
            setMeasuredDimension(w, Math.max(h, Math.max(getSuggestedMinimumHeight(), getMinHeight())));  // (TextView's own minimums too)
        }

        @Override
        protected void onDraw(Canvas canvas) {
            if (!game()) { super.onDraw(canvas); return; }
            int inner = getWidth() - getCompoundPaddingLeft() - getCompoundPaddingRight();
            layOut(Math.max(1, inner));
            float k = scale(), lh = lineHeight(), total = lines.size() * lh;
            int g = getGravity(), hg = Gravity.getAbsoluteGravity(g, getLayoutDirection()) & Gravity.HORIZONTAL_GRAVITY_MASK, vg = g & Gravity.VERTICAL_GRAVITY_MASK;
            float top = getPaddingTop(), boxH = getHeight() - getPaddingTop() - getPaddingBottom();
            float y = vg == Gravity.CENTER_VERTICAL ? top + (boxH - total) / 2 : vg == Gravity.BOTTOM ? top + boxH - total : top;
            glyphPaint.setColor(getCurrentTextColor());
            TextPaint tp = getPaint();
            tp.setColor(getCurrentTextColor());
            float cellH = GameFont.MSG.height() * k, base = GameFont.MSG.baselinePx() * k;
            for (String l : lines) {
                float lw = widthOf(l, k);
                float x = getCompoundPaddingLeft() + (hg == Gravity.CENTER_HORIZONTAL ? (inner - lw) / 2 : hg == Gravity.RIGHT ? inner - lw : 0);
                float cy = y + (lh - cellH) / 2;  // the cell in the middle of its line
                for (int i = 0; i < l.length(); i++) {
                    char ch = l.charAt(i);
                    if (GameFont.MSG.has(ch)) x += GameFont.MSG.draw(canvas, ch, x, cy, k, glyphPaint);
                    else {
                        String one = String.valueOf(ch);
                        canvas.drawText(one, x, cy + base, tp);
                        x += tp.measureText(one);
                    }
                }
                y += lh;
            }
            android.graphics.drawable.Drawable end = getCompoundDrawables()[2];  // (the right end's drawable only)
            if (end != null) {
                android.graphics.Rect eb = end.getBounds();
                canvas.save();
                canvas.translate(getWidth() - getPaddingRight() - eb.width(), top + (boxH - eb.height()) / 2f);
                end.draw(canvas);
                canvas.restore();
            }
        }
    }

    /**
     * A row in the shape of the game's inventory buttons (Save / Options: BtnMenuSaveOption_00, from the player's game
     * files via GameIcons): the button's skewed paper plate with its rim and scratches (BtnSequencePattern_01) in
     * paper tones (lit, pressed or chosen: the yellow highlight), the white edge and soft shadow behind it
     * (BtnSequenceShadow_01). The textures' visible parts are fitted to the row: its ends keep their shape, the
     * middle stretches. Until the textures are built: the slot.
     */
    static final class GameRowPlate extends Drawable {
        private final Context c;
        private final float unit;
        private final Drawable plain;
        private final Paint base = new Paint(Paint.FILTER_BITMAP_FLAG | Paint.ANTI_ALIAS_FLAG), p = new Paint(Paint.FILTER_BITMAP_FLAG);
        // the visible parts of the 320 x 100 textures; their corners (CAP texels) keep their size: the rim stays thin
        private static final android.graphics.Rect PLATE = new android.graphics.Rect(9, 14, 310, 85), EDGE = new android.graphics.Rect(5, 10, 320, 99);
        private static final int CAP = 16;

        /** unit: pixels per layout unit (a texel of the game's button is 0.9 of one) */
        GameRowPlate(Context c, boolean lit, float unit) {
            this.c = c;
            this.unit = unit;
            plain = lit ? slot(c, GLOW, YELLOW) : slot(c, 0x1F000000, 0);
            // rim (the texture's dark) to fill (its light): a soft paper shade, or the highlight's yellows
            base.setColorFilter(lit ? GameTabPlate.lerpFilter(0xFFE0BE2A, GLOW) : GameTabPlate.lerpFilter(0xFFB4B9A2, 0xFFE4E7D6));
        }

        /** the part src of a texture in r in nine pieces: corners of CAP texels at k pixels each, the rest stretched */
        private static void drawNine(Canvas canvas, android.graphics.Bitmap b, android.graphics.Rect src, RectF r, float k, Paint paint) {
            float cx = Math.min(CAP * k, r.width() / 2), cy = Math.min(CAP * k, r.height() / 2);
            int sx = Math.round(cx / k), sy = Math.round(cy / k);
            int[] xs = {src.left, src.left + sx, src.right - sx, src.right}, ys = {src.top, src.top + sy, src.bottom - sy, src.bottom};
            float[] dx = {r.left, r.left + cx, r.right - cx, r.right}, dy = {r.top, r.top + cy, r.bottom - cy, r.bottom};
            for (int i = 0; i < 3; i++)
                for (int j = 0; j < 3; j++)
                    canvas.drawBitmap(b, new android.graphics.Rect(xs[i], ys[j], xs[i + 1], ys[j + 1]), new RectF(dx[i], dy[j], dx[i + 1], dy[j + 1]), paint);
        }

        @Override
        public void draw(Canvas canvas) {
            Runnable again = () -> new android.os.Handler(android.os.Looper.getMainLooper()).post(this::invalidateSelf);
            android.graphics.Bitmap pat = GameIcons.get(c, "BtnSequencePattern_01^t", again), sh = GameIcons.get(c, "BtnSequenceShadow_01^t", null);
            if (pat == null) {
                plain.setBounds(getBounds());
                plain.draw(canvas);
                return;
            }
            RectF bd = new RectF(getBounds());
            float k = 0.9f * unit;
            // the plate leaves room right and below for its edge and shadow
            RectF plate = new RectF(bd.left + (PLATE.left - EDGE.left) * k, bd.top + (PLATE.top - EDGE.top) * k,
                    bd.right - (EDGE.right - PLATE.right) * k, bd.bottom - (EDGE.bottom - PLATE.bottom) * k);
            if (sh != null) {
                p.setColorFilter(GameTabPlate.lerpFilter(0xFF1E1E1E, 0xFFF4F4EA));
                drawNine(canvas, sh, EDGE, bd, k, p);
            }
            drawNine(canvas, pat, PLATE, plate, k, base);
        }

        @Override public void setAlpha(int alpha) {}
        @Override public void setColorFilter(ColorFilter cf) {}
        @Override public int getOpacity() { return PixelFormat.TRANSLUCENT; }
    }

    // ---- the game's Options screen (SequenceOption_00, BtnOption_00; from the player's game files via GameIcons)
    // (its texts are green on its green panel; on the paper they take the ink of the game's item texts, the green
    // stays with the buttons: all green was too much)
    static final int OPTION_TEXT = 0xFF2E3D47, OPTION_HINT = 0xFF5E6B73, OPTION_BUTTON_TEXT = 0xFF001E00;

    /**
     * Its value button (OlvSetBtnBase_00: a torn plate with a dark rim, green 158200 .. 93d926 by its luminance,
     * deeper towards its foot), with its white edge and shadow, scratches and gloss; off: grey; lit (pressed or
     * chosen): the game's highlight over it at full strength (else faint). Ends unstretched, the middle stretched.
     */
    static final class GameValueButton extends Drawable {
        private final Context c;
        private final boolean lit;
        private final Drawable plain;
        private final Paint base = new Paint(Paint.FILTER_BITMAP_FLAG | Paint.ANTI_ALIAS_FLAG), p = new Paint(Paint.FILTER_BITMAP_FLAG);
        private static final android.graphics.Rect PLATE = new android.graphics.Rect(3, 3, 352, 70), EDGE = new android.graphics.Rect(2, 0, 366, 85);

        GameValueButton(Context c, boolean on, boolean lit) {
            this.c = c;
            this.lit = lit;
            plain = lit ? slot(c, GLOW, YELLOW) : slot(c, on ? GREEN.mid : 0xFFCBC8B6, on ? GREEN.dark : 0xFFA9A595);
            base.setColorFilter(on ? GameTabPlate.lerpFilter(0xFF158200, 0xFF93D926) : GameTabPlate.lerpFilter(0xFF6E6E64, 0xFFD6D6C8));
        }

        /** the part src of a texture in r: its ends (half its height each) unstretched, the middle stretched */
        static void drawThree(Canvas canvas, android.graphics.Bitmap b, android.graphics.Rect src, RectF r, Paint paint) {
            float k = r.height() / src.height(), cw = Math.min(src.height() / 2f * k, r.width() / 2);
            int cs = Math.round(cw / k);
            canvas.drawBitmap(b, new android.graphics.Rect(src.left, src.top, src.left + cs, src.bottom), new RectF(r.left, r.top, r.left + cw, r.bottom), paint);
            canvas.drawBitmap(b, new android.graphics.Rect(src.left + cs, src.top, src.right - cs, src.bottom), new RectF(r.left + cw, r.top, r.right - cw, r.bottom), paint);
            canvas.drawBitmap(b, new android.graphics.Rect(src.right - cs, src.top, src.right, src.bottom), new RectF(r.right - cw, r.top, r.right, r.bottom), paint);
        }

        @Override
        public void draw(Canvas canvas) {
            Runnable again = () -> new android.os.Handler(android.os.Looper.getMainLooper()).post(this::invalidateSelf);
            android.graphics.Bitmap bb = GameIcons.get(c, "OlvSetBtnBase_00^t", again), sh = GameIcons.get(c, "OlvSetBtnShadow_00^t", null);
            android.graphics.Bitmap dot = GameIcons.get(c, "OlvSetBtnDot_00^t", null), spec = GameIcons.get(c, "OlvSetBtnSpec_00^t", null);
            android.graphics.Bitmap sel = GameIcons.get(c, "BtnSelect_00^q", null);
            if (bb == null) {
                plain.setBounds(getBounds());
                plain.draw(canvas);
                return;
            }
            RectF bd = new RectF(getBounds());
            // the plate: 349 x 67 of its texture; its edge and shadow reach 3 right and 15 below (the layout's offset)
            float k = bd.height() / (PLATE.height() + 15f);
            RectF r = new RectF(bd.left, bd.top, bd.right - 6 * k, bd.top + PLATE.height() * k);
            if (sh != null) {
                p.setColorFilter(GameTabPlate.lerpFilter(0xFF1E1E1E, 0xFFF0F0E4));
                drawThree(canvas, sh, EDGE, new RectF(r.left - 1 * k, r.top - 3 * k + 6 * k, r.right + 14 * k, r.top + (EDGE.height() + 3) * k), p);
                p.setColorFilter(null);
            }
            int layer = canvas.saveLayer(bd, null);
            drawThree(canvas, bb, PLATE, r, base);
            Paint mul = new Paint();
            mul.setShader(new LinearGradient(0, r.top, 0, r.bottom, 0xFFFFFFFF, 0xFFB4DCB4, Shader.TileMode.CLAMP));
            mul.setXfermode(new android.graphics.PorterDuffXfermode(android.graphics.PorterDuff.Mode.MULTIPLY));
            canvas.drawRect(r, mul);
            Paint atop = new Paint(Paint.FILTER_BITMAP_FLAG);
            atop.setXfermode(new android.graphics.PorterDuffXfermode(android.graphics.PorterDuff.Mode.SRC_ATOP));
            if (dot != null) {  // the scratches: darker where the texture is dark, faintly
                atop.setColorFilter(new android.graphics.ColorMatrixColorFilter(new float[] {
                        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, -0.12f, 0, 0, 0.12f, 0}));
                drawThree(canvas, dot, PLATE, r, atop);
            }
            if (spec != null) {  // the gloss: its light only
                atop.setColorFilter(new android.graphics.ColorMatrixColorFilter(new float[] {
                        0, 0, 0, 0, 255, 0, 0, 0, 0, 255, 0, 0, 0, 0, 255, 0.35f, 0, 0, 0, 0}));
                drawThree(canvas, spec, PLATE, r, atop);
            }
            if (sel != null && lit) {
                atop.setColorFilter(null);
                atop.setAlpha(200);
                canvas.drawBitmap(sel, null, r, atop);
            }
            canvas.restoreToCount(layer);
        }

        @Override public void setAlpha(int alpha) {}
        @Override public void setColorFilter(ColorFilter cf) {}
        @Override public int getOpacity() { return PixelFormat.TRANSLUCENT; }
    }

    /** its arrows beside a value (OptionArrow_00, drawn pointing up: turned to the side, a little tilted as there),
     * in the option text's green on the paper */
    static final class OptionArrow extends Drawable {
        private final Context c;
        private final boolean left;
        private final Paint p = new Paint(Paint.FILTER_BITMAP_FLAG | Paint.ANTI_ALIAS_FLAG);

        OptionArrow(Context c, boolean left) { this(c, left, OPTION_TEXT); }

        OptionArrow(Context c, boolean left, int color) {
            this.c = c;
            this.left = left;
            p.setColorFilter(new android.graphics.PorterDuffColorFilter(color, android.graphics.PorterDuff.Mode.SRC_IN));
        }

        @Override
        public void draw(Canvas canvas) {
            android.graphics.Bitmap b = GameIcons.get(c, "OptionArrow_00^s", () -> new android.os.Handler(android.os.Looper.getMainLooper()).post(this::invalidateSelf));
            android.graphics.Rect bd = getBounds();
            float cx = bd.exactCenterX(), cy = bd.exactCenterY();
            if (b == null) {
                Paint t = new Paint(Paint.ANTI_ALIAS_FLAG);
                t.setColor(OPTION_TEXT);
                t.setTextSize(bd.height() * 0.6f);
                t.setTextAlign(Paint.Align.CENTER);
                canvas.drawText(left ? "◀" : "▶", cx, cy + bd.height() * 0.2f, t);
                return;
            }
            // 64 x 96 upright; turned it is 96 wide, 64 high
            float k = Math.min(bd.width() / 96f, bd.height() / 64f);
            canvas.save();
            canvas.rotate(left ? -84 : 96, cx, cy);
            canvas.drawBitmap(b, null, new RectF(cx - 32 * k, cy - 48 * k, cx + 32 * k, cy + 48 * k), p);
            canvas.restore();
        }

        @Override public void setAlpha(int alpha) { p.setAlpha(alpha); }
        @Override public void setColorFilter(ColorFilter cf) {}
        @Override public int getOpacity() { return PixelFormat.TRANSLUCENT; }
    }

    /** the game's cursor (Cursor_00: a top-left corner in two frames, luminance + alpha, which the game tints: white
     * inside, brown outline), mirrored into the four corners of r, size pixels each, reaching a little outside; it
     * wobbles between its frames every 250 ms (the caller redraws). false: the texture isn't built yet */
    static final class MenuCursor {
        private static final Paint paint = new Paint(Paint.FILTER_BITMAP_FLAG);
        static {
            float br = 0x50, bg = 0x30, bb = 0x14;  // the outline's brown
            paint.setColorFilter(new android.graphics.ColorMatrixColorFilter(new float[] {
                    (255 - br) / 255f, 0, 0, 0, br,
                    0, (255 - bg) / 255f, 0, 0, bg,
                    0, 0, (255 - bb) / 255f, 0, bb,
                    0, 0, 0, 1, 0}));
        }
        private static final RectF dst = new RectF();

        private MenuCursor() {}

        static boolean draw(Context c, Canvas canvas, RectF r, float size, Runnable whenReady) {
            boolean second = (android.os.SystemClock.uptimeMillis() / 250) % 2 == 1;
            android.graphics.Bitmap bm = GameIcons.get(c, second ? "Crsor_01^t" : "Crsor_00^t", whenReady);
            if (bm == null) return false;
            float o = size * 0.3f;
            for (int i = 0; i < 4; i++) {
                float sx = (i & 1) == 0 ? 1 : -1, sy = (i & 2) == 0 ? 1 : -1;
                canvas.save();
                canvas.translate(sx > 0 ? r.left : r.right, sy > 0 ? r.top : r.bottom);
                canvas.scale(sx, sy);
                dst.set(-o, -o, size - o, size - o);
                canvas.drawBitmap(bm, null, dst, paint);
                canvas.restore();
            }
            return true;
        }
    }

    /** its chosen row: two soft translucent bands crossed a little (W_ContentCursor_00 / _01, turned +-8 degrees on
     * its short rows; less on ours, which are wide) */
    static final class OptionCursor extends Drawable {
        private final Paint p = new Paint(Paint.ANTI_ALIAS_FLAG);

        OptionCursor() { p.setColor(0x80FFF4AE); }

        @Override
        public void draw(Canvas canvas) {
            RectF r = new RectF(getBounds());
            float tilt = (float) Math.toDegrees(Math.atan2(r.height() * 0.14f, r.width()));
            r.inset(r.width() * 0.01f, r.height() * 0.08f);
            for (float t : new float[] {tilt, -tilt}) {
                canvas.save();
                canvas.rotate(t, r.centerX(), r.centerY());
                canvas.drawRoundRect(r, r.height() * 0.3f, r.height() * 0.3f, p);
                canvas.restore();
            }
        }

        @Override public void setAlpha(int alpha) {}
        @Override public void setColorFilter(ColorFilter cf) {}
        @Override public int getOpacity() { return PixelFormat.TRANSLUCENT; }
    }

    static final class OutlinedText extends TextView {
        int outline = CYAN_DARK;
        // > 0: in the game's own font (GameFont) at this scale (screen pixels per font pixel), as its tabs show text
        float gameFontScale;
        // or the game's font at the text size (a dialog's title), measured by it
        boolean gameFontAuto;

        OutlinedText(Context c) { super(c); }

        private float autoScale() { return getTextSize() * 1.35f / GameFont.cellHeight(); }

        // (a text with letters the game's font lacks, e.g. most kanji: the system's font)
        private boolean gameFont(Runnable whenReady) {
            return GameFont.ready(getContext(), whenReady) && GameFont.covers(getText().toString());
        }

        @Override
        protected void onMeasure(int wSpec, int hSpec) {
            if (!gameFontAuto || !gameFont(() -> { requestLayout(); invalidate(); })) { super.onMeasure(wSpec, hSpec); return; }
            float k = autoScale();
            int w = (int) Math.ceil(GameFont.width(getText().toString()) * k) + getPaddingLeft() + getPaddingRight();
            int h = (int) Math.ceil(GameFont.cellHeight() * k * 0.8f) + getPaddingTop() + getPaddingBottom();
            setMeasuredDimension(resolveSize(w, wSpec), resolveSize(h, hSpec));
        }

        @Override
        protected void onDraw(Canvas canvas) {
            if ((gameFontScale > 0 || gameFontAuto) && gameFont(this::invalidate)) {
                float k = gameFontAuto ? autoScale() : gameFontScale, ring2 = 0;  // (the glyphs carry their outline)
                android.graphics.Bitmap b = GameFont.label(getText().toString(), k, getCurrentTextColor(), outline);
                float cx = (getPaddingLeft() + getWidth() - getPaddingRight()) / 2f, cy = (getPaddingTop() + getHeight() - getPaddingBottom()) / 2f;
                // the capitals' middle (about 0.36 of the baseline's height above it) on the box's middle
                float top = cy - (GameFont.labelBaseline(k, ring2) - 0.36f * GameFont.cellHeight() * 0.78f * k);
                canvas.drawBitmap(b, cx - b.getWidth() / 2f, top, null);
                return;
            }
            Layout l = getLayout();
            if (l == null) { super.onDraw(canvas); return; }
            TextPaint p = getPaint();
            canvas.save();
            canvas.translate(getCompoundPaddingLeft(), getExtendedPaddingTop());
            p.setStyle(Paint.Style.STROKE);
            p.setStrokeJoin(Paint.Join.ROUND);
            p.setStrokeWidth(getTextSize() * 0.16f);
            p.setColor(outline);
            l.draw(canvas);
            p.setStyle(Paint.Style.FILL);
            p.setColor(getCurrentTextColor());
            l.draw(canvas);
            canvas.restore();
        }
    }
}
