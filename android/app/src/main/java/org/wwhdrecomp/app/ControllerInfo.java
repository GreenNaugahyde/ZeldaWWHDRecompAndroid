package org.wwhdrecomp.app;

import android.hardware.BatteryState;
import android.hardware.Sensor;
import android.hardware.SensorManager;
import android.hardware.lights.Light;
import android.os.Build;
import android.os.VibrationEffect;
import android.os.Vibrator;
import android.os.VibratorManager;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;

import java.util.ArrayList;
import java.util.List;
import java.util.Locale;

/**
 * Controls > Controller > Controller details: what Android tells about a connected controller (names, ids, battery,
 * rumble motors, motion sensors, lights, buttons, axes) and what this app uses of it. (Bus and Bluetooth address
 * aren't public; SELinux keeps apps out of /proc/bus/input/devices.)
 */
final class ControllerInfo {
    /** a heading and its rows (label, value) */
    static final class Section {
        final String title;
        final List<String[]> rows = new ArrayList<>();
        Section(String title) { this.title = title; }
        void add(String label, String value) { rows.add(new String[] {label, value}); }
    }

    private final MainActivity a;
    private final InputDevice d;

    private ControllerInfo(MainActivity a, InputDevice d) { this.a = a; this.d = d; }

    /** the connected controllers, the one in use first */
    static List<InputDevice> connected(InputDevice inUse) {
        List<InputDevice> l = new ArrayList<>();
        for (int id : InputDevice.getDeviceIds()) {
            InputDevice d = InputDevice.getDevice(id);
            if (d == null || d.isVirtual() || !InputMapper.isController(d)) continue;
            if (inUse != null && d.getId() == inUse.getId()) l.add(0, d);
            else l.add(d);
        }
        return l;
    }

    static List<Section> describe(MainActivity a, InputDevice d, boolean inUse) {
        ControllerInfo c = new ControllerInfo(a, d);
        List<Section> s = new ArrayList<>();
        s.add(c.general());
        s.add(c.inApp(inUse));
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) s.add(c.battery());
        s.add(c.rumble());
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            s.add(c.motion());
            s.add(c.lights());
        }
        s.add(c.buttons());
        s.add(c.axes());
        return s;
    }

    private String str(int id, Object... args) { return a.getString(id, args); }
    private String yesNo(boolean b) { return str(b ? R.string.cd_yes : R.string.cd_no); }
    private static String hex4(int v) { return String.format(Locale.ROOT, "0x%04X", v); }
    private static String num(float v) {
        if (v == Math.rint(v) && Math.abs(v) < 1e6) return String.valueOf((long) v);
        return String.format(Locale.ROOT, Math.abs(v) >= 100 ? "%.1f" : Math.abs(v) >= 1 ? "%.3f" : "%.4g", v);
    }

    private Section general() {
        Section s = new Section(str(R.string.cd_sec_general));
        s.add(str(R.string.cd_name), d.getName());
        String vendor = vendorName(d.getVendorId());
        s.add(str(R.string.cd_vendor), hex4(d.getVendorId()) + (vendor != null ? "  (" + vendor + ")" : ""));
        s.add(str(R.string.cd_product), hex4(d.getProductId()));
        s.add(str(R.string.cd_device_id), String.valueOf(d.getId()));
        s.add(str(R.string.cd_player), d.getControllerNumber() > 0 ? String.valueOf(d.getControllerNumber()) : str(R.string.cd_none));
        s.add(str(R.string.cd_descriptor), d.getDescriptor());
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q)
            s.add(str(R.string.cd_connection), str(d.isExternal() ? R.string.cd_external : R.string.cd_builtin));
        s.add(str(R.string.cd_enabled), yesNo(d.isEnabled()));
        s.add(str(R.string.cd_mic), yesNo(d.hasMicrophone()));
        int kb = d.getKeyboardType();
        s.add(str(R.string.cd_keyboard), str(kb == InputDevice.KEYBOARD_TYPE_ALPHABETIC ? R.string.cd_kb_alpha
                : kb == InputDevice.KEYBOARD_TYPE_NON_ALPHABETIC ? R.string.cd_kb_buttons : R.string.cd_none));
        s.add(str(R.string.cd_sources), sources(d.getSources()));
        s.add(str(R.string.cd_face), str(InputMapper.isSony(d) ? R.string.cd_face_sony
                : InputMapper.isNintendo(d) ? R.string.cd_face_nintendo : R.string.cd_face_xbox));
        return s;
    }

    private Section inApp(boolean inUse) {
        Section s = new Section(str(R.string.cd_sec_app));
        s.add(str(R.string.cd_in_use), yesNo(inUse));
        InputMapper m = new InputMapper();  // (its assignment, as MainActivity.usePadMap loads it)
        m.loadMap(a.prefs.getString("pad_map@" + d.getDescriptor(), a.prefs.getString("pad_map", "")));
        s.add(str(R.string.opt_buttons), str(m.isDefaultMap() ? R.string.opt_buttons_default : R.string.opt_buttons_custom));
        s.add(str(R.string.cd_set_up), yesNo(a.prefs.getBoolean("pad_asked@" + d.getDescriptor(), false)));
        boolean motion = a.prefs.getBoolean("motion", true), rumble = a.prefs.getBoolean("rumble", true);
        // (while it is in use; Controls > Motion controls / Rumble may send them to this device or another controller)
        int mFrom = a.prefs.getInt("motion_from", MainActivity.SRC_AUTO), rOn = a.prefs.getInt("rumble_on", MainActivity.SRC_AUTO);
        s.add(str(R.string.cd_motion_from), !motion ? str(R.string.opt_off)
                : str(MotionInput.hasMotion(d) && mFrom != MainActivity.SRC_DEVICE ? R.string.cd_this_controller : R.string.cd_this_device));
        s.add(str(R.string.cd_rumble_on), !rumble ? str(R.string.opt_off)
                : str(!hasRumble(d) || rOn == MainActivity.SRC_DEVICE ? R.string.cd_this_device
                : rOn == MainActivity.SRC_BOTH ? R.string.cd_both : R.string.cd_this_controller));
        return s;
    }

    private Section battery() {
        Section s = new Section(str(R.string.cd_sec_battery));
        BatteryState b = Build.VERSION.SDK_INT >= Build.VERSION_CODES.S ? d.getBatteryState() : null;
        if (b == null || !b.isPresent()) {
            s.add(str(R.string.cd_battery), str(R.string.cd_none_reported));
            return s;
        }
        float c = b.getCapacity();
        s.add(str(R.string.cd_charge), Float.isNaN(c) || c < 0 ? str(R.string.cd_unknown) : Math.round(c * 100) + " %");
        int st = b.getStatus();
        s.add(str(R.string.cd_status), str(st == BatteryState.STATUS_CHARGING ? R.string.cd_bat_charging
                : st == BatteryState.STATUS_DISCHARGING ? R.string.cd_bat_discharging
                : st == BatteryState.STATUS_NOT_CHARGING ? R.string.cd_bat_not_charging
                : st == BatteryState.STATUS_FULL ? R.string.cd_bat_full : R.string.cd_unknown));
        return s;
    }

    static boolean hasRumble(InputDevice d) {
        Vibrator v = Build.VERSION.SDK_INT >= Build.VERSION_CODES.S ? d.getVibratorManager().getDefaultVibrator() : d.getVibrator();
        return v != null && v.hasVibrator();
    }

    private static final int[] PRIMITIVES = {VibrationEffect.Composition.PRIMITIVE_CLICK, VibrationEffect.Composition.PRIMITIVE_TICK,
            VibrationEffect.Composition.PRIMITIVE_LOW_TICK, VibrationEffect.Composition.PRIMITIVE_THUD,
            VibrationEffect.Composition.PRIMITIVE_SPIN, VibrationEffect.Composition.PRIMITIVE_QUICK_RISE,
            VibrationEffect.Composition.PRIMITIVE_SLOW_RISE, VibrationEffect.Composition.PRIMITIVE_QUICK_FALL};
    private static final String[] PRIMITIVE_NAMES = {"click", "tick", "low tick", "thud", "spin", "quick rise", "slow rise", "quick fall"};

    private Section rumble() {
        Section s = new Section(str(R.string.cd_sec_rumble));
        List<Vibrator> motors = new ArrayList<>();
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            VibratorManager m = d.getVibratorManager();
            for (int id : m.getVibratorIds()) {
                Vibrator v = m.getVibrator(id);
                if (v != null && v.hasVibrator()) motors.add(v);
            }
        } else if (d.getVibrator().hasVibrator()) {
            motors.add(d.getVibrator());
        }
        s.add(str(R.string.cd_motors), motors.isEmpty() ? str(R.string.cd_none_reported) : String.valueOf(motors.size()));
        for (int i = 0; i < motors.size(); i++) {
            Vibrator v = motors.get(i);
            StringBuilder b = new StringBuilder(str(v.hasAmplitudeControl() ? R.string.cd_amplitude : R.string.cd_on_off_only));
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
                b.insert(0, "ID " + v.getId() + "  ·  ");
                float f = v.getResonantFrequency(), q = v.getQFactor();
                if (!Float.isNaN(f)) b.append("\n").append(str(R.string.cd_resonance, num(f))).append(Float.isNaN(q) ? "" : ", Q " + num(q));
            }
            boolean[] prims = v.arePrimitivesSupported(PRIMITIVES);
            StringBuilder p = new StringBuilder();
            for (int j = 0; j < prims.length; j++)
                if (prims[j]) p.append(p.length() == 0 ? "" : ", ").append(PRIMITIVE_NAMES[j]);
            b.append("\n").append(str(R.string.cd_effects, p.length() == 0 ? str(R.string.cd_none) : p));
            s.add(str(R.string.cd_motor, i + 1), b.toString());
        }
        return s;
    }

    private Section motion() {
        Section s = new Section(str(R.string.cd_sec_motion));
        SensorManager m = Build.VERSION.SDK_INT >= Build.VERSION_CODES.S ? d.getSensorManager() : null;
        List<Sensor> list = m != null ? m.getSensorList(Sensor.TYPE_ALL) : new ArrayList<>();
        if (list.isEmpty()) {
            s.add(str(R.string.cd_sensors), str(R.string.cd_none_reported));
            return s;
        }
        for (Sensor x : list) {
            String unit = x.getType() == Sensor.TYPE_GYROSCOPE ? " rad/s" : x.getType() == Sensor.TYPE_ACCELEROMETER ? " m/s²" : "";
            StringBuilder b = new StringBuilder(x.getName());
            if (x.getVendor() != null && !x.getVendor().isEmpty()) b.append("  ·  ").append(x.getVendor());
            b.append("\n").append(str(R.string.cd_range, num(x.getMaximumRange()) + unit, num(x.getResolution()) + unit));
            if (x.getMinDelay() > 0) b.append(", ").append(str(R.string.cd_rate, num(1e6f / x.getMinDelay())));
            if (x.getPower() > 0) b.append(", ").append(num(x.getPower())).append(" mA");
            s.add(sensorType(x), b.toString());
        }
        return s;
    }

    private String sensorType(Sensor x) {
        switch (x.getType()) {
            case Sensor.TYPE_GYROSCOPE: return str(R.string.cd_gyro);
            case Sensor.TYPE_ACCELEROMETER: return str(R.string.cd_accel);
            default:
                String t = x.getStringType();
                return t != null ? t.replace("android.sensor.", "") : "type " + x.getType();
        }
    }

    private Section lights() {
        Section s = new Section(str(R.string.cd_sec_lights));
        List<Light> list = Build.VERSION.SDK_INT >= Build.VERSION_CODES.S ? d.getLightsManager().getLights() : new ArrayList<>();
        if (list.isEmpty()) {
            s.add(str(R.string.cd_lights), str(R.string.cd_none_reported));
            return s;
        }
        for (Light l : list) {
            StringBuilder b = new StringBuilder(l.getName() != null ? l.getName() : "");
            if (b.length() > 0) b.append("  ·  ");
            b.append("ID ").append(l.getId());
            if (l.hasRgbControl()) b.append(", ").append(str(R.string.cd_light_colour));
            if (l.hasBrightnessControl()) b.append(", ").append(str(R.string.cd_light_brightness));
            s.add(lightType(l.getType()), b.toString());
        }
        return s;
    }

    private String lightType(int t) {
        switch (t) {
            case Light.LIGHT_TYPE_MICROPHONE: return str(R.string.cd_light_mic);
            case 10001: return str(R.string.cd_light_input);         // LIGHT_TYPE_INPUT
            case 10002: return str(R.string.cd_light_player);        // LIGHT_TYPE_PLAYER_ID
            case 10003: return str(R.string.cd_light_keyboard);      // LIGHT_TYPE_KEYBOARD_BACKLIGHT
            default: return str(R.string.cd_light) + " (" + t + ")";
        }
    }

    private static final int[] KEYS = {KeyEvent.KEYCODE_BUTTON_A, KeyEvent.KEYCODE_BUTTON_B, KeyEvent.KEYCODE_BUTTON_C,
            KeyEvent.KEYCODE_BUTTON_X, KeyEvent.KEYCODE_BUTTON_Y, KeyEvent.KEYCODE_BUTTON_Z,
            KeyEvent.KEYCODE_BUTTON_L1, KeyEvent.KEYCODE_BUTTON_R1, KeyEvent.KEYCODE_BUTTON_L2, KeyEvent.KEYCODE_BUTTON_R2,
            KeyEvent.KEYCODE_BUTTON_THUMBL, KeyEvent.KEYCODE_BUTTON_THUMBR,
            KeyEvent.KEYCODE_BUTTON_SELECT, KeyEvent.KEYCODE_BUTTON_START, KeyEvent.KEYCODE_BUTTON_MODE,
            KeyEvent.KEYCODE_BACK, KeyEvent.KEYCODE_HOME, KeyEvent.KEYCODE_MENU, KeyEvent.KEYCODE_MEDIA_RECORD,
            KeyEvent.KEYCODE_DPAD_UP, KeyEvent.KEYCODE_DPAD_DOWN, KeyEvent.KEYCODE_DPAD_LEFT, KeyEvent.KEYCODE_DPAD_RIGHT,
            KeyEvent.KEYCODE_DPAD_CENTER,
            KeyEvent.KEYCODE_BUTTON_1, KeyEvent.KEYCODE_BUTTON_2, KeyEvent.KEYCODE_BUTTON_3, KeyEvent.KEYCODE_BUTTON_4,
            KeyEvent.KEYCODE_BUTTON_5, KeyEvent.KEYCODE_BUTTON_6, KeyEvent.KEYCODE_BUTTON_7, KeyEvent.KEYCODE_BUTTON_8,
            KeyEvent.KEYCODE_BUTTON_9, KeyEvent.KEYCODE_BUTTON_10, KeyEvent.KEYCODE_BUTTON_11, KeyEvent.KEYCODE_BUTTON_12,
            KeyEvent.KEYCODE_BUTTON_13, KeyEvent.KEYCODE_BUTTON_14, KeyEvent.KEYCODE_BUTTON_15, KeyEvent.KEYCODE_BUTTON_16};

    // the key layout's buttons (Android's key codes), as hasKeys reports them
    private Section buttons() {
        Section s = new Section(str(R.string.cd_sec_buttons));
        boolean[] has = d.hasKeys(KEYS);
        StringBuilder b = new StringBuilder();
        int n = 0;
        for (int i = 0; i < KEYS.length; i++) {
            if (!has[i]) continue;
            n++;
            b.append(b.length() == 0 ? "" : ", ").append(KeyEvent.keyCodeToString(KEYS[i]).replace("KEYCODE_", ""));
        }
        s.add(str(R.string.cd_button_count, n), n == 0 ? str(R.string.cd_none_reported) : b.toString());
        return s;
    }

    private Section axes() {
        Section s = new Section(str(R.string.cd_sec_axes));
        List<InputDevice.MotionRange> ranges = d.getMotionRanges();
        if (ranges.isEmpty()) {
            s.add(str(R.string.cd_axes), str(R.string.cd_none_reported));
            return s;
        }
        boolean manySources = false;
        for (InputDevice.MotionRange r : ranges) manySources |= r.getSource() != ranges.get(0).getSource();
        for (InputDevice.MotionRange r : ranges) {
            String name = MotionEvent.axisToString(r.getAxis()).replace("AXIS_", "");
            String role = axisRole(r.getAxis());
            if (role != null) name += "  (" + role + ")";
            StringBuilder b = new StringBuilder(num(r.getMin()) + " … " + num(r.getMax()));
            b.append(", flat ").append(num(r.getFlat())).append(", fuzz ").append(num(r.getFuzz()));
            if (r.getResolution() > 0) b.append(", ").append(str(R.string.cd_resolution, num(r.getResolution())));
            if (manySources) b.append("\n").append(sources(r.getSource()));
            s.add(name, b.toString());
        }
        return s;
    }

    // what InputMapper reads the axis as
    private String axisRole(int axis) {
        switch (axis) {
            case MotionEvent.AXIS_X: case MotionEvent.AXIS_Y: return str(R.string.cd_axis_left);
            case MotionEvent.AXIS_Z: case MotionEvent.AXIS_RZ: return str(R.string.cd_axis_right);
            case MotionEvent.AXIS_HAT_X: case MotionEvent.AXIS_HAT_Y: return str(R.string.cd_axis_dpad);
            case MotionEvent.AXIS_LTRIGGER: case MotionEvent.AXIS_BRAKE: return str(R.string.cd_axis_lt);
            case MotionEvent.AXIS_RTRIGGER: case MotionEvent.AXIS_GAS: return str(R.string.cd_axis_rt);
            default: return null;
        }
    }

    private static final int[] SOURCES = {InputDevice.SOURCE_GAMEPAD, InputDevice.SOURCE_JOYSTICK, InputDevice.SOURCE_DPAD,
            InputDevice.SOURCE_KEYBOARD, InputDevice.SOURCE_MOUSE, InputDevice.SOURCE_MOUSE_RELATIVE, InputDevice.SOURCE_TOUCHPAD,
            InputDevice.SOURCE_TOUCHSCREEN, InputDevice.SOURCE_TOUCH_NAVIGATION, InputDevice.SOURCE_STYLUS,
            InputDevice.SOURCE_BLUETOOTH_STYLUS, InputDevice.SOURCE_TRACKBALL, InputDevice.SOURCE_ROTARY_ENCODER,
            InputDevice.SOURCE_HDMI, 0x04000000 /* SOURCE_SENSOR, API 31 */};
    private static final String[] SOURCE_NAMES = {"gamepad", "joystick", "d-pad", "keyboard", "mouse", "mouse (relative)",
            "touchpad", "touchscreen", "touch navigation", "stylus", "Bluetooth stylus", "trackball", "rotary encoder", "HDMI", "sensor"};

    private static String sources(int bits) {
        StringBuilder b = new StringBuilder();
        for (int i = 0; i < SOURCES.length; i++)
            if ((bits & SOURCES[i]) == SOURCES[i]) b.append(b.length() == 0 ? "" : ", ").append(SOURCE_NAMES[i]);
        return b + "  (" + String.format(Locale.ROOT, "0x%08X", bits) + ")";
    }

    private static String vendorName(int v) {
        switch (v) {
            case 0x054C: return "Sony";
            case 0x057E: return "Nintendo";
            case 0x045E: return "Microsoft";
            case 0x046D: return "Logitech";
            case 0x2DC8: return "8BitDo";
            case 0x0F0D: return "Hori";
            case 0x0E6F: return "PDP";
            case 0x24C6: return "PowerA";
            case 0x20D6: return "PowerA";
            case 0x1532: return "Razer";
            case 0x28DE: return "Valve";
            case 0x18D1: return "Google";
            case 0x1949: return "Amazon";
            case 0x0738: return "Mad Catz";
            case 0x044F: return "Thrustmaster";
            case 0x3537: return "GameSir";
            case 0x0079: return "DragonRise";
            case 0x2E95: return "SCUF";
            case 0x0B05: return "ASUS";
            case 0x1038: return "SteelSeries";
            case 0x2C22: return "Qanba";
            case 0x0955: return "NVIDIA";
            case 0x2F24: return "Backbone";
            default: return null;
        }
    }
}
