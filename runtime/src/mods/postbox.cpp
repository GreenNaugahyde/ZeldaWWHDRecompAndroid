// Pictos mailed to Carlov at the Island Postal Service's postboxes (Game menu): show a postbox the Picto Box, choose
// a picto in the game's own chooser, and the postbox takes the order Carlov would take at the Nintendo Gallery. The
// figurine is made at the next change of day and put in the gallery by itself (no visit to Carlov); with Same day
// delivery at once.
//
// The game's picto handover (decomp d_event.cpp photoCheck): showing the Picto Box (an X / Y / Z item) to an actor
// whose attention flags have 0x01000000 (Carlov, Lenzo, ...) orders a photo event: the game's album chooser, then a
// talk with the actor, chkPhoto set (WWHD play + 0x52B2) and the picto's dSnap result in play + 0x5BE6 subject,
// + 0x5BE7 flaws (1 face, 2 body), + 0x5BEA colour. The postbox (daObjTpost, WWHD actor attention flags + 0x39C)
// gets the flag in its execute (023A4BB0); its getMsg (023A5570) answers a handed picto with Carlov's checks
// (his WWHD getMsg 022A2208): colour, a gallery subject (subject remap 022A1F1C, gallery number 025BD600), not made
// (0229FE28), not on order (022A1F64), face and body seen, fewer than 12 orders (022A037C); then his order: an old
// finished one cleared (event bits 3080 4080 3F01 4040 off, the order list emptied 0229FF54), the figurine added to
// the list (022A2008, WWHD keeps up to 12 orders), event bits 2F01 (making) and 3401 on. Accepted: "Kah-chunk!
// Puhhh-whirrrr!" (0xCE8) then "Your parcel has hereby been accepted for delivery..." (0xCF2) (its message chain
// next_msgStatus 023A55BC: no postage); refused: "Krzzzzt! PAHHHFT! This item cannot be delivered." (0xCE9) with
// the postbox spitting it out (023A3DD0, as for a letter it can't take). The app shows what was mailed or why not.
// Delivery: the change of day sets 3080 while 2F01 is on (d_kankyo_dayproc); then Carlov's own completion
// (022A2078: each listed figurine made, with the ones some subjects bring along) and his last line's clean-up
// (3080 4080 3F01 4040 2F01 off, the list emptied), so he has nothing left to tell. Event bits: save object + 0x644
// (is 025B8B94, on 025B8B68, off 025B8B7C).
// Test switches: WWHD_POSTBOX_TEST=1 skips "already made / on order" (a finished save), WWHD_POSTBOX_GOTO=1 puts
// Link in front of each postbox once, WWHD_POSTBOX_SUBJECT=hex stands in for a picto without a subject.
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <vector>

#include "mods.h"
#include "../guest_addr.h"
#include "../hud.h"
#include "../input.h"
#include "../release.h"
#include "../runtime.h"
#include "../savestate.h"

namespace mods {
namespace {
const release::Data kSaveInfo{0x101F84DC};
constexpr uint32_t kEvents = 0x644, kAttnFlags = 0x39C, kAttnPhoto = 0x01000000;
constexpr uint32_t kPhoto = 0x52B2, kPhotoSubject = 0x5BE6, kPhotoFlaws = 0x5BE7, kPhotoColour = 0x5BEA;
constexpr uint32_t kPos = 0x314, kShapeAngleY = 0x32A, kAngleY = 0x322;
constexpr uint16_t kEvMet = 0x2F02, kEvMaking = 0x2F01, kEvOrdered = 0x3401, kEvDone = 0x3080, kEvGiven = 0x4080,
                   kEvRoom = 0x3F01, kEvRoomIn = 0x4040;
constexpr uint32_t kMsgAccepted = 0xCE8, kMsgRefused = 0xCE9, kMsgThanks = 0xCF2;
constexpr uint32_t kStatusContinues = 0x0F, kStatusEnds = 0x10;

std::atomic<bool> g_on{true}, g_same_day{false};
std::atomic<bool> g_offer_pending{false}, g_offer_ready{false};
std::atomic<int> g_present{0};   // the app's Yes: 1 equip / wait, 2 pressing (input thread)
// the postbox takes pictos only from the app's Yes in its chat on: the Picto Box used near it otherwise is the
// Picto Box (aiming), not the chooser. Armed by the Yes, disarmed once the handover is over (hook_023A4BB0)
std::atomic<bool> g_armed{false};
// The Picto Box's own button pressed facing a postbox otherwise: the game would show it to the postbox (its talk
// target takes shown items, letters; daPy_lk_c::orderTalk: a SPEAK target, or TALK when locked on) and the postbox
// spits it out as a letter it can't take, which crashed. The press is held back (1) until the postbox's execute has
// taken its talk attention away (kAttnTalk; g_block_applied), then let through (2), the attention given back after (3)
constexpr uint32_t kAttnTalk = 0x0000000A;  // fopAc_Attn_LOCKON_TALK | fopAc_Attn_ACTION_SPEAK
std::atomic<int> g_block{0};
std::atomic<bool> g_block_applied{false};
constexpr uint32_t kMsgWhat = 0xCE7;  // "Welcome to the Island Postal Service! What do you have for delivery?"
// postboxes seen in their execute lately (where they are) and Link facing one
std::mutex g_box_mu;
struct Box { uint32_t actor; float x, y, z; uint64_t frame; uint32_t talk_bits; };
std::vector<Box> g_boxes;
uint64_t g_frames = 0;
std::atomic<bool> g_near{false};
std::mutex g_mu;
std::deque<int> g_notices;  // kind << 8 | gallery number, for the app
uint32_t g_talking = 0;     // the postbox answering a picto

void notice(int kind, int fig) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_notices.size() < 16) g_notices.push_back(kind << 8 | (fig & 0xFF));
}

uint32_t events() { return ld32(kSaveInfo) + kEvents; }
bool ev_is(Cpu* c, uint16_t f) { return guest_call(c, GC(0x025B8B94), {events(), f}) != 0; }
void ev_on(Cpu* c, uint16_t f) { guest_call(c, GC(0x025B8B68), {events(), f}); }
void ev_off(Cpu* c, uint16_t f) { guest_call(c, GC(0x025B8B7C), {events(), f}); }
// Carlov's helpers take his actor but don't use it (they work on the save): 0
uint32_t carlov(Cpu* c, uint32_t fn, uint32_t arg = 0) { return guest_call(c, GC(fn), {0, arg}); }

int order_count(Cpu* c) { return (int)(carlov(c, 0x022A037C) & 0xFF); }

// the finished orders into the gallery, as Carlov hands them over, then his clean-up
void deliver(Cpu* c) {
    int n = order_count(c);
    for (int i = 0; i < n; i++) notice(2, (int)(carlov(c, 0x022A0898, (uint32_t)i) & 0xFF));
    carlov(c, 0x022A2078);
    for (uint16_t f : {kEvDone, kEvGiven, kEvRoom, kEvRoomIn, kEvMaking}) ev_off(c, f);
    carlov(c, 0x0229FF54);
    LOG("[postbox] %d figurine(s) delivered to the gallery", n);
}

// a handed picto: Carlov's checks and order; true if taken
bool take_picto(Cpu* c) {
    static const bool test = getenv("WWHD_POSTBOX_TEST") && atoi(getenv("WWHD_POSTBOX_TEST"));
    uint32_t play = guest_call(c, GC(0x025200D4));
    uint8_t subject = ld8(play + kPhotoSubject), flaws = ld8(play + kPhotoFlaws), colour = ld8(play + kPhotoColour);
    static const char* fake = getenv("WWHD_POSTBOX_SUBJECT");  // test aid: this subject (hex) for a picto without one
    if (fake && !subject) subject = (uint8_t)strtoul(fake, nullptr, 16);
    uint8_t remapped = (uint8_t)guest_call(c, GC(0x022A1F1C), {subject});
    int fig = (uint8_t)(remapped - 0x49) < 0x86 ? (int)(guest_call(c, GC(0x025BD600), {remapped}) & 0xFF) : -1;
    LOG("[postbox] picto: subject %02X (figurine %d), flaws %u, colour %u", subject, fig, flaws, colour);
    int why = !ev_is(c, kEvMet) ? 10 : !colour ? 11 : fig < 0 ? 12
            : !test && carlov(c, 0x0229FE28, (uint32_t)fig) ? 13 : !test && carlov(c, 0x022A1F64, (uint32_t)fig) ? 14
            : (flaws & 1) ? 15 : (flaws & 2) ? 16 : order_count(c) >= 12 ? 17 : 0;
    if (why) {
        notice(why, fig < 0 ? 0 : fig);
        return false;
    }
    if (ev_is(c, kEvGiven)) {  // the last figurines handed over but Carlov not talked to since
        for (uint16_t f : {kEvDone, kEvGiven, kEvRoom, kEvRoomIn}) ev_off(c, f);
        carlov(c, 0x0229FF54);
    }
    carlov(c, 0x022A2008, (uint32_t)fig);
    ev_on(c, kEvMaking);
    ev_on(c, kEvOrdered);
    LOG("[postbox] figurine %d ordered (%d on order)", fig, order_count(c));
    if (g_same_day) deliver(c);
    else notice(1, fig);
    return true;
}
}  // namespace

bool postbox_mail() { return g_on; }
int postbox_offer() { return g_offer_ready.exchange(false) ? 1 : 0; }
void postbox_present() { g_armed = true; g_present = 1; LOG("[postbox] the app's Yes: the Picto Box is shown"); }
bool postbox_near() { return g_on && g_near; }

// filter_pad: the app's Yes presses the Picto Box's button (lent to Y for the handover if it is on none), Link still facing
// the postbox: the game's own picto chooser opens
void postbox_pad(input::PadState& s) {
    using clk = std::chrono::steady_clock;
    static clk::time_point since;
    static int step = 0;
    const uint32_t bits[3] = {input::kX, input::kY, input::kR};
    int p = g_present.load();
    {  // the Picto Box used facing a postbox, not from its chat: aimed, not shown (g_block above)
        static uint32_t prev = 0;
        static clk::time_point block_since;
        int pb = p ? -1 : picto_box_button();
        uint32_t bit = pb >= 0 ? bits[pb] : 0;
        bool down = bit && (s.buttons & bit) && !(prev & bit);
        prev = s.buttons;
        int blk = g_block.load();
        if (!blk && down && g_on && g_near && !g_armed && event_mode_now() == 0 && !hud_picto_screen()) {
            g_block_applied = false;
            g_block = blk = 1;
            block_since = clk::now();
            LOG("[postbox] the Picto Box on %c facing a postbox: used, not shown", "XYR"[pb]);
        }
        if (blk) {
            float ms = std::chrono::duration<float, std::milli>(clk::now() - block_since).count();
            if (blk == 1) {
                if (bit) s.buttons &= ~bit;
                if (g_block_applied || ms > 200) g_block = 2, block_since = clk::now();
            } else if (blk == 2) {
                if (bit) s.buttons |= bit;  // (the finger may be up already: the press is the player's)
                if (ms > 100) g_block = 3, block_since = clk::now();
            } else if (ms > 400) {
                g_block = 0;
            }
        }
    }
    if (!p) {
        step = 0;
        return;
    }
    int b = picto_box_button();
    if (p != step) {
        step = p, since = clk::now();
        LOG("[postbox] showing the Picto Box: step %d, its button %d", p, b);
    }
    float ms = std::chrono::duration<float, std::milli>(clk::now() - since).count();
    if (b == -2 || ms > 3000) {  // (no Picto Box, or it never got a button)
        g_present = 0;
        return;
    }
    if (b < 0) {
        if (p == 1) lend_picto_box(1), g_present = 3;  // on Y for the handover, Y's own item back after it
        return;
    }
    if (p != 2) {
        g_present = 2;
        return;
    }
    // (after the question the game needs a moment before Link can show something again)
    static bool pressed = false;
    if (ms < 1200) pressed = false;
    if (ms < 1200) s.buttons &= ~bits[b];
    else if (ms < 1450) {
        if (!pressed) LOG("[postbox] pressing %c (event mode %d)", "XYR"[b], event_mode_now()), pressed = true;
        s.buttons |= bits[b];
    }
    else g_present = 0;
}
void set_postbox_mail(bool on) { g_on = on; LOG("[postbox] mailing pictos %s", on ? "on" : "off"); }
bool postbox_same_day() { return g_same_day; }
void set_postbox_same_day(bool on) { g_same_day = on; LOG("[postbox] same day delivery %s", on ? "on" : "off"); }
int postbox_notice() {
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_notices.empty()) return -1;
    int n = g_notices.front();
    g_notices.pop_front();
    return n;
}

// game main thread, frame start (interp.cpp): figurines finished by the change of day go to the gallery
void postbox_service(Cpu* c) {
    if (!g_on || !ld32(kSaveInfo) || ss::front_screen() != 0) return;
    g_frames++;
    // the question about a picto once the postbox's talk is over
    if (g_offer_pending && event_mode_now() == 0) {
        g_offer_pending = false;
        if (picto_box_button() != -2 && picto_count() > 0) g_offer_ready = true;
    }
    // facing a postbox: near one (seen in its execute lately) and it in front of him
    uint32_t link = link_actor();
    bool near = false;
    if (link) {
        float lx = u32_as_f32(ld32(link + kPos)), ly = u32_as_f32(ld32(link + kPos + 4)), lz = u32_as_f32(ld32(link + kPos + 8));
        float a = (int16_t)ld16(link + kShapeAngleY) * (3.14159265f / 32768.0f);
        std::lock_guard<std::mutex> lk(g_box_mu);
        for (const Box& b : g_boxes) {
            if (g_frames - b.frame > 10) continue;
            float dx = b.x - lx, dz = b.z - lz, d = std::sqrt(dx * dx + dz * dz);
            if (d < 170 && std::fabs(b.y - ly) < 100 && (d < 1 || (dx * std::sin(a) + dz * std::cos(a)) / d > 0.6f)) near = true;
        }
    }
    g_near = near;
    static unsigned frames = 0;
    if (++frames % 30) return;  // (twice a second is plenty)
    if (ev_is(c, kEvDone) && !ev_is(c, kEvGiven) && order_count(c) > 0) deliver(c);
}

}  // namespace mods

// daObjTpost execute: the postbox takes pictos (the photo event's condition)
extern "C" void f_023A4BB0_orig(Cpu* c);
extern "C" void hook_023A4BB0(Cpu* c) {
    uint32_t box = c->r[3];
    if (mods::postbox_mail()) {
        // the photo event's flag only while the app's Yes shows the Picto Box, its chooser is open or its talk runs;
        // a second without any of them ends it
        static int idle = 0;
        if (mods::g_armed) {
            bool busy = mods::g_present != 0 || hud_album_screen() || mods::event_mode_now() != 0;
            idle = busy ? 0 : idle + 1;
            if (idle > 30) mods::g_armed = false, idle = 0, LOG("[postbox] handover over: the postbox no longer takes pictos");
        }
        uint32_t flags = ld32(box + mods::kAttnFlags);
        flags = mods::g_armed ? flags | mods::kAttnPhoto : flags & ~mods::kAttnPhoto;
        {  // the Picto Box used facing it: no talk target for that press (g_block), given back after
            std::lock_guard<std::mutex> lk(mods::g_box_mu);
            for (auto& b : mods::g_boxes)
                if (b.actor == box) {
                    if (mods::g_block != 0 && !mods::g_armed) {
                        b.talk_bits |= flags & mods::kAttnTalk;
                        flags &= ~mods::kAttnTalk;
                        mods::g_block_applied = true;
                    } else if (b.talk_bits) {
                        flags |= b.talk_bits;
                        LOG("[postbox] talk attention %08X given back", b.talk_bits);
                        b.talk_bits = 0;
                    }
                }
        }
        st32(box + mods::kAttnFlags, flags);
        {
            std::lock_guard<std::mutex> lk(mods::g_box_mu);
            bool found = false;
            for (auto& b : mods::g_boxes)
                if (b.actor == box) found = true, b.frame = mods::g_frames;
            if (!found) {
                if (mods::g_boxes.size() > 8) mods::g_boxes.erase(mods::g_boxes.begin());
                mods::g_boxes.push_back({box, 0, 0, 0, mods::g_frames, 0});
            }
            for (auto& b : mods::g_boxes)
                if (b.actor == box) {
                    b.x = u32_as_f32(ld32(box + mods::kPos));
                    b.y = u32_as_f32(ld32(box + mods::kPos + 4));
                    b.z = u32_as_f32(ld32(box + mods::kPos + 8));
                }
        }
        // test aid: Link in front of the postbox, facing it (once per postbox)
        static const bool go = getenv("WWHD_POSTBOX_GOTO") && atoi(getenv("WWHD_POSTBOX_GOTO"));
        static uint32_t placed = 0;
        static int again = 0;  // placed over a few frames (a warp's arrival may put him back)
        uint32_t link = mods::link_actor();
        if (go && link && placed == box && again > 0) {
            if (--again == 0)
                LOG("[postbox] Link now at %.0f %.0f %.0f, the postbox at %.0f %.0f %.0f", u32_as_f32(ld32(link + mods::kPos)),
                    u32_as_f32(ld32(link + mods::kPos + 4)), u32_as_f32(ld32(link + mods::kPos + 8)), u32_as_f32(ld32(box + mods::kPos)),
                    u32_as_f32(ld32(box + mods::kPos + 4)), u32_as_f32(ld32(box + mods::kPos + 8)));
        }
        if (go && link && (placed != box || again > 20) && mods::event_mode_now() == 0 && ss::front_screen() == 0) {
            if (placed != box) again = 40;
            placed = box;
            float a = (int16_t)ld16(box + mods::kShapeAngleY) * (3.14159265f / 32768.0f);
            float x = u32_as_f32(ld32(box + mods::kPos)) + 110.0f * std::sin(a), z = u32_as_f32(ld32(box + mods::kPos + 8)) + 110.0f * std::cos(a);
            for (uint32_t at : {mods::kPos, mods::kPos - 0x14}) {  // current and old (the collision walks from old to current)
                st32(link + at, f32_as_u32(x));
                st32(link + at + 4, ld32(box + mods::kPos + 4));
                st32(link + at + 8, f32_as_u32(z));
            }
            uint16_t face = (uint16_t)(ld16(box + mods::kShapeAngleY) + 0x8000);
            st16(link + mods::kShapeAngleY, face);
            st16(link + mods::kAngleY, face);
            LOG("[postbox] Link put in front of the postbox %08X", box);
        }
    }
    c->r[3] = box;
    f_023A4BB0_orig(c);
}

// daObjTpost::getMsg: a handed picto gets Carlov's answer
extern "C" void f_023A5570_orig(Cpu* c);
extern "C" void hook_023A5570(Cpu* c) {
    using namespace mods;
    uint32_t box = c->r[3];
    if (g_on && ld8(guest_call(c, GC(0x025200D4)) + kPhoto)) {
        bool taken = take_picto(c);
        if (!taken) {  // spat out, as a letter it can't take
            guest_call(c, GC(0x023A3DD0), {box, 3, 0});
            st8(box + 0xA02, 1);
        }
        g_talking = box;
        c->r[3] = taken ? kMsgAccepted : kMsgRefused;
        return;
    }
    c->r[3] = box;  // (the guest_call above left the play object in r3: an item shown went to it, crashed in its spit-out)
    f_023A5570_orig(c);
}

// daObjTpost::next_msgStatus(u32* msgNo): after a picto, "accepted for delivery" and done
extern "C" void f_023A55BC_orig(Cpu* c);
extern "C" void hook_023A55BC(Cpu* c) {
    using namespace mods;
    if (g_on && !g_talking && ld32(c->r[4]) == kMsgWhat) g_offer_pending = true;  // the app asks about a picto after the talk
    if (g_talking && c->r[3] == g_talking) {
        uint32_t msg = c->r[4], no = ld32(msg);
        if (no == kMsgAccepted) {
            st32(msg, kMsgThanks);
            c->r[3] = kStatusContinues;
        } else {
            g_talking = 0;
            c->r[3] = kStatusEnds;
            lend_return();  // (the Picto Box lent to a button for this: the button's own item back)
        }
        return;
    }
    f_023A55BC_orig(c);
}
