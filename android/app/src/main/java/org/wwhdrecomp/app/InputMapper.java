package org.wwhdrecomp.app;

import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;

/**
 * Game controllers and hardware keyboards, read as a Wii U GamePad. Controllers map by button
 * position by default (the bottom face button is the Wii U's B, as on macOS), and the user can
 * assign every Wii U button to another controller button (Controls › Controller buttons), and so Jump
 * anywhere's and the moon jump's own inputs (L1 by default, with the Wii U's L); the
 * keyboard layout matches the macOS build: WASD move, arrows camera, K/Space = A, J = B, L = X,
 * I = Y, Q/E = L/R, Left Shift = ZL, C = ZR, Enter = +, Tab = -, H = Home, 1-4 = D-pad, X/V =
 * stick clicks.
 */
final class InputMapper {
    int padButtons, keyButtons;

    // ---- controller button assignment: for each Wii U button, the Android key code of the
    // controller button that presses it (analog triggers count as L2 / R2, a hat D-pad as the D-pad; 0: none,
    // an item button taken off for a jump), then the same for our own inputs (OWN_JUMP, OWN_MOON)
    static final int[] WIIU = {Native.A, Native.B, Native.X, Native.Y, Native.L, Native.R, Native.ZL, Native.ZR,
            Native.MINUS, Native.PLUS, Native.STICK_L, Native.STICK_R, Native.UP, Native.DOWN, Native.LEFT, Native.RIGHT,
            Native.JUMP, Native.MOON};
    static final int OWN_JUMP = 16, OWN_MOON = 17, WIIU_COUNT = 16;
    static final int[] DEFAULT_MAP = {KeyEvent.KEYCODE_BUTTON_B, KeyEvent.KEYCODE_BUTTON_A, KeyEvent.KEYCODE_BUTTON_Y,
            KeyEvent.KEYCODE_BUTTON_X, KeyEvent.KEYCODE_BUTTON_L1, KeyEvent.KEYCODE_BUTTON_R1, KeyEvent.KEYCODE_BUTTON_L2,
            KeyEvent.KEYCODE_BUTTON_R2, KeyEvent.KEYCODE_BUTTON_SELECT, KeyEvent.KEYCODE_BUTTON_START,
            KeyEvent.KEYCODE_BUTTON_THUMBL, KeyEvent.KEYCODE_BUTTON_THUMBR, KeyEvent.KEYCODE_DPAD_UP,
            KeyEvent.KEYCODE_DPAD_DOWN, KeyEvent.KEYCODE_DPAD_LEFT, KeyEvent.KEYCODE_DPAD_RIGHT,
            KeyEvent.KEYCODE_BUTTON_L1, KeyEvent.KEYCODE_BUTTON_L1};
    final int[] map = DEFAULT_MAP.clone();
    private final java.util.Set<Integer> pressed = new java.util.HashSet<>();  // controller buttons held
    private int pulse;  // Wii U buttons held for a moment by the activity (a short press of Select)

    /** the stored assignment ("" or anything unreadable: the default; one from before our own inputs: they on L1) */
    void loadMap(String s) {
        System.arraycopy(DEFAULT_MAP, 0, map, 0, map.length);
        String[] f = s == null ? new String[0] : s.split(",");
        if (f.length == map.length || f.length == WIIU_COUNT) {
            try {
                for (int i = 0; i < f.length; i++) map[i] = Integer.parseInt(f[i].trim());
            } catch (NumberFormatException e) {
                System.arraycopy(DEFAULT_MAP, 0, map, 0, map.length);
            }
        }
        recompute();  // (buttons held meanwhile: by the new map)
    }

    String mapString() {
        StringBuilder b = new StringBuilder();
        for (int i = 0; i < map.length; i++) b.append(i == 0 ? "" : ",").append(map[i]);
        return b.toString();
    }

    boolean isDefaultMap() { return java.util.Arrays.equals(map, DEFAULT_MAP); }

    /** Wii U button i goes to controller button `code`; the button that had it gets i's old one (i had none: a free
     *  one, else none) */
    void assign(int i, int code) {
        int old = map[i] != 0 ? map[i] : freeButton(code);
        for (int j = 0; j < map.length; j++)
            if (j != i && map[j] == code) map[j] = old;
        map[i] = code;
        recompute();
    }

    static boolean own(int i) { return i >= WIIU_COUNT; }

    /** a Wii U button the game doesn't read in play (L, checked 2026-10-08; d-pad down, by the user 2026-10-22): it
     *  may share a button with a jump */
    private static boolean freeInPlay(int i) { return WIIU[i] == Native.L || WIIU[i] == Native.DOWN; }

    /** the Wii U buttons (indices) on controller button `code` that our own input there would clash with */
    java.util.List<Integer> clashes(int code) {
        java.util.List<Integer> l = new java.util.ArrayList<>();
        for (int j = 0; j < WIIU_COUNT; j++)
            if (map[j] == code && !freeInPlay(j)) l.add(j);
        return l;
    }

    /** where clashes(code) move when our input i goes to `code`: i's old button, unless the other one of ours stays
     *  there, else a button with nothing on it in play; 0: none free */
    int moveTarget(int i, int code) {
        int old = map[i], other = i == OWN_JUMP ? OWN_MOON : OWN_JUMP;
        if (old != 0 && old != code && map[other] != old) return old;
        return freeButton(code);
    }

    /** a controller button (of the default ones) with neither our inputs nor a Wii U button used in play, but `except`; 0 */
    private int freeButton(int except) {
        for (int c : DEFAULT_MAP) {
            if (c == except) continue;
            boolean free = true;
            for (int j = 0; j < map.length && free; j++)
                if (map[j] == c && (own(j) || !freeInPlay(j))) free = false;
            if (free) return c;
        }
        return 0;
    }

    /** our input i (OWN_JUMP / OWN_MOON) goes to controller button `code`: the Wii U buttons there in play move to
     *  moveTarget (its free ones come over), or with `remove` press nothing any more */
    void assignOwn(int i, int code, boolean remove) {
        java.util.List<Integer> moving = clashes(code);
        int target = remove || moving.isEmpty() ? 0 : moveTarget(i, code);
        if (target != 0)
            for (int j = 0; j < WIIU_COUNT; j++)
                if (map[j] == target) map[j] = code;
        for (int j : moving) map[j] = target;
        map[i] = code;
        recompute();
    }

    // ---- L1 and d-pad down as an extra item button (Controller buttons page, the clash dialog, the setup's "to L"):
    // the item button X / Y / R moves onto it; L1 then no longer presses the Wii U's L (no page turns in the item
    // screen, no mic button for the songs), d-pad down stays d-pad down outside play (MainActivity.pushInput)
    static final int[] ITEM_INDEX = {2, 3, 5};  // X, Y, R in WIIU
    static final int[] EXTRA_ITEM_CODES = {KeyEvent.KEYCODE_BUTTON_L1, KeyEvent.KEYCODE_DPAD_DOWN};

    static boolean extraItemCode(int code) { return code == KeyEvent.KEYCODE_BUTTON_L1 || code == KeyEvent.KEYCODE_DPAD_DOWN; }

    /** the item button (WIIU index) on controller button `code`; -1: none */
    int itemOn(int code) {
        for (int j : ITEM_INDEX)
            if (map[j] == code) return j;
        return -1;
    }

    /** item button `item` (WIIU index; -1: none) on L1 / d-pad down `code`. The item leaves its button; our inputs on
     *  `code` go there (a swap), or with none to the other one of ours, else none. The item that was on `code` goes
     *  back to its own default button if that one has nothing for play, else it is taken off. L1 with an item drops
     *  the Wii U's L; without one it has it back (where nothing else has it) */
    void setExtraItem(int code, int item) {
        int prev = itemOn(code);
        if (prev == item) return;
        if (prev >= 0) {
            int home = DEFAULT_MAP[prev];
            map[prev] = home != code && !busyInPlay(home) ? home : 0;
        }
        if (item >= 0) {
            int old = map[item];
            map[item] = code;
            for (int j = OWN_JUMP; j <= OWN_MOON; j++) {
                if (map[j] != code) continue;
                int other = map[j == OWN_JUMP ? OWN_MOON : OWN_JUMP];
                map[j] = old != 0 ? old : other != code ? other : 0;
            }
            if (code == KeyEvent.KEYCODE_BUTTON_L1)
                for (int j = 0; j < WIIU_COUNT; j++)
                    if (WIIU[j] == Native.L && map[j] == code) map[j] = 0;
        } else if (code == KeyEvent.KEYCODE_BUTTON_L1) {
            for (int j = 0; j < WIIU_COUNT; j++)
                if (WIIU[j] == Native.L && map[j] == 0) map[j] = code;
        }
        recompute();
    }

    /** controller button `code` has one of our inputs or a Wii U button the game reads in play */
    private boolean busyInPlay(int code) {
        for (int j = 0; j < map.length; j++)
            if (map[j] == code && (own(j) || !freeInPlay(j))) return true;
        return false;
    }

    /** our input i goes to `code`, and the item buttons there in play to L1 / d-pad down `to` (setExtraItem): the
     *  first one, the others are taken off */
    void assignOwnMovingItem(int i, int code, int to) {
        java.util.List<Integer> moving = clashes(code);
        assignOwn(i, code, true);
        if (!moving.isEmpty()) setExtraItem(to, moving.get(0));
    }

    /** the item buttons (bits) held only through d-pad down: outside play it is d-pad down only */
    int downItemBits() {
        if (!pressed.contains(KeyEvent.KEYCODE_DPAD_DOWN)) return 0;
        int bits = bitFor(KeyEvent.KEYCODE_DPAD_DOWN) & (Native.X | Native.Y | Native.R);
        for (int code : pressed)
            if (code != KeyEvent.KEYCODE_DPAD_DOWN) bits &= ~bitFor(code);
        return bits;
    }

    // ---- the setup's presets (ControllerSetup): from the default map, the face buttons by position (as on the Wii U:
    // the bottom one is B) or by their printed letters (A is A); then Jump anywhere and the moon jump on L1 (Original), or
    // both on controller button `jump`, whose Wii U buttons in play go to L1 with the Wii U's L coming over (moveToL;
    // an item button: L1 becomes that item button, setExtraItem), or are taken off (only X / Y / R, with the item ring
    // d-pad up / left / right: removableOnly)
    static final int PRESET_ORIGINAL = 0, PRESET_TAKE_OFF = 1, PRESET_MOVE_TO_L = 2;

    void preset(boolean letters, int preset, int jump) {
        System.arraycopy(DEFAULT_MAP, 0, map, 0, map.length);
        if (letters) {
            map[0] = KeyEvent.KEYCODE_BUTTON_A;
            map[1] = KeyEvent.KEYCODE_BUTTON_B;
            map[2] = KeyEvent.KEYCODE_BUTTON_X;
            map[3] = KeyEvent.KEYCODE_BUTTON_Y;
        }
        if (preset != PRESET_ORIGINAL && jump != 0 && jump != KeyEvent.KEYCODE_BUTTON_L1) {
            java.util.List<Integer> moving = clashes(jump);
            if (preset == PRESET_MOVE_TO_L && itemsOnly(moving)) {  // an item button: L1 becomes that item button
                map[OWN_JUMP] = map[OWN_MOON] = jump;
                for (int j : moving) map[j] = 0;
                setExtraItem(KeyEvent.KEYCODE_BUTTON_L1, moving.get(0));
                return;
            }
            if (preset == PRESET_MOVE_TO_L && !moving.isEmpty())  // (a button free in play, d-pad down: nothing moves)
                for (int j = 0; j < WIIU_COUNT; j++)
                    if (map[j] == KeyEvent.KEYCODE_BUTTON_L1) map[j] = jump;
            for (int j : moving) map[j] = preset == PRESET_MOVE_TO_L ? KeyEvent.KEYCODE_BUTTON_L1 : 0;
            map[OWN_JUMP] = map[OWN_MOON] = jump;
        }
        recompute();
    }

    /** the Wii U buttons (indices) are all item buttons X / Y / R, and there is one */
    static boolean itemsOnly(java.util.List<Integer> l) {
        for (int j : l)
            if (!removable(j, false)) return false;
        return !l.isEmpty();
    }

    /** the Wii U buttons on controller button `code` can all be taken off (removable) and there is one */
    boolean removableOnly(int code, boolean ring) {
        java.util.List<Integer> l = clashes(code);
        for (int j : l)
            if (!removable(j, ring)) return false;
        return !l.isEmpty();
    }

    /** Wii U button i may lose its controller button: an item button (its item: the other two stay), or with the item
     *  ring on (ItemRing.enabled) d-pad up / left / right, whose Wind Waker, cannon and salvage arm the ring has */
    static boolean removable(int i, boolean ring) {
        int b = i < WIIU_COUNT ? WIIU[i] : 0;
        return b == Native.X || b == Native.Y || b == Native.R || ring && ringCovers(i);
    }

    static boolean ringCovers(int i) { return i < WIIU_COUNT && (WIIU[i] == Native.UP || WIIU[i] == Native.LEFT || WIIU[i] == Native.RIGHT); }

    /** the d-pad directions taken off that only the item ring stands in for */
    java.util.List<Integer> ringTakenOff() {
        java.util.List<Integer> l = new java.util.ArrayList<>();
        for (int j = 0; j < WIIU_COUNT; j++)
            if (ringCovers(j) && map[j] == 0) l.add(j);
        return l;
    }

    /** the face buttons go by their letters (A presses the Wii U's A), not by position */
    boolean byLetters() { return map[0] == KeyEvent.KEYCODE_BUTTON_A && map[1] == KeyEvent.KEYCODE_BUTTON_B; }

    /** the app's menus and dialogs: the controller button that selects (the Wii U's A: the right face button, or
     *  the A button with the Wii U's A on it, by letters; issue #61), and the one that goes back */
    static int menuSelect() {
        MainActivity a = MainActivity.instance;
        return a != null && a.inputMapper().map[0] == KeyEvent.KEYCODE_BUTTON_A ? KeyEvent.KEYCODE_BUTTON_A : KeyEvent.KEYCODE_BUTTON_B;
    }

    static int menuBack() { return menuSelect() == KeyEvent.KEYCODE_BUTTON_A ? KeyEvent.KEYCODE_BUTTON_B : KeyEvent.KEYCODE_BUTTON_A; }

    /** a PlayStation controller (Sony's vendor id, or its name): its face buttons have symbols, not letters */
    static boolean isSony(InputDevice d) {
        if (d == null) return false;
        String n = d.getName() == null ? "" : d.getName().toLowerCase(java.util.Locale.ROOT);
        return d.getVendorId() == 0x054C || n.contains("dualsense") || n.contains("dualshock") || n.contains("playstation");
    }

    /** a Nintendo controller: Android reads it by position, where its letters are the Wii U's */
    static boolean isNintendo(InputDevice d) { return d != null && d.getVendorId() == 0x057E; }

    /** the Wii U buttons the controller button presses */
    int bitFor(int code) {
        if (code == KeyEvent.KEYCODE_BACK) code = KeyEvent.KEYCODE_BUTTON_SELECT;  // some controllers send BACK for View
        if (code == 0) return 0;  // (0 in the map: none)
        int bits = 0;
        for (int i = 0; i < map.length; i++)
            if (map[i] == code) bits |= WIIU[i];
        return bits;
    }

    void pulse(int bits, boolean on) {
        pulse = on ? (pulse | bits) : (pulse & ~bits);
        recompute();
    }

    /** the buttons the activity holds for a moment (pulse), without the controller's */
    int pulseButtons() { return pulse; }

    private void press(int code, boolean down) {
        if (down) pressed.add(code);
        else pressed.remove(code);
    }

    private void recompute() {
        int b = pulse;
        for (int code : pressed) b |= bitFor(code);
        padButtons = b;
    }

    /** a controller button a user can assign (not Home / Guide, which opens the menu) */
    static boolean assignable(int code) {
        return code != KeyEvent.KEYCODE_BUTTON_MODE && code != KeyEvent.KEYCODE_HOME
                && (KeyEvent.isGamepadButton(code) || code == KeyEvent.KEYCODE_BACK || code == KeyEvent.KEYCODE_DPAD_UP
                        || code == KeyEvent.KEYCODE_DPAD_DOWN || code == KeyEvent.KEYCODE_DPAD_LEFT || code == KeyEvent.KEYCODE_DPAD_RIGHT);
    }

    /** the controller buttons an analog event holds down right now: triggers as L2 / R2, a hat as the D-pad */
    static java.util.List<Integer> analogButtons(MotionEvent e) {
        java.util.List<Integer> l = new java.util.ArrayList<>();
        if (Math.max(e.getAxisValue(MotionEvent.AXIS_LTRIGGER), e.getAxisValue(MotionEvent.AXIS_BRAKE)) > 0.5f) l.add(KeyEvent.KEYCODE_BUTTON_L2);
        if (Math.max(e.getAxisValue(MotionEvent.AXIS_RTRIGGER), e.getAxisValue(MotionEvent.AXIS_GAS)) > 0.5f) l.add(KeyEvent.KEYCODE_BUTTON_R2);
        float hx = e.getAxisValue(MotionEvent.AXIS_HAT_X), hy = e.getAxisValue(MotionEvent.AXIS_HAT_Y);
        if (hx < -0.5f) l.add(KeyEvent.KEYCODE_DPAD_LEFT);
        if (hx > 0.5f) l.add(KeyEvent.KEYCODE_DPAD_RIGHT);
        if (hy < -0.5f) l.add(KeyEvent.KEYCODE_DPAD_UP);
        if (hy > 0.5f) l.add(KeyEvent.KEYCODE_DPAD_DOWN);
        return l;
    }

    /** a short name for the game's button letters (its font: ASCII), the face buttons by position as on the Wii U;
     *  null: none */
    static String shortName(int code) {
        switch (code) {
            case KeyEvent.KEYCODE_BUTTON_A: return "B";
            case KeyEvent.KEYCODE_BUTTON_B: return "A";
            case KeyEvent.KEYCODE_BUTTON_X: return "Y";
            case KeyEvent.KEYCODE_BUTTON_Y: return "X";
            case KeyEvent.KEYCODE_BUTTON_L1: return "L";
            case KeyEvent.KEYCODE_BUTTON_R1: return "R";
            case KeyEvent.KEYCODE_BUTTON_L2: return "ZL";
            case KeyEvent.KEYCODE_BUTTON_R2: return "ZR";
            case KeyEvent.KEYCODE_BUTTON_SELECT: case KeyEvent.KEYCODE_BACK: return "-";
            case KeyEvent.KEYCODE_BUTTON_START: return "+";
            case KeyEvent.KEYCODE_BUTTON_THUMBL: return "L3";
            case KeyEvent.KEYCODE_BUTTON_THUMBR: return "R3";
            case KeyEvent.KEYCODE_DPAD_UP: return "^";
            case KeyEvent.KEYCODE_DPAD_DOWN: return "v";
            case KeyEvent.KEYCODE_DPAD_LEFT: return "<";
            case KeyEvent.KEYCODE_DPAD_RIGHT: return ">";
            case 0: return null;
            default: return "?";
        }
    }

    /** a controller button's name, by position for the face buttons (their letters differ by brand) */
    static String buttonName(int code) {
        switch (code) {
            case 0: return "—";
            case KeyEvent.KEYCODE_BUTTON_A: return "Bottom face button (A / Cross)";
            case KeyEvent.KEYCODE_BUTTON_B: return "Right face button (B / Circle)";
            case KeyEvent.KEYCODE_BUTTON_X: return "Left face button (X / Square)";
            case KeyEvent.KEYCODE_BUTTON_Y: return "Top face button (Y / Triangle)";
            case KeyEvent.KEYCODE_BUTTON_L1: return "L1 / LB";
            case KeyEvent.KEYCODE_BUTTON_R1: return "R1 / RB";
            case KeyEvent.KEYCODE_BUTTON_L2: return "L2 / LT";
            case KeyEvent.KEYCODE_BUTTON_R2: return "R2 / RT";
            case KeyEvent.KEYCODE_BUTTON_SELECT: case KeyEvent.KEYCODE_BACK: return "Select / View / Share";
            case KeyEvent.KEYCODE_BUTTON_START: return "Start / Menu / Options";
            case KeyEvent.KEYCODE_BUTTON_THUMBL: return "Left stick click";
            case KeyEvent.KEYCODE_BUTTON_THUMBR: return "Right stick click";
            case KeyEvent.KEYCODE_DPAD_UP: return "D-pad up";
            case KeyEvent.KEYCODE_DPAD_DOWN: return "D-pad down";
            case KeyEvent.KEYCODE_DPAD_LEFT: return "D-pad left";
            case KeyEvent.KEYCODE_DPAD_RIGHT: return "D-pad right";
            default: return KeyEvent.keyCodeToString(code).replace("KEYCODE_", "").replace('_', ' ');
        }
    }
    float lx, ly, rx, ry;          // controller sticks
    private boolean kW, kA, kS, kD, kUp, kDown, kLeft, kRight;
    private float hatX, hatY;
    long lastControllerInput;      // uptime ms, to hide the on-screen controls while a controller is used
    boolean motionActive;          // the last onMotion moved a stick past its dead zone, a trigger or the hat

    static boolean isController(InputDevice d) {
        if (d == null) return false;
        int s = d.getSources();
        return (s & InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD
                || (s & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK;
    }

    private static int keyboardBit(int code) {
        switch (code) {
            case KeyEvent.KEYCODE_K: case KeyEvent.KEYCODE_SPACE: return Native.A;
            case KeyEvent.KEYCODE_J: return Native.B;
            case KeyEvent.KEYCODE_L: return Native.X;
            case KeyEvent.KEYCODE_I: return Native.Y;
            case KeyEvent.KEYCODE_Q: return Native.L | Native.JUMP | Native.MOON;
            case KeyEvent.KEYCODE_E: return Native.R;
            case KeyEvent.KEYCODE_SHIFT_LEFT: return Native.ZL;
            case KeyEvent.KEYCODE_C: return Native.ZR;
            case KeyEvent.KEYCODE_ENTER: return Native.PLUS;
            case KeyEvent.KEYCODE_TAB: return Native.MINUS;
            case KeyEvent.KEYCODE_H: return Native.HOME;
            case KeyEvent.KEYCODE_1: return Native.UP;
            case KeyEvent.KEYCODE_2: return Native.DOWN;
            case KeyEvent.KEYCODE_3: return Native.LEFT;
            case KeyEvent.KEYCODE_4: return Native.RIGHT;
            case KeyEvent.KEYCODE_X: return Native.STICK_L;
            case KeyEvent.KEYCODE_V: return Native.STICK_R;
            default: return 0;
        }
    }

    /** True if the event was used. */
    boolean onKey(KeyEvent e) {
        boolean down = e.getAction() == KeyEvent.ACTION_DOWN;
        if (e.getAction() != KeyEvent.ACTION_DOWN && e.getAction() != KeyEvent.ACTION_UP) return false;
        int code = e.getKeyCode();
        InputDevice dev = e.getDevice();
        if (isController(dev) || KeyEvent.isGamepadButton(code)) {
            if (code == KeyEvent.KEYCODE_BACK) code = KeyEvent.KEYCODE_BUTTON_SELECT;
            if (bitFor(code) == 0) return false;
            press(code, down);
            recompute();
            lastControllerInput = e.getEventTime();
            return true;
        }
        switch (code) {
            case KeyEvent.KEYCODE_W: kW = down; return true;
            case KeyEvent.KEYCODE_A: kA = down; return true;
            case KeyEvent.KEYCODE_S: kS = down; return true;
            case KeyEvent.KEYCODE_D: kD = down; return true;
            case KeyEvent.KEYCODE_DPAD_UP: kUp = down; return true;
            case KeyEvent.KEYCODE_DPAD_DOWN: kDown = down; return true;
            case KeyEvent.KEYCODE_DPAD_LEFT: kLeft = down; return true;
            case KeyEvent.KEYCODE_DPAD_RIGHT: kRight = down; return true;
            default: break;
        }
        int bit = keyboardBit(code);
        if (bit == 0) return false;
        keyButtons = down ? (keyButtons | bit) : (keyButtons & ~bit);
        return true;
    }

    private static float axis(MotionEvent e, InputDevice dev, int axis) {
        InputDevice.MotionRange r = dev.getMotionRange(axis, e.getSource());
        if (r == null) return 0;
        float v = e.getAxisValue(axis);
        float flat = Math.max(r.getFlat(), 0.12f);
        if (Math.abs(v) < flat) return 0;
        return Math.max(-1f, Math.min(1f, (v - Math.signum(v) * flat) / (1f - flat)));
    }

    /** Sticks, triggers and hat switches of a controller; true if taken (motionActive: really used; sticks
     * resting on a table jitter around their centre and send events too) */
    boolean onMotion(MotionEvent e) {
        InputDevice dev = e.getDevice();
        if (!isController(dev) || e.getAction() != MotionEvent.ACTION_MOVE) return false;
        lx = axis(e, dev, MotionEvent.AXIS_X);
        ly = -axis(e, dev, MotionEvent.AXIS_Y);
        // right stick: Z/RZ on most controllers, RX/RY on some
        boolean zrz = dev.getMotionRange(MotionEvent.AXIS_Z) != null && dev.getMotionRange(MotionEvent.AXIS_RZ) != null;
        rx = zrz ? axis(e, dev, MotionEvent.AXIS_Z) : axis(e, dev, MotionEvent.AXIS_RX);
        ry = -(zrz ? axis(e, dev, MotionEvent.AXIS_RZ) : axis(e, dev, MotionEvent.AXIS_RY));
        // analog triggers (controllers without L2/R2 key events) as L2 / R2
        float lt = Math.max(axis(e, dev, MotionEvent.AXIS_LTRIGGER), axis(e, dev, MotionEvent.AXIS_BRAKE));
        float rt = Math.max(axis(e, dev, MotionEvent.AXIS_RTRIGGER), axis(e, dev, MotionEvent.AXIS_GAS));
        if (dev.getMotionRange(MotionEvent.AXIS_LTRIGGER) != null || dev.getMotionRange(MotionEvent.AXIS_BRAKE) != null)
            press(KeyEvent.KEYCODE_BUTTON_L2, lt > 0.5f);
        if (dev.getMotionRange(MotionEvent.AXIS_RTRIGGER) != null || dev.getMotionRange(MotionEvent.AXIS_GAS) != null)
            press(KeyEvent.KEYCODE_BUTTON_R2, rt > 0.5f);
        // D-pad reported as a hat, as the D-pad's buttons
        float hx = e.getAxisValue(MotionEvent.AXIS_HAT_X), hy = e.getAxisValue(MotionEvent.AXIS_HAT_Y);
        motionActive = lx != 0 || ly != 0 || rx != 0 || ry != 0 || lt > 0.5f || rt > 0.5f || hx != hatX || hy != hatY;
        if (hx != hatX || hy != hatY) {
            hatX = hx;
            hatY = hy;
            press(KeyEvent.KEYCODE_DPAD_LEFT, hx < -0.5f);
            press(KeyEvent.KEYCODE_DPAD_RIGHT, hx > 0.5f);
            press(KeyEvent.KEYCODE_DPAD_UP, hy < -0.5f);
            press(KeyEvent.KEYCODE_DPAD_DOWN, hy > 0.5f);
        }
        recompute();
        if (motionActive) lastControllerInput = e.getEventTime();
        return true;
    }

    float keyLX() { return (kD ? 1f : 0f) - (kA ? 1f : 0f); }
    float keyLY() { return (kW ? 1f : 0f) - (kS ? 1f : 0f); }
    float keyRX() { return (kRight ? 1f : 0f) - (kLeft ? 1f : 0f); }
    float keyRY() { return (kUp ? 1f : 0f) - (kDown ? 1f : 0f); }

    /** Drop everything held (the activity lost focus). */
    void reset() {
        padButtons = keyButtons = pulse = 0;
        pressed.clear();
        lx = ly = rx = ry = 0;
        kW = kA = kS = kD = kUp = kDown = kLeft = kRight = false;
        hatX = hatY = 0;
    }
}
