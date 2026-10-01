// Keyboard and GameController input, merged into one GamePad state.
//
// Keyboard: WASD move, arrows camera, K/Space = A, J = B, L = X, I = Y,
//           Q = L, E = R, Left Shift = ZL, C = ZR, Enter = +, Tab = -,
//           1-4 = D-pad up/down/left/right, X = L-stick click, V = R-stick click.
// Controllers use button positions (Xbox "A" is the bottom face = Wii U B).
#import <AppKit/AppKit.h>
#import <GameController/GameController.h>
#include <Carbon/Carbon.h>  // kVK_* key codes

#include <cmath>
#include <mutex>

#include "input.h"
#include "metal.h"
#include "../runtime.h"

#include <vector>

namespace gfx { void request_capture(); bool menu_hotkey(uint16_t keyCode); }

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

static PadState keyboard_state() {
    PadState s;
    auto k = [](int code) { return g_keys[code & 0xFF]; };
    struct { int code; uint32_t bit; } map[] = {
        {kVK_ANSI_K, kA}, {kVK_Space, kA}, {kVK_ANSI_J, kB}, {kVK_ANSI_L, kX}, {kVK_ANSI_I, kY},
        {kVK_ANSI_Q, kL}, {kVK_ANSI_E, kR}, {kVK_Shift, kZL}, {kVK_ANSI_C, kZR},
        {kVK_Return, kPlus}, {kVK_Tab, kMinus}, {kVK_ANSI_H, kHome},
        {kVK_ANSI_1, kUp}, {kVK_ANSI_2, kDown}, {kVK_ANSI_3, kLeft}, {kVK_ANSI_4, kRight},
        {kVK_ANSI_X, kStickL}, {kVK_ANSI_V, kStickR},
    };
    for (auto& m : map)
        if (k(m.code)) s.buttons |= m.bit;
    s.lx = (k(kVK_ANSI_D) ? 1.f : 0.f) - (k(kVK_ANSI_A) ? 1.f : 0.f);
    s.ly = (k(kVK_ANSI_W) ? 1.f : 0.f) - (k(kVK_ANSI_S) ? 1.f : 0.f);
    s.rx = (k(kVK_RightArrow) ? 1.f : 0.f) - (k(kVK_LeftArrow) ? 1.f : 0.f);
    s.ry = (k(kVK_UpArrow) ? 1.f : 0.f) - (k(kVK_DownArrow) ? 1.f : 0.f);
    if (s.lx && s.ly) { s.lx *= 0.7071f; s.ly *= 0.7071f; }
    return s;
}

static PadState controller_state() {
    PadState s;
    for (GCController* c in [GCController controllers]) {
        GCExtendedGamepad* g = c.extendedGamepad;
        if (!g) continue;
        auto b = [&](GCControllerButtonInput* in, uint32_t bit) { if (in && in.isPressed) s.buttons |= bit; };
        b(g.buttonA, kB); b(g.buttonB, kA); b(g.buttonX, kY); b(g.buttonY, kX);
        b(g.leftShoulder, kL); b(g.rightShoulder, kR);
        b(g.leftTrigger, kZL); b(g.rightTrigger, kZR);
        b(g.buttonMenu, kPlus); b(g.buttonOptions, kMinus); b(g.buttonHome, kHome);
        b(g.leftThumbstickButton, kStickL); b(g.rightThumbstickButton, kStickR);
        b(g.dpad.up, kUp); b(g.dpad.down, kDown); b(g.dpad.left, kLeft); b(g.dpad.right, kRight);
        auto pick = [](float& dst, float v) { if (std::fabs(v) > std::fabs(dst)) dst = v; };
        pick(s.lx, g.leftThumbstick.xAxis.value);
        pick(s.ly, g.leftThumbstick.yAxis.value);
        pick(s.rx, g.rightThumbstick.xAxis.value);
        pick(s.ry, g.rightThumbstick.yAxis.value);
    }
    return s;
}

void init() {
    NSEventMask mask = NSEventMaskKeyDown | NSEventMaskKeyUp | NSEventMaskFlagsChanged;
    [NSEvent addLocalMonitorForEventsMatchingMask:mask handler:^NSEvent*(NSEvent* e) {
        if (e.modifierFlags & NSEventModifierFlagCommand) return e;  // keep Cmd-Q etc.
        if (NSApp.keyWindow.sheetParent || NSApp.modalWindow) return e;  // text prompt has focus
        std::lock_guard<std::mutex> lk(g_mu);
        uint16_t code = e.keyCode & 0xFF;
        if (e.type == NSEventTypeKeyDown && !e.isARepeat && gfx::menu_hotkey(code)) return nil;
        if (e.type == NSEventTypeKeyDown) g_keys[code] = true;
        else if (e.type == NSEventTypeKeyUp) g_keys[code] = false;
        else if (code == kVK_Shift) g_keys[code] = (e.modifierFlags & NSEventModifierFlagShift) != 0;
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

static std::atomic<bool> g_pro{getenv("WWHD_PRO_CONTROLLER") != nullptr};
bool pro_controller() { return g_pro.load(std::memory_order_relaxed); }
void set_pro_controller(bool on) { g_pro = on; LOG("[input] keyboard/controller act as %s", on ? "Pro Controller" : "GamePad"); }

PadState read() {
    static const std::vector<Press> script = scripted();
    std::lock_guard<std::mutex> lk(g_mu);
    PadState k = keyboard_state(), s = g_pad;
    for (auto& p : script)
        if (gfx::R.frame >= p.from && gfx::R.frame <= p.to) s.buttons |= p.bits;
    s.buttons |= k.buttons;
    if (k.lx || k.ly) { s.lx = k.lx; s.ly = k.ly; }
    if (k.rx || k.ry) { s.rx = k.rx; s.ry = k.ry; }
    s.touch = g_touch;
    s.tx = g_tx;
    s.ty = g_ty;
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
