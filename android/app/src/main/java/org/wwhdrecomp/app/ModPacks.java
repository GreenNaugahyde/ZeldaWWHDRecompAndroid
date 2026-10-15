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

    private static void deleteTree(File f) {
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
    static List<String> importArchive(File base, String gameDir, ContentResolver cr, Uri uri, String name) throws IOException {
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
