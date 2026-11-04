package org.wwhdrecomp.app;

/**
 * A new game in a save file starts as the game's Link: the player model, Link's colours and the long name chosen for
 * that file belong to the game that was there before (the choices are kept per file number, and a new game takes a
 * free file). A new game is told by its event flags as the game's init leaves them (all clear but 0x7E and 0xBE) until
 * Link wakes up (the "awake" scene sets 0x3510); the runtime shows the game's Link meanwhile (mods.h save_new_game),
 * and here the file's choices are let go (read every two seconds with the achievements).
 */
final class NewGames {
    private static final int EVENTS = 0x624, EVENT_BYTES = 0x100;

    private NewGames() {}

    static boolean isNew(byte[] d) {
        if (d == null || d.length < EVENTS + EVENT_BYTES) return false;
        for (int i = 0; i < EVENT_BYTES; i++)
            if (d[EVENTS + i] != 0 && i != 0x7E && i != 0xBE) return false;
        return true;
    }

    /** the save data read: a new game's file let go of the old game's model, colours and long name */
    static void check(MainActivity a, byte[] d) {
        if (a.prefs == null || !isNew(d) || d.length <= Achievements.FILE_NO) return;
        int no = d[Achievements.FILE_NO] & 0xFF;
        if (no >= PlayerModels.FILES) return;
        int file = no + 1;
        boolean model = a.prefs.contains(PlayerModels.pref(file)), colours = LinkColors.on(a, file),
                name = a.prefs.contains(SaveNames.pref(file));
        if (!model && !colours && !name) return;
        android.util.Log.i("wwhd", "new game in save file " + file + ": the game's Link (was: " + (model ? "model " : "")
                + (colours ? "colours " : "") + (name ? "long name" : "") + ")");
        a.prefs.edit().remove(PlayerModels.pref(file)).remove(SaveNames.pref(file)).putBoolean(LinkColors.prefOn(file), false).apply();
        if (model) PlayerModels.handOver(a);
        if (name) SaveNames.handOver(a);
        if (colours) LinkColors.remakeSoon(a);
    }
}
