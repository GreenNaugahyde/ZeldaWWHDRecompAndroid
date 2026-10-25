package org.wwhdrecomp.app;

/**
 * Names for the context buttons: the game's button actions (dActStts codes) in the game's own words and
 * language (its HUD's texts, CommandGuide_00 in the player's 2D pack, read like GameFont), else short English
 * names (as in the GameCube version's decompilation); and the items on X / Y / R (item ids).
 */
final class GameText {
    private GameText() {}

    private static final String DIR = "game_text", LAYOUT = "CommandGuide_00", MESSAGES = "message", FIGURINES = "message4";
    private static final int VERSION = 3;
    // the Nintendo Gallery's figurine texts (message4: "Orca\nBirthplace: ..."): 14301 + gallery number (picto subject - 0x49)
    private static final int FIGURINE = 14301, FIGURINE_COUNT = 134;
    // the messages kept of the game's message file: "You conducted the *Wind's Requiem*!" (01453 + melody number)
    private static final int CONDUCTED = 1453;
    // the game's text (T_ACT_Command_NN) for each action code; 0: none. The HUD's own table, read from the running
    // game (Native.actionTexts; the releases may differ); until then the USA one (10491350, filled at start by 0264AB20)
    private static int[] command;
    private static final int[] COMMAND_USA = {
            0, 34, 20, 0, 4, 5, 10, 6, 13, 11, 7, 8, 12, 0, 15, 14,       // 0x00
            16, 17, 18, 21, 22, 0, 2, 19, 0, 25, 0, 4, 5, 48, 0, 0,       // 0x10
            29, 30, 24, 31, 33, 42, 1, 37, 0, 0, 38, 0, 40, 15, 4, 41,    // 0x20
            0, 0, 6, 0, 0, 0, 27, 0, 0, 0, 0, 0, 0, 43, 0, 43,            // 0x30
            44, 45, 46, 47, 49, 50, 3, 0, 23, 28, 26, 35, 36, 0};         // 0x40
    private static java.util.Map<String, String> texts;
    private static String textsLang;
    private static boolean building;

    /** the game's texts for the game language: loaded, or built in the background (whenReady runs on the main
     *  thread after) */
    static synchronized void prepare(android.content.Context c, Runnable whenReady) {
        MainActivity a = MainActivity.instance;
        if (a == null || building) return;
        String lang = a.gameLanguage();
        if (lang.equals(textsLang)) return;
        java.io.File dir = new java.io.File(new java.io.File(c.getFilesDir(), DIR), lang), f = new java.io.File(dir, LAYOUT + ".txt"),
                m = new java.io.File(dir, MESSAGES + ".txt"), fig = new java.io.File(dir, FIGURINES + ".txt");
        if (new java.io.File(dir, "version" + VERSION).exists()) {
            texts = oneLine(load(f));
            if (texts != null) texts.putAll(loadMessages(m));
            if (texts != null) texts.putAll(loadFigurines(fig));
            textsLang = lang;
            return;
        }
        building = true;
        String gameDir = a.gameDir();
        new Thread(() -> {
            Backup.deleteTree(dir);
            if ((dir.mkdirs() || dir.isDirectory()) && Native.extractUiTexts(gameDir, f.getAbsolutePath(), LAYOUT)
                    && Native.extractUiTexts(gameDir, m.getAbsolutePath(), MESSAGES)
                    && Native.extractUiTexts(gameDir, fig.getAbsolutePath(), FIGURINES)) {
                try {
                    new java.io.File(dir, "version" + VERSION).createNewFile();
                } catch (java.io.IOException ignored) {
                }
            }
            synchronized (GameText.class) {
                texts = oneLine(load(f));
                if (texts != null) texts.putAll(loadMessages(m));
                if (texts != null) texts.putAll(loadFigurines(fig));
                textsLang = lang;
                building = false;
            }
            if (whenReady != null) new android.os.Handler(android.os.Looper.getMainLooper()).post(whenReady);
        }, "game-text").start();
    }

    private static java.util.Map<String, String> load(java.io.File f) {
        java.util.Map<String, String> m = new java.util.HashMap<>();
        try (java.io.BufferedReader r = new java.io.BufferedReader(new java.io.InputStreamReader(new java.io.FileInputStream(f),
                java.nio.charset.StandardCharsets.UTF_8))) {
            String line;
            while ((line = r.readLine()) != null) {
                int tab = line.indexOf('\t');
                if (tab > 0) m.put(line.substring(0, tab), line.substring(tab + 1).trim());
            }
        } catch (java.io.IOException e) {
            return null;
        }
        return m;
    }

    // line breaks (kept as \\n in the files) as spaces
    private static java.util.Map<String, String> oneLine(java.util.Map<String, String> m) {
        if (m != null) m.replaceAll((k, v) -> v.replace("\\n", " "));
        return m;
    }

    // the few messages used (the file has thousands): the songs' "conducted" lines, keyed "msg:<label>"
    private static java.util.Map<String, String> loadMessages(java.io.File f) {
        java.util.Map<String, String> m = new java.util.HashMap<>(), all = load(f);
        if (all == null) return m;
        for (int i = 0; i < SongMatcher.SONG_COUNT; i++) {
            String k = String.format(java.util.Locale.ROOT, "%05d", CONDUCTED + i), v = all.get(k);
            if (v != null) m.put("msg:" + k, v.replace("\\n", " "));
        }
        return m;
    }

    // the figurines' names (the first line of each text), keyed "fig:<gallery number>"
    private static java.util.Map<String, String> loadFigurines(java.io.File f) {
        java.util.Map<String, String> m = new java.util.HashMap<>(), all = load(f);
        if (all == null) return m;
        for (int i = 0; i < FIGURINE_COUNT; i++) {
            String v = all.get(String.format(java.util.Locale.ROOT, "%05d", FIGURINE + i));
            if (v == null) continue;
            int nl = v.indexOf("\\n");  // (the file keeps line breaks as \n)
            String name = (nl > 0 ? v.substring(0, nl) : v).replace("*", "").trim();
            if (!name.isEmpty()) m.put("fig:" + i, name);
        }
        return m;
    }

    /** the figurine name of a picto's subject (DSNAP type, from 0x49) in the game's language; null if none */
    static String figurineName(int subject) {
        if (subject < 0x49 || subject >= 0x49 + FIGURINE_COUNT) return null;
        java.util.Map<String, String> t;
        synchronized (GameText.class) { t = texts; }
        return t == null ? null : t.get("fig:" + (subject - 0x49));
    }

    private static final String[] SONG_NAMES = {"Wind's Requiem", "Ballad of Gales", "Command Melody", "Earth God's Lyric",
            "Wind God's Aria", "Song of Passing"};

    /** a song's name (melody number) as the game writes it in its language: the coloured part of its "You conducted
     *  the ...!" message; else English */
    static String songName(int melody) {
        if (melody < 0 || melody >= SONG_NAMES.length) return "";
        java.util.Map<String, String> t;
        synchronized (GameText.class) { t = texts; }
        String msg = t == null ? null : t.get(String.format(java.util.Locale.ROOT, "msg:%05d", CONDUCTED + melody));
        if (msg != null) {
            for (String part : msg.split("\\*")) {
                part = part.trim();
                if (!part.isEmpty() && msg.indexOf('*') < msg.indexOf(part)) return part.replaceAll("\\s+", " ");
            }
        }
        return SONG_NAMES[melody];
    }

    /** the action's name: the game's own (as its HUD shows it), else the English one, else null */
    static String action(int code) {
        java.util.Map<String, String> t;
        synchronized (GameText.class) { t = texts; }
        if (t == null) return english(code);
        if (command == null) {
            int[] c = Native.actionTexts();
            if (c.length > 0) {
                command = c;
                if (!java.util.Arrays.equals(c, COMMAND_USA)) android.util.Log.i("wwhd", "game text: action table " + java.util.Arrays.toString(c));
            }
        }
        int[] cmd = command != null ? command : COMMAND_USA;
        if (code >= 0 && code < cmd.length && cmd[code] != 0) {
            String s = t.get(String.format(java.util.Locale.ROOT, "T_ACT_Command_%02d", cmd[code]));
            if (s != null && !s.isEmpty()) return s;
        }
        return english(code);
    }

    private static String english(int code) {
        switch (code) {
            case 0x01: return "Look";
            case 0x02: return "Speak";
            case 0x03: return "Charts";
            case 0x04: return "Lift";
            case 0x05: return "Climb";
            case 0x06: case 0x16: return "Let go";
            case 0x07: return "Return";
            case 0x08: return "Put away";
            case 0x09: return "Drop";
            case 0x0A: return "Check";
            case 0x0B: return "Open";
            case 0x0C: return "Roll";  // the decompilation's "Attack": A while running rolls
            case 0x0E: return "Throw";
            case 0x0F: return "Crouch";
            case 0x10: return "Sidle";
            case 0x11: return "Grab";
            case 0x12: return "Jump";
            case 0x13: return "Stop";
            case 0x17: return "Choose";
            case 0x19: return "Next";
            case 0x1A: return "Parry";
            case 0x1B: return "Pick up";
            case 0x1C: return "Get in";
            case 0x1D: return "Get out";
            case 0x20: return "Photo";
            case 0x21: return "Info";
            case 0x22: return "Swap";
            case 0x23: return "Fly";
            case 0x24: return "Call";
            case 0x25: return "Bid";
            case 0x26: return "Read";
            case 0x27: return "Cancel";
            case 0x2A: return "Zoom 1";
            case 0x2B: return "Zoom 2";
            case 0x2C: return "Cruise";
            case 0x2F: return "Swing";
            case 0x30: return "Chart";
            case 0x36: return "Defend";
            case 0x37: return "Stick";    // weapons picked up from enemies (B)
            case 0x38: return "Machete";
            case 0x39: return "Club";
            case 0x3A: return "Sword";
            case 0x3B: case 0x3C: return "Spear";
            case 0x3D: return "Bow";
            case 0x3F: return "Fire";    // the boat's cannon (HD)
            case 0x40: return "Salvage";
            case 0x41: return "Fast";    // the sail's speed
            case 0x42: return "Normal";
            case 0x43: return "Equip";
            case 0x44: return "Tingle Bottle";
            case 0x45: return "Erase";
            case 0x46: return "End";
            case 0x49: return "Album";  // the Picto Box's album (HD)
            case 0x48: return "Picto";  // the album: back to the Picto Box (HD)
            case 0x4A: return "Switch";
            case 0x4B: return "Choose";
            case 0x4C: return "Close";
            default: return null;
        }
    }

    static String item(int id) {
        switch (id) {
            case 0x20: return "Telescope";
            case 0x21: return "Tingle";
            case 0x22: return "Baton";
            case 0x23: case 0x26: return "Picto";
            case 0x24: return "Spoils";
            case 0x25: return "Hook";
            case 0x27: return "Bow";
            case 0x29: return "Boots";
            case 0x2A: return "Armor";
            case 0x2C: return "Bait";
            case 0x2D: return "Boomer.";
            case 0x2F: return "Hookshot";
            case 0x30: return "Mail";
            case 0x31: return "Bombs";
            case 0x33: return "Hammer";
            case 0x34: return "Leaf";
            case 0x35: case 0x36: return "Bow";
            case 0x50: case 0x51: case 0x52: case 0x53: case 0x54: case 0x55: case 0x56: case 0x57: case 0x58: case 0x59:
                return "Bottle";
            case 0x77: case 0x78: return "Sail";
            default: return null;
        }
    }
}
