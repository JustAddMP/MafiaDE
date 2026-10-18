#pragma once
// Explosions, fires and blown-up cars on every game (0.16): game-free logic, unit-tested by tests/explosion_test.cpp.
// Used by src/game/explosion_sync.cpp (messages, the mc_boom_* Lua functions) and src/game/explosion_sync.lua
// (MafiaCoop.Boom: the game calls). docs/game-api.md 20, docs/protocol.md "Explosions".
//
// The idea: damage is decided by ONE game, the pictures are shown on all.
//   - The game where an explosion really happens and that is authoritative for its cause (our player threw the
//     grenade / molotov / dynamite or shot the barrel; on the host one of its NPCs threw it; we drive the car that
//     blew up) sends it (Explosion 0x0240, CarExploded 0x0241). Its own DAMAGE events decide who is hurt: players
//     through Hit (0x0400), NPCs through NpcHit (0x0410) / HostNpcHit (0x0422), exactly as for bullets.
//   - Every other game shows it with a visual-only explosion (game.shotmgr:CreateScriptExplosion with damage 0, no
//     stun, damage ranges 0; fire for a molotov) and marks a "remote zone" there. Any explosion or fire damage inside
//     a remote zone whose attacker is not one of this game's own (nobody, a puppet, a spawned NPC copy, something
//     else) is undone (the local player gets his health back, an NPC its health from before), so nobody is hurt twice.
//   - The thrower's game marks an "own zone" where its player's explosion went off: explosion / fire damage there
//     whose attacker the game does not name (UNVERIFIED: a molotov's fire may have no owner) counts as his, so it
//     still reaches the other players and the host's NPCs through the hit paths.
// Which explosion is ours is decided by the Detector from what the game tells the Lua glue: HEAR_SOUND messages of
// sound type EXPLOSION (11) heard by any human (position + sound owner), our player's throws (WEAPON_THROWN), the
// PLAYER_GRENADE_EXPLOSION message to our player and our shots. Every bit of that is UNVERIFIED in game.
#include "net/wire.h"

#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace mc::game::boom {

constexpr uint16_t kMsgExplosion = 0x0240;    // sender -> everyone (Broadcast; the host relays), reliable
constexpr uint16_t kMsgCarExploded = 0x0241;  // sender -> everyone (Broadcast), reliable

struct Vec3 {
    float x = 0, y = 0, z = 0;
};
float Dist(const Vec3& a, const Vec3& b);

// ---- what exploded --------------------------------------------------------------------------------------------------
enum class Kind : uint8_t { Unknown = 0, Grenade = 1, Molotov = 2, Satchel = 3, Object = 4, Car = 5 };
constexpr uint8_t kMaxKind = 5;
const char* KindName(Kind k);
Kind KindOfWeapon(std::string_view weapon);    // exp_grenade / exp_molotov / exp_satchel, anything else Unknown
const char* WeaponOfKind(Kind k);              // the weapon a receiver shows for it (Unknown/Object/Car: exp_grenade)

enum Flags : uint8_t {
    kFire = 1,      // it starts a fire (molotov): the receiver's picture burns too
    kNpcOwner = 2,  // one of the sender's NPCs threw it (npc = its key there), not the sender's player
};

// ---- wire -----------------------------------------------------------------------------------------------------------
// Explosion (0x0240): u32 seq, u8 kind, u8 flags, f32 x y z, str weapon (<= 32), u32 npc, str object (<= 64),
// [u32 throwSeq] (since the grenade arc: the Throw 0x0242 this explosion ends, 0 none; absent in older senders,
// ignored by older readers)
struct Explosion {
    uint32_t seq = 0;
    Kind kind = Kind::Unknown;
    uint8_t flags = 0;
    Vec3 pos;
    std::string weapon;  // the weapon's name ("exp_grenade"); "" = unknown
    uint32_t npc = 0;    // kNpcOwner: the NPC's key on the sender's game
    std::string object;  // Kind::Object: the crash object's name (the same scene object on every game), "" = none
    uint32_t throwSeq = 0;  // our throw (game/throw_model.h) it was matched with, 0 none
};
std::vector<uint8_t> PackExplosion(const Explosion& e);
bool ReadExplosion(net::ByteReader& r, Explosion& out);  // false: malformed (bad kind, coordinates, names)

// CarExploded (0x0241): u8 what (0 a shared player car, key "o:g"; 1 a host traffic car, key = its traffic net id),
// str key (<= 24), f32 x y z (where it blew up)
enum class CarWhat : uint8_t { Shared = 0, Traffic = 1 };
struct CarBlast {
    CarWhat what = CarWhat::Shared;
    std::string key;
    Vec3 pos;
};
std::vector<uint8_t> PackCarBlast(const CarBlast& c);
bool ReadCarBlast(net::ByteReader& r, CarBlast& out);

// A name that may travel and be put on a Lua command line: 1..max of [A-Za-z0-9_-.:/], nothing else.
bool SafeName(std::string_view s, size_t max);

// ---- received messages: each sender's sequence numbers, so a message relayed twice is shown once ----------------
class Inbox {
public:
    // true the first time (from, seq) is seen; also true after a restart of the sender's counter (a new session,
    // a reconnect: seq far below the newest seen)
    bool Accept(uint32_t from, uint32_t seq);
    void Forget(uint32_t from);
    void Clear() { seen_.clear(); }

private:
    std::map<uint32_t, std::deque<uint32_t>> seen_;  // the last kKeep seqs per sender
    static constexpr size_t kKeep = 64;
};

// ---- zones: where an explosion or fire of ours / of another game went off ---------------------------------------------
enum class Origin : uint8_t { Own, Remote };
struct Zone {
    Origin origin = Origin::Own;
    Kind kind = Kind::Unknown;
    bool fire = false;
    Vec3 pos;
    double t = 0;       // when it went off (local clock, s)
    uint32_t id = 0;    // our seq (Own) or the sender's seq (Remote)
    uint32_t from = 0;  // Remote: the sender
};

constexpr float kBlastRadius = 14.0f;  // m: a grenade's damage reaches about this far (UNVERIFIED; generous)
constexpr float kFireRadius = 9.0f;    // m: a molotov's fire spreads about this far (UNVERIFIED)
constexpr double kBlastTime = 2.5;     // s after the blast its damage still counts as the blast's
constexpr double kFireTime = 35.0;     // s a molotov's fire burns (UNVERIFIED)
constexpr double kEarly = 0.5;         // s a damage may come BEFORE the zone is known (the event order is not known)

// damage types (enums.DamageType)
constexpr int kDmgExplosion = 1;
constexpr int kDmgFire = 8;

class Zones {
public:
    void Add(const Zone& z);
    // the newest zone of `origin` that covers a damage of type dtype at pos at time now (nullptr: none). Explosion
    // damage needs a blast zone (kBlastRadius, kBlastTime); fire damage a fire zone (kFireRadius, kFireTime) or a
    // blast zone (a burst sets things alight).
    // `before`: only zones that went off before then (the chain rule: an earlier explosion of ours)
    const Zone* Find(Origin origin, const Vec3& pos, double now, int dtype, double before = 1e300) const;
    void Expire(double now);
    void Remove(Origin origin, double t, const Vec3& pos);  // the zone added for exactly this (an early guess)
    size_t Count() const { return zones_.size(); }
    size_t Count(Origin o) const;
    void Clear() { zones_.clear(); }

private:
    std::vector<Zone> zones_;
    static constexpr size_t kMax = 64;
};

// ---- the damage guard -----------------------------------------------------------------------------------------------
enum class Victim : uint8_t { LocalPlayer, Puppet, Npc, NpcCopy };
// who the damage message names as the attacker, as the Lua glue classifies it (explosion_sync.lua B.Classify)
enum class Enemy : uint8_t { Us, OurNpc, Puppet, NpcCopy, None, Other };
enum class Verdict : uint8_t {
    Keep,  // the game's damage stands (this game decides it, or it has nothing to do with a remote picture)
    Undo,  // a remote picture hurt someone here: give it back (the sender's game decides this damage)
    Ours,  // our own explosion or fire, attacker not named: count it as our player's (it is sent on as a hit)
};
const char* VictimName(Victim v);
const char* EnemyName(Enemy e);
const char* VerdictName(Verdict v);
Victim VictimFromName(const char* s);  // "local", "puppet", "npc", "copy" (anything else: Npc)
Enemy EnemyFromName(const char* s);    // "us", "npc", "puppet", "copy", "none" (anything else: Other)

struct Judgement {
    Verdict verdict = Verdict::Keep;
    const Zone* zone = nullptr;  // the zone that decided it
};
Judgement Judge(const Zones& zones, Victim victim, Enemy enemy, int dtype, const Vec3& pos, double now);

// ---- the detector (the game where it happened): which explosions are ours to send ----------------------------------
enum class Owner : uint8_t { Unknown, Us, OurNpc, Other };  // the sound's owner as the Lua glue classifies it
const char* OwnerName(Owner o);
Owner OwnerFromName(const char* s);  // "us", "npc", "other" (anything else Unknown)

// enums.ActorSoundType
constexpr int kSoundBounce = 10;     // EXPLOSIVE_BOUNCE: a thrown explosive hits the ground
constexpr int kSoundExplosion = 11;  // EXPLOSION
constexpr int kSoundFire = 12;       // BURNING_FIRE

struct Sound {
    double t = 0;
    int type = 0;
    Vec3 pos;
    Owner owner = Owner::Unknown;
    uint32_t npc = 0;    // Owner::OurNpc: its key
    std::string object;  // the crash object that exploded (its name), "" none
};

struct Throw {
    double t = 0;
    std::string weapon;  // what was selected ("" unknown)
    Vec3 pos;            // where we stood
    uint32_t seq = 0;    // its throw number (game/throw_model.h, the Throw message 0x0242), 0 none
};

// One explosion as the detector saw it (sent or not, for the log and the trace).
struct Seen {
    Explosion e;  // seq is set only when send is true
    bool send = false;
    Owner owner = Owner::Unknown;
    int reports = 0;      // sounds merged into it
    const char* why = "";  // why it is (not) ours
    double sinceThrow = -1;  // s from the matched throw to the explosion (its fuse / flight), -1 no throw
};

constexpr double kSettle = 0.2;      // s after the first sound of an explosion before it is decided (more reports)
constexpr float kMerge = 4.0f;       // m: sounds this close (and kMergeTime) are the same explosion
constexpr double kMergeTime = 0.6;
constexpr double kThrowWindow = 12;  // s after our throw its explosion may come (a grenade's fuse, a long flight)
constexpr float kThrowReach = 70;    // m from where we threw
constexpr double kGrenadeMsgSlack = 0.6;  // s between PLAYER_GRENADE_EXPLOSION and the explosion's sound
constexpr double kShotWindow = 1.5;  // s after our shot a crash object's explosion is ours
constexpr float kShotReach = 150;
constexpr double kChainTime = 3.0;   // s: an explosion inside our own blast zone this soon (a barrel set off by our
                                     // grenade, a car) is ours too

// Own zones go into `zones` (the same set the damage guard reads): one right away for a sound that is surely ours
// (owner us, or an unknown owner matching a pending throw of ours), so damage that comes at once is attributed;
// otherwise when the explosion is decided.
class Detector {
public:
    explicit Detector(Zones& zones) : zones_(zones) {}
    void OnThrow(const Throw& t);
    void OnGrenadeMessage(double t);  // PLAYER_GRENADE_EXPLOSION to our player
    void OnShot(double t, const Vec3& from);
    void OnSound(const Sound& s);     // only kSoundExplosion makes an explosion; kSoundFire marks a molotov
    // decides the explosions whose settle time is over: `out` gets every decided one (send = ours or our NPC's);
    // nextSeq numbers the ones to send
    void Tick(double now, uint32_t& nextSeq, std::vector<Seen>& out);
    void Clear();
    size_t PendingThrows() const { return throws_.size(); }
    size_t Pending() const { return clusters_.size(); }

private:
    struct Cluster {
        double t0 = 0;
        Vec3 pos;
        Owner owner = Owner::Unknown;
        uint32_t npc = 0;
        std::string object;
        int reports = 0;
        bool early = false;  // an own zone was added already (OnSound)
    };
    const Throw* MatchThrow(const Vec3& pos, double t) const;
    void Prune(double now);
    Zones& zones_;
    std::vector<Cluster> clusters_;
    std::vector<Throw> throws_;
    std::vector<double> grenadeMsgs_;
    std::vector<std::pair<double, Vec3>> shots_;
    std::vector<std::pair<double, Vec3>> fires_;  // BURNING_FIRE sounds (a molotov's)
};

}  // namespace mc::game::boom
