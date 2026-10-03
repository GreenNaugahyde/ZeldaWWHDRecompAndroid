// Android input: the Java side merges the on-screen controls, game controllers and a hardware
// keyboard into one Wii U GamePad state (MainActivity / InputMapper) and hands it over here.
#include <atomic>
#include <mutex>
#include <vector>

#include "../input.h"
#include "../mods/mods.h"
#include "../runtime.h"
#include "jni_bridge.h"

namespace gfx { extern uint64_t current_frame(); }

namespace input {

static std::mutex g_mu;
static PadState g_pad;
static bool g_touch;
static float g_tx, g_ty;
// presses (and touches) that started since the game last read the pad: a tap shorter than the
// game's 33 ms polling interval is still seen once
static uint32_t g_latched;
static bool g_touch_latched;

void set_touch(bool down, float x, float y) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (down && !g_touch) g_touch_latched = true;
    g_touch = down;
    g_tx = x;
    g_ty = y;
}

void set_pad(uint32_t buttons, float lx, float ly, float rx, float ry) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_latched |= buttons & ~g_pad.buttons;
    g_pad.buttons = buttons;
    g_pad.lx = lx;
    g_pad.ly = ly;
    g_pad.rx = rx;
    g_pad.ry = ry;
}

void init() {}

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
void set_pro_controller(bool on) { g_pro = on; LOG("[input] controls act as %s", on ? "Pro Controller" : "GamePad"); }

PadState read() {
    static const std::vector<Press> script = scripted();
    std::unique_lock<std::mutex> lk(g_mu);
    PadState s = g_pad;
    s.buttons |= g_latched;
    g_latched = 0;
    if (!script.empty()) {
        uint64_t frame = gfx::current_frame();
        for (auto& p : script)
            if (frame >= p.from && frame <= p.to) s.buttons |= p.bits;
    }
    s.touch = g_touch || g_touch_latched;
    g_touch_latched = false;
    s.tx = g_tx;
    s.ty = g_ty;
    lk.unlock();
    mods::filter_pad(s);  // gameplay mods: direct camera, first person on R3
    return s;
}

static std::mutex g_prompt_mu;
static std::function<void(bool, std::u16string)> g_prompt_done;

void prompt_text(const std::u16string& initial, int max_len, std::function<void(bool ok, std::u16string text)> done) {
    {
        std::lock_guard<std::mutex> lk(g_prompt_mu);
        g_prompt_done = std::move(done);
    }
    if (!jni::request_text_input(initial, max_len)) prompt_finished(false, u"");
}

void prompt_finished(bool ok, const std::u16string& text) {
    std::function<void(bool, std::u16string)> done;
    {
        std::lock_guard<std::mutex> lk(g_prompt_mu);
        done.swap(g_prompt_done);
    }
    if (done) done(ok, text);
}

}  // namespace input

// gameplay mods: the mouse camera needs a captured pointer, which the Android app doesn't offer
namespace mods {
bool mouse_captured() { return false; }
void mouse_release() {}
}  // namespace mods
