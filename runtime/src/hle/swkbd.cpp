// swkbd: the system software keyboard. Text is entered through a host dialog
// (or WWHD_SWKBD_TEXT for unattended runs) instead of the GamePad keyboard.
#include <atomic>
#include <mutex>
#include <unistd.h>

#include "../input.h"
#include "../runtime.h"

namespace {

constexpr int kMaxForm = 4096;      // 16-bit characters
constexpr uint32_t kStateBlank = 0, kStateDisplayed = 2;

struct State {
    std::mutex mu;
    bool active = false;
    bool keyboard_only = false;     // SwkbdAppearKeyboard (text goes to the receiver buffer)
    bool decided = false, cancelled = false;
    bool pending = false;           // host prompt finished, not yet applied on a guest thread
    bool pending_ok = false;
    std::u16string pending_text;
    std::u16string text;
    int max_len = kMaxForm - 1;
    uint32_t receiver[6] = {};      // ReceiverArg: IEventReceiver*, stringBuf, stringBufSize, fixedCharLimit, cursorPos, selectFrom
    uint32_t form_buf = 0;          // guest copy for SwkbdGetInputFormString
    uint32_t change_param = 0;
};
State S;
std::atomic<int> g_need_font{0}, g_need_predict{0};

std::u16string read_u16(uint32_t addr, int max) {
    std::u16string s;
    if (!addr) return s;
    for (int i = 0; i < max; i++) {
        uint16_t ch = ld16(addr + i * 2);
        if (!ch) break;
        s.push_back(ch);
    }
    return s;
}

std::string narrow(const std::u16string& s) {
    std::string o;
    for (char16_t ch : s) o.push_back(ch < 0x80 ? (char)ch : '?');
    return o;
}

void read_receiver(uint32_t arg) {
    for (int i = 0; i < 6; i++) S.receiver[i] = ld32(arg + i * 4);
}

void start_prompt() {
    S.decided = S.cancelled = S.pending = false;
    if (const char* t = getenv("WWHD_SWKBD_TEXT")) {
        S.pending_text.assign(t, t + strlen(t));
        if ((int)S.pending_text.size() > S.max_len) S.pending_text.resize(S.max_len);
        S.pending_ok = S.pending = true;
        return;
    }
    input::prompt_text(S.text, S.max_len, [](bool ok, std::u16string text) {
        std::lock_guard<std::mutex> lk(S.mu);
        S.pending_text = std::move(text);
        S.pending_ok = ok;
        S.pending = true;
    });
}

// Push the text into the app's receiver buffer and notify its IEventReceiver.
void text_changed(Cpu* c) {
    uint32_t buf = S.receiver[1], size = S.receiver[2];
    if (buf && size > 1) {
        uint32_t n = std::min<uint32_t>(size - 1, S.text.size());
        for (uint32_t i = 0; i < n; i++) st16(buf + i * 2, S.text[i]);
        st16(buf + n * 2, 0);
    }
    uint32_t receiver = S.receiver[0];
    if (receiver) {
        uint32_t vtable = ld32(receiver);
        uint32_t cb = vtable ? ld32(vtable + 0x0C) : 0;
        if (cb) {
            if (!S.change_param) S.change_param = mem::runtime_alloc(8);
            st32(S.change_param, 0);
            st32(S.change_param + 4, (uint32_t)S.text.size());
            guest_call(c, cb, {receiver, S.change_param});
        }
    }
}

void sleep_ms(int ms) {
    BlockingScope b;
    usleep(ms * 1000);
}

}  // namespace

#define SWKBD(name) HLE(swkbd, name)

SWKBD(SwkbdCreate__3RplFPUcQ3_2nn5swkbd10RegionTypeUiP8FSClient) {
    if (!S.form_buf) S.form_buf = mem::runtime_alloc(kMaxForm * 2);
    g_need_font = 3;
    g_need_predict = 3;
    ret(c, 1);
}
SWKBD(SwkbdDestroy__3RplFv) { S.active = false; }

SWKBD(SwkbdGetStateKeyboard__3RplFv) { ret(c, S.active ? kStateDisplayed : kStateBlank); }
SWKBD(SwkbdGetStateInputForm__3RplFv) { ret(c, S.active ? kStateDisplayed : kStateBlank); }

SWKBD(SwkbdSetReceiver__3RplFRCQ3_2nn5swkbd11ReceiverArg) { read_receiver(arg(c, 0)); ret(c, 1); }

SWKBD(SwkbdAppearInputForm__3RplFRCQ3_2nn5swkbd9AppearArg) {
    uint32_t a = arg(c, 0);
    std::lock_guard<std::mutex> lk(S.mu);
    int32_t max = (int32_t)ld32(a + 0xD0);
    S.max_len = max <= 0 ? kMaxForm - 1 : std::min(max, kMaxForm - 1);
    S.text = read_u16(ld32(a + 0xC8), S.max_len);
    S.active = true;
    S.keyboard_only = false;
    LOG("[swkbd] input form (max %d, initial \"%s\")", S.max_len, narrow(S.text).c_str());
    start_prompt();
    ret(c, 1);
}

SWKBD(SwkbdAppearKeyboard__3RplFRCQ3_2nn5swkbd11KeyboardArg) {
    uint32_t a = arg(c, 0);
    std::lock_guard<std::mutex> lk(S.mu);
    read_receiver(a + 0xA8);
    uint32_t size = S.receiver[2];
    S.max_len = size > 1 ? std::min<int>(size - 1, kMaxForm - 1) : 0;
    S.text.clear();
    S.active = true;
    S.keyboard_only = true;
    LOG("[swkbd] keyboard (max %d)", S.max_len);
    start_prompt();
    ret(c, 1);
}

SWKBD(SwkbdDisappearInputForm__3RplFv) { S.active = false; ret(c, 1); }
SWKBD(SwkbdDisappearKeyboard__3RplFv) { S.active = false; ret(c, 1); }

// The game polls this every frame: apply a finished prompt here, on a guest thread.
SWKBD(SwkbdCalc__3RplFRCQ3_2nn5swkbd14ControllerInfo) {
    std::unique_lock<std::mutex> lk(S.mu);
    if (!S.pending || !S.active) return;
    S.pending = false;
    if (S.pending_ok) {
        S.text = S.pending_text;
        LOG("[swkbd] entered \"%s\"", narrow(S.text).c_str());
        lk.unlock();
        text_changed(c);
        lk.lock();
        S.decided = true;
    } else {
        LOG("[swkbd] cancelled");
        S.cancelled = true;
    }
}

SWKBD(SwkbdGetInputFormString__3RplFv) {
    std::lock_guard<std::mutex> lk(S.mu);
    size_t n = std::min<size_t>(S.text.size(), kMaxForm - 1);
    for (size_t i = 0; i < n; i++) st16(S.form_buf + i * 2, S.text[i]);
    st16(S.form_buf + n * 2, 0);
    ret(c, S.form_buf);
}

SWKBD(SwkbdSetInputFormString__3RplFPCw) {
    std::lock_guard<std::mutex> lk(S.mu);
    S.text = read_u16(arg(c, 0), kMaxForm - 1);
}

SWKBD(SwkbdIsDecideOkButton__3RplFPb) { ret(c, S.decided ? 1 : 0); }
SWKBD(SwkbdIsDecideCancelButton__3RplFPb) { ret(c, S.cancelled ? 1 : 0); }

SWKBD(SwkbdGetDrawStringInfo__3RplFPQ3_2nn5swkbd14DrawStringInfo) {
    uint32_t p = arg(c, 0);
    for (int i = 0; i < 6; i++) st32(p + i * 4, 0xFFFFFFFF);
    st8(p + 0x18, 0);
    ret(c, 0);
}

SWKBD(SwkbdInitLearnDic__3RplFPv) {
    // empty learning dictionary in the layout the system library expects (from Cemu)
    uint32_t d = arg(c, 0);
    if (!d) { ret(c, 0); return; }
    constexpr uint32_t kSize = 0xA460, kEntries = 1000, kStride = 0x20, kIndex = kEntries + 1, kMagic = 0x4E4A4443;
    uint32_t first = 0x48, second = first + kIndex * 2, index_end = second + kIndex * 2;
    memset(mem::ptr(d), 0, kSize);
    st32(d + 0x00, kMagic);
    st32(d + 0x04, 0x30000);
    st32(d + 0x08, 0x80020000);
    st32(d + 0x0C, kSize - 0x48);
    st32(d + 0x10, 0x2C);
    st32(d + 0x14, 0x6E);
    st32(d + 0x18, 0x6E);
    st32(d + 0x1C, index_end);
    st32(d + 0x28, kEntries);
    st32(d + 0x2C, kStride);
    st32(d + 0x3C, first);
    st32(d + 0x40, second);
    st32(d + 0x44, index_end + kEntries * kStride);
    st32(d + kSize - 4, kMagic);
    ret(c, 1);
}

// background work the real library does on helper threads; pretend there is a little
SWKBD(SwkbdIsNeedCalcSubThreadFont__3RplFv) { ret(c, g_need_font > 0); }
SWKBD(SwkbdIsNeedCalcSubThreadPredict__3RplFv) { ret(c, g_need_predict > 0); }
SWKBD(SwkbdCalcSubThreadFont__3RplFv) { if (g_need_font > 0) { g_need_font--; sleep_ms(5); } }
SWKBD(SwkbdCalcSubThreadPredict__3RplFv) { if (g_need_predict > 0) { g_need_predict--; sleep_ms(5); } }

// drawing and options: the host dialog stands in for both screens
SWKBD(SwkbdDrawTV__3RplFv) {}
SWKBD(SwkbdDrawDRC__3RplFv) {}
SWKBD(SwkbdMuteAllSound__3RplFb) {}
SWKBD(SwkbdSetUserSoundObj__3RplFPQ3_2nn5swkbd9ISoundObj) {}
SWKBD(SwkbdSetUserControllerEventObj__3RplFPQ3_2nn5swkbd19IControllerEventObj) {}
SWKBD(SwkbdSetSelectFrom__3RplFi) {}
SWKBD(SwkbdSetCursorPos__3RplFi) {}
SWKBD(SwkbdSetEnableOkButton__3RplFb) {}
SWKBD(SwkbdSetControllerRemo__3RplFQ3_2nn5swkbd14ControllerType) {}
SWKBD(SwkbdInactivateSelectCursor__3RplFv) {}
SWKBD(SwkbdConfirmUnfixAll__3RplFv) {}
SWKBD(SwkbdIsSelectCursorActive__3RplFv) { ret(c, 0); }
SWKBD(SwkbdIsCoveredWithSubWindow__3RplFv) { ret(c, 0); }
SWKBD(SwkbdIsKeyboardTarget__3RplFPQ3_2nn5swkbd14IEventReceiver) { ret(c, 0); }
SWKBD(SwkbdGetKeyboardCondition__3RplFPQ3_2nn5swkbd17KeyboardCondition) { ret(c, 0); }
