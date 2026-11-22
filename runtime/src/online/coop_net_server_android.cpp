// Upstream's mods/coop_net_server.cpp (online co-op through a server), built through this file so the fork can read
// one thing without changing upstream's file (online/compat/README.md): how much this game has sent to the server, so
// a player model sent over (online/android_models.cpp) fills what the server's upload limit leaves beside the poses.
// (fork) the fork's server words (online/fork_proto.h): the client the file makes is the fork's own, which takes the
// fork server's messages out of each poll before upstream's code sees them (an app message from player 0xFFFF, which
// upstream's code would drop as no one's) and tells android_quests.cpp of every welcome; everything else as it was
#include <vector>
#include "proto/client.h"
#include "fork_proto.h"
namespace android_quests {
void welcomed();
void stopped();
void from_server(const uint8_t* d, size_t n);
}
namespace online::proto {
class AndroidClient : public Client {
public:
    ~AndroidClient() { if (g_live == this) g_live = nullptr, android_quests::stopped(); }
    const std::vector<ClientEvent>& poll(uint32_t now_ms) {
        const std::vector<ClientEvent>& in = Client::poll(now_ms);
        out_.clear();
        for (const ClientEvent& e : in) {
            if (e.kind == ClientEvent::kApp && e.id == fork_proto::kFromServer) {
                if (e.app_type == fork_proto::kForkApp && this == g_live) android_quests::from_server(e.body.data(), e.body.size());
                continue;
            }
            if (e.kind == ClientEvent::kWelcome) g_live = this, android_quests::welcomed();  // (a server list query is never welcomed)
            out_.push_back(e);
        }
        return out_;
    }
    // a fork word to the server (an upstream server drops it)
    void fork_send(const std::string& body) {
        AppHeader h;
        h.type = fork_proto::kForkApp, h.target = fork_proto::kToServer;
        send_app(h, (const uint8_t*)body.data(), body.size(), true);
    }
    static inline AndroidClient* g_live = nullptr;  // the session's (not a server list query's)

private:
    std::vector<ClientEvent> out_;
};
}  // namespace online::proto
#define Client AndroidClient
#include "../mods/coop_net_server.cpp"
#undef Client

namespace mods::coop::net::srv {
// bytes and packets sent to the server since the session started (as the server counts them: the datagrams'
// payloads); false without a session
bool android_sent(uint64_t& bytes, uint64_t& packets) {
    if (!g_client) return false;
    const auto& s = g_client->stats();
    bytes = s.bytes_up, packets = s.packets_up;
    return true;
}
// the upload allowance left as put_state keeps it (bytes; 16000 full): a model sent over takes only what the poses
// don't need
double android_room() { return g_room; }
}  // namespace mods::coop::net::srv

namespace mods::coop::net::srv {
// (fork) a fork word to the server: false without a session online (android_quests.cpp)
bool android_fork_send(const std::string& body) {
    if (!g_client || g_client != op::AndroidClient::g_live || g_client->state() != op::ClientState::kOnline) return false;
    g_client->fork_send(body);
    return true;
}
}  // namespace mods::coop::net::srv
