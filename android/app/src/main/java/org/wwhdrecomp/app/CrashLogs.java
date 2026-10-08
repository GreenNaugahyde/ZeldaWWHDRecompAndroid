package org.wwhdrecomp.app;

import android.app.Activity;
import android.content.ClipData;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.net.Uri;
import android.os.Build;

import java.io.File;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

/**
 * The crash logs the native crash handler writes (runtime/src/main.cpp:
 * captures/tlozwwhd_VERSION_YYYYmmdd-HHMMSS.log in the app's files folder, which other apps can't
 * open): offered for sharing on the next start after a crash, and under About. Once the user picked
 * where to share a log, the app no longer lists it and deletes it at its next start (the receiving
 * app may read it only later, e.g. when an email is sent). Only the newest few are kept.
 */
final class CrashLogs {
    private static final int KEEP = 10;
    private static final String ACTION_SHARED = "org.wwhdrecomp.app.CRASH_LOGS_SHARED";

    private CrashLogs() {}

    static File dir(Context c) {
        File f = c.getExternalFilesDir(null);
        return new File(f != null ? f : c.getFilesDir(), "captures");
    }

    // also the names before 0.6 (crash-YYYYmmdd-HHMMSS.log)
    static boolean isLogName(String n) { return n.matches("(tlozwwhd_[0-9A-Za-z.-]+_|crash-)\\d{8}-\\d{6}\\.log"); }

    static String appVersion(Context c) {
        try {
            return c.getPackageManager().getPackageInfo(c.getPackageName(), 0).versionName;
        } catch (Exception e) {
            return "";
        }
    }

    private static java.util.Set<String> shared(SharedPreferences prefs) {
        return new java.util.HashSet<>(prefs.getStringSet("crash_shared", java.util.Collections.emptySet()));
    }

    /** At start: deletes the logs shared in an earlier run. */
    static void deleteShared(Context c, SharedPreferences prefs) {
        java.util.Set<String> names = shared(prefs);
        if (names.isEmpty()) return;
        File d = dir(c);
        for (String n : names)
            if (isLogName(n)) new File(d, n).delete();
        prefs.edit().remove("crash_shared").apply();
    }

    /** the logs not shared yet, newest first */
    static List<File> list(Context c, SharedPreferences prefs) {
        java.util.Set<String> gone = shared(prefs);
        File[] fs = dir(c).listFiles((d, n) -> isLogName(n) && !gone.contains(n));
        if (fs == null) return new ArrayList<>();
        Arrays.sort(fs, (x, y) -> Long.compare(y.lastModified(), x.lastModified()));
        List<File> out = new ArrayList<>(Arrays.asList(fs));
        while (out.size() > KEEP) out.remove(out.size() - 1).delete();
        return out;
    }

    /** After a crash: asks whether to share the newest log (once per log). */
    static void offerNew(Activity a, SharedPreferences prefs) {
        List<File> logs = list(a, prefs);
        if (logs.isEmpty()) return;
        File newest = logs.get(0);
        if (newest.lastModified() <= prefs.getLong("crash_seen_ms", 0)) return;
        prefs.edit().putLong("crash_seen_ms", newest.lastModified()).remove("crash_seen").apply();
        new GameDialog(a).title(R.string.crash_title).message(R.string.crash_text)
                .button(R.string.crash_not_now, null)
                .button(R.string.crash_share, () -> share(a, prefs, logs.subList(0, 1)))
                .show();
    }

    /** Android's share menu with these logs attached and the app and device in the text */
    static void share(Activity a, SharedPreferences prefs, List<File> logs) {
        if (logs.isEmpty()) return;
        ArrayList<Uri> uris = new ArrayList<>();
        ArrayList<String> names = new ArrayList<>();
        for (File f : logs) {
            uris.add(CrashLogProvider.uri(f));
            names.add(f.getName());
        }
        Intent send = new Intent(uris.size() == 1 ? Intent.ACTION_SEND : Intent.ACTION_SEND_MULTIPLE);
        send.setType("text/plain");
        if (uris.size() == 1) send.putExtra(Intent.EXTRA_STREAM, uris.get(0));
        else send.putParcelableArrayListExtra(Intent.EXTRA_STREAM, uris);
        ClipData clip = ClipData.newRawUri(logs.get(0).getName(), uris.get(0));
        for (int i = 1; i < uris.size(); i++) clip.addItem(new ClipData.Item(uris.get(i)));
        send.setClipData(clip);
        send.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
        send.putExtra(Intent.EXTRA_SUBJECT, a.getString(R.string.crash_subject));
        send.putExtra(Intent.EXTRA_TEXT, about(a));
        // told when the user picked a target (not when the menu is cancelled): then the logs count
        // as shared
        a.registerReceiver(new android.content.BroadcastReceiver() {
            @Override
            public void onReceive(Context c, Intent i) {
                java.util.Set<String> s = shared(prefs);
                s.addAll(names);
                prefs.edit().putStringSet("crash_shared", s).apply();
                try {
                    c.unregisterReceiver(this);
                } catch (IllegalArgumentException ignored) {
                }
            }
        }, new android.content.IntentFilter(ACTION_SHARED), Context.RECEIVER_NOT_EXPORTED);
        android.app.PendingIntent picked = android.app.PendingIntent.getBroadcast(a, 0,
                new Intent(ACTION_SHARED).setPackage(a.getPackageName()),
                android.app.PendingIntent.FLAG_UPDATE_CURRENT | android.app.PendingIntent.FLAG_MUTABLE);
        a.startActivity(Intent.createChooser(send, a.getString(R.string.crash_share), picked.getIntentSender()));
    }

    // settings that say nothing about a problem
    private static final java.util.Set<String> SKIP = new java.util.HashSet<>(Arrays.asList("perf_x", "perf_y", "crash_seen", "crash_seen_ms", "crash_shared", "asked_notifications"));

    /**
     * The crash log's "app" section (runtime/src/crash_info.h): app version, Android, device, screen,
     * memory, controllers by USB IDs, and the settings. No personal data: no names, accounts or
     * paths (a setting holding a path or URI is left out).
     */
    static void updateInfo(Activity a, SharedPreferences prefs) {
        StringBuilder s = new StringBuilder();
        s.append(about(a)).append(", ").append(Build.SUPPORTED_ABIS.length > 0 ? Build.SUPPORTED_ABIS[0] : "?").append('\n');
        android.app.ActivityManager am = (android.app.ActivityManager) a.getSystemService(Context.ACTIVITY_SERVICE);
        android.app.ActivityManager.MemoryInfo mi = new android.app.ActivityManager.MemoryInfo();
        if (am != null) am.getMemoryInfo(mi);
        android.view.Display d = a.getWindowManager().getDefaultDisplay();
        android.graphics.Point size = new android.graphics.Point();
        d.getRealSize(size);
        s.append(String.format(java.util.Locale.ROOT, "memory %.1f GB (%.1f GB free at start), screen %dx%d at %.0f Hz, %d display(s)\n",
                mi.totalMem / 1e9, mi.availMem / 1e9, size.x, size.y, d.getRefreshRate(),
                ((android.hardware.display.DisplayManager) a.getSystemService(Context.DISPLAY_SERVICE)).getDisplays().length));
        StringBuilder pads = new StringBuilder();
        for (int id : android.view.InputDevice.getDeviceIds()) {
            android.view.InputDevice dev = android.view.InputDevice.getDevice(id);
            if (dev == null || dev.isVirtual()) continue;
            int src = dev.getSources();
            if ((src & android.view.InputDevice.SOURCE_GAMEPAD) == android.view.InputDevice.SOURCE_GAMEPAD
                    || (src & android.view.InputDevice.SOURCE_JOYSTICK) == android.view.InputDevice.SOURCE_JOYSTICK)
                pads.append(String.format(java.util.Locale.ROOT, " %04x:%04x", dev.getVendorId(), dev.getProductId()));
        }
        s.append("controllers (USB vendor:product):").append(pads.length() > 0 ? pads : " none").append('\n');
        java.util.TreeMap<String, ?> all = new java.util.TreeMap<>(prefs.getAll());
        StringBuilder set = new StringBuilder("settings:");
        for (java.util.Map.Entry<String, ?> e : all.entrySet()) {
            String v = String.valueOf(e.getValue());
            if (SKIP.contains(e.getKey()) || v.contains("/") || v.contains(":")) continue;
            set.append(' ').append(e.getKey()).append('=').append(v);
        }
        s.append(set);
        Native.setCrashInfo("app", s.toString());
    }

    // what a bug report needs besides the log (which names the GPU and driver): the app version and
    // the device
    private static String about(Context c) {
        String version = "?";
        try {
            android.content.pm.PackageInfo pi = c.getPackageManager().getPackageInfo(c.getPackageName(), 0);
            version = pi.versionName + " (" + pi.getLongVersionCode() + ")";
        } catch (Exception ignored) {
        }
        if ((c.getApplicationInfo().flags & android.content.pm.ApplicationInfo.FLAG_DEBUGGABLE) != 0) version += " debug";
        String soc = Build.VERSION.SDK_INT >= 31 ? ", " + Build.SOC_MANUFACTURER + " " + Build.SOC_MODEL : "";
        return "App " + version + ", Android " + Build.VERSION.RELEASE + " (API " + Build.VERSION.SDK_INT + "), " + Build.MANUFACTURER + " "
                + Build.MODEL + soc;
    }
}
