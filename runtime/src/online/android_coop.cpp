// Online co-op (upstream's, issue #444), the Android app's side: the Online page's calls (Native.coop*), in place of
// upstream's ImGui pages (mods/coop_settings.cpp), and the one game hook the fork adds for it.
#include <algorithm>
#include <jni.h>

#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

#include "mods/coop_block.h"
#include "mods/coop_emotes.h"
#include "mods/coop_modset.h"
#include "mods/coop_net.h"
#include "mods/coop_pose.h"
#include "mods/coop_pvp.h"
#include "mods/coop_radar.h"
#include "mods/coop_servers.h"
#include "mods/coop_sync.h"
#include "online/android_scope.h"
#include "online/compat/online_compat.h"
#include "online/compat/overlay/extensions.h"
#include "online/compat/overlay/hostui.h"
#include "release.h"
#include "runtime.h"
#include "savestate.h"

namespace net = mods::coop::net;
namespace csync = mods::coop::sync;

// (the device build's stubs for the game functions the runtime calls are found by name in the sources, CMakeLists'
// JIT_NAMES; coop_progress.cpp makes these four with a macro: named here) f_021262D8_orig f_02126334_orig
// f_02128430_orig f_0212848C_orig

// dPa_control_c::set (025A847C, the shared emitter factory): Link's particles, for the other players (coop_pose.cpp)
extern "C" void f_025A847C_orig(Cpu* c);
extern "C" void hook_025A847C(Cpu* c) {
    mods::coop::particle(c);
    f_025A847C_orig(c);
}

namespace {
std::string jstr(JNIEnv* env, jstring s) {
    if (!s) return "";
    const char* c = env->GetStringUTFChars(s, nullptr);
    std::string out = c ? c : "";
    if (c) env->ReleaseStringUTFChars(s, c);
    return out;
}
jstring js(JNIEnv* env, const std::string& s) { return env->NewStringUTF(s.c_str()); }
jobjectArray jarr(JNIEnv* env, const std::vector<std::string>& v) {
    jobjectArray a = env->NewObjectArray((jsize)v.size(), env->FindClass("java/lang/String"), nullptr);
    for (size_t i = 0; i < v.size(); i++) {
        jstring s = js(env, v[i]);
        env->SetObjectArrayElement(a, (jsize)i, s);
        env->DeleteLocalRef(s);
    }
    return a;
}
std::string clean(std::string s) {  // (one line, no separators of the lists below)
    for (char& ch : s)
        if (ch == '\n' || ch == '\t') ch = ' ';
    return s;
}
std::mutex g_overlay_mu;
std::vector<std::string> g_overlay_texts;
}  // namespace

namespace android_models { std::vector<std::string> transfers(); }
#define COOP_FN(ret, name) extern "C" JNIEXPORT ret JNICALL Java_org_wwhdrecomp_app_Native_##name

// the app's internal folder for online (identity key, settings, block list); once at the app's start, before the game
COOP_FN(void, coopInit)(JNIEnv* env, jclass, jstring dir) {
    online_compat::set_dir(jstr(env, dir));
    static std::once_flag once;
    std::call_once(once, [] {  // (upstream's main(): the overlays over the game)
        mods::coop::emotes::register_overlay();
        mods::coop::radar::register_overlay();
        mods::coop::pvp::register_overlay();
    });
}

// the settings as "key=value" lines; written back the same way (a running session restarts for what it started with)
COOP_FN(jstring, coopSettings)(JNIEnv* env, jclass) {
    const csync::Settings s = csync::settings();
    std::string t;
    t += "on=" + std::to_string(s.on) + "\n";
    t += "notice_ok=" + std::to_string(s.notice_ok) + "\n";
    t += "server=" + std::to_string(s.server) + "\n";
    t += "server_addr=" + clean(s.server_addr) + "\n";
    t += "server_port=" + std::to_string(s.server_port) + "\n";
    t += "role=" + std::to_string(s.role) + "\n";
    t += "addr=" + clean(s.addr) + "\n";
    t += "port=" + std::to_string(s.port) + "\n";
    t += "name=" + clean(s.name) + "\n";
    t += "tunic=" + std::to_string(s.tunic) + "\n";
    t += "radar=" + std::to_string(s.radar) + "\n";
    t += "pvp_sea=" + std::to_string(s.pvp_sea) + "\n";
    t += "strangers=" + std::to_string(s.strangers) + "\n";
    t += "together=" + std::to_string(s.stay_together) + "\n";
    {  // (the fork's own: player models over the connection, online/android_models.cpp)
        std::string v;
        t += std::string("model_send=") + (hostui::get("android.model_send", v) && v == "1" ? "1" : "0") + "\n";
        t += std::string("model_receive=") + (hostui::get("android.model_receive", v) && v == "1" ? "1" : "0") + "\n";
        t += std::string("raid=") + (hostui::get("android.raid", v) ? v : "0") + "\n";  // (online/android_raid.cpp)
    }
    return js(env, t);
}
COOP_FN(void, coopSaveSettings)(JNIEnv* env, jclass, jstring text) {
    csync::Settings s = csync::settings();
    const std::string t = jstr(env, text);
    size_t at = 0;
    while (at < t.size()) {
        size_t end = t.find('\n', at);
        if (end == std::string::npos) end = t.size();
        const std::string line = t.substr(at, end - at);
        at = end + 1;
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        const int n = atoi(v.c_str());
        if (k == "on") s.on = n != 0;
        else if (k == "notice_ok") s.notice_ok = n != 0;
        else if (k == "server") s.server = n != 0;
        else if (k == "server_addr" && !v.empty()) s.server_addr = v;
        else if (k == "server_port" && n > 0 && n < 65536) s.server_port = n;
        else if (k == "role") s.role = n == 1;
        else if (k == "addr" && !v.empty()) s.addr = v;
        else if (k == "port" && n > 0 && n < 65536) s.port = n;
        else if (k == "name" && !v.empty()) s.name = v.substr(0, 16);
        else if (k == "tunic") s.tunic = n & 3;
        else if (k == "radar") s.radar = n != 0;
        else if (k == "pvp_sea") s.pvp_sea = n != 0;
        else if (k == "strangers") s.strangers = n == 1;
        else if (k == "together") s.stay_together = n != 0;
        else if (k == "model_send") hostui::set("android.model_send", n ? "1" : "0");
        else if (k == "model_receive") hostui::set("android.model_receive", n ? "1" : "0");
        else if (k == "raid" && n >= 0 && n <= 4) hostui::set("android.raid", std::to_string(n));
    }
    csync::save_settings(s);
}
// which games and save files play online (the Online page's "Plays online"; online/coop_sync_android.cpp): this game's
// key ("p", "r<n>", "o<n>"), 0 off / 1 the whole game / 2 the save files in files (bit n-1), the file whose settings
// the page shows while none of them is played
COOP_FN(void, coopScope)(JNIEnv* env, jclass, jstring key, jint scope, jint files, jint edit) {
    csync::android_set_scope(jstr(env, key), scope, files, edit);
}
// "profile<TAB>save file played (0 none)<TAB>1 when the session should run"
COOP_FN(jstring, coopScopeNow)(JNIEnv* env, jclass) {
    const csync::ScopeNow n = csync::android_scope_now();
    return js(env, n.profile + "\t" + std::to_string(n.file) + "\t" + (n.on ? "1" : "0"));
}
COOP_FN(jstring, coopNotice)(JNIEnv* env, jclass) { return js(env, csync::kNotice); }
COOP_FN(void, coopRetry)(JNIEnv*, jclass) { csync::retry(); }

// the status line; "" when off
COOP_FN(jstring, coopStatus)(JNIEnv* env, jclass) { return js(env, csync::status()); }
// why online co-op can't run with this game ("" it can): the JPN release lays out the game's info block differently
// (fields from about +0x4800 on 4 bytes earlier), and upstream's co-op code reads it at the USA / EUR offsets
COOP_FN(jboolean, coopUnsupported)(JNIEnv*, jclass) { return release::id() == release::Id::JPN; }
COOP_FN(jboolean, coopOnline)(JNIEnv*, jclass) { return net::role() != net::Role::kOff; }
COOP_FN(jstring, coopFriendCode)(JNIEnv* env, jclass) { return js(env, net::friend_code()); }
COOP_FN(jboolean, coopViaServer)(JNIEnv*, jclass) { return net::via_server(); }
// "" when sent, else why not
COOP_FN(jstring, coopInvite)(JNIEnv* env, jclass, jstring code) {
    std::string why;
    return js(env, net::invite(jstr(env, code), why) ? "" : why);
}
// "party<TAB>from" each
COOP_FN(jobjectArray, coopInvites)(JNIEnv* env, jclass) {
    std::vector<std::string> out;
    for (const auto& [party, from] : net::invites()) out.push_back(std::to_string(party) + "\t" + clean(from));
    return jarr(env, out);
}
COOP_FN(void, coopAnswer)(JNIEnv*, jclass, jint party, jboolean yes) { net::answer((uint32_t)party, yes); }
COOP_FN(void, coopLeaveParty)(JNIEnv*, jclass) { net::leave_party(); }
// "the party max / size": {max, size}
COOP_FN(jintArray, coopPartySize)(JNIEnv* env, jclass) {
    const jint v[2] = {net::party_max(), net::party_size()};
    jintArray a = env->NewIntArray(2);
    env->SetIntArrayRegion(a, 0, 2, v);
    return a;
}
// the other players this game knows: "id<TAB>name<TAB>tunic<TAB>member<TAB>ping<TAB>blocked<TAB>muted" each
COOP_FN(jobjectArray, coopPlayers)(JNIEnv* env, jclass) {
    std::vector<std::string> out;
    if (net::role() == net::Role::kOff) return jarr(env, out);
    for (int id = 0; id < net::kMaxPlayers; id++) {
        if (id == net::my_id() || !net::connected(id)) continue;
        out.push_back(std::to_string(id) + "\t" + clean(net::name(id)) + "\t" + std::to_string(net::tunic(id)) + "\t" +
                      std::to_string(net::member(id)) + "\t" + std::to_string(net::ping_ms(id)) + "\t" +
                      std::to_string(mods::coop::block::blocked_id(id)) + "\t" + std::to_string(mods::coop::block::silent_id(id)));
    }
    return jarr(env, out);
}
COOP_FN(void, coopGoTo)(JNIEnv*, jclass, jint id) { csync::go_to(id); }
// kind 1 block, 2 mute; "" done, else why not
COOP_FN(jstring, coopBlock)(JNIEnv* env, jclass, jint id, jint kind) {
    std::string why;
    const bool ok = mods::coop::block::set(id, kind == 2 ? mods::coop::block::kMuted : mods::coop::block::kBlocked, why);
    return js(env, ok ? "" : why);
}
// the block list: "key hex<TAB>kind<TAB>name" each
COOP_FN(jobjectArray, coopBlocks)(JNIEnv* env, jclass) {
    std::vector<std::string> out;
    for (const auto& e : mods::coop::block::current().entries()) {
        char k[24];
        snprintf(k, sizeof k, "%016llx", (unsigned long long)e.key);
        out.push_back(std::string(k) + "\t" + std::to_string((int)e.kind) + "\t" + clean(e.name));
    }
    return jarr(env, out);
}
COOP_FN(void, coopUnblock)(JNIEnv* env, jclass, jstring key) {
    mods::coop::block::undo(strtoull(jstr(env, key).c_str(), nullptr, 16));
}
// the emotes: "id<TAB>name" each; playing one
COOP_FN(jobjectArray, coopEmotes)(JNIEnv* env, jclass) {
    std::vector<std::string> out;
    for (const auto& e : mods::coop::emotes::kEmotes) out.push_back(std::to_string(e.id) + "\t" + e.name);
    return jarr(env, out);
}
COOP_FN(void, coopEmote)(JNIEnv*, jclass, jint id) { mods::coop::emotes::mine((uint8_t)id); }
// the saved servers: "name<TAB>addr<TAB>port" each; forgetting one
COOP_FN(jobjectArray, coopServers)(JNIEnv* env, jclass) {
    std::vector<std::string> out;
    for (const auto& s : net::saved_servers()) out.push_back(clean(s.name) + "\t" + clean(s.addr) + "\t" + std::to_string(s.port));
    return jarr(env, out);
}
COOP_FN(void, coopForgetServer)(JNIEnv* env, jclass, jstring addr, jint port) { net::forget_server(jstr(env, addr), port); }
// a server's mod rules in words ("" until they came; the first call asks)
COOP_FN(jstring, coopServerRules)(JNIEnv* env, jclass, jstring addr, jint port) {
    const std::string a = jstr(env, addr);
    std::string r = net::server_rules(a, port);
    if (r.empty()) net::query_rules(a, port);
    return js(env, r);
}
// why this game was turned away for its gameplay mods ("" none), and whether "Use the party's mods" can help
// (fork) daily quests from the fork's server (online/android_quests.cpp): none without one; the first line
// "greeting\thours\tminutes to the next day", then "id\tname\twhat\tprogress\tgoal\tdone\trupees" each
namespace android_quests { std::vector<std::string> list(); void ask(); }
COOP_FN(jobjectArray, coopQuests)(JNIEnv* env, jclass) { return jarr(env, android_quests::list()); }
COOP_FN(void, coopQuestsAsk)(JNIEnv*, jclass) { android_quests::ask(); }
// (fork) a wave of monsters around this Link, for everyone in the session (online/android_raid.cpp)
namespace android_raid { void ask_wave(); }
COOP_FN(void, coopRaidWave)(JNIEnv*, jclass) { android_raid::ask_wave(); }
// (fork) the event "Outset under siege", started where this Link stands (online/android_siege.cpp)
namespace android_siege { void ask(); }
COOP_FN(void, coopSiege)(JNIEnv*, jclass) { android_siege::ask(); }
// (fork) the event "Raiders from the sea", Grandma's door where this Link stands (online/android_shipraid.cpp)
namespace android_shipraid { void ask(); }
COOP_FN(void, coopShipRaid)(JNIEnv*, jclass) { android_shipraid::ask(); }
COOP_FN(jstring, coopModProblems)(JNIEnv* env, jclass) {
    std::vector<mods::coop::modset::Problem> ps;
    for (const auto& i : net::mod_issues()) ps.push_back({i.code, i.id, i.wanted, i.have});
    std::string t = ps.empty() ? std::string() : mods::coop::modset::describe(ps);
    const std::string refused = net::refused_mods();
    if (t.empty() && !refused.empty()) {
        mods::coop::modset::Set party;
        if (mods::coop::modset::decode((const uint8_t*)refused.data(), refused.size(), party))
            t = mods::coop::modset::describe(mods::coop::modset::diff(party, mods::coop::modset::current()));
    }
    return js(env, t);
}
// player models going over the connection: "in|out<TAB>model<TAB>player<TAB>percent" each
COOP_FN(jobjectArray, coopModelTransfers)(JNIEnv* env, jclass) { return jarr(env, android_models::transfers()); }
// the party's mods after a refusal (upstream's draw_party_mods): "" none to take (no refusal by a party, or a server's
// rules), "use" this game can switch to them, "no<TAB>why" it can't
COOP_FN(jstring, coopPartyMods)(JNIEnv* env, jclass) {
    const std::string got = net::refused_mods();
    mods::coop::modset::Set party;
    if (got.empty() || !mods::coop::modset::decode((const uint8_t*)got.data(), got.size(), party)) return js(env, "");
    if (mods::coop::modset::diff(party, mods::coop::modset::current()).empty()) return js(env, "match");
    std::string why;
    return js(env, mods::coop::modset::can_use(party, why) ? std::string("use") : "no\t" + why);
}
// switches to the party's mods: {"" done | why not, then the ids of the features turned off (the app's options follow)}
COOP_FN(jobjectArray, coopUsePartyMods)(JNIEnv* env, jclass) {
    const std::string got = net::refused_mods();
    mods::coop::modset::Set party;
    std::vector<std::string> out{"the party's mods are not known (connect again)"};
    if (got.empty() || !mods::coop::modset::decode((const uint8_t*)got.data(), got.size(), party)) return jarr(env, out);
    std::vector<std::string> before;
    for (const auto& m : mods::coop::modset::current()) before.push_back(m.id);
    bool restart = false;
    std::string why;
    if (!mods::coop::modset::use(party, restart, why)) return jarr(env, {why});
    out = {""};
    std::vector<std::string> now;
    for (const auto& m : mods::coop::modset::current()) now.push_back(m.id);
    for (const auto& id : before)
        if (std::find(now.begin(), now.end(), id) == now.end()) out.push_back(id);
    return jarr(env, out);
}
// the party's switches (upstream's "Party switches"): {flags, 1 when this player leads}; null in no party on a server
COOP_FN(jintArray, coopPartyFlags)(JNIEnv* env, jclass) {
    uint16_t flags = 0;
    bool leading = false;
    if (!net::via_server() || !net::party_flags(flags, leading)) return nullptr;
    jint v[2] = {(jint)flags, leading ? 1 : 0};
    jintArray a = env->NewIntArray(2);
    env->SetIntArrayRegion(a, 0, 2, v);
    return a;
}
// sets them (the leader): "" done, else why not
COOP_FN(jstring, coopSetPartyFlags)(JNIEnv* env, jclass, jint flags) {
    std::string why;
    return js(env, net::set_party_flags((uint16_t)flags, why) ? std::string() : why);
}
// this game's gameplay mods (the fork's features that count online), "id<TAB>name" each
COOP_FN(jobjectArray, coopMyMods)(JNIEnv* env, jclass) {
    std::vector<std::string> out;
    for (const auto& m : mods::coop::modset::current()) out.push_back(m.id + "\t" + m.name);
    return jarr(env, out);
}

// what the co-op code told the player since the last call
COOP_FN(jobjectArray, coopNotices)(JNIEnv* env, jclass) { return jarr(env, online_compat::take_notices()); }

// the overlay's shapes for a w x h view (online_compat::kShapeFloats floats each); their texts by coopOverlayTexts
COOP_FN(jfloatArray, coopOverlay)(JNIEnv* env, jclass, jfloat w, jfloat h) {
    std::vector<float> shapes;
    std::vector<std::string> texts;
    if (net::role() != net::Role::kOff) overlay::ext::draw_layers(w, h, shapes, texts);
    {
        std::lock_guard<std::mutex> lk(g_overlay_mu);
        g_overlay_texts = std::move(texts);
    }
    jfloatArray a = env->NewFloatArray((jsize)shapes.size());
    if (!shapes.empty()) env->SetFloatArrayRegion(a, 0, (jsize)shapes.size(), shapes.data());
    return a;
}
COOP_FN(jobjectArray, coopOverlayTexts)(JNIEnv* env, jclass) {
    std::lock_guard<std::mutex> lk(g_overlay_mu);
    return jarr(env, g_overlay_texts);
}
