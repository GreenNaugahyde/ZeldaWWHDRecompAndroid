package org.wwhdrecomp.app;

import android.content.SharedPreferences;

/**
 * Once, before the game starts (new installations and the first start of 0.9.6): three questions that set up
 * the new touch controls, Jump anywhere and the songs.
 *
 * 1. The new touch controls? Yes: Flexible style without drawn sticks, context buttons, items by tap with only
 *    the aiming buttons while aiming, ZL to the next target, swipe camera (+100) with the direct camera and the
 *    original walking direction, item ring, minigames by touch, hearts only in combat, rupees only when they
 *    change, the large Picto Box view, replayed conducted songs, play buttons in the song list, motion controls
 *    and rumble, and the arc layout (ControlsView.arcLayout). No: everything stays as it is.
 * 2. Jump anywhere? Yes: on, then the double jump yes / no.
 * 3. Singing the songs? Yes: on (Android asks for the microphone). No: conducting by the note pads.
 *
 * Everything stays changeable in the options; the questions don't come again (pref starter_done).
 */
final class StarterQuestions {
    private static final String DONE = "starter_done";

    private StarterQuestions() {}

    /** Shows the questions if they haven't been asked; then runs `then`. False: nothing to ask. */
    static boolean offer(MainActivity a, Runnable then) {
        SharedPreferences p = a.prefs;
        if (p.getBoolean(DONE, false) || !MainActivity.NEW_TOUCH_CONTROLS) return false;
        new GameDialog(a).title(R.string.st_touch_title).message(R.string.st_touch_text).cancelable(false)
                .button(R.string.st_touch_no, () -> askJump(a, then))
                .button(R.string.st_touch_yes, () -> {
                    applyTouchPreset(a);
                    askJump(a, then);
                }).show();
        return true;
    }

    private static void askJump(MainActivity a, Runnable then) {
        new GameDialog(a).title(R.string.st_jump_title).message(R.string.st_jump_text).cancelable(false)
                .button(R.string.st_no, () -> askSinging(a, then))
                .button(R.string.st_yes, () -> {
                    a.setJumpAnywhere(true);
                    new GameDialog(a).title(R.string.opt_mod_jump_double).message(R.string.st_double_text).cancelable(false)
                            .button(R.string.st_no, () -> {
                                a.setMod("mod_jump_double", false);
                                askSinging(a, then);
                            })
                            .button(R.string.st_yes, () -> {
                                a.setMod("mod_jump_double", true);
                                askSinging(a, then);
                            }).show();
                }).show();
    }

    private static void askSinging(MainActivity a, Runnable then) {
        new GameDialog(a).title(R.string.st_sing_title).message(R.string.st_sing_text).cancelable(false)
                .button(R.string.st_sing_no, () -> {
                    a.prefs.edit().putBoolean("conduct_pads", true).apply();
                    finish(a, then);
                })
                .button(R.string.st_sing_yes, () -> {
                    a.prefs.edit().putBoolean("sing_button", true).apply();
                    a.askSingSongs();
                    finish(a, then);
                }).show();
    }

    private static void finish(MainActivity a, Runnable then) {
        a.prefs.edit().putBoolean(DONE, true).apply();
        if (then != null) then.run();
    }

    private static void applyTouchPreset(MainActivity a) {
        android.graphics.Rect win = a.getWindowManager().getCurrentWindowMetrics().getBounds();
        int w = Math.max(win.width(), win.height()), h = Math.min(win.width(), win.height());
        a.prefs.edit()
                .putInt("controls_style", 1)                 // Flexible
                .putBoolean("controls_show_sticks", false)   // the sticks work from a blank screen
                .putBoolean("context_buttons", true)
                .putBoolean("item_tap", true)
                .putBoolean("item_tap_focus", true)          // while aiming only the aiming buttons
                .putInt("zl_after_defeat", 1)                // ZL locks the next target
                .putInt("touch_camera", 1)                   // the right half: swipe
                .putFloat("swipe_speed", MainActivity.SWIPE_SPEED_DEFAULT + 1f)  // +100
                .putBoolean("item_ring", true)
                .putBoolean("minigame_touch", true)
                .putBoolean("hearts_combat", true)
                .putBoolean("rupees_on_change", true)
                .putBoolean("picto_big", true)
                .putBoolean("song_list_play", true)
                .putString("controls_layout_flex", ControlsView.arcLayout(w, h, a.prefs.getFloat("controls_scale", 1f)))
                .apply();
        a.setMotion(true);
        a.setRumble(true);
        a.setSteerMode(0);                       // the walking direction follows the camera (original)
        a.setMod("mod_direct_camera", true);
        a.setMod("mod_song_replay", true);       // conducted songs replayed (GameCube)
        Native.setOption("ring_no_baton", 1);
        a.applyControlsAppearance();
    }
}
