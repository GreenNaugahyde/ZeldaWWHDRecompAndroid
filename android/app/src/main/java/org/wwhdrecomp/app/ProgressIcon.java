package org.wwhdrecomp.app;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.ColorMatrix;
import android.graphics.ColorMatrixColorFilter;
import android.graphics.Paint;
import android.graphics.RectF;
import android.view.View;

/**
 * Icons of a game progress checklist line (OptionsMenu's Game progress page): the game's own textures (GameIcons), side
 * by side in square cells; what is still missing grey and faint. CHECK is the GamePad map's check mark (MapCheck_00,
 * white with a black edge) in the menus' green.
 */
final class ProgressIcon extends View {
    static final String CHECK = "MapCheck_00^t";
    private static final ColorMatrixColorFilter GREY;
    static {
        ColorMatrix m = new ColorMatrix();
        m.setSaturation(0);
        GREY = new ColorMatrixColorFilter(m);
    }
    private static final ColorMatrixColorFilter GREEN = GameUi.GameTabPlate.lerpFilter(GameUi.GREEN.dark, GameUi.GREEN.light);

    private final String[] textures;
    private final boolean[] got;
    private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG | Paint.FILTER_BITMAP_FLAG);
    private final RectF r = new RectF();

    ProgressIcon(Context c, String[] textures, boolean[] got) {
        super(c);
        this.textures = textures;
        this.got = got;
    }

    /** how full a part is: a dark groove with the menus' green in it, as the magic meter */
    static final class Bar extends View {
        private final float fraction;
        private final Paint p = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final RectF r = new RectF();

        Bar(Context c, float fraction) {
            super(c);
            this.fraction = Math.max(0, Math.min(1, fraction));
        }

        @Override
        protected void onDraw(Canvas c) {
            float h = getHeight(), rad = h / 2;
            r.set(0, 0, getWidth(), h);
            p.setShader(null);
            p.setStyle(Paint.Style.FILL);
            p.setColor(0x66000000);
            c.drawRoundRect(r, rad, rad, p);
            if (fraction > 0) {
                float in = Math.max(1, h * 0.16f);
                r.set(in, in, in + (getWidth() - 2 * in) * fraction, h - in);
                if (r.width() < r.height()) r.right = r.left + r.height();
                p.setShader(new android.graphics.LinearGradient(0, r.top, 0, r.bottom, GameUi.GREEN.light, GameUi.GREEN.mid,
                        android.graphics.Shader.TileMode.CLAMP));
                c.drawRoundRect(r, rad - in, rad - in, p);
                p.setShader(null);
                p.setStyle(Paint.Style.STROKE);
                p.setStrokeWidth(Math.max(1, h * 0.08f));
                p.setColor(GameUi.GREEN.dark);
                c.drawRoundRect(r, rad - in, rad - in, p);
            }
        }
    }

    @Override
    protected void onDraw(Canvas c) {
        float cell = Math.min(getHeight(), getWidth() / (float) Math.max(1, textures.length));
        float x0 = (getWidth() - cell * textures.length) / 2f;
        for (int i = 0; i < textures.length; i++) {
            if (textures[i] == null) continue;
            Bitmap b = GameIcons.get(getContext(), textures[i], () -> post(this::invalidate));
            if (b == null) continue;
            float k = cell * 0.9f / Math.max(b.getWidth(), b.getHeight()), w = b.getWidth() * k, h = b.getHeight() * k;
            float cx = x0 + cell * (i + 0.5f), cy = getHeight() / 2f;
            r.set(cx - w / 2, cy - h / 2, cx + w / 2, cy + h / 2);
            boolean on = got == null || got[i];
            paint.setColorFilter(CHECK.equals(textures[i]) ? GREEN : on ? null : GREY);
            paint.setAlpha(on ? 255 : 80);
            c.drawBitmap(b, null, r, paint);
        }
    }
}
