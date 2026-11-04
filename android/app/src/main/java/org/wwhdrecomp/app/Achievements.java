package org.wwhdrecomp.app;

import android.content.Context;
import android.content.SharedPreferences;

import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;

/**
 * Achievements (Game > Achievements, and a popup on screen when one is earned, AchievementPopup): goals checked on the
 * save data as it is now (Native.progressData, as GameProgress; dSv_save_c as on the GameCube), earned once and kept
 * for good, twice: for all save games (prefs file "achievements": key -> the time earned, whichever file earned it) and
 * for the save game in play (prefs file "achievements_files": file + "/" + key -> the time). A save game is its file
 * number (the file select's slot) and the player's name; a new file started in a slot is seen as reads that meet no
 * goal at all (the start of the game) and forgets what that slot's earlier file had (newFile).
 * <p>
 * The goals follow the GameCube set on retroachievements.org (game 9190) where the HD game has the same thing, and add
 * HD ones (Swift Sail, Tingle Bottle, salvage, 100 %); the texts are our own. The GameCube set's flags were taken from
 * its code notes (their RAM 0x803CC530 + n = save + n here) and checked on a 100% HD save:
 * <ul>
 * <li>Events (save + 0x624, dSv_event_c; flag 0xBBMM = byte BB, mask MM): e.g. Killer Bees 0x1340, Hurricane Spin
 *     0x0B20; some bytes are counters (Lenzo 0xC4 = 6 when done, Orca 0xD0: 3 = 500 hits, 4 = 1000; Beedle's points
 *     0x86; Squid-Hunt prizes 0xFE / 0xFF).</li>
 * <li>A stage's dSv_memBit_c (table + 0x380, 0x24 each; the stage in play from its working copy + 0x778): mTbox +0
 *     (chests; per dungeon the mask a 100% file has), mSwitch +4.. (the sea, stage 0: set sail 0x0A bit 4, King of
 *     Red Lions 0x06 bit 2, Gallery ladder 0x0C bit 6, Tower raised 0x1B bit 0, the bell 0x09 bit 7, Big Octos 0x05
 *     bits 0..2 + 0x06 bits 5..7, the black pig 0x17 bit 2); submarines are stage 0x0A's chests 0xDF.</li>
 * <li>dSv_player_info_c (+0x148): mClearCount +0x58 (the game was beaten and the file saved after it).</li>
 * <li>Hero Mode: not in dSv_save_c but in the save file's HD-only data (GcSave: the player section, 16 bytes per slot
 *     after the 3 slots, byte 6 = 1); read from save/user/cking.sav for the slot in play when its HD name there is
 *     the player's, so a new file counts once the game has saved it (heroMode).</li>
 * </ul>
 * Chests by number from the stage files (TRES, params >> 7 & 0x1F, the stage's save table from STAG): the dungeons'
 * masks (the Dragon Roost Cavern's chest 16 left out: no file opens it), the Tingle Statues (chest 15 of stages 3..7),
 * the Savage Labyrinth (table 0x0D: floor 30 chest 11, floor 50 chest 12 the Hero's Charm).
 * Not here (they need more of the game's live state: bosses, hits taken, input): most of the GameCube set's
 * challenges. Not in the HD game: the Tingle Tuner's quests and discounts (no Tuner; their flags stay 0 in a 100%
 * file). An achievement counts once its goal holds in two reads in a row (two seconds apart: a stage being loaded
 * changes the working copy and its stage number one after the other). None are earned while a cheat that keeps
 * running is on, nor for the rest of the run after a one-shot cheat or a story preset (pauseRun).
 */
final class Achievements {
    private Achievements() {}

    static final int STORY = 0, ITEMS = 1, SONGS = 2, QUESTS = 3, COLLECT = 4, GALLERY = 5;

    // dSv_save_c offsets (GameProgress has the same)
    private static final int MAX_LIFE = 0x00, WALLET = 0x12, MAX_MAGIC = 0x13, ITEM = 0x3C, GOT_ITEMS = 0x51;
    private static final int ARROW_MAX = 0x6F, BOMB_MAX = 0x70, COLLECTS = 0xB4, TACT = 0xBD, TRIFORCE = 0xBE, PEARLS = 0xBF;
    private static final int MAP = 0xC4, FMAP = MAP + 0x40, INFO = 0x148, STAGES = 0x380, STAGE_SIZE = 0x24;
    private static final int EVENTS = 0x624, STAGE_COPY = 0x778, STAGE_NO = 0x79C, TIMER = 0x7A0;
    static final int FILE_NO = 0x7B0;  // (PlayerModels too)
    // the figurines' event registers (GameProgress), and the gallery's rooms by figurine number
    private static final int[] FIGURE_BYTES = {0x95, 0x94, 0x93, 0x92, 0x91, 0x90, 0x8F, 0x8E, 0x8D, 0x8C, 0xB1, 0x9C, 0x84, 0x83,
            0x82, 0x81, 0x80};

    /** the save data read at once, with the game's ways of looking at it */
    static final class Save {
        final byte[] d;
        String stage = "";       // the stage's name (Native.miniMapStage)
        Save prev;               // the read before (two seconds earlier), null at the first
        SharedPreferences store; // for goals that keep a state between reads

        Save(byte[] d) { this.d = d; }

        /** the save game: file number (1..3, 0 unknown) and the player's name, e.g. "2:Link" */
        String file() {
            int no = d.length > FILE_NO ? u8(FILE_NO) : 0xFF;
            return (no < 3 ? no + 1 : 0) + ":" + playerName();
        }

        String playerName() {
            StringBuilder b = new StringBuilder();
            for (int i = 0; i < 16 && d[INFO + 0x14 + i] != 0; i++) b.append((char) u8(INFO + 0x14 + i));
            return b.toString();
        }

        boolean at(String name) { return name.equals(stage) && prev != null && name.equals(prev.stage); }

        /** the play's timer (mods/progress.cpp): its mode (-1 none) and the time since it started */
        int timerMode() { return d.length >= TIMER + 16 ? (int) u32(TIMER + 12) : -1; }

        long timerMs() { return u32(TIMER + 4); }

        int puzzles() { return u8(INFO + 0x57); }  // the cabana's slide puzzles solved (mPuzzleData[16]; back to 0 after the 16th)

        int u8(int o) { return d[o] & 0xFF; }

        long u32(int o) { return ((long) u8(o) << 24) | (u8(o + 1) << 16) | (u8(o + 2) << 8) | u8(o + 3); }

        boolean event(int flag) { return (u8(EVENTS + (flag >> 8)) & (flag & 0xFF)) == (flag & 0xFF); }

        int register(int b) { return u8(EVENTS + b); }

        boolean item(int slot) { return u8(ITEM + slot) != 0xFF; }

        /** a byte of a stage's dSv_memBit_c, with its working copy while the stage is in play */
        int stageByte(int stage, int o) {
            int v = u8(STAGES + stage * STAGE_SIZE + o);
            if ((byte) d[STAGE_NO] == stage) v |= u8(STAGE_COPY + o);
            return v;
        }

        boolean stageBit(int stage, int o, int bit) { return (stageByte(stage, o) & (1 << bit)) != 0; }

        boolean chests(int stage, long mask) {
            long v = 0;
            for (int i = 0; i < 4; i++) v = v << 8 | stageByte(stage, i);
            return (v & mask) == mask;
        }

        boolean song(int m) { return (u8(TACT) & (1 << m)) != 0; }

        boolean pearl(int p) { return (u8(PEARLS) & (1 << p)) != 0; }

        int bits(int o, int n) {
            int c = 0;
            for (int i = 0; i < n; i++) if ((u8(o + i) & 1) != 0) c++;
            return c;
        }

        boolean figurine(int f) { return (u8(EVENTS + FIGURE_BYTES[f / 8]) & (1 << (f % 8))) != 0; }

        int figurines(int from, int to) {
            int c = 0;
            for (int f = from; f < to; f++) if (figurine(f)) c++;
            return c;
        }

        boolean chart(int set, int n) { return (u32(MAP + set * 16 + ((n - 1) >> 5) * 4) & (1L << ((n - 1) & 31))) != 0; }

        // the charts the game counts (GameProgress): set 1 got, set 3 salvaged
        int charts(int set) {
            int c = 0;
            for (int i = 1; i <= 51; i++) {
                if (i == 35 || i == 36) continue;
                if ((u32(MAP + set * 16 + ((i - 1) >> 5) * 4) & (1L << ((i - 1) & 31))) != 0) c++;
            }
            return c;
        }

        boolean hero() { return heroMode(this); }

        boolean bow(int got) { return item(12) && ((u8(GOT_ITEMS + 12) & got) != 0 || u8(ITEM + 12) >= (got == 2 ? 0x35 : 0x36)); }
    }

    interface Goal { boolean met(Save s); }

    interface Text { String of(Context c); }

    static final class Def {
        final String key, icon;
        final int group, points;
        final Text title, desc;
        final Goal goal;
        boolean once;  // a goal on the change between two reads: earned at the first read it holds in

        Def(String key, int group, int points, String icon, Text title, Text desc, Goal goal) {
            this.key = key;
            this.group = group;
            this.points = points;
            this.icon = icon;
            this.title = title;
            this.desc = desc;
            this.goal = goal;
        }
    }

    private static Text str(int res) { return c -> c.getString(res); }

    private static String name(Context c, int id) {
        String n = GameText.itemName(id);
        if (id == 0x26 && (n == null || n.equals(GameText.itemName(0x23)))) return c.getString(R.string.progress_deluxe);
        if (n != null) return n;
        String e = GameText.item(id);
        return e != null ? e : "#" + id;
    }

    private static Text getItem(int id) { return c -> c.getString(R.string.ach_get, name(c, id)); }

    private static Text learn(int melody) { return c -> c.getString(R.string.ach_learn, GameText.songName(melody)); }

    private static final String[] DUNGEON_EN = {"Forsaken Fortress", "Dragon Roost Cavern", "Forbidden Woods", "Tower of the Gods",
            "Earth Temple", "Wind Temple"};

    private static Text dungeonChests(int n) {
        return c -> {
            String s = GameText.dungeonName(n);
            return c.getString(R.string.ach_d_chests, s != null ? s : DUNGEON_EN[n - 1]);
        };
    }

    // a place in the game's words: an island by its sea room (GameText.islandName), a dungeon by its number
    private static final int OUTSET = 44, WINDFALL = 11, DRAGON_ROOST = 13, FLIGHT_PLATFORM = 14, FOREST_HAVEN = 41, TOWER = -4,
            FORTRESS = -1, FIRE_MOUNTAIN = 20, PRIVATE_OASIS = 33, ICE_RING = 40;

    private static String place(int where) {
        String s = where < 0 ? GameText.dungeonName(-where) : GameText.islandName(where);
        if (s != null) return s;
        switch (where) {
            case OUTSET: return "Outset Island";
            case WINDFALL: return "Windfall Island";
            case DRAGON_ROOST: return "Dragon Roost Island";
            case FLIGHT_PLATFORM: return "Flight Control Platform";
            case FOREST_HAVEN: return "Forest Haven";
            case FIRE_MOUNTAIN: return "Fire Mountain";
            case PRIVATE_OASIS: return "Private Oasis";
            case ICE_RING: return "Ice Ring Isle";
            default: return DUNGEON_EN[-where - 1];
        }
    }

    private static Text at(int res, int where) { return c -> c.getString(res, place(where)); }

    private static Def own(String key, int group, int points, String icon, int title, int desc, int where, Goal g) {
        return new Def(key, group, points, icon, str(title), at(desc, where), g);
    }

    private static final String SWORD = "CollectIcon118_01^l", CHEST = "MapBossIcon_00^q", HEART = "HeartPiece_02^l",
            CHART = "MapBtn_00^l", FIG = "BestShotMark_00^t", TRI = "Triforce_00^l", SAIL = "CollectIcon118_06^l";

    private static Def item(String key, int points, int id, int title, Goal g) {
        return new Def(key, ITEMS, points, GameIcons.itemTexture(id), str(title), getItem(id), g);
    }

    private static Def song(String key, int melody, int title) {
        return new Def(key, SONGS, 3, "CollectIcon118_15^l", str(title), learn(melody), s -> s.song(melody));
    }

    private static Def own(String key, int group, int points, String icon, int title, int desc, Goal g) {
        return new Def(key, group, points, icon, str(title), str(desc), g);
    }

    private static Def once(Def d) {
        d.once = true;
        return d;
    }

    // a chest of the caves (stage 0x0C: Fire Mountain's Power Bracelets bit 0, Ice Ring Isle's Iron Boots bit 1) opened
    // while the timer that started when the island was frozen / thawed (mode 3) shows at most `ms`; both reads inside
    private static Goal timedChest(int bit, long ms) {
        return s -> {
            Save p = s.prev;
            if (p == null || (byte) s.d[STAGE_NO] != 0x0C || (byte) p.d[STAGE_NO] != 0x0C) return false;
            if (p.stageBit(0x0C, 3, bit) || !s.stageBit(0x0C, 3, bit)) return false;
            Save t = s.timerMode() == 3 ? s : p;
            return t.timerMode() == 3 && t.timerMs() <= ms;
        };
    }

    // the eight withered trees (event register 0x9E, a bit each): from the first watered to all eight within 20 minutes;
    // the first one's time is kept (store "trees_from"), so a file loaded with some watered already doesn't count
    private static boolean trees(Save s) {
        int now = s.register(0x9E), before = s.prev == null ? -1 : s.prev.register(0x9E);
        long t = System.currentTimeMillis();
        if (before == 0 && now != 0 && Integer.bitCount(now) == 1) s.store.edit().putLong("trees_from", t).apply();
        if (now == 0 && s.store.contains("trees_from")) s.store.edit().remove("trees_from").apply();
        long from = s.store.getLong("trees_from", 0);
        return now == 0xFF && from != 0 && t - from <= 20 * 60_000L;
    }

    // the special charts (the GameCube set's notes by chart number; all in a 100% HD file): Tingle, Ghost Ship,
    // IN-credible, Octo, Great Fairy, Island Hearts, Sea Hearts, Secret Cave, Light Ring, Platform, Beedle, Submarine
    private static final int[] SPECIAL_CHARTS = {35, 36, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61};

    private static boolean specialCharts(Save s) {
        for (int n : SPECIAL_CHARTS) if (!s.chart(1, n)) return false;
        return true;
    }

    // the Tingle Statues: chest 15 of each dungeon (stages 3..7; the stage files' items 0xA3..0xA7)
    private static boolean tingleStatues(Save s) {
        for (int st = 3; st <= 7; st++) if (!s.chests(st, 0x8000)) return false;
        return true;
    }

    // the gallery's rooms: figurine numbers from .. to (RA's notes by room; GameProgress's numbering)
    private static Def room(String key, int from, int to, int title, Text desc) {
        return new Def(key, GALLERY, 10, FIG, str(title), desc, s -> s.figurines(from, to) == to - from);
    }

    static final Def[] ALL = {
            // the story
            own("set_sail", STORY, 5, SAIL, R.string.ach_t_set_sail, R.string.ach_d_set_sail, OUTSET, s -> s.stageBit(0, 0x0A, 4)),
            own("king_red_lions", STORY, 3, SAIL, R.string.ach_t_king_red_lions, R.string.ach_d_king_red_lions, s -> s.stageBit(0, 0x06, 2)),
            new Def("pearl_din", STORY, 5, "Pearl_00^l", str(R.string.ach_t_pearl_din), getItem(0x6A), s -> s.pearl(1)),
            new Def("pearl_farore", STORY, 5, "Pearl_01^l", str(R.string.ach_t_pearl_farore), getItem(0x6B), s -> s.pearl(2)),
            new Def("pearl_nayru", STORY, 5, "Pearl_02^l", str(R.string.ach_t_pearl_nayru), getItem(0x69), s -> s.pearl(0)),
            own("tower_rises", STORY, 3, TRI, R.string.ach_t_tower_rises, R.string.ach_d_tower_rises, TOWER, s -> s.stageBit(0, 0x1B, 0)),
            own("bell", STORY, 10, TRI, R.string.ach_t_bell, R.string.ach_d_bell, TOWER, s -> s.stageBit(0, 0x09, 7)),
            new Def("master_sword", STORY, 10, SWORD, str(R.string.ach_t_master_sword), getItem(0x39), s -> (s.u8(COLLECTS) & 2) != 0),
            own("tetra", STORY, 10, TRI, R.string.ach_t_tetra, R.string.ach_d_tetra, s -> s.event(0x3280)),
            own("sword_half", STORY, 5, "CollectIcon118_02^l", R.string.ach_t_sword_half, R.string.ach_d_sword_half,
                    s -> (s.u8(COLLECTS) & 4) != 0),
            own("sword_full", STORY, 5, "CollectIcon118_03^l", R.string.ach_t_sword_full, R.string.ach_d_sword_full,
                    s -> (s.u8(COLLECTS) & 8) != 0),
            own("triforce", STORY, 10, "TriforceShards_00^l", R.string.ach_t_triforce, R.string.ach_d_triforce, s -> s.u8(TRIFORCE) == 0xFF),
            own("the_end", STORY, 25, TRI, R.string.ach_t_the_end, R.string.ach_d_the_end, s -> s.u8(INFO + 0x58) > 0),
            own("hero_mode", STORY, 5, SWORD, R.string.ach_t_hero_mode, R.string.ach_d_hero_mode, Save::hero),
            own("the_end_hero", STORY, 50, TRI, R.string.ach_t_the_end_hero, R.string.ach_d_the_end_hero,
                    s -> s.u8(INFO + 0x58) > 0 && s.hero()),
            // items and upgrades
            item("bait_bag", 3, 0x2C, R.string.ach_t_bait_bag, s -> s.item(11)),
            item("wind_waker", 3, 0x22, R.string.ach_t_wind_waker, s -> s.item(2)),
            item("bottle", 3, 0x50, R.string.ach_t_bottle, s -> s.item(14) || s.item(15) || s.item(16) || s.item(17)),
            item("grappling_hook", 5, 0x25, R.string.ach_t_grappling_hook, s -> s.item(3)),
            item("deku_leaf", 5, 0x34, R.string.ach_t_deku_leaf, s -> s.item(6)),
            item("boomerang", 3, 0x2D, R.string.ach_t_boomerang, s -> s.item(5)),
            item("bombs", 5, 0x31, R.string.ach_t_bombs, s -> s.item(13)),
            item("tingle_bottle", 5, 0x21, R.string.ach_t_tingle_bottle, s -> s.item(7)),
            item("bow", 3, 0x27, R.string.ach_t_bow, s -> s.item(12)),
            item("skull_hammer", 5, 0x33, R.string.ach_t_skull_hammer, s -> s.item(20)),
            new Def("fire_ice", ITEMS, 3, "FireArrow_00^l", str(R.string.ach_t_fire_ice), c -> c.getString(R.string.ach_get,
                    c.getString(R.string.progress_fire_ice)), s -> s.bow(2)),
            item("iron_boots", 3, 0x29, R.string.ach_t_iron_boots, s -> s.item(9)),
            new Def("power_bracelets", ITEMS, 5, "CollectIcon118_09^l", str(R.string.ach_t_power_bracelets), getItem(0x28),
                    s -> (s.u8(COLLECTS + 2) & 1) != 0),
            new Def("mirror_shield", ITEMS, 3, "CollectIcon118_05^l", str(R.string.ach_t_mirror_shield), getItem(0x3C),
                    s -> (s.u8(COLLECTS + 1) & 2) != 0),
            item("hookshot", 5, 0x2F, R.string.ach_t_hookshot, s -> s.item(19)),
            new Def("light_arrows", ITEMS, 5, "LightArrow_00^l", str(R.string.ach_t_light_arrows), c -> c.getString(R.string.ach_get,
                    c.getString(R.string.progress_light)), s -> s.bow(4)),
            item("magic_armor", 5, 0x2A, R.string.ach_t_magic_armor, s -> s.item(10)),
            new Def("heros_charm", ITEMS, 5, TRI, str(R.string.ach_t_heros_charm), getItem(0x43), s -> (s.u8(COLLECTS + 4) & 1) != 0),
            item("deluxe_picto", 5, 0x26, R.string.ach_t_deluxe_picto, s -> s.u8(ITEM + 8) == 0x26),
            new Def("swift_sail", ITEMS, 10, GameIcons.itemTexture(0x77), str(R.string.ach_t_swift_sail), getItem(0x77),
                    s -> s.u8(ITEM + 1) == 0x77),
            own("bottles", ITEMS, 10, GameIcons.itemTexture(0x50), R.string.ach_t_bottles, R.string.ach_d_bottles,
                    s -> s.item(14) && s.item(15) && s.item(16) && s.item(17)),
            own("magic", ITEMS, 5, GameIcons.itemTexture(0x52), R.string.ach_t_magic, R.string.ach_d_magic, s -> s.u8(MAX_MAGIC) >= 32),
            own("hearts", ITEMS, 10, "HeartReceptacle_00^l", R.string.ach_t_hearts, R.string.ach_d_hearts,
                    s -> (s.u8(MAX_LIFE) << 8 | s.u8(MAX_LIFE + 1)) >= 80),
            new Def("wallet_1000", ITEMS, 5, "Rupy_01^l", str(R.string.ach_t_wallet_1000), getItem(0xAB), s -> s.u8(WALLET) >= 1),
            new Def("wallet_5000", ITEMS, 10, "Rupy_02^l", str(R.string.ach_t_wallet_5000), getItem(0xAC), s -> s.u8(WALLET) >= 2),
            new Def("bombs_60", ITEMS, 5, GameIcons.itemTexture(0x31), str(R.string.ach_t_bombs_60), getItem(0xAD), s -> s.u8(BOMB_MAX) >= 60),
            new Def("bombs_99", ITEMS, 10, GameIcons.itemTexture(0x31), str(R.string.ach_t_bombs_99), getItem(0xAE), s -> s.u8(BOMB_MAX) >= 99),
            new Def("arrows_60", ITEMS, 3, "NormalArrow_00^l", str(R.string.ach_t_arrows_60), getItem(0xAF), s -> s.u8(ARROW_MAX) >= 60),
            new Def("arrows_99", ITEMS, 5, "NormalArrow_00^l", str(R.string.ach_t_arrows_99), getItem(0xB0), s -> s.u8(ARROW_MAX) >= 99),
            // songs
            song("song_requiem", 0, R.string.ach_t_song_requiem),
            song("song_ballad", 1, R.string.ach_t_song_ballad),
            song("song_command", 2, R.string.ach_t_song_command),
            song("song_earth", 3, R.string.ach_t_song_earth),
            song("song_wind", 4, R.string.ach_t_song_wind),
            song("song_passing", 5, R.string.ach_t_song_passing),
            // side quests
            own("grandma_chest", QUESTS, 2, CHEST, R.string.ach_t_grandma_chest, R.string.ach_d_grandma_chest,
                    s -> (s.stageByte(0x0B, 3) & 0x09) == 0x09),
            own("grandma_soup", QUESTS, 5, GameIcons.itemTexture(0x55), R.string.ach_t_grandma_soup, R.string.ach_d_grandma_soup,
                    s -> s.event(0x2A20)),
            own("black_pig", QUESTS, 3, HEART, R.string.ach_t_black_pig, R.string.ach_d_black_pig, OUTSET, s -> s.stageBit(0, 0x17, 2)),
            own("orca_500", QUESTS, 5, SWORD, R.string.ach_t_orca_500, R.string.ach_d_orca_500, s -> s.register(0xD0) >= 3),
            own("orca_1000", QUESTS, 10, SWORD, R.string.ach_t_orca_1000, R.string.ach_d_orca_1000, s -> s.register(0xD0) >= 4),
            own("hurricane_spin", QUESTS, 3, SWORD, R.string.ach_t_hurricane_spin, R.string.ach_d_hurricane_spin, s -> s.event(0x0B20)),
            own("killer_bees", QUESTS, 5, HEART, R.string.ach_t_killer_bees, R.string.ach_d_killer_bees, WINDFALL, s -> s.event(0x1340)),
            own("joy_pendant", QUESTS, 3, HEART, R.string.ach_t_joy_pendant, R.string.ach_d_joy_pendant, s -> s.register(0xC0) != 0),
            own("lenzo", QUESTS, 5, GameIcons.itemTexture(0x23), R.string.ach_t_lenzo, R.string.ach_d_lenzo, s -> s.register(0xC4) >= 6),
            own("anton_linda", QUESTS, 5, HEART, R.string.ach_t_anton_linda, R.string.ach_d_anton_linda, s -> s.event(0x2280)),
            own("maggie_moe", QUESTS, 5, HEART, R.string.ach_t_maggie_moe, R.string.ach_d_maggie_moe, s -> (s.register(0xAC) & 3) == 3),
            own("lighthouse", QUESTS, 5, HEART, R.string.ach_t_lighthouse, R.string.ach_d_lighthouse, WINDFALL, s -> s.event(0x1B20)),
            own("potions", QUESTS, 5, GameIcons.itemTexture(0x53), R.string.ach_t_potions, R.string.ach_d_potions, s -> s.event(0x0D06)),
            own("letter_sorting", QUESTS, 5, HEART, R.string.ach_t_letter_sorting, R.string.ach_d_letter_sorting, DRAGON_ROOST, s -> s.event(0x2702)),
            own("golden_feathers", QUESTS, 5, HEART, R.string.ach_t_golden_feathers, R.string.ach_d_golden_feathers,
                    s -> (s.register(0xAE) & 3) == 3),
            own("aryll_letter", QUESTS, 1, HEART, R.string.ach_t_aryll_letter, R.string.ach_d_aryll_letter, s -> s.event(0x8B01)),
            own("flight_record", QUESTS, 5, HEART, R.string.ach_t_flight_record, R.string.ach_d_flight_record, FLIGHT_PLATFORM, s -> s.event(0x2B40)),
            own("squid_hunt", QUESTS, 10, HEART, R.string.ach_t_squid_hunt, R.string.ach_d_squid_hunt, WINDFALL,
                    s -> (s.register(0xFE) & 3) == 3 && (s.register(0xFF) & 1) != 0),
            own("beedle_silver", QUESTS, 5, "Rupy_01^l", R.string.ach_t_beedle_silver, R.string.ach_d_beedle_silver,
                    s -> s.register(0x86) >= 30),
            own("beedle_gold", QUESTS, 10, "Rupy_02^l", R.string.ach_t_beedle_gold, R.string.ach_d_beedle_gold, s -> s.register(0x86) >= 60),
            own("trading", QUESTS, 10, HEART, R.string.ach_t_trading, R.string.ach_d_trading, s -> s.event(0x3E04)),
            // the Forsaken Fortress's four chests (stage 2) while Link has no sword (mSelectEquip[0] 0xFF: the first visit)
            own("unarmed_robbery", QUESTS, 10, CHEST, R.string.ach_t_unarmed_robbery, R.string.ach_d_unarmed_robbery, FORTRESS,
                    s -> s.chests(2, 0xF) && s.u8(0x0E) == 0xFF),
            // the Savage Labyrinth (stage table 0x0D): floor 30's chest (11, a Triforce Shard in HD); floor 50 is the Hero's Charm
            own("labyrinth_30", QUESTS, 10, "TriforceShards_00^l", R.string.ach_t_labyrinth_30, R.string.ach_d_labyrinth_30,
                    OUTSET, s -> s.chests(0x0D, 1 << 11)),
            own("puzzle_1", QUESTS, 3, "Rupy_01^l", R.string.ach_t_puzzle_1, R.string.ach_d_puzzle_1, PRIVATE_OASIS, s -> s.puzzles() >= 1),
            own("puzzle_8", QUESTS, 5, "Rupy_01^l", R.string.ach_t_puzzle_8, R.string.ach_d_puzzle_8, PRIVATE_OASIS, s -> s.puzzles() >= 8),
            once(own("puzzle_16", QUESTS, 10, "Rupy_02^l", R.string.ach_t_puzzle_16, R.string.ach_d_puzzle_16, PRIVATE_OASIS,
                    s -> s.at("Abesso") && s.prev.puzzles() == 15 && s.puzzles() == 0)),
            once(own("fire_mountain_fast", QUESTS, 10, "CollectIcon118_09^l", R.string.ach_t_fire_mountain_fast,
                    R.string.ach_d_fire_mountain_fast, FIRE_MOUNTAIN, timedChest(0, 75_000))),
            once(own("ice_ring_fast", QUESTS, 10, GameIcons.itemTexture(0x29), R.string.ach_t_ice_ring_fast,
                    R.string.ach_d_ice_ring_fast, ICE_RING, timedChest(1, 75_000))),
            once(own("trees_fast", QUESTS, 5, HEART, R.string.ach_t_trees_fast, R.string.ach_d_trees_fast, Achievements::trees)),
            // collections
            new Def("chests_drc", COLLECT, 5, CHEST, str(R.string.ach_t_chests_drc), dungeonChests(2), s -> s.chests(3, 0xFCDF)),
            new Def("chests_fw", COLLECT, 10, CHEST, str(R.string.ach_t_chests_fw), dungeonChests(3), s -> s.chests(4, 0xDFFF)),
            new Def("chests_totg", COLLECT, 10, CHEST, str(R.string.ach_t_chests_totg), dungeonChests(4), s -> s.chests(5, 0x8F7F)),
            new Def("chests_et", COLLECT, 10, CHEST, str(R.string.ach_t_chests_et), dungeonChests(5), s -> s.chests(6, 0xDEFF)),
            new Def("chests_wt", COLLECT, 10, CHEST, str(R.string.ach_t_chests_wt), dungeonChests(6), s -> s.chests(7, 0xBFBD)),
            own("special_charts", COLLECT, 10, CHART, R.string.ach_t_special_charts, R.string.ach_d_special_charts,
                    Achievements::specialCharts),
            own("tingle_statues", COLLECT, 10, GameIcons.itemTexture(0x21), R.string.ach_t_tingle_statues, R.string.ach_d_tingle_statues,
                    Achievements::tingleStatues),
            own("platforms", COLLECT, 10, CHEST, R.string.ach_t_platforms, R.string.ach_d_platforms, s -> s.chests(1, 0x0FBFFDFFL)),
            own("submarines", COLLECT, 5, CHEST, R.string.ach_t_submarines, R.string.ach_d_submarines, s -> s.chests(0x0A, 0xDF)),
            own("big_octos", COLLECT, 10, CHEST, R.string.ach_t_big_octos, R.string.ach_d_big_octos,
                    s -> (s.stageByte(0, 0x05) & 0x07) == 0x07 && (s.stageByte(0, 0x06) & 0xE0) == 0xE0),
            own("charts_20", COLLECT, 5, CHART, R.string.ach_t_charts_20, R.string.ach_d_charts_20, s -> s.charts(1) >= 20),
            own("charts_all", COLLECT, 10, CHART, R.string.ach_t_charts_all, R.string.ach_d_charts_all, s -> s.charts(1) >= 49),
            own("salvage_all", COLLECT, 10, "IconSalvage_00^q", R.string.ach_t_salvage_all, R.string.ach_d_salvage_all,
                    s -> s.charts(3) >= 49),
            own("sea_half", COLLECT, 5, "FishMan_00^l", R.string.ach_t_sea_half, R.string.ach_d_sea_half, s -> s.bits(FMAP, 49) >= 25),
            own("sea_all", COLLECT, 10, "FishMan_00^l", R.string.ach_t_sea_all, R.string.ach_d_sea_all, s -> s.bits(FMAP, 49) >= 49),
            own("progress_100", COLLECT, 25, TRI, R.string.ach_t_progress_100, R.string.ach_d_progress_100,
                    s -> GameProgress.count(s.d, GameProgress.NAMELESS).percent() >= 100),
            // the Nintendo Gallery
            own("gallery_open", GALLERY, 3, FIG, R.string.ach_t_gallery_open, R.string.ach_d_gallery_open, s -> s.stageBit(0, 0x0C, 6)),
            own("figurine_first", GALLERY, 3, FIG, R.string.ach_t_figurine_first, R.string.ach_d_figurine_first,
                    s -> s.figurines(0, GameProgress.FIGURINES) > 0),
            room("room_outset", 0, 14, R.string.ach_t_room_outset, at(R.string.ach_d_room, OUTSET)),
            room("room_windfall", 14, 41, R.string.ach_t_room_windfall, at(R.string.ach_d_room, WINDFALL)),
            room("room_sea", 41, 65, R.string.ach_t_room_sea, str(R.string.ach_d_room_sea)),
            room("room_dragon", 65, 83, R.string.ach_t_room_dragon, at(R.string.ach_d_room, DRAGON_ROOST)),
            room("room_forest", 83, 97, R.string.ach_t_room_forest, at(R.string.ach_d_room, FOREST_HAVEN)),
            room("room_enemies", 97, 116, R.string.ach_t_room_enemies, str(R.string.ach_d_room_enemies)),
            room("room_fortress", 116, 134, R.string.ach_t_room_fortress, at(R.string.ach_d_room, FORTRESS)),
            own("figurines_all", GALLERY, 50, FIG, R.string.ach_t_figurines_all, R.string.ach_d_figurines_all,
                    s -> s.figurines(0, GameProgress.FIGURINES) == GameProgress.FIGURINES),
    };

    static int totalPoints() {
        int p = 0;
        for (Def a : ALL) p += a.points;
        return p;
    }

    private static SharedPreferences store(Context c) { return c.getSharedPreferences("achievements", Context.MODE_PRIVATE); }

    /** when it was earned (System.currentTimeMillis), 0 if not yet */
    static long earned(Context c, Def a) { return store(c).getLong(a.key, 0); }

    private static SharedPreferences files(Context c) { return c.getSharedPreferences("achievements_files", Context.MODE_PRIVATE); }

    private static volatile String file;  // the save game in play (Save.file) at the last read, null outside a game

    /** the save game in play ("2:Link"), null when none is (title screen, file select, before the first read) */
    static String currentFile() { return file; }

    /** for a page: "Link · file 2" */
    static String fileLabel(Context c, String f) {
        int colon = f.indexOf(':');
        int no = Integer.parseInt(f.substring(0, colon));
        String name = f.substring(colon + 1);
        // the name the game shows (the HD one, Mods > Save file names too): the save data's own name field (the
        // GameCube's) is empty in HD saves, or another player's after an older story preset
        String[] shown = no > 0 ? SaveNames.current() : null;
        if (shown != null && no <= shown.length && shown[no - 1] != null && !shown[no - 1].isEmpty()) name = shown[no - 1];
        if (no <= 0) return name;
        return name.isEmpty() ? c.getString(R.string.ach_file_no, no) : c.getString(R.string.ach_file, name, no);
    }

    /** when it was earned in save game f (null: in any), 0 if not yet */
    static long earned(Context c, Def a, String f) { return f == null ? earned(c, a) : files(c).getLong(f + "/" + a.key, 0); }

    static int earnedCount(Context c) { return earnedCount(c, null); }

    static int earnedCount(Context c, String f) {
        int n = 0;
        for (Def a : ALL) if (earned(c, a, f) != 0) n++;
        return n;
    }

    static int earnedPoints(Context c, String f) {
        int n = 0;
        for (Def a : ALL) if (earned(c, a, f) != 0) n += a.points;
        return n;
    }

    static void reset(Context c) {
        store(c).edit().clear().apply();
        files(c).edit().clear().apply();
        pending = null;
        last = null;
        quiet = 0;
    }

    /** a new file was started in save game f's slot: forget what f had */
    private static void newFile(SharedPreferences fs, String f) {
        SharedPreferences.Editor e = null;
        for (String k : fs.getAll().keySet())
            if (k.startsWith(f + "/")) {
                if (e == null) e = fs.edit();
                e.remove(k);
            }
        if (e != null) e.apply();
    }

    // what paused this run (one-shot cheats, story presets), in the order used: named on the achievements page
    private static final java.util.LinkedHashSet<String> pausedBy = new java.util.LinkedHashSet<>();

    /** a one-shot cheat or a story preset (what, as its menu names it) was used: nothing is earned until the app is
     *  started again */
    static void pauseRun(String what) { synchronized (pausedBy) { pausedBy.add(what); } }

    /** Achievement settings > Earn with cheats: cheats and story presets no longer pause them (default off) */
    static final String PREF_WITH_CHEATS = "achievement_with_cheats";

    /** whether nothing is earned now (pauseRun, or a cheat that keeps running is on) */
    static boolean paused(SharedPreferences settings) {
        if (settings.getBoolean(PREF_WITH_CHEATS, false)) return false;  // (Achievement settings: earned with cheats too)
        synchronized (pausedBy) { if (!pausedBy.isEmpty()) return true; }
        for (String k : MainActivity.CHEATS_INFINITE)
            if (settings.getBoolean(k, false) && !k.equals("cheat_fast_push")) return true;
        return false;
    }

    /** the cheats on now that keep anything from being earned, as their menu lines name them */
    static java.util.List<String> pausedOn(android.content.Context c, SharedPreferences settings) {
        java.util.ArrayList<String> on = new java.util.ArrayList<>();
        for (String k : MainActivity.CHEATS_INFINITE)
            if (settings.getBoolean(k, false) && !k.equals("cheat_fast_push")) {
                int id = c.getResources().getIdentifier(k, "string", c.getPackageName());  // (each key is its label's name)
                on.add(id != 0 ? c.getString(id) : k);
            }
        return on;
    }

    /** the one-shot cheats and story presets used in this run (pauseRun) */
    static java.util.List<String> pausedUsed() {
        synchronized (pausedBy) { return new java.util.ArrayList<>(pausedBy); }
    }

    /** the save folder in play (MainActivity, before the game starts: the game's own or a randomized game's) */
    static volatile File saveDir;
    private static long heroStamp = -1;  // cking.sav's time and length when heroBytes was read
    private static byte[] heroBytes;

    // the save file's HD-only data (GcSave): per slot the player section (16 bytes, byte 6 Hero Mode), ..., the names
    private static final int HD_FILE = 8966, HD_DATA = 3 * 0xA94, HD_NAMES = HD_DATA + 3 * (16 + 4 + 20);

    /** the file in play is a Hero Mode file: the slot's Hero Mode byte in cking.sav, if its HD name there is the one the
     *  game shows now */
    static synchronized boolean heroMode(Save s) {
        File dir = saveDir;
        int no = s.d.length > FILE_NO ? s.u8(FILE_NO) : 0xFF;
        if (dir == null || no > 2) return false;
        File f = new File(dir, "user/cking.sav");
        long stamp = f.lastModified() ^ f.length() << 40;
        if (stamp != heroStamp) {
            heroStamp = stamp;
            heroBytes = null;
            if (f.length() == HD_FILE) {
                byte[] b = new byte[HD_FILE];
                try (FileInputStream in = new FileInputStream(f)) {
                    int n = 0;
                    while (n < b.length) {
                        int r = in.read(b, n, b.length - n);
                        if (r < 0) break;
                        n += r;
                    }
                    if (n == b.length) heroBytes = b;
                } catch (IOException ignored) {
                }
            }
        }
        byte[] b = heroBytes;
        if (b == null || b[HD_DATA + no * 16 + 6] != 1) return false;
        int at = HD_NAMES + no * 18, chars = 0;
        while (chars < 9 && (b[at + 2 * chars] != 0 || b[at + 2 * chars + 1] != 0)) chars++;
        String saved = new String(b, at, 2 * chars, StandardCharsets.UTF_16BE);
        // the name the game shows for the file (the HD one; save data's GameCube name field is empty in HD saves): the
        // same as what cking.sav keeps, or (a long name, Mods > Save file names) starting with it
        String[] shown = SaveNames.current();
        String now = shown != null && no < shown.length ? shown[no] : null;
        return now != null && !saved.isEmpty() && (now.equals(saved) || now.startsWith(saved));
    }

    private static boolean[] pending;  // the goals met in the last read and not earned yet
    private static Save last;          // the last read
    private static int quiet;          // reads in a row that met no goal at all

    /** reads the save data and earns what holds now and held in the last read, for all save games and for the one in
     *  play; returns what is new for the one in play (eachFile) or for all of them, to show (empty when nothing, outside a
     *  game, or paused). Any thread, one at a time. */
    static synchronized List<Def> check(Context c, SharedPreferences settings, boolean eachFile) {
        List<Def> got = new ArrayList<>();
        byte[] d = Native.progressData();
        if (d == null || d.length < STAGE_NO + 1) file = null;
        if (d == null || d.length < STAGE_NO + 1 || paused(settings)) {
            pending = null;
            last = null;
            return got;
        }
        Save s = new Save(d);
        SharedPreferences p = store(c), fs = files(c);
        String f = s.file();
        if (!f.equals(file)) {  // (another save game: the reads before were of the other one)
            pending = null;
            quiet = 0;
        }
        file = f;
        String stage = Native.miniMapStage();
        s.stage = stage != null ? stage : "";
        if (s.prev == null && last != null && f.equals(last.file())) s.prev = last;
        s.store = p;
        last = s;
        if (s.prev != null) s.prev.prev = null;  // (only the one read before is kept)
        boolean[] met = new boolean[ALL.length];
        SharedPreferences.Editor e = null;
        long now = System.currentTimeMillis();
        SharedPreferences.Editor fe = null;
        boolean any = false;
        for (int i = 0; i < ALL.length; i++) {
            boolean all = p.contains(ALL[i].key), here = fs.contains(f + "/" + ALL[i].key);
            met[i] = ALL[i].goal.met(s);
            if (!ALL[i].key.equals("hero_mode")) any |= met[i];  // (holds from a new Hero Mode file's start)
            if (all && here) continue;
            if (met[i] && (ALL[i].once || pending != null && pending[i])) {
                if (!all) {
                    if (e == null) e = p.edit();
                    e.putLong(ALL[i].key, now);
                }
                if (!here) {
                    if (fe == null) fe = fs.edit();
                    fe.putLong(f + "/" + ALL[i].key, now);
                }
                if (eachFile ? !here : !all) got.add(ALL[i]);
                met[i] = false;
            }
        }
        if (e != null) e.apply();
        if (fe != null) fe.apply();
        // two reads in a row that meet nothing: the start of a new game (the file had something, or it would meet it)
        quiet = any ? 0 : quiet + 1;
        if (quiet == 2) {
            newFile(fs, f);
            ProgressPlaces.forget(c, f);
        }
        ProgressPlaces.observe(c, f, d);
        pending = met;
        return got;
    }
}
