package org.wwhdrecomp.app;

import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;

/**
 * Game controllers and hardware keyboards, read as a Wii U GamePad. Controllers map by button
 * position by default (the bottom face button is the Wii U's B, as on macOS), and the user can
 * assign every Wii U button to another controller button (Controls › Controller buttons); the
 * keyboard layout matches the macOS build: WASD move, arrows camera, K/Space = A, J = B, L = X,
 * I = Y, Q/E = L/R, Left Shift = ZL, C = ZR, Enter = +, Tab = -, H = Home, 1-4 = D-pad, X/V =
 * stick clicks.
 */
final class InputMapper {
    int padButtons, keyButtons;

    // ---- controller button assignment: for each Wii U button, the Android key code of the
    // controller button that presses it (analog triggers count as L2 / R2, a hat D-pad as the D-pad)
    static final int[] WIIU = {Native.A, Native.B, Native.X, Native.Y, Native.L, Native.R, Native.ZL, Native.ZR,
            Native.MINUS, Native.PLUS, Native.STICK_L, Native.STICK_R, Native.UP, Native.DOWN, Native.LEFT, Native.RIGHT};
    static final int[] DEFAULT_MAP = {KeyEvent.KEYCODE_BUTTON_B, KeyEvent.KEYCODE_BUTTON_A, KeyEvent.KEYCODE_BUTTON_Y,
            KeyEvent.KEYCODE_BUTTON_X, KeyEvent.KEYCODE_BUTTON_L1, KeyEvent.KEYCODE_BUTTON_R1, KeyEvent.KEYCODE_BUTTON_L2,
            KeyEvent.KEYCODE_BUTTON_R2, KeyEvent.KEYCODE_BUTTON_SELECT, KeyEvent.KEYCODE_BUTTON_START,
            KeyEvent.KEYCODE_BUTTON_THUMBL, KeyEvent.KEYCODE_BUTTON_THUMBR, KeyEvent.KEYCODE_DPAD_UP,
            KeyEvent.KEYCODE_DPAD_DOWN, KeyEvent.KEYCODE_DPAD_LEFT, KeyEvent.KEYCODE_DPAD_RIGHT};
    final int[] map = DEFAULT_MAP.clone();
    private final java.util.Set<Integer> pressed = new java.util.HashSet<>();  // controller buttons held
    private int pulse;  // Wii U buttons held for a moment by the activity (a short press of Select)

    /** the stored assignment ("" or anything unreadable: the default) */
    void loadMap(String s) {
        System.arraycopy(DEFAULT_MAP, 0, map, 0, map.length);
        String[] f = s == null ? new String[0] : s.split(",");
        if (f.length != map.length) return;
        try {
            for (int i = 0; i < map.length; i++) map[i] = Integer.parseInt(f[i].trim());
        } catch (NumberFormatException e) {
            System.arraycopy(DEFAULT_MAP, 0, map, 0, map.length);
        }
    }

    String mapString() {
        StringBuilder b = new StringBuilder();
        for (int i = 0; i < map.length; i++) b.append(i == 0 ? "" : ",").append(map[i]);
        return b.toString();
    }

    boolean isDefaultMap() { return java.util.Arrays.equals(map, DEFAULT_MAP); }

    /** Wii U button i goes to controller button `code`; the button that had it gets i's old one */
    void assign(int i, int code) {
        for (int j = 0; j < map.length; j++)
            if (j != i && map[j] == code) map[j] = map[i];
        map[i] = code;
        recompute();
    }

    /** the Wii U buttons the controller button presses */
    int bitFor(int code) {
        if (code == KeyEvent.KEYCODE_BACK) code = KeyEvent.KEYCODE_BUTTON_SELECT;  // some controllers send BACK for View
        int bits = 0;
        for (int i = 0; i < map.length; i++)
            if (map[i] == code) bits |= WIIU[i];
        return bits;
    }

    void pulse(int bits, boolean on) {
        pulse = on ? (pulse | bits) : (pulse & ~bits);
        recompute();
    }

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

    /** a controller button's name, by position for the face buttons (their letters differ by brand) */
    static String buttonName(int code) {
        switch (code) {
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
            case KeyEvent.KEYCODE_Q: return Native.L;
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

    /** Sticks, triggers and hat switches of a controller; true if used. */
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
        if (hx != hatX || hy != hatY) {
            hatX = hx;
            hatY = hy;
            press(KeyEvent.KEYCODE_DPAD_LEFT, hx < -0.5f);
            press(KeyEvent.KEYCODE_DPAD_RIGHT, hx > 0.5f);
            press(KeyEvent.KEYCODE_DPAD_UP, hy < -0.5f);
            press(KeyEvent.KEYCODE_DPAD_DOWN, hy > 0.5f);
        }
        recompute();
        lastControllerInput = e.getEventTime();
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
