#pragma once
// Traffic sync, the part without the game (docs/protocol.md "Traffic", docs/host-authority.md): the host's game owns
// all ambient traffic and police cars. The host's AuthorityTraffic is his per-guest bookkeeping (which of his traffic
// cars each guest gets, net ids, spawn / despawn / state packing on top of core's InterestManager, the driver watch);
// a guest's ReceiverTraffic is his table of the host's cars (core replica::Stream, the target of each copy, which cars
// get a copy, a VehicleController + core Corrector per copy). A guest's game runs no authority (it scans and sends no
// cars of its own: its own spawning is off, game/guest_world) and the host's game shows no copies (nobody else's
// traffic exists). Route() decides which traffic message a game reads; the guest -> guest relay and the region
// authority of 0.16 are gone (0x0224-0x0227 retired). Pure data in, decisions out: no Lua, no session, no OS calls, so
// tests/traffic_test.cpp runs it without the game. game/traffic_sync.cpp feeds it from the Lua glue
// (traffic_sync.lua) and the session. Single-threaded (the caller locks).
#include "net/correction.h"
#include "net/interest.h"
#include "net/playout.h"
#include "net/remote_motion.h"
#include "net/replica.h"
#include "net/session.h"
#include "net/wire.h"

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace mc::game::traffic {

// ---- protocol (0x0220-0x022F) --------------------------------------------------------------------------------------
// The host's traffic, as in 0.15 (the only traffic there is):
constexpr uint16_t kMsgSpawn = 0x0220;    // host -> one guest, reliable: cars he gets copies of (model, kind, look)
constexpr uint16_t kMsgDespawn = 0x0221;  // host -> one guest, reliable: cars he no longer gets
constexpr uint16_t kMsgStates = 0x0222;   // host -> one guest, unreliable: packed car states
constexpr uint16_t kMsgBump = 0x0223;     // a guest -> host, reliable: his copy of a host car was knocked off (logged)
// Retired with the region authority (docs/host-authority.md): TrafficSpawn2 / Despawn2 / States2 / Bump2, a guest's
// own traffic with its id in front, guest -> guest through core's relay. Neither sent nor read; the ids stay reserved
// and are never reused.
constexpr uint16_t kRetiredFirst = 0x0224;
constexpr uint16_t kRetiredLast = 0x0227;

// Which traffic message a game reads, and as what (traffic_sync.cpp registers 0x0220-0x0223 and asks this for each):
// a guest reads TrafficSpawn / Despawn / States from the host only; the host reads TrafficBump from a guest only;
// everything else (the host's own messages back, a guest's traffic, another guest's bump, the retired ids) is ignored.
enum class Inbound : uint8_t { Ignore, Spawn, Despawn, States, Bump };
Inbound Route(uint16_t id, bool weHost, net::PlayerId from);
// Who runs which half: the host's game is the only authority (scans its traffic, sends it to each guest); a guest's game
// only shows the host's cars (no scan, nothing sent but the bump reports of its copies).
inline bool RunsAuthority(bool isHost) { return isHost; }
inline bool ShowsCopies(bool isHost) { return !isHost; }

enum Kind : uint8_t { kTraffic = 0, kPolice = 1 };
enum StateFlags : uint8_t {
    kHandbrake = 1,   // handbrake > 0.5
    kSiren = 2,       // police: siren and beacons on (the host's heuristic, see traffic_sync.lua)
    kNoDriver = 4,    // the driver is dead or gone, confirmed (DriverWatch: missing for kOrphanConfirm s while slow)
    kControls = 8,    // steer / power / brake are the driver's (else unknown: 0)
    kReverse = 16,    // moving backwards (power then means reverse: not applied on the copy)
};
// Never on the wire: the receiver's Drive command to its Lua sets it while the copy's crew is out (CrewGetsOut, then
// held until the host's car is driven again).
constexpr uint8_t kCrewOut = 32;
// Never on the wire: the host's Lua (TC) sets it for a car without a driver read that is farther than
// kDriverLod from his player - the game streams traffic drivers out there while their cars drive on (DriverWatch).
// A guest's receiver sets it in the Drive command to its Lua for a copy farther than kDriverLod from our player: the
// game streams the copy's crew out there just the same, so a crew member out of his seat is not put back.
constexpr uint8_t kDriverFar = 64;

// A traffic car is known by the host's net id everywhere (the receiver's key, the Lua's copy key, car damage, car look,
// explosions): one authority, so no authority part (0.16's authority << 16 | net is gone; for the host's cars it was
// the net id already).

// ---- drivers ------------------------------------------------------------------------------------------------------
// The host's Lua reports each round whether it read a living traffic driver at the wheel (TC flag 4 = "none read
// now"). That read flickers: in the 0.15.0 two-PC test 118 of 179 cars flagged on the first miss were still moving
// at 2-15 m/s and 84 kept their speed 4 s later (someone was driving), mostly 50-100 m from the host's player
// (docs/game-api.md 19). So a car only counts as driverless (kNoDriver sent) when no driver was read for
// kOrphanConfirm s while it is slow; it is driven again (flag cleared) when a driver is read or it speeds up.
constexpr double kOrphanConfirm = 1.0;  // s without a driver read, slow all that time
constexpr float kOrphanSlow = 2.0f;     // m/s: slow
constexpr float kOrphanDriven = 4.0f;   // m/s: a declared driverless car this fast is driven again (someone drives it)
constexpr double kOrphanGap = 0.5;      // s between two rounds (normally 0.05): the game stood, the watch restarts
// The game's level of detail for traffic drivers (2026-10-20 two-PC test, both authorities): every "seat 0 empty" read
// of a car was 45-169 m from the authority's player (A: 28 misses 45-76 m but one at 16 m; B: 33 misses 49-169 m), 14 of 28
// while the car drove on at 10-14 m/s, and the driver was read again when the player came back within 44-51 m: the game
// streams the human out of a traffic car beyond ~50 m and its traffic system drives the car on without him. Such a car
// is not driverless: beyond kDriverLod a missing driver is "not streamed" (kDriverFar), not "gone".
constexpr float kDriverLod = 40.0f;     // m from the authority's player

enum class DriverRead : uint8_t {
    Seen,     // a living driver at the wheel
    Missing,  // none, within kDriverLod of our player: he got out, died or was dragged out
    Far,      // none, beyond kDriverLod: streamed out by the game (its car is still driven)
};

class DriverWatch {
public:
    // One round: was a living driver read at the wheel now, how fast the car goes (m/s). Returns Declared().
    // A Far read keeps a car that was declared driverless declared (its driver got out near our player, then we went
    // away) and is a driven car otherwise.
    bool Update(double now, DriverRead read, float speed);
    bool Update(double now, bool driverSeen, float speed) {
        return Update(now, driverSeen ? DriverRead::Seen : DriverRead::Missing, speed);
    }
    bool Declared() const { return declared_; }
    bool Missing() const { return missSince_ >= 0; }
    bool Far() const { return far_; }  // the last read was Far (the game streamed the driver out)
    double MissingFor(double now) const { return missSince_ >= 0 ? now - missSince_ : 0; }

private:
    double missSince_ = -1;  // first round without a driver read (-1: one was read last round)
    double slowSince_ = -1;  // first round slow while missing (-1: not slow)
    double lastAt_ = -1;     // the last round (kOrphanGap)
    bool declared_ = false;
    bool far_ = false;
};

// Receiver: may the crew of a copy get out now? Only for a car confirmed driverless (kNoDriver) that stands or rolls
// slower than kOrphanSlow, and whose copy here is that slow too (copySpeed < 0: no report of the copy yet). A moving
// copy always keeps its crew seated (0.15.0: 249 "lost its driver" get-outs left empty copies driving on and their
// drivers standing in the road). Once out, the crew stays out until the authority's car is driven again (kNoDriver
// gone): ReceiverTraffic holds it (2026-10-20: 19 of 40 get-outs were followed by "back in" within 2 s because the
// copy's speed went over 2 m/s and back).
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

// TrafficStates: u32 ms (the host's clock), f32 origin x y z, u8 n, then n x 22 bytes: u16 net, i16 x y z (1/32 m
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

// ---- authority (the host: his traffic -> each guest) -----------------------------------------------------------------
struct HostCar {
    uint32_t key = 0;  // its entity id on this game
    uint8_t kind = kTraffic;
    std::string model;
    int16_t color[2] = {-1, -1};
    std::string plate;
    CarState st;  // net is filled in by AuthorityTraffic; kNoDriver here = no driver read this round (DriverWatch),
                  // with kDriverFar: beyond kDriverLod of our player (streamed out); neither is sent as such
};

struct HostConfig {
    float enterRadius = 260, leaveRadius = 320;  // m from the other player
    size_t maxPerPeer = 24;                      // copies one player gets at most (nearest first)
    float evictFar = 150, evictNear = 90;        // a car beyond evictFar gives way to one within evictNear
    uint32_t bytesPerSecond = 40 * 1024;         // state budget per player (interest manager)
    double refresh = 1.5;                        // s: an unchanged state is sent again after this long anyway
    double viewTimeout = 2.0;                    // s without a viewpoint: the player sees nothing
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

class AuthorityTraffic {
public:
    explicit AuthorityTraffic(const HostConfig& cfg = {});
    // This round's traffic cars near any other player (a car missing from the list is gone: despawned everywhere).
    // `now` (s) times each car's DriverWatch: its kNoDriver read becomes the confirmed flag that is sent.
    void SetCars(const std::vector<HostCar>& cars, double now = 0);
    // Cars that went driverless (confirmed) / were found driven again since the start (logs and traces).
    uint64_t Orphaned() const { return orphaned_; }
    uint64_t DrivenAgain() const { return drivenAgain_; }
    size_t MissingNow() const;   // cars without a driver read now (within kDriverLod of our player)
    size_t DeclaredNow() const;  // of those, confirmed driverless
    size_t FarNow() const;       // cars whose driver the game streamed out (beyond kDriverLod): driven, not missing
    // A guest's position (his puppet, or his car, on the host's game); one seen for the first time is added.
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
private:
    struct Car {
        HostCar c;
        bool seen = false;
        DriverWatch drv;
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
using HostTraffic = AuthorityTraffic;  // the name of 0.15

// ---- receiver (a guest: the host's cars near him) --------------------------------------------------------------------
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
    size_t maxCopies = 20;          // copies on this game at most
    float addRadius = 240;          // m from us: a car gets a copy
    float dropRadius = 290;         // m: its copy goes
    float viewRadius = 150;         // m: a car this near counts for "shown" (Stats: due / shown)
    float nearDrive = 90;           // m: copies farther than this are driven every farDriveEvery ticks
    int farDriveEvery = 3;
    double reportFresh = 0.3;       // s: an older copy report is not used
    float bumpError = 3.0f;         // m: a copy this far off its target that was on it a moment before was knocked
    double bumpEvery = 2.0;         // s between bump reports of one car
    double despawnGrace = 1.0;      // s a copy stays after its despawn (player sync may adopt it: a car jacked)
    // the host's pause (game/peer_pause.h): no knock checks while it is paused and this long after its resume,
    // and after a pause of our own (a tick gap of more than localGap s: our copies were left behind meanwhile)
    double quietAfter = 1.0;
    double localGap = 0.5;
    net::correction::Tuning correction = TrafficCorrection();  // the copy's position correction
    // The game's own sideways move of a copy between two of our commands (its traffic system holds a car on its own
    // line; the 0.17.0 one-PC run: 0.03-0.07 m put back every tick, copies held 1.05 m across their car): learned
    // from each report (CopyReport::pre against the previous report, across the copy's heading) with this share per
    // report, and added against it to the next command, at most sidePullMax m, while the host's car moves at least
    // sidePullSpeed m/s
    float sidePullAlpha = 0.3f;
    float sidePullMax = 0.3f;
    float sidePullSpeed = 1.0f;
};

struct CopyReport {
    Vec3 pos, vel;
    float rot[4] = {0, 0, 0, 1};
    // traces only: where the copy was before this tick's commands (the game's own placement since the last tick; pos
    // is after our SetPos / nudge)
    Vec3 pre;
    bool havePre = false;
};

struct GuestCmd {
    enum class Type : uint8_t { Add, Remove, Drive };
    Type type = Type::Drive;
    uint32_t key = 0;  // the host's net id
    // Add
    const SpawnInfo* info = nullptr;  // valid until the next call into ReceiverTraffic
    // Add / Drive: where the host's car is now
    CarState target;
    // Drive
    net::motion::VehicleCommand vc;
    // Drive: the velocity the copy is given after this command (SetSpeed takes one number: the speed along the copy's
    // own heading, `reverse` backwards): the target's speed plus the error along the copy's heading (CopySpeed), not
    // vc.vel's length (a vector toward the target: given as a speed along the heading it drove a copy that was past a
    // standing target away from it at up to 4 m/s, 2026-10-20 two-PC test).
    Vec3 vel;
    bool reverse = false;
    bool grace = false;  // Remove: after despawnGrace (the host no longer sends it), not at once
};

// The speed (m/s, >= 0) a traffic copy is given along its heading: the target's speed plus the error along the copy's
// heading (in the direction it drives: backwards when reversing) x velGain, that part at most maxVelCorr; never
// negative (a copy past a stopping target stops, the position blend takes the rest). copyFwd: the copy's heading
// (unit, ground plane); reverse: the host's car reverses.
float CopySpeed(const Vec3& copy, const Vec3& target, float targetSpeed, const float copyFwd[2], bool reverse,
                const net::motion::VehicleTuning& t);

struct GuestBump {
    uint32_t key = 0;
    float error = 0, copySpeed = 0, targetSpeed = 0;
};

struct GuestStats {
    uint32_t known = 0, live = 0, copies = 0, pins = 0, snaps = 0, bumps = 0;
    uint32_t quietBumps = 0;    // knocks not reported: the host was paused or had just resumed, or we had
    uint32_t pauses = 0, localGaps = 0;
    bool frozen = false;        // the host is paused: his cars' copies are held
    float avgErr = 0, maxErr = 0;
    uint64_t states = 0, spawns = 0, despawns = 0, dropsOld = 0;
    uint32_t due = 0, shown = 0;  // host cars within viewRadius (at most maxCopies) / with a copy here
    uint64_t blends = 0, fixSnaps = 0;  // corrector: blended / snapped updates so far
    uint64_t sidePulls = 0;  // commands with the game's sideways pull taken out (GuestConfig::sidePull*)
};

class ReceiverTraffic {
public:
    explicit ReceiverTraffic(const GuestConfig& cfg = {});
    // The host's messages (the caller has checked the sender: Route). ms: the host's clock (its 32-bit wire value).
    void OnSpawn(const SpawnInfo& s, double now);
    void OnDespawn(uint16_t net, double now);
    void OnStates(uint32_t ms, const std::vector<CarState>& states, double now);
    // Our player's position (haveMe false: not in the world: no copies).
    void SetLocal(const Vec3& me, bool haveMe = true);
    void SetReport(uint32_t key, const CopyReport& r, double now);  // where our copy is (after this tick's commands)
    void Release(uint32_t key);  // the copy is someone's car now (player sync took it): never driven or removed by us
    void Gone(uint32_t key);     // the copy is gone on our game (despawned by the game, world change): spawn anew
    void Clear();                // world change / session end: everything forgotten (the game removed the copies)
    // The host paused his game (true) or resumed it (false), game/peer_pause.h: while paused each copy is held where
    // it stands (Pin to that pose, no speed, braked), none goes stale or is spawned, no knock is reported; after the
    // resume they are driven again and knocks are not reported for quietAfter s.
    void SetHostPaused(bool paused, double now);
    bool HostPaused() const { return host_.paused; }
    // Commands for this tick: Remove first, then Add (nearest first), then Drive for the copies that are due.
    void Tick(double now, float dt, std::vector<GuestCmd>& out, std::vector<GuestBump>& bumps);
    double PlayoutDelay() const;  // s the copies are shown behind the host's states
    const GuestStats& Stats() const { return stats_; }
    // For traces: the copy of `key` (false if none) and its last error.
    bool CopyInfo(uint32_t key, float& err, bool& pinned, int& pins) const;
    // The target of a car this tick (false: unknown / stale). rtMs: the host's clock time (ms, his unwrapped wire
    // clock) that this target stands for (render time + the lead): the truth record TRS of that time is where the
    // host's car was then.
    bool Target(uint32_t key, double now, CarState& out, double* rtMs = nullptr) const;
    // The copies (keys in order) whose report came at exactly `now` (this tick's).
    std::vector<uint32_t> ReportedAt(double now) const;

private:
    struct Car {
        SpawnInfo info;
        bool haveInfo = false;
        double lastRecv = -1e9;
        bool copy = false;      // a copy exists (Add sent)
        bool released = false;  // handed over to player sync
        double despawnAt = -1;  // the host despawned it: removed at this time
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
        bool crewOut = false;  // the copy's crew is out (CrewGetsOut), until the host's car is driven again
        bool frozen = false;  // held at frozenPos / frozenRot (the host is paused)
        Vec3 frozenPos;
        float frozenRot[4] = {0, 0, 0, 1};
        Vec3 sidePull;          // the game's sideways move of the copy per command, learned (SetReport)
        Vec3 pullFrom;          // where the last report had the copy (after our commands)
        bool havePullFrom = false;
        Car(const net::motion::VehicleTuning& v, const net::correction::Tuning& c) : vc(v), fix(c) {}
    };
    struct Host {
        net::replica::Stream stream;
        int64_t clockMs = 0;  // his clock unwrapped from the 32-bit wire value
        uint32_t lastRawMs = 0;
        bool haveClock = false;
        bool paused = false;
        explicit Host(const net::replica::StreamTuning& t) : stream(t) {}
    };
    void ResetPull(Car& c);
    bool Present(const Car& c, uint32_t key, double now, CarState& out, double* rtMs = nullptr) const;
    float DistToMe(const Car& c, uint32_t key) const;
    Car& CarOf(uint32_t key);
    void Erase(uint32_t key);  // forgets a car and its track

    GuestConfig cfg_;
    net::replica::StreamTuning streamTuning_;
    net::motion::VehicleTuning vehicleTuning_;
    std::map<uint32_t, Car> cars_;  // by the host's net id
    Host host_;
    std::vector<uint32_t> pendingRemove_;  // copies of a car whose model changed (spawned again)
    Vec3 me_;
    bool haveMe_ = false;
    GuestStats stats_;
    double quietUntil_ = -1e9;   // no knock reports before this
    double lastTickAt_ = -1;     // our previous Tick (a gap: our own pause)
};
using GuestTraffic = ReceiverTraffic;  // the 0.15 name

}  // namespace mc::game::traffic
