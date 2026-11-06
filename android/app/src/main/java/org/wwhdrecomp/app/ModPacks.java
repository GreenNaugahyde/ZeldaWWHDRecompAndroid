package org.wwhdrecomp.app;

import android.content.ContentResolver;
import android.content.SharedPreferences;
import android.net.Uri;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/**
 * Mod packs: Cemu graphic packs (rules.txt + patch_*.asm) in files/packs/&lt;folder&gt;/. The native side
 * (runtime/src/packs.h) reads files/packs/enabled.txt at start; code patches recompile the game code.
 * Prefs: pack_on_&lt;folder&gt; (enabled), pack_sel_&lt;folder&gt; ("category\tpreset" lines).
 */
final class ModPacks {
    private ModPacks() {}

    static File dir(File base) { return new File(base, "packs"); }

    /** the installed packs (folders with a rules.txt), sorted */
    static List<String> list(File base) {
        List<String> out = new ArrayList<>();
        File[] fs = dir(base).listFiles();
        if (fs == null) return out;
        for (File f : fs)
            if (f.isDirectory() && new File(f, "rules.txt").isFile()) out.add(f.getName());
        out.sort(String.CASE_INSENSITIVE_ORDER);
        return out;
    }

    /** a pack as the native side describes it (Native.packDescribe) */
    static final class Info {
        String name = "", path = "", description = "";
        int groups, groupsOther, contentFiles, contentAdded, shaders, textureRules;
        String versions;  // the game versions rules.txt names, when this one isn't among them
        final List<String> added = new ArrayList<>();  // (some of) the files the game doesn't have
        boolean hasPatches;
        final List<String[]> categories = new ArrayList<>();  // {category, active, preset...}
        final List<String> notes = new ArrayList<>(), unsupported = new ArrayList<>();

        /** what the pack does for this game, in one line */
        String summary(android.content.Context c) {
            String files = contentFiles > 0 ? c.getResources().getQuantityString(R.plurals.packs_content, contentFiles, contentFiles) : null;
            if (textureRules > 0) {
                String tr = c.getResources().getQuantityString(R.plurals.packs_texture_rules, textureRules, textureRules);
                files = files == null ? tr : files + "; " + tr;
            }
            if (shaders > 0) {
                String sh = c.getResources().getQuantityString(R.plurals.packs_shaders, shaders, shaders);
                files = files == null ? sh : files + "; " + sh;
            }
            if (hasPatches && groups > 0) return c.getString(R.string.packs_applies) + (files != null ? "; " + files : "");
            if (files != null) return files;
            if (hasPatches && groupsOther > 0) return c.getString(R.string.packs_other_version);
            if (!notes.isEmpty()) return c.getString(R.string.packs_error, notes.get(0));
            if (!unsupported.isEmpty()) return c.getString(R.string.packs_unsupported, String.join(", ", unsupported));
            return c.getString(R.string.packs_nothing);
        }
    }

    static Info describe(File base, String gameDir, SharedPreferences prefs, String folder) {
        Info info = new Info();
        String d = Native.packDescribe(new File(dir(base), folder).getAbsolutePath(), prefs.getString(selKey(editScope(prefs), folder), ""), gameDir);
        for (String line : d.split("\n")) {
            String[] f = line.split("\t", -1);
            if (f.length < 2) continue;
            switch (f[0]) {
                case "name": info.name = f[1]; break;
                case "path": info.path = f[1]; break;
                case "description": info.description = f[1].replace("|", "\n"); break;
                case "category": if (f.length >= 4) info.categories.add(Arrays.copyOfRange(f, 1, f.length)); break;
                case "patches":
                    info.hasPatches = true;
                    info.groups = Integer.parseInt(f[1]);
                    info.groupsOther = f.length > 2 ? Integer.parseInt(f[2]) : 0;
                    break;
                case "content":
                    info.contentFiles = Integer.parseInt(f[1]);
                    info.contentAdded = f.length > 2 ? Integer.parseInt(f[2]) : 0;
                    break;
                case "added": info.added.add(f[1]); break;
                case "shaders": info.shaders = Integer.parseInt(f[1]); break;
                case "texrules": info.textureRules = Integer.parseInt(f[1]); break;
                case "versions": info.versions = f[1]; break;
                case "note": info.notes.add(f[1]); break;
                case "unsupported": info.unsupported.add(f[1]); break;
                default: break;
            }
        }
        if (info.name.isEmpty()) info.name = folder;
        return info;
    }

    static void select(SharedPreferences prefs, String folder, String category, String preset) {
        String key = selKey(editScope(prefs), folder);
        Map<String, String> sel = new LinkedHashMap<>();
        for (String line : prefs.getString(key, "").split("\n")) {
            String[] f = line.split("\t", -1);
            if (f.length >= 2) sel.put(f[0], f[1]);
        }
        sel.put(category, preset);
        StringBuilder b = new StringBuilder();
        for (Map.Entry<String, String> e : sel.entrySet()) b.append(e.getKey()).append('\t').append(e.getValue()).append('\n');
        prefs.edit().putString(key, b.toString()).commit();
    }

    // ---- per save file (Mods > Mod packs > Per save file, pref packs_per_file): each save file 1..3 has its own packs
    // turned on, presets and loading order (pack_f<N>_on_<folder>, pack_f<N>_sel_<folder>, pack_f<N>_order); scope 0 is
    // the one list as before (pack_on_<folder>, pack_sel_<folder>, pack_order). The page edits save file
    // packs_edit_file; enabled.txt holds packs_active_file's list (the one the game starts with), enabled_<N>.txt each
    // file's: when the file select hands over a file whose list differs, the runtime holds the game and the app restarts
    // it with that file's (MainActivity.packFileSwitch). Player models have their own choice per file (PlayerModels).
    static final int FILES = 3;

    static boolean perFile(SharedPreferences prefs) { return prefs.getBoolean("packs_per_file", false); }

    /** the save file 1..3 the page edits (0: packs aren't per save file) */
    static int editScope(SharedPreferences prefs) {
        return perFile(prefs) ? Math.max(1, Math.min(FILES, prefs.getInt("packs_edit_file", 1))) : 0;
    }

    /** the save file 1..3 whose packs enabled.txt holds (0: packs aren't per save file) */
    static int activeScope(SharedPreferences prefs) {
        return perFile(prefs) ? Math.max(1, Math.min(FILES, prefs.getInt("packs_active_file", 1))) : 0;
    }

    private static String onKey(int scope, String folder) {
        return scope == 0 || isPlayerModel(folder) ? "pack_on_" + folder : "pack_f" + scope + "_on_" + folder;
    }

    private static String selKey(int scope, String folder) {
        return scope == 0 || isPlayerModel(folder) ? "pack_sel_" + folder : "pack_f" + scope + "_sel_" + folder;
    }

    private static String orderKey(int scope) { return scope == 0 ? "pack_order" : "pack_f" + scope + "_order"; }

    /** a pack is on (for the save file the page edits) */
    static boolean isOn(SharedPreferences prefs, String folder) { return prefs.getBoolean(onKey(editScope(prefs), folder), false); }

    /** a change on the page needs a restart: it is to the list the game started with */
    static boolean editsRunning(SharedPreferences prefs) { return editScope(prefs) == activeScope(prefs); }

    /**
     * Per save file on: each file starts with the one list as it is (nothing changes until one is edited); off: the one
     * list again (the files' own lists are kept for the next time).
     */
    static void setPerFile(File base, SharedPreferences prefs, boolean on) {
        SharedPreferences.Editor e = prefs.edit().putBoolean("packs_per_file", on);
        if (on && !prefs.getBoolean("packs_per_file_made", false)) {
            e.putBoolean("packs_per_file_made", true);
            for (int n = 1; n <= FILES; n++) {
                for (String f : list(base)) {
                    if (isPlayerModel(f)) continue;
                    e.putBoolean(onKey(n, f), prefs.getBoolean(onKey(0, f), false));
                    String sel = prefs.getString(selKey(0, f), null);
                    if (sel != null) e.putString(selKey(n, f), sel);
                }
                e.putString(orderKey(n), prefs.getString(orderKey(0), ""));
            }
        }
        e.commit();
        writeEnabled(base, prefs);
    }

    /** the game starts with save file `file`'s packs from now on (enabled.txt); false: they are the ones it has */
    static boolean makeActive(File base, SharedPreferences prefs, int file) {
        String before = enabledText(base, prefs, activeScope(prefs));
        prefs.edit().putInt("packs_active_file", file).commit();
        writeEnabled(base, prefs);
        return !before.equals(enabledText(base, prefs, file));
    }

    /**
     * The packs turned on (player models aside: they always go last), in their loading order: top first, a later
     * pack's files, shaders and code patches win over an earlier one's. Pref pack_order (folders, one a line); a pack
     * not in it yet goes to the end.
     */
    static List<String> order(File base, SharedPreferences prefs) { return order(base, prefs, editScope(prefs)); }

    private static List<String> order(File base, SharedPreferences prefs, int scope) {
        List<String> on = new ArrayList<>(), out = new ArrayList<>();
        for (String f : list(base)) if (!isPlayerModel(f) && prefs.getBoolean(onKey(scope, f), false)) on.add(f);
        for (String f : prefs.getString(orderKey(scope), "").split("\n"))
            if (on.remove(f)) out.add(f);
        out.addAll(on);
        return out;
    }

    static void setOrder(SharedPreferences prefs, List<String> folders) {
        prefs.edit().putString(orderKey(editScope(prefs)), String.join("\n", folders)).commit();
    }

    /** a pack turned on or off: on, it loads last (it wins over the ones on before) */
    static void setOn(File base, SharedPreferences prefs, String folder, boolean on) {
        List<String> o = order(base, prefs);
        o.remove(folder);
        if (on) o.add(folder);
        prefs.edit().putBoolean(onKey(editScope(prefs), folder), on).commit();
        setOrder(prefs, o);
        writeEnabled(base, prefs);
    }

    /** the game files below a pack's content/ folder (lower case, "Common/Pack/x.pack" as "common/pack/x.pack") */
    static java.util.Set<String> contentFiles(File base, String folder) {
        List<String> all = new ArrayList<>();
        File[] kids = new File(dir(base), folder).listFiles();
        if (kids != null)
            for (File k : kids)
                if (k.isDirectory() && k.getName().equalsIgnoreCase("content")) listFiles(k, "", all);
        java.util.Set<String> out = new java.util.HashSet<>();
        for (String f : all) out.add(f.toLowerCase(java.util.Locale.ROOT));
        return out;
    }

    /** a list as enabled.txt has it (scope 0: the one list, 1..3: that save file's) */
    private static String enabledText(File base, SharedPreferences prefs, int scope) {
        StringBuilder b = new StringBuilder("# written by the app: enabled mod packs (in their loading order: the last wins) and their presets\n");
        List<String> folders = order(base, prefs, scope);
        for (String f : list(base)) if (isPlayerModel(f) && prefs.getBoolean(onKey(0, f), false)) folders.add(f);
        for (String folder : folders) {
            b.append("pack\t").append(folder).append('\n');
            for (String line : prefs.getString(selKey(scope, folder), "").split("\n")) {
                String[] f = line.split("\t", -1);
                if (f.length >= 2) b.append("preset\t").append(folder).append('\t').append(f[0]).append('\t').append(f[1]).append('\n');
            }
        }
        return b.toString();
    }

    private static void write(File f, String text) {
        try (OutputStream out = new FileOutputStream(f)) {
            out.write(text.getBytes(StandardCharsets.UTF_8));
        } catch (IOException ignored) {
        }
    }

    /**
     * files/packs/enabled.txt from the prefs (read by the native side at start), in the loading order; per save file
     * also enabled_1..3.txt (the runtime compares the chosen file's with the running list), else none of them
     */
    static void writeEnabled(File base, SharedPreferences prefs) {
        File d = dir(base);
        //noinspection ResultOfMethodCallIgnored
        d.mkdirs();
        boolean per = perFile(prefs);
        write(new File(d, "enabled.txt"), enabledText(base, prefs, activeScope(prefs)));
        for (int n = 1; n <= FILES; n++) {
            File f = new File(d, "enabled_" + n + ".txt");
            if (per) write(f, enabledText(base, prefs, n));
            else //noinspection ResultOfMethodCallIgnored
                f.delete();
        }
    }

    /** the number of packs turned on (for the save file the page edits) */
    static int countOn(File base, SharedPreferences prefs) {
        int n = 0;
        for (String folder : list(base)) if (isOn(prefs, folder)) n++;
        return n;
    }

    /** turns every pack off in every list (their files stay), e.g. when the game no longer starts with them */
    static void turnAllOff(File base, SharedPreferences prefs) {
        SharedPreferences.Editor e = prefs.edit();
        for (String folder : list(base))
            for (int n = 0; n <= FILES; n++) e.remove(onKey(n, folder));
        e.commit();
        writeEnabled(base, prefs);
    }

    /**
     * Per save file: a save file deleted in the file select (it shows as a new file there) lets go of its packs, so a
     * new game there starts without the old game's (packs chosen for an empty file stay). Read every two seconds with
     * the achievements while the file select's list shows (Native.saveSlotsUsed); the files holding a game are kept
     * in "packs_slots_used" (bits), the first read only notes them.
     */
    static void slotsCheck(MainActivity a) {
        if (a.prefs == null || !perFile(a.prefs)) return;
        int used = Native.saveSlotsUsed();
        if (used < 0) return;
        int before = a.prefs.getInt("packs_slots_used", -1);
        if (before == used) return;
        SharedPreferences.Editor e = a.prefs.edit().putInt("packs_slots_used", used);
        boolean changed = false;
        if (before >= 0)
            for (int k = 0; k < FILES; k++) {
                if ((before & 1 << k) == 0 || (used & 1 << k) != 0) continue;
                int scope = k + 1;
                android.util.Log.i("wwhd", "save file " + scope + " deleted: its mod packs let go");
                for (String folder : list(a.baseDir()))
                    if (!isPlayerModel(folder)) e.remove(onKey(scope, folder)).remove(selKey(scope, folder));
                changed = true;
            }
        e.commit();
        if (changed) writeEnabled(a.baseDir(), a.prefs);
    }

    static void delete(File base, SharedPreferences prefs, String folder) {
        deleteTree(new File(dir(base), folder));
        SharedPreferences.Editor e = prefs.edit();
        for (int n = 0; n <= FILES; n++) e.remove(onKey(n, folder)).remove(selKey(n, folder));
        e.commit();
        writeEnabled(base, prefs);
    }

    static void deleteTree(File f) {
        File[] kids = f.listFiles();
        if (kids != null)
            for (File k : kids) deleteTree(k);
        //noinspection ResultOfMethodCallIgnored
        f.delete();
    }

    /**
     * Imports the mods in an archive (zip, RAR, 7z). Packs are found, in this order:
     * - every folder with a rules.txt (a Cemu graphic pack; the deepest one owns the files below it);
     * - else every folder named content (a plain game file mod; the folder above it is the pack);
     * - else each file is placed by the end of its path among the game's own files ("Pack/x.pack" ->
     *   content/Common/Pack/x.pack); files that match no game file (read-mes) are left out.
     * Packs without a rules.txt get one. Returns the imported folder names (an existing pack of the
     * same name is replaced).
     */
    /** what an import is at, for its progress line (on the importing thread) */
    interface Progress {
        void at(String text);
    }

    static List<String> importArchive(android.content.Context ctx, File base, String gameDir, ContentResolver cr, Uri uri, String name)
            throws IOException {
        return importArchive(ctx, base, gameDir, cr, uri, name, null);
    }

    static List<String> importArchive(android.content.Context ctx, File base, String gameDir, ContentResolver cr, Uri uri, String name,
                                      Progress progress) throws IOException {
        File packs = dir(base), stage = new File(packs, ".import");
        deleteTree(stage);
        //noinspection ResultOfMethodCallIgnored
        stage.mkdirs();
        try (android.os.ParcelFileDescriptor pfd = cr.openFileDescriptor(uri, "r")) {
            if (pfd == null) throw new IOException("cannot open " + name);
            String err = Native.extractModArchive(pfd.getFd(), stage.getAbsolutePath());
            if (err != null) throw new IOException(err);
        }
        String base0 = clean(name.replaceAll("(?i)\\.(zip|rar|7z)$", ""));
        List<String> out = new ArrayList<>();
        try {
            // GameCube player models (custom model packs for the GameCube randomizer: a Link.arc each, with
            // metadata.txt): converted into the game's own permanent_3d.pack (runtime/src/gcmodel)
            List<File> arcs = new ArrayList<>();
            List<String> failed = new ArrayList<>();
            findNamed(stage, "Link.arc", arcs);
            if (!arcs.isEmpty()) {
                File gamePack = new File(gameDir, "content/Common/Pack/permanent_3d.pack");
                if (!gamePack.isFile()) throw new IOException("the game's permanent_3d.pack is missing");
                int k = 0;
                for (File arc : arcs) {
                    File dir = arc.getParentFile();
                    String model = dir.equals(stage) ? base0 : dir.getName();
                    File root = new File(stage, ".gcmodel" + k++), szs = new File(root, MODEL_SZS);
                    //noinspection ResultOfMethodCallIgnored
                    szs.getParentFile().mkdirs();
                    final int nr = k;
                    if (progress != null) {
                        progress.at(ctx.getString(R.string.packs_gc_converting, nr, arcs.size(), model, 0));
                        Native.gcProgressListener = pc -> progress.at(ctx.getString(R.string.packs_gc_converting, nr, arcs.size(), model, pc));
                    }
                    String report;
                    try {
                        report = Native.convertGcModel(arc.getAbsolutePath(), gamePack.getAbsolutePath(), szs.getAbsolutePath());
                    } finally {
                        Native.gcProgressListener = null;
                    }
                    if (report == null || report.startsWith("!")) {
                        String why = model + ": " + (report == null ? "failed" : report.substring(1));
                        if (arcs.size() == 1) throw new IOException(why);
                        failed.add(why);  // (one model of many: the others still come in)
                        continue;
                    }
                    try (OutputStream o = new FileOutputStream(new File(root, "conversion.txt"))) {
                        o.write(report.getBytes(StandardCharsets.UTF_8));
                    }
                    String author = metadata(new File(dir, "metadata.txt"), "author");
                    String title = ctx.getString(R.string.packs_gc_model_name, model);
                    String desc = author != null ? ctx.getString(R.string.packs_gc_model_desc_by, author) : ctx.getString(R.string.packs_gc_model_desc);
                    String folder = clean(PLAYER_MODEL + model);
                    if (new File(packs, folder).isDirectory() && isPlayerModel(folder)) {
                        // (a model imported again: replaced; PlayerModels.handOver after the import makes the runtime read it again)
                        deleteTree(new File(packs, folder));
                    }
                    out.add(install(packs, root, folder,
                            "[Definition]\ntitleIds = 0005000010143400,0005000010143500,0005000010143600\nname = \"" + title.replace("\"", "'")
                                    + "\"\ndescription = " + desc.replace("\n", " ") + "\nversion = 6\n"));
                }
                if (!failed.isEmpty()) lastFailures = failed;
                return out;
            }
            List<File> rules = new ArrayList<>(), contents = new ArrayList<>();
            find(stage, rules, contents);
            if (!rules.isEmpty()) {
                // deepest first: a pack inside another pack's folder keeps its own files
                rules.sort((x, y) -> y.getAbsolutePath().length() - x.getAbsolutePath().length());
                for (File r : rules) out.add(install(packs, r, r == stage ? base0 : clean(r.getName()), null));
            } else if (!contents.isEmpty()) {
                for (File c : contents) {
                    File root = c.getParentFile();
                    String n = root.equals(stage) ? base0 : clean(root.getName());
                    out.add(install(packs, root, n, rulesText(n, name)));
                }
            } else {
                File root = new File(stage, ".matched");
                int placed = placeByGameFiles(stage, new File(gameDir, "content"), new File(root, "content"));
                if (placed == 0) throw new IOException("no rules.txt, no content folder and no file of the game in " + name);
                out.add(install(packs, root, base0, rulesText(base0, name)));
            }
        } finally {
            deleteTree(stage);
        }
        return out;
    }

    static final String PLAYER_MODEL = "gcmodel-";
    // a player model pack keeps only its converted Link.szs (PlayerModels: put into the game's memory per save file);
    // MODEL_PACK is where packs from before had the game's permanent_3d.pack with it (choosePlayerModel takes them out)
    static final String MODEL_SZS = "model/Link.szs", MODEL_PACK = "content/Common/Pack/permanent_3d.pack";
    /** the models of the last import that could not be converted (null if none); taken by the caller */
    static volatile List<String> lastFailures;

    /** the player model packs, sorted by their names */
    static List<String> playerModels(File base) {
        List<String> out = new ArrayList<>();
        for (String f : list(base)) if (isPlayerModel(f)) out.add(f);
        out.sort((x, y) -> playerModelName(base, x).compareToIgnoreCase(playerModelName(base, y)));
        return out;
    }

    /** a player model's name (the model's folder in its pack: "Cloud") */
    static String playerModelName(File base, String folder) {
        return folder.startsWith(PLAYER_MODEL) ? folder.substring(PLAYER_MODEL.length()) : folder;
    }

    /** the permanent_3d.pack a game plays: a randomized game's own (its changes, Link's colours among them), else the game's */
    static File basePack(String gameDir, String randoOut) {
        if (randoOut != null) {
            File r = new File(randoOut, "content/Common/Pack/permanent_3d.pack");
            if (r.isFile()) return r;
        }
        return new File(gameDir, "content/Common/Pack/permanent_3d.pack");
    }

    private static String baseStamp(File base) { return base.getAbsolutePath() + "\t" + base.length() + "\t" + base.lastModified(); }

    /** player model packs from before keep a full permanent_3d.pack each (25 MB): choosePlayerModel trims them */
    static boolean needsTrim(File base) {
        for (String f : list(base))
            if (isPlayerModel(f) && !new File(new File(dir(base), f), MODEL_SZS).isFile()) return true;
        return false;
    }

    /** the player model switched on, or null */
    static String playerModelOn(File base, SharedPreferences prefs) {
        for (String f : list(base)) if (isPlayerModel(f) && prefs.getBoolean("pack_on_" + f, false)) return f;
        return null;
    }

    /**
     * Switches player model `folder` on (null: none): its permanent_3d.pack made from the game's and its Link.szs, the
     * others' removed; packs from before keeping only Link.szs are trimmed on the way. Null when done, else why not.
     * Not on the main thread (seconds).
     */
    static String choosePlayerModel(File base, String gameDir, SharedPreferences prefs, String folder, Progress progress) {
        File packs = dir(base), gamePack = new File(gameDir, "content/Common/Pack/permanent_3d.pack");
        List<String> models = new ArrayList<>();
        for (String f : list(base)) if (isPlayerModel(f)) models.add(f);
        int i = 0;
        String err = null;
        for (String f : models) {
            File d = new File(packs, f), szs = new File(d, MODEL_SZS), pack = new File(d, MODEL_PACK);
            if (!szs.isFile() && pack.isFile()) {  // (made before: Link.szs out of its full pack)
                if (progress != null) progress.at(f + " (" + (++i) + "/" + models.size() + ")");
                //noinspection ResultOfMethodCallIgnored
                szs.getParentFile().mkdirs();
                if (!Native.extractGcModel(pack.getAbsolutePath(), szs.getAbsolutePath())) continue;
            }
            if (!f.equals(folder)) {
                deleteTree(new File(d, "content"));
                prefs.edit().putBoolean("pack_on_" + f, false).apply();
            }
        }
        if (folder != null) {
            File d = new File(packs, folder), pack = new File(d, MODEL_PACK);
            //noinspection ResultOfMethodCallIgnored
            pack.getParentFile().mkdirs();
            if (progress != null) progress.at(playerModelName(base, folder));
            err = Native.assembleGcModel(gamePack.getAbsolutePath(), new File(d, MODEL_SZS).getAbsolutePath(), pack.getAbsolutePath());
            if (err == null)
                try {
                    java.nio.file.Files.write(new File(d, "content/.base").toPath(), baseStamp(gamePack).getBytes(StandardCharsets.UTF_8));
                } catch (IOException ignored) {
                }
            prefs.edit().putBoolean("pack_on_" + folder, err == null).apply();
        }
        prefs.edit().commit();
        writeEnabled(base, prefs);
        return err;
    }

    /** a pack made from a GameCube player model (they replace the same file: one at a time) */
    static boolean isPlayerModel(String folder) { return folder.startsWith(PLAYER_MODEL); }

    private static void findNamed(File d, String name, List<File> out) {
        File[] kids = d.listFiles();
        if (kids == null) return;
        for (File k : kids) {
            if (k.isDirectory()) findNamed(k, name, out);
            else if (k.getName().equalsIgnoreCase(name)) out.add(k);
        }
    }

    /** a "key: value" line of a model pack's metadata.txt, or null */
    private static String metadata(File f, String key) {
        if (!f.isFile()) return null;
        try {
            for (String line : new String(java.nio.file.Files.readAllBytes(f.toPath()), StandardCharsets.UTF_8).split("\n")) {
                int c = line.indexOf(':');
                if (c > 0 && line.substring(0, c).trim().equalsIgnoreCase(key)) {
                    String v = line.substring(c + 1).trim();
                    return v.isEmpty() ? null : v;
                }
            }
        } catch (IOException ignored) {
        }
        return null;
    }

    private static String clean(String n) {
        n = n.replaceAll("[^A-Za-z0-9._ -]", "_").trim();
        return n.isEmpty() || n.startsWith(".") ? "pack" + n : n;
    }

    private static String rulesText(String packName, String archiveName) {
        return "[Definition]\ntitleIds = 0005000010143400,0005000010143500,0005000010143600\nname = \"" + packName.replace("\"", "'")
                + "\"\ndescription = Game files from " + archiveName.replace("\n", " ") + "\nversion = 6\n";
    }

    private static void find(File d, List<File> rules, List<File> contents) {
        File[] kids = d.listFiles();
        if (kids == null) return;
        boolean hasRules = false;
        for (File k : kids)
            if (k.isFile() && k.getName().equalsIgnoreCase("rules.txt")) hasRules = true;
        if (hasRules) rules.add(d);
        for (File k : kids) {
            if (!k.isDirectory()) continue;
            if (!hasRules && k.getName().equalsIgnoreCase("content")) contents.add(k);
            else find(k, rules, contents);
        }
    }

    /** moves `root` (its files not owned by a deeper pack: already moved) to packs/<name>; a rules.txt is written if given */
    private static String install(File packs, File root, String name, String rules) throws IOException {
        File dest = new File(packs, name);
        deleteTree(dest);
        if (!root.renameTo(dest)) throw new IOException("cannot move " + root + " to " + dest);
        if (rules != null)
            try (OutputStream o = new FileOutputStream(new File(dest, "rules.txt"))) {
                o.write(rules.getBytes(StandardCharsets.UTF_8));
            }
        return name;
    }

    // ---- placing loose files by the game's own files
    private static void listFiles(File d, String rel, List<String> out) {
        File[] kids = d.listFiles();
        if (kids == null) return;
        for (File k : kids) {
            String r = rel.isEmpty() ? k.getName() : rel + "/" + k.getName();
            if (k.isDirectory()) listFiles(k, r, out);
            else out.add(r);
        }
    }

    /** moves each file below `from` whose path ends like exactly one game file to that place below `to`; how many */
    private static int placeByGameFiles(File from, File gameContent, File to) {
        List<String> game = new ArrayList<>(), files = new ArrayList<>();
        listFiles(gameContent, "", game);
        listFiles(from, "", files);
        Map<String, List<String>> byName = new java.util.HashMap<>();  // lower-case file name -> game paths
        for (String g : game) {
            String n = g.substring(g.lastIndexOf('/') + 1).toLowerCase(java.util.Locale.ROOT);
            byName.computeIfAbsent(n, k -> new ArrayList<>()).add(g);
        }
        int placed = 0;
        for (String f : files) {
            String lf = f.toLowerCase(java.util.Locale.ROOT);
            List<String> same = byName.get(lf.substring(lf.lastIndexOf('/') + 1));
            if (same == null) continue;
            // the longest end of the path that game files share, then only one of them
            String match = null;
            for (String suffix = lf; match == null; ) {
                List<String> hits = new ArrayList<>();
                for (String g : same) {
                    String lg = g.toLowerCase(java.util.Locale.ROOT);
                    if (lg.equals(suffix) || lg.endsWith("/" + suffix)) hits.add(g);
                }
                if (hits.size() == 1) match = hits.get(0);
                else if (hits.size() > 1) break;  // ambiguous at this length: a longer end didn't match, so leave it
                int cut = suffix.indexOf('/');
                if (match == null && cut < 0) break;
                if (match == null) suffix = suffix.substring(cut + 1);
            }
            if (match == null) continue;
            File dest = new File(to, match);
            //noinspection ResultOfMethodCallIgnored
            dest.getParentFile().mkdirs();
            if (new File(from, f).renameTo(dest)) placed++;
        }
        return placed;
    }
}
