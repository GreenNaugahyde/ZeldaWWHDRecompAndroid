// Exercise event routing and capture transitions without opening a desktop window.
// The SDL window boundary is simulated; the production mouse event code is linked.
#include <SDL3/SDL.h>
#include <cassert>
#include <cstdio>
#include "mods/mods.h"
#include "platform/mouse_sdl.h"
#include "platform/keycodes.h"
static bool camera = true, wheel_enabled = true, relative = false;
static float dx = 0, dy = 0, wheel = 0;
static bool buttons[2]{};
extern "C" SDL_WindowID SDL_GetWindowID(SDL_Window*) { return 17; }
extern "C" bool SDL_SetWindowRelativeMouseMode(SDL_Window*, bool value) { relative = value; return true; }
void log_msg(const char*, ...) {}
namespace mods {
bool mouse_camera() { return camera; }
bool first_person_wheel() { return wheel_enabled; }
void mouse_add(float x, float y) { dx += x; dy += y; }
void mouse_wheel(float y) { wheel += y; }
void mouse_button(int i, bool value) { buttons[i] = value; }
}
int main() {
    mods::mouse_init(reinterpret_cast<void*>(1));
    SDL_Event e{};
    auto capture = [&] {
        e = {}; e.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
        e.button.windowID = 17; e.button.button = SDL_BUTTON_LEFT;
        assert(mods::handle_mouse_event(e));
        assert(relative && mods::mouse_captured());
    };
    capture();
    e = {}; e.type = SDL_EVENT_MOUSE_MOTION; e.motion.windowID = 18;
    e.motion.xrel = 4; e.motion.yrel = -3;
    assert(!mods::handle_mouse_event(e)); assert(dx == 0 && dy == 0);
    e.motion.windowID = 17; assert(mods::handle_mouse_event(e));
    assert(dx == 4 && dy == -3);
    e = {}; e.type = SDL_EVENT_MOUSE_BUTTON_DOWN; e.button.windowID = 17;
    e.button.button = SDL_BUTTON_RIGHT; assert(mods::handle_mouse_event(e)); assert(buttons[1]);
    e = {}; e.type = SDL_EVENT_MOUSE_WHEEL; e.wheel.windowID = 17;
    e.wheel.y = 2; e.wheel.direction = SDL_MOUSEWHEEL_FLIPPED;
    assert(mods::handle_mouse_event(e)); assert(wheel == -2);
    e = {}; e.type = SDL_EVENT_WINDOW_FOCUS_LOST; e.window.windowID = 17;
    mods::handle_mouse_event(e);
    assert(!relative && !mods::mouse_captured() && !buttons[1]);
    capture(); assert(mods::host_key_down(kVK_Escape)); mods::update_mouse();
    assert(!relative && !mods::mouse_captured());
    capture(); camera = false; mods::update_mouse();
    assert(!relative && !mods::mouse_captured()); camera = true;
    capture(); e = {}; e.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
    e.button.windowID = 17; e.button.button = SDL_BUTTON_MIDDLE;
    assert(mods::handle_mouse_event(e)); assert(!relative && !mods::mouse_captured());
    mods::mouse_init(nullptr);
    puts("mouse_sdl_test: capture, relative motion, window routing, wheel and release passed");
}
