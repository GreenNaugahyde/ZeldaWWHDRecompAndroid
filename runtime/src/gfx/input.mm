// Keyboard and GameController input, merged into one GamePad state.
//
// Which key or controller input drives which GamePad input is the controls mapping (input_map.h,
// edited in Input > Controls…). Defaults: WASD move, arrows camera, K/Space = A, J = B, L = X, I = Y,
// Q = L, E = R, Left Shift = ZL, C = ZR, Enter = +, Tab = -, H = Home, 1-4 = D-pad up/down/left/right,
// X = L-stick click, V = R-stick click; controllers use button positions (Xbox "A" = Wii U B).
#import <AppKit/AppKit.h>
#import <GameController/GameController.h>
#include <Carbon/Carbon.h>  // kVK_* key codes

#include <cmath>
#include <mutex>

#include "input.h"
#include "input_map.h"
#include "renderer.h"
#include "../runtime.h"

#include <vector>

namespace gfx { void request_capture(); bool menu_hotkey(uint16_t keyCode); bool controls_window_is_key(); }
namespace mods { void filter_pad(input::PadState& s); bool host_key_down(uint16_t code); }  // mods/

namespace input {

static std::mutex g_mu;
static bool g_keys[256];
static PadState g_pad;  // controller part, refreshed on the main thread
static bool g_touch;
static float g_tx, g_ty;

void set_touch(bool down, float x, float y) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_touch = down;
    g_tx = x;
    g_ty = y;
}

// keys held by WWHD_KEYS (debug), merged with the real keyboard before the mapping is applied
static bool g_script_keys[256];

static PadState keyboard_state(bool use_host) {
    bool keys[256];
    for (int i = 0; i < 256; i++) keys[i] = (use_host && g_keys[i]) || g_script_keys[i];
    return input_map::keyboard_state(input_map::current(), keys);
}

// every mappable controller input, 0..1, the strongest over all connected controllers
void controller_values(float* v) {
    using namespace input_map;
    for (int i = 0; i < kPadCount; i++) v[i] = 0;
    auto put = [&](int p, float x) { if (x > v[p]) v[p] = x; };
    for (GCController* c in [GCController controllers]) {
        GCExtendedGamepad* g = c.extendedGamepad;
        if (!g) continue;
        auto b = [&](GCControllerButtonInput* in, int p) {
            if (in) put(p, in.isPressed ? std::max(in.value, 0.51f) : std::min(in.value, 0.5f));
        };
        b(g.buttonA, kPadA); b(g.buttonB, kPadB); b(g.buttonX, kPadX); b(g.buttonY, kPadY);
        b(g.leftShoulder, kPadLB); b(g.rightShoulder, kPadRB);
        b(g.leftTrigger, kPadLT); b(g.rightTrigger, kPadRT);
        b(g.buttonMenu, kPadMenu); b(g.buttonOptions, kPadOptions); b(g.buttonHome, kPadHome);
        b(g.leftThumbstickButton, kPadL3); b(g.rightThumbstickButton, kPadR3);
        b(g.dpad.up, kPadDUp); b(g.dpad.down, kPadDDown); b(g.dpad.left, kPadDLeft); b(g.dpad.right, kPadDRight);
        auto stick = [&](GCControllerDirectionPad* d, int up, int down, int left, int right) {
            float x = d.xAxis.value, y = d.yAxis.value;
            put(right, std::max(x, 0.f)); put(left, std::max(-x, 0.f));
            put(up, std::max(y, 0.f)); put(down, std::max(-y, 0.f));
        };
        stick(g.leftThumbstick, kPadLSUp, kPadLSDown, kPadLSLeft, kPadLSRight);
        stick(g.rightThumbstick, kPadRSUp, kPadRSDown, kPadRSLeft, kPadRSRight);
    }
}

static PadState controller_state() {
    float v[input_map::kPadCount];
    controller_values(v);
    return input_map::controller_state(input_map::current(), v);
}

void held_keys(bool* keys) {
    std::lock_guard<std::mutex> lk(g_mu);
    for (int i = 0; i < 256; i++) keys[i] = g_keys[i] || g_script_keys[i];
}

void release_keys() {
    std::lock_guard<std::mutex> lk(g_mu);
    memset(g_keys, 0, sizeof(g_keys));
}

// modifier keys arrive as flagsChanged; the device-dependent bits tell left from right
static bool modifier_down(uint16_t code, NSEventModifierFlags f) {
    switch (code) {
    case kVK_Shift: return f & 0x02;          // NX_DEVICELSHIFTKEYMASK
    case kVK_RightShift: return f & 0x04;     // NX_DEVICERSHIFTKEYMASK
    case kVK_Control: return f & 0x01;        // NX_DEVICELCTLKEYMASK
    case kVK_RightControl: return f & 0x2000; // NX_DEVICERCTLKEYMASK
    case kVK_Option: return f & 0x20;         // NX_DEVICELALTKEYMASK
    case kVK_RightOption: return f & 0x40;    // NX_DEVICERALTKEYMASK
    case kVK_CapsLock: return f & NSEventModifierFlagCapsLock;
    default: return false;
    }
}

void init() {
    input_map::load_startup();
    NSEventMask mask = NSEventMaskKeyDown | NSEventMaskKeyUp | NSEventMaskFlagsChanged;
    [NSEvent addLocalMonitorForEventsMatchingMask:mask handler:^NSEvent*(NSEvent* e) {
        if (e.modifierFlags & NSEventModifierFlagCommand) return e;  // keep Cmd-Q etc.
        if (NSApp.keyWindow.sheetParent || NSApp.modalWindow) return e;  // text prompt has focus
        if (gfx::controls_window_is_key()) return e;  // Controls window: keys go to it, not the game
        std::lock_guard<std::mutex> lk(g_mu);
        uint16_t code = e.keyCode & 0xFF;
        if (getenv("WWHD_NO_HOST_INPUT")) return e;
        if (e.type == NSEventTypeKeyDown && !e.isARepeat && mods::host_key_down(code)) return nil;  // e.g. Esc releases the mouse
        if (e.type == NSEventTypeKeyDown && !e.isARepeat && gfx::menu_hotkey(code)) return nil;
        if (e.type == NSEventTypeKeyDown) g_keys[code] = true;
        else if (e.type == NSEventTypeKeyUp) g_keys[code] = false;
        else g_keys[code] = modifier_down(code, e.modifierFlags);
        return nil;  // swallow, no system beep
    }];
    // drop held keys when the window loses focus
    [[NSNotificationCenter defaultCenter] addObserverForName:NSApplicationDidResignActiveNotification
                                                      object:nil queue:nil usingBlock:^(NSNotification*) {
        std::lock_guard<std::mutex> lk(g_mu);
        memset(g_keys, 0, sizeof(g_keys));
    }];
    [GCController startWirelessControllerDiscoveryWithCompletionHandler:nil];
    // GameController values are polled on the main thread
    [NSTimer scheduledTimerWithTimeInterval:1.0 / 240 repeats:YES block:^(NSTimer*) {
        PadState s = controller_state();
        std::lock_guard<std::mutex> lk(g_mu);
        g_pad = s;
    }];
}

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

void prompt_text(const std::u16string& initial, int max_len,
                 std::function<void(bool ok, std::u16string text)> done) {
    NSString* init = [NSString stringWithCharacters:(const unichar*)initial.data() length:initial.size()];
    dispatch_async(dispatch_get_main_queue(), ^{
        {
            std::lock_guard<std::mutex> lk(g_mu);
            memset(g_keys, 0, sizeof(g_keys));  // keys pressed now belong to the prompt
        }
        NSAlert* alert = [[NSAlert alloc] init];
        alert.messageText = @"Enter text";
        alert.informativeText = [NSString stringWithFormat:@"Up to %d characters.", max_len];
        [alert addButtonWithTitle:@"OK"];
        [alert addButtonWithTitle:@"Cancel"];
        NSTextField* field = [[NSTextField alloc] initWithFrame:NSMakeRect(0, 0, 260, 24)];
        field.stringValue = init;
        alert.accessoryView = field;
        alert.window.initialFirstResponder = field;
        auto finish = [done, max_len, field](NSModalResponse r) {
            NSString* t = field.stringValue;
            std::u16string out(t.length, u'\0');
            [t getCharacters:(unichar*)out.data() range:NSMakeRange(0, t.length)];
            if ((int)out.size() > max_len) out.resize(max_len);
            done(r == NSAlertFirstButtonReturn, out);
        };
        NSWindow* parent = NSApp.mainWindow ?: NSApp.windows.firstObject;
        if (parent) [alert beginSheetModalForWindow:parent completionHandler:^(NSModalResponse r) { finish(r); }];
        else finish([alert runModal]);
    });
}

}  // namespace input
