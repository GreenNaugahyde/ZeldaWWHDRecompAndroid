// Songs sung, hummed or whistled into the microphone (the app recognises the melody, SongListener.java):
// the game's conductor plays it as if Link had conducted it.
//   While Link holds the Wind Waker (daPy_lk_c::procTactWait), each beat the game asks its judge
//   (JAIZelInst::judge, USA 0202A3B4: the notes conducted so far against mMelodyPattern, USA data
//   1018DBB8, same as the GameCube's) which song they make; its answer (a melody number, -1 none) starts
//   the song: the match chime, Link plays it, its effect. Our hook lets the judge run (its sounds) and
//   answers the sung song once. The game's own checks stay: only songs Link has learned play
//   (dComIfGs_isTact), events that wait for one song take only that one.
//   Learned songs: dSv_player_collect_c mTact (bits by melody number), save + 0xB4 + 9.
//   Conducted by voice (the mic button held: the app gives the game the sticks, SongListener.conductsByVoice):
//   when the game's judge finds a learned song itself, the sung one sent on letting go is dropped (no second play).
//   Debug: WWHD_SONG_DUMP=1 (with WWHD_AUDIO_DUMP=<file.wav> recording the game's sound): while Link holds the Wind
//   Waker (the game mutes its music then), the game plays its six songs one after another, 9 s apart, by its own
//   JAIZelInst::melodyPlay (USA 0202A8F0: sound effect JA_SE_WTAKT_DANCE01 + melody); logged as "[songs] dump".
//   Replay (set_song_replay, Mods): HD's procTactPlay (USA 0243AC64) begins with a branch the GameCube's hasn't: in
//   free play (mProcVar4 +0x691E == 0, mProcVar7 +0x69C4 != 0, no partner or the partner's song) it plays its sound
//   0x871 and goes straight to the song's effect (f_02412948), so the replay (Link's song animation, the tune: the
//   play timer +0x6920 -> melodyPlay) never comes. While Link is in tact play (proc 155) and its animation runs
//   (mProcVar0 +0x6916 == 0), +0x691E is held at 1 at each step's start, so that branch waits: the GameCube's
//   replay runs. Once the animation is done (+0x6916 set) it goes back to 0 and HD's branch gives the effect as
//   before. (+0x691E is the GameCube's m34D8, "an event waits for this song": only read when the replay is done.)
//   Story scenes that wait for one song (song_state out[8], Link's tact wait +0x69C0): the app shows a play button
//   for it; debug WWHD_SONG_WANT=<melody> shows it in free play too.
//   Debug: WWHD_SONG_TRACE=1 logs each beat the judge takes (c_pos -> its answer) and Link's proc changes (154 tact
//   wait, 155 tact play, 156 tact play end: HD's procTactPlay (USA 0243AC64) goes on to the end at once for a learned
//   song, no replay of the tune as on the GameCube); WWHD_SONG_NO_HANDOVER=1 leaves the songs to the game's judge.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <algorithm>

#include "mods.h"
#include "runtime.h"
#include "../hud.h"
#include "../release.h"

extern "C" void f_0202A3B4_orig(Cpu* c);  // JAIZelInst::judge(this, now_beat, c_pos)

namespace mods {
namespace {
const release::Data kPlay{0x1046F0B0};
constexpr uint32_t kPlayerStatus1 = 0x5CDC;  // dComIfGp mPlayerStatus[0][1]
constexpr uint32_t kConducting = 0x1;        // daPyStts1_WIND_WAKER_CONDUCT_e
constexpr uint32_t kCollectTact = 0xB4 + 9, kInventory = 0x3C;
constexpr uint8_t kWindWaker = 0x22;
constexpr int kMelodies = 6;

std::atomic<int> g_song{-1};
std::atomic<uint64_t> g_song_step{0};
std::atomic<int> g_handed{0};
std::atomic<uint64_t> g_game_took{0};  // the step the game's judge itself found a learned song (conducted by voice)
constexpr uint32_t kMelodyPlay = 0x0202A8F0;  // JAIZelInst::melodyPlay(this (unused), melody)
// the count-in's sounds (the app counts one bar before the first sung note): the game's own, as its bar plays them
// (JAIZelInst::metronomePlay / judge): each beat the metronome's click (its note: the centre's of that time), the
// last one the time's upbeat. Played through the sound interface's seStart as melodyPlay calls it (USA 0202A958:
// interface *(101FFC78), id, 0, param, 0, 0, f1 = f2 = (10003A68), f3 = f4 = (10003AA8))
constexpr uint32_t kSeStart = 0x0201EBA0;
const release::Data kSoundInterface{0x101FFC78}, kSeF1{0x10003A68}, kSeF3{0x10003AA8};
constexpr uint32_t kSeMetronome = 0x0887, kSeAuftakt3 = 0x2868, kSeAuftakt4 = 0x2880, kSeAuftakt6 = 0x2881;
std::atomic<bool> g_replay{false};
uint32_t g_replay_link = 0;  // Link while +0x691E is held at 1 by us
constexpr uint32_t kCurProc = 0x65F0, kTactPlay = 155, kAnimDone = 0x6916, kEventSong = 0x691E, kFreePlay = 0x69C4;
// what Link's tact wait (proc 154) waits for: the GameCube's mProcVar6.m3570 (USA procTactWait 0243A82C on): a melody
// number when a story scene waits for that song (the judge's answer must be it, learned or not), -1 free play, other
// negatives / 6 / 7 the game's own uses
constexpr uint32_t kTactWait = 154, kWaitFor = 0x69C0;
std::atomic<int> g_count{0};  // a count-in beat to sound: time * 16 + (1: last), 0 none
// the beats the game's judge took while Link conducts (the app's timeline shows each one with the note taken):
// how many, the last one's beat and C-stick position (now_beat * 16 + c_pos), its time (steady clock, ms)
std::atomic<int> g_beats{0}, g_beat_pos{0};
std::atomic<int64_t> g_beat_ms{0};

int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void se_start(Cpu* c, uint32_t id, uint32_t param) {
    uint32_t iface = ld32(kSoundInterface);
    if (!iface) return;
    const Cpu saved = *c;
    double f1 = ldf32(kSeF1), f3 = ldf32(kSeF3);
    c->f[1].ps0 = c->f[1].ps1 = c->f[2].ps0 = c->f[2].ps1 = f1;
    c->f[3].ps0 = c->f[3].ps1 = c->f[4].ps0 = c->f[4].ps1 = f3;
    guest_call(c, release::Code{kSeStart}, {iface, id, 0, param, 0, 0});
    *c = saved;
}

bool conducting() { return player_save() && (ld32(kPlay + kPlayerStatus1) & kConducting); }
}  // namespace

void song_state(int32_t out[kSongStateLen]) {
    uint32_t s = player_save();
    int32_t compass[2];
    out[0] = conducting() && !hud_wind_compass(compass) ? 1 : 0;  // (the conduct bit stays on under the wind compass)
    out[1] = s ? ld8(s + kCollectTact) : -1;
    out[2] = 0;
    if (s)
        for (int i = 0; i < 21; i++)
            if (ld8(s + kInventory + i) == kWindWaker) out[2] = 1;
    out[3] = s && !out[0] && (event_mode_now() != 0 || hud_menu_open()) ? 1 : 0;
    out[4] = g_handed.load();
    out[5] = g_beats.load();
    out[6] = g_beat_pos.load();
    out[7] = (int32_t)std::min<int64_t>(now_ms() - g_beat_ms.load(), INT32_MAX);  // how long ago
    out[8] = -1;
    if (uint32_t link = out[0] ? link_actor() : 0)
        if (ld32(link + kCurProc) == kTactWait) {
            int32_t want = (int32_t)ld32(link + kWaitFor);
            if (want >= 0 && want < kMelodies) out[8] = want;
            static const char* fake = getenv("WWHD_SONG_WANT");  // debug: as if a story scene waited for that song (shown only)
            if (fake && out[8] < 0) out[8] = atoi(fake);
        }
}

void play_song(int melody) {
    if (melody < 0 || melody >= kMelodies) return;
    static const bool off = getenv("WWHD_SONG_NO_HANDOVER") != nullptr;  // debug: only the game's own judge
    if (off) {
        LOG("[songs] sung: melody %d (not handed over: WWHD_SONG_NO_HANDOVER)", melody);
        return;
    }
    if (g_game_took.load() && step() < g_game_took.load() + 90) {  // 3 s: the game is playing it already
        LOG("[songs] sung: melody %d, the game took it itself", melody);
        return;
    }
    g_song_step = step();
    g_song = melody;
    LOG("[songs] sung: melody %d", melody);
}
void set_song_replay(bool on) {
    g_replay = on;
    LOG("[mods] replay conducted songs (GameCube): %s", on ? "on" : "off");
}

// at each step's start (song_dump_service): hold HD's shortcut off while the replay runs
void song_replay_step() {
    uint32_t link = link_actor();
    if (!link) return;
    bool play = ld32(link + kCurProc) == kTactPlay;
    if (g_replay_link && (!play || link != g_replay_link)) {  // tact play left (another proc reuses the field)
        g_replay_link = 0;
        return;
    }
    if (!play) return;
    bool animDone = ld16(link + kAnimDone) != 0;
    if (g_replay_link) {
        if (animDone) {  // the replay is over: HD's branch gives the effect
            st16(link + kEventSong, 0);
            g_replay_link = 0;
        }
        return;
    }
    if (g_replay.load() && !animDone && ld16(link + kEventSong) == 0 && ld32(link + kFreePlay) != 0) {
        st16(link + kEventSong, 1);
        g_replay_link = link;
    }
}

void count_beat(int time, bool last) {
    if (time == 3 || time == 4 || time == 6) g_count = time * 16 + (last ? 1 : 0);
}

void song_dump_service(Cpu* c) {
    song_replay_step();
    if (int k = g_count.exchange(0)) {  // a count-in beat (the game thread: its sound system)
        if (conducting()) {
            int time = k / 16;
            uint32_t note = time == 3 ? 0x30 : time == 4 ? 0x3B : 0x3E;  // (mCPosToNote3/4/61[centre])
            se_start(c, kSeMetronome, note | (100u << 8));
            if (k & 1) se_start(c, time == 3 ? kSeAuftakt3 : time == 4 ? kSeAuftakt4 : kSeAuftakt6, 0);
        }
    }
    static const bool trace = getenv("WWHD_SONG_TRACE") != nullptr;  // debug: Link's proc (mCurProc) changes
    if (trace) {
        static uint32_t last_proc = 0;
        if (uint32_t link = link_actor()) {
            uint32_t proc = ld32(link + 0x65F0);
            if (proc != last_proc)
                LOG("[songs] step %llu: proc %u, play timer %u, melody %d", (unsigned long long)step(), proc,
                    ld16(link + 0x6920), (int32_t)ld32(link + 0x69C0));
            last_proc = proc;
        }
    }
    static const bool on = getenv("WWHD_SONG_DUMP") != nullptr;
    if (!on) return;
    static int next = 0;
    static uint64_t since = 0;
    if (next >= kMelodies) return;
    if (!conducting()) {
        since = 0;
        return;
    }
    uint64_t now = step();
    if (!since) since = now;
    if (now < since + 60) return;  // 2 s: the Wind Waker's own sounds have faded
    since = now + 270 - 60;        // the next one 9 s later
    LOG("[songs] dump: melody %d starts (step %llu)", next, (unsigned long long)now);
    const Cpu saved = *c;
    guest_call(c, release::Code{kMelodyPlay}, {0, (uint32_t)next});
    *c = saved;
    next++;
    if (next == kMelodies) LOG("[songs] dump: all songs played");
}
}  // namespace mods

using namespace mods;

extern "C" void hook_0202A3B4(Cpu* c) {
    int now_beat = (int32_t)c->r[4], c_pos = (int32_t)c->r[5];
    f_0202A3B4_orig(c);
    int heard = (int32_t)c->r[3];
    if (conducting() && c_pos >= 0 && c_pos <= 4) {  // a beat taken: for the app's timeline
        g_beat_ms = now_ms();
        g_beat_pos = now_beat * 16 + c_pos;
        g_beats++;
        static const bool trace = getenv("WWHD_SONG_TRACE") != nullptr;  // debug: each beat the judge takes
        if (trace) LOG("[songs] beat %d: c_pos %d -> %d", now_beat, c_pos, heard);
    }
    if (heard >= 0 && heard < kMelodies && conducting()) {
        uint32_t s = player_save();
        if (s && (ld8(s + kCollectTact) & (1 << heard))) {
            LOG("[songs] the game's judge found melody %d itself", heard);
            g_game_took = step();
            g_song = -1;
            return;
        }
    }
    int song = g_song.load();
    if (song < 0) return;
    if (step() > g_song_step.load() + 150) {  // 5 s: the Wind Waker never came out
        g_song = -1;
        return;
    }
    if (!conducting()) return;  // (the quest status screen's song practice uses the judge too)
    LOG("[songs] the judge answered %d, now melody %d", (int32_t)c->r[3], song);
    c->r[3] = (uint32_t)song;
    g_song = -1;
    g_handed++;
}
