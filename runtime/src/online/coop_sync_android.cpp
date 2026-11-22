// Upstream's mods/coop_sync.cpp (online co-op: the session), built through this file so the fork can add its own
// message without changing upstream's file (online/compat/README.md): its net::poll goes through android_poll, which
// takes the fork's message types (online/android_models.cpp: which player model a player plays with, and a model sent
// over) and hands every other one on. Upstream's games never see these: their handler drops a type it doesn't know.
// (the JIT stubs it calls, named here for the stub scan: f_0252012C_orig f_025202F8_orig)
#include "../mods/coop_net.h"
#include "../mods/coop_sync.h"

namespace mods::coop::net {
void android_poll(Handler& h);  // (android_models.cpp)
}

#define poll android_poll
#include "../mods/coop_sync.cpp"
#undef poll

// (fork: Android) which games and save files play online: the Online page's "Plays online" (Native.coopScope). A game
// plays online as a whole (upstream's way: the session runs from the title on, every save file in it) or only in the
// save files chosen (the session runs while one of them is played; at the title and in the others it is off, so the
// party's progress never comes into them). Each such game or file has its own settings (hostui profiles, compat.cpp):
// "<game>" or "<game>-f<n>". The game: "p" the game as it is, "r<n>" randomized game n, "o<n>" online game n.
#include <cstdlib>
#include "../online/compat/overlay/hostui.h"
#include "../online/android_scope.h"

namespace mods { int name_file(); }

namespace mods::coop::sync {
namespace {
std::mutex g_scope_mu;
std::string g_scope_key;   // the game ("" not told yet: offline)
int g_scope = 0;           // 0 off, 1 the whole game, 2 the chosen save files
int g_scope_files = 0;     // bit n-1: save file n (scope 2)
int g_scope_edit = 1;      // the save file whose settings are shown while none of the chosen is played
int g_scope_file_now = 0;  // the save file being played (0: the title, the file select)
bool g_scope_on = false;
}  // namespace

namespace {
// the profile for this game and scope with this save file played; whether the session runs
std::string profile_for(const std::string& key, int scope, int files, int edit, int file, bool& on) {
    const bool chosen = file >= 1 && file <= 3 && (files >> (file - 1) & 1);
    on = false;
    if (key.empty()) return "";
    if (scope == 2) return on = chosen, key + "-f" + std::to_string(chosen ? file : edit);
    on = scope == 1;
    return key;
}
// those settings in use (read again when they are another profile's)
void use_profile(const std::string& profile, bool on) {
    if (!hostui::set_profile(profile)) return;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_set_read = false, g_set = Settings{}, g_set_gen++;
    }
    settings();
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_set.on = on;  // (the scope's, not the profile's switch)
    }
    mods::trace("coop: settings of %s", profile.empty() ? "(none)" : profile.c_str());
}
}  // namespace

void android_set_scope(const std::string& key, int scope, int files, int edit) {
    int file;
    {
        std::lock_guard<std::mutex> lk(g_scope_mu);
        g_scope_key = key, g_scope = scope, g_scope_files = files & 7, g_scope_edit = edit >= 1 && edit <= 3 ? edit : 1;
        file = g_scope_file_now;
        edit = g_scope_edit;
    }
    bool on;
    const std::string profile = profile_for(key, scope, files & 7, edit, file, on);
    use_profile(profile, on || getenv("WWHD_ONLINE_SERVER") || getenv("WWHD_COOP"));  // (at once: the page shows them next)
}

int android_online_files() {
    std::lock_guard<std::mutex> lk(g_scope_mu);
    if (g_scope_key.empty()) return 0;
    return g_scope == 1 ? 7 : g_scope == 2 ? g_scope_files : 0;
}

ScopeNow android_scope_now() {
    std::lock_guard<std::mutex> lk(g_scope_mu);
    return {hostui::profile(), g_scope_file_now, g_scope_on};
}

namespace {
// each step, before the session's poll: the profile in use and whether the session should run
bool scope_step() {
    std::string key;
    int scope, files, edit;
    {
        std::lock_guard<std::mutex> lk(g_scope_mu);
        key = g_scope_key, scope = g_scope, files = g_scope_files, edit = g_scope_edit;
    }
    const std::string stage = stage_now();
    const bool title = stage.empty() || stage == "sea_T" || stage == "Name" || stage == "ENDumi";
    const int file = title ? 0 : mods::name_file();
    bool on;
    const std::string profile = profile_for(key, scope, files, edit, file, on);
    static const bool forced = getenv("WWHD_ONLINE_SERVER") || getenv("WWHD_COOP");  // (test runs: online as told)
    if (forced) on = true;
    use_profile(profile, on);
    settings();  // (read once)
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (g_set.on != on) {
            g_set.on = on, g_set_gen++;
            mods::trace("coop: %s (%s, file %d)", on ? "online" : "offline", profile.empty() ? "no game told" : profile.c_str(), file);
        }
    }
    {
        std::lock_guard<std::mutex> lk(g_scope_mu);
        g_scope_file_now = file, g_scope_on = on;
    }
    return on;
}
}  // namespace
}  // namespace mods::coop::sync

// (the session's own poll, renamed with it above; whether it runs is the scope's, not the settings')
void mods::coop::sync::poll(Cpu* c, bool) { android_poll(c, scope_step()); }
