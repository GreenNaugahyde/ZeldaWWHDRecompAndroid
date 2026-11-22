package org.wwhdrecomp.app;

/**
 * Mods > Loot swords / Loot shields: a sword or shield won in online quests (notes/loot.md) in place of Link's own, per
 * save file and switchable, in a colour tier. The runtime makes it from the game's own files and puts it in live over
 * Link's model (runtime/src/mods/player_models.cpp, gcmodel/hd_sword.h; a sword's strength mods/loot_sword.cpp). Loot
 * helmets and chest armour the same way, on his spare models, with a defence.
 */
final class Loot {
    static final int FILES = 3;
    /** the colour tiers: 0 its own colours .. 5 rainbow; a sword's strength in percent of its own */
    static final int TIERS = 6;
    static final int[] TIER_POWER = {100, 110, 120, 135, 150, 175};

    static final Loot SWORDS = new Loot(true, -1), SHIELDS = new Loot(false, -1), HELMETS = new Loot(false, 0), CHESTS = new Loot(false, 1), OUTFITS = new Loot(false, 2);

    final boolean sword;
    /** armour: 0 head, 1 chest, 2 a whole outfit; -1 neither */
    final int armor;

    private Loot(boolean sword, int armor) {
        this.sword = sword;
        this.armor = armor;
    }

    /** the kind's word in its preferences and names */
    String word() { return sword ? "sword" : armor == 0 ? "helmet" : armor == 1 ? "chest" : armor == 2 ? "outfit" : "shield"; }

    String pref(int file) { return "loot_" + word() + "_" + file; }
    String prefTier(int file) { return (sword ? "loot_tier_" : "loot_" + word() + "_tier_") + file; }
    /** the save file its page shows */
    String prefFile() { return "loot_" + word() + "_file"; }

    /** a text of this kind: loot_title, or loot_<word>_title and so on */
    int text(MainActivity a, String part) {
        return a.getResources().getIdentifier(sword ? "loot_" + part : "loot_" + word() + "_" + part, "string", a.getPackageName());
    }

    int tier(MainActivity a, int file) { return Math.max(0, Math.min(TIERS - 1, a.prefs.getInt(prefTier(file), 0))); }

    void setTier(MainActivity a, int file, int tier) {
        a.prefs.edit().putInt(prefTier(file), tier).apply();
        handOver(a);
    }

    /** save file `file`'s choice (1..3), null for the game's own */
    String get(MainActivity a, int file) {
        String id = a.prefs.getString(pref(file), null);
        return id != null && has(id) ? id : null;
    }

    void set(MainActivity a, int file, String id) {
        if (id == null) a.prefs.edit().remove(pref(file)).apply();
        else a.prefs.edit().putString(pref(file), id).apply();
        handOver(a);
    }

    /** both kinds' choices to the runtime (it puts them in live) */
    static void handOver(MainActivity a) {
        if (!BuildConfig.LOOT) return;  // (a build without loot: the game never gets any)
        if (SavePages.viewing()) return;  // (another page's settings: not the game's)
        Native.setLootSwords(a.gameDir(), SWORDS.withTier(a, 1), SWORDS.withTier(a, 2), SWORDS.withTier(a, 3));
        Native.setLootShields(SHIELDS.withTier(a, 1), SHIELDS.withTier(a, 2), SHIELDS.withTier(a, 3));
        for (Loot L : new Loot[] {HELMETS, CHESTS, OUTFITS}) Native.setLootArmor(L.armor, L.withTier(a, 1), L.withTier(a, 2), L.withTier(a, 3));
    }

    /** "id" or "id#tier" for the runtime */
    private String withTier(MainActivity a, int file) {
        String id = get(a, file);
        int t = tier(a, file);
        return id == null || t == 0 ? id : id + "#" + t;
    }

    // the categories (rated in the in-game switcher, LootTestOverlay; lines "time, category, id, name, value" of loot_verdicts.tsv): where a thing can be
    // equipped: a sword on the Sword slot, a shield on the Shield slot, another weapon on X / Y / R
    static final String SWORD = "sword", SHIELD = "shield", OTHER = "other";
    private static java.util.Map<String, String> categories;

    private static java.io.File rated(MainActivity a) { return new java.io.File(a.baseDir(), "loot_verdicts.tsv"); }

    private static java.util.Map<String, String> categories(MainActivity a) {
        if (categories == null) {
            categories = new java.util.HashMap<>();
            java.io.File f = rated(a);
            if (f.exists())
                try (java.io.BufferedReader r = new java.io.BufferedReader(new java.io.FileReader(f))) {
                    for (String l; (l = r.readLine()) != null; ) {
                        String[] c = l.split("\t", -1);
                        if (c.length >= 5 && c[1].equals("category")) categories.put(c[2], c[4]);
                        if (c.length >= 5 && c[1].equals("action")) categories.put("action/" + c[2], c[4]);
                    }
                } catch (java.io.IOException ignored) {
                }
        }
        return categories;
    }

    /** its category: as rated, else the hand-picked ones by their list and the found things another weapon */
    String category(MainActivity a, String id) {
        String c = categories(a).get(id);
        return c != null ? c : found(id) ? OTHER : sword ? SWORD : SHIELD;
    }

    /** which of the game's six carried weapons an Other thing acts as (0 stick .. 5 Phantom Ganon's sword; rated like the category) */
    static int action(MainActivity a, String id) {
        String v = categories(a).get("action/" + id);
        try {
            return v == null ? 0 : Integer.parseInt(v);
        } catch (NumberFormatException e) {
            return 0;
        }
    }

    static void setAction(MainActivity a, String id, String name, int value) {
        categories(a).put("action/" + id, String.valueOf(value));
        String line = String.join("\t", new java.text.SimpleDateFormat("yyyy-MM-dd HH:mm:ss", java.util.Locale.ROOT).format(new java.util.Date()), "action", id, name, String.valueOf(value), "") + "\n";
        try (java.io.FileWriter w = new java.io.FileWriter(rated(a), true)) {
            w.write(line);
        } catch (java.io.IOException ignored) {
        }
    }

    static void setCategory(MainActivity a, String id, String name, String value) {
        categories(a).put(id, value);
        String line = String.join("\t", new java.text.SimpleDateFormat("yyyy-MM-dd HH:mm:ss", java.util.Locale.ROOT).format(new java.util.Date()), "category", id, name, value, "") + "\n";
        try (java.io.FileWriter w = new java.io.FileWriter(rated(a), true)) {
            w.write(line);
        } catch (java.io.IOException ignored) {
        }
    }

    // the collection, per save file: what it has won (quests give nothing yet: LootMenu's test list adds to it)
    private String prefOwned(int file) { return "loot_owned_" + word() + "_" + file; }

    java.util.Set<String> owned(MainActivity a, int file) {
        return new java.util.HashSet<>(a.prefs.getStringSet(prefOwned(file), java.util.Collections.emptySet()));
    }

    boolean owns(MainActivity a, int file, String id) { return owned(a, file).contains(id); }

    void setOwned(MainActivity a, int file, String id, boolean on) {
        java.util.Set<String> set = owned(a, file);
        if (on) set.add(id);
        else set.remove(id);
        a.prefs.edit().putStringSet(prefOwned(file), set).apply();
    }

    /** a new game in this file: nothing collected */
    static void clearCollection(MainActivity a, int file) {
        android.content.SharedPreferences.Editor e = a.prefs.edit();
        for (Loot L : new Loot[] {SWORDS, SHIELDS, HELMETS, CHESTS, OUTFITS}) e.remove(L.prefOwned(file));
        e.apply();
        for (String key : new java.util.ArrayList<>(a.prefs.getAll().keySet()))
            if (key.startsWith("loot_w_" + file + "_") || key.startsWith("loot_broken_" + file + "_")) e.remove(key);
        e.apply();
        LootQuick.clear(a, file);
        LootWorld.clear(a, file);
    }

    // rarity (0 common .. 3 legendary), the same for a thing every time: the found things by their name, the named ones above them
    private static final int[] USES = {40, 80, 150, 300}, REPAIR = {20, 50, 100, 200};

    int rarity(String id) {
        if (!found(id)) return sword && power(id) >= 150 || armor == 2 ? 3 : 2;
        int h = id.hashCode() % 100;
        if (h < 0) h += 100;
        return h < 70 ? 0 : h < 93 ? 1 : 2;
    }

    int uses(String id) { return USES[rarity(id)]; }
    int repairCost(String id) { return REPAIR[rarity(id)]; }

    // wear (hits landed) and broken, per save file
    private String prefWear(int file, String id) { return "loot_w_" + file + "_" + word() + "_" + id; }
    private String prefBroken(int file, String id) { return "loot_broken_" + file + "_" + word() + "_" + id; }
    int wear(MainActivity a, int file, String id) { return a.prefs.getInt(prefWear(file, id), 0); }
    void setWear(MainActivity a, int file, String id, int n) { a.prefs.edit().putInt(prefWear(file, id), n).apply(); }
    boolean broken(MainActivity a, int file, String id) { return a.prefs.getBoolean(prefBroken(file, id), false); }

    void setBroken(MainActivity a, int file, String id, boolean on) {
        android.content.SharedPreferences.Editor e = a.prefs.edit();
        if (on) e.putBoolean(prefBroken(file, id), true);
        else e.remove(prefBroken(file, id)).remove(prefWear(file, id));
        e.apply();
    }

    // the runtime's list ("id\tname\tpower" rows), read once (about a thousand: the found things too)
    private String[][] rows;

    private String[][] rows() {
        if (rows == null) {
            String[] r = armor >= 0 ? Native.lootArmor(armor) : sword ? Native.lootSwords() : Native.lootShields();
            String[][] out = new String[r == null ? 0 : r.length][];
            for (int i = 0; i < out.length; i++) out[i] = r[i].split("\t");
            rows = out;
        }
        return rows;
    }

    /** a found thing (every other model of the game that can be held; runtime hd_sword_found.inc) */
    static boolean found(String id) { return id != null && id.length() == 11 && id.charAt(0) == 'f'; }

    /** the ones there are: ids (the chosen ones first, then the found things) */
    java.util.List<String> ids() {
        java.util.List<String> l = new java.util.ArrayList<>();
        for (String[] f : rows()) l.add(f[0]);
        return l;
    }

    boolean has(String id) {
        for (String[] f : rows())
            if (f[0].equals(id)) return true;
        return false;
    }

    /** a sword's strength in percent of the game's sword (0: none; shields: 100) */
    int power(String id) {
        for (String[] f : rows())
            if (f.length >= 3 && f[0].equals(id)) return Integer.parseInt(f[2]);
        return 0;
    }

    /** its name (the app's translation: loot_sword_<id> / loot_shield_<id>, a sword's for a shield too; else the runtime's) */
    String name(MainActivity a, String id) {
        if (id == null) return a.getString(sword ? R.string.loot_game_sword : armor >= 0 ? R.string.loot_no_armor : R.string.loot_game_shield);
        for (String key : sword ? new String[] {"loot_sword_"} : armor >= 0 ? new String[] {"loot_" + word() + "_"} : new String[] {"loot_shield_", "loot_sword_"}) {
            int res = a.getResources().getIdentifier(key + id, "string", a.getPackageName());
            if (res != 0) return a.getString(res);
        }
        for (String[] f : rows())
            if (f.length >= 2 && f[0].equals(id)) return f[1];
        return id;
    }
}
