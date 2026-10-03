package org.wwhdrecomp.app;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.LinearGradient;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.PathMeasure;
import android.graphics.RadialGradient;
import android.graphics.Shader;
import android.os.SystemClock;
import android.view.View;

/**
 * The animated background of the setup, extraction and compile screens: a cel-shaded seascape
 * (sky, sun, curled clouds, wind swirls, toon waves, an island, gulls) with a little sailboat that
 * crosses the screen as the work progresses. Drawn entirely in code; no game assets.
 */
public final class SeaView extends View {
    private final float dp;
    private float progress = -1;  // 0..1: the boat's way across; < 0: it bobs in the middle
    private float boatX = -1;     // eased towards the progress position
    private final long t0 = SystemClock.uptimeMillis();

    private final Paint fill = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint stroke = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint sky = new Paint();
    private final Paint sunGlow = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Path path = new Path();
    private final Path seg = new Path();
    private final PathMeasure measure = new PathMeasure();

    // clouds: x (fraction, wraps), y (fraction of the sky), size, speed (fraction per second)
    private final float[][] clouds = {{0.05f, 0.18f, 1.0f, 0.006f}, {0.42f, 0.10f, 0.7f, 0.009f}, {0.70f, 0.30f, 1.2f, 0.005f},
                                      {0.95f, 0.22f, 0.8f, 0.007f}};
    private static final int SKY_TOP = 0xFF2B7FD9, SKY_HORIZON = 0xFFA8E6F7;
    private static final int[] SEA = {0xFF2E8BE0, 0xFF2477D1, 0xFF1C63BD, 0xFF1550A6, 0xFF0F3F8C};
    private static final int CLOUD = 0xFFFFFFFF, CLOUD_SHADE = 0xFFCFE6F5, FOAM = 0xFFF2FBFF;

    public SeaView(Context c) {
        super(c);
        dp = c.getResources().getDisplayMetrics().density;
        stroke.setStyle(Paint.Style.STROKE);
        stroke.setStrokeCap(Paint.Cap.ROUND);
        stroke.setStrokeJoin(Paint.Join.ROUND);
    }

    /** 0..1 moves the boat across the screen; a negative value lets it bob in the middle. */
    public void setProgress(float p) { progress = p; }

    @Override
    protected void onSizeChanged(int w, int h, int ow, int oh) {
        float horizon = h * 0.58f;
        sky.setShader(new LinearGradient(0, 0, 0, horizon, SKY_TOP, SKY_HORIZON, Shader.TileMode.CLAMP));
        float sx = w * 0.82f, sy = h * 0.17f, r = Math.min(w, h) * 0.16f;
        sunGlow.setShader(new RadialGradient(sx, sy, r, new int[] {0xFFFFFDE8, 0xCCFFF3B0, 0x00FFF3B0}, new float[] {0f, 0.35f, 1f},
                Shader.TileMode.CLAMP));
    }

    @Override
    protected void onDraw(Canvas c) {
        float w = getWidth(), h = getHeight();
        if (w == 0 || h == 0) return;
        float t = (SystemClock.uptimeMillis() - t0) / 1000f;
        float horizon = h * 0.58f;

        c.drawRect(0, 0, w, horizon + 24 * dp, sky);  // below the horizon too: the waves dip under it
        drawSun(c, w, h, t);
        for (float[] cl : clouds) {
            float x = (float) (((cl[0] + cl[3] * t) % 1.3) - 0.15) * w;
            drawCloud(c, x, cl[1] * horizon + h * 0.04f, cl[2] * Math.min(w, h) * 0.11f);
        }
        drawGulls(c, w, h, t);
        drawWind(c, w, horizon, t);
        drawIsland(c, w * 0.13f, horizon, Math.min(w, h) * 0.065f, t);
        drawSea(c, w, h, horizon, t);
        postInvalidateOnAnimation();
    }

    // ---- sky
    private void drawSun(Canvas c, float w, float h, float t) {
        float sx = w * 0.82f, sy = h * 0.17f, r = Math.min(w, h) * 0.055f;
        c.drawCircle(sx, sy, Math.min(w, h) * 0.16f, sunGlow);
        fill.setColor(0x40FFF6C8);
        int rays = 12;
        for (int i = 0; i < rays; i++) {
            double a = t * 0.12 + i * 2 * Math.PI / rays, da = Math.PI / rays * 0.45;
            float r1 = r * 1.35f, r2 = r * (i % 2 == 0 ? 2.6f : 2.1f);
            path.reset();
            path.moveTo(sx + (float) Math.cos(a - da) * r1, sy + (float) Math.sin(a - da) * r1);
            path.lineTo(sx + (float) Math.cos(a) * r2, sy + (float) Math.sin(a) * r2);
            path.lineTo(sx + (float) Math.cos(a + da) * r1, sy + (float) Math.sin(a + da) * r1);
            path.close();
            c.drawPath(path, fill);
        }
        fill.setColor(0xFFFFF4C2);
        c.drawCircle(sx, sy, r * 1.12f, fill);
        fill.setColor(0xFFFFFDF0);
        c.drawCircle(sx, sy, r, fill);
    }

    // a flat-bottomed cloud of overlapping puffs, a shade below, a curl at each end
    private void drawCloud(Canvas c, float x, float y, float s) {
        float[][] puffs = {{-1.1f, 0.15f, 0.45f}, {-0.55f, -0.2f, 0.62f}, {0.1f, -0.38f, 0.75f}, {0.75f, -0.12f, 0.6f},
                           {1.25f, 0.18f, 0.42f}};
        fill.setColor(CLOUD_SHADE);
        for (float[] p : puffs) c.drawCircle(x + p[0] * s, y + p[1] * s + s * 0.12f, p[2] * s, fill);
        c.drawRect(x - 1.2f * s, y + 0.1f * s, x + 1.35f * s, y + 0.5f * s, fill);
        fill.setColor(CLOUD);
        for (float[] p : puffs) c.drawCircle(x + p[0] * s, y + p[1] * s, p[2] * s, fill);
        c.drawRect(x - 1.15f * s, y + 0.05f * s, x + 1.3f * s, y + 0.38f * s, fill);
        stroke.setColor(CLOUD);
        stroke.setStrokeWidth(s * 0.13f);
        spiral(c, x - 1.45f * s, y + 0.28f * s, s * 0.32f, true);
        spiral(c, x + 1.62f * s, y + 0.3f * s, s * 0.28f, false);
    }

    private void spiral(Canvas c, float cx, float cy, float r, boolean mirror) {
        path.reset();
        int n = 40;
        for (int i = 0; i <= n; i++) {
            double th = i * 2.6 * Math.PI / n;
            float rr = r * (1f - (float) i / n * 0.85f);
            float px = (float) Math.cos(th) * rr, py = (float) Math.sin(th) * rr;
            if (mirror) px = -px;
            if (i == 0) path.moveTo(cx + px, cy + py);
            else path.lineTo(cx + px, cy + py);
        }
        c.drawPath(path, stroke);
    }

    private void drawGulls(Canvas c, float w, float h, float t) {
        stroke.setColor(0xFFFFFFFF);
        stroke.setStrokeWidth(2.2f * dp);
        for (int i = 0; i < 3; i++) {
            float x = (float) (((0.2 + i * 0.27 + t * (0.012 + i * 0.004)) % 1.2) - 0.1) * w;
            float y = h * (0.26f + 0.06f * i) + (float) Math.sin(t * 0.7 + i * 2) * h * 0.02f;
            float s = (9 + 3 * i) * dp, flap = (float) Math.sin(t * 5 + i * 1.7) * 0.6f * s;
            path.reset();
            path.moveTo(x - s, y - flap * 0.5f);
            path.quadTo(x - s * 0.5f, y - s * 0.55f - flap, x, y);
            path.quadTo(x + s * 0.5f, y - s * 0.55f - flap, x + s, y - flap * 0.5f);
            c.drawPath(path, stroke);
        }
    }

    // a gust: a white stroke that draws across the sky and ends in a curl, every few seconds
    private void drawWind(Canvas c, float w, float horizon, float t) {
        for (int k = 0; k < 2; k++) {
            float period = 4.5f + k * 1.7f, local = (t + k * 2.3f) % period / period;  // 0..1
            int cycle = (int) ((t + k * 2.3f) / period);
            float y0 = horizon * (0.3f + 0.35f * (float) Math.abs(Math.sin(cycle * 1.9 + k)));
            float x0 = w * (0.05f + 0.3f * (float) Math.abs(Math.cos(cycle * 1.3 + k))), len = w * 0.45f;
            path.reset();
            path.moveTo(x0, y0);
            int n = 24;
            for (int i = 1; i <= n; i++) {
                float f = (float) i / n;
                path.lineTo(x0 + f * len, y0 + (float) Math.sin(f * Math.PI * 2) * horizon * 0.03f);
            }
            // the curl: continues to the right, turns upwards and winds inwards (a shrinking spiral
            // around a centre above the end of the line)
            float ex = x0 + len, r = horizon * 0.045f;
            for (int i = 1; i <= 40; i++) {
                double th = Math.PI / 2 - i * 2.4 * Math.PI / 40;
                float rr = r * (1f - i / 40f * 0.75f);
                path.lineTo(ex + (float) Math.cos(th) * rr, y0 - r + (float) Math.sin(th) * rr);
            }
            measure.setPath(path, false);
            float L = measure.getLength(), head = local * 1.5f * L, tail = head - L * 0.45f;
            seg.reset();
            if (measure.getSegment(Math.max(0, tail), Math.min(L, head), seg, true)) {
                stroke.setColor(0xFFFFFFFF);
                stroke.setAlpha((int) (220 * Math.min(1f, (1.5f - local * 1.5f) * 2)));
                stroke.setStrokeWidth(3 * dp);
                c.drawPath(seg, stroke);
                stroke.setAlpha(255);
            }
        }
    }

    private void drawIsland(Canvas c, float x, float horizon, float s, float t) {
        fill.setColor(0xFF3F9A55);
        path.reset();
        path.moveTo(x - s * 1.6f, horizon + 2);
        path.cubicTo(x - s, horizon - s * 0.9f, x + s * 0.4f, horizon - s * 1.0f, x + s * 1.7f, horizon + 2);
        path.close();
        c.drawPath(path, fill);
        fill.setColor(0xFF2F7D45);
        path.reset();
        path.moveTo(x + s * 0.1f, horizon - s * 0.62f);
        path.cubicTo(x + s * 0.7f, horizon - s * 0.5f, x + s * 1.2f, horizon - s * 0.2f, x + s * 1.7f, horizon + 2);
        path.lineTo(x + s * 0.2f, horizon + 2);
        path.close();
        c.drawPath(path, fill);
        // a palm, swaying a little
        float px = x - s * 0.3f, py = horizon - s * 0.66f, sway = (float) Math.sin(t * 0.9) * s * 0.05f;
        stroke.setColor(0xFF8A5A2E);
        stroke.setStrokeWidth(s * 0.09f);
        path.reset();
        path.moveTo(px, py);
        path.quadTo(px - s * 0.05f, py - s * 0.5f, px + s * 0.12f + sway, py - s * 0.95f);
        c.drawPath(path, stroke);
        fill.setColor(0xFF2E8C3E);
        float tx = px + s * 0.12f + sway, ty = py - s * 0.95f;
        for (int i = 0; i < 5; i++) {
            double a = Math.PI * (1.05 + i * 0.22) + Math.sin(t * 1.3 + i) * 0.05;
            float ex = tx + (float) Math.cos(a) * s * 0.6f, ey = ty + (float) Math.sin(a) * s * 0.35f + s * 0.25f;
            path.reset();
            path.moveTo(tx, ty);
            path.quadTo((tx + ex) / 2, ty - s * 0.22f, ex, ey);
            path.quadTo((tx + ex) / 2, ty - s * 0.05f, tx, ty);
            c.drawPath(path, fill);
        }
    }

    // ---- sea: toon bands, nearer ones bigger and faster, with foam crests
    private float waveY(int layer, float x, float w, float h, float horizon, float t) {
        float band = (h - horizon) / SEA.length;
        float base = horizon + band * layer * (0.55f + 0.12f * layer);
        float amp = (1.5f + layer * 2.2f) * dp, k = (float) (2 * Math.PI / (w * (0.28f + 0.1f * layer)));
        float spd = 0.5f + layer * 0.25f;
        return base + (float) (Math.sin(x * k - t * spd + layer) * amp + Math.sin(x * k * 2.3 + t * spd * 0.7 + layer * 2) * amp * 0.35);
    }

    private void drawSea(Canvas c, float w, float h, float horizon, float t) {
        float step = 6 * dp;
        float boatTarget = progress < 0 ? w * (0.5f + 0.06f * (float) Math.sin(t * 0.25)) : w * (0.1f + 0.8f * progress);
        boatX = boatX < 0 ? boatTarget : boatX + (boatTarget - boatX) * 0.04f;
        int boatLayer = SEA.length - 2;
        for (int layer = 0; layer < SEA.length; layer++) {
            path.reset();
            path.moveTo(0, h);
            for (float x = 0; x <= w + step; x += step) path.lineTo(x, waveY(layer, x, w, h, horizon, t));
            path.lineTo(w, h);
            path.close();
            fill.setColor(SEA[layer]);
            c.drawPath(path, fill);
            if (layer > 0) {  // foam: short white dashes riding the crests
                stroke.setColor(FOAM);
                stroke.setStrokeWidth((1.2f + layer * 0.7f) * dp);
                float wl = w * (0.28f + 0.1f * layer), off = (t * (0.5f + layer * 0.25f) / (float) (2 * Math.PI)) * wl;
                for (float x0 = -wl + (off % wl) + layer * 37 * dp; x0 < w; x0 += wl * 0.5f) {
                    path.reset();
                    float len = wl * 0.16f;
                    for (float x = x0; x <= x0 + len; x += step / 2)
                        if (x == x0) path.moveTo(x, waveY(layer, x, w, h, horizon, t) + 1);
                        else path.lineTo(x, waveY(layer, x, w, h, horizon, t) + 1);
                    c.drawPath(path, stroke);
                }
            }
            if (layer == boatLayer) drawBoat(c, w, h, horizon, t, layer);
        }
    }

    private void drawBoat(Canvas c, float w, float h, float horizon, float t, int layer) {
        float s = Math.min(w, h) * 0.075f;
        float x = boatX, y = waveY(layer, x, w, h, horizon, t);
        float slope = (waveY(layer, x + 4 * dp, w, h, horizon, t) - waveY(layer, x - 4 * dp, w, h, horizon, t)) / (8 * dp);
        float moving = progress < 0 ? 0.3f : 1f;
        // wake
        stroke.setColor(FOAM);
        for (int i = 1; i <= 4; i++) {
            float wx = x - s * (0.9f + i * 0.55f), wy = waveY(layer, wx, w, h, horizon, t);
            stroke.setStrokeWidth((3.2f - i * 0.6f) * dp);
            stroke.setAlpha((int) (230 * moving * (1 - i / 5f)));
            c.drawLine(wx - s * 0.25f, wy + 2, wx + s * 0.2f, wy + 1, stroke);
        }
        stroke.setAlpha(255);
        c.save();
        c.translate(x, y);
        c.rotate((float) Math.toDegrees(Math.atan(slope)) * 0.8f);
        // sail
        fill.setColor(0xFFFFFBF0);
        path.reset();
        path.moveTo(-s * 0.05f, -s * 1.55f);
        path.quadTo(s * 0.75f + (float) Math.sin(t * 1.5) * s * 0.05f, -s * 0.9f, s * 0.05f, -s * 0.28f);
        path.close();
        c.drawPath(path, fill);
        fill.setColor(0xFFD9ECF7);
        path.reset();
        path.moveTo(-s * 0.08f, -s * 1.3f);
        path.quadTo(-s * 0.55f, -s * 0.8f, -s * 0.08f, -s * 0.3f);
        path.close();
        c.drawPath(path, fill);
        // mast and pennant
        stroke.setColor(0xFF6B4423);
        stroke.setStrokeWidth(s * 0.07f);
        c.drawLine(0, -s * 0.2f, 0, -s * 1.65f, stroke);
        fill.setColor(0xFFE0452B);
        path.reset();
        path.moveTo(0, -s * 1.65f);
        path.lineTo(-s * 0.4f, -s * 1.58f + (float) Math.sin(t * 6) * s * 0.04f);
        path.lineTo(0, -s * 1.5f);
        path.close();
        c.drawPath(path, fill);
        // hull
        fill.setColor(0xFFB8452E);
        path.reset();
        path.moveTo(-s * 0.95f, -s * 0.3f);
        path.lineTo(s * 1.05f, -s * 0.38f);
        path.quadTo(s * 0.8f, s * 0.25f, s * 0.1f, s * 0.28f);
        path.quadTo(-s * 0.7f, s * 0.25f, -s * 0.95f, -s * 0.3f);
        path.close();
        c.drawPath(path, fill);
        fill.setColor(0xFF7E2A1C);
        c.drawRect(-s * 0.8f, -s * 0.14f, s * 0.85f, -s * 0.04f, fill);
        c.restore();
    }
}
