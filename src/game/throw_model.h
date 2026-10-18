#pragma once
// The flight of a thrown grenade / molotov / dynamite on every game (0.16, "grenade arc"): game-free logic, unit-tested
// by tests/throw_arc_test.cpp. Used by src/game/throw_sync.cpp (messages, the mc_arc_* Lua functions, the overlay) and
// src/game/throw_arc.lua (MafiaCoop.Arc: the game calls). docs/game-api.md 20 "The flight", docs/protocol.md
// "Explosions" (Throw 0x0242, ThrowFlight 0x0243).
//
// The thrower's game:
//   - while our player lines up a throw (an explosive selected, the aim input held, or the game's PLAYER_AIM_ENTER)
//     the Lua glue samples the aim (where we stand, the head / hand, the camera's direction, the explosive, a
//     predicted arc and where it lands) and logs it (AimLog: GA trace records, rate-limited);
//   - on the release (WEAPON_THROWN, player_sync.lua action kind 5) a throw number (seq) is given out at once, the
//     explosion detector (explosion_model.h) remembers it with the throw, so the later Explosion (0x0240) carries it
//     (Explosion::throwSeq), and the glue looks for the real projectile entity (entity types 25 grenade, 26 molotov,
//     88 projectile explosive, 90 satchel charge; new, or moved, near our hand) and reads its position every tick;
//   - the Tracker turns those samples into the Throw message (origin + launch velocity from the first two samples,
//     reliable, once) and the flight points (ThrowFlight, 20 Hz, unreliable; the last one, "end", reliable). If no
//     projectile is found in time, the aim model's launch (camera direction pitched up, a fixed speed: UNVERIFIED
//     constants) is sent instead (flag kFromModel).
// Every other game:
//   - the Book keeps each sender's flights: a ballistic arc from origin + velocity with the gravity, corrected by the
//     streamed points (played back kPlaybackDelay behind, interpolated; past the last point extrapolated a little,
//     then held); it ends when the Explosion naming its seq arrives (or, without a seq, the nearest flight of that
//     sender), at its end point + kHoldAfterEnd, or after kMaxFlight;
//   - the glue shows it: a marker and a fading trail drawn by our overlay at the projected positions (nothing is
//     spawned in the game world, so it can hurt nobody; docs/game-api.md 20 lists what was looked at for a real model).
#include "game/explosion_model.h"
#include "net/wire.h"

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mc::game::arc {

using boom::Kind;
using boom::Vec3;

constexpr uint16_t kMsgThrow = 0x0242;        // thrower -> everyone (Broadcast; the host relays), reliable
constexpr uint16_t kMsgThrowFlight = 0x0243;  // thrower -> everyone, unreliable (the end point reliable)

constexpr float kGravity = 9.81f;          // m/s^2 (the game's gravity for projectiles: UNVERIFIED; measured, GE)
constexpr double kPlaybackDelay = 0.10;    // s the receiver plays the streamed points behind (to interpolate)
constexpr double kExtrapolate = 0.5;       // s past the last point it is carried on ballistically, then held
constexpr double kHoldAfterEnd = 3.0;      // s a flight is held at its end point waiting for the explosion
constexpr double kMaxFlight = 15.0;        // s: a flight with no explosion and no end is dropped then
constexpr double kPointInterval = 0.05;    // s between streamed points (20 Hz)
constexpr double kEntityWait = 0.6;        // s after the release the real projectile may take to be found
constexpr double kTrackMax = 15.0;         // s a projectile is followed at most
constexpr float kMatchReach = 30.0f;       // m: an explosion without a throw seq ends its sender's nearest flight
constexpr size_t kMaxFlights = 16;         // per game (all senders)
constexpr size_t kMaxPointsPerMsg = 8;

enum Flags : uint8_t {
    kFire = 1,        // a molotov: it burns where it lands
    kFromEntity = 2,  // origin and velocity measured on the real projectile (else: the aim model's guess)
    kHasLanding = 4,  // landing + flightMs are the sender's prediction (ray cast along the arc)
};
enum PointFlags : uint8_t {
    kBounce = 1,  // an EXPLOSIVE_BOUNCE sound of ours was heard here (a point of the flight, maybe not a sample)
    kEnd = 2,     // the projectile is gone (exploded, burnt out, removed): the last point
};

// ---- wire -----------------------------------------------------------------------------------------------------------
// Throw (0x0242): u32 seq, u8 kind, u8 flags, f32 origin xyz, f32 velocity xyz, f32 gravity, u16 delayMs (how old
// the origin was when sent), u16 flightMs (predicted time to landing, 0 unknown), f32 landing xyz, str weapon (<= 32)
struct ThrowMsg {
    uint32_t seq = 0;
    Kind kind = Kind::Unknown;
    uint8_t flags = 0;
    Vec3 origin, vel;
    float gravity = kGravity;
    uint16_t delayMs = 0;
    uint16_t flightMs = 0;
    Vec3 landing;
    std::string weapon;
};
std::vector<uint8_t> PackThrow(const ThrowMsg& t);
bool ReadThrow(net::ByteReader& r, ThrowMsg& out);  // false: malformed (kind, coordinates, speed, gravity, name)

// ThrowFlight (0x0243): u32 seq, u8 n (1..8), n x { u16 tMs (since the origin), u8 flags, f32 x y z }
struct Point {
    uint16_t tMs = 0;
    uint8_t flags = 0;
    Vec3 pos;
};
struct FlightMsg {
    uint32_t seq = 0;
    std::vector<Point> points;
};
std::vector<uint8_t> PackFlight(const FlightMsg& f);
bool ReadFlight(net::ByteReader& r, FlightMsg& out);

// ---- arc math -------------------------------------------------------------------------------------------------------
Vec3 Ballistic(const Vec3& origin, const Vec3& vel, float gravity, double t);  // position t s after the origin
Vec3 BallisticVel(const Vec3& vel, float gravity, double t);
// launch velocity at sample 1 from two samples of a ballistic flight (p(t) = p1 + v dt - g/2 dt^2 on z)
Vec3 EstimateLaunch(const Vec3& p1, double t1, const Vec3& p2, double t2, float gravity);
// the gravity three samples of a free flight show (positive down); false when they cannot tell (too close in time,
// the middle one not between, a bounce in between: the result is not plausible, 2..30 m/s^2)
bool FitGravity(const Vec3& p1, double t1, const Vec3& p2, double t2, const Vec3& p3, double t3, float& g);
// the aim model's launch: the camera direction turned up by pitchUp (radians) in its vertical plane, times speed
Vec3 AimLaunch(const Vec3& camDir, float speed, float pitchUp);
void YawPitch(const Vec3& dir, float& yawDeg, float& pitchDeg);  // yaw 0 = +y, 90 = +x; pitch up positive

// ---- the receiver: flights of the other games ------------------------------------------------------------------------
struct Flight {
    uint32_t from = 0;
    ThrowMsg t;
    double t0 = 0;         // local time of the origin (receive time - delayMs)
    std::vector<Point> points;  // sorted by tMs
    bool ended = false;    // the end point came
    double endedAt = 0;
    int bounces = 0;
};

enum class EndWhy : uint8_t { Explosion, EndPoint, Timeout, Left, Replaced };
const char* EndWhyName(EndWhy w);

struct Ended {
    uint32_t from = 0, seq = 0;
    EndWhy why = EndWhy::Timeout;
    Vec3 pos;           // where it was shown last
    double age = 0;     // s since its origin
    size_t points = 0;  // streamed points it got
};

struct Visual {
    uint32_t from = 0, seq = 0;
    Kind kind = Kind::Unknown;
    uint8_t flags = 0;
    Vec3 pos;
    double age = 0;
    bool held = false;  // not moving any more (landed, end point, past the extrapolation)
    const char* src = "arc";  // what the position comes from: "arc" (origin + velocity), "points", "extrapolated",
                              // "landing", "end"
};

// the position of a flight `age` s after its origin (playback already applied by the caller)
Visual PositionAt(const Flight& f, double age);

class Book {
public:
    // a new throw (false: seen before, or malformed); an older flight of the same sender and seq is replaced
    bool Add(uint32_t from, const ThrowMsg& t, double now);
    // streamed points; points for a throw not here yet are kept a moment (they may overtake the reliable Throw)
    void AddPoints(uint32_t from, const FlightMsg& f, double now);
    // an Explosion arrived: the flight it names (throwSeq), or without one the sender's nearest flight within
    // kMatchReach; true + `out` when one ended
    bool EndByExplosion(uint32_t from, uint32_t throwSeq, const Vec3& pos, double now, Ended& out);
    // removes flights over their time, returns them
    void Tick(double now, std::vector<Ended>& out);
    void Visuals(double now, std::vector<Visual>& out) const;
    void Forget(uint32_t from, double now, std::vector<Ended>& out);
    void Clear() {
        flights_.clear();
        orphans_.clear();
    }
    size_t Count() const { return flights_.size(); }
    const Flight* Find(uint32_t from, uint32_t seq) const;

private:
    using Key = std::pair<uint32_t, uint32_t>;
    Ended MakeEnded(const Flight& f, EndWhy why, double now) const;
    std::map<Key, Flight> flights_;
    struct Orphan {
        uint32_t from, seq;
        double at;
        Point p;
    };
    std::vector<Orphan> orphans_;
};

// ---- the thrower: one throw followed from the release to its end -----------------------------------------------------
struct Out {
    enum class Type : uint8_t { Throw, Points } type = Type::Throw;
    ThrowMsg t;    // Throw
    FlightMsg f;   // Points
    bool reliable = true;
};

// the tracker's view of one throw, for the log / trace when it is over
struct Summary {
    uint32_t seq = 0;
    std::string weapon;
    bool sent = false, fromEntity = false;
    size_t samples = 0, points = 0, bounces = 0;
    double flight = 0;   // s from the origin to the last sample
    float gravity = 0;   // measured (0: could not tell)
    float speed = 0;     // launch speed sent
    float modelSpeed = 0, modelPitch = 0, realPitch = 0;  // the aim model's guess against the real launch (deg)
    Vec3 origin, last;
    const char* why = "";
};

class Tracker {
public:
    // the release: seq given out by the caller; standing = where our player stood
    void Begin(uint32_t seq, Kind kind, const std::string& weapon, double now, const Vec3& standing);
    // the aim model's launch (sent if the real projectile is not found in time)
    void Model(uint32_t seq, const Vec3& origin, const Vec3& vel, bool hasLanding, const Vec3& landing,
               uint16_t flightMs);
    // a position of the real projectile at `now`
    void Sample(uint32_t seq, double now, const Vec3& pos, std::vector<Out>& out);
    void Bounce(uint32_t seq, double now, const Vec3& pos, std::vector<Out>& out);
    // the projectile is gone (exploded / removed) or our explosion with this seq was decided; pos: where (nullptr:
    // the last sample)
    void End(uint32_t seq, double now, const Vec3* pos, const char* why, std::vector<Out>& out,
             std::vector<Summary>& done);
    void Tick(double now, std::vector<Out>& out, std::vector<Summary>& done);
    bool Active(uint32_t seq) const;
    bool Any() const { return !throws_.empty(); }
    uint32_t Current() const { return throws_.empty() ? 0 : throws_.back().seq; }
    void Clear() { throws_.clear(); }

private:
    struct T {
        uint32_t seq = 0;
        Kind kind = Kind::Unknown;
        std::string weapon;
        double begin = 0;
        Vec3 standing;
        bool hasModel = false, modelLanding = false;
        Vec3 modelOrigin, modelVel, modelLandingPos;
        uint16_t modelFlightMs = 0;
        bool sent = false, fromEntity = false;
        double t0 = 0;  // the origin's time
        Vec3 origin, vel;
        std::vector<std::pair<double, Vec3>> samples;  // first few kept for the gravity fit
        double lastPointAt = -1;
        Vec3 last;
        double lastAt = 0;
        size_t nSamples = 0, nPoints = 0, nBounces = 0;
        std::vector<Point> pending;  // points to send (bounces between two ticks)
    };
    T* Get(uint32_t seq);
    void SendThrow(T& t, double now, std::vector<Out>& out);
    void Flush(T& t, bool reliable, std::vector<Out>& out);
    Summary Summarize(const T& t, double now, const char* why) const;
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
