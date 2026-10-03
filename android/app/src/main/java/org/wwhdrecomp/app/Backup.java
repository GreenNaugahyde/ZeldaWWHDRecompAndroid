package org.wwhdrecomp.app;

import android.content.ContentResolver;
import android.database.Cursor;
import android.net.Uri;
import android.provider.DocumentsContract;
import android.provider.DocumentsContract.Document;

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

/**
 * Export and import of the game save (files/save: user/ and common/) and the save states
 * (files/states/slotN.bin + .png) to and from a folder the user picked (Storage Access Framework).
 * Runs on a background thread; callers show progress.
 */
final class Backup {
    private Backup() {}

    static final Pattern STATE_FILE = Pattern.compile("slot[1-5]\\.(bin|png)");

    // ------------------------------------------------------------------ export
    /** Writes "WindWakerHD backup <date>" into the picked folder; returns the folder's name. */
    static String export(ContentResolver cr, Uri tree, File base, boolean save, boolean states) throws IOException {
        Uri root = DocumentsContract.buildDocumentUriUsingTree(tree, DocumentsContract.getTreeDocumentId(tree));
        String name = "WindWakerHD backup " + new SimpleDateFormat("yyyy-MM-dd HH.mm", Locale.ROOT).format(new Date());
        Uri dir = mkdir(cr, root, name);
        if (save) {
            File s = new File(base, "save");
            if (s.isDirectory()) copyTreeOut(cr, s, mkdir(cr, dir, "save"));
        }
        if (states) {
            File[] files = new File(base, "states").listFiles((d, n) -> STATE_FILE.matcher(n).matches());
            if (files != null && files.length > 0) {
                Uri sd = mkdir(cr, dir, "states");
                for (File f : files) copyOut(cr, f, sd);
            }
        }
        return name;
    }

    private static Uri mkdir(ContentResolver cr, Uri parent, String name) throws IOException {
        Uri d = DocumentsContract.createDocument(cr, parent, Document.MIME_TYPE_DIR, name);
        if (d == null) throw new IOException("cannot create folder " + name);
        return d;
    }

    private static void copyTreeOut(ContentResolver cr, File src, Uri dst) throws IOException {
        File[] list = src.listFiles();
        if (list == null) return;
        for (File f : list) {
            if (f.isDirectory()) copyTreeOut(cr, f, mkdir(cr, dst, f.getName()));
            else copyOut(cr, f, dst);
        }
    }

    private static void copyOut(ContentResolver cr, File f, Uri dir) throws IOException {
        Uri doc = DocumentsContract.createDocument(cr, dir, "application/octet-stream", f.getName());
        if (doc == null) throw new IOException("cannot create " + f.getName());
        try (InputStream in = new FileInputStream(f); OutputStream out = cr.openOutputStream(doc)) {
            if (out == null) throw new IOException("cannot write " + f.getName());
            copy(in, out);
        }
    }

    // ------------------------------------------------------------------ import
    /** What a picked folder holds. */
    static final class Found {
        Map<String, Uri> saveUser = new LinkedHashMap<>();    // game save: file name -> document
        Map<String, Uri> saveCommon = new LinkedHashMap<>();
        Map<String, Uri> states = new LinkedHashMap<>();      // slotN.bin / slotN.png -> document
        String saveFrom;                                      // where the game save was found (for the dialog)

        boolean empty() { return saveUser.isEmpty() && states.isEmpty(); }

        List<Integer> slots() {
            List<Integer> v = new ArrayList<>();
            for (int i = 1; i <= 5; i++)
                if (states.containsKey("slot" + i + ".bin")) v.add(i);
            return v;
        }
    }

    private static final class Entry {
        String id, name;
        boolean dir;
    }

    private static List<Entry> children(ContentResolver cr, Uri tree, String docId) {
        List<Entry> v = new ArrayList<>();
        Uri q = DocumentsContract.buildChildDocumentsUriUsingTree(tree, docId);
        try (Cursor c = cr.query(q, new String[] {Document.COLUMN_DOCUMENT_ID, Document.COLUMN_DISPLAY_NAME,
                Document.COLUMN_MIME_TYPE}, null, null, null)) {
            while (c != null && c.moveToNext()) {
                Entry e = new Entry();
                e.id = c.getString(0);
                e.name = c.getString(1);
                e.dir = Document.MIME_TYPE_DIR.equals(c.getString(2));
                v.add(e);
            }
        }
        return v;
    }

    /**
     * Looks through the picked folder (a few levels deep): the game save is the first folder that
     * holds cking.sav (this app's save/user, or Cemu's .../user/80000001), with a "common" folder
     * next to it if there is one; save states are slot1..5.bin and their .png pictures.
     */
    static Found scan(ContentResolver cr, Uri tree) {
        Found f = new Found();
        scan(cr, tree, DocumentsContract.getTreeDocumentId(tree), "", 0, f);
        return f;
    }

    private static void scan(ContentResolver cr, Uri tree, String docId, String path, int depth, Found f) {
        List<Entry> kids = children(cr, tree, docId);
        boolean hasSave = false;
        for (Entry e : kids)
            if (!e.dir && e.name.equals("cking.sav")) hasSave = true;
        if (hasSave && f.saveUser.isEmpty()) {
            for (Entry e : kids)
                if (!e.dir) f.saveUser.put(e.name, DocumentsContract.buildDocumentUriUsingTree(tree, e.id));
            f.saveFrom = path.isEmpty() ? "." : path;
        }
        for (Entry e : kids)
            if (!e.dir && STATE_FILE.matcher(e.name).matches() && !f.states.containsKey(e.name))
                f.states.put(e.name, DocumentsContract.buildDocumentUriUsingTree(tree, e.id));
        if (depth >= 8) return;
        for (Entry e : kids) {
            if (!e.dir) continue;
            String sub = path.isEmpty() ? e.name : path + "/" + e.name;
            boolean before = f.saveUser.isEmpty();
            scan(cr, tree, e.id, sub, depth + 1, f);
            // the game save was in that folder: take a "common" folder next to it (this app:
            // save/user + save/common; Cemu: user/80000001 + user/common)
            if (before && sub.equals(f.saveFrom))
                for (Entry c : kids)
                    if (c.dir && c.name.equals("common"))
                        for (Entry x : children(cr, tree, c.id))
                            if (!x.dir) f.saveCommon.put(x.name, DocumentsContract.buildDocumentUriUsingTree(tree, x.id));
        }
    }

    /** Replaces the game save; the current one is kept in save-before-import. */
    static void importSave(ContentResolver cr, Found f, File base) throws IOException {
        File save = new File(base, "save"), keep = new File(base, "save-before-import");
        File incoming = new File(base, "save-importing");
        deleteTree(incoming);
        File user = new File(incoming, "user"), common = new File(incoming, "common");
        if (!user.mkdirs()) throw new IOException("cannot create " + user);
        for (Map.Entry<String, Uri> e : f.saveUser.entrySet()) copyIn(cr, e.getValue(), new File(user, e.getKey()));
        if (!f.saveCommon.isEmpty()) {
            if (!common.mkdirs()) throw new IOException("cannot create " + common);
            for (Map.Entry<String, Uri> e : f.saveCommon.entrySet()) copyIn(cr, e.getValue(), new File(common, e.getKey()));
        }
        // everything copied: swap the folders
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
