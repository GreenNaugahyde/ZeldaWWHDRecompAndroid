package org.wwhdrecomp.app;

import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.RectF;
import android.os.SystemClock;
import android.view.View;

/**
 * Singing the songs (SongListener): a take as a timeline under the game's own time bar and note row while Link
 * conducts by voice, in the game's conducting look. The game's message window as the panel; a lane per note of the
 * time (its C-stick direction's sign from BatonSign_00 at the left, lowest pitch at the bottom, the note sent now
 * lit); the time bar (BatonInput_00) along the bottom, a dot per beat. Now is a fixed yellow line under the middle
 * of the game's time bar (NOW_X): the timeline scrolls left under it, the beats ahead come from the right, so a note
 * is taken as its beat crosses the line. The pitch heard as a line over the lanes (yellow where it is on a note); at
 * each beat the game took a note (the moment its judge reads the C stick) a mark down the lanes and the game's sign
 * of the note taken. The count-in's two bars are numbered on the bar; in the first the singer hums their middle note
 * (shown on its own line, the voice drawn against it), the second is a breath. Practice: a title, the result, and a
 * close mark.
 * Placed in the game's layout units (1280 x 720 around the TV picture), like GameHint.
 */
final class SongPanel {
    private static final int YELLOW = 0xFFFFE100, INK = 0xFF1B2A4A;
    // the panel (layout units from the picture's top / centre): under the game's note row (signs at 115 +- 48)
    private static final float TOP = 172, BOTTOM = 396, HALF_W = 390;
    /** how far the 3D picture's centre moves down while Link conducts (layout units): his face, about 40
     *  above the centre in the game's conducting view, lands just below the panel (zoomed out 1.5x) */
    static final float VIEW_SHIFT = 130;
    private static final float LANE_TOP = 220, LANE_BOTTOM = 328, BAR_Y = 358, BAR_H = 22, SIGN = 28;
    private static final float X0 = -300, X1 = 352, TITLE_Y = 194, TEXT = 26;
    private static final float NOW_X = 0;  // now: under the middle of the game's time bar (centred on the picture)

    private final View host;
    private final Paint img = new Paint(Paint.FILTER_BITMAP_FLAG | Paint.ANTI_ALIAS_FLAG);
    private final Paint fill = new Paint(Paint.ANTI_ALIAS_FLAG), line = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint text = new Paint(Paint.FILTER_BITMAP_FLAG | Paint.ANTI_ALIAS_FLAG);
    private final RectF dst = new RectF(), closeRect = new RectF();
    private final Path path = new Path();
    private final float[] ys = new float[2];
    private float k, cx, top;  // the layout's unit, the picture's centre x and top

    SongPanel(View host) {
        this.host = host;
        line.setStyle(Paint.Style.STROKE);
        line.setStrokeCap(Paint.Cap.ROUND);
        line.setStrokeJoin(Paint.Join.ROUND);
    }

    private float x(float lx) { return cx + lx * k; }

    private float y(float ly) { return top + ly * k; }

    /** the practice panel's close mark was hit */
    boolean hitsClose(float px, float py) {
        return !closeRect.isEmpty() && closeRect.contains(px, py);
    }

    void draw(Canvas c, RectF tv, SongListener.Take t, boolean practice) {
        closeRect.setEmpty();
        if (tv.isEmpty() || t == null && !practice) return;
        k = tv.height() / 720f;
        cx = tv.centerX();
        top = tv.top;
        Runnable redraw = () -> host.post(host::invalidate);
        long now = SystemClock.uptimeMillis();
        // the panel: the game's message window
        Bitmap win = GameIcons.get(host.getContext(), "MsgWindowMain_00^t", redraw);
        dst.set(x(-HALF_W), y(TOP), x(HALF_W), y(BOTTOM));
        if (win != null) {
            img.setAlpha(205);
            c.drawBitmap(win, null, dst, img);
        } else {
            fill.setColor(0xC0000000);
            c.drawRoundRect(dst, 18 * k, 18 * k, fill);
        }
        int beat = t != null ? t.beat : MainActivity.instance != null ? MainActivity.instance.prefs.getInt("sing_beat", 3) : 3;
        if (beat != 4 && beat != 6) beat = 3;
        int beatMs = beat == 4 ? 857 : beat == 6 ? 750 : 1000;
        int[] scale = SongMatcher.scale(beat, t != null && t.alt);
        Integer[] order = {0, 1, 2, 3, 4};  // the lanes by pitch, lowest first
        int[] sc = scale;
        java.util.Arrays.sort(order, (p, q) -> Integer.compare(sc[p], sc[q]));
        float[] laneY = new float[5];
        for (int r = 0; r < 5; r++) laneY[order[r]] = LANE_BOTTOM - r * (LANE_BOTTOM - LANE_TOP) / 4f;
        // the key the voice is drawn in: the notes' own once there are any, in the count-in the middle note's
        float key = t == null ? Float.NaN : !Float.isNaN(t.key) ? t.key : t.ref - SongMatcher.middle(scale);
        boolean keyed = !Float.isNaN(key);
        int lit = t != null && !Float.isNaN(t.key) && t.end < 0 ? t.dir : -1;

        // the time shown: now (where the take ended, once it has) at NOW_X, two bars and a half beat across
        float perMs = (X1 - X0) / (beat * 2f * beatMs + beatMs / 2f);
        long at = t == null ? now : t.end >= 0 ? t.end : now;
        long from = at - (long) ((NOW_X - X0) / perMs), to = at + (long) ((X1 - NOW_X) / perMs);
        java.util.function.LongToDoubleFunction tx = tt -> x(NOW_X + (tt - at) * perMs);

        // lanes: a thin bar each, the sent note's lit
        for (int d = 0; d < 5; d++) {
            float ly = y(laneY[d]);
            fill.setColor(d == lit ? 0x48FFE100 : 0x30FFFFFF);
            dst.set(x(X0 - 14), ly - 2.2f * k, x(X1 + 6), ly + 2.2f * k);
            c.drawRoundRect(dst, 2.2f * k, 2.2f * k, fill);
        }
        // the time bar along the bottom
        Bitmap bar = GameIcons.get(host.getContext(), "#batonbar", redraw);
        dst.set(x(X0 - 46), y(BAR_Y - BAR_H / 2), x(X1 + 22), y(BAR_Y + BAR_H / 2));
        if (bar != null) {
            img.setAlpha(255);
            c.drawBitmap(bar, null, dst, img);
        } else {
            fill.setColor(0xDC000000);
            c.drawRoundRect(dst, BAR_H / 2 * k, BAR_H / 2 * k, fill);
        }
        c.save();
        c.clipRect(x(X0 - 18), y(TOP), x(X1 + 14), y(BOTTOM));
        if (t != null) {
            long countEnd = t.countEnd(), humEnd = t.start + (long) t.beat * t.beatMs;
            // the count-in: a darker stretch, its beats numbered on the bar (1 2 3 1 2 3)
            fill.setColor(0x38000000);
            dst.set((float) tx.applyAsDouble(t.start - t.beatMs / 2), y(LANE_TOP - 16), (float) tx.applyAsDouble(countEnd - t.beatMs / 2), y(LANE_BOTTOM + 16));
            c.drawRect(dst, fill);
            for (int i = 0; i < SongListener.COUNT_BARS * t.beat; i++) {
                long bt = t.start + (long) i * t.beatMs;
                boolean past = at >= bt;
                drawNumber(c, Integer.toString(i % t.beat + 1), (float) tx.applyAsDouble(bt), y(BAR_Y), past ? YELLOW : 0xFFB0B0B0, past && at - bt < 250 ? 1 + 0.4f * (1 - (at - bt) / 250f) : 1, redraw);
            }
            // the first bar: the middle note's line (where the hummed note is drawn once it is known)
            pitchY(SongMatcher.middle(scale), scale, laneY, ys);
            float my = y(ys[0]);
            line.setColor(0x90FFE100);
            line.setStrokeWidth(2.5f * k);
            float ma = (float) tx.applyAsDouble(t.start - t.beatMs / 2), mb = (float) tx.applyAsDouble(humEnd - t.beatMs / 2);
            for (float dx = ma; dx < mb; dx += 14 * k) c.drawLine(dx, my, Math.min(mb, dx + 7 * k), my, line);
            // the beats ahead: dots on the bar where the game will take the next notes
            int bn = t.bn;
            long next = bn > 0 ? t.bt[bn - 1] + t.beatMs : countEnd;
            Bitmap dot = GameIcons.get(host.getContext(), "#batonpoint:ffffff", redraw);
            for (long bt = next; bt < to + t.beatMs; bt += t.beatMs) {
                if (t.end >= 0 && bt > t.end) break;
                float bx = (float) tx.applyAsDouble(bt);
                line.setColor(0x40FFFFFF);
                line.setStrokeWidth(1.5f * k);
                c.drawLine(bx, y(LANE_TOP - 12), bx, y(LANE_BOTTOM + 12), line);
                drawPoint(c, dot, bx, y(BAR_Y), 7 * k, 200);
            }
            // the pitch heard
            if (keyed) drawPitch(c, t, key, scale, laneY, tx, from, at);
            // the beats the game took: a mark down the lanes, its sign on the note taken
            for (int i = 0; i < bn; i++) {
                long bt = t.bt[i];
                float bx = (float) tx.applyAsDouble(bt);
                line.setColor(0xA0FFFFFF);
                line.setStrokeWidth(2.5f * k);
                c.drawLine(bx, y(LANE_TOP - 14), bx, y(LANE_BOTTOM + 14), line);
                drawPoint(c, dot, bx, y(BAR_Y), 9 * k, 255);
                int d = t.bd[i];
                if (d < 0) continue;
                float pop = Math.max(0, 1 - (now - bt) / 220f);
                drawSign(c, d, true, bx, y(laneY[d]), SIGN * 1.3f * k * (1 + 0.45f * pop), redraw);
            }
        }
        c.restore();
        // now: a line under the middle of the game's time bar (below the title row), the input point on the bar
        float px = x(NOW_X);
        boolean live = t != null && t.end < 0;
        line.setColor(0x60000000);
        line.setStrokeWidth(5.5f * k);
        c.drawLine(px, y(LANE_TOP - 18), px, y(BAR_Y), line);
        line.setColor(live ? 0xE0FFE100 : 0x70FFE100);
        line.setStrokeWidth(3 * k);
        c.drawLine(px, y(LANE_TOP - 18), px, y(BAR_Y), line);
        drawPoint(c, GameIcons.get(host.getContext(), "#batonpoint:ffe100", redraw), px, y(BAR_Y), 12 * k, live || t == null ? 255 : 150);
        // the signs at the lanes' left, the note sent now lit
        for (int d = 0; d < 5; d++) drawSign(c, d, d == lit, x(X0 - 40), y(laneY[d]), SIGN * k * (d == lit ? 1.12f : 1f), redraw);
        // the title line: practice (and its close mark), the result
        String title = null;
        if (practice) title = host.getContext().getString(R.string.song_practice_title);
        String result = t != null ? t.result : null;
        if (practice && t == null) result = host.getContext().getString(R.string.song_practice_go);
        if (t != null && t.end < 0 && now < t.countEnd())  // the count-in: what to do now
            result = host.getContext().getString(now < t.start + (long) t.beat * t.beatMs ? R.string.song_count_hum : R.string.song_count_breathe);
        if (title != null) drawText(c, title, x(-HALF_W + 50), y(TITLE_Y), 0xFFFF7A2E, false, redraw);
        if (result != null) drawText(c, result, x(0), y(TITLE_Y), t != null && t.found ? YELLOW : 0xFFFFFFFF, true, redraw);
        if (practice) {
            float r = 15 * k, ccx = x(HALF_W - 48), ccy = y(TITLE_Y);
            line.setColor(0xFFFFFFFF);
            line.setStrokeWidth(3.2f * k);
            c.drawLine(ccx - r * 0.6f, ccy - r * 0.6f, ccx + r * 0.6f, ccy + r * 0.6f, line);
            c.drawLine(ccx - r * 0.6f, ccy + r * 0.6f, ccx + r * 0.6f, ccy - r * 0.6f, line);
            closeRect.set(ccx - 3 * r, ccy - 2.4f * r, ccx + 3 * r, ccy + 2.4f * r);
        }
        if (t != null && (t.end < 0 || now - t.end < 300)) host.postInvalidateOnAnimation();
    }

    /** the pitch line: y between the lanes by pitch (the singer's pitch minus the key), yellow where it is on a
     *  note, white between; a gap where nothing was heard */
    private void drawPitch(Canvas c, SongListener.Take t, float key, int[] scale, float[] laneY, java.util.function.LongToDoubleFunction tx, long from, long at) {
        int n = t.pn, first = Math.max(0, n - SongListener.Take.SAMPLES + 1);
        float lastX = Float.NaN, lastY = 0;
        long lastT = 0;
        boolean lastOn = false;
        float[] ys = new float[2];
        for (int i = first; i < n; i++) {
            long tt = t.pt[i % SongListener.Take.SAMPLES];
            float p = t.pp[i % SongListener.Take.SAMPLES];
            if (tt < from || tt > at || tt < t.start) {
                lastX = Float.NaN;
                continue;
            }
            if (Float.isNaN(p)) {
                lastX = Float.NaN;
                continue;
            }
            p -= key;
            float near = Float.MAX_VALUE;
            for (int s : scale) near = Math.min(near, Math.abs(p - s));
            boolean on = near < 0.8f;
            pitchY(p, scale, laneY, ys);
            float px = (float) tx.applyAsDouble(tt), py = y(ys[0]);
            if (!Float.isNaN(lastX) && tt - lastT <= 80) {
                line.setColor(0x90000000);  // a dark edge (bright scenes)
                line.setStrokeWidth(8.5f * k);
                c.drawLine(lastX, lastY, px, py, line);
                line.setColor(on && lastOn ? YELLOW : 0xFFFFFFFF);
                line.setStrokeWidth(5 * k);
                c.drawLine(lastX, lastY, px, py, line);
            }
            lastX = px;
            lastY = py;
            lastT = tt;
            lastOn = on;
        }
        if (!Float.isNaN(lastX) && t.end < 0 && at - lastT < 120) {  // the voice now: a glowing point
            fill.setColor(lastOn ? 0x60FFE100 : 0x50FFFFFF);
            c.drawCircle(lastX, lastY, 11 * k, fill);
            fill.setColor(lastOn ? YELLOW : 0xFFFFFFFF);
            c.drawCircle(lastX, lastY, 5.5f * k, fill);
        }
    }

    /** a pitch (in the scale's key) to the lanes' y: linear between neighbouring lanes, beyond them a lane per
     *  4 semitones, kept within the panel */
    private static void pitchY(float p, int[] scale, float[] laneY, float[] out) {
        int[] idx = {0, 1, 2, 3, 4};
        int lo = -1, hi = -1;
        for (int d : idx) {
            if (scale[d] <= p && (lo < 0 || scale[d] > scale[lo])) lo = d;
            if (scale[d] >= p && (hi < 0 || scale[d] < scale[hi])) hi = d;
        }
        float step = (LANE_BOTTOM - LANE_TOP) / 4f;
        float y;
        if (lo < 0) y = laneY[hi] + (scale[hi] - p) / 4f * step;
        else if (hi < 0) y = laneY[lo] - (p - scale[lo]) / 4f * step;
        else if (scale[hi] == scale[lo]) y = laneY[lo];
        else y = laneY[lo] + (laneY[hi] - laneY[lo]) * (p - scale[lo]) / (scale[hi] - scale[lo]);
        out[0] = Math.max(LANE_TOP - 20, Math.min(LANE_BOTTOM + 18, y));
    }

    private void drawSign(Canvas c, int d, boolean lit, float px, float py, float size, Runnable redraw) {
        Bitmap b = GameIcons.get(host.getContext(), "#baton:" + d + ":" + (lit ? 1 : 0), redraw);
        if (b == null) {
            fill.setColor(lit ? YELLOW : 0xFFB8B8B8);
            c.drawCircle(px, py, size * 0.25f, fill);
            return;
        }
        dst.set(px - size / 2, py - size / 2, px + size / 2, py + size / 2);
        img.setAlpha(255);
        c.drawBitmap(b, null, dst, img);
    }

    private void drawPoint(Canvas c, Bitmap b, float px, float py, float r, int alpha) {
        if (b == null) {
            fill.setColor((alpha << 24) | 0xFFFFFF);
            c.drawCircle(px, py, r * 0.7f, fill);
            return;
        }
        float w = r * b.getWidth() / b.getHeight();
        dst.set(px - w, py - r, px + w, py + r);
        img.setAlpha(alpha);
        c.drawBitmap(b, null, dst, img);
    }

    /** a count-in number on the time bar, in the game's font */
    private void drawNumber(Canvas c, String s, float px, float py, int color, float grow, Runnable redraw) {
        float h = 30 * k * grow;
        if (GameFont.ready(host.getContext(), redraw)) {
            Bitmap b = GameFont.label(s, 30 * k / GameFont.cellHeight(), color, INK);
            float bw = b.getWidth() * grow, bh = b.getHeight() * grow;
            dst.set(px - bw / 2, py - bh / 2, px + bw / 2, py + bh / 2);
            img.setAlpha(255);
            c.drawBitmap(b, null, dst, img);
        } else {
            text.setTextSize(h * 0.8f);
            text.setColor(color);
            text.setTextAlign(Paint.Align.CENTER);
            c.drawText(s, px, py - (text.descent() + text.ascent()) / 2, text);
        }
    }

    /** a line of text in the game's message font (as its message windows), centred or from the left */
    private void drawText(Canvas c, String s, float px, float py, int color, boolean centred, Runnable redraw) {
        if (!GameFont.MSG.isReady(host.getContext(), redraw)) {
            text.setTextSize(TEXT * k * 0.8f);
            text.setColor(color);
            text.setTextAlign(centred ? Paint.Align.CENTER : Paint.Align.LEFT);
            c.drawText(s, px, py - (text.descent() + text.ascent()) / 2, text);
            return;
        }
        float scale = TEXT * k / GameFont.MSG.height(), w = 0;
        for (int i = 0; i < s.length(); i++) w += GameFont.MSG.advance(s.charAt(i)) * scale;
        float x = centred ? px - w / 2 : px, yy = py - TEXT * k / 2;
        for (int i = 0; i < s.length(); i++) {
            char ch = s.charAt(i);
            text.setColor(0x90000000);
            GameFont.MSG.draw(c, ch, x + 2 * k, yy + 2 * k, scale, text);
            text.setColor(color);
            x += GameFont.MSG.draw(c, ch, x, yy, scale, text);
        }
    }
}
