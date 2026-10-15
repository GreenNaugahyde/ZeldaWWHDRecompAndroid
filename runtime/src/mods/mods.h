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
void set_mouse_sensitivity(float s);
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
// {valid, A action, sword id, X item, Y item, R item, B action, ZR action, event mode, pause menu open,
// has the Wind Waker, shield id} (ids 0xFF: none; all 0 when not in a game: title screen, file select)
void touch_state(int32_t out[12]);

// ---- cheats (cheats.cpp): one-shot save data edits, applied at the top of the next frame ----
enum Cheat {
    kCheatItems = 1, kCheatSword = 2, kCheatStats = 4,
    // story progress (can change or break story events)
    kCheatSongs = 8, kCheatTriforce = 16, kCheatDungeon = 32, kCheatKey = 64,
    kCheatBags = 128,  // the spoils and bait bag filled
};
void request_cheat(int which);  // any thread
enum Infinite { kInfHealth = 1, kInfMagic = 2, kInfAmmo = 4, kMoonJump = 8, kMoonNoFall = 16, kMoonAllAnims = 32, kInfRupees = 64, kInfBags = 128, kInfBreath = 256 };  // switches (kMoonJump: hold L)
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
bool l_held();                  // the GamePad's L (shoulder) in the latest read (run_input)

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

// ---- warp (warp.cpp, developer menu): a scene change, applied at the top of the next frame ----
// false when no save file is loaded; layer -1: the stage's layer for the story so far
// story: a story preset (the save's dSv_save_c, 0x778 bytes) written into the live save first, or null
bool request_warp(const char* stage, int room, int spawn, int layer, const uint8_t* story = nullptr, size_t story_len = 0);
std::vector<uint8_t> story_block();  // the live save's story part (dSv_save_c), empty outside the game
std::string warp_where();  // "stage,room,spawn,layer" of the current scene ("" outside the game)
void warp_service();       // game main thread, frame start (interp.cpp)
// the latest warp: {0 none, 1 requested, 2 changing scene, 3 loaded, 4 refused (a change was under
// way); ms since the request, or the load time once loaded; the event mode now (event_mode_now)}
void warp_status(int64_t out[3]);

// shared helpers (mods.cpp)
uint64_t step();    // full logic steps so far (interp::logic_steps)
double game_time(); // seconds of game time (steps / 30)
bool trace_on();    // WWHD_MODS_TRACE: log mod decisions
void trace(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

}  // namespace mods
