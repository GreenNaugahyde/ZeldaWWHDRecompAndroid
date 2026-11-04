package org.wwhdrecomp.app;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.LinearGradient;
import android.graphics.Paint;
import android.graphics.RectF;
import android.graphics.Shader;
import android.graphics.Typeface;
import android.view.MotionEvent;
import android.view.View;

// a colour picker by touch: hue round the disc, saturation from its middle out, brightness on the bar beside it;
// the colour and its code at the end. While a finger moves only this view changes; on lifting `done` gets the colour
// (the page then remakes Link's picture)
final class ColorWheel extends View {
    interface Done { void color(float[] hsv); }

    private final float[] hsv = new float[3];
    private final Done done;
    private final int ink;
    private final float dp;
    private Bitmap disc;
    private float cx, cy, radius;
    private final RectF bar = new RectF(), swatch = new RectF();
    private int dragging;  // 0 none, 1 disc, 2 bar
    private final Paint p = new Paint(Paint.ANTI_ALIAS_FLAG), dim = new Paint(Paint.ANTI_ALIAS_FLAG);

    ColorWheel(Context c, int rgb, int ink, Done done) {
        super(c);
        Color.colorToHSV(rgb | 0xFF000000, hsv);
        this.done = done;
        this.ink = ink;
        dp = c.getResources().getDisplayMetrics().density;
    }

    @Override
    protected void onSizeChanged(int w, int h, int ow, int oh) {
        float pad = 10 * dp;
        radius = (h - 2 * pad) / 2;
        cx = pad + radius;
        cy = h / 2f;
        bar.set(cx + radius + 22 * dp, pad, cx + radius + 22 * dp + 34 * dp, h - pad);
        float sl = bar.right + 22 * dp, size = Math.min(w - sl - pad, h * 0.5f);
        swatch.set(sl, cy - size * 0.75f, sl + Math.max(0, size), cy + size * 0.25f);
        int d = Math.max(1, Math.round(2 * radius));
        int[] px = new int[d * d];
        float r = d / 2f;
        float[] t = new float[3];
        t[2] = 1;
        for (int y = 0; y < d; y++)
            for (int x = 0; x < d; x++) {
                float dx = x + 0.5f - r, dy = y + 0.5f - r, dist = (float) Math.hypot(dx, dy);
                if (dist > r) continue;
                t[0] = (float) ((Math.toDegrees(Math.atan2(-dy, dx)) + 360) % 360);
                t[1] = dist / r;
                int a = dist > r - 1 ? Math.round((r - dist) * 255) : 255;  // a soft edge
                px[y * d + x] = (Color.HSVToColor(t) & 0xFFFFFF) | (a << 24);
            }
        disc = Bitmap.createBitmap(px, d, d, Bitmap.Config.ARGB_8888);
    }

    @Override
    protected void onDraw(Canvas c) {
        if (disc == null) return;
        c.drawBitmap(disc, cx - radius, cy - radius, null);
        dim.setColor(Color.argb(Math.round((1 - hsv[2]) * 255), 0, 0, 0));  // the disc as dark as the colour
        c.drawCircle(cx, cy, radius, dim);
        p.setStyle(Paint.Style.STROKE);
        p.setStrokeWidth(2 * dp);
        p.setColor(0x60000000);
        c.drawCircle(cx, cy, radius, p);

        double ang = Math.toRadians(hsv[0]);
        float mx = cx + (float) Math.cos(ang) * hsv[1] * radius, my = cy - (float) Math.sin(ang) * hsv[1] * radius;
        marker(c, mx, my, 9 * dp);

        float[] top = {hsv[0], hsv[1], 1};
        p.setStyle(Paint.Style.FILL);
        p.setShader(new LinearGradient(0, bar.top, 0, bar.bottom, Color.HSVToColor(top), 0xFF000000, Shader.TileMode.CLAMP));
        c.drawRoundRect(bar, 8 * dp, 8 * dp, p);
        p.setShader(null);
        p.setStyle(Paint.Style.STROKE);
        p.setColor(0x60000000);
        c.drawRoundRect(bar, 8 * dp, 8 * dp, p);
        float by = bar.top + (1 - hsv[2]) * bar.height();
        RectF knob = new RectF(bar.left - 4 * dp, by - 5 * dp, bar.right + 4 * dp, by + 5 * dp);
        p.setStyle(Paint.Style.FILL);
        p.setColor(Color.HSVToColor(hsv));
        c.drawRoundRect(knob, 5 * dp, 5 * dp, p);
        p.setStyle(Paint.Style.STROKE);
        p.setStrokeWidth(3 * dp);
        p.setColor(0xFFFFFFFF);
        c.drawRoundRect(knob, 5 * dp, 5 * dp, p);
        p.setStrokeWidth(1.5f * dp);
        p.setColor(0xFF000000);
        c.drawRoundRect(knob, 5 * dp, 5 * dp, p);

        if (swatch.width() > 8 * dp) {
            p.setStyle(Paint.Style.FILL);
            p.setColor(Color.HSVToColor(hsv));
            c.drawRoundRect(swatch, 10 * dp, 10 * dp, p);
            p.setStyle(Paint.Style.STROKE);
            p.setStrokeWidth(2 * dp);
            p.setColor(0x60000000);
            c.drawRoundRect(swatch, 10 * dp, 10 * dp, p);
            p.setStyle(Paint.Style.FILL);
            p.setColor(ink);
            p.setTextSize(16 * dp);
            p.setTypeface(Typeface.create("sans-serif-medium", Typeface.BOLD));
            p.setTextAlign(Paint.Align.CENTER);
            c.drawText(hex(), swatch.centerX(), swatch.bottom + 26 * dp, p);
            p.setTextAlign(Paint.Align.LEFT);
        }
    }

    private void marker(Canvas c, float x, float y, float r) {
        p.setStyle(Paint.Style.FILL);
        p.setColor(Color.HSVToColor(hsv));
        c.drawCircle(x, y, r, p);
        p.setStyle(Paint.Style.STROKE);
        p.setStrokeWidth(3 * dp);
        p.setColor(0xFFFFFFFF);
        c.drawCircle(x, y, r, p);
        p.setStrokeWidth(1.5f * dp);
        p.setColor(0xFF000000);
        c.drawCircle(x, y, r + 1.5f * dp, p);
    }

    String hex() { return String.format(java.util.Locale.ROOT, "#%06X", Color.HSVToColor(hsv) & 0xFFFFFF); }

    @Override
    public boolean onTouchEvent(MotionEvent e) {
        float x = e.getX(), y = e.getY();
        switch (e.getActionMasked()) {
            case MotionEvent.ACTION_DOWN:
                if (Math.hypot(x - cx, y - cy) <= radius + 12 * dp) dragging = 1;
                else if (x >= bar.left - 16 * dp && x <= bar.right + 16 * dp) dragging = 2;
                else return false;
                getParent().requestDisallowInterceptTouchEvent(true);  // (the page scrolls otherwise)
                // fall through
            case MotionEvent.ACTION_MOVE:
                if (dragging == 1) {
                    float dx = x - cx, dy = y - cy;
                    hsv[0] = (float) ((Math.toDegrees(Math.atan2(-dy, dx)) + 360) % 360);
                    hsv[1] = Math.min(1, (float) Math.hypot(dx, dy) / radius);
                    if (hsv[2] < 0.05f) hsv[2] = 1;  // (on black the disc did nothing that could be seen)
                } else if (dragging == 2) {
                    hsv[2] = Math.max(0, Math.min(1, 1 - (y - bar.top) / bar.height()));
                }
                invalidate();
                return true;
            case MotionEvent.ACTION_UP:
                if (dragging != 0) {
                    dragging = 0;
                    getParent().requestDisallowInterceptTouchEvent(false);
                    done.color(hsv.clone());
                }
                return true;
            case MotionEvent.ACTION_CANCEL:
                dragging = 0;
                return true;
        }
        return false;
    }
}
