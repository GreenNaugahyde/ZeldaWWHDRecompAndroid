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

        private static int mix(int a, int b, float t) {
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

    static int px(Context c, float v) { return Math.round(v * c.getResources().getDisplayMetrics().density); }

    // the game's light-blue bar buttons
    static TextView barButton(Context c, String text, View.OnClickListener click) { return barButton(c, text, click, false); }

    // green: the confirming button of a dialog
    static TextView barButton(Context c, String text, View.OnClickListener click, boolean green) {
        TextView b = new TextView(c);
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
    static TextView pill(Context c, boolean on) {
        TextView pill = new TextView(c);
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
    static final class OutlinedText extends TextView {
        int outline = CYAN_DARK;

        OutlinedText(Context c) { super(c); }

        @Override
        protected void onDraw(Canvas canvas) {
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
