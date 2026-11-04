package org.wwhdrecomp.app;

/**
 * The GamePad's dungeon maps for the mini map (MiniMap): per dungeon stage its layout (DungeonMapFloor_0x of the 2D
 * pack) and room pictures as that layout places them (P_Room_NN: texture, floor 0 = 1F / -1 = B1 ..., the stage's
 * room, centre x / y in the layout's N_All_00 space (y up), size = the texture's in texels). The rooms come from the
 * game's own tables (USA .rodata 100F10B4 .. 100F2B94: {picture, floor 0x80 + n, room} per dungeon; -1: a picture of
 * no room, shown with the map); the rest from the layouts (tools: notes/minimap.md). The layout's (0, 0) is the
 * stage's DMAP origin and a texel DMAP scale x TEXEL world units, x east, y north (Native.miniMapStageInfo).
 * Stairs: the layouts' N_<floor>Up_NN / Down_NN marks inside the room pictures (DungeonMapUp_00 / Down_00 parts), at
 * their picture's place plus their own.
 */
final class MiniMapDungeons {
    static final float TEXEL = 0.382f;

    static final class Pic {
        final String texture;
        final int floor, room;
        final float x, y, w, h;
        Pic(String texture, int floor, int room, float x, float y, float w, float h) {
            this.texture = texture;
            this.floor = floor;
            this.room = room;
            this.x = x;
            this.y = y;
            this.w = w;
            this.h = h;
        }
    }

    static final class Stair {
        final int floor, room;
        final float x, y;
        final boolean up;
        Stair(int floor, int room, float x, float y, boolean up) {
            this.floor = floor;
            this.room = room;
            this.x = x;
            this.y = y;
            this.up = up;
        }
    }

    static final class Dungeon {
        final String stage, layout;
        final Pic[] pics;
        final Stair[] stairs;
        /** the stage whose DMAP places the pictures, and how far (degrees, as Canvas.rotate on x east / z south) that
         *  stage's world is turned about the origin to this one's */
        final String mapStage;
        final float turn;
        Dungeon(String stage, String layout, Pic[] pics, Stair[] stairs) { this(stage, layout, pics, stairs, stage, 0); }
        Dungeon(String stage, String layout, Pic[] pics, Stair[] stairs, String mapStage, float turn) {
            this.stage = stage;
            this.layout = layout;
            this.pics = pics;
            this.stairs = stairs;
            this.mapStage = mapStage;
            this.turn = turn;
        }
    }

    private static Pic P(String t, int f, int r, float x, float y, int w, int h) { return new Pic(t, f, r, x, y, w, h); }
    private static Stair S(int f, int r, float x, float y, boolean up) { return new Stair(f, r, x, y, up); }

    private static final Stair[] MAJROOM_STAIRS = {S(0, 1, 159.7f, -154.9f, true), S(0, 4, -233.3f, 52.4f, true)};
    private static final Stair[] NEWD2_STAIRS = {S(0, 14, -147.1f, 178.5f, true), S(0, 2, 30.5f, 118.3f, true), S(1, 8, 77.2f, -84f, true),
            S(1, 2, 31.1f, 117.3f, false), S(1, 14, -181f, 165.6f, false), S(1, 7, 218f, 16f, true), S(2, 9, -61.3f, -207.7f, true),
            S(2, 8, 66.6f, -119f, false), S(2, 7, 172.6f, 85f, false), S(3, 9, -268.4f, -38.2f, false), S(3, 9, 13.6f, -147.2f, false)};
    private static final Stair[] KINDAN_STAIRS = {S(1, 5, 13f, 102f, false), S(2, 9, 7.7f, 263.7f, true), S(3, 9, 6.2f, 260.2f, false),
            S(-1, 5, 14f, 103f, true)};
    private static final Stair[] SIREN_STAIRS = {S(0, 6, 0.4f, 4f, true), S(1, 6, 1f, -83.4f, false), S(1, 7, 2.2f, 84f, true),
            S(2, 17, 1.2f, 128f, false), S(2, 18, -126f, 111f, true), S(3, 18, -125.2f, 146.3f, false)};
    private static final Stair[] DAI_STAIRS = {S(0, 3, 69.2f, 143.1f, false), S(-1, 15, -206f, -278.7f, false), S(-1, 3, 38f, 141.6f, true),
            S(-2, 15, -147.1f, -278.4f, true)};
    private static final Stair[] KAZE_STAIRS = {S(0, 16, -169.2f, 303.1f, true), S(1, 16, -217f, 303.1f, false)};

    // majroom (DungeonMapFloor_00)
    private static final Pic[] MAJROOM = {
            P("MapMajyu1F_00^t", 0, 0, -65f, -154f, 280, 130),
            P("MapMajyu1F_01^t", 0, 1, 147f, -85f, 180, 250),
            P("MapMajyu1F_02^t", 0, 2, 167f, 141f, 200, 240),
            P("MapMajyu1F_03^t", 0, 3, -55f, 228f, 300, 250),
            P("MapMajyu1F_04^t", 0, 4, -214f, 78f, 110, 110),
            P("MapMajyu2F_00^t", 1, 0, -160f, -254f, 130, 280),
            P("MapMajyu2F_01^t", 1, -1, -13f, -165f, 189, 94),
            P("MapMajyu2F_02^t", 1, 1, 120f, -155f, 110, 110),
            P("MapMajyu2F_03^t", 1, -1, 178f, -46f, 114, 174),
            P("MapMajyu2F_04^t", 1, 2, 221.2f, 78.5f, 90, 120),
            P("MapMajyu2F_05^t", 1, -1, 154.1f, 181f, 160, 150),
            P("MapMajyu2F_06^t", 1, 3, 9f, 306f, 180, 180),
            P("MapMajyu2F_07^t", 1, -1, -125f, 181f, 160, 150),
            P("MapMajyu2F_08^t", 1, 4, -215f, 76f, 110, 110),
            P("MapMajyu2F_09^t", 1, -1, -181f, -46f, 95, 174)};

    // M_NewD2 (DungeonMapFloor_01)
    private static final Pic[] NEWD2 = {
            P("MapRyu1F_00^t", 0, 3, -237f, 116f, 198, 284),
            P("MapRyu1F_01^t", 0, 11, -205f, 28f, 166, 200),
            P("MapRyu1F_02^t", 0, 14, -204f, 172f, 166, 88),
            P("MapRyu1F_03^t", 0, 0, -79f, -242f, 190, 280),
            P("MapRyu1F_04^t", 0, 2, -18f, 66f, 314, 386),
            P("MapRyu1F_05^t", 0, 15, 77f, 282f, 260, 114),
            P("MapRyu1F_06^t", 0, 1, 98f, -100f, 213, 178),
            P("MapRyu1F_07^t", 0, 13, 172f, 18f, 148, 178),
            P("MapRyu1F_08^t", 0, 12, 258f, 240f, 150, 200),
            P("MapRyu2F_00^t", 1, 8, 18f, -132f, 200, 100),
            P("MapRyu2F_01^t", 1, 6, 18f, -68f, 200, 120),
            P("MapRyu2F_02^t", 1, 2, -4f, 88f, 244, 222),
            P("MapRyu2F_03^t", 1, 4, 32f, 246f, 171, 132),
            P("MapRyu2F_04^t", 1, 14, -142f, 194f, 80, 126),
            P("MapRyu2F_05^t", 1, 3, -72f, 320f, 220, 164),
            P("MapRyu2F_06^t", 1, 7, 166f, 74f, 176, 118),
            P("MapRyu3F_00^t", 2, 9, 74f, -61f, 536, 426),
            P("MapRyu3F_01^t", 2, 10, 6f, -8f, 120, 288),
            P("MapRyu3F_02^t", 2, 8, 80f, -102f, 60, 104),
            P("MapRyu3F_03^t", 2, 7, 180f, 82f, 182, 108),
            P("MapRyu4F_00^t", 3, 9, -170f, -18f, 382, 304)};

    // kindan (DungeonMapFloor_02)
    private static final Pic[] KINDAN = {
            P("MapKindan1F_00^t", 0, 0, 20f, -357f, 118, 80),
            P("MapKindan1F_01^t", 0, 1, 16f, -262f, 110, 138),
            P("MapKindan1F_02^t", 0, 5, 13f, 100f, 132, 132),
            P("MapKindan2F_00^t", 1, 1, 14f, -262f, 110, 138),
            P("MapKindan2F_01^t", 1, 2, 14f, -170f, 110, 90),
            P("MapKindan2F_02^t", 1, 3, 14f, -90f, 110, 115),
            P("MapKindan2F_03^t", 1, 5, 13f, 100f, 132, 132),
            P("MapKindan2F_04^t", 1, 15, 126f, 83f, 130, 76),
            P("MapKindan2F_05^t", 1, 16, 225f, 83f, 96, 76),
            P("MapKindan3F_00^t", 2, 7, -181f, 104f, 110, 120),
            P("MapKindan3F_01^t", 2, 6, -93f, 104f, 100, 54),
            P("MapKindan3F_02^t", 2, 1, 12f, -269f, 110, 152),
            P("MapKindan3F_03^t", 2, 3, 18f, -88f, 100, 110),
            P("MapKindan3F_04^t", 2, 4, 16f, -3f, 56, 91),
            P("MapKindan3F_05^t", 2, 5, 13f, 100f, 132, 132),
            P("MapKindan3F_06^t", 2, 8, 6f, 195f, 92, 78),
            P("MapKindan3F_07^t", 2, 9, 6f, 263f, 92, 90),
            P("MapKindan4F_00^t", 3, 5, 15f, 99f, 132, 132),
            P("MapKindan4F_01^t", 3, 8, 10f, 194f, 107, 78),
            P("MapKindan4F_02^t", 3, 9, 10f, 261f, 107, 90),
            P("MapKindan4F_03^t", 3, 10, 6f, 337f, 98, 90),
            P("MapKindanB1_00^t", -1, 13, -148.1f, 60f, 122, 88),
            P("MapKindanB1_01^t", -1, 12, -180f, 108f, 184, 148),
            P("MapKindanB1_02^t", -1, 14, -149f, 238f, 122, 144),
            P("MapKindanB1_03^t", -1, 11, -71f, 103f, 106, 94),
            P("MapKindanB1_04^t", -1, 1, 17f, -263f, 110, 138),
            P("MapKindanB1_05^t", -1, 5, 14f, 102f, 90, 94)};

    // Siren (DungeonMapFloor_03)
    private static final Pic[] SIREN = {
            P("MapKami1F_00^t", 0, 2, -188f, -252f, 122, 74),
            P("MapKami1F_01^t", 0, 1, -170f, -142f, 86, 74),
            P("MapKami1F_02^t", 0, 0, 67f, -237f, 420, 264),
            P("MapKami1F_03_2F_04^t", 0, 6, 2f, -50f, 70, 150),
            P("MapKami1F_04^t", 0, 3, 171f, -266f, 90, 99),
            P("MapKami1F_05^t", 0, 5, 235f, -330f, 68, 120),
            P("MapKami1F_06^t", 0, 4, 211f, -134f, 168, 58),
            P("MapKami2F_00^t", 1, 16, -225f, 89f, 124, 96),
            P("MapKami2F_01^t", 1, 23, -123.4f, -27f, 103, 100),
            P("MapKami2F_02^t", 1, 14, -115f, 74f, 120, 126),
            P("MapKami2F_03^t", 1, 21, -107f, 227f, 100, 110),
            P("MapKami1F_03_2F_04^t", 1, 6, 2f, -50f, 70, 150),
            P("MapKami2F_05^t", 1, 7, 0f, 84f, 152, 148),
            P("MapKami2F_06^t", 1, 8, 0f, 213f, 152, 138),
            P("MapKami2F_07^t", 1, 10, 3.2f, 328f, 80, 120),
            P("MapKami2F_08^t", 1, 22, 128f, -27f, 110, 100),
            P("MapKami2F_09^t", 1, 11, 119f, 72f, 126, 126),
            P("MapKami2F_10^t", 1, 9, 114f, 222f, 116, 120),
            P("MapKami2F_11^t", 1, 13, 230f, 86f, 124, 98),
            P("MapKami3F_00^t", 2, 17, 1f, 116f, 126, 150),
            P("MapKami3F_01^t", 2, 18, -65f, 194f, 184, 148),
            P("MapKami4F_00^t", 3, 18, -110f, 108f, 94, 52)};

    // M_Dai (DungeonMapFloor_04)
    private static final Pic[] DAI = {
            P("MapDaichi1F_00^t", 0, 0, -291f, -60f, 204, 132),
            P("MapDaichi1F_01^t", 0, 1, -291f, 120f, 203, 244),
            P("MapDaichi1F_02^t", 0, 18, -135f, -75f, 86, 118),
            P("MapDaichi1F_03^t", 0, 2, -143f, 165f, 109, 156),
            P("MapDaichi1F_04^t", 0, 4, -23.4f, -33f, 155, 201),
            P("MapDaichi1F_05^t", 0, 3, -8f, 148f, 185, 192),
            P("MapDaichi1F_06^t", 0, 5, 4f, 261.2f, 157, 120),
            P("MapDaichi1F_07^t", 0, 12, 203f, -48f, 174, 171),
            P("MapDaichi1F_08^t", 0, 7, 177f, 134f, 200, 220),
            P("MapDaichi1F_09^t", 0, 6, 152f, 261f, 152, 120),
            P("MapDaichi1F_10^t", 0, 8, 329f, 72f, 124, 96),
            P("MapDaichiB1_00^t", -1, 15, -148f, -230f, 292, 179),
            P("MapDaichiB1_01^t", -1, 14, 32f, -206f, 150, 132),
            P("MapDaichiB1_02^t", -1, 11, 33f, -28f, 150, 225),
            P("MapDaichiB1_03^t", -1, 3, 76f, 139f, 40, 40),
            P("MapDaichiB1_04^t", -1, 13, 189f, -204f, 194, 230),
            P("MapDaichiB1_05^t", -1, 10, 162f, -117f, 140, 144),
            P("MapDaichiB1_06^t", -1, 9, 193f, 57f, 202, 204),
            P("MapDaichiB2_00^t", -2, 19, -356f, -86f, 88, 88),
            P("MapDaichiB2_01^t", -2, 15, -180f, -236f, 266, 147),
            P("MapDaichiB2_02^t", -2, 16, -180f, -41f, 266, 254),
            P("MapDaichiB2_03^t", -2, 17, -180f, 142f, 266, 130)};

    // kaze (DungeonMapFloor_05)
    private static final Pic[] KAZE = {
            P("MapKaze1F_00^t", 0, 15, -308f, -295f, 78, 98),
            P("MapKaze1F_01^t", 0, 0, -308f, -141f, 78, 221),
            P("MapKaze1F_02^t", 0, 1, -308f, 55f, 78, 178),
            P("MapKaze1F_03^t", 0, 3, -308f, 204f, 78, 126),
            P("MapKaze1F_04^t", 0, 16, -265f, 297f, 162, 96),
            P("MapKaze1F_05^t", 0, 7, -15f, -38f, 387, 315),
            P("MapKaze1F_06^t", 0, 2, -175f, 164f, 205, 219),
            P("MapKaze1F_07^t", 0, 5, 36f, 158f, 227, 102),
            P("MapKaze1F_08^t", 0, 8, 200f, 158f, 120, 102),
            P("MapKaze2F_00^t", 1, 4, -176f, 5f, 80, 120),
            P("MapKaze2F_01^t", 1, 2, -177f, 166f, 211, 215),
            P("MapKaze2F_02^t", 1, 16, -176f, 300f, 49, 73),
            P("MapKazeB1_00^t", -1, 9, -189f, 36.9f, 144, 122),
            P("MapKazeB1_01^t", -1, 2, -189f, 160f, 144, 146),
            P("MapKazeB1_02^t", -1, 6, -189f, 282f, 144, 114),
            P("MapKazeB1_03^t", -1, 10, -72f, 160f, 108, 146),
            P("MapKazeB1_04^t", -1, 11, 150f, 160f, 349, 146),
            P("MapKazeB1_05^t", -1, 12, 305f, -25f, 83, 322)};

    static final Dungeon[] ALL = {
            new Dungeon("majroom", "DungeonMapFloor_00", MAJROOM, MAJROOM_STAIRS),
            // the Forsaken Fortress again (2nd / 3rd visit): majroom's rooms, the same collision turned by -65 degrees
            // about the origin (every vertex, exactly); their own DMAP (another origin and scale) doesn't place these pictures
            new Dungeon("ma2room", "DungeonMapFloor_00", MAJROOM, MAJROOM_STAIRS, "majroom", -65f),
            new Dungeon("ma3room", "DungeonMapFloor_00", MAJROOM, MAJROOM_STAIRS, "majroom", -65f),
            new Dungeon("M_NewD2", "DungeonMapFloor_01", NEWD2, NEWD2_STAIRS),
            new Dungeon("kindan", "DungeonMapFloor_02", KINDAN, KINDAN_STAIRS),
            new Dungeon("Siren", "DungeonMapFloor_03", SIREN, SIREN_STAIRS),
            new Dungeon("M_Dai", "DungeonMapFloor_04", DAI, DAI_STAIRS),
            new Dungeon("kaze", "DungeonMapFloor_05", KAZE, KAZE_STAIRS)};

    /** the dungeon of a stage, or null */
    static Dungeon of(String stage) {
        for (Dungeon d : ALL)
            if (d.stage.equals(stage)) return d;
        return null;
    }

    private MiniMapDungeons() {}
}
