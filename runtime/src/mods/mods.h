// Optional gameplay mods (Gameplay menu). All off by default; each can be switched live.
//   camera.cpp  direct right-stick camera, mouse camera, first person on R3 / mouse wheel
//   mouse.mm    mouse capture in the game window (macOS events)
//   turbo.cpp   quick doors, fast scene changes and fast forward (extra logic steps while they run)
//   mods.cpp    faster running, swimming, sidling (true60_link.cpp's posMoveFromFootPos site applies it)
//               and climbing (hooks on the climbing animations' speed)
// Test/start-up switches: WWHD_MOD_<NAME>=1 (see mods.cpp).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct Cpu;
namespace input { struct PadState; }

namespace mods {

// ---- switches (menu + env) ----
bool direct_camera();
void set_direct_camera(bool on);
float camera_speed();  // direct camera: multiplier of the game's own top turning speed
void set_camera_speed(float s);
bool mouse_camera();
void set_mouse_camera(bool on);
float mouse_sensitivity();  // degrees per mouse point
float swipe_boat();  // multiplier of the swipe / mouse camera while Link sails (third person only)
void set_swipe_boat(float k);
void set_mouse_sensitivity(float s);
// the game's first-person look ran just now (camera.cpp)
bool first_person_view();
bool first_person_wheel();
void set_first_person_wheel(bool on);
bool quick_doors();
void set_quick_doors(bool on);
bool fast_scenes();
void set_fast_scenes(bool on);
float run_speed();  // Link's running: multiplier of the distance per step (1 = off)
void set_run_speed(float s);
float swim_speed();  // the same while Link swims
void set_swim_speed(float s);
float climb_speed();  // ladders, ivy, along a ledge: multiplier of the climbing animation's speed
void set_climb_speed(float s);
float sidle_speed();  // sidling along a wall: multiplier of the distance per step
void set_sidle_speed(float s);
// when each applies: 0 always, 1 while L3 is held, 2 L3 switches it on and off (a press switches
// the one for what Link is doing: swimming, climbing, sidling, else running)
int run_mode();
void set_run_mode(int m);
int swim_mode();
void set_swim_mode(int m);
int climb_mode();
void set_climb_mode(int m);
int sidle_mode();
void set_sidle_mode(int m);
void run_input(uint32_t buttons);  // filter_pad: the GamePad buttons of this read (L3 edges, ZR)
void stick_input(float x, float y);  // filter_pad: the left stick of this read
// fast forward (turbo.cpp): while ZR is held, cutscenes / dialogues run several logic steps per
// frame
bool ff_cutscenes();
void set_ff_cutscenes(bool on);
// songs conducted: Link plays the tune once more before its effect, as on the GameCube (songs.cpp)
void set_song_replay(bool on);
// pictos mailed to Carlov at postboxes (postbox.cpp): the figurine the next day, or at once (same day delivery)
bool postbox_mail();
void set_postbox_mail(bool on);
bool postbox_same_day();
void set_postbox_same_day(bool on);
int postbox_notice();             // the app's next notice: kind << 8 | gallery number; -1 none
int postbox_offer();              // 1 once after the postbox asked what to send (the app asks about a picto)
void postbox_present();           // the app's Yes: the Picto Box shown to the postbox (its button pressed)
bool postbox_near();              // Link faces a postbox (its A icon)
void postbox_pad(input::PadState& s);  // filter_pad: the Picto Box's button pressed for postbox_present
void postbox_service(Cpu* c);     // game main thread, frame start (interp.cpp)
// Link's clothes (clothes.cpp): 0 the story's, 1 Hero's clothes, 2 casual clothes (a change: a puff of smoke)
int link_clothes();
void set_link_clothes(int mode);
void clothes_service(Cpu* c);  // game main thread, frame start (interp.cpp)
// Picto Box album pages (picto_pages.cpp): any number of pages of 12 pictos; a full album starts a new page,
// the album viewer's page switch (the app) asks for the next / previous one
bool picto_pages();
void set_picto_pages(bool on);
void picto_page_request(int dir);  // -1 / +1, done at the next frame start
int picto_page_current();          // 0-based; -1 off or no file in play
int picto_page_count();
int picto_page_used();             // pictos on the page in play
bool picto_album_open();          // the Picto Box's album is on screen (and the pages on)
bool picto_album_shown();         // the album is on screen (with the pages off too)
int picto_count();                // pictos in the game's album now
int picto_subject();              // the album's picto: subject | flaws << 8 | colour << 16; -1 none
void picto_pad(input::PadState& s);  // filter_pad: ZL / L turn the pages in the album
void picto_pages_service(Cpu* c);  // game main thread, frame start (interp.cpp)
bool ff_dialogues();
void set_ff_dialogues(bool on);
bool ff_button();  // ZR is held (last pad read)
int event_mode_now();  // turbo.cpp: 0 no event, 1 dialogue, 2 / 3 cutscene
// for daPy_lk_c::posMoveFromFootPos (true60_link.cpp): the factor for Link's horizontal movement
// this step (run_speed() while he runs, swim / sidle speed, else 1); link = the daPy_lk_c
float link_move_factor(uint32_t link);

// inverted camera up/down (camera.cpp): 0 off, 1 only while the game looks around with the right
// stick (first person, aiming an item), 2 always (also the third-person camera's height)
int invert_y();
void set_invert_y(int mode);
bool camera_turned_by_player();  // camera.cpp: the right-stick / swipe / mouse camera ran in the last steps
// walking direction (steer.cpp): 0 the game's, 1 kept while the player turns the camera, 2 kept while
// the stick is held (every camera turn)
int steer_mode();
void set_steer_mode(int mode);
bool steer_swing();  // the camera swings behind Link while he runs
void set_steer_swing(bool on);
void set_steer_resync(int deg);  // mode 2: how far the stick moves before it's camera-relative again
void set_steer_swing_delay(int ms);  // how long the right stick is left alone (or he runs towards the camera) first
void steer_pad(input::PadState& s, bool looking);  // filter_pad: the swing's right stick
bool steer_swing_turn(float& deg);  // manualCamera: the swing's degrees this step (signed like right-stick x)

// ---- input (input.mm) ----
// called at the end of input::read(): synthetic stick and buttons (mouse camera, R3 pulses)
void filter_pad(input::PadState& s);
// a key went down in the game window (before the game's own mapping); true = consumed
bool host_key_down(uint16_t code);

// ---- mouse (mouse.mm -> camera.cpp) ----
void mouse_add(float dx, float dy);  // points moved while captured (dy > 0 = down)
void mouse_wheel(float dy);          // wheel / scroll (dy > 0 = forward)
void mouse_button(int button, bool down);  // 0 left, 1 right
bool mouse_captured();
void mouse_init(void* tv_window);  // NSWindow* of the TV picture (menu.mm, main thread)
void mouse_release();  // mouse.mm: release the pointer (also called when the mod is switched off)

// ---- logic (interp.cpp: fpcEx_Handler) ----
// after a normal logic step: runs extra steps while a door event or a scene change is in progress
void after_execute(Cpu* c, uint32_t execute_fn);

uint32_t link_actor();  // camera.cpp: Link (daPy_lk_c), 0 until seen
// context-aware touch controls' game state (touch_state.cpp): once per logic step
void touch_state_step();
// touch for the minigames (minigames.cpp): once per logic step
void minigame_step();
// the minigame played by touch now (0 none; 1 Sploosh Kaboom, 2 picture puzzle, 3 mail sorting, 4 cannon, 5 auction, 6 a message's choices, 7 its number entry, 8 a shop's items, 9 looking at a figure, 10 conducting)
int minigame_mode();
// a finger on the TV picture (any thread): action 0 down, 1 moved, 2 up (NaN: cancelled); x, y in layout
// units (720 across the picture's height, 0 at its centre, +y up); 3 a second finger pinches (x: distance
// now / at its touch), 4 the pinch ended
void minigame_touch(int action, float x, float y);
// debug (WWHD_MG_WATCH): points to mark on the TV picture (x, y pairs in layout units); how many
constexpr int kMinigameMarks = 72;  // 64 squares + the cursor's
int minigame_marks(float* xy);
// filter_pad (camera.cpp): the minigame's stick pushes and button presses
void minigame_pad(input::PadState& s);
// a finger held on the picture in a talk (the app, flexible touch): A each time the message waits for it; with its
// fast forward on, the event runs fast forward while held (turbo.cpp)
void set_talk_hold(bool on);
void set_talk_hold_ff(bool on);
void set_talk_hold_pad(bool on);  // A held does it too (Game > Hold to go on in talks)
bool talk_hold_ff();
// (ids 0xFF: none; all 0 but [15..19] when not in a game: title screen, file select)
// [0] in play, [1] A action, [2] sword, [3..5] X / Y / R item, [6] B action, [7] ZR action,
// [8] event mode, [9] pause menu, [10] has the Wind Waker, [11] shield, [12] / [13] Link's mode bits
// (mPlayerStatus 0 / 1), [14] minigame type, [15..19] the game's X / Y / R / B / ZR pane alpha (hud.h),
// [20..22] the game's command text alpha on X / Y / R (hud.h; -1 unknown), [23..28] the zoom bar of the
// Picto Box / telescope (hud_zoom_bar, hud.h), [29] the item in Link's hand, [30] targets defeated while
// locked on (a count: the lock ended with the enemy dead), [31] locked on now, [32] targets in reach,
// [33] a fade / wipe is under way (scene change, also on the title screen), [34] the attention type A acts on
// (fopAc_Attn_TYPE: 5 a door, 6 a chest; -1 none), [35] A's Jump is the jump attack (1; the side hop / back flip 0),
// [36] Link's mode flags (daPy_lk_c::mModeFlg: 0x400000 on a ladder), [37] has the Swift Sail
constexpr int kTouchStateLen = 38;
void touch_state(int32_t out[kTouchStateLen]);
// the item ring: {21 inventory item ids (0xFF empty), X / Y / R inventory slots, item in Link's hand,
// the bags' 3 x 8 items (slots 24.., 36.., 48..)} (all -1 outside the game); request_equip puts an
// inventory slot (also a bag's) on X / Y / R (0..2) at the next step; request_conduct puts the Wind
// Waker's slot there only until Link has conducted (the app presses the button), then the old item
constexpr int kItemRingLen = 49;
void item_ring_state(int32_t out[kItemRingLen]);
void request_equip(int button, int slot);
int picto_box_button();  // touch_state.cpp: 0 X, 1 Y, 2 R with the Picto Box; -1 on none, -2 not owned
void lend_picto_box(int button);  // touch_state.cpp: the Picto Box on this button until lend_return (or 20 s)
void lend_return();               // the postbox answered the picto: the button's own item back
void request_conduct(int button, int slot);
// with the item ring on: the Wind Waker is taken off X / Y / R outside the ring's own conducting press
void set_ring_no_baton(bool on);
// the game's pace while the item ring is open: 0 normal, 1 slow motion, 2 paused (pace_service waits
// at the frame boundary, interp.cpp)
void set_game_pace(int mode);
void pace_service();
// the player's save data (dSv_player_c, as on the GameCube; touch_state.cpp), 0 outside a game
uint32_t player_save();
// ---- songs sung into the microphone (songs.cpp): the Wind Waker plays the song the app recognised
// {conducting (Link holds the Wind Waker: free play), learned songs (bits by melody number; -1 unknown),
//  has the Wind Waker, an event other than conducting runs (or the pause menu is open), songs handed to the game,
//  beats the game's judge took while conducting, the last one's now_beat * 16 + c_pos, ms since it,
//  the song a story scene waits for (learning one, a sage's: melody number; -1 none)}
constexpr int kSongStateLen = 9;
void song_state(int32_t out[kSongStateLen]);
// a beat of the app's count-in (time 3, 4, 6; last: its last beat): the game's metronome click (and upbeat) sound
void count_beat(int time, bool last);
// the game's judge of the conducted notes answers this melody (0 Wind's Requiem .. 5 Song of Passing) at the
// next beat while Link conducts (forgotten after 5 s)
void play_song(int melody);
// debug WWHD_SONG_DUMP: the game plays its songs while Link holds the Wind Waker (top of a frame, game thread)
void song_dump_service(Cpu* c);
// the 21 inventory slots' item numbers (0xFF empty); false without a loaded save
bool inventory(uint8_t out[21]);
// the HUD's text for each button action code (dActStts): NN of its message label T_ACT_Command_NN, 0 none;
// false until the game has filled its table
constexpr int kActionCodes = 0x4E;
bool action_texts(int32_t out[kActionCodes]);

// ---- cheats (cheats.cpp): one-shot save data edits, applied at the top of the next frame ----
enum Cheat {
    kCheatItems = 1, kCheatSword = 2, kCheatStats = 4,
    // story progress (can change or break story events)
    kCheatSongs = 8, kCheatTriforce = 16, kCheatDungeon = 32, kCheatKey = 64,
    kCheatBags = 128,  // the spoils and bait bag filled
};
void request_cheat(int which);  // any thread
enum Infinite { kInfHealth = 1, kInfMagic = 2, kInfAmmo = 4, kMoonJump = 8, kMoonNoFall = 16, kMoonAllAnims = 32, kInfRupees = 64, kInfBags = 128, kInfBreath = 256, kOneHit = 512 };  // switches (kMoonJump: hold L)
bool infinite(int which);
void set_infinite(int which, bool on);
void cheats_service();          // game main thread, frame start (interp.cpp)
void moon_jump_step(Cpu* c, uint32_t link, bool before);  // around daPy_lk_c::execute (climb.cpp)
// the moon jump's horizontal move for Link's `current.pos += speed` in this step (true60_link.cpp)
bool moon_jump_move(uint32_t link, float& x, float& z);
// rise and flight speed in percent of the tested values (25 and 40 units per step), from the menu
void set_moon_jump_speeds(int rise, int fly);
int moon_jump_rise();
int moon_jump_fly();
bool jump_held();               // Jump anywhere's input (input::kJump) in the latest read (run_input)
bool moon_held();               // the moon jump's (input::kMoonJump); on one controller button the app sends only that

// ---- jump anywhere (jump.cpp): L jumps, optionally once more in the air ----
bool jump_anywhere();
void set_jump_anywhere(bool on);
bool double_jump();
void set_double_jump(bool on);
bool no_auto_jump();  // the game's own jump off ledges becomes a plain fall (with jump anywhere on)
void set_no_auto_jump(bool on);
void jump_step(Cpu* c, uint32_t link);        // before daPy_lk_c::execute (climb.cpp)
void jump_after_step(Cpu* c, uint32_t link);  // after it
void left_stick(float& x, float& y);  // ... and its left stick (-1..1, +y up)

// the live save data (dSv_info_c) once a save file is loaded, else 0 (cheats.cpp)
uint32_t loaded_save();
// the live save data's address, also while it is the title's placeholder
uint32_t save_info();

// ---- warp (warp.cpp, developer menu): a scene change, applied at the top of the next frame ----
// false when no save file is loaded; layer -1: the stage's layer for the story so far
// story: a story preset (the save's dSv_save_c, 0x778 bytes) written into the live save first, or null
bool request_warp(const char* stage, int room, int spawn, int layer, const uint8_t* story = nullptr, size_t story_len = 0);
// the next request_warp puts Link there (angle: s16, 0 = +z) once it has loaded and no cutscene runs
void set_warp_place(float x, float y, float z, int angle);
std::vector<uint8_t> story_block();  // the live save's story part (dSv_save_c), empty outside the game
std::string warp_where();  // "stage,room,spawn,layer" of the current scene ("" outside the game)
void warp_service();       // game main thread, frame start (interp.cpp)
// the latest warp: {0 none, 1 requested, 2 changing scene, 3 loaded, 4 refused (a change was under
// way); ms since the request, or the load time once loaded; the event mode now (event_mode_now)}
void warp_status(int64_t out[3]);
// the graphics benchmark: warps also leave the title screen (its placeholder save data), so a fixed
// place loads without a save file; request_warp works from the title then
void set_warp_from_title(bool on);
// the play / opening scene's draw (dScnPly_Draw, game thread): a pending warp from the title starts
// here
void warp_title_draw(Cpu* c, uint32_t scene);

// shared helpers (mods.cpp)
uint64_t step();    // full logic steps so far (interp::logic_steps)
double game_time(); // seconds of game time (steps / 30)
bool trace_on();    // WWHD_MODS_TRACE: log mod decisions
void trace(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

}  // namespace mods
