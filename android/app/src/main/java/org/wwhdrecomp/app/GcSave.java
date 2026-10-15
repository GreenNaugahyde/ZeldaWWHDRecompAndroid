package org.wwhdrecomp.app;

import java.nio.charset.Charset;
import java.nio.charset.StandardCharsets;
import java.text.Normalizer;
import java.util.zip.CRC32;

/**
 * GameCube Wind Waker saves (.gci memory card files, USA GZLE01 / Japan GZLJ01 / Europe GZLP01) for WWHD: their
 * slots for story presets (StoryPresets), and the whole file as a WWHD cking.sav (Import saves), as
 * the upstream converter tools/savegame/gc2hd.py writes it (layouts and checksums: its README and
 * wwsave.py). Plain Java (no Android classes), so it can be checked on a PC against gc2hd.py.
 *
 * .gci: a 0x40-byte directory entry, then card blocks of 0x2000; blocks 1 and 2 are two copies of
 * the save data: 8 bytes, then 3 slots of 0x770 = 0x768 bytes of packed save data + a u64 checksum.
 * The game reads a slot from the first copy whose checksum holds (mDoMemCdRWm_Restore). The packed
 * data is the same on both systems (dSv_info_c::card_to_memory copies the same blocks) and in all
 * three regions. Europe (upstream 62e05dd): the first u32 of the card data (block 1, not the slot's
 * save counter) holds the card-wide language, 0..4 English, German, French, Spanish, Italian (larger:
 * English, as mDoMemCdRWm_Restore); it goes to byte 4 of each slot's HD options (the console language
 * still picks the text). Names are single-byte Western there, CP1252 (Œ/œ at 0x8C/0x9C, d_name.cpp).
 *
 * cking.sav (8966 bytes): 3 slots of 0xA94 (packed data, zero up to 0xA8C, u32 byte sum and u32
 * complement sum over those 0xA8C bytes), the HD-only data of the 3 slots (player 16, status 4,
 * event 20, name 18 = UTF-16BE, map 220 bytes: each section for slots 1..3), the format word 4 and a
 * CRC-32 over the HD-only data, where each name counts only (characters + 1) bytes, zero padded.
 */
final class GcSave {
    private GcSave() {}

    static final int PACKED_LEN = 0x768;
    private static final int GCI_HEADER = 0x40, CARD_BLOCK = 0x2000, GC_SLOT = 0x770;
    private static final int HD_SLOT = 0xA94, HD_SLOT_USED = 0xA8C, HD_FILE = 8966;
    private static final int[] HD_EXTRA = {16, 4, 20, 18, 220};  // player, status, event, name, map
    // dSv_player_info_c in the packed data (0x143): save counter +0x10 (u16), three name fields of 17
    private static final int INFO = 0x143, SAVE_COUNT = INFO + 0x10, NAME = INFO + 0x14;
    private static final int[] NAME_FIELDS = {0x14, 0x25, 0x36};

    /** Not a supported Wind Waker GameCube save; the message says why. */
    static final class Unsupported extends Exception {
        Unsupported(String why) { super(why); }
    }

    /** A Wind Waker GameCube save file (by its directory entry), of any region. */
    static boolean isGci(byte[] d) {
        return d.length >= 15 && d[0] == 'G' && d[1] == 'Z' && d[2] == 'L'
                && new String(d, 8, 7, StandardCharsets.US_ASCII).equals("gczelda");
    }

    static boolean japanese(byte[] gci) { return gci[3] == 'J'; }

    /** The offset of each slot's packed save data in the file (-1: neither copy is intact). */
    static int[] slots(byte[] gci) throws Unsupported {
        if (!isGci(gci)) throw new Unsupported("not a Wind Waker GameCube save");
        if ((gci[3] != 'E' && gci[3] != 'J' && gci[3] != 'P') || gci[4] != '0' || gci[5] != '1')
            throw new Unsupported("unsupported game code " + new String(gci, 0, 6, StandardCharsets.US_ASCII));
        if (gci.length < GCI_HEADER + 3 * CARD_BLOCK) throw new Unsupported("the file is too short");
        int[] out = new int[3];
        for (int s = 0; s < 3; s++) {
            out[s] = -1;
            for (int k = 1; k <= 2 && out[s] < 0; k++) {
                int o = GCI_HEADER + k * CARD_BLOCK + 8 + s * GC_SLOT;
                if (checksumOk(gci, o)) out[s] = o;
            }
        }
        return out;
    }

    /** The three name fields hold Shift-JIS on the Japanese disc; WWHD's own files leave them empty. */
    static void clearShiftJisNames(byte[] data, int info) {
        for (int f : NAME_FIELDS) {
            int o = info + f;
            boolean sjis = false;
            for (int i = 0; i < 17; i++) sjis |= data[o + i] < 0;
            if (sjis) java.util.Arrays.fill(data, o, o + 17, (byte) 0);
        }
    }

    /** The whole save file as a WWHD cking.sav (its three slots become the three quest logs). */
    static byte[] toCkingSav(byte[] gci) throws Unsupported {
        int[] at = slots(gci);
        boolean jp = japanese(gci), pal = gci[3] == 'P';
        long language = pal ? get32(gci, GCI_HEADER + CARD_BLOCK) : 0;  // the card's, read as the game does
        if (language > 4) language = 0;
        byte[] out = new byte[HD_FILE];
        String[] names = new String[3];
        for (int s = 0; s < 3; s++) {
            if (at[s] < 0) throw new Unsupported("slot " + (s + 1) + ": both copies on the card are damaged");
            byte[] g = java.util.Arrays.copyOfRange(gci, at[s], at[s] + PACKED_LEN);
            String gcName = gcName(g, jp, pal);
            boolean fresh = g[NAME] == 0;  // GameCube file select: no name = "New Game"
            if (jp) clearShiftJisNames(g, INFO);
            names[s] = fresh || gcName.isEmpty() ? "Link" : hdSafeName(gcName);
            // HD marks a used file by its save counter (0 shows "New Game"); the GameCube leaves it 0
            if (!fresh && g[SAVE_COUNT] == 0 && g[SAVE_COUNT + 1] == 0) g[SAVE_COUNT + 1] = 1;
            // the Tingle Tuner (item 0x21, slot 7) stays: HD's Tingle Bottle has the same number
            int o = s * HD_SLOT;
            System.arraycopy(g, 0, out, o, PACKED_LEN);
            long sum = 0;
            for (int i = 0; i < HD_SLOT_USED; i++) sum += out[o + i] & 0xFF;
            long c0 = sum & 0xFFFFFFFFL, c1 = (HD_SLOT_USED * 0xFFFFFFFFL - c0) & 0xFFFFFFFFL;
            put32(out, o + HD_SLOT_USED, c0);
            put32(out, o + HD_SLOT_USED + 4, c1);
        }
        // HD-only data: a fresh file's (player byte 5 = 1), with the names and a European card's language
        int p = 3 * HD_SLOT;
        CRC32 crc = new CRC32();
        for (int sect = 0; sect < HD_EXTRA.length; sect++) {
            for (int s = 0; s < 3; s++) {
                byte[] v = new byte[HD_EXTRA[sect]];
                byte[] forCrc = v;
                if (sect == 0) {
                    v[4] = (byte) language;
                    v[5] = 1;
                }
                if (sect == 3) {
                    byte[] u = names[s].getBytes(StandardCharsets.UTF_16BE);
                    System.arraycopy(u, 0, v, 0, Math.min(16, u.length));
                    int chars = 0;
                    while (chars < 9 && (v[2 * chars] != 0 || v[2 * chars + 1] != 0)) chars++;
                    forCrc = new byte[18];  // the game copies (characters + 1) BYTES of the name
                    System.arraycopy(v, 0, forCrc, 0, chars + 1);
                }
                System.arraycopy(v, 0, out, p, v.length);
                crc.update(forCrc);
                p += v.length;
            }
        }
        put32(out, p, 4);  // format word
        put32(out, p + 4, crc.getValue());
        return out;
    }

    private static String gcName(byte[] g, boolean jp, boolean pal) {
        int n = 0;
        while (n < 17 && g[NAME + n] != 0) n++;
        Charset cs = jp ? Charset.forName("Shift_JIS") : pal ? Charset.forName("windows-1252") : StandardCharsets.ISO_8859_1;
        return new String(g, NAME, n, cs);
    }

    // the HD name area holds 8 UTF-16 characters and the USA font is Latin only: full-width letters
    // become ASCII (NFKC), anything else non-Latin becomes "Link"
    private static String hdSafeName(String name) {
        String f = Normalizer.normalize(name, Normalizer.Form.NFKC);
        if (f.isEmpty()) return "Link";
        for (int i = 0; i < f.length(); i++) {
            char c = f.charAt(i);
            if (c < 0x20 || c >= 0x250) return "Link";
        }
        return f.length() > 8 ? f.substring(0, 8) : f;
    }

    // mDoMemCdRWm_CalcCheckSumGameData: u32 byte sum and u32 sum of the complements after the data
    private static boolean checksumOk(byte[] b, int o) {
        long sum = 0;
        for (int i = 0; i < PACKED_LEN; i++) sum += b[o + i] & 0xFF;
        long c0 = sum & 0xFFFFFFFFL, c1 = (PACKED_LEN * 0xFFFFFFFFL - c0) & 0xFFFFFFFFL;
        return get32(b, o + PACKED_LEN) == c0 && get32(b, o + PACKED_LEN + 4) == c1;
    }

    private static long get32(byte[] b, int o) {
        return ((long) (b[o] & 0xFF) << 24) | ((b[o + 1] & 0xFF) << 16) | ((b[o + 2] & 0xFF) << 8) | (b[o + 3] & 0xFF);
    }

    private static void put32(byte[] b, int o, long v) {
        b[o] = (byte) (v >> 24);
        b[o + 1] = (byte) (v >> 16);
        b[o + 2] = (byte) (v >> 8);
        b[o + 3] = (byte) v;
    }
}
