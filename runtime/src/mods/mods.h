// Optional gameplay mods (Gameplay menu). All off by default; each can be switched live.
//   camera.cpp  direct right-stick camera, mouse camera, first person on R3 / mouse wheel
//   mouse.mm    mouse capture in the game window (macOS events)
//   turbo.cpp   quick doors, fast scene changes and fast forward (extra logic steps while they run)
//   mods.cpp    faster running (true60_link.cpp's posMoveFromFootPos site applies it)
// Test/start-up switches: WWHD_MOD_<NAME>=1 (see mods.cpp).
#pragma once
#include <cstdint>

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
// when each applies: 0 always, 1 while L3 is held, 2 L3 switches it on and off (a press switches
// the one for what Link is doing: swimming or not)
int run_mode();
void set_run_mode(int m);
int swim_mode();
void set_swim_mode(int m);
void run_input(uint32_t buttons);  // filter_pad: the GamePad buttons of this read (L3 edges, ZR)
// fast forward (turbo.cpp): while ZR is held, cutscenes / dialogues run several logic steps per
// frame
bool ff_cutscenes();
void set_ff_cutscenes(bool on);
bool ff_dialogues();
void set_ff_dialogues(bool on);
bool ff_button();  // ZR is held (last pad read)
int event_mode_now();  // turbo.cpp: 0 no event, 1 dialogue, 2 / 3 cutscene
// for daPy_lk_c::posMoveFromFootPos (true60_link.cpp): the factor for Link's horizontal movement
// this step (run_speed() while he runs, else 1); link = the daPy_lk_c
float link_move_factor(uint32_t link);

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

// shared helpers (mods.cpp)
uint64_t step();    // full logic steps so far (interp::logic_steps)
double game_time(); // seconds of game time (steps / 30)
bool trace_on();    // WWHD_MODS_TRACE: log mod decisions
void trace(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

}  // namespace mods
