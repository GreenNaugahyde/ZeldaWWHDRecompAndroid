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

    /**
     * After a crash, before the game starts: asks whether to share the newest log (once per log).
     * Returns true if it asked; `then` runs when the dialog closes (it starts the game).
     */
    static boolean offerNew(Activity a, SharedPreferences prefs, Runnable then) {
        List<File> logs = list(a, prefs);
        if (logs.isEmpty()) return false;
        File newest = logs.get(0);
        if (newest.lastModified() <= prefs.getLong("crash_seen_ms", 0)) return false;
        prefs.edit().putLong("crash_seen_ms", newest.lastModified()).remove("crash_seen").apply();
        GameDialog d = new GameDialog(a).title(R.string.crash_title).message(R.string.crash_text).cancelable(false)
                .button(R.string.crash_start_game, then)
                .button(R.string.crash_share, () -> {
                    share(a, prefs, logs.subList(0, 1));
                    then.run();
                });
        // a mod pack may be why the game doesn't start: start without them (their files stay)
        File base = a.getExternalFilesDir(null);
        if (base == null) base = a.getFilesDir();
        final File packsBase = base;
        if (ModPacks.countOn(packsBase, prefs) > 0)
            d.button(R.string.crash_without_packs, () -> {
                ModPacks.turnAllOff(packsBase, prefs);
                then.run();
            });
        d.show();
        return true;
    }

    /**
     * At start: if Android recorded that the last run ended unexpectedly while the game was in the
     * foreground (a crash our crash handler can't catch, such as one in the GPU driver's process,
     * running out of memory, or a watchdog), writes a log of it in the crash logs' place: the exit
     * reason with Android's description, the memory the app used, Android's trace for a hang, the
     * end of the previous run's log and the device context. Android 11 and newer.
     */
    static void recordExit(Activity a, SharedPreferences prefs) {
        if (Build.VERSION.SDK_INT < 30) return;
        try {
            android.app.ActivityManager am = (android.app.ActivityManager) a.getSystemService(Context.ACTIVITY_SERVICE);
            if (am == null) return;
            List<android.app.ApplicationExitInfo> exits = am.getHistoricalProcessExitReasons(a.getPackageName(), 0, 5);
            long seen = prefs.getLong("exit_seen_ms", 0);
            android.app.ApplicationExitInfo e = null;
            long newest = seen;
            for (android.app.ApplicationExitInfo x : exits) {
                newest = Math.max(newest, x.getTimestamp());
                if (x.getTimestamp() > seen && !isRestarter(x) && (e == null || x.getTimestamp() > e.getTimestamp())) e = x;
            }
            if (newest > seen) prefs.edit().putLong("exit_seen_ms", newest).apply();
            if (e == null || !unexpected(e) || restarted(e, exits)) return;
            // our crash handler wrote a log for this one already: the run's log lines go into it (the
            // handler can't read them; they show e.g. the GPU driver's own messages before an abort)
            File[] own = dir(a).listFiles((d, n) -> isLogName(n));
            if (own != null)
                for (File f : own)
                    if (Math.abs(f.lastModified() - e.getTimestamp()) < 120_000) {
                        appendRunLog(f, e);
                        return;
                    }
            writeExitLog(a, prefs, e);
        } catch (RuntimeException ignored) {
            // no exit record (some vendors): nothing to report
        }
    }

    // RestartActivity's own process (it ends itself once the game is launched again)
    private static boolean isRestarter(android.app.ApplicationExitInfo x) {
        String n = x.getProcessName();
        return n != null && n.endsWith(":restart");
    }

    // the game's process ended by RestartActivity (a restart after a settings change or "Delete
    // shader cache"): SIGKILL in the foreground, with the restarter's own exit seconds later
    private static boolean restarted(android.app.ApplicationExitInfo e, List<android.app.ApplicationExitInfo> exits) {
        if (e.getReason() != android.app.ApplicationExitInfo.REASON_SIGNALED) return false;
        for (android.app.ApplicationExitInfo x : exits)
            if (isRestarter(x) && x.getTimestamp() >= e.getTimestamp() && x.getTimestamp() - e.getTimestamp() < 10_000) return true;
        return false;
    }

    // a real problem in the foreground: not our own quit (exit) or restart (killed in the background),
    // not the user closing the app, not Android ending it in the background
    private static boolean unexpected(android.app.ApplicationExitInfo e) {
        if (e.getImportance() > android.app.ActivityManager.RunningAppProcessInfo.IMPORTANCE_PERCEPTIBLE) return false;
        switch (e.getReason()) {
            case android.app.ApplicationExitInfo.REASON_CRASH:
            case android.app.ApplicationExitInfo.REASON_CRASH_NATIVE:
            case android.app.ApplicationExitInfo.REASON_ANR:
            case android.app.ApplicationExitInfo.REASON_LOW_MEMORY:
            case android.app.ApplicationExitInfo.REASON_EXCESSIVE_RESOURCE_USAGE:
            case android.app.ApplicationExitInfo.REASON_INITIALIZATION_FAILURE:
            case android.app.ApplicationExitInfo.REASON_SIGNALED:
            case android.app.ApplicationExitInfo.REASON_OTHER:
            case android.app.ApplicationExitInfo.REASON_UNKNOWN:
                return true;
            default:
                return false;
        }
    }

    private static final String[] REASONS = {"unknown", "exit (self)", "signaled", "low memory", "crash (Java)", "crash (native)", "not responding (ANR)",
            "initialization failure", "permission change", "excessive resource use", "user requested", "user stopped", "dependency died", "other",
            "freezer", "package state change", "package updated"};

    private static void appendRunLog(File f, android.app.ApplicationExitInfo e) {
        String s = "\n  [log of the run]\n" + logcat("--pid=" + e.getPid(), 1500) + "  [crash buffer]\n" + logcat("-b crash", 200);
        try (java.io.FileOutputStream out = new java.io.FileOutputStream(f, true)) {
            out.write(s.getBytes(java.nio.charset.StandardCharsets.UTF_8));
        } catch (java.io.IOException ignored) {
        }
    }

    private static void writeExitLog(Activity a, SharedPreferences prefs, android.app.ApplicationExitInfo e) {
        StringBuilder s = new StringBuilder();
        int r = e.getReason();
        s.append("EXIT: ").append(r >= 0 && r < REASONS.length ? REASONS[r] : "reason " + r).append(" (status ").append(e.getStatus()).append(")\n");
        s.append("  description: ").append(e.getDescription() != null ? e.getDescription() : "-").append('\n');
        s.append("  when: ").append(new java.text.SimpleDateFormat("yyyy-MM-dd HH:mm:ss", java.util.Locale.US).format(new java.util.Date(e.getTimestamp())))
                .append(", importance ").append(e.getImportance()).append(", PSS ").append(e.getPss() / 1024).append(" MB, RSS ")
                .append(e.getRss() / 1024).append(" MB\n");
        s.append("  [app]\n    ").append(appInfo(a, prefs).replace("\n", "\n    ")).append('\n');
        if (r == android.app.ApplicationExitInfo.REASON_ANR) {
            s.append("  [trace]\n");
            try (java.io.InputStream in = e.getTraceInputStream()) {
                if (in != null) s.append(text(in, 200_000));
            } catch (java.io.IOException ignored) {
            }
        }
        s.append("  [log of the previous run]\n").append(logcat("--pid=" + e.getPid(), 1500));
        s.append("  [crash buffer]\n").append(logcat("-b crash", 200));
        String when = new java.text.SimpleDateFormat("yyyyMMdd-HHmmss", java.util.Locale.US).format(new java.util.Date(e.getTimestamp()));
        File f = new File(dir(a), "tlozwwhd_" + appVersion(a) + "_" + when + ".log");
        //noinspection ResultOfMethodCallIgnored
        f.getParentFile().mkdirs();
        try (java.io.FileOutputStream out = new java.io.FileOutputStream(f)) {
            out.write(s.toString().getBytes(java.nio.charset.StandardCharsets.UTF_8));
        } catch (java.io.IOException ignored) {
        }
    }

    // the app's own log lines (Android lets an app read only those), the last `max`; paths cut to their last part
    private static String logcat(String filter, int max) {
        java.util.ArrayDeque<String> lines = new java.util.ArrayDeque<>();
        try {
            List<String> cmd = new ArrayList<>(Arrays.asList("logcat", "-d", "-v", "time"));
            cmd.addAll(Arrays.asList(filter.split(" ")));
            Process p = new ProcessBuilder(cmd).redirectErrorStream(true).start();
            try (java.io.BufferedReader rd = new java.io.BufferedReader(new java.io.InputStreamReader(p.getInputStream(),
                    java.nio.charset.StandardCharsets.UTF_8))) {
                for (String l; (l = rd.readLine()) != null; ) {
                    lines.add("    " + l.replaceAll("(/[^\\s/:,;)\"']+)+/([^\\s/:,;)\"']+)", ".../$2"));
                    if (lines.size() > max) lines.removeFirst();
                }
            }
        } catch (java.io.IOException ex) {
            lines.add("    logcat failed: " + ex.getMessage());
        }
        return String.join("\n", lines) + "\n";
    }

    private static String text(java.io.InputStream in, int max) throws java.io.IOException {
        byte[] b = new byte[max];
        int n = 0;
        for (int k; n < max && (k = in.read(b, n, max - n)) > 0; ) n += k;
        return new String(b, 0, n, java.nio.charset.StandardCharsets.UTF_8);
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
    private static final java.util.Set<String> SKIP = new java.util.HashSet<>(Arrays.asList("perf_x", "perf_y", "crash_seen", "crash_seen_ms", "crash_shared", "exit_seen_ms", "asked_notifications"));

    /**
     * The crash log's "app" section (runtime/src/crash_info.h): app version, Android, device, screen,
     * memory, controllers by USB IDs, and the settings. No personal data: no names, accounts or
     * paths (a setting holding a path or URI is left out).
     */
    static void updateInfo(Activity a, SharedPreferences prefs) { Native.setCrashInfo("app", appInfo(a, prefs)); }

    static String appInfo(Activity a, SharedPreferences prefs) {
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
        return s.toString();
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
