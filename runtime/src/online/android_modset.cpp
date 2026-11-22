// Online co-op: this player's gameplay mods (mods/coop_modset.h), the Android fork's. Stands in for upstream's
// mods/coop_modset.cpp, which reads upstream's mods manager and packages.
//
// The fork's own features that change how the game plays count, each under an id of its own ("android.*"): they are
// not upstream's mods (another behaviour), so a party whose leader plays upstream's build names each one that
// differs. Look-only and comfort options (camera, touch controls, texts, instant text, hit freezes, icons) never
// count, as upstream's look-only mods. A randomized game is upstream's own entry ("randomizer", the SHA-256 of its
// code file), so the same seed on both builds matches.
#include "mods/coop_modset.h"

#include <cstdio>
#include <string>

#include "mods/mods.h"
#include "online/proto/sha256.h"
#include "online/compat/randomizer_identity.h"
#include "runtime.h"

namespace mods::coop::modset {
namespace {

std::array<uint8_t, 32> sha(const std::string& text) {
    std::array<uint8_t, 32> out{};
    online::proto::sha256(text.data(), text.size(), out.data());
    return out;
}
std::string hex(const uint8_t* d, size_t n) {
    static const char* k = "0123456789abcdef";
    std::string s;
    for (size_t i = 0; i < n; i++) s += k[d[i] >> 4], s += k[d[i] & 15];
    return s;
}
std::string file_sha256(const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return "";
    online::proto::Sha256 h;
    std::vector<uint8_t> buf(1 << 20);
    size_t n;
    while ((n = fread(buf.data(), 1, buf.size(), f)) > 0) h.update(buf.data(), n);
    fclose(f);
    uint8_t out[32];
    h.final(out);
    return hex(out, 32);
}
constexpr const char* kVersion = "android";  // (the fork's features: their behaviour comes with the build)

struct Feature {
    const char* id;
    const char* name;
    bool (*on)();
    std::string (*settings)();
    void (*off)();
};
std::string fmt(const char* f, double v) {
    char b[48];
    snprintf(b, sizeof b, f, v);
    return b;
}
const Feature kFeatures[] = {
    {"android.jump-anywhere", "Jump anywhere", [] { return mods::jump_anywhere(); },
     [] { return std::string(mods::double_jump() ? "double=1;" : "double=0;") + (mods::no_auto_jump() ? "noauto=1;" : "noauto=0;"); },
     [] { mods::set_jump_anywhere(false); }},
    {"android.leaf-lift", "Deku Leaf lift", [] { return mods::leaf_lift(); },
     [] { return "height=" + std::to_string(mods::leaf_lift_height()) + ";"; }, [] { mods::set_leaf_lift(false); }},
    {"android.walk-doors", "Walk-through doors", [] { return mods::walk_doors(); },
     [] { return std::string(mods::walk_doors_by_a() ? "a=1;" : "a=0;"); }, [] { mods::set_walk_doors(false); }},
    {"android.quick-doors", "Quick doors", [] { return mods::quick_doors() || mods::fast_scenes(); },
     [] { return std::string(mods::quick_doors() ? "doors=1;" : "doors=0;") + (mods::fast_scenes() ? "scenes=1;" : "scenes=0;"); },
     [] { mods::set_quick_doors(false), mods::set_fast_scenes(false); }},
    {"android.speeds", "Movement speeds",
     [] { return mods::run_speed() != 1.0f || mods::swim_speed() != 1.0f || mods::climb_speed() != 1.0f || mods::sidle_speed() != 1.0f; },
     [] {
         return fmt("run=%.3f;", mods::run_speed()) + fmt("swim=%.3f;", mods::swim_speed()) + fmt("climb=%.3f;", mods::climb_speed()) +
                fmt("sidle=%.3f;", mods::sidle_speed());
     },
     [] { mods::set_run_speed(1), mods::set_swim_speed(1), mods::set_climb_speed(1), mods::set_sidle_speed(1); }},
    {"android.cheats", "Cheats",
     [] {
         for (int b = 1; b <= mods::kFastPush; b <<= 1)
             if (mods::infinite(b)) return true;
         return false;
     },
     [] {
         int mask = 0;
         for (int b = 1; b <= mods::kFastPush; b <<= 1)
             if (mods::infinite(b)) mask |= b;
         return "mask=" + std::to_string(mask) + ";";
     },
     [] {
         for (int b = 1; b <= mods::kFastPush; b <<= 1) mods::set_infinite(b, false);
     }},
};

}  // namespace

std::string name_of(const std::string& id) {
    if (id == "randomizer") return "Randomizer seed";
    for (const Feature& f : kFeatures)
        if (id == f.id) return f.name;
    return id;
}
std::string describe(const std::vector<Problem>& ps) {
    std::string out;
    for (size_t i = 0; i < ps.size() && i < 6; i++) out += (i ? "; " : "") + words(ps[i], name_of(ps[i].id));
    if (ps.size() > 6) out += "; and " + std::to_string(ps.size() - 6) + " more";
    return out;
}

Set current() {
    Set out;
    for (const Feature& f : kFeatures) {
        if (!f.on()) continue;
        ModRef r;
        r.id = f.id, r.version = kVersion, r.name = f.name;
        r.hash = sha(std::string(f.id) + "\n" + kVersion + "\n" + f.settings());
        out.push_back(std::move(r));
    }
    if (randomizer::compiled_seed()) {  // (upstream's entry: its code file's SHA-256, the seed and its settings)
        static const std::string rpx_sha = file_sha256(randomizer::game_rpx(config::game_dir));
        ModRef r;
        r.id = "randomizer", r.name = "Randomizer seed";
        r.hash = sha(std::string("randomizer\n") + rpx_sha);
        out.push_back(std::move(r));
    }
    if (out.size() > kMaxMods) out.resize(kMaxMods);
    return out;
}

bool can_use(const Set& party, std::string& why) {
    const Set mine = current();
    for (const ModRef& p : party) {
        if (std::find(mine.begin(), mine.end(), p) != mine.end()) continue;
        if (p.id == "randomizer") return why = "the party plays another randomized game (start that seed to join)", false;
        bool ours = false;
        for (const Feature& f : kFeatures) ours |= p.id == f.id;
        if (!ours) return why = p.id + " " + p.version + " is not in the Android version", false;
        return why = name_of(p.id) + ": set it like the party (Options), its settings differ", false;
    }
    return true;
}

bool use(const Set& party, bool& restart, std::string& why) {
    restart = false;
    if (!can_use(party, why)) return false;
    // the party's set has none of this game's other gameplay features: those off (the app's options follow:
    // Native.coopModsOff)
    for (const ModRef& m : current()) {
        if (m.id == "randomizer" || std::find(party.begin(), party.end(), m) != party.end()) continue;
        for (const Feature& f : kFeatures)
            if (m.id == f.id) f.off();
    }
    mods::trace("coop: switched to the party's mods (%zu)", party.size());
    return true;
}

}  // namespace mods::coop::modset
