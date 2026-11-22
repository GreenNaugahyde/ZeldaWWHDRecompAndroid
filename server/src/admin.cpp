// SPDX-License-Identifier: MPL-2.0
// The admin socket (a local Unix socket, mode 0600; never on the network), bans by friend code and the report log.
#include <cctype>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <sstream>

#include "server.h"

#ifndef _WIN32
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace online::server {

using namespace online::proto;

namespace {
int64_t unix_now() { return int64_t(std::time(nullptr)); }

// "30m", "12h", "7d", "perm"; seconds (0: permanent), -1 if unreadable
int64_t parse_duration(const std::string& s) {
    if (s == "perm" || s == "permanent" || s == "forever") return 0;
    if (s.empty()) return -1;
    char unit = s.back();
    long long n = std::atoll(s.substr(0, s.size() - (std::isdigit(uint8_t(unit)) ? 0 : 1)).c_str());
    if (n <= 0) return -1;
    switch (unit) {
        case 's': return n;
        case 'm': return n * 60;
        case 'h': return n * 3600;
        case 'd': return n * 86400;
        default: return std::isdigit(uint8_t(unit)) ? n : -1;
    }
}

// (fork) a JSON string; player names are the players' own text: every control character escaped, the rest as is
// (the status page decodes invalid UTF-8 with replacement characters and shows names as text, never as HTML)
std::string json_str(const std::string& v) {
    std::string o = "\"";
    for (unsigned char c : v) {
        if (c == '"' || c == '\\') {
            o += '\\';
            o += char(c);
        } else if (c < 0x20 || c == 0x7F) {
            char b[8];
            std::snprintf(b, sizeof b, "\\u%04x", c);
            o += b;
        } else {
            o += char(c);
        }
    }
    return o + "\"";
}
}  // namespace

uint64_t Server::ip_hash(const std::string& ip) const {
    uint8_t h[32];
    hmac_sha256(ip_salt_, 32, ip.data(), ip.size(), h);
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= uint64_t(h[i]) << (8 * i);
    return v | 1;  // never 0 (0 means "no IP kept")
}

bool Server::banned(uint64_t kh, const std::string& ip, std::string& reason) {
    int64_t now = unix_now();
    uint64_t c = code40_of(kh);
    uint64_t ih = ip.empty() ? 0 : ip_hash(ip);
    for (auto& b : bans_) {
        if (b.until && b.until < now) continue;
        if (b.code40 == c || (b.ip_hash && b.ip_hash == ih)) {
            reason = b.reason;
            return true;
        }
    }
    return false;
}

void Server::load_bans() {
    bans_.clear();
    std::ifstream in(cfg_.ban_file);
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream ls(line);
        Ban b;
        std::string code, iph;
        ls >> code >> b.until >> iph;
        std::getline(ls, b.reason);
        if (!b.reason.empty() && b.reason[0] == ' ') b.reason.erase(0, 1);
        if (!parse_friend_code(code, b.code40)) continue;
        b.ip_hash = std::strtoull(iph.c_str(), nullptr, 16);
        bans_.push_back(b);
    }
}

void Server::save_bans() {
    int64_t now = unix_now();
    std::string tmp = cfg_.ban_file + ".new";
    std::FILE* f = std::fopen(tmp.c_str(), "w");
    if (!f) return;
    std::fprintf(f, "# friend-code expiry(unix, 0 = permanent) ip-hash(0 = none) reason\n");
    for (auto& b : bans_) {
        if (b.until && b.until < now) continue;  // expired bans are dropped
        std::fprintf(f, "%s %lld %016llx %s\n", friend_code(b.code40).c_str(), (long long)b.until,
                     (unsigned long long)b.ip_hash, b.reason.c_str());
    }
    std::fclose(f);
    std::rename(tmp.c_str(), cfg_.ban_file.c_str());
}

void Server::append_report(uint64_t from, uint64_t about, uint8_t reason) {
    if (cfg_.report_file.empty()) return;
    std::FILE* f = std::fopen(cfg_.report_file.c_str(), "a");
    if (!f) return;
    // key hashes as friend codes, the reason and the time; no packet content
    std::fprintf(f, "%lld %s reports %s reason %u\n", (long long)unix_now(), friend_code(from).c_str(),
                 friend_code(about).c_str(), unsigned(reason));
    std::fclose(f);
}

std::string Server::admin(const std::string& line) {
    std::istringstream in(line);
    std::string cmd;
    in >> cmd;
    auto rest = [&] {
        std::string r;
        std::getline(in, r);
        if (!r.empty() && r[0] == ' ') r.erase(0, 1);
        return r;
    };
    auto find_code = [&](const std::string& code, uint64_t& c40) -> Session* {
        if (!parse_friend_code(code, c40)) return nullptr;
        for (auto& [cid, s] : sessions_)
            if (!s->dead && code40_of(s->key_hash) == c40) return s.get();
        return nullptr;
    };
    if (cmd == "status") return status_text();
    if (cmd == "status-json") return status_json();
    if (cmd == "regions" || cmd == "split" || cmd == "move") return scale_admin(cmd, in);
    if (cmd == "players") {
        std::ostringstream o;
        for (auto& [cid, s] : sessions_) {
            if (s->dead) continue;
            o << s->id << " " << friend_code(s->key_hash) << " " << (s->online ? "online" : "lost") << " stage "
              << (s->where.stage_name().empty() ? "-" : s->where.stage_name()) << " room " << int(s->where.room)
              << " copy " << s->copy << " party " << s->party << " ping " << s->chan.rtt_ms()
              << (s->device == kPhone ? " phone" : s->device == kBot ? " bot" : "");
            if (cfg_.log_addresses) o << " " << s->addr.text();
            o << "\n";
        }
        return o.str();
    }
    if (cmd == "kick") {
        std::string code;
        in >> code;
        std::string why = rest();
        uint64_t c40 = 0;
        Session* s = find_code(code, c40);
        if (linked() && me_ == 0 && c40) {
            Bytes lb;
            Writer w(lb);
            w.u64(c40);
            w.str8(why.empty() ? "kicked by the operator" : why);
            for (size_t i = 1; i < peers_.size(); ++i) link_send(int(i), kLKick, lb);
        }
        if (!s || !s->online) return linked() ? "kick sent to the workers" : "not online: " + code;
        kick(*s, kNoticeKick, why.empty() ? "kicked by the operator" : why);
        return "kicked " + friend_code(c40);
    }
    if (cmd == "ban") {
        std::string code, dur;
        in >> code >> dur;
        std::string why = rest();
        Ban b;
        if (!parse_friend_code(code, b.code40)) return "not a friend code: " + code;
        int64_t secs = parse_duration(dur);
        if (secs < 0) return "duration: 30m, 12h, 7d or perm";
        b.until = secs ? unix_now() + secs : 0;
        b.reason = why;
        uint64_t c40 = 0;
        Session* s = find_code(code, c40);
        if (s && cfg_.keep_ip_hash) b.ip_hash = ip_hash(s->ip);
        bans_.push_back(b);
        save_bans();
        if (linked() && me_ == 0) {
            Bytes lb;
            Writer w(lb);
            w.u64(b.code40);
            w.u64(uint64_t(b.until));
            w.str8(b.reason);
            for (size_t i = 1; i < peers_.size(); ++i) link_send(int(i), kLBan, lb);
        }
        if (s && s->online) kick(*s, kNoticeBan, why.empty() ? "banned by the operator" : "banned: " + why);
        return "banned " + friend_code(b.code40) + (secs ? " for " + dur : " permanently");
    }
    if (cmd == "unban") {
        std::string code;
        in >> code;
        uint64_t c40 = 0;
        if (!parse_friend_code(code, c40)) return "not a friend code: " + code;
        size_t before = bans_.size();
        bans_.erase(std::remove_if(bans_.begin(), bans_.end(), [&](const Ban& b) { return b.code40 == c40; }), bans_.end());
        save_bans();
        return before == bans_.size() ? "no ban for " + code : "unbanned " + friend_code(c40);
    }
    if (cmd == "bans") {
        std::ostringstream o;
        for (auto& b : bans_)
            o << friend_code(b.code40) << " " << (b.until ? std::to_string(b.until) : std::string("permanent")) << " "
              << b.reason << "\n";
        return o.str().empty() ? "no bans\n" : o.str();
    }
    if (cmd == "notice") {
        std::string text = rest();
        for (auto& [cid, s] : sessions_)
            if (s->online && !s->dead) notice(*s, kNoticeInfo, 0, text);
        return "sent";
    }
    if (cmd == "restart") {
        int secs = 30;
        in >> secs;
        if (secs < 0 || secs > 3600) secs = 30;
        for (auto& [cid, s] : sessions_)
            if (s->online && !s->dead) notice(*s, kNoticeRestart, uint16_t(secs), "Server restarting");
        restart_at_ = monotonic_ms() + uint32_t(secs) * 1000;
        if (!restart_at_) restart_at_ = 1;
        return "restarting in " + std::to_string(secs) + " s";
    }
    if (cmd == "parties") {
        std::ostringstream o;
        for (auto& [id, p] : parties_) {
            o << id << " leader " << friend_code(p.leader) << " members " << p.members.size() << " team fields "
              << p.team.fields.size() << " v" << p.team.version << (p.empty_since ? " (nobody online)" : "") << "\n";
            for (auto& [place, kh] : p.runner)
                o << "  runner " << std::string(place.stage.data(), strnlen(place.stage.data(), 8)) << "/"
                  << int(place.room) << " " << friend_code(kh) << "\n";
        }
        return o.str().empty() ? "no parties\n" : o.str();
    }
    if (cmd == "stop") {
        stop_ = true;
        return "stopping";
    }
    return "commands: status, players, kick <code> [reason], ban <code> <30m|12h|7d|perm> [reason], unban <code>, "
           "status-json, bans, notice <text>, restart [seconds], parties, regions, split <region> [worker], move <region> <worker>, stop";
}

// (fork) the status page's snapshot: the server's counts, every player's place and position, the parties. Friend codes
// and names only: never IP addresses (even with log_addresses), never keys or tokens. Read through the admin socket
// (mode 0600) by server/status/wwhd_status.py, which shows it behind a login.
std::string Server::status_json() {
    std::ostringstream o;
    o.setf(std::ios::fixed);
    o.precision(0);
    uint32_t now = monotonic_ms();
    size_t active = 0;
    for (auto& [id, p] : parties_) active += p.empty_since == 0 ? 1 : 0;
    o << "{\"name\":" << json_str(cfg_.name) << ",\"time\":" << int64_t(std::time(nullptr))
      << ",\"started\":" << started_unix_ << ",\"players\":" << players() << ",\"max_players\":" << cfg_.max_players
      << ",\"parties_active\":" << active << ",\"parties_kept\":" << (parties_.size() - active)
      << ",\"bans\":" << bans_.size() << ",\"fingerprint\":" << json_str(fingerprint())
      << ",\"crypto\":" << json_str(crypto::backend_name()) << ",\"scale\":" << json_str(cfg_.scale_mode)
      << ",\"worker\":" << me_ << ",\"bytes_out\":" << uint64_t(metrics_.bytes_out)
      << ",\"bytes_in\":" << metrics_.bytes_in << ",\"handshakes\":" << metrics_.handshakes
      << ",\"refused\":" << metrics_.refused << ",\"kicked\":" << metrics_.kicked
      << ",\"dropped_rate\":" << metrics_.dropped_rate << ",\"dropped_bad\":" << metrics_.dropped_bad
      << ",\"implausible\":" << metrics_.implausible << ",\"pvp_hits\":" << metrics_.pvp_hits
      << ",\"pvp_kos\":" << metrics_.pvp_kos << ",\"tcp\":" << tcp_.size() << ",\"list\":[";
    bool first = true;
    for (auto& [cid, s] : sessions_) {
        if (s->dead) continue;
        if (!first) o << ",";
        first = false;
        const char* dev = s->device == kPhone ? "phone" : s->device == kBot ? "bot" : "desktop";
        o << "{\"id\":" << s->id << ",\"code\":" << json_str(friend_code(s->key_hash))
          << ",\"name\":" << json_str(s->name) << ",\"online\":" << (s->online ? "true" : "false")
          << ",\"device\":\"" << dev << "\",\"tcp\":" << (s->addr.tcp_id ? "true" : "false")
          << ",\"stage\":" << json_str(s->where.stage_name()) << ",\"room\":" << int(s->where.room)
          << ",\"layer\":" << int(s->where.layer) << ",\"has_pose\":" << (s->has_pose ? "true" : "false")
          << ",\"x\":" << s->head.x << ",\"y\":" << s->head.y << ",\"z\":" << s->head.z
          << ",\"yaw\":" << s->head.yaw << ",\"flags\":" << int(s->head.flags) << ",\"square\":" << s->square
          << ",\"copy\":" << s->copy << ",\"party\":" << s->party << ",\"ping\":" << s->chan.rtt_ms()
          << ",\"pvp\":" << (s->pvp_on ? "true" : "false") << ",\"safe\":" << (s->in_zone ? "true" : "false")
          << ",\"online_s\":" << ((now - s->joined_at) / 1000) << ",\"mods\":" << s->mods.size() << "}";
    }
    o << "],\"party_list\":[";
    first = true;
    for (auto& [id, p] : parties_) {
        if (!first) o << ",";
        first = false;
        o << "{\"id\":" << id << ",\"leader\":" << json_str(friend_code(p.leader)) << ",\"members\":" << p.members.size()
          << ",\"online\":" << (p.empty_since == 0 ? "true" : "false") << ",\"team_fields\":" << p.team.fields.size()
          << ",\"names\":[";
        bool f2 = true;
        for (auto& [kh, nm] : p.names) {
            if (!f2) o << ",";
            f2 = false;
            o << json_str(nm);
        }
        o << "]}";
    }
    o << "]}";
    return o.str();
}

#ifndef _WIN32
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0  // macOS: SIGPIPE is ignored by main()
#endif
namespace {
// a listening Unix socket at path with this mode; -1 + why on errors
int open_unix(const std::string& path, mode_t mode, const char* what, std::string& why) {
    sockaddr_un a{};
    if (path.size() >= sizeof a.sun_path) {
        why = std::string(what) + " socket path too long";
        return -1;
    }
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        why = std::string(what) + " socket: socket() failed";
        return -1;
    }
    a.sun_family = AF_UNIX;
    std::strncpy(a.sun_path, path.c_str(), sizeof a.sun_path - 1);
    struct stat st{};
    if (::lstat(path.c_str(), &st) == 0) {
        if (!S_ISSOCK(st.st_mode)) {
            ::close(fd);
            why = path + " exists and is not a socket";
            return -1;
        }
        ::unlink(path.c_str());  // the stale socket of an earlier run
    }
    mode_t old = umask(0177);
    int rc = bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof a);
    umask(old);
    if (rc != 0 || listen(fd, 8) != 0) {
        ::close(fd);
        why = std::string("cannot open the ") + what + " socket " + path;
        return -1;
    }
    chmod(path.c_str(), mode);
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    return fd;
}
}  // namespace

bool Server::open_admin(std::string& why) {
    if (!cfg_.admin_socket.empty()) {
        admin_fd_ = open_unix(cfg_.admin_socket, 0600, "admin", why);
        if (admin_fd_ < 0) return false;
    }
    if (!cfg_.status_socket.empty()) {
        status_fd_ = open_unix(cfg_.status_socket, 0660, "status", why);
        if (status_fd_ < 0) return false;
    }
    return true;
}

void Server::admin_poll() {
    for (int which = 0; which < 2; ++which) {
        int lfd = which == 0 ? admin_fd_ : status_fd_;
        if (lfd < 0) continue;
        for (int k = 0; k < 16; ++k) {
            int c = accept(lfd, nullptr, nullptr);
            if (c < 0) break;
            // one short command per connection; a client gets 200 ms to send it and to take the answer
            fcntl(c, F_SETFL, fcntl(c, F_GETFL, 0) & ~O_NONBLOCK);  // accept() may hand on the listener's O_NONBLOCK
            timeval tv{0, 200000};
            setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
            setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
            char buf[512];
            ssize_t n = recv(c, buf, sizeof buf - 1, 0);
            if (n > 0) {
                buf[n] = 0;
                std::string line(buf);
                while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
                std::string out;
                if (which == 0) out = admin(line);
                else if (line == "status-json") out = status_json();  // the status socket: reading only
                else if (line == "status") out = status_text();
                else out = "the status socket answers only: status, status-json";
                if (out.empty() || out.back() != '\n') out += "\n";
                size_t at = 0;
                while (at < out.size()) {
                    ssize_t sent = send(c, out.data() + at, out.size() - at, MSG_NOSIGNAL);
                    if (sent <= 0) break;
                    at += size_t(sent);
                }
            }
            ::close(c);
        }
    }
}
#else
bool Server::open_admin(std::string&) { return true; }  // Windows: admin through the console only (TODO)
void Server::admin_poll() {}
#endif

}  // namespace online::server
