package org.wwhdrecomp.app;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;

import java.io.DataInputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.HashMap;
import java.util.Map;

/**
 * Icons for the context buttons, taken from the player's own game files (the 2D pack's item and
 * equipment textures, runtime/src/android/ui_icons.cpp) and kept as PNGs in the app's private
 * storage (files/game_icons). The app ships none of them. Built once in the background on first use.
 */
final class GameIcons {
    private static final String DIR = "game_icons";
    private static final int VERSION = 2;  // bump to rebuild after a change of the list
    private static final String ITEMS = "BtnItemIcon_00", COLLECT = "BtnCollectIcon_00";
    private static final Map<String, Bitmap> cache = new HashMap<>();
    private static volatile boolean building;

    private GameIcons() {}

    /** the texture of an item id on X / Y / R (inventory icons, mapped by looking at them), or null */
    static String itemTexture(int id) {
        int i;
        switch (id) {
            case 0x20: i = 10; break;  // telescope
            case 0x21: i = 14; break;  // Tingle Bottle
            case 0x23: i = 12; break;  // Picto Box
            case 0x26: i = 13; break;  // Deluxe Picto Box
            case 0x24: i = 25; break;  // spoils bag
            case 0x25: i = 11; break;  // grappling hook
            case 0x27: case 0x35: case 0x36: i = 18; break;  // bow (and its magic arrows)
            case 0x29: i = 15; break;  // Iron Boots
            case 0x2A: i = 16; break;  // Magic Armor
            case 0x2C: i = 24; break;  // bait bag
            case 0x2D: i = 17; break;  // boomerang
            case 0x2F: i = 19; break;  // hookshot
            case 0x30: i = 23; break;  // delivery bag
            case 0x31: i = 21; break;  // bombs
            case 0x33: i = 20; break;  // Skull Hammer
            case 0x34: i = 22; break;  // Deku Leaf
            case 0x50: i = 0; break;   // empty bottle
            case 0x51: i = 5; break;   // red potion
            case 0x52: i = 6; break;   // green potion
            case 0x53: i = 7; break;   // blue potion
            case 0x54: i = 9; break;   // half Elixir Soup
            case 0x55: i = 8; break;   // Elixir Soup
            case 0x56: i = 1; break;   // water
            case 0x57: i = 3; break;   // fairy
            case 0x58: i = 4; break;   // forest firefly
            case 0x59: i = 2; break;   // Forest Water
            case 0x78: return "CollectIcon118_06^l";  // sail
            case 0x77: return "CollectIcon118_16^l";  // swift sail
            default: return null;
        }
        return String.format(java.util.Locale.ROOT, "Icon128_%02d^l", i);
    }

    /** the texture of the equipped sword (equipment id), or null */
    static String swordTexture(int id) {
        switch (id) {
            case 0x38: return "CollectIcon118_00^l";  // Hero's Sword
            case 0x39: return "CollectIcon118_01^l";  // Master Sword (powerless)
            case 0x3A: return "CollectIcon118_02^l";  // half power
            case 0x3E: return "CollectIcon118_03^l";  // full power
            default: return null;
        }
    }

    // game HUD and menu textures used as button icons (layout archive, texture), found by looking at
    // the whole 2D pack
    private static final String[][] EXTRA = {
            {COLLECT, "CollectIcon118_04^l"},              // Hero's Shield
            {COLLECT, "CollectIcon118_05^l"},              // Mirror Shield
            {COLLECT, "CollectIcon118_15^l"},              // the Wind Waker
            {"ScrollKeyIcon_00", "MsgArrow_00^s"},         // next (message arrow)
            {"BtnBack_00", "Back_00^t"},                   // cancel / return
            {"BtnMapIcon_01", "MapCheck_00^t"},            // choose (check mark)
            {"BtnMiiversePicture_00", "MiiverseZoomIcon_00^t"},  // look / check (magnifier)
            {"Submarine_00", "SubmarineBalloon_00^t"},     // speak (speech balloon)
            {"Shortcut_00", "IconCannon_00^q"},            // d-pad left: cannon
            {"Shortcut_00", "IconSalvage_00^q"},           // d-pad right: salvage hook
            {"InformationDRC_00", "TvDrcIcon_00^t"},       // -: TV / GamePad
            {"PictographBox_00", "LStick_00^t"},           // L3
            {"PictographBox_00", "RStick_00^t"},           // R3
            {"BtnDungeonItemIcon_00", "MapItemIcon_00^l"}, // +: menu (map)
    };

    /** the game icon for an action code (dActStts), or null; sword / shield: the equipped ones */
    static String actionTexture(int code, int sword, int shield) {
        switch (code) {
            case 0x19: return "MsgArrow_00^s";
            case 0x27: case 0x07: return "Back_00^t";
            case 0x17: return "MapCheck_00^t";
            case 0x01: case 0x0A: return "MiiverseZoomIcon_00^t";
            case 0x02: return "SubmarineBalloon_00^t";
            case 0x36: return shield == 0x3C ? "CollectIcon118_05^l" : "CollectIcon118_04^l";
            default: return null;
        }
    }

    private static String[][] wanted() {
        java.util.List<String[]> l = new java.util.ArrayList<>();
        for (int i = 0; i <= 25; i++) l.add(new String[] {ITEMS, String.format(java.util.Locale.ROOT, "Icon128_%02d^l", i)});
        for (int i : new int[] {0, 1, 2, 3, 6, 16}) l.add(new String[] {COLLECT, String.format(java.util.Locale.ROOT, "CollectIcon118_%02d^l", i)});
        java.util.Collections.addAll(l, EXTRA);
        return l.toArray(new String[0][]);
    }

    /** the icon, or null (not built yet: building starts in the background and the view redraws later) */
    static synchronized Bitmap get(Context c, String texture, Runnable whenReady) {
        if (texture == null) return null;
        if (cache.containsKey(texture)) return cache.get(texture);
        File dir = new File(c.getFilesDir(), DIR);
        if (new File(dir, "version" + VERSION).exists()) {
            Bitmap b = BitmapFactory.decodeFile(new File(dir, file(texture)).getAbsolutePath());
            cache.put(texture, b);
            return b;
        }
        if (!building && MainActivity.instance != null) {
            building = true;
            String gameDir = MainActivity.instance.gameDir();
            new Thread(() -> {
                build(c, dir, gameDir);
                synchronized (GameIcons.class) {
                    cache.clear();
                    building = false;
                }
                if (whenReady != null) whenReady.run();
            }, "game-icons").start();
        }
        return null;
    }

    private static String file(String texture) { return texture.replace("^", "_") + ".png"; }

    private static void build(Context c, File dir, String gameDir) {
        File raw = new File(c.getCacheDir(), "game_textures");
        Backup.deleteTree(dir);
        Backup.deleteTree(raw);
        if (!dir.mkdirs() || !raw.mkdirs()) return;
        String[][] w = wanted();
        String[] layouts = new String[w.length], textures = new String[w.length];
        for (int i = 0; i < w.length; i++) {
            layouts[i] = w[i][0];
            textures[i] = w[i][1];
        }
        Native.extractUiTextures(gameDir, raw.getAbsolutePath(), layouts, textures);
        for (String t : textures) {
            try {
                Bitmap b = loadRgba(new File(raw, t + ".rgba"));
                if (b == null) continue;
                try (FileOutputStream o = new FileOutputStream(new File(dir, file(t)))) {
                    b.compress(Bitmap.CompressFormat.PNG, 100, o);
                }
            } catch (IOException | RuntimeException e) {
                android.util.Log.w("wwhd", "icon " + t + ": " + e);
            }
        }
        Backup.deleteTree(raw);
        try {
            new File(dir, "version" + VERSION).createNewFile();
        } catch (IOException ignored) {
        }
    }

    private static Bitmap loadRgba(File f) throws IOException {
        if (!f.exists()) return null;
        try (DataInputStream in = new DataInputStream(new FileInputStream(f))) {
            byte[] hdr = new byte[8];
            in.readFully(hdr);
            ByteBuffer h = ByteBuffer.wrap(hdr).order(ByteOrder.LITTLE_ENDIAN);
            int w = h.getInt(), ht = h.getInt();
            if (w <= 0 || ht <= 0 || w > 4096 || ht > 4096) return null;
            byte[] px = new byte[w * ht * 4];
            in.readFully(px);
            int[] argb = new int[w * ht];
            for (int i = 0; i < argb.length; i++)
                argb[i] = (px[i * 4 + 3] & 0xFF) << 24 | (px[i * 4] & 0xFF) << 16 | (px[i * 4 + 1] & 0xFF) << 8 | (px[i * 4 + 2] & 0xFF);
            return Bitmap.createBitmap(argb, w, ht, Bitmap.Config.ARGB_8888);
        }
    }
}
