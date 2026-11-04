package org.wwhdrecomp.app;

import android.content.Context;
import android.content.SharedPreferences;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.RectF;
import android.os.SystemClock;

import java.util.ArrayDeque;
import java.util.List;

/**
 * Achievements earned while playing (Achievements.check, every two seconds while a file is in play, also with the
 * popup off) and the popup for them (Game > Achievement settings > Pop-up on screen, pref achievement_popup, default on;
 * for what is new in the save game in play, or with achievement_each_file off only what no save game had before): a
 * dark plate below the top of the TV picture with the achievement's icon, "Achievement earned" and its title in the
 * game's font, for a few seconds, one after the other. More than three at once (a file loaded that had reached them
 * already) are one plate with their count. With Link (pref achievement_link, default on) he stands at the plate's left,
 * taller than it, in his item get (GameIcons.LINK_GET: the hop round from behind, then the arm up), and the icon rises
 * over his hand in a golden shine as the game shows a treasure; the plate sits a bit lower for it. The item get's
 * jingle (MenuSounds.ITEM_GET, pref achievement_sound, default on) as he starts to lift it (the game's frame 11), or
 * as the plate comes without him.
 */
final class AchievementPopup {
    private static final long POLL_MS = 2000, FADE_MS = 250, SHOW_MS = 4000;
    // Link: his pictures from GET_FROM, GET_MS each (the game's 36 frames at 30 a second), the icon rising after
    private static final long GET_FROM = 150, GET_MS = 67, RISE_MS = 300, LINK_SHOW_MS = 5000;
    private static final int GET_PICTURES = 18;
    private static final long JINGLE_AT = GET_FROM + GET_MS * 11 / 2;  // (his pictures are every second frame)
    // in his pictures: his raised hand at the end, where his feet are, the middle of where he is over all of them
    private static final float HAND_X = 0.555f, HAND_Y = 0.24f, FEET = 0.945f, MID_X = 0.49f, LINK_SIZE = 1.7f;
    private static final int PLATE = 0xE01B2A4A, INK = 0xFF1B2A4A, FAINT = 0xFFB8C4D8;

    private static final class Shown {
        final String icon, line, title;

        Shown(String icon, String line, String title) {
            this.icon = icon;
            this.line = line;
            this.title = title;
        }
    }

    private final ControlsView view;
    private final SharedPreferences settings;
    private boolean enabled = true, link = true, sound = true, jingled;
    private final ArrayDeque<Shown> queue = new ArrayDeque<>();
    private Shown current;
    private long since;
    private final RectF r = new RectF();
    private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG | Paint.FILTER_BITMAP_FLAG), text = new Paint(Paint.ANTI_ALIAS_FLAG),
            glow = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final android.graphics.Path ray = new android.graphics.Path();

    AchievementPopup(ControlsView v) {
        view = v;
        settings = v.getContext().getSharedPreferences("settings", Context.MODE_PRIVATE);
        text.setFakeBoldText(true);
        v.postDelayed(poll, POLL_MS);
    }

    private final Runnable poll = new Runnable() {
        @Override
        public void run() {
            Context c = view.getContext();
            if (c instanceof MainActivity) NewGames.check((MainActivity) c, Native.progressData());
            List<Achievements.Def> got = Achievements.check(c, settings, settings.getBoolean("achievement_each_file", true));
            if (!got.isEmpty() && enabled) {
                String line = c.getString(R.string.ach_popup);
                if (got.size() > 3) queue.add(new Shown("Triforce_00^l", line, c.getString(R.string.ach_popup_many, got.size())));
                else for (Achievements.Def a : got) queue.add(new Shown(a.icon, line, a.title.of(c)));
                view.invalidate();
            }
            view.postDelayed(this, POLL_MS);
        }
    };

    void setEnabled(boolean on) {
        enabled = on;
        if (!on) {
            queue.clear();
            current = null;
            view.invalidate();
        }
    }

    void setLink(boolean on) {
        link = on;
    }

    void setSound(boolean on) {
        sound = on;
    }

    /** Link's picture at t (ms since the plate came), null while they aren't made yet (then the plain plate) */
    private Bitmap linkAt(long t) {
        int i = (int) Math.max(0, Math.min(GET_PICTURES - 1, (t - GET_FROM) / GET_MS));
        return GameIcons.get(view.getContext(), GameIcons.LINK_GET + i, view::postInvalidate);
    }

    void draw(Canvas c, RectF tv, boolean allowed) {
        if (!allowed) return;  // (the menu open, the layout editor, the GamePad picture in front)
        long now = SystemClock.uptimeMillis();
        if (current == null) {
            current = queue.poll();
            if (current == null) return;
            since = now;
            jingled = false;
        }
        long t = now - since;
        Bitmap lk = link ? linkAt(t) : null;
        long show = lk != null ? LINK_SHOW_MS : SHOW_MS;
        if (!jingled && t >= (lk != null ? JINGLE_AT : 0)) {
            jingled = true;
            if (sound) MenuSounds.play(MenuSounds.ITEM_GET);
        }
        if (t >= show) {
            current = null;
            view.postInvalidateOnAnimation();
            return;
        }
        view.postInvalidateOnAnimation();
        if (tv.isEmpty()) return;
        float a = Math.min(1, Math.min(t, show - t) / (float) FADE_MS);
        int alpha = Math.round(255 * a);
        float h = tv.height() * 0.11f, pad = h * 0.16f, icon = h - 2 * pad;
        float small = h * 0.24f, big = h * 0.36f;
        // the title in the game's font where it has every letter
        Bitmap label = GameFont.covers(current.title) && GameFont.ready(view.getContext(), view::invalidate)
                ? GameFont.label(current.title, big / GameFont.cellHeight(), 0xFFFFFFFF, INK) : null;
        text.setTextSize(big * 0.8f);
        float tw = label != null ? label.getWidth() * (big / label.getHeight()) : text.measureText(current.title);
        text.setTextSize(small);
        float lw = text.measureText(current.line);
        float L = h * LINK_SIZE, slot = lk != null ? L * 0.62f : icon;  // (Link's picture; his room on the plate)
        float w = Math.min(tv.width() * 0.9f, pad * 3 + slot + Math.max(tw, lw));
        float slide = (1 - a) * h * 0.4f;
        float x = tv.centerX() - w / 2, top = tv.top + tv.height() * (lk != null ? 0.19f : 0.11f) - slide;
        r.set(x, top, x + w, top + h);
        paint.setColor(PLATE);
        paint.setAlpha(Math.round((PLATE >>> 24) * a));
        c.drawRoundRect(r, h * 0.22f, h * 0.22f, paint);
        Bitmap b = GameIcons.get(view.getContext(), current.icon, view::postInvalidate);
        paint.setAlpha(alpha);
        if (lk != null) {
            // Link standing on the plate's bottom edge, in the middle of his room
            float lx = x + pad + slot / 2 - MID_X * L, ly = top + h - pad * 0.4f - FEET * L;
            r.set(lx, ly, lx + L, ly + L);
            c.drawBitmap(lk, null, r, paint);
            long up = t - GET_FROM - GET_MS * (GET_PICTURES - 1);  // his arm is up: the icon rises out of his hand
            if (b != null && up > 0) {
                float k = Math.min(1, up / (float) RISE_MS), ease = 1 - (1 - k) * (1 - k) * (1 - k);
                float pop = k < 1 ? ease * (1 + 0.25f * (float) Math.sin(Math.PI * k)) : 1;  // (a little past its size, back)
                float hx = lx + HAND_X * L, hy = ly + HAND_Y * L;
                float size = icon * 0.95f, cy = hy - size * 0.55f * ease;
                drawShine(c, hx, cy, size * (0.75f + 0.25f * ease), up, a * ease);
                float half = size * pop / 2, q = half / Math.max(b.getWidth(), b.getHeight()) * 2;
                r.set(hx - b.getWidth() * q / 2, cy - b.getHeight() * q / 2, hx + b.getWidth() * q / 2, cy + b.getHeight() * q / 2);
                paint.setAlpha(Math.round(255 * a * Math.min(1, k * 2)));
                c.drawBitmap(b, null, r, paint);
                paint.setAlpha(alpha);
            }
        } else if (b != null) {
            float k = icon / Math.max(b.getWidth(), b.getHeight());
            float cx = x + pad + icon / 2, cy = top + h / 2;
            r.set(cx - b.getWidth() * k / 2, cy - b.getHeight() * k / 2, cx + b.getWidth() * k / 2, cy + b.getHeight() * k / 2);
            c.drawBitmap(b, null, r, paint);
        }
        float tx = x + pad * 2 + slot;
        c.save();
        c.clipRect(tx, top, x + w - pad, top + h);
        text.setTextSize(small);
        text.setColor(FAINT);
        text.setAlpha(alpha);
        c.drawText(current.line, tx, top + pad + small * 0.85f, text);
        float ty = top + h - pad - big;
        if (label != null) {
            r.set(tx, ty, tx + tw, ty + big);
            c.drawBitmap(label, null, r, paint);
        } else {
            text.setTextSize(big * 0.8f);
            text.setColor(0xFFFFFFFF);
            text.setAlpha(alpha);
            c.drawText(current.title, tx, ty + big * 0.8f, text);
        }
        c.restore();
    }

    /** the golden shine behind a treasure held up: a soft glow and slowly turning rays (r: the icon's size) */
    private void drawShine(Canvas c, float cx, float cy, float r, long t, float a) {
        int al = Math.round(255 * Math.max(0, Math.min(1, a)));
        glow.setShader(new android.graphics.RadialGradient(cx, cy, r * 0.95f, new int[] {0xC0FFF3B0, 0x60FFD24A, 0x00FFC830},
                new float[] {0, 0.55f, 1}, android.graphics.Shader.TileMode.CLAMP));
        glow.setAlpha(al);
        c.drawCircle(cx, cy, r * 0.95f, glow);
        glow.setShader(null);
        glow.setColor(0xFFFFF0A0);
        glow.setAlpha(Math.round(al * 0.45f));
        float turn = t * 0.04f;  // degrees
        for (int i = 0; i < 8; i++) {
            double d = Math.toRadians(turn + i * 45), w = Math.toRadians(7);
            float len = r * (i % 2 == 0 ? 1.05f : 0.8f);
            ray.rewind();
            ray.moveTo(cx, cy);
            ray.lineTo(cx + (float) Math.cos(d - w) * len, cy + (float) Math.sin(d - w) * len);
            ray.lineTo(cx + (float) Math.cos(d + w) * len, cy + (float) Math.sin(d + w) * len);
            ray.close();
            c.drawPath(ray, glow);
        }
    }
}
