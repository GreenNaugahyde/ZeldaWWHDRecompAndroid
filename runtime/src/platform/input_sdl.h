#pragma once
#include <SDL3/SDL.h>
namespace input {
// Main-thread calls from the SDL window/event loop.
void handle_event(const SDL_Event& event);
void update();
void set_prompt_window(SDL_Window* window);
}
