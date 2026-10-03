package org.wwhdrecomp.app;

import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.os.BatteryManager;
import android.os.SystemClock;

import java.io.BufferedReader;
import java.io.FileReader;
import java.util.ArrayList;
import java.util.List;

/**
 * Numbers for the performance overlay. Android lets apps read little of the system's state: the
 * whole-system CPU load (/proc/stat) is hidden, so CPU is this app's share of all cores
 * (/proc/self/stat); GPU load is the Adreno driver's busy counter (kgsl gpubusy) where readable;
 * temperatures come from the thermal zones whose names say cpu / gpu, and the battery from the
 * system's battery broadcast. Anything not readable on a device shows as "–".
 */
final class PerfStats {
    float gameFps, frameMs, frameMaxMs, shownFps, fgGpuMs;
    float cpuPercent = -1, gpuPercent = -1;
    float cpuTemp = Float.NaN, gpuTemp = Float.NaN, batteryTemp = Float.NaN;

    private final Context context;
    private final int cores = Runtime.getRuntime().availableProcessors();
    private final long ticksPerSec = 100;  // USER_HZ, 100 on Android
    private long lastCpuTicks = -1, lastWallMs;
    private List<String> cpuZones, gpuZones;

    PerfStats(Context context) { this.context = context; }

    void update() {
        float[] n = Native.perfStats();
        gameFps = n[0];
        frameMs = n[1];
        frameMaxMs = n[2];
        shownFps = n[3];
        fgGpuMs = n[4];
        readCpu();
        readGpu();
        if (cpuZones == null) findZones();
        cpuTemp = maxTemp(cpuZones);
        gpuTemp = maxTemp(gpuZones);
        readBattery();
    }

    // this process's CPU time (all threads) per wall time and core
    private void readCpu() {
        String s = readLine("/proc/self/stat");
        if (s == null) return;
        // fields after the command name in parentheses: utime and stime are the 12th and 13th
        String[] f = s.substring(s.lastIndexOf(')') + 2).split(" ");
        if (f.length < 13) return;
        long ticks = Long.parseLong(f[11]) + Long.parseLong(f[12]);
        long now = SystemClock.elapsedRealtime();
        if (lastCpuTicks >= 0 && now > lastWallMs)
            cpuPercent = 100f * (ticks - lastCpuTicks) * 1000f / ticksPerSec / (now - lastWallMs) / cores;
        lastCpuTicks = ticks;
        lastWallMs = now;
    }

    // "busy total" over the driver's last sampling window
    private void readGpu() {
        String s = readLine("/sys/class/kgsl/kgsl-3d0/gpubusy");
        if (s == null) return;
        String[] f = s.trim().split("\\s+");
        if (f.length < 2) return;
        long busy = Long.parseLong(f[0]), total = Long.parseLong(f[1]);
        if (total > 0) gpuPercent = 100f * busy / total;
    }

    private void findZones() {
        cpuZones = new ArrayList<>();
        gpuZones = new ArrayList<>();
        for (int i = 0; i < 128; i++) {
            String type = readLine("/sys/class/thermal/thermal_zone" + i + "/type");
            if (type == null) {
                if (i > 0 && readLine("/sys/class/thermal/thermal_zone" + (i + 1) + "/type") == null) break;
                continue;
            }
            String t = type.trim().toLowerCase();
            String path = "/sys/class/thermal/thermal_zone" + i + "/temp";
            if (t.startsWith("cpu") || t.contains("cpu-") || t.startsWith("cpuss") || t.contains("cluster")) cpuZones.add(path);
            else if (t.startsWith("gpu") || t.contains("gpuss") || t.contains("mali") || t.contains("g3d")) gpuZones.add(path);
        }
    }

    // hottest sensor of a group, °C (zones report millidegrees, a few report degrees)
    private static float maxTemp(List<String> zones) {
        float max = Float.NaN;
        for (String z : zones) {
            String s = readLine(z);
            if (s == null) continue;
            try {
                float v = Float.parseFloat(s.trim());
                if (Math.abs(v) > 1000) v /= 1000f;
                if (v > -30 && v < 150 && (Float.isNaN(max) || v > max)) max = v;
            } catch (NumberFormatException ignored) {
            }
        }
        return max;
    }

    private void readBattery() {
        Intent b = context.registerReceiver(null, new IntentFilter(Intent.ACTION_BATTERY_CHANGED));
        if (b == null) return;
        int t = b.getIntExtra(BatteryManager.EXTRA_TEMPERATURE, Integer.MIN_VALUE);  // tenths of °C
        if (t != Integer.MIN_VALUE) batteryTemp = t / 10f;
    }

    private static String readLine(String path) {
        try (BufferedReader r = new BufferedReader(new FileReader(path))) {
            return r.readLine();
        } catch (Exception e) {
            return null;
        }
    }
}
