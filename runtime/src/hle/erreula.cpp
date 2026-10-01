// erreula: the system error viewer. Errors are logged and confirmed automatically (as if the
// player pressed the first button), so the game never waits on a dialog nobody can see.
#include <chrono>

#include "../runtime.h"

namespace {
enum State : uint32_t { kHidden = 0, kAppearing = 1, kVisible = 2, kDisappearing = 3 };
State g_state = kHidden;
bool g_decided = false;
std::chrono::steady_clock::time_point g_since;
bool g_home_nix = false;

std::string u16(uint32_t addr) {
    std::string s;
    for (int i = 0; addr && i < 512; i++) {
        uint16_t c = ld16(addr + i * 2);
        if (!c) break;
        s.push_back(c < 0x80 ? (char)c : '?');
    }
    return s;
}

void set_state(State s) {
    g_state = s;
    g_since = std::chrono::steady_clock::now();
}
double elapsed() { return std::chrono::duration<double>(std::chrono::steady_clock::now() - g_since).count(); }
}  // namespace

#define ERREULA(name) HLE(erreula, name)

ERREULA(ErrEulaCreate__3RplFPUcQ3_2nn7erreula10RegionTypeQ3_2nn7erreula8LangTypeP8FSClient) { set_state(kHidden); ret(c, 1); }
ERREULA(ErrEulaDestroy__3RplFv) { set_state(kHidden); }

ERREULA(ErrEulaAppearError__3RplFRCQ3_2nn7erreula9AppearArg) {
    uint32_t a = arg(c, 0);
    LOG("[erreula] error %u (type %u): %s %s", ld32(a + 0x10), ld32(a + 0x0), u16(ld32(a + 0x24)).c_str(), u16(ld32(a + 0x18)).c_str());
    g_decided = false;
    set_state(kAppearing);
}
ERREULA(ErrEulaDisappearError__3RplFv) {
    if (g_state == kVisible) set_state(kDisappearing);
}
// advance fades; once visible for a moment, confirm with the first (left) button
ERREULA(ErrEulaCalc__3RplFRCQ3_2nn7erreula14ControllerInfo) {
    if (g_state == kAppearing && elapsed() > 0.2) set_state(kVisible);
    else if (g_state == kDisappearing && elapsed() > 0.2) set_state(kHidden);
    else if (g_state == kVisible && !g_decided && elapsed() > 1.0) g_decided = true;
}
ERREULA(ErrEulaGetStateErrorViewer__3RplFv) { ret(c, g_state); }
ERREULA(ErrEulaIsDecideSelectButtonError__3RplFv) { ret(c, g_decided); }
ERREULA(ErrEulaIsDecideSelectLeftButtonError__3RplFv) { ret(c, g_decided); }
ERREULA(ErrEulaIsDecideSelectRightButtonError__3RplFv) { ret(c, 0); }
ERREULA(ErrEulaGetSelectButtonNumError__3RplFv) { ret(c, 0); }
ERREULA(ErrEulaGetResultCode__3RplFv) { ret(c, 0); }
ERREULA(ErrEulaGetResultType__3RplFv) { ret(c, g_decided ? 1 : 0); }
ERREULA(ErrEulaAppearHomeNixSign__3RplFRCQ3_2nn7erreula14HomeNixSignArg) { g_home_nix = true; }
ERREULA(ErrEulaIsAppearHomeNixSign__3RplFv) { ret(c, g_home_nix); }
ERREULA(ErrEulaDisappearHomeNixSign__3RplFv) { g_home_nix = false; }
ERREULA(ErrEulaChangeLang__3RplFQ3_2nn7erreula8LangType) {}
ERREULA(ErrEulaDrawTV__3RplFv) {}
ERREULA(ErrEulaDrawDRC__3RplFv) {}
ERREULA(ErrEulaIsSelectCursorActive__3RplFv) { ret(c, 0); }
ERREULA(ErrEulaSetControllerRemo__3RplFQ3_2nn7erreula14ControllerType) {}
