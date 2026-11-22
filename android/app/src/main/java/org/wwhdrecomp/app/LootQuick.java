package org.wwhdrecomp.app;

/**
 * A loot weapon on X / Y / R (the + menu's Loot tab, LootMenu), per save file: in play that button swaps it on, pressed
 * again the game's own comes back. The game's item leaves the button when a weapon goes on it (the HUD shows it empty;
 * the touch button shows a sword or a shield). A game item put on the button in the game's own menu takes the place back
 * and ends the binding; anywhere else (a save loaded with its old buttons) the button is emptied again.
 * Button numbers as Native.equipItem: 0 X, 1 Y, 2 R.
 */
final class LootQuick {
    static final int BUTTONS = 3;
    private static final int[] BIT = {Native.X, Native.Y, Native.R};
    private static final Loot[] KINDS = {Loot.SWORDS, Loot.SHIELDS};
    private static final long SETTLE_MS = 1000;  // (the game takes the emptied button a moment later)

    private final MainActivity a;
    private int boundMask, held;
    private long boundAt;

    LootQuick(MainActivity a) { this.a = a; }

    private static String pref(int file, int button) { return "loot_quick_" + button + "_" + file; }

    /** the weapon on it: {kind's index, id}, null for none or one no longer in the collection */
    private String[] get(int file, int button) {
        String v = a.prefs.getString(pref(file, button), null);
        if (v == null || v.length() < 3) return null;
        String[] r = {v.substring(0, 1), v.substring(2)};
        return kind(r).owns(a, file, r[1]) && !kind(r).broken(a, file, r[1]) && Loot.OTHER.equals(kind(r).category(a, r[1])) ? r : null;
    }

    private static Loot kind(String[] v) { return KINDS["1".equals(v[0]) ? 1 : 0]; }

    /** its name for the Loot tab's slot, null for none */
    String name(int file, int button) {
        String[] v = get(file, button);
        return v == null ? null : kind(v).name(a, v[1]);
    }

    boolean holds(int file, int button, Loot k, String id) {
        String[] v = get(file, button);
        return v != null && kind(v) == k && v[1].equals(id);
    }

    /** the weapon on the button (the game's item leaves it), or null: nothing on it */
    void set(int file, int button, Loot k, String id) {
        if (id == null) a.prefs.edit().remove(pref(file, button)).apply();
        else {
            a.prefs.edit().putString(pref(file, button), (k == Loot.SHIELDS ? "1/" : "0/") + id).apply();
            boundAt = android.os.SystemClock.uptimeMillis();
            Native.equipItem(button, 0xFF);
        }
        poll();
    }

    static void clear(MainActivity a, int file) {
        android.content.SharedPreferences.Editor e = a.prefs.edit();
        for (int b = 0; b < BUTTONS; b++) e.remove(pref(file, b));
        e.apply();
    }

    /** the buttons as the game should get them: those with a weapon are ours in play; press = swap */
    int filter(int bits) {
        if (!BuildConfig.LOOT) return bits;
        final int hit = bits & boundMask;
        if (hit == 0) {
            held = 0;
            return bits;
        }
        if (!a.padInPlay()) {
            held = hit;  // (no swap for a button already down when play comes back)
            return bits;
        }
        final int pressed = hit & ~held;
        held = hit;
        for (int i = 0; i < BUTTONS; i++)
            if ((pressed & BIT[i]) != 0) {
                final int button = i;
                a.runOnUiThread(() -> swap(button));
            }
        return bits & ~hit;
    }

    // the press: the thing is made into Link's sword models (so it shows in his hand), then the game's carried weapon is made for him to
    // pick up (Native.lootItemTake, A for him); once it has left his hands (thrown, let go, the press again) his sword is what it was
    private int active = -1;
    private String activeId, swordBefore;
    private boolean asked, pulsed, inHand;
    private long startedAt, askedAt, pulsedAt;
    private final android.os.Handler handler = new android.os.Handler(android.os.Looper.getMainLooper());
    private final Runnable tick = this::takeStep;

    private void swap(int button) {
        int file = PlayerModels.playedFile();
        String[] v = file > 0 ? get(file, button) : null;
        if (v == null) return;
        if (active == button) {
            Native.lootItemDrop();
            MenuSounds.play(a, MenuSounds.CANCEL);
            return;
        }
        if (active >= 0) return;
        active = button;
        activeId = v[1];
        swordBefore = Loot.SWORDS.get(a, file);
        asked = pulsed = inHand = false;
        startedAt = android.os.SystemClock.uptimeMillis();
        Loot.SWORDS.set(a, file, activeId);
        MenuSounds.play(a, MenuSounds.SWITCH);
        handler.postDelayed(tick, 100);
    }

    /** this thing is out of its button, in his hand or on its way there */
    boolean activeIs(String id) { return active >= 0 && id.equals(activeId); }

    private void takeStep() {
        if (active < 0) return;
        final long now = android.os.SystemClock.uptimeMillis();
        final int state = Native.lootItemState();
        if (!asked) {
            if (activeId.equals(Native.lootInMemory(false))) {
                Native.lootItemTake(Loot.action(a, activeId));
                asked = true;
                askedAt = now;
            } else if (now - startedAt > 8000) {
                finish();
                return;
            }
        } else if (state == 1 && !pulsed && now - askedAt > 350) {
            ControlsView c = a.controlsView();
            if (c != null) c.pulse(Native.A);  // (he picks it up as he would off the ground)
            pulsed = true;
            pulsedAt = now;
        } else if (state == 2) {
            inHand = true;
        } else if (state == 0 && (inHand || now - askedAt > 1500)) {
            finish();
            return;
        }
        handler.postDelayed(tick, 80);
    }

    private void finish() {
        final int file = PlayerModels.playedFile();
        if (file > 0) Loot.SWORDS.set(a, file, swordBefore != null && (Loot.SWORDS.broken(a, file, swordBefore)) ? null : swordBefore);
        active = -1;
    }

    /** a few times a second: the buttons with a weapon, their icons, a game item on one of them */
    void poll() {
        if (!BuildConfig.LOOT) return;
        int file = PlayerModels.playedFile(), mask = 0;
        int[] s = file > 0 ? Native.touchState() : null;
        boolean settled = android.os.SystemClock.uptimeMillis() - boundAt > SETTLE_MS;
        ControlsView c = a.controlsView();
        for (int i = 0; i < BUTTONS; i++) {
            String[] v = file > 0 ? get(file, i) : null;
            if (v != null && s != null && s.length > 5 && s[3 + i] >= 0 && s[3 + i] < 0xFF && settled) {
                if (a.drcScreenShown()) {  // (a game item dragged here: it wins)
                    a.prefs.edit().remove(pref(file, i)).apply();
                    v = null;
                } else if (a.padInPlay()) Native.equipItem(i, 0xFF);
            }
            if (v != null) mask |= BIT[i];
            if (c != null) {
                final android.graphics.Bitmap own = v == null ? null : LootIcons.get(a, kind(v), v[1], c::postInvalidate);
                c.setLootQuickIcon(i, v == null ? null : kind(v) == Loot.SHIELDS ? "CollectIcon118_04^l" : GameIcons.SWORD_DEFAULT, own);
            }
        }
        boundMask = mask;
        if (c != null) {  // what he wears: the sword and the shield the game's buttons show
            final String sword = file > 0 ? Loot.SWORDS.get(a, file) : null, shield = file > 0 ? Loot.SHIELDS.get(a, file) : null;
            c.setLootWorn(sword == null ? null : LootIcons.get(a, Loot.SWORDS, sword, c::postInvalidate),
                    shield == null ? null : LootIcons.get(a, Loot.SHIELDS, shield, c::postInvalidate));
            final android.graphics.Bitmap swordPicture = sword == null ? null : LootIcons.get(a, Loot.SWORDS, sword, c::postInvalidate);
            a.setLootBIcon(swordPicture != null && !a.hudHiddenForTouch());
            c.setLootHud(!a.hudHiddenForTouch(), Native.hudButtonPlaces(), s != null ? s : new int[0]);
        }
    }
}
