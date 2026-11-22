package org.wwhdrecomp.app;

import java.io.File;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;

/**
 * Game > Online > Plays online: which games and save files go online (Native.coopScope; the runtime's side is
 * runtime/src/online/coop_sync_android.cpp). Every game (the game as it is, each randomized game, each online game) is
 * off, online as a whole (the session from the title on, all its save files) or online in the save files chosen (the
 * session only while one of them is played: the party's progress never comes into the others). Each of those games or
 * save files keeps its own online settings (server, name, tunic, ...: the runtime's settings profile "<key>" or
 * "<key>-f<n>").
 *
 * Online games: games of their own for playing online, with the game's code as it is but their own save and save states
 * (files/onlinegames/<n>/: save/, states/, name.txt), so a party's progress never meets the other games' saves. One is
 * picked at the start like a randomized game (MainActivity.askGame); they are always online as a whole.
 */
final class OnlineGames {
    static final String PREF_GAME = "online_game";  // the online game chosen (0 none: Randomizer's choice)
    static final int OFF = 0, WHOLE = 1, FILES = 2;

    private OnlineGames() {}

    static File root(MainActivity a) { return new File(a.baseDir(), "onlinegames"); }
    static File gameDir(MainActivity a, int n) { return new File(root(a), String.valueOf(n)); }

    static boolean exists(MainActivity a, int n) { return n > 0 && new File(gameDir(a, n), "name.txt").isFile(); }

    /** The online games made, in the order made. */
    static List<Integer> games(MainActivity a) {
        List<Integer> r = new ArrayList<>();
        if (!BuildConfig.ONLINE) return r;  // (a build without online: none shown, none played)
        String[] names = root(a).list();
        if (names != null) for (String s : names) {
            try {
                int n = Integer.parseInt(s);
                if (exists(a, n)) r.add(n);
            } catch (NumberFormatException ignored) {
            }
        }
        Collections.sort(r);
        return r;
    }

    /** Its name as given, or "Online game n". */
    static String name(MainActivity a, int n) {
        try {
            String s = new String(Files.readAllBytes(new File(gameDir(a, n), "name.txt").toPath()), StandardCharsets.UTF_8).trim();
            if (!s.isEmpty()) return s;
        } catch (java.io.IOException ignored) {
        }
        return a.getString(R.string.online_game_default_name, n);
    }

    static void rename(MainActivity a, int n, String name) {
        try {
            Files.write(new File(gameDir(a, n), "name.txt").toPath(), name.trim().getBytes(StandardCharsets.UTF_8));
        } catch (java.io.IOException e) {
            android.util.Log.w("wwhd", "online game " + n + ": name not written: " + e);
        }
    }

    /** A new online game (its folders, its name); its number. */
    static int create(MainActivity a, String name) {
        List<Integer> all = games(a);
        int n = all.isEmpty() ? 1 : all.get(all.size() - 1) + 1;
        File dir = gameDir(a, n);
        Backup.deleteTree(dir);
        //noinspection ResultOfMethodCallIgnored
        new File(dir, "save").mkdirs();
        //noinspection ResultOfMethodCallIgnored
        new File(dir, "states").mkdirs();
        rename(a, n, name.trim().isEmpty() ? a.getString(R.string.online_game_default_name, n) : name);
        return n;
    }

    /** Its save or save states exist. */
    static boolean hasProgress(MainActivity a, int n) {
        if (new File(gameDir(a, n), "save/user/cking.sav").exists()) return true;
        String[] states = new File(gameDir(a, n), "states").list();
        return states != null && states.length > 0;
    }

    /** Deletes online game n with its save, save states and its settings profile. */
    static void delete(MainActivity a, int n) {
        if (chosen(a) == n) a.prefs.edit().putInt(PREF_GAME, 0).commit();
        Backup.deleteTree(gameDir(a, n));
        //noinspection ResultOfMethodCallIgnored
        new File(new File(new File(a.getFilesDir(), "online"), "profiles"), "o" + n + ".ini").delete();
    }

    static int chosen(MainActivity a) { return a.prefs.getInt(PREF_GAME, 0); }

    // decided once per process, at the start (the runtime was started with its save folder)
    private static Integer playing;

    /** Before the runtime starts: online game n (0: the Randomizer's choice) is played. */
    static void choose(MainActivity a, int n) {
        if (n == 0) a.prefs.edit().putInt(PREF_GAME, 0).commit();
        else a.prefs.edit().putInt(PREF_GAME, n).putInt(Randomizer.PREF_GAME, 0).commit();  // (its code: the game as it is)
        playing = null;
    }

    /** The online game this run plays, 0 none. */
    static int playingGame(MainActivity a) {
        if (playing == null) playing = BuildConfig.ONLINE && exists(a, chosen(a)) ? chosen(a) : 0;
        return playing;
    }

    static boolean playing(MainActivity a) { return playingGame(a) != 0; }

    // ---- which games and save files play online

    /** This run's game: "o<n>" an online game, "r<n>" a randomized game, "p" the game as it is. */
    static String key(MainActivity a) {
        if (playing(a)) return "o" + playingGame(a);
        if (Randomizer.playing(a)) return "r" + Randomizer.playingGame(a);
        return "p";
    }

    /** OFF, WHOLE or FILES for this run's game (an online game: always WHOLE). */
    static int scope(MainActivity a) {
        if (!BuildConfig.ONLINE) return OFF;
        if (playing(a)) return WHOLE;
        return a.prefs.getInt("online_scope_" + key(a), OFF);
    }

    /** The save files chosen (bit n-1: file n). */
    static int files(MainActivity a) { return a.prefs.getInt("online_files_" + key(a), 0) & 7; }

    /** The save file whose settings the Online page shows while none of the chosen is played. */
    static int editFile(MainActivity a) {
        int f = a.prefs.getInt("online_edit_" + key(a), 0);
        if (f >= 1 && f <= 3) return f;
        int files = files(a);
        for (int k = 1; k <= 3; k++) if ((files & 1 << (k - 1)) != 0) return k;
        return 1;
    }

    static void setScope(MainActivity a, int scope) {
        a.prefs.edit().putInt("online_scope_" + key(a), scope).commit();
        handOver(a);
    }

    static void setFile(MainActivity a, int file, boolean on) {
        int f = files(a);
        f = on ? f | 1 << (file - 1) : f & ~(1 << (file - 1));
        a.prefs.edit().putInt("online_files_" + key(a), f).commit();
        handOver(a);
    }

    static void setEditFile(MainActivity a, int file) {
        a.prefs.edit().putInt("online_edit_" + key(a), file).commit();
        handOver(a);
    }

    // ---- the same, for a game not being played (the start question, the randomizer's pages)

    static int scopeOf(MainActivity a, String key) { return !BuildConfig.ONLINE ? OFF : key.startsWith("o") ? WHOLE : a.prefs.getInt("online_scope_" + key, OFF); }

    static int filesOf(MainActivity a, String key) { return a.prefs.getInt("online_files_" + key, 0) & 7; }

    static void setScopeOf(MainActivity a, String key, int scope) {
        a.prefs.edit().putInt("online_scope_" + key, scope).commit();
        if (key.equals(key(a))) handOver(a);
    }

    /** What the start question adds to a game's name: "" offline, " · online", " · online: files 1, 3". */
    static String mark(MainActivity a, String key) {
        int scope = scopeOf(a, key);
        if (scope == WHOLE) return " · " + a.getString(R.string.online_mark);
        if (scope != FILES || filesOf(a, key) == 0) return "";
        StringBuilder b = new StringBuilder();
        for (int k = 1; k <= 3; k++)
            if ((filesOf(a, key) & 1 << (k - 1)) != 0) b.append(b.length() > 0 ? ", " : "").append(k);
        return " · " + a.getString(R.string.online_mark_files, b.toString());
    }

    /** Game > Randomizer: whether the next randomized game plays online (OFF, WHOLE or FILES), set on it once made. */
    static final String PREF_RANDO_NEXT = "online_rando_next";

    static void madeRando(MainActivity a, int n) {
        int scope = a.prefs.getInt(PREF_RANDO_NEXT, OFF);
        if (scope != OFF && BuildConfig.ONLINE) a.prefs.edit().putInt("online_scope_r" + n, scope).commit();
    }

    /** A friend's seed made from their text (OptionsMenu.coopSeedFrom): their server, "address\tport", for that game. */
    static final String PREF_SERVER_AFTER = "coop_server_after";

    /** At the start: a friend's randomized game plays online as a whole through their server (its own settings). */
    static void applyPending(MainActivity a) {
        String srv = a.prefs.getString(PREF_SERVER_AFTER, "");
        if (srv.isEmpty() || !Randomizer.playing(a) || !BuildConfig.ONLINE) return;
        a.prefs.edit().remove(PREF_SERVER_AFTER).commit();
        String[] f = srv.split("\t", -1);
        if (f.length < 2) return;
        if (scope(a) == OFF) a.prefs.edit().putInt("online_scope_" + key(a), WHOLE).commit();
        handOver(a);
        try {
            Native.coopSaveSettings("server=1\nserver_addr=" + f[0] + "\nserver_port=" + f[1]);
        } catch (UnsatisfiedLinkError ignored) {
        }
    }

    /** To the runtime (at the start, after each change). */
    static void handOver(MainActivity a) {
        try {
            Native.coopScope(key(a), scope(a), files(a), editFile(a));
        } catch (UnsatisfiedLinkError ignored) {  // (a build without online co-op)
        }
    }

    /** {profile in use, save file played (0 none), 1 when online should run}. */
    static String[] now() {
        try {
            String[] f = Native.coopScopeNow().split("\t", -1);
            return f.length >= 3 ? f : new String[] {"", "0", "0"};
        } catch (UnsatisfiedLinkError e) {
            return new String[] {"", "0", "0"};
        }
    }

    /**
     * A save file deleted in the file select lets go of its online mark (a new game there starts offline), as the mod
     * packs per save file do (ModPacks.slotsCheck): read with the achievements while the file select's list shows.
     */
    static void slotsCheck(MainActivity a) {
        if (a.prefs == null || SavePages.viewing() || playing(a) || scope(a) != FILES || files(a) == 0) return;
        int used = Native.saveSlotsUsed();
        if (used < 0) return;
        String usedKey = "online_slots_used_" + key(a);
        int before = a.prefs.getInt(usedKey, -1);
        if (before == used) return;
        a.prefs.edit().putInt(usedKey, used).commit();
        if (before < 0) return;
        int files = files(a), left = files;
        for (int k = 0; k < 3; k++)
            if ((before & 1 << k) != 0 && (used & 1 << k) == 0 && (files & 1 << k) != 0) {
                android.util.Log.i("wwhd", "save file " + (k + 1) + " deleted: no longer online");
                left &= ~(1 << k);
                //noinspection ResultOfMethodCallIgnored
                new File(new File(new File(a.getFilesDir(), "online"), "profiles"), key(a) + "-f" + (k + 1) + ".ini").delete();
            }
        if (left != files) {
            a.prefs.edit().putInt("online_files_" + key(a), left).commit();
            handOver(a);
        }
    }
}
