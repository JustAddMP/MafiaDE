#pragma once
// Traffic sync, the part without the game (docs/protocol.md "Traffic", docs/sync-architecture.md principle 1): the
// wire format of the traffic messages, the region a player's game is in (whose ambient traffic runs where: core's
// RegionAuthority with hysteresis), an authority's per-player bookkeeping (which of its traffic cars each other player
// gets, net ids, spawn / despawn / state packing on top of core's InterestManager) and a receiver's table of every
// other authority's cars (one core replica::Stream per authority, the target of each copy, which cars get a copy, a
// VehicleController + core Corrector per copy). Every game is an authority for the traffic its own game spawned and a
// receiver for everyone else's. Pure data in, decisions out: no Lua, no session, no OS calls, so tests/traffic_test.cpp
// runs it without the game. game/traffic_sync.cpp feeds it from the Lua glue (traffic_sync.lua) and the session.
// Single-threaded (the caller locks).
#include "net/authority.h"
#include "net/correction.h"
#include "net/interest.h"
#include "net/playout.h"
#include "net/remote_motion.h"
#include "net/replica.h"
#include "net/wire.h"

#include <cstdint>
#include <map>
#include <set>
#include <span>
#include <string>
#include <vector>

namespace mc::game::traffic {

// ---- protocol (0x0220-0x022F) --------------------------------------------------------------------------------------
// The host's own traffic (authority 0), as in 0.15:
constexpr uint16_t kMsgSpawn = 0x0220;    // host -> one player, reliable: cars he gets copies of (model, kind, look)
constexpr uint16_t kMsgDespawn = 0x0221;  // host -> one player, reliable: cars he no longer gets
constexpr uint16_t kMsgStates = 0x0222;   // host -> one player, unreliable: packed car states
constexpr uint16_t kMsgBump = 0x0223;     // a player -> host, reliable: his copy of a host car was knocked off (logged)
// A guest's traffic (any authority but the host): the same bodies after a u8 authority (= the sender; guest -> guest
// through core's relay, which keeps the sender). docs/protocol.md "Traffic".
constexpr uint16_t kMsgSpawn2 = 0x0224;    // authority -> one player, reliable
constexpr uint16_t kMsgDespawn2 = 0x0225;  // authority -> one player, reliable
constexpr uint16_t kMsgStates2 = 0x0226;   // authority -> one player, unreliable
constexpr uint16_t kMsgBump2 = 0x0227;     // a player -> that authority, reliable: u8 authority, then TrafficBump

enum Kind : uint8_t { kTraffic = 0, kPolice = 1 };
enum StateFlags : uint8_t {
    kHandbrake = 1,   // handbrake > 0.5
    kSiren = 2,       // police: siren and beacons on (authority heuristic, see traffic_sync.lua)
    kNoDriver = 4,    // the driver is dead or gone, confirmed (DriverWatch: missing for kOrphanConfirm s while slow)
    kControls = 8,    // steer / power / brake are the driver's (else unknown: 0)
    kReverse = 16,    // moving backwards (power then means reverse: not applied on the copy)
};
// Never on the wire: the receiver's Drive command to its Lua sets it when the copy's crew may get out (CrewGetsOut).
constexpr uint8_t kCrewOut = 32;

// A traffic car on every game: {authority, net} as one number (authority << 16 | net). The host's cars (authority 0)
// keep their net id as key, so everything keyed by the host's net id before (car damage, car look, explosions) is
// unchanged. As a core NetRef: {authority, RefKind::Traffic, net}.
inline uint32_t RefKey(uint8_t authority, uint16_t net) { return (static_cast<uint32_t>(authority) << 16) | net; }
inline uint8_t RefAuthority(uint32_t key) { return static_cast<uint8_t>(key >> 16); }
inline uint16_t RefNet(uint32_t key) { return static_cast<uint16_t>(key & 0xFFFFu); }
inline net::authority::NetRef RefOf(uint32_t key) {
    return net::authority::NetRef{RefAuthority(key), static_cast<uint8_t>(net::authority::RefKind::Traffic), RefNet(key)};
}

// ---- drivers ------------------------------------------------------------------------------------------------------
// The authority's Lua reports each round whether it read a living traffic driver at the wheel (TC flag 4 = "none read
// now"). That read flickers: in the 0.15.0 two-PC test 118 of 179 cars flagged on the first miss were still moving
// at 2-15 m/s and 84 kept their speed 4 s later (someone was driving), mostly 50-100 m from the host's player
// (docs/game-api.md 19). So a car only counts as driverless (kNoDriver sent) when no driver was read for
// kOrphanConfirm s while it is slow; it is driven again (flag cleared) when a driver is read or it speeds up.
constexpr double kOrphanConfirm = 1.0;  // s without a driver read, slow all that time
constexpr float kOrphanSlow = 2.0f;     // m/s: slow
constexpr float kOrphanDriven = 4.0f;   // m/s: a declared driverless car this fast is driven again (someone drives it)
constexpr double kOrphanGap = 0.5;      // s between two rounds (normally 0.05): the game stood, the watch restarts

class DriverWatch {
public:
    // One round: was a living driver read at the wheel now, how fast the car goes (m/s). Returns Declared().
    bool Update(double now, bool driverSeen, float speed);
    bool Declared() const { return declared_; }
    bool Missing() const { return missSince_ >= 0; }
    double MissingFor(double now) const { return missSince_ >= 0 ? now - missSince_ : 0; }

private:
    double missSince_ = -1;  // first round without a driver read (-1: one was read last round)
    double slowSince_ = -1;  // first round slow while missing (-1: not slow)
    double lastAt_ = -1;     // the last round (kOrphanGap)
    bool declared_ = false;
};

// Receiver: may the crew of a copy get out now? Only for a car confirmed driverless (kNoDriver) that stands or rolls
// slower than kOrphanSlow, and whose copy here is that slow too (copySpeed < 0: no report of the copy yet). A moving
// copy always keeps its crew seated (0.15.0: 249 "lost its driver" get-outs left empty copies driving on and their
// drivers standing in the road).
bool CrewGetsOut(uint8_t targetFlags, float targetSpeed, float copySpeed);

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
    int16_t color[2] = {-1, -1};  // GetColor(1) / GetColor(2) on the authority, -1 unknown
    std::string plate;            // "" unknown
    CarState st;                  // where it is (position, rotation, velocity at full precision)
};

// Quaternion <-> 32 bits ("smallest three": index of the largest component, the other three in 10 bits each).
uint32_t PackQuat(const float q[4]);
void UnpackQuat(uint32_t v, float q[4]);

// A token the Lua may take as one field: [A-Za-z0-9_-], 1..max chars.
bool SafeToken(const std::string& s, size_t max);

// TrafficStates: u32 ms (the authority's clock), f32 origin x y z, u8 n, then n x 22 bytes: u16 net, i16 x y z (1/32 m
// from the origin), i16 vx vy vz (1/100 m/s), u32 quat, i8 steer (x127), u8 power, u8 brake (x255), u8 flags.
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

struct BumpInfo {
    uint16_t net = 0;
    float error = 0, copySpeed = 0, targetSpeed = 0;
};
std::vector<uint8_t> PackBump(const BumpInfo& b);
bool ReadBump(net::ByteReader& r, BumpInfo& out);

// The 0x0224-0x0227 bodies: u8 authority, then the 0x0220-0x0223 body.
std::vector<uint8_t> WithAuthority(uint8_t authority, const std::vector<uint8_t>& body);

// ---- regions ------------------------------------------------------------------------------------------------------
// Whose region a game is in, i.e. whether its own ambient traffic runs (docs/sync-architecture.md principle 1): the
// host's game always runs its own. A guest's game runs its own unless it is in another player's region: within
// joinRadius of the region's owner it joins it (its own spawning goes off, the owner's traffic shows), and it runs its
// own again only when it is beyond leaveRadius of every region owner before it (core RegionAuthority::RegionOf: the
// host's region first, then each guest farther than leaveRadius from the host and the owners before him, in id order;
// two guests together far from the host share the lower id's region). Between the two radii a game keeps what it had.
// Every game decides this for itself from every player's position (they agree but for the latency of the positions;
// the shown traffic does not depend on it: every game shows every other game's cars near it, docs/protocol.md).
struct RegionConfig {
    float joinRadius = 250;   // m to a region's owner: join it (was: the host only)
    float leaveRadius = 400;  // m: our own again beyond this from every region owner
};

class RegionFollow {
public:
    explicit RegionFollow(const RegionConfig& cfg = {});
    // players: every player whose position we know (inWorld false: not in the world now), us included.
    // Returns true when Authority() changed.
    bool Update(net::PlayerId me, net::PlayerId host, std::span<const net::authority::PlayerSpot> players, double now);
    net::PlayerId Authority() const { return auth_; }  // whose region we are in (me: our own traffic runs)
    bool Own() const { return auth_ == me_; }
    double Since(double now) const { return now - changedAt_; }  // s since the last change
    float DistTo(net::PlayerId id) const;                          // m from us to that player (-1 unknown)
    const net::authority::RegionAuthority& Regions() const { return ra_; }

private:
    RegionConfig cfg_;
    net::authority::RegionAuthority ra_;
    std::vector<net::authority::PlayerSpot> players_;
    net::PlayerId me_ = 0, auth_ = 0;
    bool init_ = false;
    double changedAt_ = 0;
};

// ---- authority (every game: its own traffic -> the other players) ----------------------------------------------------
struct HostCar {
    uint32_t key = 0;  // its entity id on this game
    uint8_t kind = kTraffic;
    std::string model;
    int16_t color[2] = {-1, -1};
    std::string plate;
    CarState st;  // net is filled in by AuthorityTraffic; kNoDriver here = no driver read this round (DriverWatch)
};

struct HostConfig {
    float enterRadius = 260, leaveRadius = 320;  // m from the other player
    size_t maxPerPeer = 24;                      // copies one player gets at most (nearest first)
    float evictFar = 150, evictNear = 90;        // a car beyond evictFar gives way to one within evictNear
    uint32_t bytesPerSecond = 40 * 1024;         // state budget per player (interest manager)
    double refresh = 1.5;                        // s: an unchanged state is sent again after this long anyway
    double viewTimeout = 2.0;                    // s without a viewpoint: the player sees nothing
    size_t maxPerPacket = kMaxPerPacket;
    float shareRadius = 240;                     // m: a car this near another player should be his (Unshared)
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

class AuthorityTraffic {
public:
    explicit AuthorityTraffic(const HostConfig& cfg = {});
    // This round's traffic cars near any other player (a car missing from the list is gone: despawned everywhere).
    // `now` (s) times each car's DriverWatch: its kNoDriver read becomes the confirmed flag that is sent.
    void SetCars(const std::vector<HostCar>& cars, double now = 0);
    // Cars that went driverless (confirmed) / were found driven again since the start (logs and traces).
    uint64_t Orphaned() const { return orphaned_; }
    uint64_t DrivenAgain() const { return drivenAgain_; }
    size_t MissingNow() const;   // cars without a driver read now
    size_t DeclaredNow() const;  // of those, confirmed driverless
    // Another player's position (his player, or his car); one seen for the first time is added.
    void SetView(uint32_t peer, const Vec3& pos, double now);
    void RemovePeer(uint32_t peer);  // left the session: nothing is sent to him any more
    void Tick(double now, uint32_t hostMs, std::vector<PeerOut>& out);
    uint32_t KeyOf(uint16_t net) const;  // 0 = unknown
    uint16_t NetOf(uint32_t key) const;  // 0 = unknown
    size_t CarCount() const { return cars_.size(); }
    const HostPeerStats* Stats(uint32_t peer) const;
    const net::InterestManager& Interest() const { return im_; }
    // The cars whose state went to at least one player in the last Tick (full precision; the truth record TRS).
    const std::vector<CarState>& SentStates() const { return sentStates_; }
    // Our cars within `near` m of `me` that a player within shareRadius of them has not had for more than `grace` s
    // (the hand-over target: a car near a receiver is shared, not left driving on one game only). `mine`: all of our
    // cars within `near` of `me`.
    size_t Unshared(const Vec3& me, float near, double now, double grace, size_t* mine = nullptr) const;

private:
    struct Car {
        HostCar c;
        bool seen = false;
        DriverWatch drv;
        double unsharedSince = -1;  // a player near it has not got it since then (-1: everyone near has it)
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
    std::map<uint32_t, uint16_t> byKey_;  // entity id -> net id
    std::map<uint32_t, Peer> peers_;
    uint16_t nextNet_ = 1;
    uint64_t orphaned_ = 0, drivenAgain_ = 0;
    std::vector<CarState> sentStates_;
};
using HostTraffic = AuthorityTraffic;  // the host's traffic is one authority's (the name of 0.15)

// ---- receiver (every game: the other authorities' cars near us) ------------------------------------------------------
// The correction preset the copies use: core's Tuning::ForTraffic() with the dead zone the recorded-path simulation
// needs (tests/traffic_test.cpp "recorded path"; docs/protocol.md "Traffic").
net::correction::Tuning TrafficCorrection();
// The VehicleController tuning of a traffic copy: the defaults, with the snap distance the corrector's.
net::motion::VehicleTuning TrafficVehicleTuning(const net::correction::Tuning& corr);

struct GuestConfig {
    // s behind the newest state until the adaptive playout delay (net/playout.h: jitter + the state interval, ~55-70 ms
    // at 20 Hz on a quiet link; was this fixed 120 ms before 0.15) has enough states
    double interpDelay = 0.12;
    double maxExtrapolate = 0.5;    // s past the newest state: then held
    // m: the "now" lead (velocity x playout delay) at most: 30 m/s x the playout clock's 0.3 s ceiling (was 2 m: a car
    // 150 m away, sent every ~0.2 s, was shown 4.6 m behind; tests/traffic_test.cpp "recorded path")
    float maxLead = 9.0f;
    float teleport = 25.0f;         // m between two states: a jump, not interpolated across
    size_t trackSamples = 8;        // states kept per car
    double stale = 5.0;             // s without a state: the copy goes (kept in the table a while longer)
    double forget = 15.0;           // s without a state: forgotten
    size_t maxCopies = 20;          // copies on this game at most (of every authority together)
    float addRadius = 240;          // m from us: a car gets a copy
    float dropRadius = 290;         // m: its copy goes
    float viewRadius = 150;         // m: a car this near counts for "shown" (Stats: due / shown)
    float nearDrive = 90;           // m: copies farther than this are driven every farDriveEvery ticks
    int farDriveEvery = 3;
    double reportFresh = 0.3;       // s: an older copy report is not used
    float bumpError = 3.0f;         // m: a copy this far off its target that was on it a moment before was knocked
    double bumpEvery = 2.0;         // s between bump reports of one car
    double despawnGrace = 1.0;      // s a copy stays after its despawn (player sync may adopt it: a car jacked)
    // an authority's pause (game/peer_pause.h): no knock checks while it is paused and this long after its resume,
    // and after a pause of our own (a tick gap of more than localGap s: our copies were left behind meanwhile)
    double quietAfter = 1.0;
    double localGap = 0.5;
    net::correction::Tuning correction = TrafficCorrection();  // the copy's position correction
};

struct CopyReport {
    Vec3 pos, vel;
    float rot[4] = {0, 0, 0, 1};
};

struct GuestCmd {
    enum class Type : uint8_t { Add, Remove, Drive };
    Type type = Type::Drive;
    uint32_t key = 0;  // RefKey(authority, net)
    // Add
    const SpawnInfo* info = nullptr;  // valid until the next call into ReceiverTraffic
    // Add / Drive: where the authority's car is now
    CarState target;
    // Drive
    net::motion::VehicleCommand vc;
    bool grace = false;  // Remove: after despawnGrace (the authority no longer sends it), not at once
};

struct GuestBump {
    uint32_t key = 0;
    float error = 0, copySpeed = 0, targetSpeed = 0;
};

struct GuestStats {
    uint32_t known = 0, live = 0, copies = 0, pins = 0, snaps = 0, bumps = 0;
    uint32_t quietBumps = 0;    // knocks not reported: the authority was paused or had just resumed, or we had
    uint32_t pauses = 0, localGaps = 0;
    bool frozen = false;        // an authority with cars here is paused: its copies are held
    float avgErr = 0, maxErr = 0;
    uint64_t states = 0, spawns = 0, despawns = 0, dropsOld = 0;
    uint32_t due = 0, shown = 0;  // cars of other authorities within viewRadius (at most maxCopies) / with a copy here
    uint64_t blends = 0, fixSnaps = 0;  // corrector: blended / snapped updates so far
};

struct AuthorityStats {
    uint32_t live = 0, copies = 0, due = 0, shown = 0;
    bool paused = false;
};

class ReceiverTraffic {
public:
    explicit ReceiverTraffic(const GuestConfig& cfg = {});
    void OnSpawn(uint8_t authority, const SpawnInfo& s, double now);
    void OnDespawn(uint8_t authority, uint16_t net, double now);
    // ms: the authority's clock (its 32-bit wire value)
    void OnStates(uint8_t authority, uint32_t ms, const std::vector<CarState>& states, double now);
    // the host's cars (authority 0), the 0.15 calls
    void OnSpawn(const SpawnInfo& s, double now) { OnSpawn(0, s, now); }
    void OnDespawn(uint16_t net, double now) { OnDespawn(0, net, now); }
    void OnStates(uint32_t ms, const std::vector<CarState>& states, double now) { OnStates(0, ms, states, now); }
    // Our player's position (haveMe false: not in the world: no copies).
    void SetLocal(const Vec3& me, bool haveMe = true);
    void SetReport(uint32_t key, const CopyReport& r, double now);  // where our copy is (after this tick's commands)
    void Release(uint32_t key);  // the copy is someone's car now (player sync took it): never driven or removed by us
    void Gone(uint32_t key);     // the copy is gone on our game (despawned by the game, world change): spawn anew
    void Clear();                // world change / session end: everything forgotten (the game removed the copies)
    // An authority left the session: its copies go (Remove at the next Tick) and its stream is forgotten.
    void RemoveAuthority(uint8_t authority);
    // An authority paused its game (true) or resumed it (false), game/peer_pause.h: while paused each of its copies is
    // held where it stands (Pin to that pose, no speed, braked), none goes stale or is spawned, no knock is reported;
    // after the resume they are driven again and knocks are not reported for quietAfter s.
    void SetPaused(uint8_t authority, bool paused, double now);
    bool Paused(uint8_t authority) const;
    void SetHostPaused(bool paused, double now) { SetPaused(0, paused, now); }
    bool HostPaused() const { return Paused(0); }
    // Commands for this tick: Remove first, then Add (nearest first), then Drive for the copies that are due.
    void Tick(double now, float dt, std::vector<GuestCmd>& out, std::vector<GuestBump>& bumps);
    double PlayoutDelay(uint8_t authority = 0) const;  // s the copies of that authority are shown behind its states
    const GuestStats& Stats() const { return stats_; }
    // Per authority (stats of the last Tick); the authorities in id order.
    std::vector<uint8_t> Authorities() const;
    AuthorityStats StatsOf(uint8_t authority) const;
    // For traces: the copy of `key` (false if none) and its last error.
    bool CopyInfo(uint32_t key, float& err, bool& pinned, int& pins) const;
    // The target of a car this tick (false: unknown / stale). rtMs: the authority's clock time (ms, its unwrapped
    // wire clock) that this target stands for (render time + the lead): the truth record TRS of that time is where the
    // authority's car was then.
    bool Target(uint32_t key, double now, CarState& out, double* rtMs = nullptr) const;
    // The copies (keys in order) whose report came at exactly `now` (this tick's).
    std::vector<uint32_t> ReportedAt(double now) const;

private:
    struct Car {
        uint8_t auth = 0;
        SpawnInfo info;
        bool haveInfo = false;
        double lastRecv = -1e9;
        bool copy = false;      // a copy exists (Add sent)
        bool released = false;  // handed over to player sync
        double despawnAt = -1;  // the authority despawned it: removed at this time
        net::motion::VehicleController vc;
        net::correction::Corrector fix;
        CopyReport rep;
        double repAt = -1e9;
        float lastErr = 0;
        float accDt = 0;
        int tick = 0;
        double bumpAt = -1e9;
        bool haveLast = false;
        Vec3 lastTarget;
        bool frozen = false;  // held at frozenPos / frozenRot (its authority is paused)
        Vec3 frozenPos;
        float frozenRot[4] = {0, 0, 0, 1};
        Car(const net::motion::VehicleTuning& v, const net::correction::Tuning& c) : vc(v), fix(c) {}
    };
    struct Auth {
        net::replica::Stream stream;
        int64_t clockMs = 0;  // its clock unwrapped from the 32-bit wire value
        uint32_t lastRawMs = 0;
        bool haveClock = false;
        bool paused = false;
        AuthorityStats stats;
        explicit Auth(const net::replica::StreamTuning& t) : stream(t) {}
    };
    bool Present(const Car& c, uint32_t key, double now, CarState& out, double* rtMs = nullptr) const;
    float DistToMe(const Car& c, uint32_t key) const;
    Auth& AuthOf(uint8_t authority);
    const Auth* FindAuth(uint8_t authority) const;
    Car& CarOf(uint32_t key);
    void Erase(uint32_t key);  // forgets a car and its track

    GuestConfig cfg_;
    net::replica::StreamTuning streamTuning_;
    net::motion::VehicleTuning vehicleTuning_;
    std::map<uint32_t, Car> cars_;  // by RefKey
    std::map<uint8_t, Auth> auths_;
    std::vector<uint32_t> pendingRemove_;  // copies of a car whose model changed (spawned again), of a gone authority
    Vec3 me_;
    bool haveMe_ = false;
    GuestStats stats_;
    double quietUntil_ = -1e9;   // no knock reports before this
    double lastTickAt_ = -1;     // our previous Tick (a gap: our own pause)
};
using GuestTraffic = ReceiverTraffic;  // the 0.15 name (the guest showed the host's traffic)

}  // namespace mc::game::traffic
