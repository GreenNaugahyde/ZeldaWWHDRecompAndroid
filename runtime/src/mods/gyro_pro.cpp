// Gyro aiming with the Wii U Pro Controller selected. The real Pro Controller has no gyro, so the
// game's motion code asks "is the Pro Controller the active controller?" (02617AE4: the pad
// state's controller mode +0x1D0 == 0; the GamePad is 1) and stays off: the per-frame motion
// update (02618D18) skips the GamePad's samples, the attitude isn't copied or kept, and the gyro
// aiming (the code block 0270D000-02718000, which uses the attitude at pad state +0x1D8) is off. This app feeds the GamePad's motion from the device's or a controller's sensors in
// both modes (hle/system_stubs.cpp, motion.cpp), so for calls from that block the answer is "no"
// while motion data comes in; everything else (menus, inventory on the TV picture) still sees the
// Pro Controller. WWHD_NO_PRO_GYRO=1 turns it off.
#include <map>
#include <mutex>
#include <string>

#include "../input.h"
#include "../motion.h"
#include "../release.h"
#include "../runtime.h"

extern "C" {
void f_02617AE4_orig(Cpu* c);  // "is the Pro Controller active" (pad state +0x1D0 == 0)
}

extern "C" void hook_02617AE4(Cpu* c) {
    static const bool off = getenv("WWHD_NO_PRO_GYRO") != nullptr;
    static const uint32_t lo = release::code(0x0270D000), hi = release::code(0x02718000);
    // the motion update itself (return addresses): 02618D18 reads the GamePad's samples only
    // without the Pro Controller, 02618604 copies the attitude, and the controller state machine
    // (02617AF4) keeps the attitude 026173B0 just computed
    static const uint32_t sites[] = {release::code(0x02618D3C), release::code(0x0261864C), release::code(0x02617E98)};
    motion::Vpad m;
    const bool motionIn = motion::vpad(m);
    bool gyroCode = lo && hi && c->lr >= lo && c->lr < hi;
    for (uint32_t s : sites) gyroCode |= s && c->lr == s;
    const bool fake = !off && gyroCode && input::pro_controller() && motionIn;
    // debug: WWHD_LOG_PRO_GYRO=1 logs who asks (return addresses as USA addresses) every 300 calls
    static const bool log = getenv("WWHD_LOG_PRO_GYRO") != nullptr;
    if (log) {
        static std::mutex mu;
        static std::map<uint32_t, uint32_t> callers;
        static uint32_t n = 0, faked = 0;
        std::lock_guard<std::mutex> lk(mu);
        callers[release::usa_code(c->lr)]++;
        faked += fake;
        if (++n == 300) {
            std::string s;
            for (auto& [a, k] : callers) {
                char b[24];
                snprintf(b, sizeof b, " %08X:%u", a, k);
                s += b;
            }
            LOG("[progyro] 300 calls, %u answered GamePad; pro mode %d, motion %d; from%s", faked, (int)input::pro_controller(),
                (int)motionIn, s.c_str());
            callers.clear();
            n = faked = 0;
        }
    }
    if (fake) {
        c->r[3] = 0;  // the gyro code: as with the GamePad
        return;
    }
    f_02617AE4_orig(c);
}
