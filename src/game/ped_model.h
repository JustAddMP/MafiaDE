#pragma once
// Ambient pedestrians in co-op, the part without the game (docs/protocol.md "Pedestrians", docs/sync-architecture.md
// "ambient-peds"): the wire format of the pedestrian messages, the region authority's sender (which of its local
// pedestrians it simulates for the others, net ids, spawn / gone / death / packed states per receiving player on top of
// core's InterestManager) and the receiver (the authority's pedestrians on its clock through core's replica::Stream,
// which local human is the replica of which, adopted one-to-one nearest first or a spawned copy, which surplus local
// humans are hidden while nobody looks, and a LocomotionController per replica). Pure data in, decisions out: no Lua,
// no session, no OS calls, so tests/ped_test.cpp runs it without the game. game/ped_sync.cpp feeds it from the Lua
// glue (ped_sync.lua) and the session. Single-threaded (the caller locks).
//
// Every game runs both halves: it is the authority for the pedestrians where core's RegionAuthority says it simulates
// the ambient world (the host within 250 m of him, elsewhere the nearest player, with hysteresis), and a receiver of
// every other authority's pedestrians near its own player.
#include "net/authority.h"
#include "net/interest.h"
#include "net/remote_motion.h"
#include "net/replica.h"
#include "net/wire.h"

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace mc::game::ped {

using Vec3 = net::motion::Vec3;

// ---- protocol (0x0280-0x0287) ----------------------------------------------------------------------------------------
constexpr uint16_t kMsgStates = 0x0280;  // authority -> one player (SendTo; guest -> guest via the host), unreliable
constexpr uint16_t kMsgSpawn = 0x0281;   // authority -> one player, reliable: pedestrians he gets from now on
constexpr uint16_t kMsgGone = 0x0282;    // authority -> one player, reliable: pedestrians he no longer gets
constexpr uint16_t kMsgDeath = 0x0283;   // authority -> one player, reliable: one of them died
constexpr uint16_t kMsgKill = 0x0284;    // receiver -> the authority, reliable: its replica died here by our hand
// 0x0285-0x0287 reserved.

enum Flags : uint8_t {
    kDead = 1,     // IsDeath on the authority
    kPanic = 2,    // reacting to something (UNVERIFIED read: GetCurrentReactionPriority > 0)
    kCrouch = 4,   // IsCoverCrouch (a civilian's panic crouch, UNVERIFIED that it reads it)
    kRunning = 8,  // faster than a walk (the authority's measured speed)
};

struct PedState {
    uint16_t net = 0;
    Vec3 pos, vel;              // vel.z is not sent (a walker stays on the ground)
    float heading[2] = {0, 1};  // facing, unit vector in the ground plane
    uint8_t flags = 0;
    uint8_t gait = 0;  // 0 idle, 1 walk, 2 run, 3 sprint (from the measured speed, GaitOf)
};

struct PedSpawnInfo {
    uint16_t net = 0;
    uint8_t cls = 0;    // a class the receiver could pick a look by (0: unknown; nothing readable yet)
    uint32_t look = 0;  // the authority's GetCivilArchetype hashed to 32 bits (0: unknown); logged, not applied
    PedState st;        // where it is (full precision position)
};

// PedStates: u32 authMs (the authority's clock, the sample time), f32 origin x y z, u8 n, then n x 16 bytes:
// u16 net, i16 x y z (1/32 m from the origin, +-1023 m), i8 hx hy (x127), i16 vx vy (1/100 m/s), u8 flags, u8 gait.
constexpr size_t kStateBytes = 16;
constexpr size_t kStatesHeader = 17;
constexpr size_t kMaxPerPacket = 50;  // 17 + 50 x 16 = 817 bytes (unreliable limit 1100 with headers)
constexpr float kPosScale = 32.0f;
constexpr float kVelScale = 100.0f;
std::vector<std::vector<uint8_t>> PackStates(uint32_t authMs, const std::vector<PedState>& states,
                                             size_t maxPerPacket = kMaxPerPacket);
bool ReadStates(net::ByteReader& r, uint32_t& authMs, std::vector<PedState>& out);
PedState Quantised(const PedState& s);  // what a state looks like after a trip over the wire

// PedSpawn: u8 n (at most 24), n x 22 bytes: u16 net, u8 cls, u32 look, f32 x y z, i8 hx hy, u8 flags.
// PedGone: u8 n, n x u16 net.  PedDeath: u16 net, f32 x y z.  PedKill: u16 net.
constexpr size_t kMaxSpawnsPerMessage = 24;
std::vector<uint8_t> PackSpawns(const std::vector<PedSpawnInfo>& spawns);
bool ReadSpawns(net::ByteReader& r, std::vector<PedSpawnInfo>& out);
std::vector<uint8_t> PackGone(const std::vector<uint16_t>& nets);
bool ReadGone(net::ByteReader& r, std::vector<uint16_t>& out);
std::vector<uint8_t> PackDeath(uint16_t net, const Vec3& pos);
bool ReadDeath(net::ByteReader& r, uint16_t& net, Vec3& pos);
std::vector<uint8_t> PackKill(uint16_t net);
bool ReadKill(net::ByteReader& r, uint16_t& net);

// Gait from a pedestrian's measured ground speed (m/s), with hysteresis around the previous one.
uint8_t GaitOf(float speed, uint8_t prev);

// Is `p` in view of a camera at `cam` looking along `dir` (unit, 3D)? Within `nearDist` always, beyond `farDist`
// never, else inside a cone of `halfAngleDeg` around dir. Conservative: no occlusion test (a human behind a wall in
// front of the camera counts as seen).
bool InView(const Vec3& cam, const Vec3& dir, const Vec3& p, float halfAngleDeg = 60, float nearDist = 4,
            float farDist = 150);

// The correction rules of core's Corrector preset for NPCs (net/correction.h Tuning::ForNpc: 0.5 m dead zone, a
// blend capped at 35 % of the target's speed or 0.15 m/s standing, snap beyond 5 m / 3 m up, stuck 0.8 m for 3 s)
// as a LocomotionController tuning (its nudge implements the same blend rule).
net::motion::LocomotionTuning LocomotionForPed();

// ---- the authority ----------------------------------------------------------------------------------------------------
struct LocalPed {
    uint32_t id = 0;  // its entity id on this game
    Vec3 pos;
    float heading[2] = {0, 1};
    uint8_t flags = 0;  // kDead / kPanic / kCrouch as read (kRunning and the gait come from the measured speed)
    uint32_t look = 0;
};

struct PlayerView {
    uint8_t id = 0;
    Vec3 pos;  // his position (his car's while he drives)
    bool inWorld = true;
};

struct AuthorityConfig {
    float enterRadius = 50, leaveRadius = 60;  // m from a receiving player: his pedestrians
    float nearDistance = 15;                   // m: up to here every nearInterval ...
    double nearInterval = 0.1;                 // s (10 Hz)
    double farInterval = 0.33;                 // s at enterRadius (3 Hz; linear in between)
    size_t maxPerPeer = 40;                    // pedestrians one player gets at most (nearest first)
    uint32_t bytesPerSecond = 16 * 1024;       // state budget per receiving player (interest manager)
    double refresh = 1.0;                      // s: an unchanged state is sent again after this long anyway
    size_t maxPerPacket = kMaxPerPacket;
    net::authority::RegionTuning region;  // who simulates the ambient world where (core defaults)
};

struct PeerOut {
    uint8_t peer = 0;
    std::vector<std::vector<uint8_t>> spawns, gones, deaths, states;
    uint32_t spawned = 0, gone = 0, sent = 0, skipped = 0;
};

struct AuthorityPeerStats {
    uint32_t relevant = 0, admitted = 0;
    uint64_t spawned = 0, gone = 0, states = 0, skipped = 0, deaths = 0, bytes = 0;
};

// What one sent state was, for the truth trace record (PDT).
struct Truth {
    uint16_t net = 0;
    uint32_t localId = 0;
    Vec3 pos;
};

class PedAuthority {
public:
    explicit PedAuthority(const AuthorityConfig& cfg = {});
    // Every player in the session with where he is (us too); `me` is our id. Call before SetPeds each round.
    void SetPlayers(const std::vector<PlayerView>& players, uint8_t host, uint8_t me);
    // This round's ambient humans of our game near any player (a human missing from the list is gone). `now` (s, our
    // clock) gives the measured velocity. Each one's owner is decided here: RegionAuthority::Keep(its last owner, pos).
    void SetPeds(const std::vector<LocalPed>& peds, double now);
    // Spawn / gone / death / state payloads per receiving player, for this round. authMs = our clock at the sample.
    void Tick(double now, uint32_t authMs, std::vector<PeerOut>& out, std::vector<Truth>* truths = nullptr);
    void RemovePeer(uint8_t peer);  // left: nothing is sent to him any more (a rejoin starts anew)

    // Who simulates local human `id` now (its region authority, kept with hysteresis); kNoPlayer: not known.
    uint8_t OwnerOf(uint32_t id) const;
    // The local humans whose owner is another player (their region is not ours): candidates for adoption / hiding.
    std::vector<LocalPed> Foreign() const;
    size_t OwnedCount() const;  // humans we are the authority for
    size_t PedCount() const { return peds_.size(); }
    uint32_t IdOf(uint16_t net) const;  // 0: unknown
    uint16_t NetOf(uint32_t id) const;  // 0: not sent
    const AuthorityPeerStats* Stats(uint8_t peer) const;
    const net::InterestManager& Interest() const { return im_; }
    const net::authority::RegionAuthority& Region() const { return region_; }
    uint8_t Me() const { return me_; }

private:
    struct Ped {
        LocalPed p;
        uint8_t owner = net::kNoPlayer;
        uint16_t net = 0;  // 0: not registered in the interest manager (not ours)
        Vec3 vel;
        bool haveLast = false;
        Vec3 lastPos;
        double lastAt = 0;
        uint8_t gait = 0;
        bool deadSent = false;
    };
    struct Sent {
        PedState q;
        double at = -1e9;
    };
    struct Peer {
        Vec3 view;
        bool hasView = false;
        std::set<uint16_t> relevant;
        std::map<uint16_t, Sent> admitted;
        AuthorityPeerStats stats;
    };
    uint16_t NewNet();
    PedState StateOf(const Ped& p) const;
    float Dist(const Peer& peer, uint16_t net) const;

    AuthorityConfig cfg_;
    net::InterestManager im_;
    net::authority::RegionAuthority region_;
    std::map<uint32_t, Ped> peds_;        // by local id
    std::map<uint16_t, uint32_t> byNet_;  // net -> local id (registered = ours)
    std::map<uint8_t, Peer> peers_;
    uint8_t me_ = 0, host_ = 0;
    uint16_t nextNet_ = 1;
};

// ---- the receiver -----------------------------------------------------------------------------------------------------
struct ReceiverConfig {
    double stale = 3.0;          // s without a state: the replica goes (the pedestrian is kept a while longer)
    double forget = 10.0;        // s without a state: forgotten
    float zoneRadius = 50;       // m from us: local humans of another's region here are adopted or hidden
    float coverRadius = 40;      // m from us: ped.coverage counts these
    float spawnRadius = 45;      // m from us: an authority pedestrian without a replica here gets a spawned copy
    float adoptNear = 3.0f;      // m: a local human this near an unmatched authority pedestrian is adopted (in sight)
    float adoptFar = 45.0f;      // m: ... this near when neither is in sight (it is put there unseen)
    double spawnWait = 0.8;      // s an authority pedestrian stays unmatched before a copy is spawned (out of sight)
    double spawnWaitSeen = 2.5;  // ... when its spot is in sight (a copy appearing in view)
    size_t maxSpawned = 10;      // spawned copies at once (loading included)
    size_t maxLoading = 2;       // spawn requests outstanding at once
    double spawnRetry = 5.0;     // s after a failed spawn before that pedestrian is tried again
    double unspawnGrace = 3.0;   // s a spawned copy of a gone pedestrian may wait for nobody looking
    double showAfter = 2.0;      // s a hidden human's spot must be ours (our region) before it is shown again
    float showFar = 150;         // m: a hidden human this far from us is shown again (the game may clean it up)
    double reportFresh = 0.3;    // s: an older replica report is not used
    float viewHalfAngle = 60;    // deg: InView
    net::replica::StreamTuning stream;
};

struct CopyReport {
    Vec3 pos;
    float heading[2] = {0, 1};
    bool dead = false;
};

struct Camera {
    Vec3 pos, dir;
    bool valid = false;
};

enum class How : uint8_t { None, Adopted, Spawned };

struct RecvCmd {
    enum class Type : uint8_t {
        Adopt,    // local human `local` is the replica of `key` from now on
        Spawn,    // spawn a copy for `key` at target
        Release,  // `key`'s replica is released (adopted: given back to its own AI; spawned: removed)
        Hide,     // local human `local` is hidden (surplus, not in sight)
        Show,     // local human `local` is shown again
        Drive,    // drive `key`'s replica: lc, target
        Kill,     // `key`'s replica dies (the authority's died)
    };
    Type type = Type::Drive;
    uint32_t key = 0;     // (authority << 16) | net
    uint32_t local = 0;   // local entity id (Adopt / Hide / Show; Release of an adopted one)
    How how = How::None;  // Release: what the replica was
    PedState target;      // Spawn / Drive: where the authority's pedestrian is now (Present)
    net::motion::LocomotionCommand lc;
};

struct ReceiverStats {
    uint32_t known = 0;        // authority pedestrians (not stale)
    uint32_t nearby = 0;       // within coverRadius of us
    uint32_t nearReplica = 0;  // ... with a replica that stands for it now
    uint32_t adopted = 0, spawned = 0, loading = 0, hidden = 0;
    uint64_t states = 0, spawns = 0, gones = 0, deaths = 0, dropsOld = 0;
    uint64_t adoptions = 0, spawnsAsked = 0, spawnFails = 0, hides = 0, shows = 0, releases = 0, snaps = 0, kills = 0;
};

// What a replica looked like this tick (traces: PDR).
struct ReplicaInfo {
    uint32_t key = 0;
    How how = How::None;
    uint32_t local = 0;
    double targetT = 0;  // the authority clock time its target stands for (render time + lead), s
    PedState target;
    CopyReport copy;
    bool haveCopy = false;
    float err = 0;
};

class PedReceiver {
public:
    explicit PedReceiver(const ReceiverConfig& cfg = {});
    static uint32_t Key(uint8_t auth, uint16_t net) { return (static_cast<uint32_t>(auth) << 16) | net; }

    // Messages of authority `auth` (any player but us).
    void OnSpawn(uint8_t auth, const PedSpawnInfo& s, double now);
    void OnGone(uint8_t auth, uint16_t net, double now);
    void OnStates(uint8_t auth, uint32_t authMs, const std::vector<PedState>& states, double now);
    void OnDeath(uint8_t auth, uint16_t net, const Vec3& pos, double now);
    void ForgetAuthority(uint8_t auth);  // he left / reconnected: every replica of his goes

    // Our player, the camera, and who simulates the ambient world where (a copy of our authority's region; `me` our
    // id): hidden humans come back where it is ours again.
    void SetLocal(const Vec3& me, const Camera& cam);
    void SetRegion(const net::authority::RegionAuthority& region, uint8_t me);
    // Local ambient humans whose owner is another player (PedAuthority::Foreign): free ones can be adopted or hidden.
    void SetCandidates(const std::vector<LocalPed>& foreign);
    // Where a replica is (after this tick's commands); dead: it died here.
    void SetReport(uint32_t key, const CopyReport& r, double now);
    void Spawned(uint32_t key, uint32_t local);  // the spawned copy of key is in the world now (local id)
    void SpawnFailed(uint32_t key, double now);
    void LocalGone(uint32_t local);  // an adopted / hidden / spawned local human is gone (the game)
    // Our replica of `key` died here while the authority's lives: true once per replica (send PedKill).
    bool DiedHere(uint32_t key);
    void Clear();  // world change / session end: everything forgotten (the game removed or reset it)
    // Every hidden human is shown again (session end): Show commands; then nothing is hidden.
    void ShowAll(std::vector<RecvCmd>& out);

    void Tick(double now, float dt, std::vector<RecvCmd>& out);
    const ReceiverStats& Stats() const { return stats_; }
    std::vector<ReplicaInfo> Replicas(double now) const;
    bool IsHidden(uint32_t local) const { return hidden_.count(local) != 0; }
    double PlayoutDelay(uint8_t auth) const;

private:
    struct Remote {
        uint8_t auth = 0;
        uint16_t net = 0;
        PedSpawnInfo info;
        bool haveInfo = false;
        double lastRecv = -1e9;
        double firstSeen = 0;
        double unmatchedSince = -1;
        bool dead = false, killed = false;
        // replica
        How how = How::None;
        uint32_t local = 0;  // adopted / spawned local id (spawned: 0 while loading)
        bool loading = false;
        double spawnAskedAt = -1e9, retryAt = -1e9;
        bool diedHere = false, diedHereTold = false;
        net::motion::LocomotionController loco{LocomotionForPed()};
        CopyReport rep;
        double repAt = -1e9;
        bool haveRep = false;
        double goneAt = -1;  // authority said gone (or stale): the replica is released / unspawned
        float lastErr = 0;
        PedState lastTarget;
        double lastTargetT = 0;
    };
    struct Auth {
        net::replica::Stream stream;
        int64_t clockMs = 0;
        uint32_t lastRawMs = 0;
        bool haveClock = false;
    };
    struct Hidden {
        Vec3 pos;
        double oursSince = -1;
    };
    Auth& AuthOf(uint8_t auth);
    bool Target(const Remote& r, double now, PedState& out, double* t = nullptr) const;
    bool Seen(const Vec3& p) const;
    float DistToMe(const Vec3& p) const;
    void ReleaseReplica(Remote& r, std::vector<RecvCmd>& out);

    ReceiverConfig cfg_;
    std::map<uint32_t, Remote> remotes_;
    std::map<uint8_t, Auth> auths_;
    std::map<uint32_t, LocalPed> cands_;       // free candidates this round (not bound, not hidden)
    std::map<uint32_t, uint32_t> boundLocal_;  // local id -> key (adopted or spawned)
    std::map<uint32_t, Hidden> hidden_;
    Vec3 me_;
    bool haveMe_ = false;
    Camera cam_;
    net::authority::RegionAuthority region_;
    bool haveRegion_ = false;
    uint8_t myId_ = 0;
    ReceiverStats stats_;
};

// One-to-one matching of authority pedestrians to free local humans (exposed for the tests): pairs (remote index,
// local index) by distance, nearest first, each used at most once; a pair is allowed when within `nearDist`, or within
// `farDist` when neither is in sight (remoteSeen / localSeen false). Ties by index (deterministic).
struct MatchPair {
    size_t remote = 0, local = 0;
    float dist = 0;
};
std::vector<MatchPair> MatchNearest(const std::vector<Vec3>& remotes, const std::vector<bool>& remoteSeen,
                                    const std::vector<Vec3>& locals, const std::vector<bool>& localSeen, float nearDist,
                                    float farDist);

}  // namespace mc::game::ped
