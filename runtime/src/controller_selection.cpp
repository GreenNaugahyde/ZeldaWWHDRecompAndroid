// The title's controller question ("which controller?", before the file select), answered with the
// GamePad: every controller and the touch controls act as the GamePad, and the game asks even with
// no Pro Controller connected. Port of upstream's #189 (8e9555b, de09553), fixed to the GamePad: it
// supplies an input event and lets the original selection handler do the availability checks,
// outgoing event, animations and controller-mode change. WWHD_CONTROLLER_SELECT=1 asks again.
#include "input.h"
#include "runtime.h"
#include "guest_addr.h"

namespace {
bool object(uint32_t p) { return p >= mem::kMem2Start && p < mem::kMem2End - 0x200; }

bool enabled() {
    static const bool ask = getenv("WWHD_CONTROLLER_SELECT") != nullptr;
    return !ask && !input::pro_controller();
}

// the title/name controller's state machine (at +0x10): states are objects (USA 1049EA2C ControllerSelect,
// 1049EA58 SaveDataSelect = the file select, 1049EAB0 Cancel = back to the title), the previous one
// at +0x1C; a state's id comes from its vtable +0x14
constexpr uint32_t kControllerSelect = 0x1049EA2C, kSaveDataSelect = 0x1049EA58, kCancel = 0x1049EAB0;

uint32_t state_id(Cpu* c, uint32_t state) {
    uint32_t vt = ld32(state + 8);
    return object(vt) ? guest_call(c, ld32(vt + 0x14), {state}) : 0;
}

// the owner is in its Cancel state (on its way back to the title)
bool cancelling(Cpu* c) {
    if (!enabled()) return false;
    uint32_t owner = ld32(GD(0x101F82EC));
    if (!object(owner)) return false;
    uint32_t state = guest_call(c, GC(0x02006478), {owner + 0x10});
    if (!object(state)) return false;
    uint32_t id = state_id(c, state);
    return id && id == state_id(c, GD(kCancel));
}

// the GamePad's place on the screen (0), or -1: not the title's question, or it isn't complete
int option(Cpu* c, uint32_t screen, bool background_transition = false) {
    if (!enabled() || !object(screen)) return -1;
    if (!object(ld32(GD(0x101F4FF0)))) return -1;
    uint32_t owner = ld32(GD(0x101F82EC)); // title/name controller (created by 0270BE70)
    if (!object(owner) || ld32(owner + 0x130) != screen) return -1;
    // Only the title's SelectController state (enter 0270AAAC, execute 0270AB10).
    // Reconnect/error prompts and the in-game controller menu are left alone.
    uint32_t state = guest_call(c, GC(0x02006478), {owner + 0x10});
    if (!object(state)) return -1;
    uint32_t vt = ld32(state + 8);
    if (!object(vt)) return -1;
    uint32_t key = guest_call(c, ld32(vt + 0x14), {state});
    uint32_t expected = GD(kControllerSelect);
    vt = ld32(expected + 8);
    if (!object(vt)) return -1;
    if (key != guest_call(c, ld32(vt + 0x14), {expected})) {
        // The background fades its old caption after the owner has entered file
        // select. Hide that outgoing caption until the game's text update runs.
        uint32_t background = ld32(owner + 0x12C);
        uint32_t file_select = GD(kSaveDataSelect);
        vt = ld32(file_select + 8);
        if (!background_transition || ld32(owner + 0x1C) != expected || !object(background) ||
            ld32(background + 0x50) != 1 || ld32(background + 0x54) != 0 || !object(vt) ||
            key != guest_call(c, ld32(vt + 0x14), {file_select})) return -1;
    }
    if (ld32(screen + 0x48) != 2 || !object(ld32(screen + 0x4C))) return -1;
    if (ld32(GD(0x10102B80)) == ld32(GD(0x10102B80) + 4)) return -1;
    // Validate both named child slots; do not use the game's fallback-to-child-0.
    for (unsigned i = 0; i < 2; ++i) {
        uint32_t index = ld32(GD(0x10102B80) + i * 4);
        if (index >= 2) return -1;
        uint32_t child = ld32(ld32(screen + 0x4C) + index * 4);
        if (!object(child) || !object(ld32(child + 0x50))) return -1;
    }
    return guest_call(c, GC(0x026D43AC), {screen}) != 0 ? 0 : -1;  // the GamePad is available
}
}  // namespace

// After updateSelection copied the current input event into its local frame,
// before it reads the event kind. The original code consumes this exactly as a
// button's SPECIAL (17) event. No game flags, mode or selection state are written.
// Back from the file select (B / Back there leads to this question): the question's own Cancel
// instead, as its B does (the flag updateSelection sets for it, read by the owner's execute
// 0270AB10), so Back goes on to the title.
extern "C" void site_026D44A8(Cpu* c) {
    Cpu saved = *c;
    uint32_t screen = c->r[28];
    int selected = option(c, screen);
    if (selected >= 0 && ld32(ld32(GD(0x101F82EC)) + 0x1C) == GD(kSaveDataSelect)) {
        st8(screen + 0x50, 1);
        LOG("[controller-select] back from the file select: to the title");
    } else if (selected >= 0) {
        uint32_t index = ld32(GD(0x10102B80) + selected * 4);
        uint32_t child = ld32(ld32(screen + 0x4C) + index * 4);
        uint32_t receiver = ld32(child + 0x50);
        uint32_t id = guest_call(c, GC(0x026F5B74), {child});
        // updateSelection also accepts the focused receiver. Move focus using
        // the game's own setter, or its previous focus could win before this id.
        guest_call(c, GC(0x02615154), {ld32(GD(0x101F4FF0)), receiver});
        guest_call(c, GC(0x02001338), {saved.r[1] + 0x30, 17, id, 0});
        LOG("[controller-select] title: GamePad chosen");
    }
    *c = saved;
}

// Keep the title prompt invisible while its own in/out animations finish.
extern "C" void f_026D50C4_orig(Cpu*);
extern "C" void hook_026D50C4(Cpu* c) {
    Cpu saved = *c;
    bool automatic = option(c, c->r[3]) >= 0 || cancelling(c);
    *c = saved;
    if (!automatic) f_026D50C4_orig(c);
}

// StartBackground is a separate part, so hiding SelectController_00 alone
// leaves its heading visible. Suppress only this owner's background draw until
// the normal caption transition has replaced it with File Selection.
extern "C" void f_026F8B54_orig(Cpu*);
// Parts::draw runs for every HUD part each frame: only the title background pays for a Cpu copy.
extern "C" void hook_026F8B54(Cpu* c) {
    if (enabled()) {
        uint32_t owner = ld32(GD(0x101F82EC));
        if (object(owner) && ld32(owner + 0x12C) == c->r[3]) {
            Cpu saved = *c;
            bool automatic = option(c, ld32(owner + 0x130), true) >= 0 || cancelling(c);
            *c = saved;
            if (automatic) return;
        }
    }
    f_026F8B54_orig(c);
}

// The game's own Options > Controller: its switch handler (026D87D0, the row's entry in the option
// table at 101030D8) checks whether the other controller is available and enters the ChangeConnect
// state ("Please connect a Wii U Pro Controller." when it isn't). Every controller acts as the
// GamePad, so the row is locked: it keeps showing "Wii U GamePad" and changing it does nothing
// (the handler's "not available" answer, without the message). Left / right / A (026D6650) step the
// row's value (+0x104 + row, Controller = row 3: 0 GamePad, 1 Pro) before they call the handler and
// redraw the row after it: the value goes back to the GamePad here, so it doesn't show "Pro Controller".
extern "C" void f_026D87D0_orig(Cpu*);
extern "C" void hook_026D87D0(Cpu* c) {
    if (input::pro_controller()) { f_026D87D0_orig(c); return; }
    st8(c->r[3] + 0x107, 0);
    c->r[3] = 0;
}
