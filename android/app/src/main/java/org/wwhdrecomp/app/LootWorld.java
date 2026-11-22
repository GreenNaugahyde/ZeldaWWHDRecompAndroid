package org.wwhdrecomp.app;

import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.RectF;
import android.os.SystemClock;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileReader;
import java.io.FileWriter;
import java.io.IOException;
import java.io.InputStreamReader;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Random;
import java.util.Set;

/**
 * Loot found in the world, per save file (loot_world.cpp): a defeated enemy may leave a thing, shown where it fell as its picture
 * and taken by walking over it or, by choice (Options), by a press beside it; the game's own chests stand in places put by hand (files/loot_chests.tsv, assets) and, by chance, near where a
 * room's enemies stand. Hits landed wear out the sword in his hand; at the end it breaks (Options: repairable, or gone for good).
 */
final class LootWorld {
    private static final int DROP_PERCENT = 12, AUTO_CHEST_PERCENT = 20;
    private static final long LIFE_MS = 45000, BLINK_MS = 8000, BANNER_MS = 3500, TAKE_AFTER_MS = 1200;
    private static final float PICK_NEAR = 110, PICK_NEAR_SEA = 260, PICK_HIGH = 180, ICON = 58;
    private static final int[] RARITY_COLOR = {0xFFE8E8E8, 0xFF58D858, 0xFF58A8FF, 0xFFFFC030};
    private static final int[] DROP_WEIGHT = {70, 22, 7, 1}, CHEST_WEIGHT = {0, 0, 80, 20};
    private static final Loot[] KINDS = {Loot.SWORDS, Loot.SHIELDS, Loot.HELMETS, Loot.CHESTS, Loot.OUTFITS};

    private static final class Drop {
        Loot kind;
        String id;
        float x, y, z;
        long until, takeFrom;
    }

    private final MainActivity a;
    private final Random random = new Random();
    private final List<Drop> drops = new ArrayList<>();
    private final float[] link = new float[3], p = new float[3];
    private float scene = -1, tick = -1;
    private int yaw;
    private boolean force;
    private volatile Drop inReach;
    private final RectF reachRect = new RectF();
    private Runnable pending;
    private String banner;
    private int bannerColor;
    private long bannerUntil;
    private boolean wasOn;
    private final Map<Integer, Boolean> known = new HashMap<>();
    private final Paint glow = new Paint(Paint.ANTI_ALIAS_FLAG), bmp = new Paint(Paint.ANTI_ALIAS_FLAG | Paint.FILTER_BITMAP_FLAG), pill = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final RectF box = new RectF();
    private GameDraw text;

    LootWorld(MainActivity a) { this.a = a; }

    private boolean on() { return a.prefs.getBoolean("loot_world", true); }

    private static String prefOpened(int file) { return "loot_chests_open_" + file; }

    static void clear(MainActivity a, int file) {
        android.content.SharedPreferences.Editor e = a.prefs.edit();
        e.remove(prefOpened(file));
        e.apply();
    }

    /** a few times a second in play */
    void poll() {
        final boolean want = BuildConfig.LOOT && on();
        if (want != wasOn) {
            Native.lootWorldOn(want);
            wasOn = want;
            if (!want) drops.clear();
        }
        final int file = PlayerModels.playedFile();
        if (!want || file <= 0) return;
        final float[] v = Native.lootWorldPoll();
        if (v == null || v.length < 7) return;
        if (v[0] != scene) {
            scene = v[0];
            tick = -1;
            drops.clear();
            redraw();
        }
        System.arraycopy(v, 2, link, 0, 3);
        if (v[5] > 0) wear(file, (int) v[5]);
        yaw = (int) v[6];
        for (int i = 7; i + 2 < v.length; i += 3)
            if (force || random.nextInt(100) < DROP_PERCENT) drop(file, v[i], v[i + 1], v[i + 2], DROP_WEIGHT);
        if (v.length > 7) force = false;
        if (v[1] != tick) {
            tick = v[1];
            placeChests(file);
        }
        for (int key : Native.lootWorldOpened()) chestOpened(file, key);
        if (pending != null && a.padInPlay()) {  // (a chest's find: once its scene is over)
            pending.run();
            pending = null;
        }
        nextSaid();
        final long now = SystemClock.uptimeMillis();
        final boolean sea = Native.lootWorldWhere().startsWith("sea\t");
        final boolean button = byButton();
        Drop closest = null;
        double closestAt = Double.MAX_VALUE;
        for (int i = drops.size() - 1; i >= 0; i--) {
            final Drop d = drops.get(i);
            final double near = Math.hypot(d.x - link[0], d.z - link[2]);
            if (now > d.until) {
                drops.remove(i);
                redraw();
            } else if (a.padInPlay() && now >= d.takeFrom && near < (sea ? PICK_NEAR_SEA : PICK_NEAR) && (sea || Math.abs(d.y - link[1]) < PICK_HIGH)) {
                if (button) {  // (by a press: the nearest one waits for it)
                    if (near < closestAt) {
                        closest = d;
                        closestAt = near;
                    }
                    continue;
                }
                drops.remove(i);
                take(file, d.kind, d.id, false);
            }
        }
        if (closest != inReach) {
            inReach = closest;
            redraw();
        }
    }

    /** the test panel: a Chuchu to defeat; it is sure to leave something */
    void testEnemy() {
        force = true;
        Native.lootWorldTestEnemy();
    }

    // one message at a time, the next when the last has been read
    private final java.util.ArrayDeque<Object[]> said = new java.util.ArrayDeque<>();

    private void say(String t, int color, int sound) {
        said.add(new Object[] {t, color, sound});
        nextSaid();
    }

    private void nextSaid() {
        final long now = SystemClock.uptimeMillis();
        if (now < bannerUntil || said.isEmpty()) return;
        final Object[] m = said.poll();
        banner = (String) m[0];
        bannerColor = (Integer) m[1];
        bannerUntil = now + BANNER_MS;
        MenuSounds.play(a, (Integer) m[2]);
        redraw();
    }

    private boolean byButton() { return a.prefs.getBoolean("loot_pick_button", false); }

    // ---- picked up by a press: A next to it (the press is ours, Link does nothing with it), or a tap on its picture
    private boolean aSwallowed, aWasDown;

    int filter(int bits) {
        if (!BuildConfig.LOOT) return bits;
        if ((bits & Native.A) == 0) {
            aSwallowed = aWasDown = false;
            return bits;
        }
        if (aSwallowed) return bits & ~Native.A;
        if (!aWasDown && inReach != null && a.padInPlay()) {
            aSwallowed = true;
            a.runOnUiThread(this::takeInReach);
            return bits & ~Native.A;
        }
        aWasDown = true;
        return bits;
    }

    private void takeInReach() {
        final Drop d = inReach;
        final int file = PlayerModels.playedFile();
        if (d == null || file <= 0 || !drops.remove(d)) return;
        inReach = null;
        take(file, d.kind, d.id, false);
        reachRect.setEmpty();
    }

    /** a finger coming down at x, y: true if it took the thing shown there */
    boolean tapped(float x, float y) {
        if (!BuildConfig.LOOT) return false;
        if (inReach == null || reachRect.isEmpty() || !reachRect.contains(x, y)) return false;
        takeInReach();
        return true;
    }

    private void redraw() {
        final ControlsView c = a.controlsView();
        if (c != null) c.postInvalidate();
    }

    // ---- what can be found: everything but what is rated Bad, not yet in the collection
    private Map<String, String> verdicts() {
        final Map<String, String> m = new HashMap<>();
        final File f = new File(a.baseDir(), "loot_verdicts.tsv");
        if (f.exists())
            try (BufferedReader r = new BufferedReader(new FileReader(f))) {
                for (String l; (l = r.readLine()) != null; ) {
                    final String[] c = l.split("\t", -1);
                    if (c.length >= 5) m.put(c[1] + "/" + c[2], c[4]);
                }
            } catch (IOException ignored) {
            }
        return m;
    }

    private Object[] roll(int file, int[] weights) {
        final Map<String, String> rated = verdicts();
        int total = 0;
        for (int w : weights) total += w;
        int pick = random.nextInt(total), rarity = 0;
        while (pick >= weights[rarity]) pick -= weights[rarity++];
        for (int r = rarity; r >= 0; r--) {  // (none left of that rarity: the next one down)
            final List<Object[]> pool = new ArrayList<>();
            for (Loot k : KINDS) {
                final Set<String> owned = k.owned(a, file);
                for (String id : k.ids()) {
                    if (owned.contains(id) || "bad".equals(rated.get(k.word() + "/" + id)) || k.rarity(id) != r) continue;
                    final boolean shield = Loot.SHIELD.equals(k.category(a, id));
                    if (k == Loot.SWORDS ? shield : k == Loot.SHIELDS && !shield) continue;
                    pool.add(new Object[] {k, id});
                }
            }
            if (!pool.isEmpty()) return pool.get(random.nextInt(pool.size()));
            if (weights[Math.max(0, r - 1)] == 0) break;
        }
        return null;
    }

    private void drop(int file, float x, float y, float z, int[] weights) {
        final Object[] r = roll(file, weights);
        if (r == null) return;
        final Drop d = new Drop();
        d.kind = (Loot) r[0];
        d.id = (String) r[1];
        d.x = x;
        d.y = y;
        d.z = z;
        d.until = SystemClock.uptimeMillis() + LIFE_MS;
        d.takeFrom = SystemClock.uptimeMillis() + TAKE_AFTER_MS;  // (seen before it is taken, also when the enemy fell at his feet)
        drops.add(d);
        redraw();
    }

    /** the test panel: one right in front of him (false: no file in play) */
    boolean testDrop() {
        final int file = PlayerModels.playedFile();
        if (file <= 0) return false;
        final double t = yaw * Math.PI / 32768;
        drop(file, link[0] + (float) Math.sin(t) * 70, link[1], link[2] + (float) Math.cos(t) * 70, DROP_WEIGHT);
        return true;
    }

    private void take(int file, Loot k, String id, boolean later) {
        k.setOwned(a, file, id, true);
        final Runnable show = () -> {
            final int r = k.rarity(id);
            say(a.getString(R.string.loot_got, k.name(a, id), a.getString(RARITY[r])), RARITY_COLOR[r], MenuSounds.ITEM_GET);
        };
        if (later) pending = show;
        else show.run();
    }

    private static final int[] RARITY = {R.string.loot_rarity_0, R.string.loot_rarity_1, R.string.loot_rarity_2, R.string.loot_rarity_3};

    // ---- wear: the sword in his hand (a thing used from X / Y / R is his sword meanwhile)
    private void wear(int file, int hits) {
        final String id = Loot.SWORDS.get(a, file);
        if (id == null || Loot.SWORDS.broken(a, file, id)) return;
        final int used = Loot.SWORDS.wear(a, file, id) + hits;
        if (used < Loot.SWORDS.uses(id)) {
            Loot.SWORDS.setWear(a, file, id, used);
            return;
        }
        final String name = Loot.SWORDS.name(a, id);
        if (a.lootQuick.activeIs(id)) Native.lootItemDrop();
        else Loot.SWORDS.set(a, file, null);
        if (a.prefs.getBoolean("loot_gone", false)) {
            Loot.SWORDS.setOwned(a, file, id, false);
            Loot.SWORDS.setWear(a, file, id, 0);
        } else Loot.SWORDS.setBroken(a, file, id, true);
        say(a.getString(R.string.loot_broke, name), 0xFFFF7060, MenuSounds.CANCEL);
    }

    // ---- chests
    private List<String[]> placed() {
        final List<String[]> out = new ArrayList<>();
        try (BufferedReader r = new BufferedReader(new InputStreamReader(a.getAssets().open("loot_chests.tsv")))) {
            read(r, out);
        } catch (IOException ignored) {
        }
        final File f = new File(a.baseDir(), "loot_chests.tsv");
        if (f.exists())
            try (BufferedReader r = new BufferedReader(new FileReader(f))) {
                read(r, out);
            } catch (IOException ignored) {
            }
        return out;
    }

    private static void read(BufferedReader r, List<String[]> out) throws IOException {
        for (String l; (l = r.readLine()) != null; ) {
            final String[] c = l.split("\t", -1);
            if (!l.startsWith("#") && c.length >= 6) out.add(c);
        }
    }

    private void placeChests(int file) {
        final String[] where = Native.lootWorldWhere().split("\t", -1);
        if (where.length < 2 || where[0].isEmpty()) return;
        final Set<String> opened = a.prefs.getStringSet(prefOpened(file), new HashSet<>());
        final List<int[]> ints = new ArrayList<>();
        final List<float[]> at = new ArrayList<>();
        for (String[] c : placed()) {
            if (!c[0].equals(where[0])) continue;
            add(ints, at, opened, c[0].hashCode() * 31 + String.join(",", c).hashCode(), Integer.parseInt(c[1]), Float.parseFloat(c[2]), Float.parseFloat(c[3]), Float.parseFloat(c[4]), Integer.parseInt(c[5]), 1);
        }
        final float[] spots = Native.lootWorldSpots();
        final int count = spots.length / 5;
        if (count > 0) {  // (at most one a room, the same one for this file every time)
            final int room = (int) spots[0];
            final Random r = new Random((where[0] + "/" + room + "/" + file).hashCode());
            if (r.nextInt(100) < AUTO_CHEST_PERCENT) {
                final int i = r.nextInt(count);
                final double angle = r.nextDouble() * Math.PI * 2;
                add(ints, at, opened, (where[0] + "/" + room + "/auto").hashCode(), room, spots[5 * i + 2] + (float) Math.sin(angle) * 130, spots[5 * i + 3] + 30, spots[5 * i + 4] + (float) Math.cos(angle) * 130, r.nextInt(65536), 1 << 8);
            }
        }
        final int n = ints.size();
        final int[] keys = new int[n], rooms = new int[n], yaw = new int[n], flags = new int[n];
        final float[] xyz = new float[3 * n];
        known.clear();
        for (int i = 0; i < n; i++) {
            final int[] v = ints.get(i);
            keys[i] = v[0];
            rooms[i] = v[1];
            yaw[i] = v[2];
            flags[i] = v[3];
            System.arraycopy(at.get(i), 0, xyz, 3 * i, 3);
            known.put(v[0], Boolean.TRUE);
        }
        Native.lootWorldChests(where[0], keys, rooms, xyz, yaw, flags);
    }

    private static void add(List<int[]> ints, List<float[]> at, Set<String> opened, int key, int room, float x, float y, float z, int yaw, int flags) {
        if (opened.contains(String.valueOf(key))) return;
        ints.add(new int[] {key, room, yaw, flags});
        at.add(new float[] {x, y, z});
    }

    private void chestOpened(int file, int key) {
        final Set<String> opened = new HashSet<>(a.prefs.getStringSet(prefOpened(file), new HashSet<>()));
        opened.add(String.valueOf(key));
        a.prefs.edit().putStringSet(prefOpened(file), opened).apply();
        final Object[] r = roll(file, CHEST_WEIGHT);
        if (r != null) take(file, (Loot) r[0], (String) r[1], true);
    }

    /** the test panel: a chest here, kept in files/loot_chests.tsv (false: no file in play) */
    boolean putChest() {
        final int file = PlayerModels.playedFile();
        final String[] where = Native.lootWorldWhere().split("\t", -1);
        if (file <= 0 || where.length < 2 || where[0].isEmpty()) return false;
        final double t = yaw * Math.PI / 32768;
        final String line = String.join("\t", where[0], where[1], String.valueOf(link[0] + (float) Math.sin(t) * 170), String.valueOf(link[1]), String.valueOf(link[2] + (float) Math.cos(t) * 170), String.valueOf((yaw + 0x8000) & 0xFFFF)) + "\n";
        try (FileWriter w = new FileWriter(new File(a.baseDir(), "loot_chests.tsv"), true)) {
            w.write(line);
        } catch (IOException e) {
            return false;
        }
        placeChests(file);
        return true;
    }

    // ---- on the screen
    /** drawn over the picture; true: to be drawn again next frame */
    boolean draw(Canvas c, ControlsView v, RectF tv, float k) {
        if (!BuildConfig.LOOT) return false;
        if (text == null) text = new GameDraw(v.getContext(), v::postInvalidate);
        final long now = SystemClock.uptimeMillis();
        final boolean moving = !drops.isEmpty();
        if (a.padInPlay()) {
            final float half = ICON / 2 * k, bob = (float) Math.sin(now / 300.0) * 8 * k;
            for (Drop d : drops) {
                if (d.until - now < BLINK_MS && (now / 200) % 2 == 0) continue;
                p[0] = d.x;
                p[1] = d.y + 45;
                p[2] = d.z;
                if (!Native.lootWorldProject(p)) continue;
                final float x = tv.centerX() + p[0] * k, y = tv.centerY() - p[1] * k + bob;
                glow.setColor(RARITY_COLOR[d.kind.rarity(d.id)] & 0x90FFFFFF);
                c.drawCircle(x, y, half * 1.15f, glow);
                final Bitmap b = LootIcons.get(a, d.kind, d.id, v::postInvalidate);
                if (b != null) {
                    box.set(x - half, y - half, x + half, y + half);
                    c.drawBitmap(b, null, box, bmp);
                }
                if (d == inReach) {
                    final float h = 30 * k, ty = y + half + h * 0.9f, w = text.msgWidth(a.getString(R.string.loot_pick_prompt), h) / 2 + 12 * k;
                    reachRect.set(x - Math.max(w, half * 1.6f), y - half * 1.6f, x + Math.max(w, half * 1.6f), ty + h);
                    pill.setColor(0xB0000000);
                    box.set(x - w, ty - h * 0.7f, x + w, ty + h * 0.7f);
                    c.drawRoundRect(box, 12 * k, 12 * k, pill);
                    text.msg(c, a.getString(R.string.loot_pick_prompt), x, ty, h, 0xFFFFFFFF, 0);
                }
            }
        }
        if (banner != null && now < bannerUntil) {
            final float h = 40 * k, x = tv.centerX(), y = tv.top + 70 * k, w = text.msgWidth(banner, h) / 2 + 24 * k;
            pill.setColor(0xB0000000);
            box.set(x - w, y - h * 0.75f, x + w, y + h * 0.75f);
            c.drawRoundRect(box, 18 * k, 18 * k, pill);
            text.msg(c, banner, x, y, h, bannerColor, 0);
            v.postInvalidateDelayed(bannerUntil - now);
        }
        return moving;
    }
}
