package org.wwhdrecomp.app;

import java.io.File;

/**
 * Mods > Mod packs > Player models: a GameCube player model (or HD Link) per save file, put into the game's memory by
 * the runtime when that file is played (runtime/src/mods/player_models.cpp: no restart, the game's pack stays as it
 * is). The models are mod packs "gcmodel-*" keeping their converted model/Link.szs (ModPacks).
 */
final class PlayerModels {
    static final int FILES = 3;

    private PlayerModels() {}

    static String pref(int file) { return "player_model_" + file; }

    /** save file `file`'s model folder (1..3), null for HD Link (also when its pack is gone) */
    static String get(MainActivity a, int file) {
        String f = a.prefs.getString(pref(file), null);
        if (f == null || !ModPacks.isPlayerModel(f) || !new File(new File(ModPacks.dir(a.baseDir()), f), ModPacks.MODEL_SZS).isFile()) return null;
        return f;
    }

    static void set(MainActivity a, int file, String folder) {
        if (folder == null) a.prefs.edit().remove(pref(file)).apply();
        else a.prefs.edit().putString(pref(file), folder).apply();
        handOver(a);
    }

    /** the files' models to the runtime (it takes them over live) */
    static void handOver(MainActivity a) {
        File base = a.baseDir();
        Native.setPlayerModels(ModPacks.basePack(a.gameDir(), Randomizer.runtimeDir(a)).getAbsolutePath(), ModPacks.dir(base).getAbsolutePath(),
                get(a, 1), get(a, 2), get(a, 3));
    }

    /**
     * Before the game starts. Once: a model switched on the way it was before (its own permanent_3d.pack over the
     * game's, for all files) becomes every save file's model, and its pack goes.
     */
    static void prepare(MainActivity a) {
        File base = a.baseDir();
        String on = ModPacks.playerModelOn(base, a.prefs);
        if (on != null) {
            android.content.SharedPreferences.Editor e = a.prefs.edit();
            for (int f = 1; f <= FILES; f++) if (!a.prefs.contains(pref(f))) e.putString(pref(f), on);
            e.apply();
            ModPacks.choosePlayerModel(base, a.gameDir(), a.prefs, null, null);
        }
        handOver(a);
    }

    /** the save file in play, 1..3 (0: none: the title, the file select) */
    static int playedFile() {
        byte[] d = Native.progressData();
        if (d == null || d.length <= Achievements.FILE_NO) return 0;
        int no = d[Achievements.FILE_NO] & 0xFF;
        return no < FILES ? no + 1 : 0;
    }

    /** the name shown for a file's model */
    static String name(MainActivity a, int file) {
        String f = get(a, file);
        return f == null ? a.getString(R.string.packs_gc_off) : ModPacks.playerModelName(a.baseDir(), f);
    }
}
