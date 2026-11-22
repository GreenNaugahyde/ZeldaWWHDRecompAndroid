// (fork) Daily quests on the fork's server (runtime/src/online/fork_proto.h; notes/online-modes.md, "Server quests").
// The server picks today's quests from a pool (quests.txt beside the config, else a built-in pool), keeps each
// player's progress by their key (quests-state.txt, over restarts), counts the defeats the games report (a monster a
// game runs for its place counts for every party member in that place: helping counts), and hands out the reward when
// a quest is done. Only games that said hello (the fork's) hear of it; upstream games and servers are untouched.
#pragma once
#include <cstddef>
#include <cstdint>

namespace online::server {
class Server;
struct Session;
namespace fork_quests {
// a fork message (target fork_proto::kToServer) from a session
void on_message(Server& srv, Session& s, const uint8_t* body, size_t n, uint32_t now);
// each tick: a new day's quests to everyone who said hello, the state saved now and then
void tick(Server& srv, uint32_t now);
}  // namespace fork_quests
}  // namespace online::server
