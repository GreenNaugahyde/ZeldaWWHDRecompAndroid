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
        String d = Native.packDescribe(new File(dir(base), folder).getAbsolutePath(), prefs.getString("pack_sel_" + folder, ""), gameDir);
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
        Map<String, String> sel = new LinkedHashMap<>();
        for (String line : prefs.getString("pack_sel_" + folder, "").split("\n")) {
            String[] f = line.split("\t", -1);
            if (f.length >= 2) sel.put(f[0], f[1]);
        }
        sel.put(category, preset);
        StringBuilder b = new StringBuilder();
        for (Map.Entry<String, String> e : sel.entrySet()) b.append(e.getKey()).append('\t').append(e.getValue()).append('\n');
        prefs.edit().putString("pack_sel_" + folder, b.toString()).commit();
    }

    /** files/packs/enabled.txt from the prefs (read by the native side at start) */
    static void writeEnabled(File base, SharedPreferences prefs) {
        StringBuilder b = new StringBuilder("# written by the app: enabled mod packs and their presets\n");
        for (String folder : list(base)) {
            if (!prefs.getBoolean("pack_on_" + folder, false)) continue;
            b.append("pack\t").append(folder).append('\n');
            for (String line : prefs.getString("pack_sel_" + folder, "").split("\n")) {
                String[] f = line.split("\t", -1);
                if (f.length >= 2) b.append("preset\t").append(folder).append('\t').append(f[0]).append('\t').append(f[1]).append('\n');
            }
        }
        File d = dir(base);
        //noinspection ResultOfMethodCallIgnored
        d.mkdirs();
        try (OutputStream out = new FileOutputStream(new File(d, "enabled.txt"))) {
            out.write(b.toString().getBytes(StandardCharsets.UTF_8));
        } catch (IOException ignored) {
        }
    }

    /** the number of packs turned on */
    static int countOn(File base, SharedPreferences prefs) {
        int n = 0;
        for (String folder : list(base)) if (prefs.getBoolean("pack_on_" + folder, false)) n++;
        return n;
    }

    /** turns every pack off (their files stay), e.g. when the game no longer starts with them */
    static void turnAllOff(File base, SharedPreferences prefs) {
        SharedPreferences.Editor e = prefs.edit();
        for (String folder : list(base)) e.remove("pack_on_" + folder);
        e.commit();
        writeEnabled(base, prefs);
    }

    static void delete(File base, SharedPreferences prefs, String folder) {
        deleteTree(new File(dir(base), folder));
        prefs.edit().remove("pack_on_" + folder).remove("pack_sel_" + folder).commit();
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
     * Imports the mods in an archive (zip, RAR). Packs are found, in this order:
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
