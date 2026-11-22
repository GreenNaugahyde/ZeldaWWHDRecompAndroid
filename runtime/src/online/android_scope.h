// (fork: Android) which games and save files play online (online/coop_sync_android.cpp)
#pragma once
#include <string>

namespace mods::coop::sync {
// key: "p" the game as it is, "r<n>" randomized game n, "o<n>" online game n ("" offline); scope 0 off, 1 the whole
// game, 2 the save files in files (bit n-1: file n); edit: the file whose settings are used while none of them is played
void android_set_scope(const std::string& key, int scope, int files, int edit);
struct ScopeNow {
    std::string profile;  // the settings in use
    int file = 0;         // the save file being played (0 none)
    bool on = false;      // the session should run
};
ScopeNow android_scope_now();
// the save files that play online (bit n-1: file n; all three for a game online as a whole), for their marks
int android_online_files();
}  // namespace mods::coop::sync
