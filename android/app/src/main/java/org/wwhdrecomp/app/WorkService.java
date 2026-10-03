package org.wwhdrecomp.app;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ServiceInfo;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.os.SystemClock;
import android.util.Log;

import java.util.concurrent.Callable;

/**
 * Keeps the app running while the game is extracted or its code compiled (several minutes): a
 * foreground service with a progress notification, so the work continues when the app is in the
 * background. The work itself runs on a thread of {@link Work}; the service only shows it.
 */
public final class WorkService extends Service {
    private static final String CHANNEL = "work";
    private static final int ID = 1;
    private final Handler handler = new Handler(Looper.getMainLooper());
    private final Runnable update = this::update;

    /** The one long job that may run (extraction or compile), shared by every MainActivity instance. */
    static final class Work {
        static final int EXTRACT = 1, COMPILE = 2;
        private static int running;                  // EXTRACT, COMPILE or 0
        private static long startedAt;               // SystemClock.elapsedRealtime()
        private static volatile boolean cancelled;
        private static int finished;                 // a finished job not yet shown by an activity
        private static String finishedErr;
        private static boolean finishedCancelled;

        static synchronized int running() { return running; }

        static long elapsedSeconds() { return (SystemClock.elapsedRealtime() - startedAt) / 1000; }

        static void markCancelled() { cancelled = true; }

        /** Starts `job` (returns null or an error) unless a job is running; the result goes to MainActivity.workFinished. */
        static synchronized boolean start(Context ctx, int kind, Callable<String> job) {
            if (running != 0) return false;
            running = kind;
            cancelled = false;
            finished = 0;
            startedAt = SystemClock.elapsedRealtime();
            Context app = ctx.getApplicationContext();
            try {
                app.startForegroundService(new Intent(app, WorkService.class));
            } catch (RuntimeException e) {  // e.g. not allowed from the background: the work still runs
                Log.w("wwhd", "work notification", e);
            }
            new Thread(() -> {
                String err;
                try {
                    err = job.call();
                } catch (Exception e) {
                    err = String.valueOf(e);
                }
                final String result = err;
                new Handler(Looper.getMainLooper()).post(() -> {
                    synchronized (Work.class) {
                        finished = running;
                        finishedErr = result;
                        finishedCancelled = cancelled;
                        running = 0;
                    }
                    MainActivity a = MainActivity.instance;
                    if (a != null && !a.isDestroyed()) takeFinished(a);
                });
            }, kind == EXTRACT ? "extract" : "compile").start();
            return true;
        }

        /** Shows a finished job's result in `a` (once); false if there is none. */
        static boolean takeFinished(MainActivity a) {
            int kind;
            String err;
            boolean wasCancelled;
            synchronized (Work.class) {
                if (finished == 0) return false;
                kind = finished;
                err = finishedErr;
                wasCancelled = finishedCancelled;
                finished = 0;
            }
            a.workFinished(kind, err, wasCancelled);
            return true;
        }
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        NotificationManager nm = getSystemService(NotificationManager.class);
        nm.createNotificationChannel(new NotificationChannel(CHANNEL, getString(R.string.work_channel), NotificationManager.IMPORTANCE_LOW));
        startForeground(ID, build(), ServiceInfo.FOREGROUND_SERVICE_TYPE_DATA_SYNC);
        handler.removeCallbacks(update);
        handler.postDelayed(update, 1000);
        return START_NOT_STICKY;
    }

    private void update() {
        if (Work.running() == 0) {
            stopForeground(STOP_FOREGROUND_REMOVE);
            stopSelf();
            return;
        }
        getSystemService(NotificationManager.class).notify(ID, build());
        handler.postDelayed(update, 1000);
    }

    private Notification build() {
        boolean extract = Work.running() == Work.EXTRACT;
        long[] p = extract ? Native.extractProgress() : Native.compileProgress();
        long s = Work.elapsedSeconds();
        String text = p[1] <= 0 ? "" : extract
                ? getString(R.string.extract_detail_short, p[0] / 1048576, p[1] / 1048576)
                : getString(R.string.compile_detail, p[0], p[1], s / 60, s % 60);
        Intent open = new Intent(this, MainActivity.class).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        PendingIntent pi = PendingIntent.getActivity(this, 0, open, PendingIntent.FLAG_IMMUTABLE | PendingIntent.FLAG_UPDATE_CURRENT);
        return new Notification.Builder(this, CHANNEL)
                .setSmallIcon(android.R.drawable.stat_notify_sync)
                .setContentTitle(getString(extract ? R.string.work_extracting : R.string.work_compiling))
                .setContentText(text)
                .setProgress(1000, p[1] > 0 ? (int) (p[0] * 1000 / p[1]) : 0, p[1] <= 0)
                .setOngoing(true)
                .setOnlyAlertOnce(true)
                .setContentIntent(pi)
                .build();
    }

    @Override
    public void onDestroy() {
        handler.removeCallbacks(update);
        super.onDestroy();
    }

    @Override
    public IBinder onBind(Intent intent) { return null; }
}
