package org.wwhdrecomp.app;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ServiceInfo;
import android.os.IBinder;
import android.util.Log;

/**
 * Keeps the GamePad / screen process (PadActivity, process :pad) connected while it is in the background: Android
 * 15+ cuts the network of a background app after a few seconds (Snapdragon 855, Android 16: blocked=APP_BACKGROUND, the
 * console saw "Connection reset" 5 s after the home screen). A foreground service of the connected-device kind
 * keeps it, with a quiet notification that brings the GamePad back. Runs only while connected.
 */
public final class PadService extends Service {
    private static final String TAG = "wwhd";
    private static final String CHANNEL = "pad";
    private static final int ID = 2;
    static final String EXTRA_NAME = "name", EXTRA_SCREEN = "screen";

    static void start(Context c, String consoleName, boolean screen) {
        try {
            c.startForegroundService(new Intent(c, PadService.class).putExtra(EXTRA_NAME, consoleName).putExtra(EXTRA_SCREEN, screen));
        } catch (RuntimeException e) {  // (from the background: not allowed; the connection then lasts while in front)
            Log.w(TAG, "GamePad: no foreground service: " + e);
        }
    }

    static void stop(Context c) { c.stopService(new Intent(c, PadService.class)); }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        String name = intent != null ? intent.getStringExtra(EXTRA_NAME) : null;
        boolean screen = intent != null && intent.getBooleanExtra(EXTRA_SCREEN, false);
        NotificationManager nm = getSystemService(NotificationManager.class);
        nm.createNotificationChannel(new NotificationChannel(CHANNEL, getString(R.string.pad_title), NotificationManager.IMPORTANCE_LOW));
        Intent open = new Intent(this, PadActivity.class).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_SINGLE_TOP);
        PendingIntent pi = PendingIntent.getActivity(this, 0, open, PendingIntent.FLAG_IMMUTABLE | PendingIntent.FLAG_UPDATE_CURRENT);
        Notification n = new Notification.Builder(this, CHANNEL)
                .setSmallIcon(android.R.drawable.stat_sys_data_bluetooth)
                .setContentTitle(getString(screen ? R.string.pad_screen_title : R.string.pad_title))
                .setContentText(name != null ? name : "")
                .setOngoing(true)
                .setOnlyAlertOnce(true)
                .setContentIntent(pi)
                .build();
        try {
            startForeground(ID, n, ServiceInfo.FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE);
        } catch (RuntimeException e) {
            Log.w(TAG, "GamePad: no foreground service: " + e);
            stopSelf();
        }
        return START_NOT_STICKY;
    }

    @Override
    public IBinder onBind(Intent intent) { return null; }
}
