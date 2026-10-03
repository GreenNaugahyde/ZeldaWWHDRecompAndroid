// SDL3 keyboard/gamepad input. Stable key IDs preserve existing controls.json mappings.
#include "input_sdl.h"
#include "keycodes.h"
#include "mouse_sdl.h"
#include "../input.h"
#include "../input_map.h"
#include "../runtime.h"
#include "../savestate.h"
#include "../gfx/vulkan/settings.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include <vector>
namespace render { uint64_t frame_count(); }
namespace gfxvk { bool graphics_hotkey(char key, bool activate); }
namespace mods { void filter_pad(input::PadState&); }
namespace input {
static std::mutex g_mu;
static bool g_keys[256]={},g_script_keys[256]={};
static PadState g_pad;
static float g_values[input_map::kPadCount]={};
static bool g_touch=false;static float g_tx=0,g_ty=0;
static std::map<SDL_JoystickID,SDL_Gamepad*> g_controllers;
static SDL_Window* g_prompt_window=nullptr;
static std::function<void(bool,std::u16string)> g_pending,g_done;
static std::u16string g_initial,g_text;
static int g_max_len=0,g_pending_max_len=0;
static std::string g_previous_title;
void set_prompt_window(SDL_Window* window){g_prompt_window=window;}
static int keycode(SDL_Scancode code) {
 switch(code) {
 case SDL_SCANCODE_A: return kVK_ANSI_A;
 case SDL_SCANCODE_B: return kVK_ANSI_B;
 case SDL_SCANCODE_C: return kVK_ANSI_C;
 case SDL_SCANCODE_D: return kVK_ANSI_D;
 case SDL_SCANCODE_E: return kVK_ANSI_E;
 case SDL_SCANCODE_F: return kVK_ANSI_F;
 case SDL_SCANCODE_G: return kVK_ANSI_G;
 case SDL_SCANCODE_H: return kVK_ANSI_H;
 case SDL_SCANCODE_I: return kVK_ANSI_I;
 case SDL_SCANCODE_J: return kVK_ANSI_J;
 case SDL_SCANCODE_K: return kVK_ANSI_K;
 case SDL_SCANCODE_L: return kVK_ANSI_L;
 case SDL_SCANCODE_M: return kVK_ANSI_M;
 case SDL_SCANCODE_N: return kVK_ANSI_N;
 case SDL_SCANCODE_O: return kVK_ANSI_O;
 case SDL_SCANCODE_P: return kVK_ANSI_P;
 case SDL_SCANCODE_Q: return kVK_ANSI_Q;
 case SDL_SCANCODE_R: return kVK_ANSI_R;
 case SDL_SCANCODE_S: return kVK_ANSI_S;
 case SDL_SCANCODE_T: return kVK_ANSI_T;
 case SDL_SCANCODE_U: return kVK_ANSI_U;
 case SDL_SCANCODE_V: return kVK_ANSI_V;
 case SDL_SCANCODE_W: return kVK_ANSI_W;
 case SDL_SCANCODE_X: return kVK_ANSI_X;
 case SDL_SCANCODE_Y: return kVK_ANSI_Y;
 case SDL_SCANCODE_Z: return kVK_ANSI_Z;
 case SDL_SCANCODE_0: return kVK_ANSI_0;
 case SDL_SCANCODE_1: return kVK_ANSI_1;
 case SDL_SCANCODE_2: return kVK_ANSI_2;
 case SDL_SCANCODE_3: return kVK_ANSI_3;
 case SDL_SCANCODE_4: return kVK_ANSI_4;
 case SDL_SCANCODE_5: return kVK_ANSI_5;
 case SDL_SCANCODE_6: return kVK_ANSI_6;
 case SDL_SCANCODE_7: return kVK_ANSI_7;
 case SDL_SCANCODE_8: return kVK_ANSI_8;
 case SDL_SCANCODE_9: return kVK_ANSI_9;
 case SDL_SCANCODE_MINUS: return kVK_ANSI_Minus;
 case SDL_SCANCODE_EQUALS: return kVK_ANSI_Equal;
 case SDL_SCANCODE_LEFTBRACKET: return kVK_ANSI_LeftBracket;
 case SDL_SCANCODE_RIGHTBRACKET: return kVK_ANSI_RightBracket;
 case SDL_SCANCODE_BACKSLASH: return kVK_ANSI_Backslash;
 case SDL_SCANCODE_SEMICOLON: return kVK_ANSI_Semicolon;
 case SDL_SCANCODE_APOSTROPHE: return kVK_ANSI_Quote;
 case SDL_SCANCODE_COMMA: return kVK_ANSI_Comma;
 case SDL_SCANCODE_PERIOD: return kVK_ANSI_Period;
 case SDL_SCANCODE_SLASH: return kVK_ANSI_Slash;
 case SDL_SCANCODE_GRAVE: return kVK_ANSI_Grave;
 case SDL_SCANCODE_NONUSBACKSLASH: return kVK_ISO_Section;
 case SDL_SCANCODE_SPACE: return kVK_Space;
 case SDL_SCANCODE_RETURN: return kVK_Return;
 case SDL_SCANCODE_TAB: return kVK_Tab;
 case SDL_SCANCODE_BACKSPACE: return kVK_Delete;
 case SDL_SCANCODE_DELETE: return kVK_ForwardDelete;
 case SDL_SCANCODE_ESCAPE: return kVK_Escape;
 case SDL_SCANCODE_LSHIFT: return kVK_Shift;
 case SDL_SCANCODE_RSHIFT: return kVK_RightShift;
 case SDL_SCANCODE_LCTRL: return kVK_Control;
 case SDL_SCANCODE_RCTRL: return kVK_RightControl;
 case SDL_SCANCODE_LALT: return kVK_Option;
 case SDL_SCANCODE_RALT: return kVK_RightOption;
 case SDL_SCANCODE_LGUI: return kVK_Command;
 case SDL_SCANCODE_RGUI: return kVK_RightCommand;
 case SDL_SCANCODE_CAPSLOCK: return kVK_CapsLock;
 case SDL_SCANCODE_UP: return kVK_UpArrow;
 case SDL_SCANCODE_DOWN: return kVK_DownArrow;
 case SDL_SCANCODE_LEFT: return kVK_LeftArrow;
 case SDL_SCANCODE_RIGHT: return kVK_RightArrow;
 case SDL_SCANCODE_HOME: return kVK_Home;
 case SDL_SCANCODE_END: return kVK_End;
 case SDL_SCANCODE_PAGEUP: return kVK_PageUp;
 case SDL_SCANCODE_PAGEDOWN: return kVK_PageDown;
 case SDL_SCANCODE_KP_DECIMAL: return kVK_ANSI_KeypadDecimal;
 case SDL_SCANCODE_KP_PERIOD: return kVK_ANSI_KeypadDecimal;
 case SDL_SCANCODE_KP_MULTIPLY: return kVK_ANSI_KeypadMultiply;
 case SDL_SCANCODE_KP_PLUS: return kVK_ANSI_KeypadPlus;
 case SDL_SCANCODE_KP_MINUS: return kVK_ANSI_KeypadMinus;
 case SDL_SCANCODE_KP_DIVIDE: return kVK_ANSI_KeypadDivide;
 case SDL_SCANCODE_KP_ENTER: return kVK_ANSI_KeypadEnter;
 case SDL_SCANCODE_KP_EQUALS: return kVK_ANSI_KeypadEquals;
 case SDL_SCANCODE_NUMLOCKCLEAR: return kVK_ANSI_KeypadClear;
 case SDL_SCANCODE_F1: return kVK_F1;
 case SDL_SCANCODE_F2: return kVK_F2;
 case SDL_SCANCODE_F3: return kVK_F3;
 case SDL_SCANCODE_F4: return kVK_F4;
 case SDL_SCANCODE_F5: return kVK_F5;
 case SDL_SCANCODE_F6: return kVK_F6;
 case SDL_SCANCODE_F7: return kVK_F7;
 case SDL_SCANCODE_F8: return kVK_F8;
 case SDL_SCANCODE_F9: return kVK_F9;
 case SDL_SCANCODE_F10: return kVK_F10;
 case SDL_SCANCODE_F11: return kVK_F11;
 case SDL_SCANCODE_F12: return kVK_F12;
 case SDL_SCANCODE_F13: return kVK_F13;
 case SDL_SCANCODE_F14: return kVK_F14;
 case SDL_SCANCODE_F15: return kVK_F15;
 case SDL_SCANCODE_F16: return kVK_F16;
 case SDL_SCANCODE_KP_0: return kVK_ANSI_Keypad0;
 case SDL_SCANCODE_KP_1: return kVK_ANSI_Keypad1;
 case SDL_SCANCODE_KP_2: return kVK_ANSI_Keypad2;
 case SDL_SCANCODE_KP_3: return kVK_ANSI_Keypad3;
 case SDL_SCANCODE_KP_4: return kVK_ANSI_Keypad4;
 case SDL_SCANCODE_KP_5: return kVK_ANSI_Keypad5;
 case SDL_SCANCODE_KP_6: return kVK_ANSI_Keypad6;
 case SDL_SCANCODE_KP_7: return kVK_ANSI_Keypad7;
 case SDL_SCANCODE_KP_8: return kVK_ANSI_Keypad8;
 case SDL_SCANCODE_KP_9: return kVK_ANSI_Keypad9;
 default:return -1;
 }
}
void set_touch(bool down,float x,float y){std::lock_guard lk(g_mu);g_touch=down;g_tx=x;g_ty=y;}
void release_keys(){std::lock_guard lk(g_mu);memset(g_keys,0,sizeof g_keys);g_touch=false;}
void held_keys(bool* keys){std::lock_guard lk(g_mu);for(int i=0;i<256;i++)keys[i]=g_keys[i]||g_script_keys[i];}
void controller_values(float* out){std::lock_guard lk(g_mu);std::copy(std::begin(g_values),std::end(g_values),out);}
static PadState keyboard_state(bool host){bool keys[256];for(int i=0;i<256;i++)keys[i]=(host&&g_keys[i])||g_script_keys[i];return input_map::keyboard_state(input_map::current(),keys);}
static void open_controller(SDL_JoystickID id){if(!g_controllers.contains(id))if(auto* pad=SDL_OpenGamepad(id))g_controllers[id]=pad;}
void init(){
 input_map::load_startup();
 if(!getenv("WWHD_NO_GAMEPAD")) {
  if(!SDL_InitSubSystem(SDL_INIT_GAMEPAD)){LOG("[input] SDL gamepad initialization: %s",SDL_GetError());return;}
  int count=0;auto* ids=SDL_GetGamepads(&count);for(int i=0;i<count;i++)open_controller(ids[i]);SDL_free(ids);
 }
}
static std::string utf8(const std::u16string& text){
 std::string out;
 for(size_t i=0;i<text.size();i++){
  uint32_t c=text[i];if(c>=0xD800&&c<=0xDBFF&&i+1<text.size()&&text[i+1]>=0xDC00&&text[i+1]<=0xDFFF)c=0x10000+((c-0xD800)<<10)+(text[++i]-0xDC00);
  if(c<0x80)out.push_back((char)c);
  else if(c<0x800){out.push_back((char)(0xC0|(c>>6)));out.push_back((char)(0x80|(c&63)));}
  else if(c<0x10000){out.push_back((char)(0xE0|(c>>12)));out.push_back((char)(0x80|((c>>6)&63)));out.push_back((char)(0x80|(c&63)));}
  else{out.push_back((char)(0xF0|(c>>18)));out.push_back((char)(0x80|((c>>12)&63)));out.push_back((char)(0x80|((c>>6)&63)));out.push_back((char)(0x80|(c&63)));}
 }return out;
}
static void show_prompt(){if(g_prompt_window)SDL_SetWindowTitle(g_prompt_window,("Enter text (Enter confirms, Escape cancels): "+utf8(g_text)).c_str());}
static void finish_prompt(bool ok){
 auto done=std::move(g_done);auto text=std::move(g_text);
 if(g_prompt_window){SDL_StopTextInput(g_prompt_window);SDL_SetWindowTitle(g_prompt_window,g_previous_title.c_str());}
 release_keys();if(done)done(ok,std::move(text));
}
void handle_event(const SDL_Event& event){
 if(mods::handle_mouse_event(event))return;
 if(event.type==SDL_EVENT_GAMEPAD_ADDED&&!getenv("WWHD_NO_GAMEPAD"))open_controller(event.gdevice.which);
 if(event.type==SDL_EVENT_GAMEPAD_REMOVED){auto i=g_controllers.find(event.gdevice.which);if(i!=g_controllers.end()){SDL_CloseGamepad(i->second);g_controllers.erase(i);}}
 if(event.type==SDL_EVENT_WINDOW_FOCUS_LOST)release_keys();
 if(g_done){
  if(event.type==SDL_EVENT_KEY_DOWN){
   if(event.key.scancode==SDL_SCANCODE_RETURN){finish_prompt(true);return;}
   if(event.key.scancode==SDL_SCANCODE_ESCAPE){finish_prompt(false);return;}
   if(event.key.scancode==SDL_SCANCODE_BACKSPACE&&!g_text.empty()){char16_t c=g_text.back();g_text.pop_back();if(c>=0xDC00&&c<=0xDFFF&&!g_text.empty()&&g_text.back()>=0xD800&&g_text.back()<=0xDBFF)g_text.pop_back();show_prompt();}
  }
  if(event.type==SDL_EVENT_TEXT_INPUT){
   const unsigned char* p=(const unsigned char*)event.text.text;
   while(*p){uint32_t c=*p++;int rest=0;if(c>=0xF0){c&=7;rest=3;}else if(c>=0xE0){c&=15;rest=2;}else if(c>=0xC0){c&=31;rest=1;}while(rest--&&*p)c=(c<<6)|(*p++&63);
    size_t units=c>0xFFFF?2:1;if(g_text.size()+units>(size_t)g_max_len)break;
    if(c>0xFFFF){c-=0x10000;g_text.push_back((char16_t)(0xD800+(c>>10)));g_text.push_back((char16_t)(0xDC00+(c&1023)));}else g_text.push_back((char16_t)c);
   }show_prompt();
  }return;
 }
 if(getenv("WWHD_NO_HOST_INPUT"))return;
 // Save states belong to the game window, not auxiliary controls/text windows.
 if((event.type==SDL_EVENT_KEY_DOWN||event.type==SDL_EVENT_KEY_UP) &&
    g_prompt_window && event.key.windowID==SDL_GetWindowID(g_prompt_window) &&
    event.key.scancode>=SDL_SCANCODE_F1 && event.key.scancode<=SDL_SCANCODE_F5){
  if(event.type==SDL_EVENT_KEY_DOWN && !event.key.repeat){
   const int slot=int(event.key.scancode-SDL_SCANCODE_F1)+1;
   if(event.key.mod&SDL_KMOD_SHIFT)ss::request_save(slot);
   else ss::request_load(slot);
  }
  return;
 }
 // Graphics shortcuts are plain keys scoped to the game window; repeats never toggle.
 if((event.type==SDL_EVENT_KEY_DOWN||event.type==SDL_EVENT_KEY_UP) && g_prompt_window &&
    event.key.windowID==SDL_GetWindowID(g_prompt_window) &&
    !(event.key.mod&(SDL_KMOD_CTRL|SDL_KMOD_ALT|SDL_KMOD_GUI))){
  char action=0;
  switch(event.key.scancode){
   case SDL_SCANCODE_R: action='R';break; case SDL_SCANCODE_O: action='O';break;
   case SDL_SCANCODE_M: action='M';break; case SDL_SCANCODE_N: action='N';break;
   case SDL_SCANCODE_8: action='8';break; case SDL_SCANCODE_6: action='6';break;
   case SDL_SCANCODE_7: action='7';break; default:break;
  }
  if(action && gfxvk::graphics_hotkey(action,event.type==SDL_EVENT_KEY_DOWN&&!event.key.repeat))return;
 }
 if(event.type==SDL_EVENT_KEY_DOWN||event.type==SDL_EVENT_KEY_UP){int code=keycode(event.key.scancode);if(code>=0){std::lock_guard lk(g_mu);g_keys[code]=event.type==SDL_EVENT_KEY_DOWN;}}
}
void update(){
 mods::update_mouse();
 std::function<void(bool,std::u16string)> cancelled;
 {std::lock_guard lk(g_mu);if(g_pending&&!g_done){g_done=std::move(g_pending);g_text=std::move(g_initial);g_max_len=g_pending_max_len;memset(g_keys,0,sizeof g_keys);if(!g_prompt_window)g_prompt_window=SDL_GetKeyboardFocus();if(g_prompt_window){g_previous_title=SDL_GetWindowTitle(g_prompt_window);if(SDL_StartTextInput(g_prompt_window))show_prompt();else cancelled=std::move(g_done);}else cancelled=std::move(g_done);}}
 if(cancelled)cancelled(false,{});
 float v[input_map::kPadCount]={};using namespace input_map;
 auto put=[&](int p,float x){v[p]=std::max(v[p],x);};
 for(auto [id,pad]:g_controllers){
  if(!SDL_GamepadConnected(pad))continue;
  auto button=[&](SDL_GamepadButton b,int p){put(p,SDL_GetGamepadButton(pad,b)?1.f:0.f);};
  button(SDL_GAMEPAD_BUTTON_SOUTH,kPadA);button(SDL_GAMEPAD_BUTTON_EAST,kPadB);button(SDL_GAMEPAD_BUTTON_WEST,kPadX);button(SDL_GAMEPAD_BUTTON_NORTH,kPadY);
  button(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER,kPadLB);button(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,kPadRB);
  button(SDL_GAMEPAD_BUTTON_START,kPadMenu);button(SDL_GAMEPAD_BUTTON_BACK,kPadOptions);button(SDL_GAMEPAD_BUTTON_GUIDE,kPadHome);
  button(SDL_GAMEPAD_BUTTON_LEFT_STICK,kPadL3);button(SDL_GAMEPAD_BUTTON_RIGHT_STICK,kPadR3);
  button(SDL_GAMEPAD_BUTTON_DPAD_UP,kPadDUp);button(SDL_GAMEPAD_BUTTON_DPAD_DOWN,kPadDDown);button(SDL_GAMEPAD_BUTTON_DPAD_LEFT,kPadDLeft);button(SDL_GAMEPAD_BUTTON_DPAD_RIGHT,kPadDRight);
  put(kPadLT,std::max(0.f,SDL_GetGamepadAxis(pad,SDL_GAMEPAD_AXIS_LEFT_TRIGGER)/32767.f));put(kPadRT,std::max(0.f,SDL_GetGamepadAxis(pad,SDL_GAMEPAD_AXIS_RIGHT_TRIGGER)/32767.f));
  auto stick=[&](SDL_GamepadAxis ax,SDL_GamepadAxis ay,int up,int down,int left,int right){float x=SDL_GetGamepadAxis(pad,ax)/32768.f,y=SDL_GetGamepadAxis(pad,ay)/32768.f;put(right,std::max(x,0.f));put(left,std::max(-x,0.f));put(up,std::max(-y,0.f));put(down,std::max(y,0.f));};
  stick(SDL_GAMEPAD_AXIS_LEFTX,SDL_GAMEPAD_AXIS_LEFTY,kPadLSUp,kPadLSDown,kPadLSLeft,kPadLSRight);stick(SDL_GAMEPAD_AXIS_RIGHTX,SDL_GAMEPAD_AXIS_RIGHTY,kPadRSUp,kPadRSDown,kPadRSLeft,kPadRSRight);
 }
 auto state=input_map::controller_state(input_map::current(),v);std::lock_guard lk(g_mu);std::copy(std::begin(v),std::end(v),g_values);g_pad=state;
}
void prompt_text(const std::u16string& initial,int max_len,std::function<void(bool,std::u16string)> done){std::lock_guard lk(g_mu);g_initial=initial;g_pending_max_len=std::max(0,max_len);if(g_initial.size()>(size_t)g_pending_max_len)g_initial.resize(g_pending_max_len);g_pending=std::move(done);}
// debug: WWHD_PRESS=1000-1010:8000,1500-1505:0008 holds VPAD buttons (hex) during TV frame ranges
struct Press { uint64_t from, to; uint32_t bits; };
static std::vector<Press> scripted() {
    std::vector<Press> v;
    if (const char* e = getenv("WWHD_PRESS")) {
        unsigned long long a, b; unsigned bits; int n;
        while (sscanf(e, "%llu-%llu:%x%n", &a, &b, &bits, &n) == 3) {
            v.push_back({a, b, bits});
            e += n;
            if (*e != ',') break;
            e++;
        }
    }
    return v;
}

// debug: WWHD_KEYS=1200-1210:K,1300-1305:LeftShift+J holds keyboard keys (input_map key names)
// during TV frame ranges. Unlike WWHD_PRESS they go through the controls mapping, so a test can
// check a remapped controls file (also with WWHD_NO_HOST_INPUT=1).
struct KeyPress { uint64_t from, to; std::vector<int> codes; };
static std::vector<KeyPress> scripted_keys() {
    std::vector<KeyPress> v;
    if (const char* e = getenv("WWHD_KEYS")) {
        unsigned long long a, b; int n;
        while (sscanf(e, "%llu-%llu:%n", &a, &b, &n) == 2) {
            e += n;
            KeyPress kp{a, b, {}};
            for (;;) {
                size_t len = strcspn(e, "+,");
                int code = input_map::key_from_id(std::string(e, len));
                if (code == input_map::kNoKey) LOG("[input] WWHD_KEYS: unknown key %.*s", (int)len, e);
                else kp.codes.push_back(code);
                e += len;
                if (*e != '+') break;
                e++;
            }
            v.push_back(kp);
            if (*e != ',') break;
            e++;
        }
    }
    return v;
}

// debug: WWHD_STICK=9400-9600:0:1,... holds the left stick at (x, y) during TV frame ranges
// (WWHD_RSTICK: the same for the right stick)
struct Stick { uint64_t from, to; float x, y; };
static std::vector<Stick> scripted_stick(const char* var = "WWHD_STICK") {
    std::vector<Stick> v;
    if (const char* e = getenv(var)) {
        unsigned long long a, b; float x, y; int n;
        while (sscanf(e, "%llu-%llu:%f:%f%n", &a, &b, &x, &y, &n) == 4) {
            v.push_back({a, b, x, y});
            e += n;
            if (*e != ',') break;
            e++;
        }
    }
    return v;
}

// debug: timed test scenario, in real seconds from TV frame WWHD_TEST_ORIGIN (so a 30 fps and a 60 fps
// run get the same input at the same real time):
//   WWHD_TEST_STICK=2-5:0:1,...   left stick (x, y) from 2 s to 5 s
//   WWHD_TEST_RSTICK=2-5:1:0,...  right stick
//   WWHD_TEST_PRESS=3-3.1:8000    buttons (hex)
//   WWHD_TEST_MODE=2@0.5          60 fps mode at 0.5 s (0 off, 1 interpolation, 2 true 60)
//   WWHD_TEST_END=12              writes the file "test_done" at 12 s (the test script stops the game)
namespace {
struct TimedStick { double from, to; float x, y; };
struct TimedPress { double from, to; uint32_t bits; };
struct Scenario {
    uint64_t origin = 0;
    std::vector<TimedStick> sticks, rsticks;
    std::vector<TimedPress> presses;
    int mode = -1;
    double mode_at = 0, end = 0;
    Scenario() {
        if (const char* e = getenv("WWHD_TEST_ORIGIN")) origin = strtoull(e, nullptr, 10);
        auto timed_sticks = [](const char* e, std::vector<TimedStick>& out) {
            double a, b; float x, y; int n;
            while (e && sscanf(e, "%lf-%lf:%f:%f%n", &a, &b, &x, &y, &n) == 4) {
                out.push_back({a, b, x, y});
                e += n;
                if (*e != ',') break;
                e++;
            }
        };
        timed_sticks(getenv("WWHD_TEST_STICK"), sticks);
        timed_sticks(getenv("WWHD_TEST_RSTICK"), rsticks);
        if (const char* e = getenv("WWHD_TEST_PRESS")) {
            double a, b; unsigned bits; int n;
            while (sscanf(e, "%lf-%lf:%x%n", &a, &b, &bits, &n) == 3) {
                presses.push_back({a, b, bits});
                e += n;
                if (*e != ',') break;
                e++;
            }
        }
        if (const char* e = getenv("WWHD_TEST_MODE")) sscanf(e, "%d@%lf", &mode, &mode_at);
        if (const char* e = getenv("WWHD_TEST_END")) end = atof(e);
    }
};
}  // namespace
}  // namespace input
namespace interp { void set_mode(int m); uint64_t logic_steps(); }
namespace input {
static void apply_scenario(PadState& s) {
    static const Scenario sc;
    if (!sc.origin || render::frame_count() < sc.origin) return;
    // scenario time = game time: full logic steps / 30 (frame-time hitches don't shift the input)
    static const uint64_t s0 = [] {
        LOG("[test] origin at guest time %.4f s, logic step %llu", (double)timebase::now() / timebase::kTicksPerSec,
            (unsigned long long)interp::logic_steps());
        return interp::logic_steps();
    }();
    double t = (double)(interp::logic_steps() - s0) / 30.0;
    static std::atomic<bool> mode_set{false}, ended{false};
    static std::atomic<int> dbg{0};
    if (getenv("WWHD_TEST_DEBUG") && dbg++ % 30 == 0) LOG("[test] t=%.3f frame %llu", t, (unsigned long long)render::frame_count());
    if (sc.mode >= 0 && t >= sc.mode_at && !mode_set.exchange(true)) {
        LOG("[test] t=%.3f s: 60 fps mode %d", t, sc.mode);
        interp::set_mode(sc.mode);
    }
    if (sc.end > 0 && t >= sc.end && !ended.exchange(true)) {
        LOG("[test] t=%.3f s: end", t);
        if (FILE* f = fopen("test_done", "w")) fclose(f);
    }
    for (auto& p : sc.sticks)
        if (t >= p.from && t < p.to) { s.lx = p.x; s.ly = p.y; }
    for (auto& p : sc.rsticks)
        if (t >= p.from && t < p.to) { s.rx = p.x; s.ry = p.y; }
    for (auto& p : sc.presses)
        if (t >= p.from && t < p.to) s.buttons |= p.bits;
}

static std::atomic<bool> g_pro{getenv("WWHD_PRO_CONTROLLER") != nullptr};
bool pro_controller() { return g_pro.load(std::memory_order_relaxed); }
void set_pro_controller(bool on) { g_pro = on; LOG("[input] keyboard/controller act as %s", on ? "Pro Controller" : "GamePad"); }

PadState read() {
    static const std::vector<Press> script = scripted();
    static const std::vector<Stick> sticks = scripted_stick();
    static const std::vector<KeyPress> keys = scripted_keys();
    static const std::vector<Stick> rsticks = scripted_stick("WWHD_RSTICK");
    std::lock_guard<std::mutex> lk(g_mu);
    // debug: WWHD_NO_HOST_INPUT=1 ignores keyboard and host controllers (scripted test runs)
    static const bool no_host = getenv("WWHD_NO_HOST_INPUT") != nullptr;
    if (!keys.empty()) {
        memset(g_script_keys, 0, sizeof g_script_keys);
        for (auto& p : keys)
            if (render::frame_count() >= p.from && render::frame_count() <= p.to)
                for (int c : p.codes) g_script_keys[c] = true;
    }
    PadState k = keyboard_state(!no_host), s = g_pad;
    if (no_host) s = PadState{};
    for (auto& p : script)
        if (render::frame_count() >= p.from && render::frame_count() <= p.to) s.buttons |= p.bits;
    for (auto& p : sticks)
        if (render::frame_count() >= p.from && render::frame_count() <= p.to) { s.lx = p.x; s.ly = p.y; }
    for (auto& p : rsticks)
        if (render::frame_count() >= p.from && render::frame_count() <= p.to) { s.rx = p.x; s.ry = p.y; }
    apply_scenario(s);
    s.buttons |= k.buttons;
    if (k.lx || k.ly) { s.lx = k.lx; s.ly = k.ly; }
    if (k.rx || k.ry) { s.rx = k.rx; s.ry = k.ry; }
    s.touch = g_touch;
    s.tx = g_tx;
    s.ty = g_ty;
    // debug: WWHD_LOG_BUTTONS=1 logs every change of the merged button bits
    static const bool log_buttons = getenv("WWHD_LOG_BUTTONS") != nullptr;
    static uint32_t last_buttons = 0;
    if (log_buttons && s.buttons != last_buttons) {
        LOG("[input] frame %llu buttons %04X", (unsigned long long)render::frame_count(), s.buttons);
        last_buttons = s.buttons;
    }
    mods::filter_pad(s);  // gameplay mods: mouse camera, wheel -> R3
    return s;
}


} // namespace input
