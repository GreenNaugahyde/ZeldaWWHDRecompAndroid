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
    private static final int VERSION = 35;  // bump to rebuild after a change of the list
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
            case 0x24: i = 23; break;  // spoils bag
            case 0x25: i = 11; break;  // grappling hook
            case 0x27: case 0x35: case 0x36: i = 18; break;  // bow (and its magic arrows)
            case 0x29: i = 15; break;  // Iron Boots
            case 0x2A: i = 16; break;  // Magic Armor
            case 0x2C: i = 24; break;  // bait bag
            case 0x2D: i = 17; break;  // boomerang
            case 0x2F: i = 19; break;  // hookshot
            case 0x30: i = 25; break;  // delivery bag
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
            case 0x22: return "CollectIcon118_15^l";  // the Wind Waker (as on d-pad up)
            // the bags' contents (Icon128_26..54, mapped by looking at them; the delivery bag's
            // letters and cards 36..42 not yet confirmed in the game)
            case 0x83: i = 26; break;  // Hyoi Pear
            case 0x82: i = 27; break;  // All-Purpose Bait
            case 0x1F: i = 28; break;  // Joy Pendant
            case 0x46: i = 29; break;  // Boko Baba Seed
            case 0x48: i = 30; break;  // Knight's Crest
            case 0x47: i = 31; break;  // Golden Feather
            case 0x45: i = 32; break;  // Skull Necklace
            case 0x49: i = 33; break;  // Red Chu Jelly
            case 0x4A: i = 34; break;  // Green Chu Jelly
            case 0x4B: i = 35; break;  // Blue Chu Jelly
            case 0x9C: i = 36; break;  // Cabana Deed (?)
            case 0x9D: i = 37; break;  // Complimentary ID (?)
            case 0x98: i = 38; break;  // Father's Letter (?)
            case 0x9B: i = 39; break;  // Moblin's Letter (?)
            case 0x99: i = 40; break;  // Note to Mom (?)
            case 0x9A: i = 41; break;  // Maggie's Letter (?)
            case 0x9E: i = 42; break;  // Fill-Up Coupon (?)
            case 0x8C: i = 43; break;  // Town Flower
            case 0x8D: i = 44; break;  // Sea Flower
            case 0x8E: i = 45; break;  // Exotic Flower
            case 0x8F: i = 46; break;  // Hero's Flag
            case 0x93: i = 47; break;  // Sickle Moon Flag
            case 0x90: i = 48; break;  // Big Catch Flag
            case 0x91: i = 49; break;  // Big Sale Flag
            case 0x92: i = 50; break;  // Pinwheel
            case 0x94: i = 51; break;  // Skull Tower Idol
            case 0x95: i = 52; break;  // Fountain Idol
            case 0x96: i = 53; break;  // Postman Statue
            case 0x97: i = 54; break;  // Shop Guru Statue
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
            {"DungeonMap_00", "MapLinkFace_00^l"},         // ZL in the bow's first person: Link seen (third person; link_poses off)
            {"PictographBox_00", "PictographFinder_00^t"}, // the Picto Box's finder: its top left corner (R3: first person)
            {"MsgWindowScroll_00", "MsgWindowScrollStone_00^q"},  // the stone message window's pattern (the wall of sidle)
            // the HUD's button bases (context buttons): white round, A's yellow star
            {"CommandGuide_00", "ABXY^f"}, {"CommandGuide_00", "ABXY_f"},
            {"CommandGuide_00", "ASpecial^l"}, {"CommandGuide_00", "ASpecial_l"},
            // parts of composed action icons
            {"Battleship_00", "BattleshipIcon_00^l"},      // boat (get in / out)
            {"Auction_00", "AuctionIcon_00^l"},            // bid
            {"BtnMapIcon_01", "MapBtn_00^l"},              // sea chart
            {"Arrow_00", "NormalArrow_00^l"}, {"Arrow_00", "FireArrow_00^l"}, {"Arrow_00", "IceArrow_00^l"},
            {"BestShotMark_00", "BestShotMark_00^t"},      // the album's gold medal (a picto good enough for a figurine)
            {"Cursor_00", "Crsor_00^t"},                   // the menus' cursor: its top left corner, frame 1
            {"Cursor_00", "Crsor_01^t"},                   // ... frame 2
            {"BtnMenuArrow_00", "ArrowBase_00^t"},         // the GamePad menus' page arrow (GameUi.MenuArrow)
            {"BtnMenuArrow_00", "ArrowShadow_00^t"},       // ... its shadow
            // conducting (the song panel of singing the songs, SongPanel): the note signs and the input points
            {"BatonSign_00", "BatonSignBase_00^t"}, {"BatonSign_00", "BatonSignFrame_00^t"},
            {"BatonSign_00", "BatonSignArrow_00^t"}, {"BatonSign_00", "BatonSign_00^t"},
            {"BatonInput_00", "BatonInputPointBase_00^t"},
            {"BatonInputPoint_01", "BatonInputPoint_00^t"}, {"BatonInputPoint_01", "BatonInputPoint_01^t"},
            {"BatonInputPoint_01", "BatonInputPoint_02^t"},
            {"BatonSignList_00", "BatonSlidInputFrame_00^t"}, {"BatonSignList_00", "GradationFrameLT_00^t"},
            {"BtnMenuArrow_00", "ArrowSpec_00^t"},         // ... its gloss
            {"BtnMenu_00", "ScratchPattern_00^t"},         // the menu tabs' scratches (GameUi.TabPlate)
            {"BtnMenu_00", "MenuBtnBase_00^t"},            // ... their plate
            {"BtnMenu_00", "MenuBtnShadow_00^t"},          // ... its shadow
            {"BtnMenu_00", "MenuBtnSpec_00^t"},            // ... its gloss
            {"BtnMenu_00", "MenuBtnDot_00^t"},             // ... its dots
            {"BtnMenuSaveOption_00", "BtnSequencePattern_01^t"},  // the inventory's Save / Options button (GameUi.GameRowPlate)
            {"BtnMenuSaveOption_00", "BtnSequenceShadow_01^t"},   // ... its edge and shadow
            {"BtnMenuSaveOption_00", "BtnSequenceSpec_01^q"},     // ... its gloss
            {"BtnMenuSaveOption_00", "BtnSelect_02^q"},           // ... its highlight
            {"BtnOption_00", "OlvSetBtnBase_00^t"},        // the game's Options: its value button (GameUi.GameValueButton)
            {"BtnOption_00", "OlvSetBtnShadow_00^t"},      // ... its edge and shadow
            {"BtnOption_00", "OlvSetBtnSpec_00^t"},        // ... its gloss
            {"BtnOption_00", "OlvSetBtnDot_00^t"},         // ... its dots
            {"BtnOption_00", "BtnSelect_00^q"},            // ... its highlight
            {"SequenceOption_00", "OptionArrow_00^s"},     // ... the arrows beside a value
            {"SequenceOption_00", "OptionBaseLT_00^s"},    // ... the chosen row's crossed bands (content cursor)
            {"MenuItem_00", "ItemBasePattern_00^r"},       // the GamePad menus' blue background (GameUi.GamePaper)
            {"MenuItem_00", "ItemBasePattern_01^r"},       // their paper
            {"MenuItem_00", "RepeatWindowLT_00^t"},        // the paper sheet's torn corner,
            {"MenuItem_00", "RepeatWindowTB_00^t"},        // ... top / bottom edge,
            {"MenuItem_00", "RepeatWindowLR_00^t"},        // ... left / right edge
            {"MenuItem_00", "RepeatWindowEdgeShadowLT_00^t"},
            {"MenuItem_00", "RepeatWindowEdgeShadowRT_00^t"},
            {"MenuItem_00", "RepeatWindowEdgeShadowLB_00^t"},
            {"MenuItem_00", "RepeatWindowEdgeShadowRB_00^t"},
            {"MenuItem_00", "RepeatWindowEdgeShadowT_00^t"},
            {"MenuItem_00", "RepeatWindowEdgeShadowB_00^t"},
            {"MenuItem_00", "RepeatWindowEdgeShadowL_00^t"},
            {"MenuItem_00", "RepeatWindowEdgeShadowR_00^t"},
            {"MenuItem_00", "RepeatWindowGlowLineLT_00^t"},
            {"MenuItem_00", "RepeatWindowGlowLineTB_00^t"},
            {"MenuItem_00", "RepeatWindowGlowLineLR_00^t"},
            {"MsgWindowMain_00", "MsgWindowMain_00^t"},    // the game's message window (GameHint)
            {"MsgWindowMain_00", "MsgSubWindowC2_00^t"},   // ... its two-choice box (left half)
            {"Cursor_01", "CrsorS_00^t"},                  // ... the choices' cursor, frame 1
            {"Cursor_01", "CrsorS_01^t"},                  // ... frame 2
            {"Moon_00", "CrescentMoon_00^t"},              // the moon jump (with Link's jump)
    };

    // Link in poses of his own animations, drawn from the game's model (Native.renderLinkIcons): jump, crouch,
    // standing (stand up), seen from behind (third person), sidling (a step), crouching down and standing up, the roll's LINK_ROLL_FRAMES pictures (LINK_ROLL + frame)
    // and its resting one, carrying a pot: lifting, throwing, setting it down (the lift backwards), opening a door, a
    // chest, pushing (grab), picking up a Bokoblin's stick, the jump attack (sword in hand), letting go of a ladder, a wall of vines, a ledge; the boat raising / lowering its sail and sailing (the King of
    // Red Lions' own model, also with linkPoses off); linkPoses off: the pictograms (behind: MapLinkFace).
    // Animated ones (ANIMATED: prefix and pictures) are named with '*' for the picture in icon keys
    // (ControlsView plays them, then keeps the last)
    static final String LINK_JUMP = "link_jump_0", LINK_CROUCH = "link_crouch_0", LINK_STAND = "link_stand_0", LINK_BACK = "link_back_0",
            LINK_SIDLE = "link_sidle_", LINK_ROLL = "link_roll_", LINK_LIFT = "link_lift_", LINK_THROW = "link_throw_",
            LINK_DROP = "link_drop_", LINK_DOOR = "link_door_", LINK_CHEST = "link_chest_", LINK_PUSH = "link_push_",
            LINK_PICKUP = "link_pickup_", LINK_KNEEL = "link_kneel_", LINK_RISE = "link_rise_", POSTBOX = "link_postbox_",
            LINK_JATTACK = "link_jattack_", LINK_LADDER = "link_ladder_", LINK_IVY = "link_ivy_", LINK_HANG = "link_hang_",
            SAIL_UP = "link_sailup_", SAIL_UP_SWIFT = "link_sailup_swift_", SAIL_DOWN = "link_saildown_",
            SAIL_DOWN_SWIFT = "link_saildown_swift_", SAILING = "link_sailing_", DRIFTING = "link_drifting_",
            LINK_GETIN = "link_getin_", LINK_GETOUT = "link_getout_", LINK_TALK = "link_talk_", LINK_WALKAWAY = "link_walkaway_",
            LINK_CHECK = "link_check_";
    static final int LINK_ROLL_FRAMES = 12, LINK_ROLL_REST = 12;
    private static final Object[][] ANIMATED = {{LINK_SIDLE, 16}, {LINK_LIFT, 8}, {LINK_THROW, 8}, {LINK_DROP, 8},
            {LINK_DOOR, 12}, {LINK_CHEST, 10}, {LINK_PUSH, 12}, {LINK_PICKUP, 8}, {LINK_KNEEL, 8}, {LINK_RISE, 8}, {POSTBOX, 10},
            {LINK_JATTACK, 12}, {LINK_LADDER, 10}, {LINK_IVY, 10}, {LINK_HANG, 10},
            {SAIL_UP, 10}, {SAIL_UP_SWIFT, 10}, {SAIL_DOWN, 10}, {SAIL_DOWN_SWIFT, 10}, {SAILING, 8}, {DRIFTING, 8},
            {LINK_GETIN, 10}, {LINK_GETOUT, 10}, {LINK_TALK, 12}, {LINK_WALKAWAY, 12}, {LINK_CHECK, 12}};

    /** how many pictures the animation in an icon key ("...link_x_*...") has, 0 for a still icon */
    static int pictures(String key) {
        for (Object[] a : ANIMATED)
            if (key.contains((String) a[0] + "*")) return (Integer) a[1];
        return 0;
    }
    static volatile boolean linkPoses = true;

    private static final Map<Bitmap, float[]> offsets = new java.util.WeakHashMap<>();

    /** how far the visible part's centre (alpha >= 32) is from b's centre, in b's width / height */
    static synchronized float[] contentOffset(Bitmap b) {
        float[] o = offsets.get(b);
        if (o != null) return o;
        int w = b.getWidth(), h = b.getHeight(), x0 = w, y0 = h, x1 = -1, y1 = -1;
        int[] px = new int[w * h];
        b.getPixels(px, 0, w, 0, 0, w, h);
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
                if ((px[y * w + x] >>> 24) >= 32) {
                    x0 = Math.min(x0, x); x1 = Math.max(x1, x);
                    y0 = Math.min(y0, y); y1 = Math.max(y1, y);
                }
        o = x1 < 0 ? new float[2] : new float[] {((x0 + x1 + 1) / 2f - w / 2f) / w, ((y0 + y1 + 1) / 2f - h / 2f) / h};
        offsets.put(b, o);
        return o;
    }

    /** b is one of Link's poses (as get() returned it) */
    static synchronized boolean isLinkPose(Bitmap b) {
        if (b == null) return false;
        for (Map.Entry<String, Bitmap> e : cache.entrySet())
            if (e.getValue() == b) return e.getKey().startsWith("link_") || e.getKey().startsWith("#sidle:") || e.getKey().startsWith("#carry:")
                        || e.getKey().startsWith("#grab:") || e.getKey().startsWith("#sailing:") || e.getKey().startsWith("#stop:")
                        || e.getKey().startsWith("#speak:") || e.getKey().startsWith("#exit:") || e.getKey().startsWith("#check:");
        return false;
    }

    // the HUD's button bases ("ABXY^f" may also be named with '_': both are extracted to one file)
    static final String BASE_ROUND = "ABXY^f", BASE_STAR = "ASpecial^l";

    // ---- action icons: the game's own textures, alone or composed ("#..." keys, built on first use).
    // Arrows are the message arrow with a thick dark outline, like the game's own Return icon.
    static final String SWORD_DEFAULT = "CollectIcon118_00^l";

    /** the icon for an action code (dActStts) on A / B / ZR, or null (then text): sword and shield
     *  are the equipped ones */
    static String actionIcon(int code, int sword, int shield) {
        String sw = swordTexture(sword);
        if (sw == null) sw = SWORD_DEFAULT;
        switch (code) {
            case 0x01: case 0x0A: return linkPoses ? "#check:" + LINK_CHECK + "*" : "#lens";  // look, check: Link peering through a magnifying glass
            case 0x02: return linkPoses ? "#speak:" + LINK_TALK + "*" : "SubmarineBalloon_00^t";  // speak: Link talking, a balloon
            case 0x03: case 0x30: return "MapBtn_00^l";             // (sea) chart
            case 0x04: return linkPoses ? "#carry:" + LINK_LIFT + "*:180" : "#arrow:180";  // lift: Link lifting a pot, arrow up
            case 0x05: return "#arrow:180";                         // climb: up
            case 0x06: case 0x16: case 0x09: return "#arrow:0";     // let go, drop: down
            case 0x07: case 0x27: return "Back_00^t";               // return, cancel
            case 0x08: return "#putaway:" + sw;                     // put away: the sword, pointing down, and an arrow
            case 0x0E: return "#arrow:135";                         // throw: up and away
            case 0x12: return linkPoses ? LINK_JUMP : "#arrow:150";  // jump
            case 0x0C: return linkPoses ? LINK_ROLL : null;          // roll (ControlsView plays its frames)
            case 0x0F: return linkPoses ? LINK_KNEEL + "*" : null;   // crouch: Link going down
            case 0x10: return linkPoses ? "#sidle:*" : null;         // sidle: Link at a wall, arrows to both sides
            case 0x11: return linkPoses ? "#grab:*" : null;          // grab: Link pushing a stone block
            case 0x1B: return linkPoses ? LINK_PICKUP + "*" : null;  // pick up (a Bokoblin's stick): Link crouching to it
            case 0x17: return "MapCheck_00^t";                      // choose
            case 0x19: return "#arrow:90";                          // next
            case 0x1C: return "#ship:0";                            // get in: boat, arrow down
            case 0x1D: return "#ship:180";                          // get out: boat, arrow up
            case 0x20: return "Icon128_12^l";                       // take a photo: the Picto Box
            case 0x22: return "#arrows";                            // swap modes (arrow types)
            case 0x23: return "Icon128_22^l";                       // fly: the Deku Leaf
            case 0x25: return "AuctionIcon_00^l";                   // bid
            case 0x2C: return "CollectIcon118_06^l";                // cruise: the sail (ControlsView: the boat setting sail)
            case 0x2F: return "Icon128_11^l";                       // swing: the grappling hook
            case 0x35: return sw;                                   // the sword
            case 0x36: return shield == 0x3C ? "CollectIcon118_05^l" : "CollectIcon118_04^l";  // defend
            case 0x3D: return "Icon128_18^l";                       // the bow (Windfall's bomb game)
            case 0x45: return "#delete";                            // erase (HD)
            case 0x4B: return "MapCheck_00^t";                      // choose (HD)
            case 0x4C: return "Back_00^t";                          // close (HD)
            case 0x49: return "#album";                             // the Picto Box's album (HD)
            case 0x48: return "Icon128_12^l";                       // the album: back to the Picto Box (HD)
            default: return null;
        }
    }

    private static String[][] wanted() {
        java.util.List<String[]> l = new java.util.ArrayList<>();
        for (int i = 0; i <= 54; i++) l.add(new String[] {ITEMS, String.format(java.util.Locale.ROOT, "Icon128_%02d^l", i)});
        for (int i : new int[] {0, 1, 2, 3, 6, 16}) l.add(new String[] {COLLECT, String.format(java.util.Locale.ROOT, "CollectIcon118_%02d^l", i)});
        java.util.Collections.addAll(l, EXTRA);
        return l.toArray(new String[0][]);
    }

    /** the icon, or null (not built yet: building starts in the background and the view redraws later) */
    static synchronized Bitmap get(Context c, String texture, Runnable whenReady) {
        if (texture == null) return null;
        if (cache.containsKey(texture)) return cache.get(texture);
        if (texture.startsWith("#")) {
            Bitmap b = compose(c, texture.substring(1), whenReady);
            if (b != null) cache.put(texture, b);
            return b;
        }
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

    // ---- composed icons: 256 x 256, parts placed by their centre (fractions) and longest side
    private static final int SIZE = 256;

    private static Bitmap compose(Context c, String key, Runnable whenReady) {
        Bitmap out = Bitmap.createBitmap(SIZE, SIZE, Bitmap.Config.ARGB_8888);
        android.graphics.Canvas cv = new android.graphics.Canvas(out);
        String[] k = key.split(":", 2);
        switch (k[0]) {
            case "arrow": {
                Bitmap a = arrow(c, whenReady);
                if (a == null) return null;
                place(cv, a, Float.parseFloat(k[1]), 0.5f, 0.5f, Float.parseFloat(k[1]) % 90 == 0 ? 0.62f : 0.58f);
                return out;
            }
            case "target": {  // four yellow arrows (the Z-target's colour) pointing in: targeting; "1" latched: closer in
                Bitmap a = arrow(c, 0xFFFFC828, whenReady);
                if (a == null) return null;
                float d = k[1].equals("1") ? 0.22f : 0.3f;
                place(cv, a, 0, 0.5f, 0.5f - d, 0.34f);
                place(cv, a, 270, 0.5f + d, 0.5f, 0.34f);
                place(cv, a, 180, 0.5f, 0.5f + d, 0.34f);
                place(cv, a, 90, 0.5f - d, 0.5f, 0.34f);
                return out;
            }
            case "finder": {  // the Picto Box's finder, its four round corners with a dark edge: first person
                Bitmap f = outlined(get(c, "PictographFinder_00^t", whenReady), 0xFF0A0A14, 0, 6, 2);
                if (f == null) return null;
                android.graphics.Paint p = new android.graphics.Paint(android.graphics.Paint.FILTER_BITMAP_FLAG | android.graphics.Paint.ANTI_ALIAS_FLAG);
                float sc = 0.36f * SIZE / f.getWidth();
                for (int i = 0; i < 4; i++) {  // top left, top right, bottom right, bottom left: mirrored
                    float sx = i == 1 || i == 2 ? -1 : 1, sy = i >= 2 ? -1 : 1;
                    android.graphics.Matrix m = new android.graphics.Matrix();
                    m.postTranslate(-f.getWidth() / 2f, -f.getHeight() / 2f);
                    m.postScale(sx * sc, sy * sc);
                    m.postTranslate((0.5f - sx * 0.2f) * SIZE, (0.5f - sy * 0.2f) * SIZE);
                    cv.drawBitmap(f, m, p);
                }
                return out;
            }
            case "sidle": {  // Link stepping along a stone wall (the stone message window's pattern), arrows to both sides (#sidle:<picture>)
                Bitmap link = get(c, LINK_SIDLE + k[1], whenReady), stone = get(c, "MsgWindowScrollStone_00^q", whenReady), a = arrow(c, whenReady);
                if (link == null || stone == null || a == null) return null;
                android.graphics.RectF wall = new android.graphics.RectF(0.18f * SIZE, 0.06f * SIZE, 0.82f * SIZE, 0.9f * SIZE);
                android.graphics.Paint edge = new android.graphics.Paint(android.graphics.Paint.ANTI_ALIAS_FLAG);
                edge.setColor(0xFF0A0A14);
                float e = SIZE * 0.025f, rr = SIZE * 0.05f;
                cv.drawRoundRect(new android.graphics.RectF(wall.left - e, wall.top - e, wall.right + e, wall.bottom + e), rr + e, rr + e, edge);
                android.graphics.Paint fill = new android.graphics.Paint(android.graphics.Paint.ANTI_ALIAS_FLAG | android.graphics.Paint.FILTER_BITMAP_FLAG);
                android.graphics.Matrix sm = new android.graphics.Matrix();
                sm.setScale((float) SIZE / stone.getWidth(), (float) SIZE / stone.getHeight());
                android.graphics.BitmapShader sh = new android.graphics.BitmapShader(stone, android.graphics.Shader.TileMode.REPEAT, android.graphics.Shader.TileMode.REPEAT);
                sh.setLocalMatrix(sm);
                fill.setShader(sh);
                cv.drawRoundRect(wall, rr, rr, fill);
                place(cv, link, 0, 0.5f, 0.52f, 0.9f);
                place(cv, a, 270, 0.13f, 0.62f, 0.26f);
                place(cv, a, 90, 0.87f, 0.62f, 0.26f);
                return out;
            }
            case "third": {  // Link from behind, the item he aims held up at his shoulder: the third person view
                Bitmap link = get(c, LINK_BACK, whenReady), item = get(c, k[1], whenReady);
                if (link == null || item == null) return null;
                place(cv, link, 0, 0.42f, 0.5f, 0.95f);
                place(cv, item, 0, 0.66f, 0.40f, 0.5f);
                return out;
            }
            case "grab": {  // Link pushing a block of stone (the stone message window's pattern), as the sidle wall (#grab:<picture>)
                Bitmap link = get(c, LINK_PUSH + k[1], whenReady), stone = get(c, "MsgWindowScrollStone_00^q", whenReady);
                if (link == null || stone == null) return null;
                android.graphics.RectF block = new android.graphics.RectF(0.56f * SIZE, 0.3f * SIZE, 0.94f * SIZE, 0.9f * SIZE);
                android.graphics.Paint edge = new android.graphics.Paint(android.graphics.Paint.ANTI_ALIAS_FLAG);
                edge.setColor(0xFF0A0A14);
                float e = SIZE * 0.025f, rr = SIZE * 0.04f;
                cv.drawRoundRect(new android.graphics.RectF(block.left - e, block.top - e, block.right + e, block.bottom + e), rr + e, rr + e, edge);
                android.graphics.Paint fill = new android.graphics.Paint(android.graphics.Paint.ANTI_ALIAS_FLAG | android.graphics.Paint.FILTER_BITMAP_FLAG);
                android.graphics.Matrix sm = new android.graphics.Matrix();
                sm.setScale((float) SIZE / stone.getWidth(), (float) SIZE / stone.getHeight());
                android.graphics.BitmapShader sh = new android.graphics.BitmapShader(stone, android.graphics.Shader.TileMode.REPEAT, android.graphics.Shader.TileMode.REPEAT);
                sh.setLocalMatrix(sm);
                fill.setShader(sh);
                cv.drawRoundRect(block, rr, rr, fill);
                place(cv, link, 0, 0.33f, 0.56f, 0.8f);
                return out;
            }
            case "carry": {  // Link with the pot he carries and an arrow where it goes (#carry:<picture>:<arrow's angle>)
                String[] p = k[1].split(":");
                Bitmap link = get(c, p[0], whenReady), a = arrow(c, whenReady);
                if (link == null || a == null) return null;
                int deg = Integer.parseInt(p[1]);
                place(cv, link, 0, 0.42f, 0.54f, 0.9f);
                place(cv, a, deg, 0.8f, deg == 0 ? 0.74f : 0.26f, 0.36f);
                return out;
            }
            case "moon": {  // Link's jump and the crescent moon above, ahead of him: the moon jump
                Bitmap link = get(c, k[1], whenReady), moon = get(c, "CrescentMoon_00^t", whenReady);
                if (link == null || moon == null) return null;
                place(cv, link, 0, 0.42f, 0.56f, 0.86f);
                place(cv, moon, 0, 0.78f, 0.22f, 0.38f);
                return out;
            }
            case "sailing": {  // the boat sailing and one arrow ahead (Normal) or two (Fast): #sailing:<picture>:<arrows>
                String[] p = k[1].split(":");
                Bitmap ship = get(c, p[0], whenReady), a = arrow(c, whenReady);
                if (ship == null || a == null) return null;
                place(cv, ship, 0, 0.4f, 0.5f, 0.86f);
                if (p[1].equals("2")) {
                    place(cv, a, 90, 0.74f, 0.72f, 0.34f);
                    place(cv, a, 90, 0.88f, 0.72f, 0.34f);
                } else {
                    place(cv, a, 90, 0.82f, 0.72f, 0.34f);
                }
                return out;
            }
            case "stop": {  // the boat drifting, the sail down, and an arrow ahead into a bar: Stop (#stop:<picture>)
                Bitmap ship = get(c, k[1], whenReady), a = arrow(c, whenReady);
                if (ship == null || a == null) return null;
                place(cv, ship, 0, 0.42f, 0.4f, 0.8f);
                place(cv, a, 90, 0.7f, 0.8f, 0.26f);
                android.graphics.Paint bar = new android.graphics.Paint(android.graphics.Paint.ANTI_ALIAS_FLAG);
                android.graphics.RectF r = new android.graphics.RectF(0.83f * SIZE, 0.68f * SIZE, 0.88f * SIZE, 0.92f * SIZE);
                float e = SIZE * 0.02f, rr = SIZE * 0.015f;
                bar.setColor(0xFF0A0A14);
                cv.drawRoundRect(new android.graphics.RectF(r.left - e, r.top - e, r.right + e, r.bottom + e), rr + e, rr + e, bar);
                bar.setColor(0xFFFFFFFF);
                cv.drawRoundRect(r, rr, rr, bar);
                return out;
            }
            case "speak": {  // Link talking and the speech balloon at his face popping up, its dots coming one by one (#speak:<picture>)
                Bitmap link = get(c, k[1], whenReady), src = get(c, "SubmarineBalloon_00^t", whenReady);
                if (link == null || src == null) return null;
                int i = Integer.parseInt(k[1].substring(k[1].lastIndexOf('_') + 1)), n = pictures(LINK_TALK + "*");
                android.graphics.Matrix flip = new android.graphics.Matrix();
                flip.setScale(-1, 1);  // (its tail to the left, down to him)
                Bitmap b = outlined(Bitmap.createBitmap(src, 0, 0, src.getWidth(), src.getHeight(), flip, true), 0xFF0A0A14, 0, 3, 2);
                android.graphics.Canvas bc = new android.graphics.Canvas(b);
                android.graphics.Paint dot = new android.graphics.Paint(android.graphics.Paint.ANTI_ALIAS_FLAG);
                dot.setColor(0xFF0A0A14);
                int dots = Math.min(3, i * 4 / n);  // (none while it pops up, then one more each quarter)
                for (int d = 0; d < dots; d++)
                    bc.drawCircle(b.getWidth() * (0.5f + (d - 1) * 0.24f), b.getHeight() * 0.25f, b.getWidth() * 0.075f, dot);
                place(cv, link, 0, 0.34f, 0.6f, 0.86f);
                place(cv, b, 0, 0.7f, 0.32f, 0.62f * (0.6f + 0.4f * Math.min(1, (i + 1) / 3f)));
                return out;
            }
            case "exit": {  // a doorway out into the daylight, Link walking away into it (#exit:<picture>): leaving
                Bitmap link = get(c, k[1], whenReady);
                if (link == null) return null;
                int i = Integer.parseInt(k[1].substring(k[1].lastIndexOf('_') + 1)), n = pictures(LINK_WALKAWAY + "*");
                android.graphics.Paint p = new android.graphics.Paint(android.graphics.Paint.ANTI_ALIAS_FLAG);
                android.graphics.RectF door = new android.graphics.RectF(0.2f * SIZE, 0.08f * SIZE, 0.8f * SIZE, 0.92f * SIZE);
                float e = SIZE * 0.025f, rr = SIZE * 0.15f, fw = SIZE * 0.07f;
                p.setColor(0xFF0A0A14);
                cv.drawRoundRect(new android.graphics.RectF(door.left - e, door.top - e, door.right + e, door.bottom + e), rr + e, rr + e, p);
                p.setColor(0xFF7A5430);  // the wooden frame
                cv.drawRoundRect(door, rr, rr, p);
                android.graphics.RectF light = new android.graphics.RectF(door.left + fw, door.top + fw, door.right - fw, door.bottom);
                p.setShader(new android.graphics.LinearGradient(0, light.top, 0, light.bottom, 0xFFBEE1FF, 0xFFFAFAD7, android.graphics.Shader.TileMode.CLAMP));
                cv.drawRoundRect(light, rr - fw * 0.6f, rr - fw * 0.6f, p);
                cv.drawRect(light.left, light.bottom - rr, light.right, light.bottom, p);  // (square at the floor)
                float t = n > 1 ? (float) i / (n - 1) : 0;  // going away: smaller, higher up
                place(cv, link, 0, 0.5f, 0.62f - 0.08f * t, 0.8f - 0.25f * t);
                return out;
            }
            case "check": {  // Link leaning in to peer through a magnifying glass, it sweeping once to his eye, a glint crossing it (#check:<picture>)
                Bitmap link = get(c, k[1], whenReady);
                if (link == null) return null;
                int i = Integer.parseInt(k[1].substring(k[1].lastIndexOf('_') + 1)), n = pictures(LINK_CHECK + "*");
                float t = n > 1 ? (float) i / (n - 1) : 1;
                place(cv, link, 0, 0.36f, 0.58f, 0.86f);
                lens(cv, 0.72f + 0.05f * (float) Math.sin(t * 2 * Math.PI), 0.36f - 0.04f * t, 0.17f, t > 0.15f ? t * 1.4f - 0.2f : -1);
                return out;
            }
            case "lens": {  // the magnifying glass alone (Link's poses off)
                lens(cv, 0.6f, 0.4f, 0.26f, -1);
                return out;
            }
            case "ff": {  // two arrows to the right, as Next's: fast forward
                Bitmap a = arrow(c, whenReady);
                if (a == null) return null;
                place(cv, a, 90, 0.33f, 0.5f, 0.5f);
                place(cv, a, 90, 0.67f, 0.5f, 0.5f);
                return out;
            }
            case "putaway": {
                Bitmap sw = get(c, k[1], whenReady), a = arrow(c, whenReady);
                if (sw == null || a == null) return null;
                place(cv, sw, 200, 0.42f, 0.44f, 0.66f);
                place(cv, a, 0, 0.76f, 0.72f, 0.42f);
                return out;
            }
            case "stow": {  // an item held in the hand (upright) and the arrow down: put it away
                Bitmap item = get(c, k[1], whenReady), a = arrow(c, whenReady);
                if (item == null || a == null) return null;
                place(cv, item, 0, 0.42f, 0.42f, 0.66f);
                place(cv, a, 0, 0.76f, 0.72f, 0.42f);
                return out;
            }
            case "shoot": {  // an aimed item and an arrow away from it, up and to the right: shoot / throw / fire
                Bitmap item = get(c, k[1], whenReady), a = arrow(c, whenReady);
                if (item == null || a == null) return null;
                place(cv, item, 0, 0.40f, 0.58f, 0.62f);
                place(cv, a, 135, 0.74f, 0.28f, 0.40f);
                return out;
            }
            case "ship": {
                Bitmap ship = get(c, "BattleshipIcon_00^l", whenReady), a = arrow(c, whenReady);
                if (ship == null || a == null) return null;
                place(cv, ship, 0, 0.5f, 0.6f, 0.75f);
                boolean in = k[1].equals("0");
                place(cv, a, in ? 0 : 180, in ? 0.5f : 0.78f, 0.22f, 0.38f);
                return out;
            }
            case "turn": {  // an item inside a turning arrow (the Picto Box's selfie)
                Bitmap item = get(c, k[1], whenReady);
                if (item == null) return null;
                place(cv, item, 0, 0.5f, 0.5f, 0.56f);
                android.graphics.RectF ring = new android.graphics.RectF(0.07f * SIZE, 0.07f * SIZE, 0.93f * SIZE, 0.93f * SIZE);
                android.graphics.Paint arc = new android.graphics.Paint(android.graphics.Paint.ANTI_ALIAS_FLAG);
                arc.setStyle(android.graphics.Paint.Style.STROKE);
                arc.setStrokeCap(android.graphics.Paint.Cap.ROUND);
                // two arcs, each with a head: a full turn around the item
                float cx = SIZE / 2f, r = 0.43f * SIZE;
                for (int pass = 0; pass < 2; pass++) {
                    arc.setColor(pass == 0 ? 0xFF0A0A14 : 0xFFFFFFFF);
                    arc.setStrokeWidth(SIZE * (pass == 0 ? 0.12f : 0.055f));
                    android.graphics.Paint head = new android.graphics.Paint(android.graphics.Paint.ANTI_ALIAS_FLAG);
                    head.setColor(arc.getColor());
                    head.setStyle(android.graphics.Paint.Style.FILL_AND_STROKE);
                    head.setStrokeJoin(android.graphics.Paint.Join.ROUND);
                    head.setStrokeWidth(pass == 0 ? SIZE * 0.06f : 0);
                    for (int half = 0; half < 2; half++) {
                        float start = -150 + half * 180;
                        cv.drawArc(ring, start, 120, false, arc);
                        double end = Math.toRadians(start + 120);
                        float ex = cx + (float) Math.cos(end) * r, ey = cx + (float) Math.sin(end) * r;
                        // the head points along the circle (clockwise)
                        float tx = (float) -Math.sin(end), ty = (float) Math.cos(end), nx = (float) Math.cos(end), ny = (float) Math.sin(end);
                        float hl = SIZE * 0.11f, hw = SIZE * 0.09f;
                        android.graphics.Path p = new android.graphics.Path();
                        p.moveTo(ex + tx * hl, ey + ty * hl);
                        p.lineTo(ex + nx * hw, ey + ny * hw);
                        p.lineTo(ex - nx * hw, ey - ny * hw);
                        p.close();
                        cv.drawPath(p, head);
                    }
                }
                return out;
            }
            case "cross": {  // a red cross with a dark edge (no / cancel)
                android.graphics.Paint x = new android.graphics.Paint(android.graphics.Paint.ANTI_ALIAS_FLAG);
                x.setStyle(android.graphics.Paint.Style.STROKE);
                x.setStrokeCap(android.graphics.Paint.Cap.ROUND);
                float a = 0.27f * SIZE, b = 0.73f * SIZE;
                x.setColor(0xFF0A0A14);
                x.setStrokeWidth(SIZE * 0.19f);
                cv.drawLine(a, a, b, b, x);
                cv.drawLine(a, b, b, a, x);
                x.setColor(0xFFE0473A);
                x.setStrokeWidth(SIZE * 0.11f);
                cv.drawLine(a, a, b, b, x);
                cv.drawLine(a, b, b, a, x);
                return out;
            }
            case "delete": {  // a photo card with a red cross (the album's Delete)
                android.graphics.Paint card = new android.graphics.Paint(android.graphics.Paint.ANTI_ALIAS_FLAG);
                android.graphics.Paint edge = new android.graphics.Paint(android.graphics.Paint.ANTI_ALIAS_FLAG);
                edge.setStyle(android.graphics.Paint.Style.STROKE);
                edge.setStrokeWidth(SIZE * 0.035f);
                edge.setColor(0xFF0A0A14);
                card.setColor(0xFFFFFFFF);
                android.graphics.RectF r = new android.graphics.RectF(0.2f * SIZE, 0.28f * SIZE, 0.8f * SIZE, 0.72f * SIZE);
                cv.save();
                cv.rotate(-6, SIZE / 2f, SIZE / 2f);
                cv.drawRoundRect(r, SIZE * 0.03f, SIZE * 0.03f, card);
                cv.drawRoundRect(r, SIZE * 0.03f, SIZE * 0.03f, edge);
                card.setColor(0xFF8FC8F0);
                cv.drawRect(r.left + SIZE * 0.05f, r.top + SIZE * 0.05f, r.right - SIZE * 0.05f, r.bottom - SIZE * 0.05f, card);
                cv.restore();
                android.graphics.Paint x = new android.graphics.Paint(android.graphics.Paint.ANTI_ALIAS_FLAG);
                x.setStyle(android.graphics.Paint.Style.STROKE);
                x.setStrokeCap(android.graphics.Paint.Cap.ROUND);
                float a = 0.3f * SIZE, b = 0.7f * SIZE;
                x.setColor(0xFF0A0A14);
                x.setStrokeWidth(SIZE * 0.16f);
                cv.drawLine(a, a, b, b, x);
                cv.drawLine(a, b, b, a, x);
                x.setColor(0xFFE0473A);
                x.setStrokeWidth(SIZE * 0.09f);
                cv.drawLine(a, a, b, b, x);
                cv.drawLine(a, b, b, a, x);
                return out;
            }
            case "album": {  // two photo cards (white, dark edge like the arrows) behind the Picto Box
                Bitmap box = get(c, "Icon128_12^l", whenReady);
                if (box == null) return null;
                android.graphics.Paint card = new android.graphics.Paint(android.graphics.Paint.ANTI_ALIAS_FLAG);
                android.graphics.Paint edge = new android.graphics.Paint(android.graphics.Paint.ANTI_ALIAS_FLAG);
                edge.setStyle(android.graphics.Paint.Style.STROKE);
                edge.setStrokeWidth(SIZE * 0.035f);
                edge.setStrokeJoin(android.graphics.Paint.Join.ROUND);
                edge.setColor(0xFF0A0A14);
                card.setColor(0xFFFFFFFF);
                android.graphics.RectF r = new android.graphics.RectF(-0.27f * SIZE, -0.2f * SIZE, 0.27f * SIZE, 0.2f * SIZE);
                float[][] at = {{0.42f, 0.36f, -12}, {0.58f, 0.42f, 8}};
                for (float[] p : at) {
                    cv.save();
                    cv.translate(p[0] * SIZE, p[1] * SIZE);
                    cv.rotate(p[2]);
                    cv.drawRoundRect(r, SIZE * 0.03f, SIZE * 0.03f, card);
                    cv.drawRoundRect(r, SIZE * 0.03f, SIZE * 0.03f, edge);
                    android.graphics.Paint sky = new android.graphics.Paint(android.graphics.Paint.ANTI_ALIAS_FLAG);
                    sky.setColor(0xFF8FC8F0);
                    cv.drawRect(r.left + SIZE * 0.05f, r.top + SIZE * 0.05f, r.right - SIZE * 0.05f, r.bottom - SIZE * 0.05f, sky);
                    cv.restore();
                }
                place(cv, box, 0, 0.5f, 0.68f, 0.56f);
                return out;
            }
            case "arrows": {
                String[] t = {"NormalArrow_00^l", "FireArrow_00^l", "IceArrow_00^l"};
                float[][] at = {{0.32f, 0.36f}, {0.68f, 0.36f}, {0.5f, 0.68f}};
                for (int i = 0; i < 3; i++) {
                    Bitmap b = get(c, t[i], whenReady);
                    if (b == null) return null;
                    place(cv, b, 0, at[i][0], at[i][1], 0.5f);
                }
                return out;
            }
            case "baton": {  // a conducting note sign as the game's row shows it (BatonSign_00): "dir:lit"
                String[] a = k[1].split(":");
                int dir = Integer.parseInt(a[0]);
                boolean lit = a[1].equals("1");
                Bitmap base = get(c, "BatonSignBase_00^t", whenReady), sign = get(c, dir == 0 ? "BatonSign_00^t" : "BatonSignArrow_00^t", whenReady),
                        frame = get(c, "BatonSignFrame_00^t", whenReady);
                if (base == null || sign == null || frame == null) return null;
                // the layout's materials: the base black at half, the sign yellow (unlit: grey), the frame white at 200
                Bitmap one = Bitmap.createBitmap(base.getWidth() * 2, base.getHeight(), Bitmap.Config.ARGB_8888);
                android.graphics.Canvas oc = new android.graphics.Canvas(one);
                drawMirrored(oc, base, 0xFF000000, 128);
                drawMirrored(oc, sign, lit ? 0xFFFFE100 : 0xFFE6E6E6, lit ? 255 : 235);
                drawMirrored(oc, frame, 0xFFFFFFFF, 200);
                place(cv, one, dir == 2 ? -90 : dir == 3 ? 180 : dir == 4 ? 90 : 0, 0.5f, 0.5f, 1f);  // (the arrow sign points up)
                return out;
            }
            case "batonbar": {  // the time bar's dark base (BatonInput_00), whole: 560 x 46
                Bitmap bar = get(c, "BatonInputPointBase_00^t", whenReady);
                if (bar == null) return null;
                Bitmap whole = Bitmap.createBitmap(bar.getWidth() * 2, bar.getHeight(), Bitmap.Config.ARGB_8888);
                drawMirrored(new android.graphics.Canvas(whole), bar, 0xFFFFFFFF, 220);
                return whole;
            }
            case "batonpoint": {  // the time bar's input point (BatonInputPoint_00), whole, in a colour: "rrggbb"
                Bitmap pt = get(c, "BatonInputPoint_00^t", whenReady);
                if (pt == null) return null;
                Bitmap whole = Bitmap.createBitmap(pt.getWidth() * 2, pt.getHeight(), Bitmap.Config.ARGB_8888);
                drawMirrored(new android.graphics.Canvas(whole), pt, 0xFF000000 | Integer.parseInt(k[1], 16), 255);
                return whole;
            }
            default:
                return null;
        }
    }

    /** a half texture the layouts mirror (wrap mode mirror): it and its mirror image side by side, its colours
     *  multiplied by color (a material's white colour), at alpha */
    private static void drawMirrored(android.graphics.Canvas cv, Bitmap half, int color, int alpha) {
        android.graphics.Paint p = new android.graphics.Paint(android.graphics.Paint.FILTER_BITMAP_FLAG);
        float r = ((color >> 16) & 0xFF) / 255f, g = ((color >> 8) & 0xFF) / 255f, b = (color & 0xFF) / 255f;
        p.setColorFilter(new android.graphics.ColorMatrixColorFilter(new float[] {
                r, 0, 0, 0, 0, 0, g, 0, 0, 0, 0, 0, b, 0, 0, 0, 0, 0, alpha / 255f, 0}));
        cv.drawBitmap(half, 0, 0, p);
        android.graphics.Matrix m = new android.graphics.Matrix();
        m.setScale(-1, 1);
        m.postTranslate(half.getWidth() * 2, 0);
        cv.drawBitmap(half, m, p);
    }

    /** draws b rotated by deg (counter-clockwise, as the mockup), centred at (fx, fy), longest side = fs */
    /** a magnifying glass (golden rim, light blue glass, wooden handle down to the left, a dark edge) centred at
     *  (fx, fy) with radius fr (in SIZE); glint 0..1: a white streak crossing the glass, < 0 none */
    private static void lens(android.graphics.Canvas cv, float fx, float fy, float fr, float glint) {
        float cx = fx * SIZE, cy = fy * SIZE, r = fr * SIZE, e = r * 0.16f, ri = r * 0.8f, hw = r * 0.36f;
        double a = Math.toRadians(125);
        float hx0 = cx + (float) Math.cos(a) * r * 0.95f, hy0 = cy + (float) Math.sin(a) * r * 0.95f;
        float hx1 = cx + (float) Math.cos(a) * r * 2.05f, hy1 = cy + (float) Math.sin(a) * r * 2.05f;
        android.graphics.Paint p = new android.graphics.Paint(android.graphics.Paint.ANTI_ALIAS_FLAG);
        p.setStrokeCap(android.graphics.Paint.Cap.ROUND);
        p.setColor(0xFF0A0A14);
        p.setStrokeWidth(hw + 2 * e);
        cv.drawLine(hx0, hy0, hx1, hy1, p);
        cv.drawCircle(cx, cy, r + e, p);
        p.setColor(0xFF7A5430);  // the handle: wood, as the doorway's frame
        p.setStrokeWidth(hw);
        cv.drawLine(hx0, hy0, hx1, hy1, p);
        p.setColor(0xFFFAD65A);  // the rim
        cv.drawCircle(cx, cy, r, p);
        p.setColor(0xFF0A0A14);
        cv.drawCircle(cx, cy, ri + e * 0.5f, p);
        p.setColor(0xFFC8E8FF);  // the glass
        cv.drawCircle(cx, cy, ri, p);
        cv.save();
        android.graphics.Path clip = new android.graphics.Path();
        clip.addCircle(cx, cy, ri, android.graphics.Path.Direction.CW);
        cv.clipPath(clip);
        if (glint >= 0) {
            float off = (glint * 2.6f - 1.3f) * ri, w = ri * 0.32f;
            android.graphics.Path g = new android.graphics.Path();
            g.moveTo(cx + off - 2 * ri, cy + 2 * ri);
            g.lineTo(cx + off - 2 * ri + w, cy + 2 * ri);
            g.lineTo(cx + off + 2 * ri + w, cy - 2 * ri);
            g.lineTo(cx + off + 2 * ri, cy - 2 * ri);
            g.close();
            p.setColor(0xE6FFFFFF);
            cv.drawPath(g, p);
        }
        cv.restore();
        p.setStyle(android.graphics.Paint.Style.STROKE);  // a small fixed shine at its top left
        p.setStrokeWidth(r * 0.12f);
        p.setColor(0xFFFFFFFF);
        float s = ri * 0.7f;
        cv.drawArc(new android.graphics.RectF(cx - s, cy - s, cx + s, cy + s), 200, 50, false, p);
    }

    private static void place(android.graphics.Canvas cv, Bitmap b, float deg, float fx, float fy, float fs) {
        float sc = fs * SIZE / Math.max(b.getWidth(), b.getHeight());
        android.graphics.Matrix m = new android.graphics.Matrix();
        m.postTranslate(-b.getWidth() / 2f, -b.getHeight() / 2f);
        m.postRotate(-deg);
        m.postScale(sc, sc);
        m.postTranslate(fx * SIZE, fy * SIZE);
        cv.drawBitmap(b, m, new android.graphics.Paint(android.graphics.Paint.FILTER_BITMAP_FLAG | android.graphics.Paint.ANTI_ALIAS_FLAG));
    }

    /** the message arrow (pointing down) with a thick dark outline */
    private static Bitmap arrow(Context c, Runnable whenReady) {
        return arrow(c, 0, whenReady);
    }

    /** ... its white multiplied by fill (0: as it is) */
    private static Bitmap arrow(Context c, int fill, Runnable whenReady) {
        return outlined(get(c, "MsgArrow_00^s", whenReady), 0xFF0A0A14, fill, 4, 4);
    }

    /** src (white) enlarged scale times with an outline of r source pixels in ink, its fill multiplied (0: as it is),
     *  or null for null */
    private static Bitmap outlined(Bitmap src, int ink0, int fill, int scale, int rSrc) {
        if (src == null) return null;
        int r = rSrc * scale, pad = r + 4;
        int w = src.getWidth() * scale + 2 * pad, h = src.getHeight() * scale + 2 * pad;
        Bitmap big = Bitmap.createBitmap(w, h, Bitmap.Config.ARGB_8888);
        android.graphics.Canvas cv = new android.graphics.Canvas(big);
        android.graphics.Rect s = new android.graphics.Rect(0, 0, src.getWidth(), src.getHeight());
        android.graphics.Paint ink = new android.graphics.Paint(android.graphics.Paint.FILTER_BITMAP_FLAG | android.graphics.Paint.ANTI_ALIAS_FLAG);
        ink.setColorFilter(new android.graphics.PorterDuffColorFilter(ink0, android.graphics.PorterDuff.Mode.SRC_IN));
        // the outline: the arrow's shape stamped around a circle of radius r (and inside it)
        for (int rr = r; rr > 0; rr -= scale * 2)
            for (int i = 0; i < 32; i++) {
                double t = i * Math.PI / 16;
                float dx = (float) (Math.cos(t) * rr), dy = (float) (Math.sin(t) * rr);
                cv.drawBitmap(src, s, new android.graphics.RectF(pad + dx, pad + dy, w - pad + dx, h - pad + dy), ink);
            }
        android.graphics.Paint top = new android.graphics.Paint(android.graphics.Paint.FILTER_BITMAP_FLAG | android.graphics.Paint.ANTI_ALIAS_FLAG);
        if (fill != 0) top.setColorFilter(new android.graphics.PorterDuffColorFilter(fill, android.graphics.PorterDuff.Mode.MULTIPLY));
        cv.drawBitmap(src, s, new android.graphics.RectF(pad, pad, w - pad, h - pad), top);
        return big;
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
        Native.renderLinkIcons(gameDir, raw.getAbsolutePath());
        java.util.List<String> all = new java.util.ArrayList<>(java.util.Arrays.asList(textures));
        java.util.Collections.addAll(all, LINK_JUMP, LINK_CROUCH, LINK_STAND, LINK_BACK);
        for (Object[] a : ANIMATED)
            for (int i = 0; i < (Integer) a[1]; i++) all.add((String) a[0] + i);
        for (int i = 0; i <= LINK_ROLL_REST; i++) all.add(LINK_ROLL + i);
        for (String t : all) {
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
