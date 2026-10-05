package org.wwhdrecomp.app;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.BlurMaskFilter;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.RectF;
import android.graphics.drawable.Drawable;
import android.util.Log;

import java.io.DataInputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.BufferedInputStream;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;

/**
 * Builds the touch controls' icons from the game's own artwork (item, equipment and HUD textures),
 * read from the player's game files on the device; the app ships none of it. Each texture is
 * cropped, scaled into a button bubble and given the dark navy outline of the other icons, then
 * saved as files/game_icons/<icon>.png, which TouchIcons prefers over the built-in icons.
 */
final class IconForge {
    static final String DIR = "game_icons";
    private static final int VERSION = 1;  // bump to rebuild the icons after a change here
    private static final int S = 512;      // icon size
    private static final int NAVY = 0xFF1B2A4A;
    private static final int GREEN = 0xFF3CB043, RED = 0xFFD9362B, SKY = 0xFF2F8FD8, AMBER = 0xFFE9A925,
            TEAL = 0xFF22A6A0, BONE = 0xFFF1E6C8;
    private static final int BADGE_FIRE = 0xFFEB5A1E, BADGE_ICE = 0xFF78C8F5, BADGE_LIGHT = 0xFFFAD750;

    /** an icon: its name, the layout archive and texture it comes from, the bubble, the symbol's size */
    private static final class Spec {
        final String icon, layout, texture;
        final int color, badge;
        final float scale;
        Spec(String icon, String layout, String texture, int color, float scale, int badge) {
            this.icon = icon;
            this.layout = layout;
            this.texture = texture;
            this.color = color;
            this.scale = scale;
            this.badge = badge;
        }
    }

    private static final String ITEMS = "BtnItemIcon_00", COLLECT = "BtnCollectIcon_00";
    private static final Spec[] SPECS;
    static {
        java.util.List<Spec> l = new java.util.ArrayList<>();
        l.add(new Spec("b_sword", COLLECT, "CollectIcon118_00^l", RED, 0.62f, 0));
        l.add(new Spec("b_sword_master", COLLECT, "CollectIcon118_11^l", RED, 0.62f, 0));
        l.add(new Spec("zr_shield", COLLECT, "CollectIcon118_04^l", AMBER, 0.62f, 0));
        l.add(new Spec("zr_shield_mirror", COLLECT, "CollectIcon118_05^l", AMBER, 0.62f, 0));
        l.add(new Spec("a_pick_up", "Rupy_00", "Rupy_00^l", GREEN, 0.5f, 0));
        l.add(new Spec("a_next", "ScrollKeyIcon_00", "MsgArrow_00^s", GREEN, 0.5f, 0));
        l.add(new Spec("b_cancel", "BtnBack_00", "Back_00^t", RED, 0.55f, 0));
        l.add(new Spec("dpad_up_wind_waker", COLLECT, "CollectIcon118_15^l", TEAL, 0.62f, 0));
        l.add(new Spec("dpad_left_cannon", "Shortcut_00", "IconCannon_00^q", TEAL, 0.62f, 0));
        l.add(new Spec("dpad_right_grapple", "Shortcut_00", "IconSalvage_00^q", TEAL, 0.62f, 0));
        l.add(new Spec("btn_first_person", ITEMS, "Icon128_10^l", BONE, 0.62f, 0));
        l.add(new Spec("btn_menu", "BtnDungeonItemIcon_00", "MapItemIcon_00^l", BONE, 0.62f, 0));
        l.add(new Spec("stick_base", "ControlWind_00", "WindDirectionBase_01^t", SKY, 0.86f, 0));
        l.add(new Spec("stick_knob", "BtnControlWind_00", "BtnWindMark_00^t", SKY, 0.6f, 0));
        l.add(new Spec("editor_done", "BtnMapIcon_01", "MapCheck_00^t", GREEN, 0.6f, 0));
        l.add(new Spec("item_sail", COLLECT, "CollectIcon118_06^l", SKY, 0.62f, 0));
        l.add(new Spec("item_power_bracelets", COLLECT, "CollectIcon118_08^l", SKY, 0.62f, 0));
        l.add(new Spec("item_wind_waker", COLLECT, "CollectIcon118_15^l", SKY, 0.62f, 0));
        // the arrows: the bow with a badge in the arrow's colour (the game's arrow icons are framed tiles)
        l.add(new Spec("item_fire_arrow", ITEMS, "Icon128_18^l", SKY, 0.62f, BADGE_FIRE));
        l.add(new Spec("item_ice_arrow", ITEMS, "Icon128_18^l", SKY, 0.62f, BADGE_ICE));
        l.add(new Spec("item_light_arrow", ITEMS, "Icon128_18^l", SKY, 0.62f, BADGE_LIGHT));
        // the inventory icons: TouchIcons' item names where known, every one as game_item_NN
        String[] named = {"item_bottle_empty", "item_bottle_water", "item_bottle_forest_water", "item_bottle_fairy",
                "item_bottle_firefly", "item_bottle_red_potion", "item_bottle_green_potion", "item_bottle_blue_potion",
                "item_bottle_soup", "item_bottle_half_soup", "item_telescope", "item_grappling_hook", "item_picto_box",
                "item_deluxe_picto_box", "item_tingle_bottle", "item_iron_boots", "item_magic_armor", "item_boomerang",
                "item_hero_bow", "item_hookshot", "item_skull_hammer", "item_bombs", "item_deku_leaf", "item_delivery_bag",
                "item_bait_bag", "item_spoils_bag", "item_hyoi_pear", "item_all_purpose_bait"};
        for (int i = 0; i <= 54; i++) {
            String tex = String.format(java.util.Locale.ROOT, "Icon128_%02d^l", i);
            if (i < named.length) l.add(new Spec(named[i], ITEMS, tex, SKY, 0.62f, 0));
            l.add(new Spec(String.format(java.util.Locale.ROOT, "game_item_%02d", i), ITEMS, tex, SKY, 0.62f, 0));
        }
        SPECS = l.toArray(new Spec[0]);
    }

    /** true if the icons are there for this version */
    static boolean ready(Context c) {
        return new File(new File(c.getFilesDir(), DIR), "version" + VERSION).exists();
    }

    /** builds the icons from the game files (call off the UI thread); true if any were made */
    static boolean build(Context c, String gameDir) {
        File dir = new File(c.getFilesDir(), DIR), raw = new File(c.getCacheDir(), "game_textures");
        Backup.deleteTree(dir);
        Backup.deleteTree(raw);
        if (!dir.mkdirs() || !raw.mkdirs()) return false;
        String[] layouts = new String[SPECS.length], textures = new String[SPECS.length];
        for (int i = 0; i < SPECS.length; i++) {
            layouts[i] = SPECS[i].layout;
            textures[i] = SPECS[i].texture;
        }
        int n = Native.extractUiTextures(gameDir, raw.getAbsolutePath(), layouts, textures);
        int made = 0;
        if (n > 0) {
            for (Spec s : SPECS) {
                try {
                    Bitmap src = loadRgba(new File(raw, s.texture + ".rgba"));
                    if (src == null) continue;
                    Bitmap icon = compose(c, s, src);
                    try (FileOutputStream o = new FileOutputStream(new File(dir, s.icon + ".png"))) {
                        icon.compress(Bitmap.CompressFormat.PNG, 100, o);
                    }
                    made++;
                } catch (IOException | RuntimeException e) {
                    Log.w("wwhd", "icon " + s.icon + ": " + e);
                }
            }
        }
        Backup.deleteTree(raw);
        try {
            if (!new File(dir, "version" + VERSION).createNewFile()) Log.w("wwhd", "icons: no version mark");
        } catch (IOException ignored) {
        }
        Log.i("wwhd", "icons: " + made + " made from the game files");
        return made > 0;
    }

    private static Bitmap loadRgba(File f) throws IOException {
        if (!f.exists()) return null;
        try (DataInputStream in = new DataInputStream(new BufferedInputStream(new FileInputStream(f)))) {
            byte[] hdr = new byte[8];
            in.readFully(hdr);
            ByteBuffer h = ByteBuffer.wrap(hdr).order(ByteOrder.LITTLE_ENDIAN);
            int w = h.getInt(), ht = h.getInt();
            byte[] px = new byte[w * ht * 4];
            in.readFully(px);
            int[] argb = new int[w * ht];
            for (int i = 0; i < argb.length; i++)
                argb[i] = (px[4 * i + 3] & 0xFF) << 24 | (px[4 * i] & 0xFF) << 16 | (px[4 * i + 1] & 0xFF) << 8 | (px[4 * i + 2] & 0xFF);
            return Bitmap.createBitmap(argb, w, ht, Bitmap.Config.ARGB_8888);
        }
    }

    // ---- composition, as in the reference icons: crop to the artwork, fit it into scale * 512
    // (larger textures scaled down, small ones up to fill it), a 7 px navy outline, centred on the bubble
    private static Bitmap compose(Context c, Spec s, Bitmap src) {
        Bitmap art = crop(src);
        int box = (int) (S * s.scale);
        float w = art.getWidth(), h = art.getHeight();
        if (w > box || h > box) {
            float f = Math.min(box / w, box / h);
            w *= f;
            h *= f;
        }
        if (w < box * 0.9f && h < box * 0.9f) {
            float f = box / Math.max(w, h);
            w *= f;
            h *= f;
        }
        Bitmap scaled = Bitmap.createScaledBitmap(art, Math.max(1, Math.round(w)), Math.max(1, Math.round(h)), true);
        Bitmap outlined = outline(scaled, Math.max(4, (int) (S * 0.014f)));
        Bitmap out = bubble(c, s.color);
        Canvas cv = new Canvas(out);
        Paint p = new Paint(Paint.FILTER_BITMAP_FLAG | Paint.ANTI_ALIAS_FLAG);
        cv.drawBitmap(outlined, (S - outlined.getWidth()) / 2f, (S - outlined.getHeight()) / 2f, p);
        if (s.badge != 0) {  // arrow colour, lower right
            Paint b = new Paint(Paint.ANTI_ALIAS_FLAG);
            b.setColor(NAVY);
            cv.drawCircle(S - 150, S - 150, 70, b);
            b.setColor(s.badge);
            cv.drawCircle(S - 150, S - 150, 58, b);
        }
        return out;
    }

    private static Bitmap crop(Bitmap b) {
        int w = b.getWidth(), h = b.getHeight();
        int[] px = new int[w * h];
        b.getPixels(px, 0, w, 0, 0, w, h);
        int x0 = w, y0 = h, x1 = -1, y1 = -1;
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
                if ((px[y * w + x] >>> 24) != 0) {
                    x0 = Math.min(x0, x);
                    y0 = Math.min(y0, y);
                    x1 = Math.max(x1, x);
                    y1 = Math.max(y1, y);
                }
        if (x1 < 0) return b;
        return Bitmap.createBitmap(b, x0, y0, x1 - x0 + 1, y1 - y0 + 1);
    }

    /** the artwork over its silhouette grown by r pixels (square), softened a little, in navy */
    private static Bitmap outline(Bitmap a, int r) {
        int pad = r * 2, w = a.getWidth() + pad * 2, h = a.getHeight() + pad * 2;
        int[] src = new int[a.getWidth() * a.getHeight()];
        a.getPixels(src, 0, a.getWidth(), 0, 0, a.getWidth(), a.getHeight());
        float[] m = new float[w * h];
        for (int y = 0; y < a.getHeight(); y++)
            for (int x = 0; x < a.getWidth(); x++)
                if ((src[y * a.getWidth() + x] >>> 24) > 60) m[(y + pad) * w + x + pad] = 1f;
        m = maxPass(maxPass(m, w, h, r, true), w, h, r, false);
        m = blurPass(blurPass(m, w, h, true), w, h, false);
        int[] px = new int[w * h];
        for (int i = 0; i < px.length; i++) px[i] = Math.round(Math.min(1f, m[i]) * 255) << 24 | (NAVY & 0xFFFFFF);
        Bitmap out = Bitmap.createBitmap(px, w, h, Bitmap.Config.ARGB_8888).copy(Bitmap.Config.ARGB_8888, true);
        new Canvas(out).drawBitmap(a, pad, pad, null);
        return out;
    }

    private static float[] maxPass(float[] in, int w, int h, int r, boolean horizontal) {
        float[] out = new float[in.length];
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                float v = 0;
                for (int k = -r; k <= r && v < 1f; k++) {
                    int xx = horizontal ? x + k : x, yy = horizontal ? y : y + k;
                    if (xx >= 0 && yy >= 0 && xx < w && yy < h) v = Math.max(v, in[yy * w + xx]);
                }
                out[y * w + x] = v;
            }
        return out;
    }

    // Gaussian, sigma 0.8
    private static final float[] KERNEL = {0.0219f, 0.2285f, 0.4992f, 0.2285f, 0.0219f};

    private static float[] blurPass(float[] in, int w, int h, boolean horizontal) {
        float[] out = new float[in.length];
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                float v = 0;
                for (int k = -2; k <= 2; k++) {
                    int xx = horizontal ? Math.max(0, Math.min(w - 1, x + k)) : x, yy = horizontal ? y : Math.max(0, Math.min(h - 1, y + k));
                    v += in[yy * w + xx] * KERNEL[k + 2];
                }
                out[y * w + x] = v;
            }
        return out;
    }

    /** the button: the built-in blank bubbles where they exist, else the same construction drawn */
    private static Bitmap bubble(Context c, int color) {
        Bitmap b = Bitmap.createBitmap(S, S, Bitmap.Config.ARGB_8888);
        Canvas cv = new Canvas(b);
        String blank = color == GREEN ? "bubble_blank_a" : color == RED ? "bubble_blank_b" : color == AMBER ? "bubble_blank_zr" : null;
        int id = blank != null ? c.getResources().getIdentifier(blank, "drawable", c.getPackageName()) : 0;
        if (id != 0) {
            Drawable d = c.getDrawable(id);
            if (d != null) {
                d.setBounds(0, 0, S, S);
                d.draw(cv);
                return b;
            }
        }
        Paint p = new Paint(Paint.ANTI_ALIAS_FLAG);
        p.setColor(NAVY);
        cv.drawOval(new RectF(10, 10, S - 10, S - 10), p);
        p.setColor(lighter(color, 55));
        cv.drawOval(new RectF(26, 26, S - 26, S - 26), p);
        p.setColor(color);
        cv.drawOval(new RectF(36, 36, S - 36, S - 36), p);
        p.setColor(0x6EFFFFFF);  // highlight, top left
        p.setMaskFilter(new BlurMaskFilter(9.5f, BlurMaskFilter.Blur.NORMAL));
        cv.drawOval(new RectF(90, 62, 210, 120), p);
        return b;
    }

    private static int lighter(int c, int d) {
        int r = Math.min(255, ((c >> 16) & 0xFF) + d), g = Math.min(255, ((c >> 8) & 0xFF) + d), b = Math.min(255, (c & 0xFF) + d);
        return 0xFF000000 | r << 16 | g << 8 | b;
    }
}
