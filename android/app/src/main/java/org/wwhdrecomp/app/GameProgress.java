package org.wwhdrecomp.app;

import java.util.ArrayList;
import java.util.List;

/**
 * How far the file in play is (Game > Game progress, and the percent on screen, ProgressBadge): the save data as it
 * is now (Native.progressData, mods/progress.cpp: dSv_save_c as on the GameCube) counted in ten parts that weigh the
 * same, each a checklist of what is collected and what is missing:
 * <ul>
 * <li>Dungeons: per dungeon (stage table entries 2..7: Forsaken Fortress .. Wind Temple, dSv_memBit_c mDungeonItem)
 *     its map (bit 0), compass (1), Big Key (2; the Forsaken Fortress has none), boss (3, STAGE_BOSS_ENEMY) and
 *     Heart Container (4, STAGE_LIFE); the stage in play from its working copy (save + 0x778, mStageNo + 0x79C).</li>
 * <li>Hearts: Pieces of Heart = max life (quarters) - 12 - 4 per Heart Container taken: 44 (20 hearts).</li>
 * <li>Items: the 21 inventory slots (+0x3C; the four bottles one row), the Swift Sail (0x77 in the sail's slot),
 *     the Deluxe Picto Box (0x26), the bow's Fire &amp; Ice and Light Arrows (its "got" bits +0x51 + 12: 2, 4).</li>
 * <li>Equipment: dSv_player_collect_c +0xB4: swords (byte 0: Hero's Sword, Master Sword, half, full power), shields
 *     (1), Power Bracelets (2), Pirate's Charm (3), Hero's Charm (4).</li>
 * <li>Upgrades: wallet (+0x12: 1000, 5000), bomb bag / quiver (max +0x70 / +0x6F: 60, 99), magic (+0x13: 32).</li>
 * <li>Songs (+0xBD, 6 bits); Pearls (+0xBF, 3 bits) and Triforce Shards (+0xBE, 8 bits).</li>
 * <li>Charts: dSv_player_map_c +0xC4 (field_0x0[4][4] u32: [1] got, [3] its treasure salvaged), the charts the
 *     game counts (getCollectMapNum: 1..51 without 35, 36): 49 each.</li>
 * <li>Sea chart: the 49 squares the fishman filled in (mFmapBits +0x104, bit 0).</li>
 * <li>Nintendo Gallery: the 134 figurines (event registers, daNpcMt_c::isFigureGet: 17 bytes, events at +0x624).</li>
 * </ul>
 * Checked against a 100% save (every part full). Not counted: the Tingle Statues and the Hurricane Spin (no flag of
 * their own), the delivery bag's trading, the last fight (the game saves nothing after it).
 */
final class GameProgress {
    private GameProgress() {}

    // dSv_save_c offsets (the player's data first)
    private static final int MAX_LIFE = 0x00, WALLET = 0x12, MAX_MAGIC = 0x13, ITEMS = 0x3C, GOT_ITEMS = 0x51;
    private static final int ARROW_MAX = 0x6F, BOMB_MAX = 0x70, COLLECT = 0xB4, TACT = 0xBD, TRIFORCE = 0xBE, PEARLS = 0xBF;
    private static final int MAP = 0xC4, FMAP = MAP + 0x40, STAGES = 0x380, STAGE_SIZE = 0x24, DUNGEON_ITEM = 0x21;
    private static final int EVENTS = 0x624, STAGE_COPY = 0x778, STAGE_NO = 0x79C;
    // the figurines' event registers, 8 figurines each (d_a_npc_mt.cpp l_figure_comp)
    private static final int[] FIGURE_BYTES = {0x95, 0x94, 0x93, 0x92, 0x91, 0x90, 0x8F, 0x8E, 0x8D, 0x8C, 0xB1, 0x9C, 0x84, 0x83,
            0x82, 0x81, 0x80};
    static final int FIGURINES = 134, DUNGEONS = 6;

    /** A line of a checklist: a thing to have (total 1) or a count (41 of 44). Icons are GameIcons textures. */
    static final class Entry {
        final String name;
        final String[] icons;   // dungeons: one per part (map, compass, Big Key, boss, Heart Container); else one or none
        final boolean[] got;    // ... and whether each is had
        final int done, total;

        Entry(String name, String icon, int done, int total) {
            this(name, new String[] {icon}, new boolean[] {done >= total}, done, total);
        }

        Entry(String name, String[] icons, boolean[] got, int done, int total) {
            this.name = name;
            this.icons = icons;
            this.got = got;
            this.done = done;
            this.total = total;
        }

        boolean complete() { return done >= total; }
    }

    static final class Section {
        final String title, icon;
        final List<Entry> entries = new ArrayList<>();
        int done, total;

        Section(String title, String icon) {
            this.title = title;
            this.icon = icon;
        }

        void add(Entry e) {
            entries.add(e);
            done += Math.min(e.done, e.total);
            total += e.total;
        }

        void add(String name, String icon, boolean got) { add(new Entry(name, icon, got ? 1 : 0, 1)); }

        float fraction() { return total == 0 ? 1 : done / (float) total; }
    }

    /** The names shown: the game's own words where it has them (GameText), else the app's strings. */
    interface Labels {
        String section(int part);              // 0..9 in the order of the parts above
        String item(int id);                   // an item's name (its "You got ..." message)
        String dungeon(int n);                 // 1..6: Forsaken Fortress .. Wind Temple
        String island(int room);               // 1..49
        String song(int melody);
        String figurine(int n);                // 0..133
        String own(int which);                 // OWN_*: names the game has no words for
    }

    /** for a count that shows no names (the percent on screen, Achievements) */
    static final Labels NAMELESS = new Labels() {
        public String section(int part) { return ""; }
        public String item(int id) { return ""; }
        public String dungeon(int n) { return ""; }
        public String island(int room) { return ""; }
        public String song(int melody) { return ""; }
        public String figurine(int n) { return ""; }
        public String own(int which) { return ""; }
    };

    static final int OWN_FIRE_ICE = 0, OWN_LIGHT = 1, OWN_MASTER_HALF = 2, OWN_MASTER_FULL = 3, OWN_PIRATE = 4, OWN_SALVAGED = 5,
            OWN_MAGIC = 6;

    /** The parts and the whole (0..100, 100 only when nothing is missing). */
    static final class Result {
        final List<Section> sections = new ArrayList<>();

        int percent() {
            float sum = 0;
            boolean all = true;
            for (Section s : sections) {
                sum += s.fraction();
                all &= s.done >= s.total;
            }
            int p = (int) Math.floor(100 * sum / sections.size());
            return all ? 100 : Math.min(99, p);
        }
    }

    private static int u8(byte[] d, int o) { return d[o] & 0xFF; }

    private static long u32(byte[] d, int o) {
        return ((long) u8(d, o) << 24) | (u8(d, o + 1) << 16) | (u8(d, o + 2) << 8) | u8(d, o + 3);
    }

    private static boolean bit(byte[] d, int o, int b) { return (u8(d, o) & (1 << b)) != 0; }

    // a chart's bit in one of dSv_player_map_c's four 128-bit sets (dSv_player_map_c::isGetMap and friends)
    private static boolean chart(byte[] d, int set, int i) { return (u32(d, MAP + set * 16 + (i >> 5) * 4) & (1L << (i & 31))) != 0; }

    /** a dungeon's items byte (stage table entry, with the working copy of the stage in play) */
    private static int dungeonItems(byte[] d, int stage) {
        int v = u8(d, STAGES + stage * STAGE_SIZE + DUNGEON_ITEM);
        if ((byte) d[STAGE_NO] == stage) v |= u8(d, STAGE_COPY + DUNGEON_ITEM);
        return v;
    }

    // English names for a game whose texts aren't read yet
    private static final String[] DUNGEON_EN = {"Forsaken Fortress", "Dragon Roost Cavern", "Forbidden Woods", "Tower of the Gods",
            "Earth Temple", "Wind Temple"};

    /** the names in the game's words (GameText) where it has them, else the app's strings */
    static Labels labels(android.content.Context c) {
        return new Labels() {
            final int[] parts = {R.string.progress_part_dungeons, R.string.progress_part_hearts, R.string.progress_part_items,
                    R.string.progress_part_equipment, R.string.progress_part_upgrades, R.string.progress_part_songs,
                    R.string.progress_part_quest, R.string.progress_part_charts, R.string.progress_part_sea,
                    R.string.progress_part_figurines};
            final int[] own = {R.string.progress_fire_ice, R.string.progress_light, R.string.progress_master_half,
                    R.string.progress_master_full, R.string.progress_pirate, R.string.progress_salvaged, R.string.progress_magic};

            public String section(int part) { return c.getString(parts[part]); }

            public String item(int id) {
                String n = GameText.itemName(id);
                // (the Deluxe Picto Box's message names only the Picto Box in some languages)
                if (id == 0x26 && (n == null || n.equals(GameText.itemName(0x23)))) return c.getString(R.string.progress_deluxe);
                if (n != null) return n;
                String e = GameText.item(id);
                return e != null ? e : "#" + id;
            }

            public String dungeon(int n) {
                String s = GameText.dungeonName(n);
                return s != null ? s : DUNGEON_EN[n - 1];
            }

            public String island(int room) {
                String s = GameText.islandName(room);
                return s != null ? s : c.getString(R.string.progress_square, room);
            }

            public String song(int melody) { return GameText.songName(melody); }

            public String figurine(int n) {
                String s = GameText.figurineByNumber(n);
                return s != null ? s : c.getString(R.string.progress_figurine, n + 1);
            }

            public String own(int which) { return c.getString(own[which]); }
        };
    }

    /** the count of the save data as it is now; null outside a game */
    static Result read(Labels l) {
        byte[] d = Native.progressData();
        return d == null || d.length < STAGE_NO + 1 ? null : count(d, l);
    }

    static Result count(byte[] d, Labels l) {
        Result r = new Result();
        // dungeons: map, compass, Big Key, boss, Heart Container
        Section dun = new Section(l.section(0), "MapBossIcon_00^q");
        String[] parts = {"MapItemIcon_00^l", "MapItemIcon_02^l", "MapItemIcon_01^l", "MapBossIcon_00^q", "HeartReceptacle_00^l"};
        int containers = 0;
        for (int n = 1; n <= DUNGEONS; n++) {
            int v = dungeonItems(d, n + 1);
            boolean ff = n == 1;
            List<String> icons = new ArrayList<>();
            List<Boolean> got = new ArrayList<>();
            int done = 0;
            for (int b = 0; b < 5; b++) {
                if (ff && b == 2) continue;  // (no Big Key there)
                icons.add(parts[b]);
                boolean g = (v & (1 << b)) != 0;
                got.add(g);
                if (g) done++;
            }
            if ((v & 0x10) != 0) containers++;
            boolean[] g = new boolean[got.size()];
            for (int i = 0; i < g.length; i++) g[i] = got.get(i);
            dun.add(new Entry(l.dungeon(n), icons.toArray(new String[0]), g, done, icons.size()));
        }
        r.sections.add(dun);
        // hearts: the pieces (the containers are the dungeons')
        Section hearts = new Section(l.section(1), "HeartPiece_02^l");
        int pieces = Math.max(0, Math.min(44, ((d[MAX_LIFE] & 0xFF) << 8 | u8(d, MAX_LIFE + 1)) - 12 - 4 * containers));
        hearts.add(new Entry(l.item(0x07), "HeartPiece_02^l", pieces, 44));
        r.sections.add(hearts);
        // items
        Section items = new Section(l.section(2), "Icon128_10^l");
        int[][] slots = {  // slot, item id for the name and icon
                {0, 0x20}, {1, 0x78}, {2, 0x22}, {3, 0x25}, {4, 0x24}, {5, 0x2D}, {6, 0x34}, {7, 0x21}, {8, 0x23}, {9, 0x29},
                {10, 0x2A}, {11, 0x2C}, {12, 0x27}, {13, 0x31}, {18, 0x30}, {19, 0x2F}, {20, 0x33}};
        for (int[] s : slots) {
            int have = u8(d, ITEMS + s[0]);
            items.add(l.item(s[1]), GameIcons.itemTexture(s[1]), have != 0xFF);
            if (s[0] == 1) items.add(l.item(0x77), GameIcons.itemTexture(0x77), have == 0x77);
            if (s[0] == 8) items.add(l.item(0x26), GameIcons.itemTexture(0x26), have == 0x26);
            if (s[0] == 12) {
                int bow = u8(d, GOT_ITEMS + 12);
                items.add(l.own(OWN_FIRE_ICE), "FireArrow_00^l", have != 0xFF && ((bow & 2) != 0 || have >= 0x35));
                items.add(l.own(OWN_LIGHT), "LightArrow_00^l", have != 0xFF && ((bow & 4) != 0 || have == 0x36));
            }
        }
        int bottles = 0;
        for (int s = 14; s < 18; s++) if (u8(d, ITEMS + s) != 0xFF) bottles++;
        items.add(new Entry(l.item(0x50), GameIcons.itemTexture(0x50), bottles, 4));
        r.sections.add(items);
        // equipment
        Section eq = new Section(l.section(3), "CollectIcon118_03^l");
        int swords = u8(d, COLLECT), shields = u8(d, COLLECT + 1);
        eq.add(l.item(0x38), "CollectIcon118_00^l", (swords & 1) != 0);
        eq.add(l.item(0x39), "CollectIcon118_01^l", (swords & 2) != 0);
        eq.add(l.own(OWN_MASTER_HALF), "CollectIcon118_02^l", (swords & 4) != 0);
        eq.add(l.own(OWN_MASTER_FULL), "CollectIcon118_03^l", (swords & 8) != 0);
        eq.add(l.item(0x3B), "CollectIcon118_04^l", (shields & 1) != 0);
        eq.add(l.item(0x3C), "CollectIcon118_05^l", (shields & 2) != 0);
        eq.add(l.item(0x28), "CollectIcon118_09^l", bit(d, COLLECT + 2, 0));
        eq.add(l.own(OWN_PIRATE), "CollectIcon118_08^l", bit(d, COLLECT + 3, 0));
        eq.add(l.item(0x43), null, bit(d, COLLECT + 4, 0));
        r.sections.add(eq);
        // upgrades
        Section up = new Section(l.section(4), "Rupy_02^l");
        int wallet = u8(d, WALLET), bombs = u8(d, BOMB_MAX), arrows = u8(d, ARROW_MAX);
        up.add(l.item(0xAB) + " 1000", "Rupy_01^l", wallet >= 1);
        up.add(l.item(0xAC) + " 5000", "Rupy_02^l", wallet >= 2);
        up.add(l.item(0xAD) + " 60", GameIcons.itemTexture(0x31), bombs >= 60);
        up.add(l.item(0xAE) + " 99", GameIcons.itemTexture(0x31), bombs >= 99);
        up.add(l.item(0xAF) + " 60", "NormalArrow_00^l", arrows >= 60);
        up.add(l.item(0xB0) + " 99", "NormalArrow_00^l", arrows >= 99);
        up.add(l.own(OWN_MAGIC), GameIcons.itemTexture(0x52), u8(d, MAX_MAGIC) >= 32);
        r.sections.add(up);
        // songs
        Section songs = new Section(l.section(5), "CollectIcon118_15^l");
        for (int m = 0; m < 6; m++) songs.add(l.song(m), "CollectIcon118_15^l", bit(d, TACT, m));
        r.sections.add(songs);
        // pearls (dSymbol: Nayru, Din, Farore) and the Triforce
        Section quest = new Section(l.section(6), "Triforce_00^l");
        int[] pearlItem = {0x69, 0x6A, 0x6B};
        String[] pearlIcon = {"Pearl_02^l", "Pearl_00^l", "Pearl_01^l"};  // (Collection_00: Din's red, Farore's green, Nayru's blue)
        for (int p = 0; p < 3; p++) quest.add(l.item(pearlItem[p]), pearlIcon[p], bit(d, PEARLS, p));
        for (int t = 0; t < 8; t++) quest.add(l.item(0x61) + " " + (t + 1), "TriforceShards_00^l", bit(d, TRIFORCE, t));
        r.sections.add(quest);
        // charts the game counts, found and their treasure salvaged
        Section charts = new Section(l.section(7), "MapBtn_00^l");
        int got = 0, salvaged = 0;
        for (int i = 1; i <= 51; i++) {
            if (i == 35 || i == 36) continue;
            if (chart(d, 1, i - 1)) got++;
            if (chart(d, 3, i - 1)) salvaged++;
        }
        charts.add(new Entry(l.item(0xCC), "MapBtn_00^l", got, 49));
        charts.add(new Entry(l.own(OWN_SALVAGED), "IconSalvage_00^q", salvaged, 49));
        r.sections.add(charts);
        // the sea chart's squares
        Section sea = new Section(l.section(8), "FishMan_00^l");
        for (int room = 1; room <= 49; room++) sea.add(l.island(room), null, bit(d, FMAP + room - 1, 0));
        r.sections.add(sea);
        // figurines
        Section figs = new Section(l.section(9), "BestShotMark_00^t");
        for (int f = 0; f < FIGURINES; f++) figs.add(l.figurine(f), null, bit(d, EVENTS + FIGURE_BYTES[f / 8], f % 8));
        r.sections.add(figs);
        return r;
    }
}
