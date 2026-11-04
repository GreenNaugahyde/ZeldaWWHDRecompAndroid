package org.wwhdrecomp.app;

import android.content.Context;
import android.content.SharedPreferences;

/**
 * Where each thing the game progress counts (GameProgress) is found: its square of the sea chart (1..49; 0: no fixed
 * one, as the Ghost Ship's), the stage inside if it is in one (a dungeon's are drawn on its map), its spot (world x / y
 * / z of that stage, NaN: none drawn) and the save flag that says it was found (Game > Game progress > Map,
 * ProgressMapView). In the unchanged game each thing has one place of its own, so a flag set is "found there".
 * <p>
 * The table (TABLE .. END TABLE) is made on the host from the game files (notes/progress-map-tools: dump.py reads every
 * stage's actors, gen.py joins them with the hand-made part and checks each flag against a 100% save):
 * <ul>
 * <li>Chests: TRES of stage.dzs / room.dzr (number params >> 7 &amp; 0x1F, item angle.z >> 8), flag the stage's save
 *     table (STAG) dSv_memBit_c mTbox bit; the sea is table 0, the submarines 10, the caves 12 / 13.</li>
 * <li>Sunken treasures: the sea rooms' Salvage actors of kind 0 (params >> 28): chart number params >> 20, item
 *     params >> 4, four points by angle.z &amp; 3, the one in use dSv_player_info_c mRandomSalvagePoint (info + 0x59);
 *     the chart's treasure salvaged is dSv_player_map_c set 3. Light rings that come after a Big Octo / gunboat
 *     (SwSlvg, kind 2): dSv_ocean_c (save + 0x5C0, u16 per square) bit params >> 20.</li>
 * <li>On the ground: item actors' item bit (params >> 8), the sea's mItem (table + 0x14).</li>
 * <li>Rewards: event flags from the GameCube decomp (auction l_item_dat: 0x1020; Beedle's ship: 0x2010; letters: their
 *     event register = 3 when read, d_letter.cpp) and the GameCube set's code notes on retroachievements.org (Sam
 *     0x1B10, Maggie's letter mailed 0x1220); the charts' numbers from the same notes (dSv_player_map_c bit = the chart
 *     number: Lenzo's chest is chart 25 in both games), HD's own 42..46 from the wikis' lists.</li>
 * <li>From the HD code (cking.rpx; dSv_event_c onEventBit 025B8B68, isEventBit 025B8B94, setEventReg 025B8AF4): the
 *     Squid-Hunt (daNpc_Kg1) counts its wins in register 0xFE07 and gives {Piece of Heart, chart 51, Purple Rupee}
 *     by count - 1; the cannon game (daNpc_Kg2) counts in 0xB703, its prizes {-, Piece of Heart, chart 40} by the count
 *     after the win: both hearts are the first win (count >= 1). The withered trees (d_a_obj_ftree, 02346534) count a
 *     tree as watered by its bit of 0x9EFF or event 0x0102, and clear bits again: 0x0102 is the trees done.</li>
 * </ul>
 * A WEAK flag (Maggie's: set when her letter is mailed, the reward may come later) counts by the number of pieces too:
 * what the pieces Link has leave over after the sure ones are "found, not sure which" (MAYBE) among the weak ones not
 * set. A 100% save and a mid-game one match the flags exactly (44 = 43 + Maggie's, 21 = 21). The four bottles have no flags of their own but the submarine's chest and Beedle's sale; the others
 * count in story order (Medli first).
 */
final class ProgressPlaces {
    private ProgressPlaces() {}

    // what a place holds
    static final int HEART = 0, CONTAINER = 1, MAP = 2, COMPASS = 3, BIGKEY = 4, BOSS = 5, CHART = 6, SALVAGE = 7, SQUARE = 8,
            ITEM = 9, ARROWS = 10, BOTTLE = 11, EQUIP = 12, UPGRADE = 13, SONG = 14, PEARL = 15, SHARD = 16, GALLERY = 17;
    // the map's choice of things: everything, hearts, charts and treasure, items, dungeons
    static final int ALL_KINDS = 0, HEARTS = 1, CHARTS = 2, ITEMS = 3, DUNGEONS = 4;
    // the spoiler log's map (SpoilerLog): only the places with things the game can't be finished without
    static final int IMPORTANT = 5;
    // flags: a stage table's chest / memory item bit / dungeon item bit, a square's ocean bit, an event flag 0xBBMM, an
    // event register (all of mask b; its bits in mask b >> 8 at least b & 0xFF), a chart got / its treasure salvaged, a
    // square filled in; the rest are had
    private static final int F_CHEST = 0, F_ITEMBIT = 1, F_DUNGEON = 2, F_OCEAN = 3, F_EVENT = 4, F_REG = 5, F_REGGE = 6, F_CHART = 7,
            F_SALVAGED = 8, F_SQUARE = 9, F_ITEM = 10, F_ARROWS = 11, F_BOTTLE = 12, F_EQUIP = 13, F_UPGRADE = 14, F_SONG = 15,
            F_PEARL = 16, F_SHARD = 17, F_GALLERY = 18;
    // how it is found (the list's words)
    static final int S_CHEST = 0, S_SUB = 1, S_SUNKEN = 2, S_OCTO = 3, S_GUNBOAT = 4, S_GROUND = 5, S_REWARD = 6, S_AUCTION = 7,
            S_MINIGAME = 8, S_LETTER = 9, S_SHOP = 10, S_BOSS = 11, S_FISHMAN = 12, S_FAIRY = 13, S_UNKNOWN = 14;

    static final int MISSING = 0, FOUND = 1, MAYBE = 2;

    // dSv_save_c offsets (GameProgress has the same)
    private static final int MAX_LIFE = 0x00, WALLET = 0x12, MAX_MAGIC = 0x13, ITEMS_AT = 0x3C, GOT_ITEMS = 0x51, ARROW_MAX = 0x6F,
            BOMB_MAX = 0x70, COLLECT = 0xB4, TACT = 0xBD, TRIFORCE = 0xBE, PEARLS = 0xBF, MAP_BITS = 0xC4, FMAP = MAP_BITS + 0x40,
            INFO = 0x148, STAGES = 0x380, STAGE_SIZE = 0x24, OCEAN = 0x5C0, EVENTS = 0x624, STAGE_COPY = 0x778, STAGE_NO = 0x79C;
    private static final int[] FIGURE_BYTES = {0x95, 0x94, 0x93, 0x92, 0x91, 0x90, 0x8F, 0x8E, 0x8D, 0x8C, 0xB1, 0x9C, 0x84, 0x83,
            0x82, 0x81, 0x80};

    static final class Place {
        final int kind, what, square;
        final String stage;         // null: on the sea (x / z on the sea's world), else the stage inside
        final float x, y, z;
        final int flag, a, b, src;
        final boolean weak;
        final int index;

        Place(int index, int kind, int what, int square, String stage, float x, float y, float z, int flag, int a, int b, int src,
              boolean weak) {
            this.index = index;
            this.kind = kind;
            this.what = what;
            this.square = square;
            this.stage = stage;
            this.x = x;
            this.y = y;
            this.z = z;
            this.flag = flag;
            this.a = a;
            this.b = b;
            this.src = src;
            this.weak = weak;
        }

        // the randomizer's spoiler log (SpoilerLog.place): its item's icon, whether the game needs it, its words
        String tex;
        boolean important = true;
        String label;

        /** the chart whose sunken treasure this is (its spot is the salvage point in use), 0 if none */
        int sunkenChart() { return flag == F_SALVAGED ? a : flag == F_SHARD ? b : 0; }

        boolean inGroup(int group) {
            switch (group) {
                case HEARTS: return kind == HEART || kind == CONTAINER;
                case CHARTS: return kind == CHART || kind == SALVAGE || kind == SQUARE;
                case ITEMS: return kind >= ITEM;
                case DUNGEONS: return kind >= MAP && kind <= BOSS || kind == CONTAINER;
                case IMPORTANT: return important;
                default: return true;
            }
        }

        /** a key that stays across app versions (the found-at log) */
        String key() { return kind + ":" + what + ":" + flag + ":" + a + ":" + b; }
    }

    private static int n;

    /** a place of the randomizer's spoiler log (SpoilerLog): its own index (into the log's states), no save flag */
    static Place spoiler(int index, int kind, int square, String stage, float x, float y, float z, String tex, boolean important, String label) {
        Place p = new Place(index, kind, 0, square, stage, x, y, z, -1, 0, 0, S_UNKNOWN, false);
        p.tex = tex;
        p.important = important;
        p.label = label;
        return p;
    }

    private static Place p(int kind, int what, int square, String stage, float x, float y, float z, int flag, int a, int b, int src) {
        return p(kind, what, square, stage, x, y, z, flag, a, b, src, false);
    }

    private static Place p(int kind, int what, int square, String stage, float x, float y, float z, int flag, int a, int b, int src,
                           boolean weak) {
        return new Place(n++, kind, what, square, stage, x, y, z, flag, a, b, src, weak);
    }

    private static final float NaN = Float.NaN;

    // TABLE (tools: gen.py; the order is the list's within a square)
    static final Place[] ALL = {
            p(HEART, 0x7, 49, "Abship", 0f, -550f, -3800f, F_CHEST, 0xa, 0x1, S_SUB),
            p(HEART, 0x7, 25, "Abship", 0f, -550f, -3800f, F_CHEST, 0xa, 0x0, S_SUB),
            p(HEART, 0x7, 34, "Cave01", 4315f, 124f, -150f, F_CHEST, 0xc, 0x5, S_CHEST),
            p(HEART, 0x7, 2, "Cave02", 2500f, 27f, 0f, F_CHEST, 0xc, 0x6, S_CHEST),
            p(HEART, 0x7, 12, "TyuTyu", -4777f, -200f, 4132f, F_CHEST, 0xc, 0x1a, S_CHEST),
            p(HEART, 0x7, 1, "majroom", -3247f, 728f, 3003f, F_CHEST, 0x2, 0x3, S_CHEST),
            p(HEART, 0x7, 11, null, -4013f, 118f, -194453f, F_CHEST, 0x0, 0xa, S_CHEST),
            p(HEART, 0x7, 23, null, -200774f, 698f, 940f, F_CHEST, 0x0, 0x6, S_CHEST),
            p(HEART, 0x7, 29, null, -279600f, 452f, 124900f, F_CHEST, 0x0, 0x3, S_CHEST),
            p(HEART, 0x7, 31, null, -89600f, 2100f, 104600f, F_CHEST, 0x0, 0x14, S_CHEST),
            p(HEART, 0x7, 47, null, 99925f, 1050f, 320240f, F_CHEST, 0x0, 0x0, S_CHEST),
            p(HEART, 0x7, 46, null, NaN, NaN, NaN, F_SALVAGED, 0x8, 0x0, S_SUNKEN),
            p(HEART, 0x7, 5, null, NaN, NaN, NaN, F_SALVAGED, 0x9, 0x0, S_SUNKEN),
            p(HEART, 0x7, 47, null, NaN, NaN, NaN, F_SALVAGED, 0xa, 0x0, S_SUNKEN),
            p(HEART, 0x7, 12, null, NaN, NaN, NaN, F_SALVAGED, 0xb, 0x0, S_SUNKEN),
            p(HEART, 0x7, 34, null, NaN, NaN, NaN, F_SALVAGED, 0xc, 0x0, S_SUNKEN),
            p(HEART, 0x7, 28, null, NaN, NaN, NaN, F_SALVAGED, 0xd, 0x0, S_SUNKEN),
            p(HEART, 0x7, 36, null, NaN, NaN, NaN, F_SALVAGED, 0xe, 0x0, S_SUNKEN),
            p(HEART, 0x7, 41, null, NaN, NaN, NaN, F_SALVAGED, 0xf, 0x0, S_SUNKEN),
            p(HEART, 0x7, 49, null, NaN, NaN, NaN, F_SALVAGED, 0x10, 0x0, S_SUNKEN),
            p(HEART, 0x7, 16, null, NaN, NaN, NaN, F_SALVAGED, 0x11, 0x0, S_SUNKEN),
            p(HEART, 0x7, 22, null, NaN, NaN, NaN, F_SALVAGED, 0x12, 0x0, S_SUNKEN),
            p(HEART, 0x7, 39, null, NaN, NaN, NaN, F_SALVAGED, 0x1e, 0x0, S_SUNKEN),
            p(HEART, 0x7, 6, null, 184800f, -5000f, -283200f, F_OCEAN, 0x6, 0x0, S_OCTO),
            p(HEART, 0x7, 16, null, -214774f, -5000f, -80449f, F_OCEAN, 0x10, 0x0, S_GUNBOAT),
            p(HEART, 0x7, 17, null, -103800f, -5000f, -121400f, F_OCEAN, 0x11, 0x0, S_OCTO),
            p(HEART, 0x7, 44, null, NaN, NaN, NaN, F_ITEMBIT, 0x0, 0x2, S_GROUND),
            p(HEART, 0x7, 45, null, -79300f, 4200f, 319000f, F_ITEMBIT, 0x0, 0x8, S_GROUND),
            p(HEART, 0x7, 44, null, NaN, NaN, NaN, F_REGGE, 0xd0, 0xff03, S_REWARD),
            p(HEART, 0x7, 11, null, NaN, NaN, NaN, F_EVENT, 0x1340, 0x0, S_REWARD),
            p(HEART, 0x7, 11, null, NaN, NaN, NaN, F_EVENT, 0x1220, 0x0, S_REWARD, true),
            p(HEART, 0x7, 11, null, NaN, NaN, NaN, F_EVENT, 0x1b20, 0x0, S_REWARD),
            p(HEART, 0x7, 11, null, NaN, NaN, NaN, F_EVENT, 0x1020, 0x0, S_AUCTION),
            p(HEART, 0x7, 11, null, NaN, NaN, NaN, F_EVENT, 0x1b10, 0x0, S_REWARD),
            p(HEART, 0x7, 11, null, NaN, NaN, NaN, F_REGGE, 0xfe, 0x701, S_MINIGAME),
            p(HEART, 0x7, 11, null, NaN, NaN, NaN, F_EVENT, 0x2280, 0x0, S_REWARD),
            p(HEART, 0x7, 13, null, NaN, NaN, NaN, F_REG, 0xae, 0x3, S_LETTER),
            p(HEART, 0x7, 13, null, NaN, NaN, NaN, F_REG, 0xac, 0x3, S_LETTER),
            p(HEART, 0x7, 13, null, NaN, NaN, NaN, F_REG, 0xb5, 0x3, S_LETTER),
            p(HEART, 0x7, 23, null, NaN, NaN, NaN, F_EVENT, 0x3e04, 0x0, S_REWARD),
            p(HEART, 0x7, 41, null, NaN, NaN, NaN, F_EVENT, 0x102, 0x0, S_REWARD),
            p(HEART, 0x7, 10, null, NaN, NaN, NaN, F_REGGE, 0xb7, 0x301, S_MINIGAME),
            p(HEART, 0x7, 16, null, NaN, NaN, NaN, F_EVENT, 0x2010, 0x0, S_SHOP),
            p(HEART, 0x7, 14, null, NaN, NaN, NaN, F_EVENT, 0x2b40, 0x0, S_MINIGAME),
            p(MAP, 0x2, 1, "majroom", -2630f, 1704f, 4642f, F_DUNGEON, 0x2, 0x0, S_CHEST),
            p(COMPASS, 0x2, 1, "majroom", 4502f, 1714f, 4892f, F_DUNGEON, 0x2, 0x1, S_CHEST),
            p(BOSS, 0x2, 1, "M2tower", NaN, NaN, NaN, F_DUNGEON, 0x2, 0x3, S_BOSS),
            p(CONTAINER, 0x2, 1, "M2tower", NaN, NaN, NaN, F_DUNGEON, 0x2, 0x4, S_BOSS),
            p(MAP, 0x3, 13, "M_NewD2", 4717f, 25f, 1219f, F_DUNGEON, 0x3, 0x0, S_CHEST),
            p(COMPASS, 0x3, 13, "M_NewD2", -2018f, 2875f, -2421f, F_DUNGEON, 0x3, 0x1, S_CHEST),
            p(BIGKEY, 0x3, 13, "M_NewD2", 6439f, -116f, -2036f, F_DUNGEON, 0x3, 0x2, S_CHEST),
            p(BOSS, 0x3, 13, "M_DragB", NaN, NaN, NaN, F_DUNGEON, 0x3, 0x3, S_BOSS),
            p(CONTAINER, 0x3, 13, "M_DragB", NaN, NaN, NaN, F_DUNGEON, 0x3, 0x4, S_BOSS),
            p(MAP, 0x4, 41, "kindan", 1854f, 2738f, 13561f, F_DUNGEON, 0x4, 0x0, S_CHEST),
            p(COMPASS, 0x4, 41, "kindan", -6950f, 6012f, -654f, F_DUNGEON, 0x4, 0x1, S_CHEST),
            p(BIGKEY, 0x4, 41, "kindan", -5681f, 563f, -732f, F_DUNGEON, 0x4, 0x2, S_CHEST),
            p(BOSS, 0x4, 41, "kinBOSS", NaN, NaN, NaN, F_DUNGEON, 0x4, 0x3, S_BOSS),
            p(CONTAINER, 0x4, 41, "kinBOSS", NaN, NaN, NaN, F_DUNGEON, 0x4, 0x4, S_BOSS),
            p(MAP, 0x5, 26, "Siren", 5170f, -1554f, 2010f, F_DUNGEON, 0x5, 0x0, S_CHEST),
            p(COMPASS, 0x5, 26, "Siren", -5054f, -1591f, -2370f, F_DUNGEON, 0x5, 0x1, S_CHEST),
            p(BIGKEY, 0x5, 26, "Siren", -4f, 7650f, -8354f, F_DUNGEON, 0x5, 0x2, S_CHEST),
            p(BOSS, 0x5, 26, "SirenB", NaN, NaN, NaN, F_DUNGEON, 0x5, 0x3, S_BOSS),
            p(CONTAINER, 0x5, 26, "SirenB", NaN, NaN, NaN, F_DUNGEON, 0x5, 0x4, S_BOSS),
            p(MAP, 0x6, 45, "M_Dai", -2550f, 250f, -1050f, F_DUNGEON, 0x6, 0x0, S_CHEST),
            p(COMPASS, 0x6, 45, "M_Dai", 6300f, 650f, -3700f, F_DUNGEON, 0x6, 0x1, S_CHEST),
            p(BIGKEY, 0x6, 45, "M_Dai", -3400f, -5650f, -1200f, F_DUNGEON, 0x6, 0x2, S_CHEST),
            p(BOSS, 0x6, 45, "M_DaiB", NaN, NaN, NaN, F_DUNGEON, 0x6, 0x3, S_BOSS),
            p(CONTAINER, 0x6, 45, "M_DaiB", NaN, NaN, NaN, F_DUNGEON, 0x6, 0x4, S_BOSS),
            p(MAP, 0x7, 4, "kaze", 8726f, 963f, 9597f, F_DUNGEON, 0x7, 0x0, S_CHEST),
            p(COMPASS, 0x7, 4, "kaze", -1600f, -1900f, 0f, F_DUNGEON, 0x7, 0x1, S_CHEST),
            p(BIGKEY, 0x7, 4, "kaze", 12124f, 1080f, -2f, F_DUNGEON, 0x7, 0x2, S_CHEST),
            p(BOSS, 0x7, 4, "kazeB", NaN, NaN, NaN, F_DUNGEON, 0x7, 0x3, S_BOSS),
            p(CONTAINER, 0x7, 4, "kazeB", NaN, NaN, NaN, F_DUNGEON, 0x7, 0x4, S_BOSS),
            p(CHART, 0x1, 30, "ShipD", 2990f, 260f, 0f, F_CHART, 0x1, 0x0, S_CHEST),
            p(CHART, 0x2, 11, null, NaN, NaN, NaN, F_CHART, 0x2, 0x0, S_MINIGAME),
            p(CHART, 0x3, 35, "TF_03", 2830f, 245f, 0f, F_CHART, 0x3, 0x0, S_CHEST),
            p(CHART, 0x4, 43, null, -293800f, 1900f, 321200f, F_CHART, 0x4, 0x0, S_CHEST),
            p(CHART, 0x5, 29, null, -270000f, -5000f, 85000f, F_CHART, 0x5, 0x0, S_GUNBOAT),
            p(CHART, 0x6, 21, null, 330400f, 2100f, -105400f, F_CHART, 0x6, 0x0, S_CHEST),
            p(CHART, 0x7, 11, null, NaN, NaN, NaN, F_CHART, 0x7, 0x0, S_REWARD),
            p(CHART, 0x8, 12, "Cave07", 2500f, 27f, 0f, F_CHART, 0x8, 0x0, S_CHEST),
            p(CHART, 0x9, 13, "M_NewD2", -3367f, 350f, -290f, F_CHART, 0x9, 0x0, S_CHEST),
            p(CHART, 0xa, 41, "kindan", -5965f, 674f, -6930f, F_CHART, 0xa, 0x0, S_CHEST),
            p(CHART, 0xb, 26, "Siren", -3119f, -326f, -12938f, F_CHART, 0xb, 0x0, S_CHEST),
            p(CHART, 0xc, 45, "M_Dai", 1898f, -1800f, 4994f, F_CHART, 0xc, 0x0, S_CHEST),
            p(CHART, 0xd, 4, "kaze", 3032f, -4350f, -1f, F_CHART, 0xd, 0x0, S_CHEST),
            p(CHART, 0xe, 11, null, NaN, NaN, NaN, F_CHART, 0xe, 0x0, S_MINIGAME),
            p(CHART, 0xf, 11, null, NaN, NaN, NaN, F_CHART, 0xf, 0x0, S_REWARD),
            p(CHART, 0x10, 11, null, NaN, NaN, NaN, F_CHART, 0x10, 0x0, S_REWARD),
            p(CHART, 0x11, 11, null, NaN, NaN, NaN, F_CHART, 0x11, 0x0, S_REWARD),
            p(CHART, 0x12, 11, null, NaN, NaN, NaN, F_CHART, 0x12, 0x0, S_AUCTION),
            p(CHART, 0x13, 13, "M_NewD2", -764f, 3590f, 2866f, F_CHART, 0x13, 0x0, S_CHEST),
            p(CHART, 0x14, 11, null, NaN, NaN, NaN, F_CHART, 0x14, 0x0, S_REWARD),
            p(CHART, 0x15, 26, "Siren", -5787f, -1542f, -2363f, F_CHART, 0x15, 0x0, S_CHEST),
            p(CHART, 0x16, 45, "M_Dai", -7500f, -5500f, 5700f, F_CHART, 0x16, 0x0, S_CHEST),
            p(CHART, 0x17, 4, "kaze", -604f, -3400f, 4284f, F_CHART, 0x17, 0x0, S_CHEST),
            p(CHART, 0x18, 41, "kindan", -1444f, 4843f, 12961f, F_CHART, 0x18, 0x0, S_CHEST),
            p(CHART, 0x19, 11, "Ocmera", 549f, 450f, -633f, F_CHART, 0x19, 0x0, S_CHEST),
            p(CHART, 0x1a, 0, null, NaN, NaN, NaN, F_CHART, 0x1a, 0x0, S_REWARD),
            p(CHART, 0x1b, 11, null, NaN, NaN, NaN, F_CHART, 0x1b, 0x0, S_AUCTION),
            p(CHART, 0x1c, 6, null, 210000f, 1700f, -287700f, F_CHART, 0x1c, 0x0, S_CHEST),
            p(CHART, 0x1d, 43, null, -319504f, 1023f, 300724f, F_CHART, 0x1d, 0x0, S_CHEST),
            p(CHART, 0x1e, 16, null, NaN, NaN, NaN, F_CHART, 0x1e, 0x0, S_SHOP),
            p(CHART, 0x1f, 41, null, 219285f, 1370f, 208115f, F_CHART, 0x1f, 0x0, S_CHEST),
            p(CHART, 0x20, 39, null, 3500f, 1300f, 200200f, F_CHART, 0x20, 0x0, S_CHEST),
            p(CHART, 0x21, 5, null, 98200f, 436f, -300000f, F_CHART, 0x21, 0x0, S_CHEST),
            p(CHART, 0x22, 45, "Abship", 0f, -550f, -3800f, F_CHART, 0x22, 0x0, S_SUB),
            p(CHART, 0x25, 5, "Abship", 0f, -550f, -3800f, F_CHART, 0x25, 0x0, S_SUB),
            p(CHART, 0x26, 3, "Abship", 0f, -550f, -3900f, F_CHART, 0x26, 0x0, S_SUB),
            p(CHART, 0x27, 40, null, 82300f, 20f, 220200f, F_CHART, 0x27, 0x0, S_CHEST),
            p(CHART, 0x28, 10, null, NaN, NaN, NaN, F_CHART, 0x28, 0x0, S_MINIGAME),
            p(CHART, 0x29, 42, null, 278700f, 1200f, 178500f, F_CHART, 0x29, 0x0, S_CHEST),
            p(CHART, 0x2a, 16, "Cave04", 677f, 100f, -404f, F_CHART, 0x2a, 0x0, S_CHEST),
            p(CHART, 0x2b, 43, "Cave05", 677f, 50f, -404f, F_CHART, 0x2b, 0x0, S_CHEST),
            p(CHART, 0x2c, 25, null, 18100f, 1070f, -17300f, F_CHART, 0x2c, 0x0, S_CHEST),
            p(CHART, 0x2d, 37, null, -220000f, 1071f, 180000f, F_CHART, 0x2d, 0x0, S_CHEST),
            p(CHART, 0x2e, 8, null, -282177f, 1266f, -222325f, F_CHART, 0x2e, 0x0, S_CHEST),
            p(CHART, 0x2f, 22, null, -322000f, 1577f, 22500f, F_CHART, 0x2f, 0x0, S_CHEST),
            p(CHART, 0x30, 46, null, 2000f, 922f, 282000f, F_CHART, 0x30, 0x0, S_CHEST),
            p(CHART, 0x31, 24, null, -80000f, 1120f, 20000f, F_CHART, 0x31, 0x0, S_CHEST),
            p(CHART, 0x32, 33, null, 122245f, 1788f, 120746f, F_CHART, 0x32, 0x0, S_CHEST),
            p(CHART, 0x33, 11, null, NaN, NaN, NaN, F_CHART, 0x33, 0x0, S_MINIGAME),
            p(SALVAGE, 0x1, 23, null, NaN, NaN, NaN, F_SALVAGED, 0x1, 0x0, S_SUNKEN),
            p(SALVAGE, 0x2, 4, null, NaN, NaN, NaN, F_SALVAGED, 0x2, 0x0, S_SUNKEN),
            p(SALVAGE, 0x3, 31, null, NaN, NaN, NaN, F_SALVAGED, 0x3, 0x0, S_SUNKEN),
            p(SALVAGE, 0x4, 44, null, NaN, NaN, NaN, F_SALVAGED, 0x4, 0x0, S_SUNKEN),
            p(SALVAGE, 0x5, 42, null, NaN, NaN, NaN, F_SALVAGED, 0x5, 0x0, S_SUNKEN),
            p(SALVAGE, 0x6, 32, null, NaN, NaN, NaN, F_SALVAGED, 0x6, 0x0, S_SUNKEN),
            p(SALVAGE, 0x7, 6, null, NaN, NaN, NaN, F_SALVAGED, 0x7, 0x0, S_SUNKEN),
            p(SALVAGE, 0x8, 46, null, NaN, NaN, NaN, F_SALVAGED, 0x8, 0x0, S_SUNKEN),
            p(SALVAGE, 0x9, 5, null, NaN, NaN, NaN, F_SALVAGED, 0x9, 0x0, S_SUNKEN),
            p(SALVAGE, 0xa, 47, null, NaN, NaN, NaN, F_SALVAGED, 0xa, 0x0, S_SUNKEN),
            p(SALVAGE, 0xb, 12, null, NaN, NaN, NaN, F_SALVAGED, 0xb, 0x0, S_SUNKEN),
            p(SALVAGE, 0xc, 34, null, NaN, NaN, NaN, F_SALVAGED, 0xc, 0x0, S_SUNKEN),
            p(SALVAGE, 0xd, 28, null, NaN, NaN, NaN, F_SALVAGED, 0xd, 0x0, S_SUNKEN),
            p(SALVAGE, 0xe, 36, null, NaN, NaN, NaN, F_SALVAGED, 0xe, 0x0, S_SUNKEN),
            p(SALVAGE, 0xf, 41, null, NaN, NaN, NaN, F_SALVAGED, 0xf, 0x0, S_SUNKEN),
            p(SALVAGE, 0x10, 49, null, NaN, NaN, NaN, F_SALVAGED, 0x10, 0x0, S_SUNKEN),
            p(SALVAGE, 0x11, 16, null, NaN, NaN, NaN, F_SALVAGED, 0x11, 0x0, S_SUNKEN),
            p(SALVAGE, 0x12, 22, null, NaN, NaN, NaN, F_SALVAGED, 0x12, 0x0, S_SUNKEN),
            p(SALVAGE, 0x13, 13, null, NaN, NaN, NaN, F_SALVAGED, 0x13, 0x0, S_SUNKEN),
            p(SALVAGE, 0x14, 3, null, NaN, NaN, NaN, F_SALVAGED, 0x14, 0x0, S_SUNKEN),
            p(SALVAGE, 0x15, 25, null, NaN, NaN, NaN, F_SALVAGED, 0x15, 0x0, S_SUNKEN),
            p(SALVAGE, 0x16, 37, null, NaN, NaN, NaN, F_SALVAGED, 0x16, 0x0, S_SUNKEN),
            p(SALVAGE, 0x17, 30, null, NaN, NaN, NaN, F_SALVAGED, 0x17, 0x0, S_SUNKEN),
            p(SALVAGE, 0x18, 33, null, NaN, NaN, NaN, F_SALVAGED, 0x18, 0x0, S_SUNKEN),
            p(SALVAGE, 0x19, 9, null, NaN, NaN, NaN, F_SALVAGED, 0x19, 0x0, S_SUNKEN),
            p(SALVAGE, 0x1a, 27, null, NaN, NaN, NaN, F_SALVAGED, 0x1a, 0x0, S_SUNKEN),
            p(SALVAGE, 0x1b, 11, null, NaN, NaN, NaN, F_SALVAGED, 0x1b, 0x0, S_SUNKEN),
            p(SALVAGE, 0x1c, 38, null, NaN, NaN, NaN, F_SALVAGED, 0x1c, 0x0, S_SUNKEN),
            p(SALVAGE, 0x1d, 29, null, NaN, NaN, NaN, F_SALVAGED, 0x1d, 0x0, S_SUNKEN),
            p(SALVAGE, 0x1e, 39, null, NaN, NaN, NaN, F_SALVAGED, 0x1e, 0x0, S_SUNKEN),
            p(SALVAGE, 0x1f, 19, null, NaN, NaN, NaN, F_SALVAGED, 0x1f, 0x0, S_SUNKEN),
            p(SALVAGE, 0x20, 45, null, NaN, NaN, NaN, F_SALVAGED, 0x20, 0x0, S_SUNKEN),
            p(SALVAGE, 0x21, 17, null, NaN, NaN, NaN, F_SALVAGED, 0x21, 0x0, S_SUNKEN),
            p(SALVAGE, 0x22, 26, null, NaN, NaN, NaN, F_SALVAGED, 0x22, 0x0, S_SUNKEN),
            p(SALVAGE, 0x25, 43, null, NaN, NaN, NaN, F_SALVAGED, 0x25, 0x0, S_SUNKEN),
            p(SALVAGE, 0x26, 10, null, NaN, NaN, NaN, F_SALVAGED, 0x26, 0x0, S_SUNKEN),
            p(SALVAGE, 0x27, 35, null, NaN, NaN, NaN, F_SALVAGED, 0x27, 0x0, S_SUNKEN),
            p(SALVAGE, 0x28, 40, null, NaN, NaN, NaN, F_SALVAGED, 0x28, 0x0, S_SUNKEN),
            p(SALVAGE, 0x29, 1, null, NaN, NaN, NaN, F_SALVAGED, 0x29, 0x0, S_SUNKEN),
            p(SALVAGE, 0x2a, 20, null, NaN, NaN, NaN, F_SALVAGED, 0x2a, 0x0, S_SUNKEN),
            p(SALVAGE, 0x2b, 15, null, NaN, NaN, NaN, F_SALVAGED, 0x2b, 0x0, S_SUNKEN),
            p(SALVAGE, 0x2c, 18, null, NaN, NaN, NaN, F_SALVAGED, 0x2c, 0x0, S_SUNKEN),
            p(SALVAGE, 0x2d, 8, null, NaN, NaN, NaN, F_SALVAGED, 0x2d, 0x0, S_SUNKEN),
            p(SALVAGE, 0x2e, 14, null, NaN, NaN, NaN, F_SALVAGED, 0x2e, 0x0, S_SUNKEN),
            p(SALVAGE, 0x2f, 48, null, NaN, NaN, NaN, F_SALVAGED, 0x2f, 0x0, S_SUNKEN),
            p(SALVAGE, 0x30, 7, null, NaN, NaN, NaN, F_SALVAGED, 0x30, 0x0, S_SUNKEN),
            p(SALVAGE, 0x31, 24, null, NaN, NaN, NaN, F_SALVAGED, 0x31, 0x0, S_SUNKEN),
            p(SALVAGE, 0x32, 21, null, NaN, NaN, NaN, F_SALVAGED, 0x32, 0x0, S_SUNKEN),
            p(SALVAGE, 0x33, 2, null, NaN, NaN, NaN, F_SALVAGED, 0x33, 0x0, S_SUNKEN),
            p(SQUARE, 0x1, 1, null, -300000f, 0f, -300000f, F_SQUARE, 0x1, 0x0, S_FISHMAN),
            p(SQUARE, 0x2, 2, null, -200000f, 0f, -300000f, F_SQUARE, 0x2, 0x0, S_FISHMAN),
            p(SQUARE, 0x3, 3, null, -100000f, 0f, -300000f, F_SQUARE, 0x3, 0x0, S_FISHMAN),
            p(SQUARE, 0x4, 4, null, 0f, 0f, -300000f, F_SQUARE, 0x4, 0x0, S_FISHMAN),
            p(SQUARE, 0x5, 5, null, 100000f, 0f, -300000f, F_SQUARE, 0x5, 0x0, S_FISHMAN),
            p(SQUARE, 0x6, 6, null, 200000f, 0f, -300000f, F_SQUARE, 0x6, 0x0, S_FISHMAN),
            p(SQUARE, 0x7, 7, null, 300000f, 0f, -300000f, F_SQUARE, 0x7, 0x0, S_FISHMAN),
            p(SQUARE, 0x8, 8, null, -300000f, 0f, -200000f, F_SQUARE, 0x8, 0x0, S_FISHMAN),
            p(SQUARE, 0x9, 9, null, -200000f, 0f, -200000f, F_SQUARE, 0x9, 0x0, S_FISHMAN),
            p(SQUARE, 0xa, 10, null, -100000f, 0f, -200000f, F_SQUARE, 0xa, 0x0, S_FISHMAN),
            p(SQUARE, 0xb, 11, null, 0f, 0f, -200000f, F_SQUARE, 0xb, 0x0, S_FISHMAN),
            p(SQUARE, 0xc, 12, null, 100000f, 0f, -200000f, F_SQUARE, 0xc, 0x0, S_FISHMAN),
            p(SQUARE, 0xd, 13, null, 200000f, 0f, -200000f, F_SQUARE, 0xd, 0x0, S_FISHMAN),
            p(SQUARE, 0xe, 14, null, 300000f, 0f, -200000f, F_SQUARE, 0xe, 0x0, S_FISHMAN),
            p(SQUARE, 0xf, 15, null, -300000f, 0f, -100000f, F_SQUARE, 0xf, 0x0, S_FISHMAN),
            p(SQUARE, 0x10, 16, null, -200000f, 0f, -100000f, F_SQUARE, 0x10, 0x0, S_FISHMAN),
            p(SQUARE, 0x11, 17, null, -100000f, 0f, -100000f, F_SQUARE, 0x11, 0x0, S_FISHMAN),
            p(SQUARE, 0x12, 18, null, 0f, 0f, -100000f, F_SQUARE, 0x12, 0x0, S_FISHMAN),
            p(SQUARE, 0x13, 19, null, 100000f, 0f, -100000f, F_SQUARE, 0x13, 0x0, S_FISHMAN),
            p(SQUARE, 0x14, 20, null, 200000f, 0f, -100000f, F_SQUARE, 0x14, 0x0, S_FISHMAN),
            p(SQUARE, 0x15, 21, null, 300000f, 0f, -100000f, F_SQUARE, 0x15, 0x0, S_FISHMAN),
            p(SQUARE, 0x16, 22, null, -300000f, 0f, 0f, F_SQUARE, 0x16, 0x0, S_FISHMAN),
            p(SQUARE, 0x17, 23, null, -200000f, 0f, 0f, F_SQUARE, 0x17, 0x0, S_FISHMAN),
            p(SQUARE, 0x18, 24, null, -100000f, 0f, 0f, F_SQUARE, 0x18, 0x0, S_FISHMAN),
            p(SQUARE, 0x19, 25, null, 0f, 0f, 0f, F_SQUARE, 0x19, 0x0, S_FISHMAN),
            p(SQUARE, 0x1a, 26, null, 100000f, 0f, 0f, F_SQUARE, 0x1a, 0x0, S_FISHMAN),
            p(SQUARE, 0x1b, 27, null, 200000f, 0f, 0f, F_SQUARE, 0x1b, 0x0, S_FISHMAN),
            p(SQUARE, 0x1c, 28, null, 300000f, 0f, 0f, F_SQUARE, 0x1c, 0x0, S_FISHMAN),
            p(SQUARE, 0x1d, 29, null, -300000f, 0f, 100000f, F_SQUARE, 0x1d, 0x0, S_FISHMAN),
            p(SQUARE, 0x1e, 30, null, -200000f, 0f, 100000f, F_SQUARE, 0x1e, 0x0, S_FISHMAN),
            p(SQUARE, 0x1f, 31, null, -100000f, 0f, 100000f, F_SQUARE, 0x1f, 0x0, S_FISHMAN),
            p(SQUARE, 0x20, 32, null, 0f, 0f, 100000f, F_SQUARE, 0x20, 0x0, S_FISHMAN),
            p(SQUARE, 0x21, 33, null, 100000f, 0f, 100000f, F_SQUARE, 0x21, 0x0, S_FISHMAN),
            p(SQUARE, 0x22, 34, null, 200000f, 0f, 100000f, F_SQUARE, 0x22, 0x0, S_FISHMAN),
            p(SQUARE, 0x23, 35, null, 300000f, 0f, 100000f, F_SQUARE, 0x23, 0x0, S_FISHMAN),
            p(SQUARE, 0x24, 36, null, -300000f, 0f, 200000f, F_SQUARE, 0x24, 0x0, S_FISHMAN),
            p(SQUARE, 0x25, 37, null, -200000f, 0f, 200000f, F_SQUARE, 0x25, 0x0, S_FISHMAN),
            p(SQUARE, 0x26, 38, null, -100000f, 0f, 200000f, F_SQUARE, 0x26, 0x0, S_FISHMAN),
            p(SQUARE, 0x27, 39, null, 0f, 0f, 200000f, F_SQUARE, 0x27, 0x0, S_FISHMAN),
            p(SQUARE, 0x28, 40, null, 100000f, 0f, 200000f, F_SQUARE, 0x28, 0x0, S_FISHMAN),
            p(SQUARE, 0x29, 41, null, 200000f, 0f, 200000f, F_SQUARE, 0x29, 0x0, S_FISHMAN),
            p(SQUARE, 0x2a, 42, null, 300000f, 0f, 200000f, F_SQUARE, 0x2a, 0x0, S_FISHMAN),
            p(SQUARE, 0x2b, 43, null, -300000f, 0f, 300000f, F_SQUARE, 0x2b, 0x0, S_FISHMAN),
            p(SQUARE, 0x2c, 44, null, -200000f, 0f, 300000f, F_SQUARE, 0x2c, 0x0, S_FISHMAN),
            p(SQUARE, 0x2d, 45, null, -100000f, 0f, 300000f, F_SQUARE, 0x2d, 0x0, S_FISHMAN),
            p(SQUARE, 0x2e, 46, null, 0f, 0f, 300000f, F_SQUARE, 0x2e, 0x0, S_FISHMAN),
            p(SQUARE, 0x2f, 47, null, 100000f, 0f, 300000f, F_SQUARE, 0x2f, 0x0, S_FISHMAN),
            p(SQUARE, 0x30, 48, null, 200000f, 0f, 300000f, F_SQUARE, 0x30, 0x0, S_FISHMAN),
            p(SQUARE, 0x31, 49, null, 300000f, 0f, 300000f, F_SQUARE, 0x31, 0x0, S_FISHMAN),
            p(ITEM, 0x20, 44, null, NaN, NaN, NaN, F_ITEM, 0x20, 0x0, S_REWARD),
            p(ITEM, 0x78, 11, null, NaN, NaN, NaN, F_ITEM, 0x78, 0x0, S_SHOP),
            p(ITEM, 0x77, 11, null, NaN, NaN, NaN, F_ITEM, 0x77, 0x0, S_AUCTION),
            p(ITEM, 0x22, 13, null, NaN, NaN, NaN, F_ITEM, 0x22, 0x0, S_REWARD),
            p(ITEM, 0x25, 13, "M_Dra09", NaN, NaN, NaN, F_ITEM, 0x25, 0x0, S_CHEST),
            p(ITEM, 0x24, 11, "Asoko", NaN, NaN, NaN, F_ITEM, 0x24, 0x0, S_CHEST),
            p(ITEM, 0x2d, 41, "kinMB", NaN, NaN, NaN, F_ITEM, 0x2d, 0x0, S_CHEST),
            p(ITEM, 0x34, 41, null, NaN, NaN, NaN, F_ITEM, 0x34, 0x0, S_GROUND),
            p(ITEM, 0x21, 11, "Pnezumi", NaN, NaN, NaN, F_ITEM, 0x21, 0x0, S_REWARD),
            p(ITEM, 0x23, 11, "Pnezumi", 1007f, 78f, 190f, F_ITEM, 0x23, 0x0, S_CHEST),
            p(ITEM, 0x26, 11, "Ocmera", NaN, NaN, NaN, F_ITEM, 0x26, 0x0, S_REWARD),
            p(ITEM, 0x29, 40, "MiniHyo", -461f, 75f, 2268f, F_ITEM, 0x29, 0x0, S_CHEST),
            p(ITEM, 0x2a, 11, null, NaN, NaN, NaN, F_ITEM, 0x2a, 0x0, S_SHOP),
            p(ITEM, 0x2c, 11, null, NaN, NaN, NaN, F_ITEM, 0x2c, 0x0, S_SHOP),
            p(ITEM, 0x27, 26, "SirenMB", NaN, NaN, NaN, F_ITEM, 0x27, 0x0, S_CHEST),
            p(ITEM, 0x31, 11, "Asoko", NaN, NaN, NaN, F_ITEM, 0x31, 0x0, S_CHEST),
            p(ITEM, 0x30, 13, null, NaN, NaN, NaN, F_ITEM, 0x30, 0x0, S_REWARD),
            p(ITEM, 0x2f, 4, "kazeMB", NaN, NaN, NaN, F_ITEM, 0x2f, 0x0, S_CHEST),
            p(ITEM, 0x33, 1, null, -300302f, 715f, -302364f, F_ITEM, 0x33, 0x0, S_CHEST),
            p(ARROWS, 0x2, 9, null, NaN, NaN, NaN, F_ARROWS, 0x2, 0x0, S_FAIRY),
            p(ARROWS, 0x4, 26, "GanonJ", -30f, -0f, -12350f, F_ARROWS, 0x4, 0x0, S_CHEST),
            p(BOTTLE, 0x0, 13, null, NaN, NaN, NaN, F_BOTTLE, 0x0, 0x0, S_REWARD),
            p(BOTTLE, 0x1, 11, null, NaN, NaN, NaN, F_BOTTLE, 0x1, 0x0, S_REWARD),
            p(BOTTLE, 0x2, 34, "Abship", NaN, NaN, NaN, F_BOTTLE, 0x2, 0x0, S_SUB),
            p(BOTTLE, 0x3, 16, null, NaN, NaN, NaN, F_BOTTLE, 0x3, 0x0, S_SHOP),
            p(EQUIP, 0x0, 44, null, NaN, NaN, NaN, F_EQUIP, 0x0, 0x0, S_REWARD),
            p(EQUIP, 0x1, 26, "kenroom", NaN, NaN, NaN, F_EQUIP, 0x1, 0x0, S_GROUND),
            p(EQUIP, 0x2, 45, "M_DaiB", NaN, NaN, NaN, F_EQUIP, 0x2, 0x0, S_BOSS),
            p(EQUIP, 0x3, 4, "kazeB", NaN, NaN, NaN, F_EQUIP, 0x3, 0x0, S_BOSS),
            p(EQUIP, 0x4, 44, null, NaN, NaN, NaN, F_EQUIP, 0x4, 0x0, S_REWARD),
            p(EQUIP, 0x5, 45, "M_DaiMB", NaN, NaN, NaN, F_EQUIP, 0x5, 0x0, S_CHEST),
            p(EQUIP, 0x6, 20, "MiniKaz", -563f, 100f, 2792f, F_EQUIP, 0x6, 0x0, S_CHEST),
            p(EQUIP, 0x7, 44, null, NaN, NaN, NaN, F_EQUIP, 0x7, 0x0, S_REWARD),
            p(EQUIP, 0x8, 44, "Cave10", NaN, NaN, NaN, F_EQUIP, 0x8, 0x0, S_CHEST),
            p(UPGRADE, 0x0, 3, null, NaN, NaN, NaN, F_UPGRADE, 0x0, 0x0, S_FAIRY),
            p(UPGRADE, 0x1, 44, null, NaN, NaN, NaN, F_UPGRADE, 0x1, 0x0, S_FAIRY),
            p(UPGRADE, 0x2, 19, null, NaN, NaN, NaN, F_UPGRADE, 0x2, 0x0, S_FAIRY),
            p(UPGRADE, 0x3, 39, null, NaN, NaN, NaN, F_UPGRADE, 0x3, 0x0, S_FAIRY),
            p(UPGRADE, 0x4, 15, null, NaN, NaN, NaN, F_UPGRADE, 0x4, 0x0, S_FAIRY),
            p(UPGRADE, 0x5, 28, null, NaN, NaN, NaN, F_UPGRADE, 0x5, 0x0, S_FAIRY),
            p(UPGRADE, 0x6, 46, null, NaN, NaN, NaN, F_UPGRADE, 0x6, 0x0, S_FAIRY),
            p(SONG, 0x0, 13, null, NaN, NaN, NaN, F_SONG, 0x0, 0x0, S_REWARD),
            p(SONG, 0x1, 24, null, NaN, NaN, NaN, F_SONG, 0x1, 0x0, S_REWARD),
            p(SONG, 0x2, 26, "Siren", NaN, NaN, NaN, F_SONG, 0x2, 0x0, S_REWARD),
            p(SONG, 0x3, 45, "Edaichi", NaN, NaN, NaN, F_SONG, 0x3, 0x0, S_REWARD),
            p(SONG, 0x4, 4, "Ekaze", NaN, NaN, NaN, F_SONG, 0x4, 0x0, S_REWARD),
            p(SONG, 0x5, 11, null, NaN, NaN, NaN, F_SONG, 0x5, 0x0, S_REWARD),
            p(PEARL, 0x1, 13, null, NaN, NaN, NaN, F_PEARL, 0x1, 0x0, S_REWARD),
            p(PEARL, 0x2, 41, null, NaN, NaN, NaN, F_PEARL, 0x2, 0x0, S_REWARD),
            p(PEARL, 0x0, 44, "Pjavdou", NaN, NaN, NaN, F_PEARL, 0x0, 0x0, S_REWARD),
            p(SHARD, 0x1, 23, null, NaN, NaN, NaN, F_SHARD, 0x1, 0x1, S_SUNKEN),
            p(SHARD, 0x2, 33, "TF_04", 4150f, 50f, -50f, F_SHARD, 0x2, 0x0, S_CHEST),
            p(SHARD, 0x3, 31, null, NaN, NaN, NaN, F_SHARD, 0x3, 0x3, S_SUNKEN),
            p(SHARD, 0x4, 0, "PShip", 0f, -550f, -3840f, F_SHARD, 0x4, 0x0, S_CHEST),
            p(SHARD, 0x5, 42, null, NaN, NaN, NaN, F_SHARD, 0x5, 0x5, S_SUNKEN),
            p(SHARD, 0x6, 44, "Cave09", NaN, NaN, NaN, F_SHARD, 0x6, 0x0, S_CHEST),
            p(SHARD, 0x7, 31, "TF_01", 2600f, 20f, 0f, F_SHARD, 0x7, 0x0, S_CHEST),
            p(SHARD, 0x8, 7, "TF_02", 2600f, 20f, 0f, F_SHARD, 0x8, 0x0, S_CHEST),
            p(GALLERY, 0x0, 11, "Pfigure", NaN, NaN, NaN, F_GALLERY, 0x0, 0x0, S_REWARD),
    };
    // END TABLE

    /** the charts' sunken treasures: {chart, square, point 0 x, z, .. point 3 x, z} (the sea's world) */
    private static final float[][] SUNKEN = {
            {1, 23, -204600f, -4500f, -195700f, 7900f, -199200f, 4000f, -191500f, -3500f},
            {2, 4, 22100f, -324200f, 26500f, -315700f, 16400f, -317200f, 15200f, -326400f},
            {3, 31, -124200f, 95800f, -115500f, 96800f, -123300f, 104100f, -123700f, 108600f},
            {4, 44, -191300f, 314500f, -196700f, 324700f, -197200f, 313400f, -208100f, 328500f},
            {5, 42, 276000f, 175100f, 276100f, 182300f, 281100f, 176800f, 282800f, 186600f},
            {6, 32, 3000f, 95000f, -4000f, 104500f, -1900f, 96100f, 4000f, 106100f},
            {7, 6, 215400f, -316900f, 218100f, -325800f, 223300f, -319700f, 227100f, -312700f},
            {8, 46, -3200f, 280800f, 1800f, 283600f, 2300f, 276800f, 8300f, 285300f},
            {9, 5, 96600f, -297200f, 102700f, -303700f, 100800f, -300200f, 103900f, -293800f},
            {10, 47, 98600f, 321900f, 100600f, 316600f, 102200f, 321000f, 95300f, 320500f},
            {11, 12, 78300f, -176200f, 78300f, -185500f, 74700f, -179200f, 86300f, -178300f},
            {12, 34, 216600f, 83500f, 223600f, 81600f, 223200f, 74900f, 221100f, 71300f},
            {13, 28, 324300f, 23200f, 317700f, 14900f, 320000f, 16000f, 314400f, 24400f},
            {14, 36, -304700f, 200000f, -301600f, 203100f, -300000f, 196900f, -294600f, 202000f},
            {15, 41, 217400f, 205200f, 211905f, 194439f, 214200f, 195500f, 207200f, 205200f},
            {16, 49, 322000f, 283000f, 317500f, 282000f, 319800f, 276900f, 324300f, 276300f},
            {17, 16, -221600f, -115900f, -221700f, -121700f, -215300f, -121900f, -211300f, -123400f},
            {18, 22, -327900f, 13400f, -322800f, 19100f, -317300f, 20800f, -311300f, 15300f},
            {19, 13, 196200f, -196800f, 207600f, -210000f, 208000f, -202700f, 202500f, -217100f},
            {20, 3, -75100f, -276600f, -84600f, -276300f, -75400f, -284900f, -88100f, -288700f},
            {21, 25, 20100f, -16700f, 23600f, -18600f, 20000f, -21400f, 23700f, -23700f},
            {22, 37, -217500f, 175800f, -216900f, 180000f, -221100f, 181300f, -222900f, 176000f},
            {23, 30, -184000f, 82400f, -182400f, 75300f, -180000f, 83200f, -172900f, 75900f},
            {24, 33, 115100f, 115400f, 117900f, 124400f, 125000f, 119300f, 124100f, 112100f},
            {25, 9, -176000f, -202700f, -174400f, -193400f, -184400f, -195400f, -186200f, -190300f},
            {26, 27, 197800f, -5100f, 203200f, 4100f, 196700f, 2400f, 192200f, 5000f},
            {27, 11, 4400f, -198200f, 4300f, -206100f, -4000f, -195900f, -4800f, -207500f},
            {28, 38, -100700f, 226400f, -97900f, 219800f, -96100f, 225200f, -105200f, 221600f},
            {29, 29, -275900f, 115600f, -285800f, 118700f, -283300f, 123400f, -282600f, 128400f},
            {30, 39, -18700f, 184700f, -24400f, 187000f, -19300f, 174500f, -25900f, 172600f},
            {31, 19, 125600f, -84700f, 114200f, -80500f, 117700f, -74900f, 128400f, -73500f},
            {32, 45, -76600f, 324000f, -85600f, 323300f, -83100f, 317100f, -82100f, 310900f},
            {33, 17, -98500f, -75500f, -96100f, -83000f, -104100f, -80700f, -105200f, -86000f},
            {34, 26, 107400f, -8100f, 103000f, 2500f, 96400f, -2000f, 89800f, 7600f},
            {37, 43, -325200f, 297900f, -316700f, 305900f, -321900f, 304500f, -314300f, 294800f},
            {38, 10, -120000f, -186200f, -120500f, -180400f, -114200f, -186100f, -112700f, -178200f},
            {39, 35, 297500f, 93200f, 298700f, 104400f, 297200f, 102400f, 306000f, 96900f},
            {40, 40, 86700f, 223100f, 74200f, 223400f, 76900f, 215000f, 88200f, 213000f},
            {41, 1, -307900f, -295900f, -294100f, -305800f, -297500f, -292500f, -301300f, -311500f},
            {42, 20, 184300f, -115900f, 176600f, -116400f, 183000f, -118000f, 175300f, -123500f},
            {43, 15, -318100f, -106400f, -316000f, -95600f, -325600f, -101200f, -327300f, -93600f},
            {44, 18, 4300f, -95600f, -2500f, -94300f, 4700f, -101700f, -6300f, -104300f},
            {45, 8, -276300f, -219000f, -276400f, -223700f, -280800f, -220700f, -283000f, -211500f},
            {46, 14, 296900f, -199200f, 303600f, -200300f, 297300f, -204300f, 303500f, -196500f},
            {47, 48, 183400f, 283600f, 182900f, 275000f, 177500f, 278800f, 177400f, 286200f},
            {48, 7, 296900f, -297200f, 295700f, -303000f, 303200f, -302500f, 306200f, -295800f},
            {49, 24, -71700f, 20700f, -83300f, 23200f, -77300f, 20900f, -88000f, 11800f},
            {50, 21, 281500f, -94600f, 284500f, -100700f, 279900f, -101900f, 279600f, -107200f},
            {51, 2, -195000f, -326100f, -206400f, -322700f, -197200f, -314800f, -202300f, -311200f},
    };

    /** the sea world spot of a chart's treasure, by the file's salvage point (0..3); null if none */
    static float[] sunkenSpot(int chart, int point) {
        for (float[] s : SUNKEN)
            if (s[0] == chart) {
                int i = Math.max(0, Math.min(3, point));
                return new float[] {s[2 + 2 * i], s[3 + 2 * i]};
            }
        return null;
    }

    /** the salvage point the file uses (dSv_player_info_c mRandomSalvagePoint) */
    static int salvagePoint(byte[] d) { return u8(d, INFO + 0x59); }

    private static int u8(byte[] d, int o) { return d[o] & 0xFF; }

    private static long u32(byte[] d, int o) {
        return ((long) u8(d, o) << 24) | (u8(d, o + 1) << 16) | (u8(d, o + 2) << 8) | u8(d, o + 3);
    }

    /** a byte of a stage's dSv_memBit_c, with its working copy while the stage is in play */
    private static int stageByte(byte[] d, int stage, int o) {
        int v = u8(d, STAGES + stage * STAGE_SIZE + o);
        if ((byte) d[STAGE_NO] == stage) v |= u8(d, STAGE_COPY + o);
        return v;
    }

    /** bit b of a big-endian u32 at a stage's memBit offset o (mTbox +0, mItem +0x14) */
    private static boolean stageBit32(byte[] d, int stage, int o, int b) { return (stageByte(d, stage, o + 3 - b / 8) >> (b % 8) & 1) != 0; }

    /** a chart's bit of dSv_player_map_c set (1 got, 2 deciphered, 3 its treasure salvaged; MiniMap's chart) */
    static boolean chartBit(byte[] d, int set, int c) { return chart(d, set, c); }

    private static boolean chart(byte[] d, int set, int c) {
        return (u32(d, MAP_BITS + set * 16 + ((c - 1) >> 5) * 4) & (1L << ((c - 1) & 31))) != 0;
    }

    private static boolean slot(byte[] d, int s) { return u8(d, ITEMS_AT + s) != 0xFF; }

    // the inventory slot of an item (GameProgress's list)
    private static int itemSlot(int id) {
        switch (id) {
            case 0x20: return 0;
            case 0x78: case 0x77: return 1;
            case 0x22: return 2;
            case 0x25: return 3;
            case 0x24: return 4;
            case 0x2D: return 5;
            case 0x34: return 6;
            case 0x21: return 7;
            case 0x23: case 0x26: return 8;
            case 0x29: return 9;
            case 0x2A: return 10;
            case 0x2C: return 11;
            case 0x27: return 12;
            case 0x31: return 13;
            case 0x30: return 18;
            case 0x2F: return 19;
            case 0x33: return 20;
            default: return -1;
        }
    }

    /** whether the place's own flag is set (bottles: their few flags; else what is had) */
    private static boolean flag(byte[] d, Place p) {
        switch (p.flag) {
            case F_CHEST: return stageBit32(d, p.a, 0, p.b);
            case F_ITEMBIT: return stageBit32(d, p.a, 0x14, p.b);
            case F_DUNGEON: return (stageByte(d, p.a, 0x21) >> p.b & 1) != 0;
            case F_OCEAN: return ((u8(d, OCEAN + 2 * p.a) << 8 | u8(d, OCEAN + 2 * p.a + 1)) >> p.b & 1) != 0;
            case F_EVENT: return (u8(d, EVENTS + (p.a >> 8)) & (p.a & 0xFF)) == (p.a & 0xFF);
            case F_REG: return (u8(d, EVENTS + p.a) & p.b) == p.b;
            case F_REGGE: return (u8(d, EVENTS + p.a) & (p.b >> 8)) >= (p.b & 0xFF);  // (b: mask << 8 | least)
            case F_CHART: return chart(d, 1, p.a);
            case F_SALVAGED: return chart(d, 3, p.a);
            case F_SQUARE: return (u8(d, FMAP + p.a - 1) & 1) != 0;
            case F_ITEM: {
                int s = itemSlot(p.a), v = u8(d, ITEMS_AT + s);
                if (p.a == 0x77 || p.a == 0x26) return v == p.a;
                return v != 0xFF;
            }
            case F_ARROWS: return slot(d, 12) && ((u8(d, GOT_ITEMS + 12) & p.a) != 0 || u8(d, ITEMS_AT + 12) >= (p.a == 2 ? 0x35 : 0x36));
            case F_BOTTLE: return false;  // (counted in states)
            case F_EQUIP: {
                int[][] e = {{0, 1}, {0, 2}, {0, 4}, {0, 8}, {1, 1}, {1, 2}, {2, 1}, {3, 1}, {4, 1}};
                return (u8(d, COLLECT + e[p.a][0]) & e[p.a][1]) != 0;
            }
            case F_UPGRADE: {
                switch (p.a) {
                    case 0: return u8(d, WALLET) >= 1;
                    case 1: return u8(d, WALLET) >= 2;
                    case 2: return u8(d, BOMB_MAX) >= 60;
                    case 3: return u8(d, BOMB_MAX) >= 99;
                    case 4: return u8(d, ARROW_MAX) >= 60;
                    case 5: return u8(d, ARROW_MAX) >= 99;
                    default: return u8(d, MAX_MAGIC) >= 32;
                }
            }
            case F_SONG: return (u8(d, TACT) >> p.a & 1) != 0;
            case F_PEARL: return (u8(d, PEARLS) >> p.a & 1) != 0;
            case F_SHARD: return (u8(d, TRIFORCE) >> (p.a - 1) & 1) != 0;
            case F_GALLERY: return figurines(d) >= GameProgress.FIGURINES;
            default: return false;
        }
    }

    static int figurines(byte[] d) {
        int c = 0;
        for (int f = 0; f < GameProgress.FIGURINES; f++) if ((u8(d, EVENTS + FIGURE_BYTES[f / 8]) >> (f % 8) & 1) != 0) c++;
        return c;
    }

    /** each place's state (MISSING, FOUND, MAYBE) in the save data d, by ALL's index */
    static int[] states(byte[] d) {
        int[] s = new int[ALL.length];
        int containers = 0, sure = 0, bottlesKnown = 0;
        for (Place p : ALL) {
            boolean f = flag(d, p);
            s[p.index] = f ? FOUND : MISSING;
            if (p.kind == CONTAINER && f) containers++;
            if (p.kind == HEART && f) sure++;
        }
        // the pieces the weak flags miss: found, not sure which
        int pieces = Math.max(0, ((u8(d, MAX_LIFE) << 8 | u8(d, MAX_LIFE + 1)) - 12 - 4 * containers));
        int extra = pieces - sure;
        for (Place p : ALL)
            if (extra > 0 && p.weak && s[p.index] == MISSING) {
                s[p.index] = MAYBE;
                extra--;
            }
        // the bottles: Bomb Island's submarine (its chest, table 10 bit 2) and Beedle's 500 Rupee bottle (0x2020), the
        // others in story order
        int bottles = 0;
        for (int i = 14; i < 18; i++) if (slot(d, i)) bottles++;
        boolean sub = stageBit32(d, 10, 0, 2), beedle = (u8(d, EVENTS + 0x20) & 0x20) != 0;
        bottlesKnown = (sub ? 1 : 0) + (beedle ? 1 : 0);
        int left = bottles - bottlesKnown;
        for (Place p : ALL) {
            if (p.flag != F_BOTTLE) continue;
            boolean f = p.a == 2 ? sub : p.a == 3 ? beedle : left-- > 0;
            s[p.index] = f ? FOUND : MISSING;
        }
        return s;
    }

    /** the places in a square (0: those of no fixed square) of a group, in the table's order */
    static java.util.List<Place> inSquare(int square, int group) {
        java.util.List<Place> l = new java.util.ArrayList<>();
        for (Place p : ALL) if (p.square == square && p.inGroup(group)) l.add(p);
        return l;
    }

    /** the places inside a stage (a dungeon's map) of a group */
    static java.util.List<Place> inStage(String stage, int group) {
        java.util.List<Place> l = new java.util.ArrayList<>();
        for (Place p : ALL) if (stage.equals(p.stage) && p.inGroup(group)) l.add(p);
        return l;
    }

    // ---- names and icons (as the checklist's)

    private static final int[] EQUIP_ITEM = {0x38, 0x39, -1, -1, 0x3B, 0x3C, 0x28, -1, 0x43};
    private static final String[] EQUIP_ICON = {"CollectIcon118_00^l", "CollectIcon118_01^l", "CollectIcon118_02^l",
            "CollectIcon118_03^l", "CollectIcon118_04^l", "CollectIcon118_05^l", "CollectIcon118_09^l", "CollectIcon118_08^l", "Triforce_00^l"};
    private static final int[] PEARL_ITEM = {0x69, 0x6A, 0x6B};
    private static final String[] PEARL_ICON = {"Pearl_02^l", "Pearl_00^l", "Pearl_01^l"};

    // a chart's number as the game shows it ("Treasure Chart 29" is chart 25), by its number in the save; 0: none (the
    // Triforce and special charts) - the GameCube set's notes, the same in HD (Lenzo's chest); 42..46 HD's own (the
    // charts that replaced five Triforce Charts: 2 Dampa's, 4 / 6 the platforms near Horseshoe Island / Star Belt,
    // 7 Mrs. Marie's, 8 the Wizzrobe cave by Pawprint Isle; the wikis' lists, matched by place and sunken treasure)
    private static final int[] CHART_NO = {0, 0, 42, 0, 43, 0, 44, 45, 46, 11, 15, 30, 20, 5, 23, 31, 33, 2, 38, 39, 24, 6, 12, 35, 1, 29, 34, 18, 16, 28, 4, 3, 40, 10, 14, 0, 0, 9, 22, 36, 17, 25, 37, 8, 26, 41, 19, 32, 13, 21, 27, 7, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};

    private static String chartName(int n, GameProgress.Labels l) {
        String s = l.item(0xFF - n);
        return n < CHART_NO.length && CHART_NO[n] > 0 ? s + " " + CHART_NO[n] : s;
    }

    static String name(Place p, GameProgress.Labels l, Context c) {
        switch (p.kind) {
            case HEART: return l.item(0x07);
            case CONTAINER: return l.item(0x08);
            case MAP: return l.item(0x4C) + " · " + l.dungeon(p.what - 1);
            case COMPASS: return l.item(0x4D) + " · " + l.dungeon(p.what - 1);
            case BIGKEY: return l.item(0x4E) + " · " + l.dungeon(p.what - 1);
            case BOSS: return c.getString(R.string.pmap_boss, l.dungeon(p.what - 1));
            case CHART: case SALVAGE: return chartName(p.what, l);
            case SQUARE: return l.section(8);
            case ITEM: return l.item(p.what);
            case ARROWS: return l.own(p.what == 2 ? GameProgress.OWN_FIRE_ICE : GameProgress.OWN_LIGHT);
            case BOTTLE: return l.item(0x50);
            case EQUIP:
                if (p.what == 2) return l.own(GameProgress.OWN_MASTER_HALF);
                if (p.what == 3) return l.own(GameProgress.OWN_MASTER_FULL);
                if (p.what == 7) return l.own(GameProgress.OWN_PIRATE);
                return l.item(EQUIP_ITEM[p.what]);
            case UPGRADE: {
                String[] u = {l.item(0xAB) + " 1000", l.item(0xAC) + " 5000", l.item(0xAD) + " 60", l.item(0xAE) + " 99",
                        l.item(0xAF) + " 60", l.item(0xB0) + " 99", l.own(GameProgress.OWN_MAGIC)};
                return u[p.what];
            }
            case SONG: return l.song(p.what);
            case PEARL: return l.item(PEARL_ITEM[p.what]);
            case SHARD: return l.item(0x61) + " " + p.what;
            case GALLERY: return l.section(9);
            default: return "";
        }
    }

    static String icon(Place p) {
        if (p.tex != null) return p.tex;
        switch (p.kind) {
            case HEART: return "HeartPiece_02^l";
            case CONTAINER: return "HeartReceptacle_00^l";
            case MAP: return "MapItemIcon_00^l";
            case COMPASS: return "MapItemIcon_02^l";
            case BIGKEY: return "MapItemIcon_01^l";
            case BOSS: return "MapBossIcon_00^q";
            case CHART: return "MapBtn_00^l";
            case SALVAGE: return "IconSalvage_00^q";
            case SQUARE: return "FishMan_00^l";
            case ITEM: return GameIcons.itemTexture(p.what);
            case ARROWS: return p.what == 2 ? "FireArrow_00^l" : "LightArrow_00^l";
            case BOTTLE: return GameIcons.itemTexture(0x50);
            case EQUIP: return EQUIP_ICON[p.what];
            case UPGRADE: {
                String[] u = {"Rupy_01^l", "Rupy_02^l", GameIcons.itemTexture(0x31), GameIcons.itemTexture(0x31), "NormalArrow_00^l",
                        "NormalArrow_00^l", GameIcons.itemTexture(0x52)};
                return u[p.what];
            }
            case SONG: return "CollectIcon118_15^l";
            case PEARL: return PEARL_ICON[p.what];
            case SHARD: return "TriforceShards_00^l";
            case GALLERY: return "BestShotMark_00^t";
            default: return null;
        }
    }

    private static final int[] SOURCES = {R.string.pmap_src_chest, R.string.pmap_src_sub, R.string.pmap_src_sunken,
            R.string.pmap_src_octo, R.string.pmap_src_gunboat, R.string.pmap_src_ground, R.string.pmap_src_reward,
            R.string.pmap_src_auction, R.string.pmap_src_minigame, R.string.pmap_src_letter, R.string.pmap_src_shop,
            R.string.pmap_src_boss, R.string.pmap_src_fishman, R.string.pmap_src_fairy, R.string.pmap_src_unknown};

    /** how it is found, and where inside ("Chest · inside") */
    static String source(Place p, Context c) {
        String s = c.getString(SOURCES[p.src]);
        if (p.stage != null && p.src != S_SUB && p.src != S_BOSS) s += " · " + c.getString(R.string.pmap_inside);
        return s;
    }

    // ---- when each place was first seen found in play, and for the places of no fixed square where Link was then
    // (prefs file "progress_found": file + "/" + key -> time, file + "/" + key + "@" -> square)

    private static SharedPreferences log(Context c) { return c.getSharedPreferences("progress_found", Context.MODE_PRIVATE); }

    private static int[] before;
    private static String beforeFile;

    /** a read of the save data in play (Achievements.check, every two seconds): places newly found are logged with the
     *  time and, for those of no fixed square, the square Link is on (the sea) */
    static void observe(Context c, String file, byte[] d) {
        int[] now = states(d);
        int[] was = file.equals(beforeFile) ? before : null;
        before = now;
        beforeFile = file;
        if (was == null) return;
        SharedPreferences.Editor e = null;
        long t = System.currentTimeMillis();
        for (Place p : ALL) {
            if (now[p.index] != FOUND || was[p.index] == FOUND) continue;
            if (e == null) e = log(c).edit();
            e.putLong(file + "/" + p.key(), t);
            if (p.square == 0) {
                int sq = linkSquare();
                if (sq > 0) e.putInt(file + "/" + p.key() + "@", sq);
            }
        }
        if (e != null) e.apply();
    }

    /** the square Link is on on the sea now, 0 elsewhere */
    static int linkSquare() {
        float[] s = Native.miniMapState();
        if (s == null || s.length < 3 || s[0] != 1) return 0;
        int col = (int) Math.floor((s[1] + 350000f) / 100000f), row = (int) Math.floor((s[2] + 350000f) / 100000f);
        return col < 0 || col > 6 || row < 0 || row > 6 ? 0 : row * 7 + col + 1;
    }

    /** when the place was seen found in play in save game f (0: not seen) */
    static long foundAt(Context c, String f, Place p) { return f == null ? 0 : log(c).getLong(f + "/" + p.key(), 0); }

    /** for a place of no fixed square: the square it was found on (0: not known) */
    static int foundOn(Context c, String f, Place p) { return f == null ? 0 : log(c).getInt(f + "/" + p.key() + "@", 0); }

    /** a new file was started in save game f's slot (Achievements): forget its log */
    static void forget(Context c, String f) {
        SharedPreferences.Editor e = null;
        for (String k : log(c).getAll().keySet())
            if (k.startsWith(f + "/")) {
                if (e == null) e = log(c).edit();
                e.remove(k);
            }
        if (e != null) e.apply();
        before = null;
    }

    static void reset(Context c) {
        log(c).edit().clear().apply();
        before = null;
    }
}
