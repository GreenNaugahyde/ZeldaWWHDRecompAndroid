package org.wwhdrecomp.app;

import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;

/**
 * Game controllers and hardware keyboards, read as a Wii U GamePad. Controllers map by button
 * position (the bottom face button is the Wii U's B, as on macOS); the keyboard layout matches the
 * macOS build: WASD move, arrows camera, K/Space = A, J = B, L = X, I = Y, Q/E = L/R,
 * Left Shift = ZL, C = ZR, Enter = +, Tab = -, H = Home, 1-4 = D-pad, X/V = stick clicks.
 */
final class InputMapper {
    int padButtons, keyButtons;
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

    private static int controllerBit(int code) {
        switch (code) {
            case KeyEvent.KEYCODE_BUTTON_A: return Native.B;   // bottom
            case KeyEvent.KEYCODE_BUTTON_B: return Native.A;   // right
            case KeyEvent.KEYCODE_BUTTON_X: return Native.Y;   // left
            case KeyEvent.KEYCODE_BUTTON_Y: return Native.X;   // top
            case KeyEvent.KEYCODE_BUTTON_L1: return Native.L;
            case KeyEvent.KEYCODE_BUTTON_R1: return Native.R;
            case KeyEvent.KEYCODE_BUTTON_L2: return Native.ZL;
            case KeyEvent.KEYCODE_BUTTON_R2: return Native.ZR;
            case KeyEvent.KEYCODE_BUTTON_START: return Native.PLUS;
            case KeyEvent.KEYCODE_BUTTON_SELECT: case KeyEvent.KEYCODE_BACK: return Native.MINUS;  // some send BACK for View
            case KeyEvent.KEYCODE_BUTTON_MODE: return Native.HOME;
            case KeyEvent.KEYCODE_BUTTON_THUMBL: return Native.STICK_L;
            case KeyEvent.KEYCODE_BUTTON_THUMBR: return Native.STICK_R;
            case KeyEvent.KEYCODE_DPAD_UP: return Native.UP;
            case KeyEvent.KEYCODE_DPAD_DOWN: return Native.DOWN;
            case KeyEvent.KEYCODE_DPAD_LEFT: return Native.LEFT;
            case KeyEvent.KEYCODE_DPAD_RIGHT: return Native.RIGHT;
            default: return 0;
        }
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
            int bit = controllerBit(code);
            if (bit == 0) return false;
            padButtons = down ? (padButtons | bit) : (padButtons & ~bit);
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
        // analog triggers (controllers without L2/R2 key events)
        float lt = Math.max(axis(e, dev, MotionEvent.AXIS_LTRIGGER), axis(e, dev, MotionEvent.AXIS_BRAKE));
        float rt = Math.max(axis(e, dev, MotionEvent.AXIS_RTRIGGER), axis(e, dev, MotionEvent.AXIS_GAS));
        if (dev.getMotionRange(MotionEvent.AXIS_LTRIGGER) != null || dev.getMotionRange(MotionEvent.AXIS_BRAKE) != null)
            padButtons = lt > 0.5f ? (padButtons | Native.ZL) : (padButtons & ~Native.ZL);
        if (dev.getMotionRange(MotionEvent.AXIS_RTRIGGER) != null || dev.getMotionRange(MotionEvent.AXIS_GAS) != null)
            padButtons = rt > 0.5f ? (padButtons | Native.ZR) : (padButtons & ~Native.ZR);
        // D-pad reported as a hat
        float hx = e.getAxisValue(MotionEvent.AXIS_HAT_X), hy = e.getAxisValue(MotionEvent.AXIS_HAT_Y);
        if (hx != hatX || hy != hatY) {
            hatX = hx;
            hatY = hy;
            padButtons &= ~(Native.UP | Native.DOWN | Native.LEFT | Native.RIGHT);
            if (hx < -0.5f) padButtons |= Native.LEFT;
            if (hx > 0.5f) padButtons |= Native.RIGHT;
            if (hy < -0.5f) padButtons |= Native.UP;
            if (hy > 0.5f) padButtons |= Native.DOWN;
        }
        lastControllerInput = e.getEventTime();
        return true;
    }

    float keyLX() { return (kD ? 1f : 0f) - (kA ? 1f : 0f); }
    float keyLY() { return (kW ? 1f : 0f) - (kS ? 1f : 0f); }
    float keyRX() { return (kRight ? 1f : 0f) - (kLeft ? 1f : 0f); }
    float keyRY() { return (kUp ? 1f : 0f) - (kDown ? 1f : 0f); }

    /** Drop everything held (the activity lost focus). */
    void reset() {
        padButtons = keyButtons = 0;
        lx = ly = rx = ry = 0;
        kW = kA = kS = kD = kUp = kDown = kLeft = kRight = false;
        hatX = hatY = 0;
    }
}
