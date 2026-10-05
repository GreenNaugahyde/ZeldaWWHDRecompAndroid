package org.wwhdrecomp.app;

import android.app.Activity;
import android.content.Context;
import android.hardware.Sensor;
import android.hardware.SensorEvent;
import android.hardware.SensorEventListener;
import android.hardware.SensorManager;
import android.view.Surface;

/**
 * Gyro aiming: the phone's gyroscope and accelerometer passed to the game as the GamePad's
 * (runtime/src/motion.cpp). Samples are turned from the phone's natural axes into the screen's as
 * the activity is rotated (x right, y up, z out of the screen), which is how the GamePad is held.
 */
final class Gyro implements SensorEventListener {
    private final Activity activity;
    private final SensorManager sm;
    private final Sensor gyro, accel;
    private final float[] acc = new float[3], v = new float[3];
    private boolean running;

    Gyro(Activity a) {
        activity = a;
        sm = (SensorManager) a.getSystemService(Context.SENSOR_SERVICE);
        gyro = sm != null ? sm.getDefaultSensor(Sensor.TYPE_GYROSCOPE) : null;
        accel = sm != null ? sm.getDefaultSensor(Sensor.TYPE_ACCELEROMETER) : null;
    }

    boolean available() { return gyro != null; }

    void start() {
        if (running || gyro == null) return;
        running = true;
        sm.registerListener(this, gyro, SensorManager.SENSOR_DELAY_GAME);
        if (accel != null) sm.registerListener(this, accel, SensorManager.SENSOR_DELAY_GAME);
    }

    void stop() {
        if (!running) return;
        running = false;
        sm.unregisterListener(this);
    }

    // phone axes -> screen axes for the current rotation ("One Screen Turn Deserves Another")
    private void toScreen(float[] in, float[] out) {
        int rot = activity.getDisplay() != null ? activity.getDisplay().getRotation() : Surface.ROTATION_0;
        switch (rot) {
            case Surface.ROTATION_90: out[0] = -in[1]; out[1] = in[0]; break;
            case Surface.ROTATION_180: out[0] = -in[0]; out[1] = -in[1]; break;
            case Surface.ROTATION_270: out[0] = in[1]; out[1] = -in[0]; break;
            default: out[0] = in[0]; out[1] = in[1]; break;
        }
        out[2] = in[2];
    }

    @Override
    public void onSensorChanged(SensorEvent e) {
        if (e.sensor.getType() == Sensor.TYPE_ACCELEROMETER) {
            toScreen(e.values, acc);
            return;
        }
        toScreen(e.values, v);
        Native.setMotion(v[0], v[1], v[2], acc[0], acc[1], acc[2], e.timestamp);
    }

    @Override
    public void onAccuracyChanged(Sensor s, int accuracy) {}
}
