package org.wwhdrecomp.app;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.Typeface;

/** text in the game's fonts on a Canvas (PadScreen, LootMenu); the system's font until they are here */
final class GameDraw {
    private final Context context;
    private final Runnable redraw;
    private final Paint text = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint glyphs = new Paint(Paint.ANTI_ALIAS_FLAG | Paint.FILTER_BITMAP_FLAG);
    private final Paint bmp = new Paint(Paint.ANTI_ALIAS_FLAG | Paint.FILTER_BITMAP_FLAG);

    GameDraw(Context context, Runnable redraw) {
        this.context = context;
        this.redraw = redraw;
        text.setTypeface(Typeface.create("sans-serif-medium", Typeface.NORMAL));
    }

    /** text in the game's text font (CKingMsg), h its cell height in pixels, centred on x (align 0), from x (-1) or
     *  ending at x (1), its cell's middle at y */
    float msg(Canvas c, String t, float x, float y, float h, int color, int align) {
        GameFont f = GameFont.MSG;
        if (!f.isReady(context, redraw)) {
            text.setColor(color);
            text.setTextSize(h * 0.62f);
            text.setTextAlign(align == 0 ? Paint.Align.CENTER : align < 0 ? Paint.Align.LEFT : Paint.Align.RIGHT);
            c.drawText(t, x, y + h * 0.22f, text);
            return text.measureText(t);
        }
        float k = h / f.height(), w = msgWidth(t, h);
        float left = align == 0 ? x - w / 2 : align < 0 ? x : x - w, top = y - h / 2;
        glyphs.setColor(color);
        text.setColor(color);
        text.setTextAlign(Paint.Align.LEFT);
        for (int i = 0; i < t.length(); i++) {
            char ch = t.charAt(i);
            if (f.has(ch)) left += f.draw(c, ch, left, top, k, glyphs);
            else {
                String one = String.valueOf(ch);
                c.drawText(one, left, top + f.baselinePx() * k, text);
                left += text.measureText(one);
            }
        }
        return w;
    }

    /** the width msg would take */
    float msgWidth(String t, float h) {
        GameFont f = GameFont.MSG;
        text.setTextSize(h * 0.62f);
        if (!f.isReady(context, redraw)) return text.measureText(t);
        float k = h / f.height(), w = 0;
        for (int i = 0; i < t.length(); i++) w += f.has(t.charAt(i)) ? f.advance(t.charAt(i)) * k : text.measureText(String.valueOf(t.charAt(i)));
        return w;
    }

    /** a label in the game's title font with its outline (CKingMain: tabs, counts), centred at x, y, cell height h */
    void title(Canvas c, String t, float x, float y, float h, int fillColor, int outline) {
        if (GameFont.ready(context, redraw) && GameFont.covers(t)) {
            float k = h / GameFont.cellHeight();
            Bitmap b = GameFont.label(t, k, fillColor, outline);
            if (b != null) {
                c.drawBitmap(b, x - b.getWidth() / 2f, y - b.getHeight() / 2f, bmp);
                return;
            }
        }
        text.setTextSize(h * 0.6f);
        text.setTextAlign(Paint.Align.CENTER);
        text.setStyle(Paint.Style.STROKE);
        text.setStrokeWidth(h * 0.1f);
        text.setColor(outline);
        c.drawText(t, x, y + h * 0.21f, text);
        text.setStyle(Paint.Style.FILL);
        text.setColor(fillColor);
        c.drawText(t, x, y + h * 0.21f, text);
    }
}
