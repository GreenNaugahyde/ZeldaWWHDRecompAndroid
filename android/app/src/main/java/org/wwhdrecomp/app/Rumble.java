package org.wwhdrecomp.app;

import android.content.Context;
import android.os.Build;
import android.os.Handler;
import android.os.Looper;
import android.os.VibrationEffect;
import android.os.Vibrator;
import android.os.VibratorManager;
import android.view.InputDevice;

import java.util.ArrayDeque;

/**
 * The GamePad's rumble on the controller in use, or on this device while playing by touch or with a
 * controller without rumble (Controls > Rumble may pick this device, a controller or both: MainActivity.rumbleController). The game sends patterns of up to one second (VPADControlMotor: one bit
 * per 1/120 s); like Cemu, up to five play one after another, in steps of 1/60 s. The Pro Controller's motor is just on or off.
 */
final class Rumble {
    private static final int QUEUE_MAX = 5;
    private static final long HOLD_MS = 10_000;  // "on" until switched off (renewed while on)

    private final Context context;
    private final Handler main = new Handler(Looper.getMainLooper());
    private final ArrayDeque<long[]> queue = new ArrayDeque<>();
    private boolean playing, holding;
    private InputDevice controller;  // in use, or null (touch)
    private boolean both;            // this device rumbles with the controller
    boolean enabled = true;

    Rumble(Context c) { context = c; }

    /** The controller that rumbles now (null: this device); both: this device as well. */
    void setController(InputDevice d, boolean both) {
        if (d == controller && both == this.both) return;
        stop();
        controller = d;
        this.both = both;
    }

    /** From the game: a pattern of `bits` steps (LSB first), or stop (bits == 0). Any thread. */
    void pattern(byte[] p, int bits) {
        long[] timings = bits > 0 ? timings(p, bits) : null;
        main.post(() -> {
            if (timings == null) { stop(); return; }
            if (!enabled || queue.size() >= QUEUE_MAX) return;
            queue.add(timings);
            if (!playing) playNext();
        });
    }

    /** From the game (Pro Controller): motor on or off. Any thread. */
    void hold(boolean on) {
        main.post(() -> {
            if (on == holding) return;
            holding = on;
            for (Vibrator v : vibrators()) {
                if (on && enabled) v.vibrate(VibrationEffect.createOneShot(HOLD_MS, VibrationEffect.DEFAULT_AMPLITUDE));
                else v.cancel();
            }
        });
    }

    void stop() {
        queue.clear();
        main.removeCallbacksAndMessages(null);
        playing = holding = false;
        for (Vibrator v : vibrators()) v.cancel();
    }

    private void playNext() {
        long[] t = queue.poll();
        if (t == null) { playing = false; return; }
        playing = true;
        long total = 0;
        for (long d : t) total += d;
        if (t.length > 1) for (Vibrator v : vibrators()) v.vibrate(VibrationEffect.createWaveform(t, -1));
        main.postDelayed(this::playNext, Math.max(total, 1));
    }

    // off/on durations in ms (starting with off), from 1/60 s steps: a step is on if either of its
    // two 1/120 s bits is
    private static long[] timings(byte[] p, int bits) {
        int steps = (bits + 1) / 2;
        java.util.ArrayList<Long> out = new java.util.ArrayList<>();
        boolean on = false;
        int run = 0;
        for (int s = 0; s < steps; s++) {
            int b = s * 2;
            boolean set = bit(p, b) || (b + 1 < bits && bit(p, b + 1));
            if (set != on) {
                out.add(ms(run));
                run = 0;
                on = set;
            }
            run++;
        }
        out.add(ms(run));
        long[] t = new long[out.size()];
        for (int i = 0; i < t.length; i++) t[i] = out.get(i);
        return t;
    }

    private static boolean bit(byte[] p, int i) { return (p[i >> 3] >> (i & 7) & 1) != 0; }

    private static long ms(int steps) { return Math.round(steps * 1000.0 / 60); }

    // the controller's motor if it has one (and this device's too with both), else this device's (playing by touch,
    // or a controller without rumble)
    private java.util.List<Vibrator> vibrators() {
        java.util.List<Vibrator> l = new java.util.ArrayList<>(2);
        if (controller != null) {
            Vibrator v = Build.VERSION.SDK_INT >= Build.VERSION_CODES.S ? controller.getVibratorManager().getDefaultVibrator()
                    : controller.getVibrator();
            if (v != null && v.hasVibrator()) l.add(v);
        }
        if (l.isEmpty() || both) {
            Vibrator v = deviceVibrator(context);
            if (v != null) l.add(v);
        }
        return l;
    }

    /** this device's motor, or null */
    static Vibrator deviceVibrator(Context c) {
        Vibrator v = Build.VERSION.SDK_INT >= Build.VERSION_CODES.S
                ? ((VibratorManager) c.getSystemService(Context.VIBRATOR_MANAGER_SERVICE)).getDefaultVibrator()
                : (Vibrator) c.getSystemService(Context.VIBRATOR_SERVICE);
        return v != null && v.hasVibrator() ? v : null;
    }
}
