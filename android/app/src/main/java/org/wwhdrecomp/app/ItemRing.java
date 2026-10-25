package org.wwhdrecomp.app;

import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.RectF;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;

/**
 * The item ring (Controls > Item ring): X, Y or R held for a moment opens a ring of the items Link
 * owns; the direction picks one (the finger's drag from where the ring opened, or a controller's left
 * stick), and letting go of the button puts it on that button (Native.equipItem: as the game's item
 * screen, an item on another button swaps). A bag isn't put on a button (as in the game): resting on
 * it for DWELL_MS opens it as a ring of its contents, resting in the middle goes back. The Wind Waker
 * isn't put on the button either: Link conducts with it (Native.conductWith and a press of the button
 * after CONDUCT_DELAY_MS; the button's item comes back once he conducts). On the boat the ring also
 * has the cannon and the salvage arm (with the bombs / the grappling hook owned): picked, they press the
 * d-pad's left / right as the game's d-pad does, and go on no button. A button with an item also has an empty
 * entry (a struck-through circle): picked, the button is left empty. Meanwhile the game keeps running, runs in slow motion
 * or pauses (Controls > While the ring is open: pref item_ring_pace 0 / 1 / 2, Native.setGamePace).
 *
 * While it could open (in play, no event, pause menu closed, Link not aiming or using an item, no
 * command shown on the button), a press of X / Y / R is held back: let go before HOLD_MS it reaches
 * the game as a short press then; held longer it opens the ring and never reaches the game. While he
 * aims or uses one (bow / hookshot / grappling hook / boomerang aim, first person, telescope or Picto
 * Box view, conducting, Deku Leaf, sail) the buttons pass at once, so the press that shoots isn't
 * delayed; an item only held in his hand (the telescope after its view, a bomb) doesn't stop it. The controller's left stick doesn't reach the game while the ring is open.
 */
final class ItemRing {
    private static final int[] BITS = {Native.X, Native.Y, Native.R};
    private static final long HOLD_MS = 400, TAP_MS = 120, DWELL_MS = 350, CONDUCT_DELAY_MS = 150;
    private static final int WIND_WAKER = 0x22;
    // the boat's d-pad entries (slots below 0): cannon (left, needs the bombs), salvage arm (right, the grappling hook)
    private static final int SLOT_CANNON = -10, SLOT_SALVAGE = -11, ST0_SHIP = 0x10000;
    // the entry that empties the button (shown while it has an item; Native.equipItem with 0xFF)
    private static final int SLOT_EMPTY = -12;
    private long dpadFrom, dpadUntil;
    private int dpadBit;
    // the bags' inventory slots (spoils, bait, delivery) and their contents' slots on a button
    private static final int[] BAG_SLOTS = {4, 11, 18}, BAG_FIRST = {24, 36, 48};
    // Link aiming or using an item (mPlayerStatus words, touchState 12 / 13; GameCube daPyStts0/1):
    // bow 0x1000, first person 0x2000, hookshot 0x4000, grappling hook 0x20000, boomerang 0x80000,
    // telescope view 0x200000; conducting 0x1, Picto Box view 0x8, Deku Leaf flight 0x20 / fan 0x40, sail 0x400
    private static final int BUSY0 = 0x1000 | 0x2000 | 0x4000 | 0x20000 | 0x80000 | 0x200000;
    private static final int BUSY1 = 0x1 | 0x8 | 0x20 | 0x40 | 0x400;

    private final MainActivity a;
    private final ControlsView view;
    private final Handler h = new Handler(Looper.getMainLooper());
    private final boolean[] wasDown = new boolean[3], gated = new boolean[3], passNext = new boolean[3];
    private final long[] downAt = new long[3], tapFrom = new long[3], tapUntil = new long[3];

    // the open ring: its button (-1 closed), opened by touch (else a controller), the items in it
    private int open = -1;
    private boolean byTouch;
    private float originX, originY;
    private int[] slots = new int[0], items = new int[0];
    private int[] onButtons = new int[3];
    private int sel = -1;
    private int[] state;    // Native.itemRing() at the opening
    private boolean onShip;  // Link on the boat at the opening
    private int inBag = -1;  // the bag shown (0 spoils, 1 bait, 2 delivery), -1 the items

    ItemRing(MainActivity a, ControlsView view) {
        this.a = a;
        this.view = view;
    }

    boolean enabled() { return MainActivity.NEW_TOUCH_CONTROLS && a.prefs.getBoolean("item_ring", true); }

    /** the button held for the open ring (its press isn't moved to another button meanwhile) */
    boolean holds(int bit) { return open >= 0 && BITS[open] == bit; }

    /** the controller's left stick picks in the ring and doesn't reach the game */
    boolean takesLeftStick() { return open >= 0 && !byTouch; }

    /** button `bit` (X / Y / R) goes to the game as it is, held: its press now (held back so far) or its next one
     *  (ControlsView's tapped item buttons, latched while Link aims) doesn't open the ring */
    void passThrough(int bit) {
        for (int i = 0; i < 3; i++) {
            if (BITS[i] != bit || open == i) continue;
            if (gated[i]) gated[i] = false;
            else passNext[i] = true;
        }
    }

    /** the buttons for the game: X / Y / R held back or replaced by a short press (see above) */
    int filter(int bits, int touchBits, float lx, float ly) {
        long now = SystemClock.uptimeMillis();
        for (int i = 0; i < 3; i++) {
            boolean down = (bits & BITS[i]) != 0;
            if (down && !wasDown[i] && passNext[i]) passNext[i] = false;
            else if (down && !wasDown[i] && enabled() && eligible(i)) {
                gated[i] = true;
                downAt[i] = now;
                h.postDelayed(checkHold, HOLD_MS);
            }
            if (!down && wasDown[i] && gated[i]) {
                gated[i] = false;
                if (open == i) {
                    confirm();
                } else {  // a short press: now, for long enough that the game sees it
                    tap(i, now, 0);
                }
            }
            wasDown[i] = down;
            if (gated[i]) bits &= ~BITS[i];
            if (now >= tapFrom[i] && now < tapUntil[i]) bits |= BITS[i];
        }
        if (now >= dpadFrom && now < dpadUntil) bits |= dpadBit;
        if (open >= 0) {
            if (byTouch) {
                float[] f = view.fingerOn(BITS[open]);
                if (f != null) pick(f[0] - originX, f[1] - originY, 36 * a.getResources().getDisplayMetrics().density);
            } else {
                pick(lx, -ly, 0.5f);  // stick +y is up
            }
        }
        return bits;
    }

    // held long enough: open (or, if it can't open now, let the press through as a held press)
    private final Runnable checkHold = this::checkHold;

    private void checkHold() {
        long now = SystemClock.uptimeMillis();
        for (int i = 0; i < 3; i++) {
            if (!gated[i] || open >= 0 || now - downAt[i] < HOLD_MS - 5) continue;
            if (!openRing(i, (view.buttons() & BITS[i]) != 0)) {
                gated[i] = false;
                a.repushInput();
            }
        }
    }

    private boolean eligible(int i) {
        int[] r = Native.itemRing();
        if (r[0] == -1) return false;
        int[] s = Native.touchState();
        if (s[0] == 0 || s[8] != 0 || s[9] != 0) return false;  // outside play, an event, the pause menu
        if ((s[12] & BUSY0) != 0 || (s[13] & BUSY1) != 0) return false;  // aiming / using an item
        // a command on the button (album: Delete; -1: no command pane, as ControlsView.command)
        if (s.length > 20 + i && s[20 + i] > 0 && s[15 + i] != 0) return false;
        for (int k = 0; k < 21; k++) if (r[k] != 0xFF) return true;
        return false;
    }

    private boolean openRing(int i, boolean touch) {
        if (!eligible(i)) return false;
        int[] r = Native.itemRing();
        state = r;
        onShip = (Native.touchState()[12] & ST0_SHIP) != 0;
        onButtons = new int[] {r[21], r[22], r[23]};
        open = i;
        fillRing(-1);
        byTouch = touch;
        Native.setGamePace(a.prefs.getInt("item_ring_pace", 1));
        sel = -1;
        float[] f = touch ? view.fingerOn(BITS[i]) : null;
        originX = f != null ? f[0] : 0;
        originY = f != null ? f[1] : 0;
        view.invalidate();
        return true;
    }

    // the ring's entries: the inventory, or a bag's contents
    private void fillRing(int bag) {
        int[] r = state;
        int from = bag < 0 ? 0 : 25 + 8 * bag, count = bag < 0 ? 21 : 8;
        boolean cannon = false, salvage = false;
        for (int k = 0; bag < 0 && onShip && k < 21; k++) {
            cannon |= r[k] == 0x31;
            salvage |= r[k] == 0x25;
        }
        boolean empty = bag < 0 && open >= 0 && onButtons[open] != 0xFF;
        int n = (cannon ? 1 : 0) + (salvage ? 1 : 0) + (empty ? 1 : 0);
        for (int k = 0; k < count; k++) if (r[from + k] != 0xFF) n++;
        slots = new int[n];
        items = new int[n];
        n = 0;
        for (int k = 0; k < count; k++)
            if (r[from + k] != 0xFF) {
                slots[n] = bag < 0 ? k : BAG_FIRST[bag] + k;
                items[n++] = r[from + k];
            }
        if (cannon) slots[n++] = SLOT_CANNON;
        if (salvage) slots[n++] = SLOT_SALVAGE;
        if (empty) {
            slots[n] = SLOT_EMPTY;
            items[n++] = 0xFF;
        }
        inBag = bag;
        sel = -1;
        view.invalidate();
    }

    private int bagOf(int slot) {
        for (int b = 0; b < 3; b++) if (BAG_SLOTS[b] == slot) return b;
        return -1;
    }

    // resting on a bag opens it; resting in the middle of a bag goes back to the items
    private final Runnable dwell = this::dwell;
    private int dwellOn = -2;

    private void dwell() {
        if (open < 0 || sel != dwellOn) return;
        if (inBag < 0 && sel >= 0) {
            int b = bagOf(slots[sel]);
            boolean any = false;
            for (int k = 0; b >= 0 && k < 8; k++) any |= state[25 + 8 * b + k] != 0xFF;
            if (any) fillRing(b);
        } else if (inBag >= 0 && sel < 0) {
            fillRing(-1);
        }
    }

    // the item in the direction (vx, vy) (screen: +y down), none within the dead zone
    private void pick(float vx, float vy, float dead) {
        int was = sel;
        if (Math.hypot(vx, vy) < dead || items.length == 0) {
            sel = -1;
        } else {
            double ang = Math.atan2(vx, -vy);  // 0 at the top, clockwise
            if (ang < 0) ang += 2 * Math.PI;
            sel = (int) Math.round(ang / (2 * Math.PI / items.length)) % items.length;
        }
        if (sel != was) {
            view.invalidate();
            h.removeCallbacks(dwell);
            dwellOn = sel;
            if ((inBag < 0 && sel >= 0 && bagOf(slots[sel]) >= 0) || (inBag >= 0 && sel < 0)) h.postDelayed(dwell, DWELL_MS);
        }
    }

    // a short press of the button for the game, after delay ms
    private void tap(int i, long now, long delay) {
        tapFrom[i] = now + delay;
        tapUntil[i] = now + delay + TAP_MS;
        h.postDelayed(a::repushInput, delay);
        h.postDelayed(a::repushInput, delay + TAP_MS + 10);
    }

    private void confirm() {
        if (open >= 0 && sel >= 0 && slots[sel] == SLOT_EMPTY) {
            Native.equipItem(open, 0xFF);
        } else if (open >= 0 && sel >= 0 && slots[sel] < 0) {  // the boat's cannon / salvage arm: the d-pad's press
            long now = SystemClock.uptimeMillis();
            dpadBit = slots[sel] == SLOT_CANNON ? Native.LEFT : Native.RIGHT;
            dpadFrom = now;
            dpadUntil = now + TAP_MS;
            h.post(a::repushInput);
            h.postDelayed(a::repushInput, TAP_MS + 10);
        } else if (open >= 0 && sel >= 0 && items[sel] == WIND_WAKER) {  // conducts: its button, else the ring's for one press
            long now = SystemClock.uptimeMillis();
            int on = -1;
            for (int b = 0; b < 3; b++) if (onButtons[b] == slots[sel]) on = b;
            if (on >= 0) {
                tap(on, now, 0);
            } else {
                Native.conductWith(open, slots[sel]);
                tap(open, now, CONDUCT_DELAY_MS);
            }
        } else if (open >= 0 && sel >= 0 && onButtons[open] != slots[sel] && bagOf(slots[sel]) < 0) {
            Native.equipItem(open, slots[sel]);  // (a bag itself goes on no button: it opens)
        }
        close();
    }

    /** closed without a choice (the app goes to the background: the game must run for its save) */
    void cancel() {
        if (open < 0) return;
        for (int i = 0; i < 3; i++) gated[i] = false;
        close();
    }

    private void close() {
        Native.setGamePace(0);
        h.removeCallbacks(dwell);
        inBag = -1;
        open = -1;
        sel = -1;
        view.invalidate();
    }

    // ---- drawing (ControlsView.onDraw): the items around the screen's centre, the picked one larger
    private final Paint dim = new Paint(Paint.ANTI_ALIAS_FLAG), ring = new Paint(Paint.ANTI_ALIAS_FLAG),
            glow = new Paint(Paint.ANTI_ALIAS_FLAG), icon = new Paint(Paint.FILTER_BITMAP_FLAG | Paint.ANTI_ALIAS_FLAG),
            label = new Paint(Paint.ANTI_ALIAS_FLAG), badge = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final RectF dst = new RectF();

    void draw(Canvas c) {
        if (open < 0) return;
        float w = view.getWidth(), hgt = view.getHeight(), m = Math.min(w, hgt);
        float cx = w / 2, cy = hgt / 2, radius = m * 0.34f;
        int n = items.length;
        float slot = n > 0 ? (float) (2 * Math.PI * radius / n) : radius;
        float size = Math.min(m * 0.13f, slot * 0.82f);
        dim.setColor(0x99000000);
        c.drawCircle(cx, cy, radius + size * 0.9f, dim);
        ring.setStyle(Paint.Style.STROKE);
        ring.setStrokeWidth(m * 0.006f);
        ring.setColor(0x66FFFFFF);
        c.drawCircle(cx, cy, radius, ring);
        // the button being set, in the middle, with the picked item above it
        label.setColor(0xFFFFFFFF);
        label.setTextAlign(Paint.Align.CENTER);
        label.setFakeBoldText(true);
        label.setTextSize(m * 0.09f);
        c.drawText(view.slotName(open), cx, cy + m * 0.03f, label);
        if (inBag >= 0) {  // the open bag above the button's letter
            Bitmap bag = GameIcons.get(view.getContext(), GameIcons.itemTexture(state[BAG_SLOTS[inBag]]), view::postInvalidate);
            float bs = m * 0.09f;
            dst.set(cx - bs / 2, cy - m * 0.07f - bs, cx + bs / 2, cy - m * 0.07f);
            if (bag != null) c.drawBitmap(bag, null, dst, icon);
        }
        for (int k = 0; k < n; k++) {
            double ang = 2 * Math.PI * k / n;
            float ix = cx + (float) Math.sin(ang) * radius, iy = cy - (float) Math.cos(ang) * radius;
            float s = k == sel ? size * 1.35f : size;
            if (k == sel) {
                glow.setColor(0xCCFFE680);
                c.drawCircle(ix, iy, s * 0.62f, glow);
            }
            if (slots[k] == SLOT_EMPTY) {  // an empty circle, struck through
                ring.setColor(0xFFFFFFFF);
                ring.setStrokeWidth(s * 0.07f);
                c.drawCircle(ix, iy, s * 0.32f, ring);
                c.drawLine(ix - s * 0.22f, iy + s * 0.22f, ix + s * 0.22f, iy - s * 0.22f, ring);
                continue;
            }
            String tex = slots[k] == SLOT_CANNON ? "IconCannon_00^q" : slots[k] == SLOT_SALVAGE ? "IconSalvage_00^q" : GameIcons.itemTexture(items[k]);
            Bitmap b = GameIcons.get(view.getContext(), tex, view::postInvalidate);
            dst.set(ix - s / 2, iy - s / 2, ix + s / 2, iy + s / 2);
            if (b != null) {
                c.drawBitmap(b, null, dst, icon);
            } else {
                badge.setColor(0xFFFFFFFF);
                c.drawCircle(ix, iy, s * 0.3f, badge);
            }
            for (int btn = 0; btn < 3; btn++)  // the buttons it is on now
                if (onButtons[btn] == slots[k]) {
                    float bx = ix + s * 0.36f, by = iy + s * 0.36f, br = s * 0.2f;
                    badge.setColor(btn == open ? 0xFFFFC040 : 0xFF404040);
                    c.drawCircle(bx, by, br, badge);
                    label.setTextSize(br * 1.3f);
                    c.drawText(view.slotName(btn), bx, by + br * 0.45f, label);
                }
        }
    }
}
