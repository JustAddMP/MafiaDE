#pragma once
// The flight of a thrown grenade / molotov / dynamite on every game (0.15.0, "grenade arc"): game-free logic, unit-tested
// by tests/throw_arc_test.cpp. Used by src/game/throw_sync.cpp (the message, the mc_arc_* Lua functions, the
// receivers' simulation, the overlay) and src/game/throw_arc.lua (MafiaCoop.Arc: the game calls).
// docs/game-api.md 23 "The flight", docs/protocol.md "Explosions" (Throw 0x0242).
//
// The thrower's game:
//   - while our player lines up a throw the Lua glue samples the aim (where we stand, the head / hand, the camera's
//     direction, the explosive, the aim model's arc and its landing) and logs it (AimLog: GA trace records);
//   - on the release (WEAPON_THROWN) the throw gets its number at once; the explosion detector (explosion_model.h)
//     keeps it with the throw, so the later Explosion (0x0240) names it (Explosion::throwSeq);
//   - the glue looks for the real projectile entity near our hand for at most kMeasureWindow and reads its first
//     positions; the Tracker turns two or three of them into the launch (origin + velocity) and sends ONE Throw
//     message (reliable). No projectile found in time: the aim model's launch (camera direction pitched up, a fixed
//     speed: UNVERIFIED constants) is sent instead;
//   - then it keeps following the projectile cheaply (its position read every kCheckInterval, and on each of our
//     EXPLOSIVE_BOUNCE sounds): at a bounce, at a sharp turn the readings show, when it has settled and when it is
//     gone it sends one small ThrowFix (0x0243: position + velocity at that moment), at once (a bounce / turn one
//     tick later: two readings give the velocity), at most kMaxFixes per throw; the settled / gone fix reliable.
// Every other game:
//   - simulates the throw itself (Sim): ballistic steps with the sent gravity, one ray cast per step against its own
//     world (and a short probe on a hit to tell ground from wall), bounces with kRestitution / kFriction, rolls and
//     settles on the ground (a molotov stops where it first hits). One step per script tick per flight, nothing once
//     it has settled;
//   - a ThrowFix re-anchors the simulation (position + velocity at the fix's time, advanced to now by the
//     simulation); the marker slides there over kBlendTime instead of jumping; a settled / gone fix stops it there;
//   - the Book ends a flight when the Explosion naming its seq arrives (the marker jumps to the true position and
//     stays there kExplodedShow), without a seq the sender's nearest flight, else after its fuse (if sent) or
//     kMaxFlight;
//   - the glue shows it: a marker and a fading trail drawn by our overlay (nothing is spawned in the game world, so it
//     can hurt nobody; docs/game-api.md 23 lists what was looked at for a real model).
#include "game/explosion_model.h"
#include "net/wire.h"

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace mc::game::arc {

using boom::Kind;
using boom::Vec3;

constexpr uint16_t kMsgThrow = 0x0242;  // thrower -> everyone (Broadcast; the host relays), reliable
constexpr uint16_t kMsgThrowFix = 0x0243;  // thrower -> everyone: a correction event (unreliable; settled / gone reliable)

constexpr float kGravity = 9.81f;          // m/s^2 (the game's gravity for projectiles: UNVERIFIED; measured, GE)
constexpr double kMeasureWindow = 0.3;     // s after the release the real projectile is read (then the model)
constexpr size_t kMeasureSamples = 3;      // positions read at most (2 give the launch, 3 also the gravity)
constexpr double kMaxFlight = 15.0;        // s: a flight with no explosion (and no fuse) is dropped then
constexpr double kFuseGrace = 1.0;         // s past a sent fuse before the flight is dropped
constexpr double kExplodedShow = 0.25;     // s the marker stays at the explosion's true position
constexpr double kCheckInterval = 0.2;     // s between two cheap position reads while following (5 Hz)
constexpr size_t kMaxFixes = 8;            // ThrowFix messages per throw at most (the last one kept for settled / gone)
constexpr float kTurnDeviation = 1.0f;     // m a reading may be off the predicted path before it is a sharp turn
constexpr float kStillMove = 0.05f;        // m between two reads: not moving (two in a row = settled)
constexpr double kTrackMax = 15.0;         // s a projectile is followed at most
constexpr double kBlendTime = 0.1;         // s the receivers' marker slides to a re-anchored position
constexpr float kMatchReach = 30.0f;       // m: an explosion without a throw seq ends its sender's nearest flight
constexpr size_t kMaxFlights = 16;         // per game (all senders)

// the receivers' simulation (UNVERIFIED against the game's own grenade physics; tune from the GE / GR records)
constexpr float kRestitution = 0.35f;      // share of the speed into a surface that comes back out of it
constexpr float kFriction = 0.25f;         // share of the speed along a surface lost in a bounce
constexpr float kRollDecel = 4.0f;         // m/s^2 while rolling on the ground
constexpr float kRollSpeed = 1.5f;         // m/s: a ground bounce slower than this (into the ground) starts rolling
constexpr float kSettleSpeed = 0.2f;       // m/s: rolling slower than this = settled (no more ray casts)
constexpr double kMaxStep = 0.1;           // s per step at most (a long tick catches up over the next ticks)
constexpr float kProbe = 0.35f;            // m: the ground probe on a hit (up and down from the hit point)
constexpr float kSkin = 0.05f;             // m the projectile is kept off a surface it hit
constexpr float kIgnoreNear = 0.6f;        // m around the origin: hits there in the first kIgnoreTime are the thrower
constexpr double kIgnoreTime = 0.15;

enum Flags : uint8_t {
    kFire = 1,        // a molotov: it burns where it lands (and stops at its first hit)
    kFromEntity = 2,  // origin and velocity measured on the real projectile (else: the aim model's guess)
};

// ---- wire -----------------------------------------------------------------------------------------------------------
// Throw (0x0242): u32 seq, u8 kind, u8 flags, f32 origin xyz, f32 velocity xyz, f32 gravity, u16 delayMs (how old
// the origin was when sent), u16 fuseMs (from the origin to the explosion, 0 unknown), str weapon (<= 32)
struct ThrowMsg {
    uint32_t seq = 0;
    Kind kind = Kind::Unknown;
    uint8_t flags = 0;
    Vec3 origin, vel;
    float gravity = kGravity;
    uint16_t delayMs = 0;
    uint16_t fuseMs = 0;
    std::string weapon;
};
std::vector<uint8_t> PackThrow(const ThrowMsg& t);
bool ReadThrow(net::ByteReader& r, ThrowMsg& out);  // false: malformed (kind, coordinates, speed, gravity, name)

// ThrowFix (0x0243): u32 seq, u16 tMs (since the Throw's origin), u8 flags, f32 x y z, i16 vx vy vz (cm/s) = 25 bytes
enum FixFlags : uint8_t {
    kFixBounce = 1,   // our EXPLOSIVE_BOUNCE sound: it bounced here
    kFixSettled = 2,  // it lies still here (the receivers stop it here)
    kFixStop = 4,     // it is gone (exploded / removed): its last position (stop here too)
    kFixTurn = 8,     // the readings left the predicted path (a bounce without a sound, a hit)
    kFixNoVel = 16,   // position only (no projectile entity to read: the sound's position); keep the own velocity
};
struct FixMsg {
    uint32_t seq = 0;
    uint16_t tMs = 0;
    uint8_t flags = 0;
    Vec3 pos, vel;
};
std::vector<uint8_t> PackFix(const FixMsg& f);
bool ReadFix(net::ByteReader& r, FixMsg& out);

// ---- arc math -------------------------------------------------------------------------------------------------------
Vec3 Ballistic(const Vec3& origin, const Vec3& vel, float gravity, double t);  // position t s after the origin
Vec3 BallisticVel(const Vec3& vel, float gravity, double t);
// launch velocity at sample 1 from two samples of a ballistic flight (p(t) = p1 + v dt - g/2 dt^2 on z)
Vec3 EstimateLaunch(const Vec3& p1, double t1, const Vec3& p2, double t2, float gravity);
// the gravity three samples of a free flight show (positive down); false when they cannot tell (too close in time,
// a bounce in between: the result is not plausible, 2..30 m/s^2)
bool FitGravity(const Vec3& p1, double t1, const Vec3& p2, double t2, const Vec3& p3, double t3, float& g);
// the aim model's launch: the camera direction turned up by pitchUp (radians) in its vertical plane, times speed
Vec3 AimLaunch(const Vec3& camDir, float speed, float pitchUp);
void YawPitch(const Vec3& dir, float& yawDeg, float& pitchDeg);  // yaw 0 = +y, 90 = +x; pitch up positive

// ---- the receivers' simulation -----------------------------------------------------------------------------------------
// a ray cast against the world: 1 hit (position in hit), 0 nothing in between, -1 not available
using RayFn = std::function<int(const Vec3& from, const Vec3& to, Vec3& hit)>;

struct Sim {
    Vec3 origin, pos, vel;
    float gravity = kGravity;
    double t = 0;            // s since the origin simulated so far
    bool rolling = false;    // on the ground, z fixed
    bool settled = false;    // not moving any more: no more casts
    bool stopAtHit = false;  // a molotov: settles at its first hit
    int bounces = 0;
    size_t casts = 0;        // ray casts made (CPU per throw)
    bool noRay = false;      // the ray cast is not available: plain arc

    void Start(const Vec3& o, const Vec3& v, float g, bool molotov);
    // advances to `toT` (s since the origin) by one step of at most kMaxStep: one segment cast, plus one ground probe
    // on a hit
    void Step(double toT, const RayFn& ray);
};

struct Flight {
    uint32_t from = 0;
    ThrowMsg t;
    double t0 = 0;           // local time of the origin (receive time - delayMs)
    Sim sim;
    bool exploded = false;   // the Explosion came: shown at its position until explodedAt + kExplodedShow
    double explodedAt = 0;
    Vec3 boomPos;
    int lastFixMs = -1;      // the newest fix applied (older ones that come late are ignored)
    size_t fixes = 0;
    Vec3 blend;              // shown = sim + blend * (1 - (now - blendAt) / kBlendTime)
    double blendAt = -1e9;
};

enum class EndWhy : uint8_t { Explosion, Fuse, Timeout, Left };
const char* EndWhyName(EndWhy w);

struct Ended {
    uint32_t from = 0, seq = 0;
    EndWhy why = EndWhy::Timeout;
    Vec3 pos;           // where the simulation had it
    double age = 0;     // s since its origin
    float off = -1;     // Explosion: m between the simulated and the true position
    int bounces = 0;
    size_t casts = 0;
    bool settled = false;
};

struct Visual {
    uint32_t from = 0, seq = 0;
    Kind kind = Kind::Unknown;
    uint8_t flags = 0;
    Vec3 pos;
    double age = 0;
    bool held = false;        // not moving (settled, exploded)
    const char* src = "sim";  // "sim" (flying), "roll", "settled", "boom" (at the explosion's position), "arc" (no ray)
};

class Book {
public:
    // a new throw (false: seen before); the oldest goes when full
    bool Add(uint32_t from, const ThrowMsg& t, double now);
    // every flight one simulation step toward now
    void Step(double now, const RayFn& ray);
    // a ThrowFix: re-anchors that flight's simulation (true: applied; false: unknown flight, older than the last fix,
    // exploded already)
    bool Fix(uint32_t from, const FixMsg& f, double now, const RayFn& ray);
    // an Explosion arrived: the flight it names (throwSeq), or without one the sender's nearest within kMatchReach;
    // true + `out` when one ended (it is shown at pos for kExplodedShow, then removed silently by Tick)
    bool EndByExplosion(uint32_t from, uint32_t throwSeq, const Vec3& pos, double now, Ended& out);
    // removes flights over their fuse / time, returns them (not the exploded ones: reported by EndByExplosion)
    void Tick(double now, std::vector<Ended>& out);
    void Visuals(double now, std::vector<Visual>& out) const;
    void Forget(uint32_t from, double now, std::vector<Ended>& out);
    void Clear() { flights_.clear(); }
    size_t Count() const { return flights_.size(); }
    const Flight* Find(uint32_t from, uint32_t seq) const;

private:
    using Key = std::pair<uint32_t, uint32_t>;
    Ended MakeEnded(const Flight& f, EndWhy why, double now) const;
    std::map<Key, Flight> flights_;
};

// ---- the thrower: the launch measured in the first moments after the release ----------------------------------------
struct Out {
    enum class Type : uint8_t { Throw, Fix } type = Type::Throw;
    ThrowMsg t;
    FixMsg f;
    bool reliable = true;
};

// the tracker's view of one throw, for the log / trace
struct Summary {
    uint32_t seq = 0;
    std::string weapon;
    bool sent = false, fromEntity = false;
    size_t samples = 0;
    double after = 0;    // s from the release to the send
    float gravity = 0;   // measured (0: could not tell)
    float speed = 0;     // launch speed sent
    float modelSpeed = 0, modelPitch = 0, realPitch = 0;  // the aim model's guess against the real launch (deg)
    Vec3 origin;
    const char* why = "";  // "measured", "model", "no_projectile_no_model", "ended"
};

class Tracker {
public:
    // the release: seq given out by the caller; standing = where our player stood
    void Begin(uint32_t seq, Kind kind, const std::string& weapon, double now, const Vec3& standing);
    // the aim model's launch (sent if the real projectile is not read in time)
    void Model(uint32_t seq, const Vec3& origin, const Vec3& vel);
    // a position of the real projectile at `now`: while measuring the Throw goes out at the kMeasureSamples-th; while
    // following it may make a fix. Returns when to read it next: 0 the next tick, > 0 in that many s, < 0 never
    double Sample(uint32_t seq, double now, const Vec3& pos, std::vector<Out>& out, std::vector<Summary>& done);
    // our EXPLOSIVE_BOUNCE sound while following: entity = pos is the projectile's own position (its velocity is read
    // from the next tick's reading: returns 0), else pos is the sound's (a position-only fix at once)
    double Bounce(uint32_t seq, double now, const Vec3& pos, bool entity, std::vector<Out>& out);
    // the projectile is gone (the entity cannot be read any more): a stop fix at its last position
    void Gone(uint32_t seq, double now, std::vector<Out>& out);
    // the measure window is over: sent from what there is (2 samples, else the model); followed throws time out
    void Tick(double now, std::vector<Out>& out, std::vector<Summary>& done);
    // our explosion for it was decided: still measuring -> sent now from what there is; no more following
    void End(uint32_t seq, double now, std::vector<Out>& out, std::vector<Summary>& done);
    bool Active(uint32_t seq) const;     // measured or followed (the glue keeps reading it)
    bool Measuring(uint32_t seq) const;
    size_t Fixes(uint32_t seq) const;    // fixes sent for it so far
    bool Any() const { return !throws_.empty(); }
    void Clear() { throws_.clear(); }

private:
    struct T {
        uint32_t seq = 0;
        Kind kind = Kind::Unknown;
        std::string weapon;
        double begin = 0;
        Vec3 standing;
        bool hasModel = false;
        Vec3 modelOrigin, modelVel;
        std::vector<std::pair<double, Vec3>> samples;
        // following (after the Throw went out)
        bool following = false, fromEntity = false;
        double t0 = 0;               // the Throw's origin time (fix times are since then)
        bool hasRead = false;
        double readT = 0;            // the last reading
        Vec3 readPos, readVel;       // and the velocity there (estimated)
        bool hasVel = false;
        uint8_t pending = 0;         // a bounce / turn waiting for the next reading (its velocity)
        int still = 0;
        size_t fixes = 0;
    };
    T* Get(uint32_t seq);
    const T* Get(uint32_t seq) const;
    void Finish(size_t i, double now, std::vector<Out>& out, std::vector<Summary>& done);
    void SendFix(T& t, double now, uint8_t flags, const Vec3& pos, const Vec3& vel, std::vector<Out>& out);
    std::vector<T> throws_;
};

// ---- the aim log: one line per sample at most every interval, start / end summaries ---------------------------------
struct AimSample {
    double t = 0;
    std::string weapon;   // the selected explosive
    std::string how;      // what said we aim: "input", "msg", "hand" (the explosive is in hand), combinations
    Vec3 standing, head, hand, cam, camDir;
    bool handBone = false;  // `hand` is a real bone (else the head + an offset)
    bool hasLanding = false;
    Vec3 landing;
    float flight = 0;     // s to the landing (model)
};
class AimLog {
public:
    // true when this sample should be written (trace), at most one per interval
    bool Take(const AimSample& s, double interval);
    // the aim ended (no sample this tick): true + the summary when it was on
    bool Stop(double now, double& lasted, size_t& samples);
    bool On() const { return on_; }
    double Since() const { return since_; }

private:
    bool on_ = false;
    double since_ = 0, lastTaken_ = -1e9;
    size_t n_ = 0;
};

}  // namespace mc::game::arc
