package org.wwhdrecomp.app;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.RectF;
import android.view.Choreographer;
import android.view.View;

/**
 * Online co-op's marks over the game (upstream's overlay layers: the sea radar, the emote bubbles, the PvP marks).
 * The native side records them as shapes (Native.coopOverlay, runtime/src/online/compat/compat.cpp) in pixels of the
 * TV picture; this view draws them over that picture's place on the screen, about 30 times a second while online.
 */
final class CoopOverlay extends View {
    private static final int F = 15;  // floats a shape
    private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Path path = new Path();
    private final RectF tv = new RectF();
    private float[] shapes = new float[0];
    private String[] texts = new String[0];
    private boolean running;
    private long lastFrame;

    CoopOverlay(Context c) {
        super(c);
        setWillNotDraw(false);
        setClickable(false);
        setFocusable(false);
        paint.setTypeface(android.graphics.Typeface.create("sans-serif-medium", android.graphics.Typeface.NORMAL));
    }

    /** The TV picture's place in this view (width < 0: not shown). */
    void setPicture(RectF r) {
        tv.set(r);
        invalidate();
    }

    private final Choreographer.FrameCallback tick = new Choreographer.FrameCallback() {
        @Override
        public void doFrame(long now) {
            if (!running) return;
            if (now - lastFrame >= 30_000_000L) {  // (~30 Hz: the game's own step)
                lastFrame = now;
                float w = tv.width(), h = tv.height();
                float[] s = w > 0 && h > 0 ? Native.coopOverlay(w, h) : new float[0];
                String[] t = s.length > 0 ? Native.coopOverlayTexts() : new String[0];
                if (s.length != 0 || shapes.length != 0) {
                    shapes = s;
                    texts = t;
                    invalidate();
                }
            }
            Choreographer.getInstance().postFrameCallback(this);
        }
    };

    /** Drawn while online (the app's poll). */
    void setRunning(boolean on) {
        if (on == running) return;
        running = on;
        if (on) Choreographer.getInstance().postFrameCallback(tick);
        else {
            shapes = new float[0];
            invalidate();
        }
    }

    private void color(int i) {
        paint.setARGB((int) shapes[i + 4], (int) shapes[i + 1], (int) shapes[i + 2], (int) shapes[i + 3]);
    }

    @Override
    protected void onDraw(Canvas c) {
        if (shapes.length == 0 || tv.width() <= 0) return;
        c.save();
        c.translate(tv.left, tv.top);
        c.clipRect(0, 0, tv.width(), tv.height());
        int clips = 0;
        for (int i = 0; i + F <= shapes.length; i += F) {
            int kind = (int) shapes[i];
            float x0 = shapes[i + 5], y0 = shapes[i + 6], x1 = shapes[i + 7], y1 = shapes[i + 8];
            float x2 = shapes[i + 9], y2 = shapes[i + 10], x3 = shapes[i + 11], y3 = shapes[i + 12], size = shapes[i + 13];
            color(i);
            switch (kind) {
                case 1:  // line
                    paint.setStyle(Paint.Style.STROKE);
                    paint.setStrokeWidth(size);
                    c.drawLine(x0, y0, x1, y1, paint);
                    break;
                case 2:  // rect (x2: rounding)
                case 3:
                    paint.setStyle(kind == 2 ? Paint.Style.STROKE : Paint.Style.FILL);
                    paint.setStrokeWidth(size);
                    c.drawRoundRect(x0, y0, x1, y1, x2, x2, paint);
                    break;
                case 4:  // quad
                case 5:
                    path.reset();
                    path.moveTo(x0, y0);
                    path.lineTo(x1, y1);
                    path.lineTo(x2, y2);
                    path.lineTo(x3, y3);
                    path.close();
                    paint.setStyle(kind == 4 ? Paint.Style.STROKE : Paint.Style.FILL);
                    paint.setStrokeWidth(size);
                    c.drawPath(path, paint);
                    break;
                case 6:  // triangle
                    path.reset();
                    path.moveTo(x0, y0);
                    path.lineTo(x1, y1);
                    path.lineTo(x2, y2);
                    path.close();
                    paint.setStyle(Paint.Style.FILL);
                    c.drawPath(path, paint);
                    break;
                case 7:  // circle (x1: radius)
                case 8:
                    paint.setStyle(kind == 7 ? Paint.Style.STROKE : Paint.Style.FILL);
                    paint.setStrokeWidth(size);
                    c.drawCircle(x0, y0, x1, paint);
                    break;
                case 9: {  // text at its top left, size: its height
                    int t = (int) shapes[i + 14];
                    if (t < 0 || t >= texts.length) break;
                    paint.setStyle(Paint.Style.FILL);
                    paint.setTextSize(size * 0.9f);
                    c.drawText(texts[t], x0, y0 + size * 0.8f, paint);
                    break;
                }
                case 10:  // clip push
                    c.save();
                    c.clipRect(x0, y0, x1, y1);
                    clips++;
                    break;
                case 11:  // clip pop
                    if (clips > 0) {
                        c.restore();
                        clips--;
                    }
                    break;
                default:
                    break;
            }
        }
        while (clips-- > 0) c.restore();
        c.restore();
    }
}
