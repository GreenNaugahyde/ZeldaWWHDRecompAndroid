// coreinit: logging, dynamic loading, system info, and small odds and ends.
#include <cstdlib>
#include <cstring>
#include "../true60.h"
#include <sys/stat.h>
#include <ctime>
#include <vector>
#include <algorithm>
#include <mutex>
#include <string>
#include <unordered_map>

#include "../platform.h"
#include "../release.h"
#include "../runtime.h"

// ---------------------------------------------------------------- guest printf
// Arguments come either from registers (OSReport: r4.., f1..) or a PPC va_list.
struct GuestArgs {
    Cpu* c = nullptr;
    int gpr = 0, fpr = 0;             // next register index (0-based from r3 / f1)
    uint32_t reg_save = 0, overflow = 0;  // va_list mode when reg_save != 0
    uint32_t next_u32() {
        if (reg_save) {
            if (gpr < 8) return ld32(reg_save + 4 * gpr++);
            uint32_t v = ld32(overflow);
            overflow += 4;
            return v;
        }
        if (gpr < 8) return c->r[3 + gpr++];
        uint32_t v = ld32(overflow);
        overflow += 4;
        return v;
    }
    uint64_t next_u64() {
        if (gpr & 1) gpr++;  // 64-bit values use aligned register pairs
        uint64_t hi = next_u32();
        return hi << 32 | next_u32();
    }
    double next_f64() {
        if (reg_save) {
            if (fpr < 8) return u64_as_f64(ld64(reg_save + 32 + 8 * fpr++));
            overflow = (overflow + 7) & ~7u;
            double v = u64_as_f64(ld64(overflow));
            overflow += 8;
            return v;
        }
        if (fpr < 8) return c->f[1 + fpr++].ps0;
        overflow = (overflow + 7) & ~7u;
        double v = u64_as_f64(ld64(overflow));
        overflow += 8;
        return v;
    }
};

std::string guest_format(const std::string& fmt, GuestArgs& a) {
    std::string out;
    char buf[512];
    for (size_t i = 0; i < fmt.size(); i++) {
        if (fmt[i] != '%') { out += fmt[i]; continue; }
        size_t j = i + 1;
        std::string spec = "%";
        while (j < fmt.size() && strchr("-+ #0123456789.*", fmt[j])) spec += fmt[j++];
        int lcount = 0;
        while (j < fmt.size() && strchr("hlLqjzt", fmt[j])) { if (fmt[j] == 'l' || fmt[j] == 'q' || fmt[j] == 'L') lcount++; j++; }
        if (j >= fmt.size()) break;
        char conv = fmt[j];
        i = j;
        switch (conv) {
        case '%': out += '%'; break;
        case 'd': case 'i': case 'u': case 'x': case 'X': case 'o': case 'c':
            if (lcount >= 2) snprintf(buf, sizeof buf, (spec + "ll" + conv).c_str(), (long long)a.next_u64());
            else if (conv == 'd' || conv == 'i') snprintf(buf, sizeof buf, (spec + conv).c_str(), (int)a.next_u32());
            else snprintf(buf, sizeof buf, (spec + conv).c_str(), a.next_u32());
            out += buf;
            break;
        case 'p': snprintf(buf, sizeof buf, "0x%08X", a.next_u32()); out += buf; break;
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G':
            snprintf(buf, sizeof buf, (spec + conv).c_str(), a.next_f64());
            out += buf;
            break;
        case 's': {
            uint32_t p = a.next_u32();
            std::string s = p ? mem::read_cstr(p) : "(null)";
            snprintf(buf, sizeof buf, (spec + "s").c_str(), s.c_str());
            out += buf;
            break;
        }
        default: out += spec + conv; break;
        }
    }
    return out;
}

static void report(const std::string& s) {
    std::string t = s;
    while (!t.empty() && (t.back() == '\n' || t.back() == '\r')) t.pop_back();
    LOG("[game] %s", t.c_str());
}

HLE(coreinit, OSReport) {
    GuestArgs a;
    a.c = c;
    a.gpr = 1;
    a.overflow = c->r[1] + 8;
    report(guest_format(mem::read_cstr(arg(c, 0)), a));
}

HLE(coreinit, OSVReport) {
    uint32_t va = arg(c, 1);
    GuestArgs a;
    a.c = c;
    a.gpr = ld8(va);
    a.fpr = ld8(va + 1);
    a.overflow = ld32(va + 4);
    a.reg_save = ld32(va + 8);
    report(guest_format(mem::read_cstr(arg(c, 0)), a));
}

HLE(coreinit, OSConsoleWrite) {
    report(std::string((const char*)mem::ptr(arg(c, 0)), arg(c, 1)));
}


// On a game halt: a crash log as for a native crash (main.cpp: name, GPU, place, app and
// settings), with the halt's message, the 60 fps pass state and the call chain (the native
// backtrace names the game functions; the guest stack's back chain is not kept by the compiled
// code).
namespace interp { const char* phase_name(); }
void crash_log_open();
void crash_log_write(const char* s, int n);
void crash_log_details();
void crash_log_close();
int crash_log_fd();
static void write_crash_log(Cpu* c, const std::string& file, uint32_t line, const std::string& msg) {
    crash_log_open();
    char buf[1024];
    int n = snprintf(buf, sizeof buf, "\nHALT (game assertion) at %s:%u: %s\n", file.c_str(), line, msg.c_str());
    crash_log_write(buf, std::min<int>(n, sizeof buf - 1));
    // sead's halt (0273AA24 -> 0273AB58 -> 0273AAFC) receives the failed check's file, line and
    // message, but passes OSPanic only its own place; the caller frames' saved arguments are still
    // on the guest stack: every text found there (printable, from the game's data) is listed
    {
        std::string found;
        uint32_t sp = c->r[1];
        for (uint32_t o = 0; o < 0x260 && found.size() < 600; o += 4) {
            uint32_t p = ld32(sp + o);
            if (p < 0x10000000 || p >= 0x10200000) continue;  // the executable's read-only data
            const char* s = (const char*)mem::ptr(p);
            size_t len = strnlen(s, 160);
            if (len < 4 || len >= 160) continue;
            bool text = true;
            for (size_t i = 0; i < len && text; i++) text = s[i] >= 0x20 && s[i] < 0x7F;
            if (!text) continue;
            std::string t(s, len);
            if (found.find(t) != std::string::npos) continue;
            found += "    \"" + t + "\"\n";
        }
        if (!found.empty()) {
            n = snprintf(buf, sizeof buf, "  texts on the guest stack (the failed check's place and message):\n%s", found.c_str());
            crash_log_write(buf, std::min<int>(n, sizeof buf - 1));
        }
    }
    crash_log_details();
    n = snprintf(buf, sizeof buf, "  60 fps pass: %s; true 60 %s, half pass %d, executing process %08X", interp::phase_name(),
                 true60::enabled() ? "on" : "off", (int)true60::half_pass(), true60::exec_proc());
    if (uint32_t p = true60::exec_proc())
        n += snprintf(buf + n, sizeof buf - n, " (words %08X %08X %08X %08X)", ld32(p), ld32(p + 4), ld32(p + 8), ld32(p + 12));
    n += snprintf(buf + n, sizeof buf - n, ", dt %.2f\n  guest lr %08X\n  host backtrace:\n", true60::dt(), c->lr);
    crash_log_write(buf, std::min<int>(n, sizeof buf - 1));
    platform::print_backtrace(crash_log_fd());
    crash_log_close();
}

HLE(coreinit, OSPanic) {
    GuestArgs a;
    a.c = c;
    a.gpr = 3;
    a.overflow = c->r[1] + 8;
    std::string msg = guest_format(mem::read_cstr(arg(c, 2)), a);
    write_crash_log(c, mem::read_cstr(arg(c, 0)), arg(c, 1), msg);
    fatal("OSPanic at %s:%d: %s", mem::read_cstr(arg(c, 0)).c_str(), arg(c, 1), msg.c_str());
}

// ---------------------------------------------------------------- OSDynLoad
static std::mutex g_dyn_mutex;
static std::unordered_map<uint32_t, std::string> g_dyn_modules;
static std::unordered_map<std::string, uint32_t> g_dyn_exports;

HLE(coreinit, OSDynLoad_Acquire) {
    std::string name = mem::read_cstr(arg(c, 0));
    if (name.size() > 4 && name.substr(name.size() - 4) == ".rpl") name.resize(name.size() - 4);
    uint32_t h = 0x70000000 | (uint32_t)(std::hash<std::string>()(name) & 0x0FFFFFFF);
    {
        std::lock_guard<std::mutex> lk(g_dyn_mutex);
        g_dyn_modules[h] = name;
    }
    TRACE("[dynload] Acquire(%s) -> %08X", name.c_str(), h);
    st32(arg(c, 1), h);
    ret(c, 0);
}

HLE(coreinit, OSDynLoad_Release) {}
HLE(coreinit, OSDynLoad_SetAllocator) { ret(c, 0); }

HLE(coreinit, OSDynLoad_FindExport) {
    uint32_t h = arg(c, 0), is_data = arg(c, 1), out = arg(c, 3);
    std::string sym = mem::read_cstr(arg(c, 2));
    std::lock_guard<std::mutex> lk(g_dyn_mutex);
    std::string lib = g_dyn_modules.count(h) ? g_dyn_modules[h] : "?";
    std::string key = lib + "." + sym;
    auto it = g_dyn_exports.find(key);
    if (it == g_dyn_exports.end()) {
        uint32_t addr = 0;
        if (!is_data) {
            PpcFunc fn = hle_find(lib.c_str(), sym.c_str());
            if (fn) addr = dispatch::register_host(fn, strdup(key.c_str()));
        }
        if (!addr) {
            LOG("[dynload] FindExport(%s, %s) not implemented", key.c_str(), is_data ? "data" : "func");
            // hand out a stub that logs when called, so the game can still look it up
            static void (*stub)(Cpu*) = [](Cpu* c) { log_msg("[dynload] call to unimplemented dynamic export (lr=%08X)", c->lr); c->r[3] = 0; };
            addr = is_data ? mem::runtime_alloc(0x100) : dispatch::register_host(stub, strdup(key.c_str()));
        }
        it = g_dyn_exports.emplace(key, addr).first;
    }
    st32(out, it->second);
    ret(c, 0);
}

// ---------------------------------------------------------------- system info
HLE(coreinit, OSGetSystemInfo) {
    static uint32_t info = 0;
    if (!info) {
        info = mem::runtime_alloc(0x20);
        st32(info + 0x00, 248625000);   // bus clock
        st32(info + 0x04, 1243125000);  // core clock
        st64(info + 0x08, 0);           // base time
        st32(info + 0x10, 0);
    }
    ret(c, info);
}

HLE(coreinit, OSGetSharedData) { ret(c, 0); }
HLE(coreinit, OSIsDebuggerPresent) { ret(c, 0); }
HLE(coreinit, OSIsDebuggerInitialized) { ret(c, 0); }
HLE(coreinit, OSEnableHomeButtonMenu) { ret(c, 1); }
HLE(coreinit, OSSavesDone_ReadyToRelease) {}
HLE(coreinit, IMDisableDim) { ret(c, 0); }
HLE(coreinit, IMEnableDim) { ret(c, 0); }
HLE(coreinit, IMIsDimEnabled) { if (arg(c, 0)) st32(arg(c, 0), 0); ret(c, 0); }
HLE(coreinit, ENVGetEnvironmentVariable) { if (arg(c, 1) && arg(c, 2)) st8(arg(c, 1), 0); ret(c, 1); }
HLE(coreinit, __gh_set_errno) {}

HLE(coreinit, exit) { LOG("[game] exit(%d)", (int)arg(c, 0)); std::exit((int)arg(c, 0)); }
HLE(coreinit, _Exit) { LOG("[game] _Exit(%d)", (int)arg(c, 0)); std::_Exit((int)arg(c, 0)); }

// ---------------------------------------------------------------- UC (system settings)
// UCSysConfig entries are 0x54 bytes: name[64], access u32, dataType u32, error s32, dataSize u32, dataPtr u32
HLE(coreinit, UCOpen) { ret(c, 1); }
HLE(coreinit, UCClose) { ret(c, 0); }
// The console language and country the game sees: WWHD_LANGUAGE (en, fr, de, it, es; the app's
// Language option), limited to the release's languages (USA: English, French, Spanish; EUR: also
// German and Italian), with a country of that language in the release's region. JPN: Japanese, Japan.
static void system_language(uint32_t& language, uint32_t& country) {
    if (release::id() == release::Id::JPN) {
        language = 0;  // cafe.language 0: Japanese
        country = 1;   // Japan
        return;
    }
    const char* l = getenv("WWHD_LANGUAGE");
    std::string want = l ? l : "en";
    const bool eur = release::id() == release::Id::EUR;
    struct Lang { const char* code; uint32_t language, usCountry, euCountry; };
    // cafe.language: 1 English, 2 French, 3 German, 4 Italian, 5 Spanish. Countries: 49 USA,
    // 18 Canada, 36 Mexico; 110 United Kingdom, 77 France, 78 Germany, 83 Italy, 105 Spain
    static const Lang langs[] = {{"en", 1, 49, 110}, {"fr", 2, 18, 77}, {"de", 3, 0, 78}, {"it", 4, 0, 83}, {"es", 5, 36, 105}};
    for (const Lang& g : langs)
        if (want == g.code && (eur ? g.euCountry : g.usCountry)) {
            language = g.language;
            country = eur ? g.euCountry : g.usCountry;
            return;
        }
    language = 1;
    country = eur ? 110 : 49;
}

HLE(coreinit, UCReadSysConfig) {
    uint32_t count = arg(c, 1), items = arg(c, 2);
    for (uint32_t i = 0; i < count; i++) {
        uint32_t e = items + i * 0x54;
        std::string name = mem::read_cstr(e);
        uint32_t size = ld32(e + 0x4C), data = ld32(e + 0x50);
        uint32_t value = 0;
        uint32_t language, country;
        system_language(language, country);
        if (name == "cafe.language") value = language;
        else if (name == "cafe.cntry_reg") value = country;
        else if (name == "cafe.eula_agree") value = 1;
        else if (name == "cafe.initial_launch") value = 2;
        else if (name == "parent.enable") value = 0;
        else if (name == "p_acct1.network_launcher") value = 0;
        TRACE("[uc] read %s (size %u) -> %u", name.c_str(), size, value);
        if (data && size) {
            memset(mem::ptr(data), 0, size);
            if (size == 1) st8(data, value);
            else if (size == 2) st16(data, value);
            else if (size >= 4) st32(data, value);
        }
        st32(e + 0x48, 0);
    }
    ret(c, 0);
}
