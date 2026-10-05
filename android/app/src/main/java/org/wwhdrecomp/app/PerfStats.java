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
 * (/proc/self/stat); GPU load is the GPU driver's busy counter (Adreno kgsl, Mali, MediaTek GED)
 * where readable;
 * temperatures come from the thermal zones whose names say cpu / gpu, and the battery from the
 * system's battery broadcast. Anything not readable on a device shows as "–".
 */
final class PerfStats {
    float gameFps, frameMs, frameMaxMs, shownFps, fgGpuMs;
    float avgFps = Float.NaN;  // since the overlay was switched on
    private float frames0 = -1, sec0;
    float cpuPercent = -1, gpuPercent = -1;
    float cpuTemp = Float.NaN, gpuTemp = Float.NaN, batteryTemp = Float.NaN;

    private final Context context;
    private final int cores = Runtime.getRuntime().availableProcessors();
    private final long ticksPerSec = 100;  // USER_HZ, 100 on Android
    private long lastCpuTicks = -1, lastWallMs;
    private List<String> cpuZones, gpuZones;

    PerfStats(Context context) { this.context = context; }

    /** the SoC's model as the system reports it (e.g. "QTI SM8150"), else the board name */
    static String socName() {
        String m = android.os.Build.VERSION.SDK_INT >= 31 ? android.os.Build.SOC_MODEL : "";
        if (m == null || m.isEmpty() || m.equals(android.os.Build.UNKNOWN)) return android.os.Build.BOARD;
        String maker = android.os.Build.SOC_MANUFACTURER;
        return maker == null || maker.isEmpty() || maker.equals(android.os.Build.UNKNOWN) ? m : maker + " " + m;
    }

    private static String gpu = "";
    /** the GPU's name without the trademark sign, "" until the renderer has started */
    static String gpuName() {
        if (gpu.isEmpty()) gpu = Native.gpuName().replace(" (TM)", "").replace("(TM)", "").trim();
        return gpu;
    }

    /** the average starts again with the next numbers */
    void resetAverage() {
        frames0 = -1;
        avgFps = Float.NaN;
    }

    void update() {
        float[] n = Native.perfStats();
        gameFps = n[0];
        frameMs = n[1];
        frameMaxMs = n[2];
        shownFps = n[3];
        fgGpuMs = n[4];
        if (frames0 < 0) {
            frames0 = n[5];
            sec0 = n[6];
        } else if (n[6] - sec0 >= 1) {
            avgFps = (n[5] - frames0) / (n[6] - sec0);
        }
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

    // GPU load files: Adreno's "busy total" counter, else the first number of a percentage file
    // (Mali: utilization of the kernel driver, MediaTek GED, Exynos). The first readable one is used.
    private static final String KGSL = "/sys/class/kgsl/kgsl-3d0/gpubusy";
    private static final String[] PERCENT_FILES = {
        "/sys/class/kgsl/kgsl-3d0/gpu_busy_percentage",
        "/sys/kernel/ged/hal/gpu_utilization",
        "/sys/class/misc/mali0/device/utilization",
        "/sys/kernel/gpu/gpu_busy",
        "/sys/class/devfreq/gpufreq/device/utilization",
    };
    private static final java.util.regex.Pattern NUMBER = java.util.regex.Pattern.compile("\\d+(\\.\\d+)?");
    private String gpuFile;  // chosen source; "" if none is readable

    private void readGpu() {
        if (gpuFile == null) {
            gpuFile = "";
            if (readLine(KGSL) != null) gpuFile = KGSL;
            else for (String p : PERCENT_FILES)
                if (!Float.isNaN(firstNumber(readLine(p)))) { gpuFile = p; break; }
        }
        if (gpuFile.isEmpty()) return;
        String s = readLine(gpuFile);
        if (s == null) return;
        if (gpuFile.equals(KGSL)) {
            String[] f = s.trim().split("\\s+");
            if (f.length < 2) return;
            long busy = Long.parseLong(f[0]), total = Long.parseLong(f[1]);
            if (total > 0) gpuPercent = 100f * busy / total;
        } else {
            float v = firstNumber(s);
            if (v >= 0 && v <= 100) gpuPercent = v;
        }
    }

    private static float firstNumber(String s) {
        if (s == null) return Float.NaN;
        java.util.regex.Matcher m = NUMBER.matcher(s);
        return m.find() ? Float.parseFloat(m.group()) : Float.NaN;
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
