package org.wwhdrecomp.app;

/**
 * libwwhdrando.so (runtime/src/rando/rando_jni.cpp): TWWHD Randomizer. `data` is its data folder (Randomizer.dataDir),
 * `work` the folder with its settings (config.yaml, preferences.yaml), logs and last.txt. Settings by the randomizer's
 * own names ("Progress Dungeons", options.cpp settingToName), as the index of their value.
 */
final class RandoNative {
    static {
        System.loadLibrary("wwhdrando");
    }

    private RandoNative() {}

    /** The setting's value index, -1 if there is no such setting. */
    static native int get(String data, String work, String name);
    /** null when set, else why not. */
    static native String set(String data, String work, String name, int value);
    static native String seed(String data, String work);
    static native String setSeed(String data, String work, String seed);
    /** A new random seed name (the randomizer's words) into the settings; returns it. */
    static native String newSeed(String data, String work);
    static native String resetDefaults(String data, String work);
    /** The settings and seed as the PC randomizer's permalink. */
    static native String permalink(String data, String work);
    /** null when the permalink's settings and seed were taken, else why not. */
    static native String loadPermalink(String data, String work, String permalink);
    /** Link's colours: "casual\t0|1", "preset\t<name>\t<0|1 if now>", "color\t<name>\t<default>\t<now>" lines; "" if unreadable. */
    static native String colors(String data, String work);
    /** One of Link's colours (hex RRGGBB, "" = its default). null when set, else why not. */
    static native String setColor(String data, String work, String name, String hex);
    static native String setCasual(String data, String work, boolean casual);
    static native String loadPreset(String data, String work, int index);
    static native String randomColors(String data, String work, boolean chaotic);
    /** Link's colours for the game as it is: the shared ones (skin, mouth, eyes) of the clothes chosen into the other's too */
    static native String syncShared(String data, String work);
    /**
     * Link's textures in these colours into the file `out` for the runtime (runtime/src/rando/link_colors_make.h;
     * `separate`: the hero's own skin, mouth and eyes); null when done
     */
    static native String applyColors(String data, String work, String game, String out, boolean separate);
    static native String version();
    /** One run, once per process: `game` (USA) randomized into `out` (changed files only). null when done, else why not. */
    static native String generate(String data, String work, String game, String out);
}
