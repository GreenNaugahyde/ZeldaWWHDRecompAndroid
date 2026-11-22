// (fork) The fork's own words between its games and its server (server/src/fork_quests.cpp,
// runtime/src/online/android_quests.cpp), downward compatible both ways:
//   - game -> server: an app message (proto kMsgApp) with target kToServer (5) and app type kForkApp. An upstream
//     server routes app messages to party targets 0..4 only and drops this one (Server::on_app: no case matches).
//   - server -> game: an app message (proto kMsgAppFrom) from player id kFromServer (0xFFFF), app type kForkApp. The
//     fork's server sends these only to a game that said kHello first, so an upstream game never gets one; the fork's
//     game takes them out before upstream's code (coop_net_server_android.cpp), and sends its words only once the
//     server answered kHello (on an upstream server it never does).
// Every body starts with an op byte. Numbers little-endian, texts u8 length + UTF-8.
#pragma once
#include <cstdint>

namespace fork_proto {
constexpr uint8_t kToServer = 5;          // AppHeader::target
constexpr uint8_t kForkApp = 0xF0;        // AppHeader::type
constexpr uint16_t kFromServer = 0xFFFF;  // the sender id of the server's words
constexpr uint16_t kVersion = 1;

enum Op : uint8_t {
    // game -> server
    kHello = 1,    // u16 version, u32 caps
    kDefeat = 2,   // stage[8], s8 room, u16 proc, u8 flags (bit 0: a monster this game runs for its place: the party
                   // members in the place are credited too; else only the sender)
    kAsk = 3,      // the quest list again
    kEvent = 4,    // text event ("siege"), u8 result (1 won): the party members in the place are credited too
    // server -> game
    kWelcome = 1,  // u16 version, u32 caps, text server's greeting
    kQuests = 2,   // u32 day, u32 seconds to the next day, u8 n, n x Quest
    kProgress = 3, // u16 quest id, u16 progress, u8 done
    kReward = 4,   // u16 quest id, u32 rupees, text what for
    kWorld = 5,    // a server-wide quest: u16 id, u16 count, u8 done, u16 mine (this player's part), u32 seconds left
};
// Quests with id >= 100 are events (stage "event:<name>"): always among the day's quests, counted by kEvent only.
// Quest: u16 id, u8 kind, u16 goal, u16 progress, u8 done, u32 rupees, text name, text what. After the list (games that
// don't know it stop before): u8 n, n x {u16 id, u16 mine, u32 seconds left} for the server-wide quests.
// A server-wide quest (kWorldKind): everyone on the server adds to one count (progress = the count); reaching the goal
// pays everyone who added to it; it starts over each day or week (pool field 8: world-day / world-week).
enum Kind : uint8_t { kDefeatKind = 1, kWorldKind = 2, kEventKind = 3 };
constexpr uint32_t kCapQuests = 1, kCapWorld = 2;
}  // namespace fork_proto
