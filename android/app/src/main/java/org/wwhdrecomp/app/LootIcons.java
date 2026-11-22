package org.wwhdrecomp.app;

import android.graphics.Bitmap;
import android.graphics.BitmapFactory;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.util.HashMap;
import java.util.HashSet;
import java.util.Map;
import java.util.Set;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

/**
 * Pictures of the loot weapons, drawn from their real models (Native.lootIcon: from the player's own game files; kept in
 * the app's cache as PNG): for the buttons a weapon is on, the worn sword and shield, what lies on the ground and the Loot tab (armour and outfits from the front).
 */
final class LootIcons {
    private static final int SIZE = 256, KEPT = 48;
    private static final Map<String, Bitmap> done = new java.util.LinkedHashMap<String, Bitmap>(64, 0.75f, true) {
        @Override
        protected boolean removeEldestEntry(Map.Entry<String, Bitmap> e) {  // (a thousand things to scroll past; the file stays)
            if (size() <= KEPT) return false;
            asked.remove(e.getKey());
            return true;
        }
    };
    private static final Set<String> asked = new HashSet<>();
    private static final ExecutorService worker = Executors.newSingleThreadExecutor();

    private LootIcons() {}

    /** its picture, or null for now (`ready` runs when it's there; then ask again) */
    static synchronized Bitmap get(MainActivity a, Loot kind, String id, Runnable ready) {
        final int number = kind == Loot.SWORDS ? 0 : kind == Loot.SHIELDS ? 1 : 2 + kind.armor;
        final String key = kind.word() + "_" + id;
        Bitmap b = done.get(key);
        if (b != null || !asked.add(key)) return b;
        final String gameDir = a.gameDir();
        final File file = new File(new File(a.getCacheDir(), "loot_icons_256"), key + ".png");
        worker.execute(() -> {
            Bitmap made = file.exists() ? BitmapFactory.decodeFile(file.getPath()) : null;
            if (made == null) {
                made = from(Native.lootIcon(gameDir, id, number, SIZE));
                if (made != null) save(made, file);
            }
            if (made != null) {
                synchronized (LootIcons.class) { done.put(key, made); }
                ready.run();
            }
        });
        return null;
    }

    private static Bitmap from(byte[] rgba) {
        if (rgba == null || rgba.length != SIZE * SIZE * 4) return null;
        int[] px = new int[SIZE * SIZE];
        for (int i = 0; i < px.length; i++)
            px[i] = (rgba[i * 4 + 3] & 255) << 24 | (rgba[i * 4] & 255) << 16 | (rgba[i * 4 + 1] & 255) << 8 | (rgba[i * 4 + 2] & 255);
        return Bitmap.createBitmap(px, SIZE, SIZE, Bitmap.Config.ARGB_8888);
    }

    private static void save(Bitmap b, File file) {
        File dir = file.getParentFile();
        if (dir != null && !dir.isDirectory() && !dir.mkdirs()) return;
        try (FileOutputStream o = new FileOutputStream(file)) {
            b.compress(Bitmap.CompressFormat.PNG, 100, o);
        } catch (IOException ignored) {
        }
    }
}
