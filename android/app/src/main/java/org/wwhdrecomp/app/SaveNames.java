package org.wwhdrecomp.app;

import java.nio.charset.Charset;

/**
 * Mods > Save file names: a save file's player name changed in play, up to 16 characters (the game's own name entry
 * allows 8). The runtime writes it into the game's memory (runtime/src/mods/save_names.cpp); the game's Save keeps
 * the first 8 (the file select shows those). A longer name is kept here ("save_name_long_N") and put back by the
 * runtime whenever that file's own name still starts with its first 8 characters.
 */
final class SaveNames {
    static final int FILES = 3;
    static final int MAX = 16;
    static final int OWN = 8;
    /** renames asked for, by file (1..3), until the game's memory has them (the game is paused under the menu) */
    static final String[] asked = new String[FILES + 1];

    private SaveNames() {}

    static String pref(int file) { return "save_name_long_" + file; }

    /** the long names to the runtime (any time; at the start too) */
    static void handOver(MainActivity a) {
        Native.setSaveNames(a.prefs.getString(pref(1), null), a.prefs.getString(pref(2), null), a.prefs.getString(pref(3), null));
    }

    /** the names in the game's memory, file 1..3 at 0..2; null when the game has none there (not started, starting) */
    static String[] current() {
        try {
            return Native.saveNames();
        } catch (UnsatisfiedLinkError e) {
            return null;
        }
    }

    /** the name as the game takes it: a letter typed as letter + accent mark (é as e + ´, が as か + ゛, as some
     *  keyboards send them) made one character (NFC), spaces at the ends gone */
    static String clean(String name) {
        return java.text.Normalizer.normalize(name, java.text.Normalizer.Form.NFC).trim();
    }

    /** the first character the game can't show, null if it can show them all (no control characters; outside Japan
     *  the Western letters of the game's name entry, Windows-1252) */
    static String badChar(MainActivity a, String name) {
        boolean jpn = "JPN".equals(Native.gameRelease(a.gameDir()));
        java.nio.charset.CharsetEncoder enc = Charset.forName("windows-1252").newEncoder();
        for (int i = 0; i < name.length(); ) {
            int cp = name.codePointAt(i);
            String ch = new String(Character.toChars(cp));
            if (Character.isISOControl(cp) || Character.isSupplementaryCodePoint(cp) || (!jpn && !enc.canEncode(ch))) return ch;
            i += Character.charCount(cp);
        }
        return null;
    }

    /** renames save file `file` (1..3) in the game now; false if the name can't be used (empty) */
    static boolean rename(MainActivity a, int file, String name) {
        name = clean(name);
        if (name.isEmpty()) return false;
        if (name.length() > MAX) name = name.substring(0, MAX);
        android.content.SharedPreferences.Editor e = a.prefs.edit();
        if (name.length() > OWN) e.putString(pref(file), name);
        else e.remove(pref(file));
        e.apply();
        handOver(a);
        Native.renameSaveFile(file, name);
        asked[file] = name;
        return true;
    }
}
