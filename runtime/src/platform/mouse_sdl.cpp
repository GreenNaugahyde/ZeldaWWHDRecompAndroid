#include "mouse_sdl.h"
#include "keycodes.h"
#include "../mods/mods.h"
#include "../runtime.h"
#include <atomic>
#include <cstdlib>

namespace mods {
namespace {
SDL_Window* tv = nullptr;
std::atomic<bool> captured{false};
std::atomic<bool> release_requested{false};
void release_now() {
    release_requested.store(false);
    if (!captured.exchange(false)) return;
    if (tv) SDL_SetWindowRelativeMouseMode(tv, false);
    mouse_button(0, false);
    mouse_button(1, false);
    LOG("[mods] mouse released");
}
}
bool mouse_captured() { return captured.load(); }
void mouse_release() { release_requested.store(true); }
void mouse_init(void* window) { release_now(); tv = static_cast<SDL_Window*>(window); }
bool host_key_down(uint16_t code) {
    if (code == kVK_Escape && mouse_captured()) { mouse_release(); return true; }
    return false;
}
void update_mouse() {
    if (release_requested.load() || (captured.load() && !mouse_camera())) release_now();
}
bool handle_mouse_event(const SDL_Event& event) {
    update_mouse();
    if (!tv || getenv("WWHD_NO_HOST_INPUT")) return false;
    const SDL_WindowID id = SDL_GetWindowID(tv);
    if (event.type == SDL_EVENT_WINDOW_FOCUS_LOST && event.window.windowID == id) release_now();
    if (event.type == SDL_EVENT_KEY_DOWN && event.key.windowID == id &&
        event.key.scancode == SDL_SCANCODE_ESCAPE && captured.load()) {
        release_now(); return true;
    }
    if (event.type == SDL_EVENT_MOUSE_WHEEL && event.wheel.windowID == id && first_person_wheel()) {
        float y = event.wheel.y;
        if (event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED) y = -y;
        mouse_wheel(y); return true;
    }
    if (!mouse_camera()) return false;
    if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && event.button.windowID == id &&
        event.button.button == SDL_BUTTON_LEFT && !captured.load()) {
        if (SDL_SetWindowRelativeMouseMode(tv, true)) {
            captured.store(true);
            LOG("[mods] mouse captured (Esc or middle click releases)");
            return true;
        }
        return false;
    }
    if (!captured.load()) return false;
    if (event.type == SDL_EVENT_MOUSE_MOTION && event.motion.windowID == id) {
        mouse_add(event.motion.xrel, event.motion.yrel); return true;
    }
    if ((event.type == SDL_EVENT_MOUSE_BUTTON_DOWN || event.type == SDL_EVENT_MOUSE_BUTTON_UP) && event.button.windowID == id) {
        const bool down = event.type == SDL_EVENT_MOUSE_BUTTON_DOWN;
        if (event.button.button == SDL_BUTTON_LEFT) mouse_button(0, down);
        else if (event.button.button == SDL_BUTTON_RIGHT) mouse_button(1, down);
        else if (event.button.button == SDL_BUTTON_MIDDLE && down) release_now();
        return true;
    }
    return false;
}
}
