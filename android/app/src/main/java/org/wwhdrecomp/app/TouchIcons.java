package org.wwhdrecomp.app;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.drawable.Drawable;

import java.util.HashMap;

/**
 * Icons of the touch controls: PNGs in res/drawable-nodpi named as in docs/touch-icons-prompts-v2.md.
 * An icon that isn't there yet is drawn as a plain bubble in its group's colour with a short label,
 * so the controls work before the artwork exists.
 */
final class TouchIcons {
    // button colours (docs/touch-icons-prompts-v2.md)
    static final int GREEN = 0xFF3CB043, RED = 0xFFD9362B, SKY = 0xFF2F8FD8, VIOLET = 0xFF7E4FC9, AMBER = 0xFFE9A925,
            ORANGE = 0xFFE8702A, TEAL = 0xFF22A6A0, BONE = 0xFFF1E6C8, NAVY = 0xFF1B2A4A;

    // the game's state as the native side reports it (Native.hudState); codes index these tables.
    // Code 0 = unknown (state not read yet), 1 = no action / empty slot.
    static final String[] A_ACTIONS = {null, "bubble_blank_a", "a_talk", "a_look", "a_read", "a_open", "a_door", "a_lift",
            "a_throw", "a_put_down", "a_pick_up", "a_grab", "a_climb", "a_jump", "a_let_go", "a_roll", "a_board_boat",
            "a_leave_boat", "a_stop", "a_put_away", "a_next", "a_parry", "a_swim", "a_drink", "a_generic"};
    static final String[] B_ACTIONS = {null, "b_empty", "b_sword", "b_cancel", "b_drop"};
    static final String[] ZR_ACTIONS = {null, "bubble_blank_zr", "zr_shield", "zr_crouch", "zr_grab", "zr_boat_jump",
            "zr_no_sail", "zr_brake"};
    static final String[] ITEMS = {null, "bubble_empty_item", "item_telescope", "item_sail", "item_swift_sail",
            "item_wind_waker", "item_grappling_hook", "item_spoils_bag", "item_boomerang", "item_deku_leaf",
            "item_tingle_bottle", "item_picto_box", "item_deluxe_picto_box", "item_iron_boots", "item_magic_armor",
            "item_bait_bag", "item_hero_bow", "item_fire_arrow", "item_ice_arrow", "item_light_arrow", "item_bombs",
            "item_bottle_empty", "item_bottle_red_potion", "item_bottle_green_potion", "item_bottle_blue_potion",
            "item_bottle_soup", "item_bottle_half_soup", "item_bottle_water", "item_bottle_forest_water",
            "item_bottle_fairy", "item_bottle_firefly", "item_delivery_bag", "item_hookshot", "item_skull_hammer",
            "item_hyoi_pear", "item_all_purpose_bait"};

    static String at(String[] table, int code) { return code > 0 && code < table.length ? table[code] : null; }

    private final Context ctx;
    private final HashMap<String, Drawable> cache = new HashMap<>();
    private final Paint fill = new Paint(Paint.ANTI_ALIAS_FLAG), line = new Paint(Paint.ANTI_ALIAS_FLAG),
            text = new Paint(Paint.ANTI_ALIAS_FLAG);

    TouchIcons(Context c) {
        ctx = c;
        fill.setStyle(Paint.Style.FILL);
        line.setStyle(Paint.Style.STROKE);
        text.setTextAlign(Paint.Align.CENTER);
        text.setFakeBoldText(true);
    }

    /**
     * The icon with that name: built from the game's own artwork (IconForge) if there is one, else
     * the PNG in the app; null if neither exists yet.
     */
    Drawable get(String name) {
        if (name == null) return null;
        if (cache.containsKey(name)) return cache.get(name);
        Drawable d = null;
        java.io.File f = new java.io.File(new java.io.File(ctx.getFilesDir(), IconForge.DIR), name + ".png");
        if (f.exists()) {
            android.graphics.Bitmap b = android.graphics.BitmapFactory.decodeFile(f.getPath());
            if (b != null) d = new android.graphics.drawable.BitmapDrawable(ctx.getResources(), b);
        }
        if (d == null) {
            int id = ctx.getResources().getIdentifier(name, "drawable", ctx.getPackageName());
            d = id != 0 ? ctx.getDrawable(id) : null;
        }
        cache.put(name, d);
        return d;
    }

    /** forget loaded icons (new ones were built) */
    void clear() { cache.clear(); }

    /**
     * Draws the icon `name` as a bubble of radius r at (cx, cy); without the PNG, a bubble of colour
     * `color` with `label`. `alpha` 0..255; `pressed` darkens it.
     */
    void draw(Canvas c, String name, int color, String label, float cx, float cy, float r, int alpha, boolean pressed) {
        Drawable d = get(name);
        if (d != null) {
            d.setBounds((int) (cx - r), (int) (cy - r), (int) (cx + r), (int) (cy + r));
            d.setAlpha(alpha);
            d.draw(c);
            if (pressed) {
                Drawable o = get("bubble_pressed_overlay");
                if (o != null) {
                    o.setBounds(d.getBounds());
                    o.setAlpha(alpha);
                    o.draw(c);
                } else {
                    fill.setColor(NAVY);
                    fill.setAlpha(alpha / 3);
                    c.drawCircle(cx, cy, r * 0.9f, fill);
                }
            }
            return;
        }
        fill.setColor(pressed ? darker(color) : color);
        fill.setAlpha(alpha);
        c.drawCircle(cx, cy, r * 0.9f, fill);
        // shine on the upper left, as the bubbles of the artwork
        fill.setColor(0xFFFFFFFF);
        fill.setAlpha(alpha / 3);
        c.drawCircle(cx - r * 0.35f, cy - r * 0.38f, r * 0.22f, fill);
        line.setColor(NAVY);
        line.setAlpha(alpha);
        line.setStrokeWidth(Math.max(2f, r * 0.09f));
        c.drawCircle(cx, cy, r * 0.9f, line);
        if (label != null && !label.isEmpty()) {
            text.setColor(color == BONE ? NAVY : 0xFFFFFFFF);
            text.setAlpha(alpha);
            text.setTextSize(r * (label.length() > 2 ? 0.5f : 0.75f));
            c.drawText(label, cx, cy - (text.descent() + text.ascent()) / 2, text);
        }
    }

    /** a letter over an icon (e.g. "X" on an empty item slot) */
    void label(Canvas c, String label, float cx, float cy, float r, int alpha) {
        text.setTextSize(r * 0.7f);
        text.setStyle(Paint.Style.STROKE);  // navy outline under the white letter
        text.setStrokeWidth(Math.max(2f, r * 0.12f));
        text.setColor(NAVY);
        text.setAlpha(alpha);
        float y = cy - (text.descent() + text.ascent()) / 2;
        c.drawText(label, cx, y, text);
        text.setStyle(Paint.Style.FILL);
        text.setColor(0xFFFFFFFF);
        text.setAlpha(alpha);
        c.drawText(label, cx, y, text);
    }

    private static int darker(int c) {
        int r = (int) (((c >> 16) & 0xFF) * 0.7f), g = (int) (((c >> 8) & 0xFF) * 0.7f), b = (int) ((c & 0xFF) * 0.7f);
        return 0xFF000000 | r << 16 | g << 8 | b;
    }
}
