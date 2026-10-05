package org.wwhdrecomp.app;

import android.content.Context;
import android.net.Uri;

import org.json.JSONException;
import org.json.JSONObject;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.List;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;
import java.util.zip.ZipInputStream;

/**
 * GPU drivers the user installed, for Adreno GPUs: driver packages in the AdrenoTools format (a zip
 * with meta.json and the driver's .so files, as other emulators use them), which libadrenotools
 * loads in place of the system driver (runtime/src/vk/vk_device.cpp, load_vulkan). Each package is
 * unpacked into files/gpu_drivers/<id>/; libadrenotools' two hook libraries are copied from the APK
 * into files/gpu_drivers/.hooks/ (the APK's native libraries aren't extracted on install).
 *
 * Every start with an installed driver is checked: MainActivity creates <id>/.probe before it, and
 * the renderer deletes it after 120 frames with that driver. A .probe left from the last start
 * means the driver crashed, hung or couldn't be loaded there, and the system driver is used again.
 */
final class GpuDrivers {
    private GpuDrivers() {}

    static final class Driver {
        String id, name, version, description, library;
        File dir;
    }

    /** Custom drivers only work on Adreno GPUs (the kernel's kgsl device). */
    static boolean supported() { return new File("/dev/kgsl-3d0").exists(); }

    static File root(Context c) { return new File(c.getFilesDir(), "gpu_drivers"); }

    static File hookDir(Context c) { return new File(root(c), ".hooks"); }

    static List<Driver> list(Context c) {
        List<Driver> out = new ArrayList<>();
        File[] dirs = root(c).listFiles(f -> f.isDirectory() && !f.getName().startsWith("."));
        if (dirs == null) return out;
        java.util.Arrays.sort(dirs);
        for (File d : dirs) {
            Driver drv = read(d);
            if (drv != null) out.add(drv);
        }
        return out;
    }

    static Driver find(Context c, String id) {
        if (id == null || id.isEmpty()) return null;
        return read(new File(root(c), id));
    }

    private static Driver read(File dir) {
        try {
            JSONObject m = new JSONObject(new String(Files.readAllBytes(new File(dir, "meta.json").toPath()), StandardCharsets.UTF_8));
            Driver d = new Driver();
            d.id = dir.getName();
            d.dir = dir;
            d.library = m.getString("libraryName");
            d.name = m.optString("name", d.library);
            d.version = m.optString("driverVersion", m.optString("packageVersion", ""));
            d.description = m.optString("description", "");
            return new File(dir, d.library).exists() ? d : null;
        } catch (IOException | JSONException e) {
            return null;
        }
    }

    /** Unpacks a driver package; returns its id. Throws with a message for the user if it isn't one. */
    static String install(Context c, Uri uri) throws IOException {
        File tmp = new File(root(c), ".install");
        Backup.deleteTree(tmp);
        if (!tmp.mkdirs()) throw new IOException("cannot create " + tmp);
        long total = 0;
        try (InputStream in = c.getContentResolver().openInputStream(uri)) {
            if (in == null) throw new IOException("no data");
            ZipInputStream zip = new ZipInputStream(in);
            byte[] buf = new byte[1 << 16];
            for (ZipEntry e; (e = zip.getNextEntry()) != null; ) {
                // only meta.json and libraries at the top level; nothing else is needed
                String n = e.getName();
                if (e.isDirectory() || n.contains("/") || n.contains("\\") || !(n.equals("meta.json") || n.endsWith(".so"))) continue;
                try (OutputStream out = new FileOutputStream(new File(tmp, n))) {
                    for (int k; (k = zip.read(buf)) > 0; ) {
                        total += k;
                        if (total > (512L << 20)) throw new IOException("too large");
                        out.write(buf, 0, k);
                    }
                }
            }
        } catch (IOException e) {
            Backup.deleteTree(tmp);
            throw e;
        }
        Driver d = read(tmp);
        if (d == null) {
            Backup.deleteTree(tmp);
            throw new IOException(c.getString(R.string.gpu_driver_not_package));
        }
        // the id: the package's name, made file-safe (installing the same package again replaces it)
        String id = (d.name + "-" + d.version).replaceAll("[^A-Za-z0-9._-]+", "_");
        if (id.length() > 80) id = id.substring(0, 80);
        File dest = new File(root(c), id);
        Backup.deleteTree(dest);
        if (!tmp.renameTo(dest)) {
            Backup.deleteTree(tmp);
            throw new IOException("cannot move " + tmp);
        }
        return id;
    }

    static void remove(Context c, String id) {
        if (id == null || id.isEmpty()) return;
        File cache = pipelineCache(c, id);
        if (cache != null) //noinspection ResultOfMethodCallIgnored
            cache.delete();
        Backup.deleteTree(new File(root(c), id));
    }

    /** the driver's pipeline cache (noted in <id>/.pipeline_cache by the renderer), or null */
    static File pipelineCache(Context c, String id) {
        try {
            File note = new File(new File(root(c), id), ".pipeline_cache");
            if (!note.exists()) return null;
            File f = new File(new String(Files.readAllBytes(note.toPath()), StandardCharsets.UTF_8).trim());
            // only a file in the shader cache folder
            File dir = new File(c.getNoBackupFilesDir(), "shadercache");
            return dir.getCanonicalPath().equals(f.getParentFile().getCanonicalPath()) ? f : null;
        } catch (IOException e) {
            return null;
        }
    }

    /** Copies libadrenotools' hooks out of the APK (again after an update): false if that fails. */
    static boolean prepareHooks(Context c) {
        File dir = hookDir(c);
        File stamp = new File(dir, "apk");
        String apk = c.getApplicationInfo().sourceDir;
        String want = apk + " " + new File(apk).lastModified();
        try {
            if (stamp.exists() && new String(Files.readAllBytes(stamp.toPath()), StandardCharsets.UTF_8).equals(want)) return true;
            Backup.deleteTree(dir);
            if (!dir.mkdirs()) return false;
            try (ZipFile z = new ZipFile(apk)) {
                for (String lib : new String[] {"libhook_impl.so", "libmain_hook.so"}) {
                    ZipEntry e = z.getEntry("lib/arm64-v8a/" + lib);
                    if (e == null) return false;
                    try (InputStream in = z.getInputStream(e); OutputStream out = new FileOutputStream(new File(dir, lib))) {
                        byte[] buf = new byte[1 << 16];
                        for (int k; (k = in.read(buf)) > 0; ) out.write(buf, 0, k);
                    }
                }
            }
            Files.write(stamp.toPath(), want.getBytes(StandardCharsets.UTF_8));
            return true;
        } catch (IOException e) {
            return false;
        }
    }
}
