// padscore: Wii Remote / Pro Controller (WPAD, KPAD). Only a Pro Controller on channel 0 exists,
// and only while the keyboard/host controllers are set to act as one (Input menu). Struct layouts
// and constants follow Cemu's padscore.
#include "../runtime.h"
#include "../input.h"
#include "../release.h"

#include <mutex>
#include <set>
#include <string>

namespace interp { bool repeat_input(); bool fresh_sticks(); }

namespace {
constexpr int32_t kWpadErrNone = 0, kWpadErrNoController = -1;
constexpr int32_t kKpadErrNone = 0, kKpadErrNoController = -2;
constexpr uint8_t kDevURCC = 31, kDevNone = 253, kFormatURCC = 22;

bool connected(uint32_t chan) { return chan == 0 && input::pro_controller(); }

// debug: WWHD_WPAD_TRACE=1 logs each new chain of game callers (USA addresses) asking for a Wii
// Remote / Pro Controller
void trace_caller(Cpu* c, const char* what) {
    static const bool on = getenv("WWHD_WPAD_TRACE") != nullptr;
    if (!on) return;
    char b[16];
    snprintf(b, sizeof b, "%08X", release::usa_code(c->lr));
    std::string chain = b;
    uint32_t sp = c->r[1];
    for (int i = 0; i < 8 && sp >= 0x10000000 && sp < 0x50000000; i++) {
        uint32_t next = ld32(sp);
        if (next <= sp || next >= 0x50000000) break;
        snprintf(b, sizeof b, " %08X", release::usa_code(ld32(next + 4)));
        chain += b;
        sp = next;
    }
    static std::mutex mu;
    static std::set<std::string> seen;
    std::lock_guard<std::mutex> lk(mu);
    if (seen.insert(std::string(what) + chain).second) LOG("[pad] %s from %s", what, chain.c_str());
}

// VPAD button bits (input::PadState) -> Pro Controller (URCC) bits
uint32_t pro_buttons(uint32_t v) {
    static const uint32_t map[][2] = {
        {input::kUp, 0x1},     {input::kLeft, 0x2},    {input::kZR, 0x4},     {input::kX, 0x8},
        {input::kA, 0x10},     {input::kY, 0x20},      {input::kB, 0x40},     {input::kZL, 0x80},
        {input::kR, 0x200},    {input::kPlus, 0x400},  {input::kHome, 0x800}, {input::kMinus, 0x1000},
        {input::kL, 0x2000},   {input::kDown, 0x4000}, {input::kRight, 0x8000},
        {input::kStickR, 0x10000}, {input::kStickL, 0x20000},
    };
    uint32_t out = 0;
    for (auto& m : map)
        if (v & m[0]) out |= m[1];
    return out;
}
}  // namespace

HLE(padscore, KPADInitEx) {}
HLE(padscore, KPADGetMplsWorkSize) { ret(c, 0x5FE0); }
HLE(padscore, KPADSetMplsWorkarea) {}
HLE(padscore, WPADEnableURCC) {}
HLE(padscore, WPADEnableWiiRemote) {}
HLE(padscore, WPADDisconnect) {}
// (chan, command): 1 = motor on, 0 = off (Pro Controller on channel 0 only)
HLE(padscore, WPADControlMotor) {
    if (arg(c, 0) == 0 && input::pro_controller()) input::rumble_hold(arg(c, 1) != 0);
}
HLE(padscore, WPADGetBatteryLevel) { ret(c, 4); }  // full
HLE(padscore, WPADCanSendStreamData) { ret(c, 0); }
HLE(padscore, WPADSendStreamData) { ret(c, (uint32_t)kWpadErrNoController); }
HLE(padscore, WPADControlSpeaker) { ret(c, (uint32_t)kWpadErrNoController); }

HLE(padscore, WPADProbe) {
    // (chan, uint32* type) -> WPAD error
    uint32_t chan = arg(c, 0), type = arg(c, 1);
    bool on = connected(chan);
    trace_caller(c, "WPADProbe");
    TRACE("[pad] WPADProbe(%u) -> %s", chan, on ? "pro" : "none");
    if (type) st32(type, on ? kDevURCC : kDevNone);
    ret(c, (uint32_t)(on ? kWpadErrNone : kWpadErrNoController));
}

HLE(padscore, KPADReadEx) {
    // (chan, KPADStatus* buf, count, int32* err) -> samples written
    uint32_t chan = arg(c, 0), st = arg(c, 1), count = arg(c, 2), err = arg(c, 3);
    trace_caller(c, "KPADReadEx");
    TRACE("[pad] KPADReadEx(%u, %08X, %u) connected=%d", chan, st, count, connected(chan));
    if (!connected(chan) || !st || (int32_t)count <= 0) {
        if (err) st32(err, (uint32_t)(connected(chan) ? kKpadErrNone : kKpadErrNoController));
        ret(c, 0);
        return;
    }
    static uint32_t last = 0;
    static input::PadState last_p;
    const bool repeat = interp::repeat_input();
    input::PadState p = repeat ? last_p : input::read();  // see interp.cpp
    if (repeat && interp::fresh_sticks()) {  // true 60: sticks every pass, buttons on full passes
        input::PadState f = input::read();
        p.lx = f.lx; p.ly = f.ly; p.rx = f.rx; p.ry = f.ry;
    }
    last_p = p;
    uint32_t hold = pro_buttons(p.buttons);
    memset(mem::ptr(st), 0, 0xF0);
    st8(st + 0x5C, kDevURCC);     // devType
    st8(st + 0x5D, 0);            // wpadErr
    st8(st + 0x5F, kFormatURCC);  // data_format
    st32(st + 0x60, hold);                // ex_status.uc.hold
    st32(st + 0x64, hold & ~last);        // trig
    st32(st + 0x68, last & ~hold);        // release
    last = hold;
    stf32(st + 0x6C, p.lx); stf32(st + 0x70, p.ly);  // lstick
    stf32(st + 0x74, p.rx); stf32(st + 0x78, p.ry);  // rstick
    st32(st + 0x7C, 1);                   // charge
    st32(st + 0x80, 1);                   // cable
    if (err) st32(err, kKpadErrNone);
    ret(c, 1);
}
