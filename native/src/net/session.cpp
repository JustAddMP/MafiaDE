#include "net/session.h"

#include <algorithm>

#include "net/net_internal.h"

namespace mc::net {
using namespace detail;

namespace {

constexpr uint32_t kHelloMagic = 0x504F434D;  // "MCOP"
constexpr size_t kMaxNameLen = 32;
constexpr size_t kMaxChatLen = 256;
constexpr size_t kMaxReasonLen = 256;
constexpr double kRejectLinger = 1.0;
constexpr double kPingTableInterval = 2.0;

enum Msg : uint8_t {
    kHello = 0x01,
    kWelcome = 0x02,
    kReject = 0x03,
    kPeerJoined = 0x04,
    kPeerLeft = 0x05,
    kPing = 0x06,
    kPong = 0x07,
    kChat = 0x08,
    kPingTable = 0x09,
    kApp = 0x10,
};

enum RejectCode : uint8_t {
    kRejectVersion = 1,
    kRejectFull = 2,
    kRejectBadHello = 3,
};

constexpr uint8_t kAppFlagReliable = 0x01;

std::string CleanName(std::string s) {
    for (char& c : s)
        if (static_cast<unsigned char>(c) < 0x20) c = ' ';
    while (!s.empty() && s.back() == ' ') s.pop_back();
    while (!s.empty() && s.front() == ' ') s.erase(s.begin());
    if (s.size() > kMaxNameLen) s.resize(kMaxNameLen);
    return s.empty() ? std::string("Player") : s;
}

uint32_t NowMs() { return static_cast<uint32_t>(Now() * 1000.0); }

}  // namespace

Session::Session(Transport& transport, SessionConfig cfg) : transport_(transport), cfg_(std::move(cfg)) {
    cfg_.maxPlayers = std::clamp(cfg_.maxPlayers, 2, kMaxPlayers);
    cfg_.playerName = CleanName(cfg_.playerName);
}

Session::~Session() { Leave(); }

// ---------------------------------------------------------------------------------------------------------------
// lifecycle

void Session::Reset() {
    players_.clear();
    pending_.clear();
    localId_ = kNoPlayer;
    hostPeer_ = kInvalidPeer;
    welcomed_ = false;
    helloSentAt_ = 0;
}

bool Session::Host(uint16_t port) {
    if (state_ == SessionState::Hosting || state_ == SessionState::Connected || state_ == SessionState::Connecting)
        Leave();
    Reset();
    lastError_.clear();
    if (!transport_.Host(port)) {
        lastError_ = "could not start hosting";
        state_ = SessionState::Disconnected;
        return false;
    }
    state_ = SessionState::Hosting;
    localId_ = kHostPlayer;
    players_.push_back({kHostPlayer, cfg_.playerName, kInvalidPeer, 0});
    Log(kInfo, "session: hosting (%s), protocol %u build %u", transport_.Name(), cfg_.protocolVersion, cfg_.modBuild);
    return true;
}

bool Session::Join(std::string_view address) {
    if (state_ != SessionState::Idle && state_ != SessionState::Disconnected) Leave();
    Reset();
    lastError_.clear();
    hostPeer_ = transport_.Connect(address);
    if (!hostPeer_) {
        lastError_ = "invalid address";
        state_ = SessionState::Disconnected;
        return false;
    }
    state_ = SessionState::Connecting;
    Log(kInfo, "session: joining %s over %s", std::string(address).c_str(), transport_.Name());
    return true;
}

void Session::Leave() {
    if (state_ == SessionState::Idle) return;
    if (state_ == SessionState::Hosting) {
        for (auto& p : players_)
            if (p.id != kHostPlayer) transport_.Disconnect(p.peer);
        for (auto& kv : pending_) transport_.Disconnect(kv.first);
    } else if (hostPeer_) {
        transport_.Disconnect(hostPeer_);
    }
    transport_.Close();
    Reset();
    state_ = SessionState::Idle;
}

void Session::Fail(const std::string& reason) {
    Log(kWarn, "session: %s", reason.c_str());
    lastError_ = reason;
    if (hostPeer_) transport_.Disconnect(hostPeer_);
    transport_.Close();
    Reset();
    state_ = SessionState::Disconnected;
    if (onDisconnected) onDisconnected(reason);
}

// ---------------------------------------------------------------------------------------------------------------
// pump

void Session::Poll() {
    if (state_ == SessionState::Idle || state_ == SessionState::Disconnected) return;
    transport_.Poll([this](const Event& e) { OnEvent(e); });
    if (state_ == SessionState::Idle || state_ == SessionState::Disconnected) return;
    double now = Now();

    if (state_ == SessionState::Hosting) {
        std::vector<PeerId> drop;
        for (auto& [peer, pend] : pending_) {
            if (pend.kickAt > 0 ? now >= pend.kickAt : now - pend.since > cfg_.helloTimeoutS) drop.push_back(peer);
        }
        for (PeerId p : drop) {
            pending_.erase(p);
            transport_.Disconnect(p);
        }
    }
    // The transport reports its own connect timeout; this only covers a host that connects but never answers.
    if (state_ == SessionState::Connecting && helloSentAt_ > 0 && now - helloSentAt_ > cfg_.welcomeTimeoutS) {
        Fail("no answer from host");
        return;
    }

    if (now - lastPing_ >= cfg_.pingIntervalS) {
        lastPing_ = now;
        ByteWriter w(8);
        w.U8(kPing);
        w.U32(NowMs());
        if (state_ == SessionState::Hosting) {
            for (auto& p : players_)
                if (p.id != kHostPlayer) SendRaw(p.peer, w, Reliability::Unreliable);
        } else if (state_ == SessionState::Connected) {
            SendRaw(hostPeer_, w, Reliability::Unreliable);
        }
    }
    if (state_ == SessionState::Hosting && now - lastPingTable_ >= kPingTableInterval) {
        lastPingTable_ = now;
        // Refresh from transport stats when available (smoother than our own ping samples).
        for (auto& p : players_) {
            PeerStats st;
            if (p.id != kHostPlayer && transport_.GetStats(p.peer, st) && st.rttMs > 0) p.rttMs = st.rttMs;
        }
        ByteWriter w(32);
        w.U8(kPingTable);
        w.U8(static_cast<uint8_t>(players_.size()));
        for (auto& p : players_) {
            w.U8(p.id);
            w.U16(static_cast<uint16_t>(std::min(p.rttMs, 65535.0f)));
        }
        BroadcastRaw(w, Reliability::Unreliable, kNoPlayer);
    }
}

void Session::OnEvent(const Event& e) {
    if (state_ == SessionState::Hosting) {
        switch (e.type) {
            case EventType::Connected:
                pending_[e.peer] = Pending{Now(), 0};
                break;
            case EventType::Disconnected:
                if (pending_.erase(e.peer)) break;
                HostRemovePlayer(e.peer, ToString(e.reason));
                break;
            case EventType::Message: {
                ByteReader r(e.data);
                HostOnMessage(e.peer, r, e.channel);
                break;
            }
        }
        return;
    }
    if (e.peer != hostPeer_) return;
    switch (e.type) {
        case EventType::Connected: {
            ByteWriter w(64);
            w.U8(kHello);
            w.U32(kHelloMagic);
            w.U32(cfg_.protocolVersion);
            w.U32(cfg_.modBuild);
            w.Str(cfg_.playerName);
            SendRaw(hostPeer_, w, Reliability::Reliable);
            helloSentAt_ = Now();
            break;
        }
        case EventType::Disconnected: {
            std::string why;
            if (!welcomed_)
                why = std::string("could not connect to host: ") + ToString(e.reason);
            else if (e.reason == DisconnectReason::Remote)
                why = "host closed the session";
            else
                why = std::string("connection to host lost: ") + ToString(e.reason);
            hostPeer_ = kInvalidPeer;
            Fail(why);
            break;
        }
        case EventType::Message: {
            ByteReader r(e.data);
            GuestOnMessage(r);
            break;
        }
    }
}

// ---------------------------------------------------------------------------------------------------------------
// host side

void Session::Reject(PeerId peer, uint8_t code, const std::string& reason) {
    Log(kInfo, "session: rejecting peer %llu: %s", static_cast<unsigned long long>(peer), reason.c_str());
    ByteWriter w(64);
    w.U8(kReject);
    w.U8(code);
    w.Str(reason);
    SendRaw(peer, w, Reliability::Reliable);
    pending_[peer].kickAt = Now() + kRejectLinger;
}

void Session::HostOnHello(PeerId peer, ByteReader& r) {
    uint32_t magic = r.U32();
    uint32_t proto = r.U32();
    uint32_t build = r.U32();
    std::string name = r.Str(256);
    if (!r.Ok() || magic != kHelloMagic) {
        Reject(peer, kRejectBadHello, "invalid handshake");
        return;
    }
    if (proto != cfg_.protocolVersion || build != cfg_.modBuild) {
        Reject(peer, kRejectVersion,
               "version mismatch: host has protocol " + std::to_string(cfg_.protocolVersion) + " build " +
                   std::to_string(cfg_.modBuild) + ", you have protocol " + std::to_string(proto) + " build " +
                   std::to_string(build) + ". Both players need the same MafiaCoop version.");
        return;
    }
    if (static_cast<int>(players_.size()) >= cfg_.maxPlayers) {
        Reject(peer, kRejectFull, "the session is full (" + std::to_string(cfg_.maxPlayers) + " players)");
        return;
    }
    PlayerId id = kNoPlayer;
    for (PlayerId c = 1; c < cfg_.maxPlayers; ++c)
        if (!FindPlayer(c)) {
            id = c;
            break;
        }
    if (id == kNoPlayer) {
        Reject(peer, kRejectFull, "the session is full");
        return;
    }
    pending_.erase(peer);
    PlayerInfo info{id, CleanName(name), peer, 0};
    players_.push_back(info);
    std::sort(players_.begin(), players_.end(), [](const PlayerInfo& a, const PlayerInfo& b) { return a.id < b.id; });

    ByteWriter w(128);
    w.U8(kWelcome);
    w.U8(id);
    w.U8(static_cast<uint8_t>(cfg_.maxPlayers));
    w.U8(static_cast<uint8_t>(players_.size()));
    for (auto& p : players_) {
        w.U8(p.id);
        w.Str(p.name);
    }
    SendRaw(peer, w, Reliability::Reliable);

    ByteWriter j(64);
    j.U8(kPeerJoined);
    j.U8(id);
    j.Str(info.name);
    BroadcastRaw(j, Reliability::Reliable, id);
    Log(kInfo, "session: player %u '%s' joined (peer %llu)", id, info.name.c_str(), static_cast<unsigned long long>(peer));
    if (onPlayerJoined) onPlayerJoined(info);
}

void Session::HostRemovePlayer(PeerId peer, const std::string& reason) {
    auto it = std::find_if(players_.begin(), players_.end(),
                           [&](const PlayerInfo& p) { return p.peer == peer && p.id != kHostPlayer; });
    if (it == players_.end()) return;
    PlayerInfo info = *it;
    players_.erase(it);
    ByteWriter w(64);
    w.U8(kPeerLeft);
    w.U8(info.id);
    w.Str(reason);
    BroadcastRaw(w, Reliability::Reliable, kNoPlayer);
    Log(kInfo, "session: player %u '%s' left: %s", info.id, info.name.c_str(), reason.c_str());
    if (onPlayerLeft) onPlayerLeft(info, reason);
}

void Session::HostOnMessage(PeerId peer, ByteReader& r, Reliability ch) {
    uint8_t type = r.U8();
    if (!r.Ok()) return;
    auto pend = pending_.find(peer);
    if (pend != pending_.end()) {
        if (pend->second.kickAt > 0) return;  // already rejected
        if (type == kHello)
            HostOnHello(peer, r);
        else
            Reject(peer, kRejectBadHello, "invalid handshake");
        return;
    }
    PlayerInfo* from = FindByPeer(peer);
    if (!from) return;
    switch (type) {
        case kPing: {
            uint32_t t = r.U32();
            if (!r.Ok()) return;
            ByteWriter w(8);
            w.U8(kPong);
            w.U32(t);
            SendRaw(peer, w, Reliability::Unreliable);
            break;
        }
        case kPong: {
            uint32_t t = r.U32();
            if (!r.Ok()) return;
            float rtt = static_cast<float>(NowMs() - t);
            from->rttMs = from->rttMs <= 0 ? rtt : from->rttMs * 0.8f + rtt * 0.2f;
            break;
        }
        case kChat: {
            r.U8();  // claimed sender, ignored
            std::string text = r.Str(kMaxChatLen);
            if (!r.Ok()) return;
            PlayerId fid = from->id;
            ByteWriter w(text.size() + 8);
            w.U8(kChat);
            w.U8(fid);
            w.Str(text);
            BroadcastRaw(w, Reliability::Reliable, fid);
            if (onChat) onChat(fid, text);
            break;
        }
        case kApp: {
            uint16_t id = static_cast<uint16_t>(r.VarU32());
            r.U8();  // claimed sender, replaced by the real one
            PlayerId target = r.U8();
            uint8_t flags = r.U8();
            auto payload = r.Rest();
            if (!r.Ok()) return;
            PlayerId fid = from->id;
            Reliability rel = (flags & kAppFlagReliable) ? Reliability::Reliable : ch;
            if (target == kHostPlayer || target == kAllPlayers) DispatchApp(id, fid, payload);
            if (target != kHostPlayer) {
                ByteWriter w = AppHeader(id, fid, target, rel);
                w.Bytes(payload);
                if (target == kAllPlayers) {
                    BroadcastRaw(w, rel, fid);
                } else if (PlayerInfo* to = FindMutable(target); to && to->id != kHostPlayer && to->id != fid) {
                    SendRaw(to->peer, w, rel);
                }
            }
            break;
        }
        default:
            break;
    }
}

// ---------------------------------------------------------------------------------------------------------------
// guest side

void Session::GuestOnMessage(ByteReader& r) {
    uint8_t type = r.U8();
    if (!r.Ok()) return;
    if (!welcomed_) {
        if (type == kWelcome) {
            PlayerId me = r.U8();
            r.U8();  // max players
            uint8_t n = r.U8();
            std::vector<PlayerInfo> list;
            for (int i = 0; i < n && r.Ok(); ++i) {
                PlayerInfo p;
                p.id = r.U8();
                p.name = r.Str(kMaxNameLen * 4);
                p.peer = p.id == kHostPlayer ? hostPeer_ : kInvalidPeer;
                list.push_back(std::move(p));
            }
            if (!r.Ok()) {
                Fail("invalid welcome from host");
                return;
            }
            players_ = std::move(list);
            localId_ = me;
            welcomed_ = true;
            state_ = SessionState::Connected;
            Log(kInfo, "session: joined as player %u (%zu players)", me, players_.size());
            if (onJoined) onJoined();
        } else if (type == kReject) {
            r.U8();
            std::string reason = r.Str(kMaxReasonLen);
            Fail("rejected by host: " + (reason.empty() ? std::string("no reason given") : reason));
        }
        return;
    }
    switch (type) {
        case kPeerJoined: {
            PlayerInfo p;
            p.id = r.U8();
            p.name = r.Str(kMaxNameLen * 4);
            if (!r.Ok() || FindPlayer(p.id)) return;
            players_.push_back(p);
            std::sort(players_.begin(), players_.end(),
                      [](const PlayerInfo& a, const PlayerInfo& b) { return a.id < b.id; });
            if (onPlayerJoined) onPlayerJoined(p);
            break;
        }
        case kPeerLeft: {
            PlayerId id = r.U8();
            std::string reason = r.Str(kMaxReasonLen);
            if (!r.Ok()) return;
            auto it = std::find_if(players_.begin(), players_.end(), [&](const PlayerInfo& p) { return p.id == id; });
            if (it == players_.end()) return;
            PlayerInfo info = *it;
            players_.erase(it);
            if (onPlayerLeft) onPlayerLeft(info, reason);
            break;
        }
        case kPing: {
            uint32_t t = r.U32();
            if (!r.Ok()) return;
            ByteWriter w(8);
            w.U8(kPong);
            w.U32(t);
            SendRaw(hostPeer_, w, Reliability::Unreliable);
            break;
        }
        case kPong: {
            uint32_t t = r.U32();
            if (!r.Ok()) return;
            float rtt = static_cast<float>(NowMs() - t);
            for (auto& p : players_)
                if (p.id == localId_ || p.id == kHostPlayer)
                    p.rttMs = (p.rttMs <= 0 ? rtt : p.rttMs * 0.8f + rtt * 0.2f);
            break;
        }
        case kPingTable: {
            uint8_t n = r.U8();
            for (int i = 0; i < n && r.Ok(); ++i) {
                PlayerId id = r.U8();
                uint16_t ms = r.U16();
                if (!r.Ok()) break;
                if (id == localId_ || id == kHostPlayer) continue;  // we measure those ourselves
                if (PlayerInfo* p = FindMutable(id)) p->rttMs = ms;
            }
            break;
        }
        case kChat: {
            PlayerId from = r.U8();
            std::string text = r.Str(kMaxChatLen);
            if (r.Ok() && onChat) onChat(from, text);
            break;
        }
        case kApp: {
            uint16_t id = static_cast<uint16_t>(r.VarU32());
            PlayerId from = r.U8();
            r.U8();  // target
            r.U8();  // flags
            auto payload = r.Rest();
            if (r.Ok()) DispatchApp(id, from, payload);
            break;
        }
        default:
            break;
    }
}

// ---------------------------------------------------------------------------------------------------------------
// app messages

void Session::On(uint16_t msgId, AppHandler handler) { handlers_[msgId] = std::move(handler); }
void Session::Off(uint16_t msgId) { handlers_.erase(msgId); }

void Session::DispatchApp(uint16_t id, PlayerId from, std::span<const uint8_t> payload) {
    auto it = handlers_.find(id);
    if (it == handlers_.end()) return;
    ByteReader r(payload);
    it->second(from, r);
}

ByteWriter Session::AppHeader(uint16_t id, PlayerId from, PlayerId target, Reliability rel) const {
    ByteWriter w(16);
    w.U8(kApp);
    w.VarU32(id);
    w.U8(from);
    w.U8(target);
    w.U8(rel == Reliability::Reliable ? kAppFlagReliable : 0);
    return w;
}

bool Session::SendToHost(uint16_t msgId, std::span<const uint8_t> payload, Reliability rel) {
    if (state_ == SessionState::Hosting) {
        DispatchApp(msgId, kHostPlayer, payload);
        return true;
    }
    if (state_ != SessionState::Connected) return false;
    ByteWriter w = AppHeader(msgId, localId_, kHostPlayer, rel);
    w.Bytes(payload);
    return SendRaw(hostPeer_, w, rel);
}

bool Session::Broadcast(uint16_t msgId, std::span<const uint8_t> payload, Reliability rel) {
    if (!IsActive()) return false;
    ByteWriter w = AppHeader(msgId, localId_, kAllPlayers, rel);
    w.Bytes(payload);
    if (state_ == SessionState::Hosting) return BroadcastRaw(w, rel, kHostPlayer);
    return SendRaw(hostPeer_, w, rel);
}

bool Session::SendTo(PlayerId player, uint16_t msgId, std::span<const uint8_t> payload, Reliability rel) {
    if (!IsActive()) return false;
    if (player == localId_) {
        DispatchApp(msgId, localId_, payload);
        return true;
    }
    ByteWriter w = AppHeader(msgId, localId_, player, rel);
    w.Bytes(payload);
    if (state_ == SessionState::Hosting) {
        PlayerInfo* p = FindMutable(player);
        return p && SendRaw(p->peer, w, rel);
    }
    return SendRaw(hostPeer_, w, rel);  // host delivers (player 0) or relays
}

bool Session::SendChat(std::string_view text) {
    if (!IsActive()) return false;
    std::string t(text.substr(0, kMaxChatLen));
    ByteWriter w(t.size() + 8);
    w.U8(kChat);
    w.U8(localId_);
    w.Str(t);
    if (state_ == SessionState::Hosting) return BroadcastRaw(w, Reliability::Reliable, kHostPlayer);
    return SendRaw(hostPeer_, w, Reliability::Reliable);
}

// ---------------------------------------------------------------------------------------------------------------
// helpers

bool Session::SendRaw(PeerId peer, const ByteWriter& w, Reliability rel) {
    if (!peer) return false;
    return transport_.Send(peer, w.Span(), rel);
}

bool Session::BroadcastRaw(const ByteWriter& w, Reliability rel, PlayerId except) {
    size_t limit = rel == Reliability::Reliable ? kMaxReliableMessage : kMaxUnreliableMessage;
    if (w.Size() > limit) return false;
    bool ok = true;
    for (auto& p : players_)
        if (p.id != kHostPlayer && p.id != except) ok &= SendRaw(p.peer, w, rel);
    return ok;
}

const PlayerInfo* Session::FindPlayer(PlayerId id) const {
    for (auto& p : players_)
        if (p.id == id) return &p;
    return nullptr;
}

PlayerInfo* Session::FindMutable(PlayerId id) {
    for (auto& p : players_)
        if (p.id == id) return &p;
    return nullptr;
}

PlayerInfo* Session::FindByPeer(PeerId peer) {
    for (auto& p : players_)
        if (p.peer == peer && p.id != kHostPlayer) return &p;
    return nullptr;
}

}  // namespace mc::net
