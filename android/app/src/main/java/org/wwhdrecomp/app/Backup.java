package org.wwhdrecomp.app;

import android.content.ContentResolver;
import android.net.Uri;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.text.SimpleDateFormat;
import java.util.ArrayList;
import java.util.Date;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.regex.Pattern;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;
import java.util.zip.ZipOutputStream;

/**
 * Export of the game save (files/save: user/ and common/) and the save states (files/states/slotN.bin
 * + .png) into one zip file, and import from that zip or from the files themselves (Storage Access
 * Framework: files only, see export).
 * Runs on a background thread; callers show progress.
 */
final class Backup {
    private Backup() {}

    static final Pattern STATE_FILE = Pattern.compile("slot[1-5]\\.(bin|png)");

    // ------------------------------------------------------------------ export
    /**
     * Writes one zip file (the picked document): save/user/..., save/common/... and states/slotN.bin /
     * .png. One file, not a folder: some devices' folder picker can't choose or make folders (Android
     * 16 with AOSP DocumentsUI), while "save as" works there. The states are stored as they are (already
     * compressed), the save files packed.
     */
    static void export(ContentResolver cr, Uri file, File base, boolean save, boolean states) throws IOException {
        try (OutputStream raw = cr.openOutputStream(file, "wt")) {
            if (raw == null) throw new IOException("cannot write " + file);
            ZipOutputStream zip = new ZipOutputStream(raw);
            if (save) {
                zip.setLevel(java.util.zip.Deflater.DEFAULT_COMPRESSION);
                File s = new File(base, "save");
                if (s.isDirectory()) zipTree(zip, s, "save");
            }
            if (states) {
                zip.setLevel(java.util.zip.Deflater.NO_COMPRESSION);
                File[] files = new File(base, "states").listFiles((d, n) -> STATE_FILE.matcher(n).matches());
                if (files != null)
                    for (File f : files) zipFile(zip, f, "states/" + f.getName());
            }
            zip.finish();
        }
    }

    static String exportName() {
        return "WindWakerHD backup " + new SimpleDateFormat("yyyy-MM-dd HH.mm", Locale.ROOT).format(new Date()) + ".zip";
    }

    private static void zipTree(ZipOutputStream zip, File dir, String path) throws IOException {
        File[] list = dir.listFiles();
        if (list == null) return;
        for (File f : list) {
            if (f.isDirectory()) zipTree(zip, f, path + "/" + f.getName());
            else zipFile(zip, f, path + "/" + f.getName());
        }
    }

    private static void zipFile(ZipOutputStream zip, File f, String name) throws IOException {
        zip.putNextEntry(new ZipEntry(name));
        try (InputStream in = new FileInputStream(f)) {
            copy(in, zip);
        }
        zip.closeEntry();
    }

    // ------------------------------------------------------------------ import
    /** What a picked folder holds. */
    static final class Found {
        Map<String, Uri> saveUser = new LinkedHashMap<>();    // game save: file name -> document
        Map<String, Uri> saveCommon = new LinkedHashMap<>();
        Map<String, Uri> states = new LinkedHashMap<>();      // slotN.bin / slotN.png -> document
        String saveFrom;                                      // where the game save was found (for the dialog)
        Uri zip;                                              // a backup zip: the maps name its entries

        boolean empty() { return saveUser.isEmpty() && states.isEmpty(); }

        List<Integer> slots() {
            List<Integer> v = new ArrayList<>();
            for (int i = 1; i <= 5; i++)
                if (states.containsKey("slot" + i + ".bin")) v.add(i);
            return v;
        }
    }

    /**
     * What the picked files hold: the cking*.sav files are the game save (this app's save/user, or
     * Cemu's .../user/80000001), slot1..5.bin and .png the save states. Files, not their folder: some
     * devices' folder picker lists nothing on the internal storage (Android 16, AOSP DocumentsUI).
     */
    static Found fromFiles(List<String> names, List<Uri> uris, String from) {
        Found f = new Found();
        for (int i = 0; i < names.size(); i++) {
            String n = names.get(i);
            if (n.startsWith("cking") && n.endsWith(".sav")) f.saveUser.put(n, uris.get(i));
            else if (STATE_FILE.matcher(n).matches()) f.states.put(n, uris.get(i));
        }
        if (!f.saveUser.containsKey("cking.sav")) f.saveUser.clear();  // the pictures alone are no game save
        f.saveFrom = from;
        return f;
    }

    /** What a backup zip (export) holds: its entries' names, read once through. */
    static Found scanZip(ContentResolver cr, Uri zipUri, String from) throws IOException {
        Found f = new Found();
        f.zip = zipUri;
        f.saveFrom = from;
        try (InputStream in = cr.openInputStream(zipUri)) {
            if (in == null) throw new IOException("cannot read " + zipUri);
            ZipInputStream zip = new ZipInputStream(in);
            for (ZipEntry e; (e = zip.getNextEntry()) != null; ) {
                String n = e.getName();
                if (e.isDirectory()) continue;
                if (n.startsWith("save/user/") && n.indexOf('/', 10) < 0) f.saveUser.put(n.substring(10), Uri.EMPTY);
                else if (n.startsWith("save/common/") && n.indexOf('/', 12) < 0) f.saveCommon.put(n.substring(12), Uri.EMPTY);
                else if (n.startsWith("states/") && STATE_FILE.matcher(n.substring(7)).matches()) f.states.put(n.substring(7), Uri.EMPTY);
            }
        }
        if (!f.saveUser.containsKey("cking.sav")) {
            f.saveUser.clear();
            f.saveCommon.clear();
        }
        return f;
    }

    /** Imports from a backup zip in one pass: the game save (if `save`) and the states found (if `states`). */
    static void importZip(ContentResolver cr, Found f, File base, boolean save, boolean states) throws IOException {
        File incoming = new File(base, "save-importing"), dir = new File(base, "states");
        deleteTree(incoming);
        File user = new File(incoming, "user"), common = new File(incoming, "common");
        if (save && !user.mkdirs()) throw new IOException("cannot create " + user);
        //noinspection ResultOfMethodCallIgnored
        dir.mkdirs();
        List<String> stateFiles = new ArrayList<>();
        try (InputStream in = cr.openInputStream(f.zip)) {
            if (in == null) throw new IOException("cannot read " + f.zip);
            ZipInputStream zip = new ZipInputStream(in);
            for (ZipEntry e; (e = zip.getNextEntry()) != null; ) {
                String n = e.getName();
                File dst = null;
                if (save && n.startsWith("save/user/") && f.saveUser.containsKey(n.substring(10))) {
                    dst = new File(user, n.substring(10));
                } else if (save && n.startsWith("save/common/") && f.saveCommon.containsKey(n.substring(12))) {
                    //noinspection ResultOfMethodCallIgnored
                    common.mkdirs();
                    dst = new File(common, n.substring(12));
                } else if (states && n.startsWith("states/") && f.states.containsKey(n.substring(7))) {
                    stateFiles.add(n.substring(7));
                    dst = new File(dir, n.substring(7) + ".importing");
                }
                if (dst == null) continue;
                try (OutputStream out = new FileOutputStream(dst)) {
                    copy(zip, out);
                }
            }
        }
        if (states) {
            for (int slot : f.slots()) {  // a slot is its state and its picture: an old picture goes
                //noinspection ResultOfMethodCallIgnored
                new File(dir, "slot" + slot + ".png").delete();
            }
            for (String n : stateFiles) {
                File dst = new File(dir, n);
                //noinspection ResultOfMethodCallIgnored
                dst.delete();
                if (!new File(dir, n + ".importing").renameTo(dst)) throw new IOException("cannot write " + dst);
            }
        }
        if (save) putSaveInPlace(base, incoming);
    }

    /** Replaces the game save; the current one is kept in save-before-import. */
    static void importSave(ContentResolver cr, Found f, File base) throws IOException {
        File incoming = new File(base, "save-importing");
        deleteTree(incoming);
        File user = new File(incoming, "user"), common = new File(incoming, "common");
        if (!user.mkdirs()) throw new IOException("cannot create " + user);
        for (Map.Entry<String, Uri> e : f.saveUser.entrySet()) copyIn(cr, e.getValue(), new File(user, e.getKey()));
        if (!f.saveCommon.isEmpty()) {
            if (!common.mkdirs()) throw new IOException("cannot create " + common);
            for (Map.Entry<String, Uri> e : f.saveCommon.entrySet()) copyIn(cr, e.getValue(), new File(common, e.getKey()));
        }
        putSaveInPlace(base, incoming);
    }

    // everything copied: swap the folders
    private static void putSaveInPlace(File base, File incoming) throws IOException {
        File save = new File(base, "save"), keep = new File(base, "save-before-import");
        deleteTree(keep);
        if (save.exists() && !save.renameTo(keep)) throw new IOException("cannot move the current save aside");
        if (!incoming.renameTo(save)) {
            //noinspection ResultOfMethodCallIgnored
            keep.renameTo(save);
            throw new IOException("cannot put the imported save in place");
        }
    }

    /** Copies the found save states over the slots with the same numbers. */
    static void importStates(ContentResolver cr, Found f, File base) throws IOException {
        File dir = new File(base, "states");
        //noinspection ResultOfMethodCallIgnored
        dir.mkdirs();
        for (int slot : f.slots()) {
            // a slot is its state and its picture: drop an old picture the import doesn't replace
            //noinspection ResultOfMethodCallIgnored
            new File(dir, "slot" + slot + ".png").delete();
            for (String ext : new String[] {"bin", "png"}) {
                Uri u = f.states.get("slot" + slot + "." + ext);
                if (u == null) continue;
                File tmp = new File(dir, "slot" + slot + "." + ext + ".importing");
                copyIn(cr, u, tmp);
                File dst = new File(dir, "slot" + slot + "." + ext);
                //noinspection ResultOfMethodCallIgnored
                dst.delete();
                if (!tmp.renameTo(dst)) throw new IOException("cannot write " + dst);
            }
        }
    }

    private static void copyIn(ContentResolver cr, Uri src, File dst) throws IOException {
        try (InputStream in = cr.openInputStream(src); OutputStream out = new FileOutputStream(dst)) {
            if (in == null) throw new IOException("cannot read " + src);
            copy(in, out);
        }
    }

    private static void copy(InputStream in, OutputStream out) throws IOException {
        byte[] buf = new byte[1 << 20];
        for (int n; (n = in.read(buf)) > 0; ) out.write(buf, 0, n);
    }

    static void deleteTree(File f) {
        File[] list = f.listFiles();
        if (list != null)
            for (File c : list) deleteTree(c);
        //noinspection ResultOfMethodCallIgnored
        f.delete();
    }
}
