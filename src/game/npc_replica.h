#pragma once
// One authority per NPC (docs/protocol.md "NPCs", docs/sync-architecture.md section 8 `npc-authority`). Pure logic, no
// game or Lua calls: unit-tested by tests/npc_replica_test.cpp; npc_sync.cpp feeds it and carries out what it decides.
//
// Every NPC is simulated by exactly one game, its authority: the host for its NPCs (mission NPCs, police, anything
// with a job), a guest for the NPCs only his game has (npc_owner.h GuestOwner). Every other game shows a *replica*: a
// local NPC paired with it, or a spawned copy, DRIVEN from the authority's states every tick - never its own AI.
//
//   wire        NpcStates2 (0x0212 host -> guest, 0x0424 guest -> host): the authority's sample time (ms, its own
//               steady clock, as PlayerState), an origin, then ~19 bytes per NPC (u16 net id, position from the origin
//               in cm, heading, ground velocity in cm/s, health, flags, shots, gait). The slow fields (entity key, AI
//               type, archetype tag, weapon, spawn profile, name class) go once per receiver and on change in the
//               reliable NpcInfo (0x0213 / 0x0425); NpcLeave (0x0214 / 0x0426) says a net id is no longer sent to him.
//   Publisher   the authority's side: velocity and gait from its rounds, net ids, core's InterestManager per receiving
//               player (viewpoints = his player; enter 120 m / leave 140 m; 15 Hz within 30 m down to 5 Hz at 120 m,
//               idle NPCs half as often; a byte budget per receiver), infos on enter / change, leaves; truth records.
//   ReplicaSet  the receiving side: one replica::Stream per sender (its playout clock; per-NPC tracks, each
//               interpolated with its own sample interval so a 2 Hz far NPC moves as smoothly as a 15 Hz near one),
//               and per replica a LocomotionController whose position fix is the Corrector preset ForNpc (core
//               net/correction.h; dead zone 0.3 m and 50 % of the speed here, see ReplicaTuning): commands for the Lua
//               glue (walk, speed, nudge,
//               snap, turn), reports back from it, replica records. A paired local NPC that does not follow (it keeps
//               walking off: three snaps within 20 s) is given up: the glue hides it and shows a spawned copy.
//   rules       which humans are ambient (the `ambient-peds` package's, never ours), when a relayed NPC hit on our
//               player is skipped (only where our own game really runs that NPC).
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "game/npc_owner.h"
#include "net/correction.h"
#include "net/interest.h"
#include "net/remote_motion.h"
#include "net/replica.h"

namespace mc::net {
class ByteWriter;
class ByteReader;
}  // namespace mc::net

namespace mc::game::npcrep {

using npcown::NpcState;
using npcown::PlayerId;
using net::motion::Vec3;

// ---- wire ----------------------------------------------------------------------------------------------------------------
enum Flag : uint8_t { kDead = 1, kInCar = 2 };

struct Fast {          // one NPC in NpcStates2
    uint16_t net = 0;  // its net id (NpcInfo says which NPC)
    float x = 0, y = 0, z = 0;  // world m (1 cm steps from the message origin)
    float hx = 0, hy = 1;       // heading (x127)
    float vx = 0, vy = 0;       // ground velocity m/s (1 cm/s steps)
    uint16_t hp = 0;
    uint8_t flags = 0;  // Flag
    uint8_t shots = 0;  // shots fired, mod 256
    uint8_t gait = 0;   // 0 stand, 1 walk, 2 run, 3 sprint (the authority NPC's, from its speed)
};
constexpr size_t kFastBytes = 19;
constexpr size_t kStatesHeader = 4 + 12 + 1;  // u32 time, f32 origin x y z, u8 count
constexpr size_t kMaxPerStates = 48;           // NPCs per message (~930 bytes)
constexpr float kMaxOffset = 327.0f;           // m from the origin an i16 in cm can hold

struct StatesMsg {
    uint32_t timeMs = 0;  // the authority's sample time (its steady clock, ms, wraps)
    std::vector<Fast> npcs;
};
// One or more messages (each with its own origin, at most kMaxPerStates NPCs).
std::vector<std::vector<uint8_t>> PackStates(uint32_t timeMs, const std::vector<Fast>& npcs);
bool ReadStates(net::ByteReader& r, StatesMsg& out);  // false on a short or bad message (nothing kept)

enum class NameClass : uint8_t { Unknown = 0, Spawner = 1, Generator = 2 };
struct Info {  // NpcInfo: the slow fields of one net id
    uint16_t net = 0;
    uint32_t key = 0;   // its entity id on the authority's game (the key of NpcDeath, NpcHit, claims, relays)
    uint8_t ai = 0;     // AI type
    uint32_t tag = 0;   // battle archetype hash
    std::string weapon = "-";
    uint64_t profile = 0;  // spawn profile hash (0 = unknown)
    NameClass cls = NameClass::Unknown;
    bool operator==(const Info& o) const {
        return net == o.net && key == o.key && ai == o.ai && tag == o.tag && weapon == o.weapon &&
               profile == o.profile && cls == o.cls;
    }
};
constexpr size_t kMaxInfoBytes = 2 + 4 + 1 + 4 + 1 + 48 + 8 + 1;
std::vector<std::vector<uint8_t>> PackInfos(const std::vector<Info>& infos);  // u8 n, records (<= 1000 bytes each)
bool ReadInfos(net::ByteReader& r, std::vector<Info>& out);
std::vector<std::vector<uint8_t>> PackLeaves(const std::vector<uint16_t>& nets);  // u8 n, u16 nets
bool ReadLeaves(net::ByteReader& r, std::vector<uint16_t>& out);

// ---- rules ---------------------------------------------------------------------------------------------------------------
// The class of a human by its scene name (docs/game-api.md 15 "spawn profile of an NPC"): `behavior_object_prime-NN-`
// followed by a mission spawner's name = Spawner; nothing (or the puppet factory driver's own generated name) after it
// = Generator (traffic, police and pedestrian generators, our puppets); any other name = Unknown.
NameClass ClassOfName(const std::string& name);
// The `ambient-peds` contract (both packages implement it the same way): an ambient human is not a player, puppet or
// any copy, not in a vehicle, AI type 0, and not named after a mission spawner. npc_sync never sends, pairs, drives or
// reports one; ped_sync.lua's humans (MafiaCoop.PedOwned) are skipped as well.
bool IsAmbientHuman(bool playerOrCopy, bool inVehicle, int aiType, NameClass cls);
// What an NPC that hit our player on another game is here (player_sync.lua M.ApplyHit, npc_sync.lua N.FightsUs):
// a Replica (a paired NPC or a spawned copy, driven from its authority: it never fights here), our Own NPC (our game
// is its authority) or None (nothing here).
enum class HitRole : uint8_t { None = 0, Replica = 1, Own = 2 };
// Skip the relayed hit (the hit already happened on our game)? Only for our own NPC that is alive and hostile here.
bool SkipRelayedHit(HitRole role, bool alive, bool hostile);

// The authority NPC's gait from its ground speed, with hysteresis (LocomotionTuning thresholds: stand < 0.35 m/s,
// walk -> run 2.5 / run -> walk 2.1, run -> sprint 5.3 / sprint -> run 4.7 m/s).
uint8_t GaitOf(float speed, uint8_t previous);

// ---- authority side ------------------------------------------------------------------------------------------------------
struct PublisherTuning {
    float enter = 120, leave = 140;   // m from a receiver's viewpoint
    float nearDist = 30;              // m: up to here nearInterval
    double nearInterval = 1.0 / 15;   // s (15 Hz)
    double farInterval = 0.2;         // s at `enter` (5 Hz), linear in between (2 Hz: the far target alone was 0.6-0.7 m
                                      // off at p95 in tests/npc_replica_test.cpp; 5 Hz: 0.3 m, still <= 8 KB/s)
    float idleSpeed = 0.35f;          // m/s: slower NPCs are sent idleScale x less often ...
    double idleScale = 2.0;
    double maxInterval = 1.0;         // ... at most this far apart
    double deadInterval = 1.0;        // s: a dead NPC's state at most this often (its death is a reliable event)
    uint32_t budget = 7000;           // bytes / s of NPC records per receiver (+ ~20 bytes per message): <= 8 KB/s
    double forgetAfter = 1.5;         // s out of the rounds: the NPC is gone (leave to everyone who had it)
    float teleport = 20.0f;           // m between two rounds: no velocity from it
};

struct Item {                // one NPC of the authority's round
    NpcState s;              // key, position, heading, health, flags, slow fields (from the Lua glue)
    PlayerId owner = 0;      // whose game simulates it (never sent back to him)
    NameClass cls = NameClass::Unknown;
    uint32_t peerMask = 0xFFFFFFFFu;  // the receivers that may get it (bit = player id); relays name theirs
};

struct PeerOut {
    PlayerId peer = 0;
    std::vector<uint16_t> leaves;  // first (reliable): these net ids are no longer sent to him
    std::vector<Info> infos;       // then (reliable): new to him or changed
    struct Group {
        uint32_t timeMs = 0;
        std::vector<Fast> npcs;
    };
    std::vector<Group> states;     // then (unreliable): the states due now, one group per sample time
    size_t Bytes() const;          // the payload bytes of all of it
};

struct Truth {  // NPT: an authority NPC state that went out (once per key and tick)
    uint32_t key = 0;
    uint32_t timeMs = 0;
    float x = 0, y = 0, z = 0, vx = 0, vy = 0;
    uint8_t gait = 0;
};

class Publisher {
public:
    explicit Publisher(const PublisherTuning& t = {});
    void Reset();
    // The receivers (other players): added / removed; a removed one is forgotten silently.
    void SetPeers(const std::vector<PlayerId>& peers);
    void SetViewpoints(PlayerId peer, const std::vector<Vec3>& points);
    // A round of the NPCs this game is the authority for (sampled at timeMs on its steady clock, local time now).
    // Items of earlier rounds that are not in this one stay until forgetAfter (a relay round comes at its own pace),
    // unless `complete` (this round lists every NPC of `owner` there is: the missing ones are gone now).
    void Round(uint32_t timeMs, double now, const std::vector<Item>& items, bool complete, PlayerId owner);
    // Interest, rates and budget: what each receiver gets now. Once per round.
    std::vector<PeerOut> Tick(double now);
    std::vector<Truth> TakeTruth() { return std::move(truth_); }
    size_t Count() const { return ents_.size(); }
    uint16_t NetOf(uint32_t key) const;  // 0: not known
    const net::interest::PeerStats* Stats(PlayerId peer) const { return im_->Stats(peer); }
    uint64_t SentBytes(PlayerId peer) const;  // payload bytes given to that receiver since he was added

private:
    struct Ent {
        uint16_t net = 0;
        PlayerId owner = 0;
        uint32_t peerMask = 0xFFFFFFFFu;
        Info info;
        Fast fast;
        uint32_t timeMs = 0;  // sample time of `fast`
        double seen = 0;      // local time of the last round with it
        bool haveVel = false;
        float lastX = 0, lastY = 0, lastZ = 0;
        uint32_t lastMs = 0;
    };
    struct PeerKnow {
        std::map<uint32_t, Info> has;       // key -> the info he has
        std::map<uint32_t, double> deadSent;  // key -> last time a dead NPC's state went to him
        std::vector<net::interest::Vec3> views;
        uint64_t bytes = 0;
    };
    uint16_t Allocate();
    void Remove(uint32_t key);
    PublisherTuning t_;
    std::unique_ptr<net::InterestManager> im_;
    std::map<uint32_t, Ent> ents_;        // key -> NPC
    std::map<uint16_t, uint32_t> nets_;   // net -> key
    uint16_t nextNet_ = 1;
    std::map<PlayerId, PeerKnow> peers_;
    std::vector<uint32_t> gone_;          // keys removed since the last Tick (their leaves go out then)
    std::vector<Truth> truth_;
};

// ---- receiving side ------------------------------------------------------------------------------------------------------
// The correction of an NPC replica (ReplicaTuning::fix).
net::correction::Tuning NpcFix();

struct ReplicaTuning {
    net::replica::StreamTuning stream;
    // Corrector preset ForNpc (snap 5 m, 3 m high, stuck 0.8 m for 3 s), with the dead zone of ForFoot (0.3 m: a
    // replica is now driven every tick like a puppet, small errors are corrected) and a share of 50 % of the speed (the
    // fallback named in docs/sync-architecture.md section 4): the session-1 cops through a simulated AI walker, p95
    // 1.06 -> 0.97 m (tests/npc_replica_test.cpp). UNVERIFIED in game.
    net::correction::Tuning fix = NpcFix();
    net::motion::LocomotionTuning loco;  // walk planning; its position fix is `fix` (see NpcLoco)
    double forget = 3.0;          // s without a sample (local clock): the NPC's track goes
    double maxExtra = 1.0;        // s: a slow track is shown up to this much further in the past (its own interval)
    double extraRate = 0.3;       // s per s the extra delay of a track may change (no jumps when its rate changes)
    float accelShare = 1.0f;      // the lead to "now" follows this share of the NPC's acceleration ...
    float maxAccel = 6.0f;        // ... bounded (m/s^2)
    // The authority NPC's gait as the controller's gait hint. Off: it is only derived from its speed on the authority
    // (no game call tells an NPC's gait), so it lags every start and stop; the controller's own choice from the wanted
    // speed and the target's acceleration followed the session-1 cops closer (tests/npc_replica_test.cpp).
    bool gaitHint = false;
    double reportFresh = 0.3;     // s: an older report = the replica is not being driven
    int refuseSnaps = 3;          // a paired local NPC snapped this often ...
    double refuseWindow = 20.0;   // ... within this many s does not follow (its AI walks off): hidden, a copy shown
    ReplicaTuning();
};
// The LocomotionController settings for an NPC replica: the walk as for player puppets; its position fix is the
// correction (NpcFix: dead zone 0.3 m, 2 /s, at most 50 % of the speed or 0.15 m/s; the snaps are decided by the
// Corrector: 5 m, 3 m high, stuck 0.8 m for 3 s, a teleport of the target), except "blocked" (walking but not getting
// anywhere for 1.5 s), which only the controller can see.
net::motion::LocomotionTuning NpcLoco(const net::correction::Tuning& fix);

struct Target {  // an NPC with an info and samples, where it is now (the "N" line for the Lua glue)
    Info info;
    Vec3 pos, vel;
    float hx = 0, hy = 1;
    uint16_t hp = 0;
    uint8_t flags = 0, shots = 0, gait = 0;
    double age = 0;        // s since its last sample arrived
    double renderMs = 0;   // the authority time it stands for (its render time + lead), ms (unwrapped)
};

struct Report {  // the Lua glue drove a replica: where it is now ("NR")
    uint32_t key = 0;
    Vec3 pos;
    float hx = 0, hy = 1;
    uint8_t kind = 1;     // 1 a paired local NPC, 2 a spawned copy
    bool busy = false;    // not to be walked or corrected this tick (dying, in a car, an action)
    double at = 0;        // local time
};

struct Drive {  // the command for one replica this tick ("R" line)
    uint32_t key = 0;
    net::motion::LocomotionCommand lc;
    Vec3 target;          // where the authority's NPC is now
    float hx = 0, hy = 1;
    uint8_t kind = 1;
    bool refuse = false;  // a paired local NPC that does not follow: hide it, show a copy (once)
    net::correction::Result::Why why = net::correction::Result::Why::None;  // why it snapped (lc.fix Snap)
    bool blocked = false;  // the controller's own snap: walking but not getting anywhere
    double renderMs = 0;
    uint8_t senderGait = 0;
};

struct ReplicaRecord {  // NPR: one replica as driven (for the sync trace, 10 Hz)
    uint32_t key = 0;
    uint8_t kind = 1;
    double renderMs = 0;  // the authority time its target stood for
    Vec3 copy, target;
    float err = 0;        // copy vs target (m, ground)
    uint8_t fix = 0;      // 0 none, 1 nudge, 2 snap
    uint8_t gait = 0, senderGait = 0;
    const char* why = "-";
};

class ReplicaSet {
public:
    explicit ReplicaSet(const ReplicaTuning& t = {});
    void Reset();  // a new stream (session start, reconnect)
    // Messages from this set's sender. timeMs on its clock (unwrapped here).
    void OnStates(const StatesMsg& m, double now);
    void OnInfos(const std::vector<Info>& infos);
    void OnLeaves(const std::vector<uint16_t>& nets);
    // Once per frame: the playout clock, tracks that stopped coming are forgotten (their keys are returned).
    std::vector<uint32_t> Update(double now);
    // Every NPC with an info and samples, in key order.
    std::vector<Target> Targets(double now) const;
    bool TargetOf(uint32_t key, double now, Target& out) const;
    // The glue's reports, then the commands for every replica reported fresh (in key order).
    void OnReport(const Report& r);
    std::vector<Drive> Commands(double now, float dt);
    // The replica record of `key` for the last command and its report (false: none).
    bool Record(uint32_t key, ReplicaRecord& out) const;
    void Forget(uint32_t key);  // the replica went (unpaired, copy dropped): its controller state goes too
    uint16_t NetOf(uint32_t key) const;
    const Info* InfoOf(uint32_t key) const;
    size_t Tracks() const { return stream_.Size(); }
    size_t Known() const { return byKey_.size(); }
    const net::replica::Stream& StreamRef() const { return stream_; }
    // counters
    uint32_t Snaps() const { return snaps_; }
    uint32_t Refusals() const { return refusals_; }
    uint32_t Dropped() const { return stream_.Dropped(); }

private:
    struct Track {
        double interval = 0;  // smoothed sample interval (s)
        double lastT = -1;
        double extra = 0;     // s further in the past than the stream's render time (slow tracks)
        bool extraSet = false;
        uint16_t hp = 0;      // newest
        uint8_t flags = 0;
    };
    struct Rep {
        net::motion::LocomotionController loco;
        net::correction::Corrector fix;
        Report report;
        bool haveReport = false;
        Drive last;
        bool haveLast = false;
        std::vector<double> snapTimes;
        bool refused = false;
        uint8_t kind = 1;
        Rep(const net::motion::LocomotionTuning& l, const net::correction::Tuning& f) : loco(l), fix(f) {}
    };
    double SenderTime(uint32_t ms);  // unwrap
    bool TargetOfNet(uint16_t net, const Track& tr, double now, Target& out) const;
    void Lead(const net::replica::SnapshotTrack& track, net::replica::Out& o, double lead) const;
    ReplicaTuning t_;
    net::motion::LocomotionTuning loco_;
    net::replica::Stream stream_;
    std::map<uint16_t, Info> infos_;      // net -> info
    std::map<uint32_t, uint16_t> byKey_;  // key -> net
    std::map<uint16_t, Track> tracks_;    // net -> per-track timing
    std::map<uint32_t, Rep> reps_;        // key -> replica
    int64_t clockMs_ = 0;
    uint32_t lastRawMs_ = 0;
    bool haveClock_ = false;
    double lastNow_ = 0;
    uint32_t snaps_ = 0, refusals_ = 0;
};

}  // namespace mc::game::npcrep
