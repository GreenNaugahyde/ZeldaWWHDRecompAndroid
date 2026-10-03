// Tests only the host input boundary. No game binary or renderer is involved.
#include <SDL3/SDL.h>
#include <cassert>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include "input.h"
#include "input_map.h"
#include "platform/input_sdl.h"
static int savedSlot=0, loadedSlot=0, saveRequests=0, loadRequests=0;
namespace ss { void request_save(int slot){savedSlot=slot;++saveRequests;} void request_load(int slot){loadedSlot=slot;++loadRequests;} }
static int graphicsRequests=0; static char graphicsKey=0;
namespace render { uint64_t frame_count(){return 0;} }
namespace gfxvk { bool graphics_hotkey(char key,bool activate){if(activate){++graphicsRequests;graphicsKey=key;}return true;} }
namespace mods { void filter_pad(input::PadState&){} bool mouse_camera(){return false;} bool first_person_wheel(){return false;} void mouse_button(int,bool){} void mouse_add(float,float){} void mouse_wheel(float){} }
namespace interp { void set_mode(int){} uint64_t logic_steps(){return 0;} }
namespace timebase { uint64_t now(){return 0;} }
void log_msg(const char*,...){}
int main(){
 SDL_SetHint(SDL_HINT_VIDEO_DRIVER,"dummy");
 assert(SDL_Init(SDL_INIT_EVENTS|SDL_INIT_VIDEO));
 input::init();input_map::set_current(input_map::Mapping::defaults(),false);
 auto key=[](SDL_Scancode code,bool down){SDL_Event e{};e.type=down?SDL_EVENT_KEY_DOWN:SDL_EVENT_KEY_UP;e.key.scancode=code;input::handle_event(e);};
 key(SDL_SCANCODE_K,true);assert(input::read().buttons&input::kA);
 key(SDL_SCANCODE_K,false);assert(!(input::read().buttons&input::kA));
 key(SDL_SCANCODE_W,true);assert(input::read().ly==1);
 key(SDL_SCANCODE_D,true);assert(input::read().lx>0.7f&&input::read().lx<0.71f);
 key(SDL_SCANCODE_LSHIFT,true);assert(input::read().buttons&input::kZL);
 SDL_Event focus{};focus.type=SDL_EVENT_WINDOW_FOCUS_LOST;input::handle_event(focus);assert(input::read().buttons==0&&input::read().lx==0);
 auto map=input_map::Mapping::defaults();map.keys[input_map::kA]={input_map::key_from_id("J"),input_map::kNoKey};map.keys[input_map::kB]={input_map::kNoKey,input_map::kNoKey};input_map::set_current(map,false);
 key(SDL_SCANCODE_J,true);assert(input::read().buttons==input::kA);
 input::set_touch(true,0.25f,0.75f);auto pad=input::read();assert(pad.touch&&pad.tx==0.25f&&pad.ty==0.75f);
 input::set_pro_controller(true);assert(input::pro_controller());input::set_pro_controller(false);
 bool held[256];input::held_keys(held);assert(held[input_map::key_from_id("J")]);
 input::release_keys();input::held_keys(held);assert(!held[input_map::key_from_id("J")]);
 input::update();float values[input_map::kPadCount];input::controller_values(values);assert(values[input_map::kPadA]==0);
 // Hidden dummy-driver windows exercise routing without a renderer or GPU.
 auto* game=SDL_CreateWindow("Game",32,32,SDL_WINDOW_HIDDEN);
 auto* controls=SDL_CreateWindow("Controls",32,32,SDL_WINDOW_HIDDEN);
 assert(game&&controls);input::set_prompt_window(game);
 auto stateKey=[&](int slot,SDL_Keymod mod,bool repeat=false,SDL_Window* window=nullptr,Uint32 type=SDL_EVENT_KEY_DOWN){
  SDL_Event e{};e.type=type;e.key.scancode=SDL_Scancode(SDL_SCANCODE_F1+slot-1);
  e.key.windowID=SDL_GetWindowID(window?window:game);e.key.mod=mod;e.key.repeat=repeat;input::handle_event(e);
 };
 for(int slot=1;slot<=5;++slot){
  stateKey(slot,SDL_KMOD_NONE);assert(loadedSlot==slot&&loadRequests==slot);
  stateKey(slot,SDL_KMOD_LSHIFT);assert(savedSlot==slot&&saveRequests==2*slot-1);
  stateKey(slot,SDL_KMOD_RSHIFT);assert(savedSlot==slot&&saveRequests==2*slot);
 }
 const int saves=saveRequests,loads=loadRequests;
 stateKey(1,SDL_KMOD_LSHIFT,true);stateKey(1,SDL_KMOD_NONE,false,controls);
 stateKey(1,SDL_KMOD_LSHIFT,false,nullptr,SDL_EVENT_KEY_UP);
 stateKey(6,SDL_KMOD_NONE);input::set_prompt_window(nullptr);stateKey(1,SDL_KMOD_NONE);
 assert(saveRequests==saves&&loadRequests==loads);
 input::set_prompt_window(game);input::release_keys();
 for(int slot=1;slot<=5;++slot)stateKey(slot,SDL_KMOD_NONE,true);
 input::held_keys(held);
 for(const char* id:{"F1","F2","F3","F4","F5"})assert(!held[input_map::key_from_id(id)]);
 auto graphicsEvent=[&](SDL_Scancode code,SDL_Window* window, bool repeat=false, Uint32 type=SDL_EVENT_KEY_DOWN,SDL_Keymod mod=SDL_KMOD_NONE){SDL_Event e{};e.type=type;e.key.scancode=code;e.key.windowID=SDL_GetWindowID(window);e.key.repeat=repeat;e.key.mod=mod;input::handle_event(e);};
 for(auto code:{SDL_SCANCODE_R,SDL_SCANCODE_O,SDL_SCANCODE_M,SDL_SCANCODE_N,SDL_SCANCODE_8,SDL_SCANCODE_6,SDL_SCANCODE_7}){
  int before=graphicsRequests;graphicsEvent(code,game);assert(graphicsRequests==before+1);
  graphicsEvent(code,game,true);graphicsEvent(code,game,false,SDL_EVENT_KEY_UP);graphicsEvent(code,controls);graphicsEvent(code,game,false,SDL_EVENT_KEY_DOWN,SDL_KMOD_CTRL);assert(graphicsRequests==before+1);
 }
 assert(graphicsKey=='7');input::release_keys();
 SDL_setenv_unsafe("WWHD_NO_HOST_INPUT","1",1);graphicsEvent(SDL_SCANCODE_R,game);assert(graphicsRequests==7);SDL_unsetenv_unsafe("WWHD_NO_HOST_INPUT");
 SDL_DestroyWindow(controls);SDL_DestroyWindow(game);input::set_prompt_window(nullptr);
 SDL_Quit();puts("input_sdl_test: keyboard mapping, focus, touch, Pro mode, guarded save-state shortcuts passed");
}
