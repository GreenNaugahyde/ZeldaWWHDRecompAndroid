package org.wwhdrecomp.app;

import android.media.AudioAttributes;
import android.media.SoundPool;
import android.util.Log;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.RandomAccessFile;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;

/** The GamePad menus' own sounds (the HD version's c_king.bfsar: SE_MENU_CURSOR, SE_MENU_DECIDE...) for the
 *  options menu, with Game > GamePad sounds (issue #64). Read once from the game's sound archive, decoded
 *  (DSP ADPCM) into WAV files in files/menu_sounds and played with a SoundPool. */
final class MenuSounds {
    static final int CURSOR = 0, SWITCH = 1, DECIDE = 2, CANCEL = 3, TAB = 4, OPEN = 5, CLOSE = 6, WINDOW_OPEN = 7,
            WINDOW_CLOSE = 8;
    private static final String[] NAMES = {"SE_MENU_CURSOR", "SE_MENU_OPTION_SWITCH", "SE_MENU_DECIDE_S", "SE_MENU_CANCEL",
            "SE_MENU_TAB_SWITCH", "SE_MENU_OPTION_IN", "SE_MENU_OPTION_OUT", "SE_MENU_MINI_WINDOW_OPEN",
            "SE_MENU_MINI_WINDOW_CLOSE"};
    private static final String TAG = "wwhd", DIR = "menu_sounds";
    private static final int VERSION = 1;

    private static SoundPool pool;
    private static final int[] ids = new int[NAMES.length];
    private static final float[] volumes = new float[NAMES.length];
    private static boolean started;

    /** loads them in the background (once); a sound asked for before that stays quiet */
    static synchronized void prepare(MainActivity a) {
        if (started) return;
        started = true;
        File dir = new File(new File(a.getFilesDir(), DIR), "v" + VERSION);
        String archive = a.gameDir() + "/content/Common/AudioRes/c_king.bfsar";
        new Thread(() -> {
            try {
                if (!new File(dir, "volumes").exists()) extract(archive, dir);
                SoundPool p = new SoundPool.Builder().setMaxStreams(4).setAudioAttributes(new AudioAttributes.Builder()
                        .setUsage(AudioAttributes.USAGE_GAME).setContentType(AudioAttributes.CONTENT_TYPE_SONIFICATION).build()).build();
                String[] vol = new String(java.nio.file.Files.readAllBytes(new File(dir, "volumes").toPath())).trim().split(" ");
                synchronized (MenuSounds.class) {
                    for (int i = 0; i < NAMES.length; i++) {
                        ids[i] = p.load(new File(dir, NAMES[i] + ".wav").getAbsolutePath(), 1);
                        volumes[i] = Math.min(1, Integer.parseInt(vol[i]) / 127f);
                    }
                    pool = p;
                }
            } catch (Exception e) {
                Log.w(TAG, "[menu sounds] not read: " + e);
            }
        }, "menu-sounds").start();
    }

    static void play(MainActivity a, int sound) {
        if (!a.prefs.getBoolean("gamepad_sounds", true)) return;
        synchronized (MenuSounds.class) {
            if (pool != null) pool.play(ids[sound], volumes[sound], volumes[sound], 1, 0, 1);
        }
    }

    // ------------------------------------------------------------------ the sound archive (NW4F, big-endian)
    private static ByteBuffer d;

    private static int u8(int o) { return d.get(o) & 0xFF; }
    private static int u16(int o) { return d.getShort(o) & 0xFFFF; }
    private static int s32(int o) { return d.getInt(o); }
    private static int refOffset(int o) { return d.getInt(o + 4); }  // a reference: type u16, pad, offset s32

    /** a reference table (count, then references) as absolute offsets */
    private static int[] table(int base) {
        int[] t = new int[s32(base)];
        for (int i = 0; i < t.length; i++) t[i] = base + s32(base + 4 + 8 * i + 4);
        return t;
    }

    /** a section's body in an NW4F file (header: magic, BOM, header size, version, size, count, then
     *  {id u16, pad, offset u32, size u32} per section) */
    private static int section(int file, int i) { return file + s32(file + 24 + 12 * i) + 8; }

    private static void extract(String archive, File dir) throws IOException {
        try (RandomAccessFile f = new RandomAccessFile(archive, "r")) {
            byte[] all = new byte[(int) f.length()];
            f.readFully(all);
            d = ByteBuffer.wrap(all).order(ByteOrder.BIG_ENDIAN);
        }
        try {
            int strg = section(0, 0), info = section(0, 1), fileBlock = section(0, 2);
            // the names (string table + patricia tree's leaves: string index -> item id)
            int strings = strg + refOffset(strg), tree = strg + refOffset(strg + 8);
            int[] items = new int[s32(strings)];
            for (int i = 0, n = s32(tree + 4); i < n; i++) {
                int node = tree + 8 + 20 * i;
                if ((u16(node) & 1) != 0 && s32(node + 12) < items.length) items[s32(node + 12)] = s32(node + 16);
            }
            int[] sounds = table(info + refOffset(info)), waveArchives = table(info + refOffset(info + 24)),
                    files = table(info + refOffset(info + 48));
            StringBuilder vol = new StringBuilder();
            dir.mkdirs();
            for (String name : NAMES) {
                int item = -1;
                for (int i = 0; i < items.length; i++) {
                    int e = strings + 4 + 12 * i, off = s32(e + 4), size = s32(e + 8);
                    if (size - 1 == name.length() && new String(d.array(), strings + off, size - 1).equals(name)) item = items[i];
                }
                if (item == -1) throw new IOException(name + " missing");
                int s = sounds[item & 0xFFFFFF];
                if (u16(s + 12) != 0x2202) throw new IOException(name + " not a wave sound");
                int index = s32(s + refOffset(s + 12));
                vol.append(u8(s + 8)).append(' ');
                // its wave sound file: the wave sound's first note -> (wave archive, wave)
                int wsd = fileData(s32(s), files, fileBlock), wi = section(wsd, 0);
                int waveIds = wi + refOffset(wi), x = table(wi + refOffset(wi + 8))[index];
                int note = table(x + refOffset(x + 16))[0], wid = s32(note);
                int arc = s32(waveIds + 4 + 8 * wid) & 0xFFFFFF, wave = s32(waveIds + 8 + 8 * wid);
                int war = fileData(s32(waveArchives[arc]), files, fileBlock);
                int warInfo = section(war, 0), warFile = section(war, 1);
                writeWav(warFile + s32(warInfo + 4 + 12 * wave + 4), new File(dir, name + ".wav"));
            }
            try (FileOutputStream o = new FileOutputStream(new File(dir, "volumes"))) {
                o.write(vol.toString().getBytes());
            }
            Log.i(TAG, "[menu sounds] read from the game's sound archive");
        } finally {
            d = null;
        }
    }

    /** a file of the archive: in its file block, or in one of its group files */
    private static int fileData(int id, int[] files, int fileBlock) throws IOException {
        int p = files[id] + refOffset(files[id]);
        if (refOffset(p) != -1) return fileBlock + refOffset(p);
        for (int fi : files) {
            if (u16(fi) != 0x220C) continue;  // (internal files)
            int q = fi + refOffset(fi);
            if (refOffset(q) == -1) continue;
            int g = fileBlock + refOffset(q);
            if (s32(g) != 0x46475250) continue;  // FGRP
            int gi = section(g, 0), gf = section(g, 1);
            for (int e : table(gi))
                if (s32(e) == id) return gf + refOffset(e + 4);
        }
        throw new IOException("file " + id + " missing");
    }

    /** a wave (FWAV, DSP ADPCM, mono) as a 16-bit WAV file */
    private static void writeWav(int wav, File out) throws IOException {
        int info = section(wav, 0), data = section(wav, 1);
        if (u8(info) != 2) throw new IOException("not DSP ADPCM");
        int rate = s32(info + 4), n = s32(info + 12);
        int ch = table(info + 0x14)[0];
        int src = data + refOffset(ch), adpcm = ch + refOffset(ch + 8);
        short[] coef = new short[16];
        for (int i = 0; i < 16; i++) coef[i] = d.getShort(adpcm + 2 * i);
        int h1 = d.getShort(adpcm + 34), h2 = d.getShort(adpcm + 36);  // (after the predictor/scale)
        ByteBuffer w = ByteBuffer.allocate(44 + 2 * n).order(ByteOrder.LITTLE_ENDIAN);
        w.put("RIFF".getBytes()).putInt(36 + 2 * n).put("WAVEfmt ".getBytes()).putInt(16).putShort((short) 1).putShort((short) 1)
                .putInt(rate).putInt(rate * 2).putShort((short) 2).putShort((short) 16).put("data".getBytes()).putInt(2 * n);
        for (int done = 0, i = src; done < n; i += 8) {  // frames of 8 bytes: predictor/scale, 14 nibbles
            int ps = u8(i), scale = 1 << (ps & 15), c1 = coef[(ps >> 4) * 2], c2 = coef[(ps >> 4) * 2 + 1];
            for (int k = 0; k < 14 && done < n; k++, done++) {
                int b = u8(i + 1 + k / 2), nib = k % 2 == 0 ? b >> 4 : b & 15;
                if (nib >= 8) nib -= 16;
                int v = Math.max(-32768, Math.min(32767, (((nib * scale) << 11) + 1024 + c1 * h1 + c2 * h2) >> 11));
                w.putShort((short) v);
                h2 = h1;
                h1 = v;
            }
        }
        try (FileOutputStream o = new FileOutputStream(out)) {
            o.write(w.array());
        }
    }
}
