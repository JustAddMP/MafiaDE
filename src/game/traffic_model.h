#pragma once
// Traffic sync, the part without the game (docs/protocol.md "Traffic", docs/research-world-sync.md B4): the wire
// format of the traffic messages, the host's per-guest bookkeeping (which of its traffic cars each guest gets, net
// ids, spawn / despawn / state packing on top of core's InterestManager) and the guest's table of host cars (their
// snapshots, the target of each copy, which cars get a copy, a VehicleController per copy). Pure data in, decisions
// out: no Lua, no session, no OS calls, so tests/traffic_test.cpp runs it without the game. game/traffic_sync.cpp
// feeds it from the Lua glue (traffic_sync.lua) and the session. Single-threaded (the caller locks).
#include "net/interest.h"
#include "net/playout.h"
#include "net/remote_motion.h"
#include "net/wire.h"

#include <cstdint>
#include <deque>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace mc::game::traffic {

// ---- protocol (0x0220-0x022F) --------------------------------------------------------------------------------------
constexpr uint16_t kMsgSpawn = 0x0220;    // host -> one guest, reliable: cars he gets copies of (model, kind, look)
constexpr uint16_t kMsgDespawn = 0x0221;  // host -> one guest, reliable: cars he no longer gets
constexpr uint16_t kMsgStates = 0x0222;   // host -> one guest, unreliable: packed car states
constexpr uint16_t kMsgBump = 0x0223;     // guest -> host, reliable: a copy was knocked far off its target (logged)

enum Kind : uint8_t { kTraffic = 0, kPolice = 1 };
enum StateFlags : uint8_t {
    kHandbrake = 1,   // handbrake > 0.5
    kSiren = 2,       // police: siren and beacons on (host heuristic, see traffic_sync.lua)
    kNoDriver = 4,    // the driver is dead or gone (the car rolls on)
    kControls = 8,    // steer / power / brake are the driver's (else unknown: 0)
    kReverse = 16,    // moving backwards (power then means reverse: not applied on the copy)
};

using Vec3 = net::motion::Vec3;

struct CarState {
    uint16_t net = 0;
    Vec3 pos, vel;
    float rot[4] = {0, 0, 0, 1};  // quaternion x y z w
    float steer = 0, power = 0, brake = 0;
    uint8_t flags = 0;
};

struct SpawnInfo {
    uint16_t net = 0;
    uint8_t kind = kTraffic;
    std::string model;
    int16_t color[2] = {-1, -1};  // GetColor(1) / GetColor(2) on the host, -1 unknown
    std::string plate;            // "" unknown
    CarState st;                  // where it is (position, rotation, velocity at full precision)
};

// Quaternion <-> 32 bits ("smallest three": index of the largest component, the other three in 10 bits each).
uint32_t PackQuat(const float q[4]);
void UnpackQuat(uint32_t v, float q[4]);

// A token the guest's Lua may take as one field: [A-Za-z0-9_-], 1..max chars.
bool SafeToken(const std::string& s, size_t max);

// TrafficStates: u32 hostMs, f32 origin x y z, u8 n, then n x 22 bytes: u16 net, i16 x y z (1/32 m from the origin),
// i16 vx vy vz (1/100 m/s), u32 quat, i8 steer (x127), u8 power, u8 brake (x255), u8 flags.
constexpr size_t kStateBytes = 22;
constexpr size_t kStatesHeader = 17;
constexpr size_t kMaxPerPacket = 40;          // 17 + 40 x 22 = 897 bytes (unreliable limit 1100 with headers)
constexpr float kPosScale = 32.0f;            // the offset range is +-1023 m around the origin
constexpr float kVelScale = 100.0f;
// Splits `states` into packets (a new one when full or when a car is out of the current origin's range).
std::vector<std::vector<uint8_t>> PackStates(uint32_t hostMs, const std::vector<CarState>& states,
                                             size_t maxPerPacket = kMaxPerPacket);
bool ReadStates(net::ByteReader& r, uint32_t& hostMs, std::vector<CarState>& out);
// What a state looks like after a trip over the wire (for change detection and tests).
CarState Quantised(const CarState& s);

// TrafficSpawn: u8 n, then n x: u16 net, u8 kind, str model, i16 color1 color2, str plate, f32 x y z, u32 quat,
// i16 vx vy vz (1/100 m/s), u8 flags.  TrafficDespawn: u8 n, n x u16 net.  TrafficBump: u16 net, f32 err m,
// f32 copy speed, f32 target speed.
constexpr size_t kMaxSpawnsPerMessage = 16;
std::vector<uint8_t> PackSpawns(const std::vector<SpawnInfo>& spawns);
bool ReadSpawns(net::ByteReader& r, std::vector<SpawnInfo>& out);
std::vector<uint8_t> PackDespawns(const std::vector<uint16_t>& nets);
bool ReadDespawns(net::ByteReader& r, std::vector<uint16_t>& out);

// ---- host -------------------------------------------------------------------------------------------------------
struct HostCar {
    uint32_t key = 0;  // its entity id on the host's game
    uint8_t kind = kTraffic;
    std::string model;
    int16_t color[2] = {-1, -1};
    std::string plate;
    CarState st;  // net is filled in by HostTraffic
};

struct HostConfig {
    float enterRadius = 260, leaveRadius = 320;  // m from the guest
    size_t maxPerPeer = 24;                      // copies one guest gets at most (nearest first)
    float evictFar = 150, evictNear = 90;        // a car beyond evictFar gives way to one within evictNear
    uint32_t bytesPerSecond = 40 * 1024;         // state budget per guest (interest manager)
    double refresh = 1.5;                        // s: an unchanged state is sent again after this long anyway
    double viewTimeout = 2.0;                    // s without a viewpoint: the guest sees nothing
    size_t maxPerPacket = kMaxPerPacket;
};

struct PeerOut {
    uint32_t peer = 0;
    std::vector<std::vector<uint8_t>> spawns;    // TrafficSpawn payloads
    std::vector<std::vector<uint8_t>> despawns;  // TrafficDespawn payloads
    std::vector<std::vector<uint8_t>> states;    // TrafficStates payloads
    uint32_t spawned = 0, despawned = 0, sent = 0, skipped = 0;  // cars
};

struct HostPeerStats {
    uint32_t relevant = 0, admitted = 0;
    uint64_t spawned = 0, despawned = 0, states = 0, skipped = 0, bytes = 0;
};

class HostTraffic {
public:
    explicit HostTraffic(const HostConfig& cfg = {});
    // This round's traffic cars near any guest (a car missing from the list is gone: despawned on every guest).
    void SetCars(const std::vector<HostCar>& cars);
    // A guest's position (his player, or his car); a guest seen for the first time is added.
    void SetView(uint32_t peer, const Vec3& pos, double now);
    void RemovePeer(uint32_t peer);  // left the session: nothing is sent to him any more
    void Tick(double now, uint32_t hostMs, std::vector<PeerOut>& out);
    uint32_t KeyOf(uint16_t net) const;  // 0 = unknown
    uint16_t NetOf(uint32_t key) const;  // 0 = unknown
    size_t CarCount() const { return cars_.size(); }
    const HostPeerStats* Stats(uint32_t peer) const;
    const net::InterestManager& Interest() const { return im_; }

private:
    struct Car {
        HostCar c;
        bool seen = false;
    };
    struct Sent {
        CarState q;  // quantised, as last sent
        double at = -1e9;
    };
    struct Peer {
        Vec3 view;
        double viewAt = -1e9;
        bool hasView = false;
        std::set<uint16_t> relevant;
        std::map<uint16_t, Sent> admitted;
        HostPeerStats stats;
    };
    uint16_t NewNet();
    float Dist(const Peer& p, uint16_t net) const;

    HostConfig cfg_;
    net::InterestManager im_;
    std::map<uint16_t, Car> cars_;        // by net id
    std::map<uint32_t, uint16_t> byKey_;  // host entity id -> net id
    std::map<uint32_t, Peer> peers_;
    uint16_t nextNet_ = 1;
};

// ---- guest ------------------------------------------------------------------------------------------------------
struct GuestConfig {
    // s behind the newest host time until the adaptive playout delay (net/playout.h: jitter + the state interval,
    // ~55-70 ms at 20 Hz on a quiet link; was this fixed 120 ms before 0.15) has enough states
    double interpDelay = 0.12;
    double maxExtrapolate = 0.5;    // s past the newest state: then held
    float maxLead = 2.0f;           // m: the "now" lead (velocity x delay) at most
    float teleport = 25.0f;         // m between two states: a jump, not lerped
    double stale = 5.0;             // s without a state: the copy goes (kept in the table a while longer)
    double forget = 15.0;           // s without a state: forgotten
    size_t maxCopies = 20;          // copies on this game at most
    float addRadius = 240;          // m from us: a car gets a copy
    float dropRadius = 290;         // m: its copy goes
    float hostNear = 250, hostFar = 400;  // m to the host: switch to host traffic / back to our own
    float nearDrive = 90;           // m: copies farther than this are driven every farDriveEvery ticks
    int farDriveEvery = 3;
    double reportFresh = 0.3;       // s: an older copy report is not used
    float bumpError = 3.0f;         // m: a copy this far off its target that was on it a moment before was knocked
    double bumpEvery = 2.0;         // s between bump reports of one car
    double despawnGrace = 1.0;      // s a copy stays after its despawn (player sync may adopt it: a car jacked)
};

struct CopyReport {
    Vec3 pos, vel;
    float rot[4] = {0, 0, 0, 1};
};

struct GuestCmd {
    enum class Type : uint8_t { Add, Remove, Drive };
    Type type = Type::Drive;
    uint16_t net = 0;
    // Add
    const SpawnInfo* info = nullptr;  // valid until the next call into GuestTraffic
    // Add / Drive: where the host's car is now
    CarState target;
    // Drive
    net::motion::VehicleCommand vc;
    bool grace = false;  // Remove: after despawnGrace (the host no longer sends it), not at once
};

struct GuestBump {
    uint16_t net = 0;
    float error = 0, copySpeed = 0, targetSpeed = 0;
};

struct GuestStats {
    uint32_t known = 0, live = 0, copies = 0, pins = 0, snaps = 0, bumps = 0;
    float avgErr = 0, maxErr = 0;
    uint64_t states = 0, spawns = 0, despawns = 0, dropsOld = 0;
};

class GuestTraffic {
public:
    explicit GuestTraffic(const GuestConfig& cfg = {});
    void OnSpawn(const SpawnInfo& s, double now);
    void OnDespawn(uint16_t net, double now);
    void OnStates(uint32_t hostMs, const std::vector<CarState>& states, double now);
    // Our player and (when known) the host's player (his puppet here).
    void SetLocal(const Vec3& me, bool haveHost, const Vec3& host);
    void SetReport(uint16_t net, const CopyReport& r, double now);  // where our copy is (after this tick's commands)
    void Release(uint16_t net);  // the copy is someone's car now (player sync took it): never driven or removed by us
    void Gone(uint16_t net);     // the copy is gone on our game (despawned by the game, world change): spawn anew
    void Clear();                // world change / session end: everything forgotten (the game removed the copies)
    // Commands for this tick: Remove first, then Add (nearest first), then Drive for the copies that are due.
    void Tick(double now, float dt, std::vector<GuestCmd>& out, std::vector<GuestBump>& bumps);
    bool HostMode() const { return hostMode_; }  // near the host: his traffic here, ours off
    double PlayoutDelay() const { return playout_.Delay(); }  // s the copies are shown behind the host's states
    const GuestStats& Stats() const { return stats_; }
    // For traces: the copy of `net` (nullptr if none) and its last error.
    bool CopyInfo(uint16_t net, float& err, bool& pinned, int& pins) const;
    // The target of a car this tick (false: unknown / stale).
    bool Target(uint16_t net, double now, CarState& out) const;

private:
    struct Snap {
        double t = 0;  // host clock, s
        CarState s;
    };
    struct Car {
        SpawnInfo info;
        bool haveInfo = false;
        std::deque<Snap> snaps;
        double lastRecv = -1e9;
        bool copy = false;      // a copy exists (Add sent)
        bool released = false;  // handed over to player sync
        double despawnAt = -1;  // host despawned it: removed at this time
        net::motion::VehicleController vc;
        CopyReport rep;
        double repAt = -1e9;
        float lastErr = 0;
        float accDt = 0;
        int tick = 0;
        double bumpAt = -1e9;
        bool haveLast = false;
        Vec3 lastTarget;
    };
    bool Present(const Car& c, double now, CarState& out) const;
    float DistToMe(const Car& c, double now) const;

    GuestConfig cfg_;
    std::map<uint16_t, Car> cars_;
    std::vector<uint16_t> pendingRemove_;  // copies of a car whose model changed (spawned again)
    int64_t clockMs_ = 0;                  // host clock unwrapped from its 32-bit wire value
    uint32_t lastRawMs_ = 0;
    bool haveClock_ = false;
    double offset_ = 0;  // local clock - host clock (the playout clock's: smallest over the last seconds)
    bool haveOffset_ = false;
    net::playout::PlayoutClock playout_;  // how far behind the host's states the copies are shown
    Vec3 me_;
    bool haveMe_ = false;
    bool hostMode_ = false;
    GuestStats stats_;
};

}  // namespace mc::game::traffic
