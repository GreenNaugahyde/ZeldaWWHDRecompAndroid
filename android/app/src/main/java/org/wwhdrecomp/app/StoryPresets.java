package org.wwhdrecomp.app;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

/**
 * Story presets for the developer warp menu (mods/warp.cpp): a save's story part (dSv_save_c,
 * 0x778 bytes: inventory, each stage's memory, sea chart, event flags) and the place to warp to.
 * Made from the game running now ("Save as preset"), from each used slot of a WWHD save file
 * (cking.sav: 3 slots of 0xA94 bytes) or a GameCube memory card file (.gci, USA / Japan / Europe: GcSave), or
 * copied from a preset file of another device (.wwp). The slot's place is its return place, where loading it would start. A slot holds the save data packed (0x768 bytes): the game's card_to_memory copies its blocks
 * one after the other into dSv_save_c, whose members sit at aligned offsets (tools/savegame/README.md
 * upstream: GameCube and WWHD pack it the same way, gc2hd.py copies it field by field unchanged).
 * Built in: assets/presets/*.wwp, from a private folder outside the repository in debug builds only
 * (android/app/build.gradle). Made on the device: files/presets/<name>.wwp
 * ("WWP1", stage[8], room s8, layer s8, spawn s16 big-endian, then the story part).
 */
final class StoryPresets {
    static final int STORY_LEN = 0x778, SLOT_LEN = 0xA94, SLOTS = 3, PACKED_LEN = 0x768;
    // dSv_save_c offset and size of each block of a slot, in their order there (dSv_info_c::card_to_memory)
    private static final int[][] BLOCKS = {{0x000, 0x18}, {0x018, 0x18}, {0x030, 0xC}, {0x03C, 0x15}, {0x051, 0x15},
            {0x066, 0x8}, {0x06E, 0x8}, {0x076, 0x18}, {0x090, 0xC}, {0x09C, 0x18}, {0x0B4, 0xD}, {0x0C4, 0x84},
            {0x148, 0x5C}, {0x1A4, 0x5}, {0x1AC, 0x10}, {0x1BC, 0x1C0}, {0x380, 0x240}, {0x5C0, 0x64}, {0x624, 0x100},
            {0x724, 0x50}};

    /** A slot's packed save data in the in-memory layout (dSv_save_c). */
    static byte[] unpack(byte[] sav, int at) {
        byte[] mem = new byte[STORY_LEN];
        int p = at;
        for (int[] b : BLOCKS) {
            System.arraycopy(sav, p, mem, b[0], b[1]);
            p += b[1];
        }
        return mem;
    }
    private static final int RETURN_PLACE = 0x30;  // dSv_player_return_place_c: name[8], room s8, point u8

    static final class Preset {
        final File file;  // null: built into the app (it can't be deleted)
        final String name, stage;
        final int room, spawn, layer;
        final byte[] story;

        Preset(File file, String name, String stage, int room, int spawn, int layer, byte[] story) {
            this.file = file;
            this.name = name;
            this.stage = stage;
            this.room = room;
            this.spawn = spawn;
            this.layer = layer;
            this.story = story;
        }

        int hearts() { return u16(story, 2) / 4; }
        int maxHearts() { return u16(story, 0) / 4; }
    }

    static File dir(MainActivity a) { return new File(a.baseDir(), "presets"); }

    /** The built-in presets (in story order), then this device's. */
    static List<Preset> list(MainActivity a) {
        List<Preset> out = builtIn(a);
        File[] files = dir(a).listFiles((d, n) -> n.endsWith(".wwp"));
        if (files == null) return out;
        Arrays.sort(files, (x, y) -> x.getName().compareToIgnoreCase(y.getName()));
        for (File f : files) {
            Preset p = read(f);
            if (p != null) out.add(p);
        }
        return out;
    }

    private static List<Preset> builtInCache;

    static List<Preset> builtIn(MainActivity a) {
        if (builtInCache == null) {
            builtInCache = new ArrayList<>();
            try {
                String[] names = a.getAssets().list("presets");
                if (names != null) {
                    Arrays.sort(names);
                    for (String n : names) {
                        if (!n.endsWith(".wwp")) continue;
                        try (InputStream in = a.getAssets().open("presets/" + n)) {
                            Preset p = read(in, null, n);
                            if (p != null) builtInCache.add(p);
                        }
                    }
                }
            } catch (IOException ignored) {
            }
        }
        return new ArrayList<>(builtInCache);
    }

    private static Preset read(File f) {
        try (InputStream in = new FileInputStream(f)) {
            return read(in, f, f.getName());
        } catch (IOException e) {
            return null;
        }
    }

    private static Preset read(InputStream in, File f, String fileName) throws IOException {
        byte[] b = new byte[16 + STORY_LEN];
        int n = 0;
        for (int r; n < b.length && (r = in.read(b, n, b.length - n)) > 0; ) n += r;
        if (n != b.length || b[0] != 'W' || b[1] != 'W' || b[2] != 'P' || b[3] != '1') return null;
        String name = fileName.substring(0, fileName.length() - 4);
        return new Preset(f, name, cstr(b, 4, 8), b[12], (short) u16(b, 14), b[13], Arrays.copyOfRange(b, 16, 16 + STORY_LEN));
    }

    /** Writes a preset; a name already used gets " 2", " 3"... Null if it couldn't be written. */
    static Preset save(MainActivity a, String name, String stage, int room, int spawn, int layer, byte[] story) {
        File d = dir(a);
        //noinspection ResultOfMethodCallIgnored
        d.mkdirs();
        String base = name.replaceAll("[/\\\\:*?\"<>|]", "_").trim();
        if (base.isEmpty()) base = "Preset";
        File f = new File(d, base + ".wwp");
        for (int i = 2; f.exists(); i++) f = new File(d, base + " " + i + ".wwp");
        byte[] b = new byte[16 + STORY_LEN];
        b[0] = 'W';
        b[1] = 'W';
        b[2] = 'P';
        b[3] = '1';
        byte[] s = stage.getBytes(StandardCharsets.US_ASCII);
        System.arraycopy(s, 0, b, 4, Math.min(8, s.length));
        b[12] = (byte) room;
        b[13] = (byte) layer;
        b[14] = (byte) (spawn >> 8);
        b[15] = (byte) spawn;
        System.arraycopy(story, 0, b, 16, STORY_LEN);
        try (FileOutputStream out = new FileOutputStream(f)) {
            out.write(b);
        } catch (IOException e) {
            return null;
        }
        return read(f);
    }

    /** Why a file gave no presets (a string resource). */
    static final class ImportError extends Exception {
        final int message;

        ImportError(int message) { this.message = message; }
    }

    /**
     * The used slots of a save file as presets: a WWHD cking.sav, or a GameCube memory card file (.gci,
     * USA GZLE01 / Japan GZLJ01). One used slot is named `base`, several "`base` slot N". How many.
     */
    static int importFile(MainActivity a, String base, byte[] data) throws ImportError {
        // a preset file (from another device): as it is
        if (data.length == 16 + STORY_LEN && data[0] == 'W' && data[1] == 'W' && data[2] == 'P' && data[3] == '1') {
            Preset p = save(a, base, cstr(data, 4, 8), data[12], (short) u16(data, 14), data[13],
                    Arrays.copyOfRange(data, 16, 16 + STORY_LEN));
            if (p == null) throw new ImportError(R.string.preset_import_none);
            return 1;
        }
        List<int[]> found = new ArrayList<>();  // {slot number, offset of its packed save data}
        boolean japanese = false;
        if (GcSave.isGci(data)) {
            int[] at;
            try {
                at = GcSave.slots(data);
            } catch (GcSave.Unsupported e) {
                throw new ImportError(R.string.preset_import_none);
            }
            japanese = GcSave.japanese(data);
            for (int i = 0; i < SLOTS; i++)
                if (at[i] >= 0) found.add(new int[] {i + 1, at[i]});
        } else {
            for (int i = 0; i < SLOTS; i++)
                if ((i + 1) * SLOT_LEN <= data.length) found.add(new int[] {i + 1, i * SLOT_LEN});
        }
        List<int[]> used = new ArrayList<>();
        for (int[] f : found)
            if (playedSlot(data, f[1])) used.add(f);
        if (used.isEmpty()) throw new ImportError(R.string.preset_import_none);
        int n = 0;
        for (int[] f : used) {
            int o = f[1];
            String stage = cstr(data, o + RETURN_PLACE, 8);
            int room = data[o + RETURN_PLACE + 8], point = data[o + RETURN_PLACE + 9] & 0xFF;
            String name = used.size() == 1 ? base : base + " slot " + f[0];
            byte[] story = unpack(data, o);
            if (japanese) GcSave.clearShiftJisNames(story, 0x148);  // dSv_player_info_c in memory
            if (save(a, name, stage, room, point, -1, story) != null) n++;
        }
        return n;
    }

    // a slot whose hearts and return place look like a played file (the packed and the in-memory layout
    // agree up to the items)
    private static boolean playedSlot(byte[] b, int o) {
        if (o + PACKED_LEN > b.length) return false;
        int max = u16(b, o), life = u16(b, o + 2);
        String stage = cstr(b, o + RETURN_PLACE, 8);
        return max >= 12 && max <= 80 && life <= max && stage.length() >= 3 && stage.matches("[0-9A-Za-z_]+");
    }

    private static int u16(byte[] b, int o) { return (b[o] & 0xFF) << 8 | (b[o + 1] & 0xFF); }

    private static String cstr(byte[] b, int o, int len) {
        int n = 0;
        while (n < len && b[o + n] != 0) n++;
        return new String(b, o, n, StandardCharsets.US_ASCII);
    }
}
