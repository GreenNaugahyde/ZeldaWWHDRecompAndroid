// (fork: Android) Quest texts in the game's own message window (notes/online-modes.md, "Native message box").
//
// The way upstream's story mods show their words (mods/story_mail.cpp, taken over here, not its letters): a message
// the game has (kCarrier, the plain talk message upstream's mod residents use) is shown, and while it is, its text is
// ours. When the message manager looks the number up (getMessage 025F50E8) the MSBT entry it found is noted; the text
// lookup of exactly that entry (0273A2E8) then hands back our UTF-16 text, after the game's own leading codes of the
// original (window and sound). The message manager's check that a label has text (its call at 0273A340) keeps the game's.
//
// The window is opened by the game itself: a native message tag (d_a_tag_msg, stage name "TagMsg") made at Link with
// its message number (home angle x) set to kCarrier, no switch, no event flag, type 1 (it orders its talk event by
// itself once Link is in its range, 300 units): the game's DEFAULT_MSG event, its own window, A to go on. Once the
// window has closed the tag is taken away and the next line (if any) comes.
#include <cstring>
#include <deque>
#include <mutex>
#include <string>

#include "../mods/coop_names.h"
#include "guest_addr.h"
#include "mods/mods.h"
#include "runtime.h"

extern "C" void f_025F50E8_orig(Cpu* c);
extern "C" void f_0273A2E8_orig(Cpu* c);

namespace android_say {
namespace {
namespace names = mods::coop::names;
constexpr uint32_t kCarrier = 0x2CF3;  // (upstream's: a plain talk message, known in every stage)
constexpr uint32_t kMsgSet = 0x8, kMsgLabelIndex = 0x118, kMsgNumber = 0x11C;  // getMessage's out (story_mail.cpp)
constexpr uint32_t kPrefixRoom = 64, kBufBytes = 4096, kMaxUnits = (kBufBytes - kPrefixRoom) / 2 - 1;
const uint32_t kPlay = GD(0x1046F0B0), kTextLengthReturn = GC(0x0273A340);

struct Override { uint32_t buffer = 0, msbt = 0, index = 0; bool armed = false, keyed = false; std::u16string text; } g;
std::deque<std::string> g_lines;  // waiting (UTF-8)
uint32_t g_tag = 0xFFFFFFFFu;     // the message tag's process id
int g_phase = 0, g_steps = 0;     // 0 idle, 1 tag made: waiting for the window, 2 shown: waiting for it to close

// the game's message codes at the start of a text (0x0E tag: 0E group type size + params; 0x0F end tag), at most kPrefixRoom
uint32_t leading_codes(uint32_t text) {
    uint32_t n = 0;
    for (;;) {
        const uint16_t unit = ld16(text + n);
        const uint32_t len = unit == 0x0E ? 8u + ld16(text + n + 6) : unit == 0x0F ? 6u : 0u;
        if (!len || n + len > kPrefixRoom) return n;
        n += len;
    }
}
std::u16string utf16(const std::string& s) {
    std::u16string out;
    for (size_t i = 0; i < s.size() && out.size() < kMaxUnits;) {
        const uint8_t b = (uint8_t)s[i];
        uint32_t ch = b, extra = 0;
        if (b >= 0xF0) ch = b & 7, extra = 3; else if (b >= 0xE0) ch = b & 15, extra = 2; else if (b >= 0xC0) ch = b & 31, extra = 1;
        i++;
        for (uint32_t k = 0; k < extra && i < s.size(); k++, i++) ch = ch << 6 | ((uint8_t)s[i] & 0x3F);
        if (ch >= 0x10000) { ch -= 0x10000; out += (char16_t)(0xD800 | ch >> 10), out += (char16_t)(0xDC00 | (ch & 0x3FF)); }
        else out += (char16_t)ch;
    }
    return out;
}
bool mesg_shown() { return ld8(kPlay + names::inf::kMesgStatus) != 0; }  // (dComIfGp_getMesgStatus)
}  // namespace

// a line for the game's message window (any thread: the game thread's step shows it when it can; '\n' breaks lines)
std::mutex g_mu;
void say(const std::string& text) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_lines.size() < 16) g_lines.push_back(text);
}

// game thread, each full step (online_compat::tick)
void step(Cpu* c) {
    const uint32_t link = ld32(kPlay + names::inf::kLink);
    if (!g.buffer) g.buffer = mem::host_alloc(kBufBytes, 0x20);
    if (link < 0x10000000u || link >= 0x50000000u) { g_phase = 0, g.armed = g.keyed = false; return; }
    if (g_phase == 1) {  // the tag waits for Link (it is at him: at once) and the event; the window opens
        if (mesg_shown()) g_phase = 2, g_steps = 0;
        else if (++g_steps > 300) {  // (no window in 10 s: a cutscene, a shop... the line is dropped)
            LOG("[say] the window did not open: line dropped");
            guest_call(c, GC(0x025D57E4), {g_tag});
            g_phase = 0, g.armed = g.keyed = false;
        }
        return;
    }
    if (g_phase == 2) {  // shown: closed for 5 steps -> the tag goes, the next line
        g_steps = mesg_shown() ? 0 : g_steps + 1;
        if (g_steps < 5) return;
        guest_call(c, GC(0x025D57E4), {g_tag});  // fopAcM_delete
        g_phase = 0, g.armed = g.keyed = false;
        LOG("[say] window closed");
    }
    if (mesg_shown()) return;  // (another window: later)
    std::string line;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (g_lines.empty()) return;
        line = g_lines.front();
        g_lines.pop_front();
    }
    // the message tag at Link: its message ours while shown
    static const uint32_t name = mem::host_alloc(16), buf = mem::host_alloc(0x30);
    memset(mem::ptr(name), 0, 16), memcpy(mem::ptr(name), "TagMsg", 6);
    Cpu probe = *c;
    const uint32_t entry = guest_call(&probe, GC(0x025C109C), {name});  // dStage_searchName
    if (!entry) return LOG("[say] no TagMsg here");
    for (int i = 0; i < 12; i += 4) st32(buf + i, ld32(link + 0x314 + i));
    st16(buf + 0x10, (uint16_t)kCarrier), st16(buf + 0x12, 0), st16(buf + 0x14, 0xFFFF);  // (message, -, no event flag)
    const uint32_t three = 0x40400000u;  // 3.0: a range of 300 units
    st32(buf + 0x20, three), st32(buf + 0x24, three), st32(buf + 0x28, three);
    g.text = utf16(line), g.armed = true, g.keyed = false;
    const uint32_t room = (uint32_t)(int32_t)(int8_t)ld8(link + 0x326);
    const uint32_t layer = guest_call(c, GC(0x025DED64));
    guest_call(c, GC(0x025DEAB4), {ld32(link + 0x2C)});
    g_tag = guest_call(c, GC(0x025D5834), {ld16(entry + 8), 0xFFFFFF40u, buf, room, buf + 0x10, buf + 0x20,
                                           (uint32_t)(int32_t)(int8_t)ld8(entry + 0xA), 0});  // fopAcM_create; type 1
    guest_call(c, GC(0x025DEAB4), {layer});
    g_phase = 1, g_steps = 0;
    LOG("[say] window asked for (%zu units): %s", g.text.size(), line.substr(0, 60).c_str());
}
}  // namespace android_say

// getMessage(manager, out, set name, label): the MSBT entry of our message, noted; our text written after its codes
extern "C" void hook_025F50E8(Cpu* c) {
    using namespace android_say;
    const uint32_t out = c->r[4];
    f_025F50E8_orig(c);
    if (!g.armed || g.keyed || !g.buffer || ld32(out + kMsgNumber) != kCarrier) return;
    const uint32_t set = ld32(out + kMsgSet);
    if (!set) return;
    g.msbt = ld32(set), g.index = ld32(out + kMsgLabelIndex), g.keyed = true;
    Cpu probe = *c;  // the original text, for its leading codes
    probe.r[3] = g.msbt, probe.r[4] = g.index;
    f_0273A2E8_orig(&probe);
    const uint32_t prefix = probe.r[3] ? leading_codes(probe.r[3]) : 0;
    for (uint32_t i = 0; i < prefix; i++) st8(g.buffer + i, ld8(probe.r[3] + i));
    for (size_t i = 0; i < g.text.size(); i++) st16(g.buffer + prefix + uint32_t(2 * i), g.text[i]);
    st16(g.buffer + prefix + uint32_t(2 * g.text.size()), 0);
    LOG("[say] message %X: our text (MSBT %08X entry %u, %u bytes of codes kept)", kCarrier, g.msbt, g.index, prefix);
}
// text(msbt, index): ours for the noted entry while it is shown (the label check keeps the game's)
extern "C" void hook_0273A2E8(Cpu* c) {
    using namespace android_say;
    if (c->lr != kTextLengthReturn && g.keyed && c->r[3] == g.msbt && c->r[4] == g.index) {
        c->r[3] = g.buffer;
        return;
    }
    f_0273A2E8_orig(c);
}
