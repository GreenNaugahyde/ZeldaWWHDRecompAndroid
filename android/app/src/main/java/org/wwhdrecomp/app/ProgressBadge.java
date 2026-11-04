package org.wwhdrecomp.app;

import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.RectF;
import android.os.SystemClock;

/**
 * The game progress in percent on the TV picture (Game > Game progress > Percent on screen, pref progress_hud): the
 * Triforce and "73 %" in the game's font at the top centre, as the HUD's counters. Shown in play as the mini map is
 * (not in events, while Link aims or looks, in the pause menu, in fades: Native.miniMapState); the count is read every
 * two seconds (GameProgress). A tap on it opens the checklist (ControlsView, MainActivity.openProgress).
 */
final class ProgressBadge {
    private static final long FADE_MS = 200, POLL_MS = 250, COUNT_MS = 2000;
    private static final int INK = 0xFF1B2A4A;  // the HUD's dark outline (ControlsView GAME_INK)

    private final ControlsView view;
    private boolean enabled, shown;
    private int percent = -1;
    private long countedAt;
    private float alpha;
    private long alphaAt;
    private final RectF bounds = new RectF(), r = new RectF();
    private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG | Paint.FILTER_BITMAP_FLAG), text = new Paint(Paint.ANTI_ALIAS_FLAG);

    ProgressBadge(ControlsView v) {
        view = v;
        text.setFakeBoldText(true);
    }

    private final Runnable poll = new Runnable() {
        @Override
        public void run() {
            if (!enabled) return;
            float[] s = Native.miniMapState();
            boolean show = s[0] != 0 && s[9] == 0 && s[10] == 0 && s[11] == 0;
            long now = SystemClock.uptimeMillis();
            int p = percent;
            if (s[0] == 0) p = -1;
            else if (now - countedAt >= COUNT_MS) {
                countedAt = now;
                GameProgress.Result res = GameProgress.read(GameProgress.NAMELESS);
                p = res == null ? -1 : res.percent();
            }
            if (show != shown || p != percent) {
                shown = show;
                percent = p;
                view.invalidate();
            }
            view.postDelayed(this, POLL_MS);
        }
    };

    void setEnabled(boolean on) {
        if (on == enabled) return;
        enabled = on;
        view.removeCallbacks(poll);
        countedAt = 0;
        if (on) view.post(poll);
        else {
            shown = false;
            alpha = 0;
        }
        view.invalidate();
    }

    /** where it was last drawn (empty while hidden) */
    RectF bounds() { return bounds; }

    void draw(Canvas c, RectF tv, boolean allowed) {
        boolean w = enabled && allowed && shown && percent >= 0 && !tv.isEmpty();
        long now = SystemClock.uptimeMillis();
        if (alphaAt == 0) alphaAt = now;
        float dt = (now - alphaAt) / (float) FADE_MS;
        alphaAt = now;
        alpha = w ? Math.min(1, alpha + dt) : Math.max(0, alpha - dt);
        if (w ? alpha < 1 : alpha > 0) view.postInvalidateOnAnimation();
        bounds.setEmpty();
        if (alpha <= 0 || percent < 0) {
            if (!w) alphaAt = 0;
            return;
        }
        int a = Math.round(255 * alpha);
        float h = tv.height() * 0.06f, top = tv.top + tv.height() * 0.025f;
        String t = percent + " %";
        Bitmap tri = GameIcons.get(view.getContext(), "Triforce_00^l", view::postInvalidate);
        float icon = h * 0.95f, gap = h * 0.18f;
        Bitmap label = GameFont.covers(t) && GameFont.ready(view.getContext(), view::invalidate)
                ? GameFont.label(t, h / GameFont.cellHeight(), 0xFFFFFFFF, INK) : null;
        float tw;
        if (label != null) tw = label.getWidth() * (h / label.getHeight());
        else {
            text.setTextSize(h * 0.8f);
            tw = text.measureText(t);
        }
        float total = icon + gap + tw, x = tv.centerX() - total / 2;
        bounds.set(x - h * 0.3f, top - h * 0.2f, x + total + h * 0.3f, top + h * 1.2f);
        paint.setAlpha(a);
        if (tri != null) {
            float k = icon / Math.max(tri.getWidth(), tri.getHeight());
            r.set(x, top + (h - tri.getHeight() * k) / 2, x + tri.getWidth() * k, top + (h + tri.getHeight() * k) / 2);
            c.drawBitmap(tri, null, r, paint);
        }
        x += icon + gap;
        if (label != null) {
            r.set(x, top, x + tw, top + h);
            c.drawBitmap(label, null, r, paint);
        } else {
            text.setStyle(Paint.Style.STROKE);
            text.setStrokeWidth(h * 0.12f);
            text.setColor(INK);
            text.setAlpha(a);
            float base = top + h / 2 - (text.descent() + text.ascent()) / 2;
            c.drawText(t, x, base, text);
            text.setStyle(Paint.Style.FILL);
            text.setColor(0xFFFFFFFF);
            text.setAlpha(a);
            c.drawText(t, x, base, text);
        }
    }
}
