// MafiaCoop net: co-op session over any Transport. Host-authoritative star: guests talk only to the host,
// the host relays. Handshake, player list, ping, chat and generic app messages. See docs/protocol.md.
#pragma once
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "net/transport.h"
#include "net/wire.h"

namespace mc::net {

// Bump when the session or any app message format changes incompatibly.
constexpr uint32_t kProtocolVersion = 3;

using PlayerId = uint8_t;
constexpr PlayerId kHostPlayer = 0;
constexpr PlayerId kNoPlayer = 0xFE;
constexpr PlayerId kAllPlayers = 0xFF;
constexpr int kMaxPlayers = 4;

struct PlayerInfo {
    PlayerId id = kNoPlayer;
    std::string name;
    PeerId peer = kInvalidPeer;  // host: transport peer of that guest; guest: host's peer for id 0, else 0
    float rttMs = 0;             // to the host (host's own entry: 0)
};

struct SessionConfig {
    uint32_t protocolVersion = kProtocolVersion;
    uint32_t modBuild = 0;
    std::string playerName = "Player";
    int maxPlayers = kMaxPlayers;
    double pingIntervalS = 1.0;
    double helloTimeoutS = 5.0;   // host: drop a connection that has not sent Hello
    double welcomeTimeoutS = 10.0; // guest: give up if no Welcome/Reject
};

enum class SessionState { Idle, Hosting, Connecting, Connected, Disconnected };

class Session {
public:
    // from: sender's player id. payload: app message body (after the header).
    using AppHandler = std::function<void(PlayerId from, ByteReader& payload)>;

    Session(Transport& transport, SessionConfig cfg);
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    bool Host(uint16_t port);
    bool Join(std::string_view address);
    void Leave();
    void Poll();  // once per frame

    // ---- app messages (ids are free for later phases; see docs/protocol.md for the allocation) ----
    void On(uint16_t msgId, AppHandler handler);
    void Off(uint16_t msgId);
    // Guest: to the host only. Host: handled locally (loopback) for symmetric code paths.
    bool SendToHost(uint16_t msgId, std::span<const uint8_t> payload, Reliability rel = Reliability::Reliable);
    // Host: to every guest. Guest: to the host, which handles it and relays it to the other guests.
    bool Broadcast(uint16_t msgId, std::span<const uint8_t> payload, Reliability rel = Reliability::Reliable);
    // To one player. Guest->guest goes through the host relay.
    bool SendTo(PlayerId player, uint16_t msgId, std::span<const uint8_t> payload,
                Reliability rel = Reliability::Reliable);
    bool SendChat(std::string_view text);

    // ---- state ----
    SessionState State() const { return state_; }
    bool IsHost() const { return state_ == SessionState::Hosting; }
    bool IsActive() const { return state_ == SessionState::Hosting || state_ == SessionState::Connected; }
    PlayerId LocalId() const { return localId_; }
    const std::vector<PlayerInfo>& Players() const { return players_; }
    const PlayerInfo* FindPlayer(PlayerId id) const;
    const std::string& LastError() const { return lastError_; }
    Transport& GetTransport() { return transport_; }
    const SessionConfig& Config() const { return cfg_; }

    // ---- events ----
    std::function<void()> onJoined;                           // guest: Welcome received
    std::function<void(const PlayerInfo&)> onPlayerJoined;    // someone else joined
    std::function<void(const PlayerInfo&, const std::string& reason)> onPlayerLeft;
    std::function<void(PlayerId from, const std::string& text)> onChat;
    std::function<void(const std::string& reason)> onDisconnected;  // guest: lost/rejected/kicked; host: never

private:
    struct Pending {  // host: connected transport peer that has not completed the handshake
        double since = 0;
        double kickAt = 0;  // >0: rejected, drop at this time (lets the Reject be delivered)
    };

    void OnEvent(const Event& e);
    void HostOnMessage(PeerId peer, ByteReader& r, Reliability ch);
    void GuestOnMessage(ByteReader& r);
    void HostOnHello(PeerId peer, ByteReader& r);
    void Reject(PeerId peer, uint8_t code, const std::string& reason);
    void HostRemovePlayer(PeerId peer, const std::string& reason);
    void DispatchApp(uint16_t id, PlayerId from, std::span<const uint8_t> payload);
    bool SendRaw(PeerId peer, const ByteWriter& w, Reliability rel);
    bool BroadcastRaw(const ByteWriter& w, Reliability rel, PlayerId except);  // false if any send failed
    void Fail(const std::string& reason);
    void Reset();
    PlayerInfo* FindByPeer(PeerId peer);
    PlayerInfo* FindMutable(PlayerId id);
    ByteWriter AppHeader(uint16_t id, PlayerId from, PlayerId target, Reliability rel) const;

    Transport& transport_;
    SessionConfig cfg_;
    SessionState state_ = SessionState::Idle;
    PlayerId localId_ = kNoPlayer;
    PeerId hostPeer_ = kInvalidPeer;
    std::vector<PlayerInfo> players_;
    std::unordered_map<PeerId, Pending> pending_;
    std::unordered_map<uint16_t, AppHandler> handlers_;
    std::string lastError_;
    double helloSentAt_ = 0, lastPing_ = 0, lastPingTable_ = 0;
    bool welcomed_ = false;
};

}  // namespace mc::net
