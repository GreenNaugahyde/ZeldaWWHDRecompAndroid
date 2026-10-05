package org.wwhdrecomp.app;

import android.app.Activity;
import android.hardware.Sensor;
import android.hardware.SensorEvent;
import android.hardware.SensorEventListener;
import android.hardware.SensorManager;
import android.os.Build;
import android.os.Handler;
import android.os.HandlerThread;
import android.view.InputDevice;
import android.view.Surface;

/**
 * The GamePad's motion sensors (gyro aiming) from this device's gyroscope and accelerometer, or
 * from a controller's (Android 12+, e.g. DualSense, DualShock 4, Switch Pro) while one is in use.
 * Samples go to the runtime (runtime/src/motion.cpp) in SDL's controller axes: held flat, +x right,
 * +y up out of the face, +z toward you. The device counts as a GamePad held like the tablet: its
 * screen is the GamePad's screen.
 */
final class MotionInput implements SensorEventListener {
    private final Activity activity;
    private HandlerThread thread;
    private Handler handler;
    private SensorManager sensors;   // the source's
    private boolean fromController;
    private InputDevice controller;  // the source, if a controller
    private boolean running;
    // latest accelerometer sample (source axes, m/s²) and the last gyro time
    private final float[] acc = new float[3];
    private boolean haveAcc;
    private long lastGyroNs;

    MotionInput(Activity a) { activity = a; }

    /** Starts reading: the controller's sensors if it has them, else the device's. */
    void start(InputDevice usedController) {
        InputDevice c = usedController != null && hasMotion(usedController) ? usedController : null;
        if (running && c == controller) return;
        stop();
        if (thread == null) {
            thread = new HandlerThread("wwhd-motion");
            thread.start();
            handler = new Handler(thread.getLooper());
        }
        controller = c;
        fromController = c != null;
        sensors = fromController ? controllerSensors(c) : (SensorManager) activity.getSystemService(Activity.SENSOR_SERVICE);
        if (sensors == null) return;
        Sensor g = sensors.getDefaultSensor(Sensor.TYPE_GYROSCOPE), a = sensors.getDefaultSensor(Sensor.TYPE_ACCELEROMETER);
        if (g == null || a == null) return;
        haveAcc = false;
        lastGyroNs = 0;
        if ((activity.getApplicationInfo().flags & android.content.pm.ApplicationInfo.FLAG_DEBUGGABLE) != 0)
            android.util.Log.d("wwhd", "motion from " + (fromController ? c.getName() : "this device") + ": " + g.getName());
        sensors.registerListener(this, a, SensorManager.SENSOR_DELAY_GAME, handler);
        sensors.registerListener(this, g, SensorManager.SENSOR_DELAY_GAME, handler);
        running = true;
    }

    /** the sensors in use are a controller's */
    boolean fromController() { return running && fromController; }

    void stop() {
        if (!running) return;
        running = false;
        if (sensors != null) sensors.unregisterListener(this);
        Native.motionReset();
    }

    static boolean hasMotion(InputDevice d) {
        SensorManager m = controllerSensors(d);
        return m != null && m.getDefaultSensor(Sensor.TYPE_GYROSCOPE) != null && m.getDefaultSensor(Sensor.TYPE_ACCELEROMETER) != null;
    }

    private static SensorManager controllerSensors(InputDevice d) {
        if (d == null || Build.VERSION.SDK_INT < Build.VERSION_CODES.S) return null;
        return d.getSensorManager();
    }

    @Override
    public void onSensorChanged(SensorEvent e) {
        if (e.sensor.getType() == Sensor.TYPE_ACCELEROMETER) {
            System.arraycopy(e.values, 0, acc, 0, 3);
            haveAcc = true;
            return;
        }
        if (e.sensor.getType() != Sensor.TYPE_GYROSCOPE || !haveAcc) return;
        float dt = lastGyroNs != 0 ? (e.timestamp - lastGyroNs) * 1e-9f : 0;
        lastGyroNs = e.timestamp;
        if (dt <= 0) return;
        float[] g = toSdl(e.values), a = toSdl(acc);
        Native.motionSample(dt, g[0], g[1], g[2], a[0], a[1], a[2]);
    }

    // source axes -> SDL controller axes
    private float[] toSdl(float[] v) {
        // a controller's sensors already use these axes (the kernel's convention for gamepads)
        if (fromController) return new float[] {v[0], v[1], v[2]};
        // the device: Android's axes in its natural orientation, turned with the screen (x right, y up
        // on the screen as it is shown, z out of the screen); lying flat (screen up) is SDL's resting pose
        float x = v[0], y = v[1];
        switch (activity.getDisplay() != null ? activity.getDisplay().getRotation() : Surface.ROTATION_0) {
            case Surface.ROTATION_90: x = -v[1]; y = v[0]; break;
            case Surface.ROTATION_180: x = -v[0]; y = -v[1]; break;
            case Surface.ROTATION_270: x = v[1]; y = -v[0]; break;
            default: break;
        }
        return new float[] {x, v[2], -y};
    }

    @Override
    public void onAccuracyChanged(Sensor sensor, int accuracy) {}
}
