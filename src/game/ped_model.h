#pragma once
// Ambient pedestrians in co-op, the part without the game (docs/protocol.md "Pedestrians", docs/host-authority.md
// WP-C "host-peds"): the wire format of the pedestrian messages, the host's sender (which of its ambient humans each
// guest gets: net ids, spawn / gone / death / packed states per guest on top of core's InterestManager) and a guest's
// receiver (the host's pedestrians on its clock through core's replica::Stream, each shown by a spawned copy from a
// pool, a LocomotionController per copy). Pure data in, decisions out: no Lua, no session, no OS calls, so
// tests/ped_test.cpp runs it without the game. game/ped_sync.cpp feeds it from the Lua glue (ped_sync.lua) and the
// session. Single-threaded (the caller locks).
//
// Host authority (docs/host-authority.md): the host's game owns every ambient pedestrian. Only the host runs the
// sender; a guest never sends pedestrians (his own pedestrian spawning is switched off by guest_world, WP-A) and shows
// the host's with copies only - nothing is adopted, nothing of his own is hidden by this module. 0.16.0's region
// authority (every game the authority for the pedestrians near it, guest -> guest relay, adoption of local humans,
// hiding the surplus) is gone.
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
constexpr uint16_t kMsgStates = 0x0280;  // host -> one guest (SendTo), unreliable
constexpr uint16_t kMsgSpawn = 0x0281;   // host -> one guest, reliable: pedestrians he gets from now on
constexpr uint16_t kMsgGone = 0x0282;    // host -> one guest, reliable: pedestrians he no longer gets
constexpr uint16_t kMsgDeath = 0x0283;   // host -> one guest, reliable: one of them died
constexpr uint16_t kMsgKill = 0x0284;    // guest -> the host, reliable: its copy died here by the guest's hand
// 0x0285-0x0287 reserved.

enum Flags : uint8_t {
    kDead = 1,     // IsDeath on the host
    kPanic = 2,    // reacting to something (UNVERIFIED read: GetCurrentReactionPriority > 0)
    kCrouch = 4,   // IsCoverCrouch (a civilian's panic crouch, UNVERIFIED that it reads it)
    kRunning = 8,  // faster than a walk (the host's measured speed)
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
    uint32_t look = 0;  // the host's GetCivilArchetype hashed to 32 bits (0: unknown); logged, not applied
    PedState st;        // where it is (full precision position)
};

// PedStates: u32 authMs (the host's clock, the sample time), f32 origin x y z, u8 n, then n x 16 bytes:
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

// The copies' looks (docs/host-authority.md D5): a rotation of six civilian spawn profiles, all in spawnprofiles.sds
// (docs/game-api.md 19 "Spawn profile names"; ped_sync.lua gives SetSpawnProfile the decimal mc_fnv hash of the name,
// as it did with the one profile of 0.16.0). The host's pedestrian's own look is not readable, so a copy's profile is
// picked by its net id (the same pedestrian gets the same look on every guest). UNVERIFIED in game that each of them
// loads through a runtime spawner and looks like a passer-by; a profile that fails twice before it ever made a copy
// is dropped from the rotation (PedReceiver). A pool copy keeps the look it was spawned with when it is reused.
constexpr size_t kCopyProfileCount = 6;
extern const char* const kCopyProfiles[kCopyProfileCount];

// ---- the host's sender ------------------------------------------------------------------------------------------------
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
    // Pedestrians within 80 m of a guest (leave at 90 m), at most 48 for him, nearest first (docs/host-authority.md
    // section 4; 0.16.0: 50/60 m and 40, when the guest still had his own pedestrians around). 48 x ~5 Hz x 16 bytes
    // is ~4 KB/s; the interest manager's budget keeps a guest under 16 KB/s whatever happens.
    float enterRadius = 80, leaveRadius = 90;  // m from a guest
    float nearDistance = 15;                   // m: up to here every nearInterval ...
    double nearInterval = 0.1;                 // s (10 Hz)
    double farInterval = 0.33;                 // s at enterRadius (3 Hz; linear in between)
    size_t maxPerPeer = 48;                    // pedestrians one guest gets at most (nearest first)
    uint32_t bytesPerSecond = 16 * 1024;       // state budget per guest (interest manager)
    double refresh = 1.0;                      // s: an unchanged state is sent again after this long anyway
    size_t maxPerPacket = kMaxPerPacket;
    // A human faster than any walk / run / sprint rides something (2026-10-20 two-PC test: four passengers of a tram,
    // 11-12 m/s for 6 s, were sent as pedestrians; their copies on the street fell 35-39 m behind, two of them died
    // under the guest's tram and his PedKill killed the passengers): not shared while it rides (gone for every
    // guest), shared again (a new net id) once it has been slower than this for rideHold.
    float rideSpeed = 8.0f;  // m/s, ground speed between two rounds (an AI sprint is ~6.5)
    int rideRounds = 2;      // rounds in a row this fast
    double rideHold = 3.0;   // s
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
    // Every player in the session with where he is (us too); `host` the host's id, `me` ours. Only the host shares
    // pedestrians: on a guest (me != host) nothing is ever registered or sent. Call before SetPeds each round.
    void SetPlayers(const std::vector<PlayerView>& players, uint8_t host, uint8_t me);
    // This round's ambient humans of our game near any player (a human missing from the list is gone). `now` (s, our
    // clock) gives the measured velocity.
    void SetPeds(const std::vector<LocalPed>& peds, double now);
    // Spawn / gone / death / state payloads per guest, for this round. authMs = our clock at the sample.
    void Tick(double now, uint32_t authMs, std::vector<PeerOut>& out, std::vector<Truth>* truths = nullptr);
    void RemovePeer(uint8_t peer);  // left: nothing is sent to him any more (a rejoin starts anew)

    bool Shares() const { return me_ == host_; }  // we are the host
    size_t OwnedCount() const;                    // humans we share now (with a net id)
    size_t RidingCount() const;                   // humans riding something now (not shared)
    uint64_t Rides() const { return rides_; }    // times a human was found riding (total)
    size_t PedCount() const { return peds_.size(); }
    uint32_t IdOf(uint16_t net) const;  // 0: unknown
    uint16_t NetOf(uint32_t id) const;  // 0: not sent
    const AuthorityPeerStats* Stats(uint8_t peer) const;
    const net::InterestManager& Interest() const { return im_; }
    uint8_t Me() const { return me_; }

private:
    struct Ped {
        LocalPed p;
        uint16_t net = 0;  // 0: not registered in the interest manager (not shared)
        Vec3 vel;
        bool haveLast = false;
        Vec3 lastPos;
        double lastAt = 0;
        uint8_t gait = 0;
        int fastRounds = 0;       // rounds in a row faster than rideSpeed
        double rideUntil = -1e9;  // riding (not shared) until then
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
    std::map<uint32_t, Ped> peds_;        // by local id
    std::map<uint16_t, uint32_t> byNet_;  // net -> local id (registered = shared)
    std::map<uint8_t, Peer> peers_;
    uint8_t me_ = 0, host_ = 0;
    uint16_t nextNet_ = 1;
    double now_ = 0;
    uint64_t rides_ = 0;
};

// ---- a guest's receiver -----------------------------------------------------------------------------------------------
struct ReceiverConfig {
    double stale = 3.0;         // s without a state: the copy goes (the pedestrian is kept a while longer)
    double forget = 10.0;       // s without a state: forgotten
    float coverRadius = 40;     // m from us: ped.coverage counts these
    float spawnRadius = 95;     // m from us: every host pedestrian this near gets a copy (the host sends within 90)
    double spawnWait = 0.2;     // s a pedestrian is known before its copy is asked for (one that only flickers in)
    // The copy pool (docs/host-authority.md WP-C): at most maxCopies spawned copies at once (bound to a pedestrian,
    // loading, or free). A copy whose pedestrian left our interest or went is hidden and kept (at most poolKeep free
    // ones); the next new pedestrian gets a free copy (put at its spot and shown: no profile load, no CreateObject)
    // before a new one is spawned. 0.16.0: 10 copies (24 in feat/sync-peds), removed when their pedestrian went.
    size_t maxCopies = 40;
    size_t poolKeep = 40;       // free copies kept hidden (0: a copy whose pedestrian went is removed, as in 0.16)
    // Spawn requests outstanding at once. Many (0.16.0: 2-4): the Lua loads one profile at a time and makes every
    // queued request of the loaded profile before it loads the next one, so a long queue costs one load per look,
    // not one per copy (the replay of session A: 4 outstanding -> 31 loads for 37 copies, coverage 48 %).
    size_t maxLoading = 40;
    double spawnTimeout = 30.0;  // s: a request that never came back (a long queue: 6 loads + 40 ticks)
    double spawnRetry = 5.0;    // s after a failed spawn before that pedestrian is tried again
    double resetRetry = 0.3;    // ... after a failure because the spawner was reset (the new one is ready in ~3 s)
    double profileRetry = 0.3;  // ... after its profile did not load (the rotation may give it another one)
    // With every copy in use: a pedestrian without a copy takes the copy of the farthest one that is more than
    // handoverMargin farther from us (beyond coverRadius, its copy out of sight): the near street first.
    float handoverMargin = 15;  // m
    // The copy spawner is broken (2026-10-20 test, session A: the guest's traffic sync hid it as its own traffic car;
    // its 10 copies went with it and every CreateObject after that failed - 139 failures in 30 s, coverage 18 %, until
    // the Lua's 30 s idle rule replaced it): a new one is asked for (ResetSpawner) after this many CreateObject failures
    // in a row, or when at least massLoss copies were gone within massLossWindow and none is left. Never while a copy
    // lives (the spawner's despawn takes its copies along).
    int spawnerFails = 2;
    int massLoss = 3;
    double massLossWindow = 0.5;   // s
    double spawnerResetGap = 5.0;  // s: at most one reset this often
    // Profiles: one that failed to load profileFails times (failures within 1 s count once: the Lua fails every
    // request of that profile at once) before it ever made a copy leaves the rotation; the last one never does.
    int profileFails = 2;
    double profileRest = 60.0;  // s a look that failed once (and never worked) is not given out before its 2nd try
    // A copy that does not close its gap (2026-10-20 test, session A: one spawned copy walked 3.2 m behind its
    // target at exactly its speed for 14 s, 118 of the 121 samples beyond 3 m; neither the multiplier nor the nudges
    // moved it): more than lagDist behind a moving target (>= lagMinSpeed) without getting lagClose nearer for
    // lagTime: put at its target (a snap). Lagging like that again within lagRespawn after such a snap: the copy
    // does not answer at all, it is removed (not kept in the pool) and the pedestrian gets another one.
    float lagDist = 1.5f;      // m
    float lagClose = 0.3f;     // m
    float lagMinSpeed = 0.5f;  // m/s
    double lagTime = 2.0;      // s
    double lagRespawn = 6.0;   // s
    float freshSnap = 1.0f;    // m: a copy this far off at its first report after it came is put at its target
    double hideGrace = 3.0;    // s a free copy may stay shown waiting for nobody looking before it is hidden
    double reportFresh = 0.3;  // s: an older copy report is not used
    float viewHalfAngle = 60;  // deg: InView
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

// How a pedestrian's copy came to it: spawned for it, or a pool copy put there (traces: PDR how=).
enum class How : uint8_t { None, Spawned, Reused };

// Why a spawn failed (the Lua's PSF reason).
enum class SpawnFail : uint8_t {
    Create,     // create_failed: the spawner's CreateObject gave nothing
    NoProfile,  // no_profile: the copy's profile did not load (or was refused)
    Reset,      // spawner_reset: the spawner was replaced while the request waited
    Timeout,    // the spawn never came back (20 s)
    Other,
    Count
};
const char* SpawnFailName(SpawnFail w);
SpawnFail SpawnFailOf(const std::string& why);  // the Lua's reason word

struct RecvCmd {
    enum class Type : uint8_t {
        Spawn,         // spawn a copy for `key` at target with profile `profile` (kCopyProfiles)
        Reuse,         // pool copy `local` is `key`'s copy from now on: put at target, shown
        Hide,          // copy `local` has no pedestrian now: hidden, kept in the pool
        Remove,        // copy `local` goes for good (local 0: `key`'s spawn request is cancelled)
        Drive,         // drive `key`'s copy: lc, target
        Kill,          // `key`'s copy dies (the host's pedestrian died)
        ResetSpawner,  // the copy spawner is broken: replace it (why)
    };
    enum class Why : uint8_t {
        None,
        LagSnap,        // Drive: a snap of a copy that did not close its gap
        Unresponsive,   // Remove: ... that lagged again right after such a snap
        CreateFailing,  // ResetSpawner: CreateObject failed spawnerFails times in a row
        CopiesLost,     // ResetSpawner: the copies were gone at once
        Body,           // Remove: a dead copy whose pedestrian went
        Surplus,        // Remove: more free copies than poolKeep
        Handover,       // Reuse: a far pedestrian's copy given to a near one
        Cancelled,      // Remove (local 0): the pedestrian went before its copy came
    };
    Type type = Type::Drive;
    Why why = Why::None;
    uint32_t key = 0;     // (authority << 16) | net
    uint32_t local = 0;   // the copy's entity id (Reuse / Hide / Remove / Kill / Drive)
    uint8_t profile = 0;  // Spawn: index into kCopyProfiles
    PedState target;      // Spawn / Reuse / Drive: where the host's pedestrian is now (Present)
    net::motion::LocomotionCommand lc;
};
const char* WhyName(RecvCmd::Why w);

struct ReceiverStats {
    uint32_t known = 0;         // host pedestrians (not stale)
    uint32_t nearby = 0;        // within coverRadius of us
    uint32_t nearReplica = 0;   // ... with a copy that stands for it now
    uint32_t copies = 0;        // copies standing for a pedestrian now
    uint32_t free = 0;          // copies in the pool without one (hidden or about to be)
    uint32_t loading = 0;       // spawn requests outstanding
    uint32_t profilesLive = 0;  // profiles still in the rotation
    uint64_t states = 0, spawns = 0, gones = 0, deaths = 0, dropsOld = 0;
    uint64_t spawnsAsked = 0, spawnFails = 0, reuses = 0, handovers = 0, hides = 0, removes = 0, snaps = 0, kills = 0;
    uint64_t failsBy[static_cast<size_t>(SpawnFail::Count)] = {};
    uint64_t lost = 0;           // copies gone without our command (the game removed them)
    uint64_t spawnerResets = 0;  // ResetSpawner asked
    uint64_t lagSnaps = 0, unresponsive = 0;
    uint64_t profilesDropped = 0;
};

// What a copy looked like this tick (traces: PDR).
struct ReplicaInfo {
    uint32_t key = 0;
    How how = How::None;
    uint32_t local = 0;
    double targetT = 0;  // the host clock time its target stands for (render time + lead), s
    PedState target;
    CopyReport copy;
    bool haveCopy = false;
    bool dead = false;  // the host's died or ours died here: a body, no target to follow (no PDR)
    float err = 0;
};

class PedReceiver {
public:
    explicit PedReceiver(const ReceiverConfig& cfg = {});
    static uint32_t Key(uint8_t auth, uint16_t net) { return (static_cast<uint32_t>(auth) << 16) | net; }

    // Messages of the host (`auth`: his player id).
    void OnSpawn(uint8_t auth, const PedSpawnInfo& s, double now);
    void OnGone(uint8_t auth, uint16_t net, double now);
    void OnStates(uint8_t auth, uint32_t authMs, const std::vector<PedState>& states, double now);
    void OnDeath(uint8_t auth, uint16_t net, const Vec3& pos, double now);
    void ForgetAuthority(uint8_t auth);  // he left / reconnected: every pedestrian of his goes (copies to the pool)

    void SetLocal(const Vec3& me, const Camera& cam);  // our player (his car while he drives) and the camera
    // Where a pedestrian's copy is (after this tick's commands); dead: it died here.
    void SetReport(uint32_t key, const CopyReport& r, double now);
    // The spawned copy of key is in the world now (local id), made with look `profile` (255: the one asked for; the
    // Lua makes a request that waited LOOK_WAIT with the look it has loaded). A copy whose request was cancelled
    // meanwhile joins the pool as a free one.
    void Spawned(uint32_t key, uint32_t local, double now = 0, uint8_t profile = 255);
    void SpawnFailed(uint32_t key, double now, SpawnFail why = SpawnFail::Other);
    void LocalGone(uint32_t local, double now);  // a copy of ours is gone (the game, or the spawner went)
    // The Lua reset the copy spawner itself (lost, deactivated, idle, an error, or our ResetSpawner): its copies went
    // with it (their LocalGone came just before) - no reason for another reset; the waiting pedestrians soon again.
    void SpawnerWasReset(double now);
    // Our copy of `key` died here while the host's pedestrian lives: true once per copy (send PedKill).
    bool DiedHere(uint32_t key);
    void Clear();  // world change / session end: everything forgotten (the game removed or reset it)

    void Tick(double now, float dt, std::vector<RecvCmd>& out);
    const ReceiverStats& Stats() const { return stats_; }
    std::vector<ReplicaInfo> Replicas(double now) const;
    double PlayoutDelay(uint8_t auth) const;
    size_t PoolSize() const { return copies_.size(); }  // copies in the world (bound and free)
    bool IsFree(uint32_t local) const;                  // a pool copy without a pedestrian
    // The profile rotation: the profile a new copy of pedestrian `net` gets now, and whether profile i is still in it.
    uint8_t ProfileFor(uint16_t net) const;
    bool ProfileLive(size_t i) const { return i < kCopyProfileCount && !profiles_[i].dropped; }
    bool ProfileWorked(size_t i) const { return i < kCopyProfileCount && profiles_[i].worked; }

private:
    struct Remote {
        uint8_t auth = 0;
        uint16_t net = 0;
        PedSpawnInfo info;
        bool haveInfo = false;
        double lastRecv = -1e9;
        double firstSeen = 0;
        bool dead = false, killed = false;
        // its copy
        How how = How::None;
        uint32_t local = 0;  // the copy's local id (0: none)
        bool loading = false;
        double spawnAskedAt = -1e9, retryAt = -1e9;
        bool diedHere = false, diedHereTold = false;
        net::motion::LocomotionController loco{LocomotionForPed()};
        CopyReport rep;
        double repAt = -1e9;
        bool haveRep = false;
        double goneAt = -1;  // the host said gone (or stale): the copy goes to the pool
        float lastErr = 0;
        PedState lastTarget;
        double lastTargetT = 0;
        bool haveTarget = false;
        float dist = 1e9f;        // from us, this tick
        double lagSince = -1;     // lagging behind a moving target since (-1: not)
        float lagRef = 0;         // the error when that started (or last got lagClose nearer)
        double lagSnapAt = -1e9;  // our last lag snap of it
        bool fresh = false;       // its copy was just made or reused: the first report decides a snap
    };
    struct Copy {
        uint32_t key = 0;  // the pedestrian it stands for (0: free)
        uint8_t profile = 0;
        bool shown = true;  // free and not hidden yet
        bool body = false;  // died: never reused
        double freeAt = 0;
        Vec3 pos;  // its last known spot
        bool havePos = false;
    };
    struct Profile {
        int fails = 0;
        double lastFailAt = -1e9;
        bool worked = false, dropped = false;
    };
    struct Auth {
        net::replica::Stream stream;
        int64_t clockMs = 0;
        uint32_t lastRawMs = 0;
        bool haveClock = false;
    };
    Auth& AuthOf(uint8_t auth);
    bool Target(const Remote& r, double now, PedState& out, double* t = nullptr) const;
    bool Seen(const Vec3& p) const;
    float DistToMe(const Vec3& p) const;
    // `r` lets go of its copy: kept in the pool (free, hidden soon), or removed (a body, or `remove`); a request
    // still loading is cancelled.
    void Unbind(Remote& r, double now, std::vector<RecvCmd>& out, bool remove = false,
                RecvCmd::Why why = RecvCmd::Why::None);
    void Bind(Remote& r, uint32_t local, How how);
    void ProfileFailed(uint8_t profile, double now);

    ReceiverConfig cfg_;
    std::map<uint32_t, Remote> remotes_;
    std::map<uint8_t, Auth> auths_;
    std::map<uint32_t, Copy> copies_;    // the pool, by local id
    std::map<uint32_t, uint8_t> asked_;  // outstanding spawn requests: key -> profile
    Vec3 me_;
    bool haveMe_ = false;
    Camera cam_;
    ReceiverStats stats_;
    Profile profiles_[kCopyProfileCount];
    double now_ = 0;  // the last Tick (ProfileFor's rest)
    // the copy spawner's health
    int createFails_ = 0;         // CreateObject failures in a row (since the last copy made)
    std::vector<double> lostAt_;  // when copies were gone without our command (recent)
    double resetAt_ = -1e9;       // our last ResetSpawner
    bool resetWanted_ = false;
    RecvCmd::Why resetWhy_ = RecvCmd::Why::None;
};

}  // namespace mc::game::ped
