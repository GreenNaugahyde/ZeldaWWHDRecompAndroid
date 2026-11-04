package org.wwhdrecomp.app;

import android.content.SharedPreferences;
import android.content.res.AssetManager;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.StandardCopyOption;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;

/**
 * Game > Randomizer: TWWHD Randomizer (RandoNative, runtime/third_party/twwhd_rando) makes randomized games from the
 * game's release; the app plays one of them or the game as it is (Native.setRandomizer: its executable compiled in place
 * of the game's, its files over the game's). Which one is asked at each start (MainActivity.askGame) while there is one.
 *
 * Folders below files/rando: data/ (the randomizer's data from assets/rando, per app version), current/ (its settings
 * for the next game made), games/<n>/ (one randomized game each, numbered as made: its settings, logs, last.txt with
 * the seed's hash and permalink, out/ = its changed files, save/ and states/ = its own game save and save states: a
 * randomized save never meets the game as it is, nor one from another seed). A new game is made at the next start
 * (pending), before the game code check: a run needs about a gigabyte of memory and the game isn't running then.
 */
final class Randomizer {
    static final String PREF_GAME = "rando_game", PREF_PENDING = "rando_pending", PREF_KEEP_PLAIN = "rando_keep_plain";
    // a game made again at the next start with Link's colours as they are now (its own settings and seed: the same game)
    static final String PREF_REMAKE = "rando_remake";
    private static final String PREF_ON_OLD = "rando_on";  // (before games/: the one game in current/ played)

    private Randomizer() {}

    static File root(MainActivity a) { return new File(a.baseDir(), "rando"); }
    static File dataDir(MainActivity a) { return new File(root(a), "data"); }
    static File workDir(MainActivity a) { return new File(root(a), "current"); }
    static File gameDir(MainActivity a, int n) { return new File(new File(root(a), "games"), String.valueOf(n)); }
    static File outDir(MainActivity a, int n) { return new File(gameDir(a, n), "out"); }

    /** Randomized game n is made (its executable is there). */
    static boolean made(MainActivity a, int n) { return n > 0 && new File(outDir(a, n), "code/cking.rpx").isFile(); }

    /** Randomized game n was made from the release in the game folder (last.txt; games made before it knew: USA). */
    static boolean madeForThisRelease(MainActivity a, int n) {
        String r = lastInfo(a, n, "release");
        return (r != null ? r : "USA").equals(Native.gameRelease(a.gameDir()));
    }

    /** The randomized games made (all releases), in the order made. */
    static List<Integer> allGames(MainActivity a) {
        migrate(a);
        List<Integer> r = new ArrayList<>();
        String[] names = new File(root(a), "games").list();
        if (names != null) for (String s : names) {
            try {
                int n = Integer.parseInt(s);
                if (made(a, n)) r.add(n);
            } catch (NumberFormatException ignored) {
            }
        }
        Collections.sort(r);
        return r;
    }

    /** The randomized games this game folder's release can play, in the order made. */
    static List<Integer> games(MainActivity a) {
        List<Integer> r = allGames(a);
        if (!releaseOk(a)) r.clear();
        r.removeIf(n -> !madeForThisRelease(a, n));
        return r;
    }

    /** The game release in the game folder is one the randomizer takes (USA; EUR and JPN by tools/rando/port_rando.py). */
    static boolean releaseOk(MainActivity a) {
        String r = Native.gameRelease(a.gameDir());
        return "USA".equals(r) || "EUR".equals(r) || "JPN".equals(r);
    }

    /** The game chosen to play: 0 the game as it is, else randomized game n (at the next start if it changed). */
    static int chosen(MainActivity a) {
        migrate(a);
        return a.prefs.getInt(PREF_GAME, 0);
    }

    // decided once per process, at the start (the runtime was started with it)
    private static Integer playing;

    /** Before the runtime starts (MainActivity.askGame): the game this run plays. */
    static void choose(MainActivity a, int n) {
        a.prefs.edit().putInt(PREF_GAME, n).commit();
        playing = null;
    }

    /** The randomized game this run plays, 0 for the game as it is. */
    static int playingGame(MainActivity a) {
        if (playing == null) {
            int n = chosen(a);
            playing = made(a, n) && releaseOk(a) && madeForThisRelease(a, n) ? n : 0;
        }
        return playing;
    }

    /** This run plays a randomized game. */
    static boolean playing(MainActivity a) { return playingGame(a) != 0; }

    /** The folder with save/ and states/ of the game this run plays. */
    static File playDir(MainActivity a) { return playing(a) ? gameDir(a, playingGame(a)) : a.baseDir(); }

    /** What Native.setRandomizer gets: the randomized game's folder or null. */
    static String runtimeDir(MainActivity a) { return playing(a) ? outDir(a, playingGame(a)).getAbsolutePath() : null; }

    /** The game's name for the start question and the menu: "Randomizer 2: Nayru Aldo". */
    static String name(MainActivity a, int n) {
        String hash = lastInfo(a, n, "hash");
        return hash != null ? a.getString(R.string.rando_game_name, n, hash) : a.getString(R.string.rando_game_name_nohash, n);
    }

    /** The compiled game code's room (all kept sets), in MB. */
    static long codeCacheMb(MainActivity a) {
        long n = 0;
        File[] fs = a.codeDir().listFiles();
        if (fs != null) for (File f : fs) n += f.length();
        return n >> 20;
    }

    /** The room a randomized game takes (its files, save and states), in MB. */
    static long gameMb(MainActivity a, int n) { return treeSize(gameDir(a, n)) >> 20; }

    private static long treeSize(File f) {
        if (f.isFile()) return f.length();
        long n = 0;
        File[] fs = f.listFiles();
        if (fs != null) for (File c : fs) n += treeSize(c);
        return n;
    }

    static boolean pending(SharedPreferences p) { return p.getBoolean(PREF_PENDING, false); }

    // before games/ (2026-10-30): current/ held the one game made next to the settings; it becomes game 1
    private static boolean migrated;

    private static void migrate(MainActivity a) {
        if (migrated) return;
        migrated = true;
        File cur = workDir(a);
        if (new File(cur, "out/code/cking.rpx").isFile()) {
            File g = gameDir(a, 1);
            //noinspection ResultOfMethodCallIgnored
            g.mkdirs();
            for (String n : new String[] {"out", "save", "states", "logs", "last.txt"}) {
                File f = new File(cur, n);
                //noinspection ResultOfMethodCallIgnored
                if (f.exists()) f.renameTo(new File(g, n));
            }
            for (String n : new String[] {"config.yaml", "preferences.yaml"}) copy(new File(cur, n), new File(g, n));
        }
        if (a.prefs.contains(PREF_ON_OLD)) {
            boolean on = a.prefs.getBoolean(PREF_ON_OLD, false);
            SharedPreferences.Editor e = a.prefs.edit().remove(PREF_ON_OLD);
            if (on && made(a, 1)) e.putInt(PREF_GAME, 1);
            e.commit();
        }
    }

    private static void copy(File from, File to) {
        try {
            if (from.isFile()) Files.copy(from.toPath(), to.toPath(), StandardCopyOption.REPLACE_EXISTING);
        } catch (IOException ignored) {
        }
    }

    // ---- the randomizer's settings (RandoNative; data extracted first: it reads its location list)

    static String data(MainActivity a) {
        ensureData(a);
        return dataDir(a).getAbsolutePath();
    }

    static String work(MainActivity a) {
        //noinspection ResultOfMethodCallIgnored
        workDir(a).mkdirs();
        return workDir(a).getAbsolutePath();
    }

    static int get(MainActivity a, String name) { return RandoNative.get(data(a), work(a), name); }
    static String set(MainActivity a, String name, int v) { return RandoNative.set(data(a), work(a), name, v); }
    static boolean on(MainActivity a, String name) { return get(a, name) > 0; }

    /** What game n's last.txt says: hash, permalink, seed, release; null when there is none. */
    static String lastInfo(MainActivity a, int n, String key) {
        try {
            for (String line : Files.readAllLines(new File(gameDir(a, n), "last.txt").toPath(), StandardCharsets.UTF_8)) {
                int tab = line.indexOf('\t');
                if (tab > 0 && line.substring(0, tab).equals(key)) return line.substring(tab + 1);
            }
        } catch (IOException ignored) {
        }
        return null;
    }

    /** Game n's spoiler log (the randomizer's logs/<hash> Spoiler Log.txt), or null. */
    static File spoilerLog(MainActivity a, int n) {
        File[] logs = new File(gameDir(a, n), "logs").listFiles((d, f) -> f.endsWith("Spoiler Log.txt") && !f.endsWith("Non-Spoiler Log.txt"));
        if (logs == null || logs.length == 0) return null;
        File best = logs[0];
        for (File f : logs) if (f.lastModified() > best.lastModified()) best = f;
        return best;
    }

    /** Game n's save or save states exist. */
    static boolean hasProgress(MainActivity a, int n) {
        if (new File(gameDir(a, n), "save/user/cking.sav").exists()) return true;
        String[] states = new File(gameDir(a, n), "states").list();
        return states != null && states.length > 0;
    }

    /** The randomized game made with the settings and seed as they are now (the same game again), 0 if none. */
    static int sameGame(MainActivity a) {
        String link = RandoNative.permalink(data(a), work(a));
        if (link == null || link.isEmpty()) return 0;
        for (int n : games(a)) if (link.equals(lastInfo(a, n, "permalink"))) return n;
        return 0;
    }

    /** Marks a new game to be made at the next start (the games made stay). */
    static void requestNew(MainActivity a) {
        a.prefs.edit().putBoolean(PREF_PENDING, true).commit();
    }

    /** Marks game n to be made again at the next start with Link's colours as they are now (its settings and seed stay). */
    static void requestRemake(MainActivity a, int n) {
        a.prefs.edit().putInt(PREF_REMAKE, n).putBoolean(PREF_PENDING, true).commit();
    }

    /** The game the next start makes again, 0 if none (a new game then, if one is pending). */
    static int remaking(SharedPreferences p) { return p.getBoolean(PREF_PENDING, false) ? p.getInt(PREF_REMAKE, 0) : 0; }

    /** Deletes randomized game n with its save and states; the game as it is is played next if it was chosen. */
    static void delete(MainActivity a, int n) {
        if (chosen(a) == n) a.prefs.edit().putInt(PREF_GAME, 0).commit();
        Backup.deleteTree(gameDir(a, n));
    }

    // ---- a run (MainActivity.runRandomizer, on a thread of its own)

    private static volatile File making;  // the game folder a run writes (its progress file)

    /** null when the game was made (and is the one chosen now), else why not. */
    static String generate(MainActivity a) {
        String data;
        try {
            data = data(a);
        } catch (RuntimeException e) {
            return "data: " + e.getMessage();
        }
        int again = a.prefs.getInt(PREF_REMAKE, 0);
        if (again != 0) {
            a.prefs.edit().remove(PREF_REMAKE).commit();
            if (made(a, again)) return remake(a, data, again);
        }
        List<Integer> all = allGames(a);
        int n = all.isEmpty() ? 1 : all.get(all.size() - 1) + 1;
        File dir = gameDir(a, n);
        Backup.deleteTree(dir);  // (a run that failed before)
        //noinspection ResultOfMethodCallIgnored
        dir.mkdirs();
        // the settings as they are now go with the game (the menu's stay for the next one)
        work(a);
        for (String f : new String[] {"config.yaml", "preferences.yaml"}) copy(new File(workDir(a), f), new File(dir, f));
        making = dir;
        String err = RandoNative.generate(data, dir.getAbsolutePath(), a.gameDir(), new File(dir, "out").getAbsolutePath());
        if (err != null) Backup.deleteTree(dir);
        else choose(a, n);
        return err;
    }

    // game n again from its own settings and seed with the menu's preferences (Link's colours): into out.new, which
    // replaces out/ only when it is the same game (the same hash); its save and states stay
    private static String remake(MainActivity a, String data, int n) {
        File dir = gameDir(a, n), out = outDir(a, n), neu = new File(dir, "out.new");
        File prefs = new File(dir, "preferences.yaml"), oldPrefs = new File(dir, "preferences.yaml.old");
        File last = new File(dir, "last.txt"), oldLast = new File(dir, "last.txt.old");
        String hash = lastInfo(a, n, "hash");
        copy(prefs, oldPrefs);
        copy(last, oldLast);
        work(a);
        copy(new File(workDir(a), "preferences.yaml"), prefs);
        Backup.deleteTree(neu);
        making = dir;
        String err = RandoNative.generate(data, dir.getAbsolutePath(), a.gameDir(), neu.getAbsolutePath());
        if (err == null && hash != null && !hash.equals(lastInfo(a, n, "hash"))) err = "not the same game (" + lastInfo(a, n, "hash") + ")";
        if (err == null) {
            Backup.deleteTree(out);
            if (!neu.renameTo(out)) err = "cannot replace " + out;
        }
        if (err != null) {  // the game as it was
            Backup.deleteTree(neu);
            copy(oldPrefs, prefs);
            copy(oldLast, last);
        } else choose(a, n);
        //noinspection ResultOfMethodCallIgnored
        oldPrefs.delete();
        //noinspection ResultOfMethodCallIgnored
        oldLast.delete();
        return err;
    }

    /** The run's progress: {percent, label}, or null before its first report. */
    static String[] progress(MainActivity a) {
        File dir = making;
        if (dir == null) return null;
        try {
            String s = new String(Files.readAllBytes(new File(dir, "progress").toPath()), StandardCharsets.UTF_8).trim();
            int tab = s.indexOf('\t');
            return tab < 0 ? null : new String[] {s.substring(0, tab), s.substring(tab + 1)};
        } catch (IOException e) {
            return null;
        }
    }

    // assets/rando -> files/rando/data, again after an app update (the randomizer's version or its data may change)
    private static void ensureData(MainActivity a) {
        File dir = dataDir(a), stamp = new File(dir, ".stamp");
        String want;
        try {
            want = RandoNative.version() + " " + a.getPackageManager().getPackageInfo(a.getPackageName(), 0).lastUpdateTime;
        } catch (android.content.pm.PackageManager.NameNotFoundException e) {
            want = RandoNative.version();
        }
        try {
            if (stamp.isFile() && new String(Files.readAllBytes(stamp.toPath()), StandardCharsets.UTF_8).equals(want)) return;
            Backup.deleteTree(dir);
            copyAssets(a.getAssets(), "rando", dir);
            Files.write(stamp.toPath(), want.getBytes(StandardCharsets.UTF_8));
        } catch (IOException e) {
            throw new RuntimeException("cannot unpack the randomizer's data: " + e.getMessage());
        }
    }

    private static void copyAssets(AssetManager am, String path, File to) throws IOException {
        String[] names = am.list(path);
        if (names == null || names.length == 0) {  // a file
            //noinspection ResultOfMethodCallIgnored
            to.getParentFile().mkdirs();
            try (InputStream in = am.open(path); OutputStream out = new FileOutputStream(to)) {
                byte[] buf = new byte[65536];
                for (int n; (n = in.read(buf)) > 0; ) out.write(buf, 0, n);
            }
            return;
        }
        //noinspection ResultOfMethodCallIgnored
        to.mkdirs();
        for (String n : names) copyAssets(am, path + "/" + n, new File(to, n));
    }
}
