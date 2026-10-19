#pragma once
// Car damage on every game (docs/game-api.md 24, docs/protocol.md "Car damage"). No game code here: the native
// reads and the game calls are in car_damage_sync.cpp, the Lua glue in car_damage.lua; this is the part that can be
// unit-tested without the game (tests/car_damage_test.cpp): the snapshot a car's damage is packed into, the wire
// format, the diff the authority sends, the raise-only merge a copy applies, repair detection and what a copy reports.
//
// What a snapshot holds (found statically in the exe, docs/re-notes.md 11, NOT checked in game):
//   parts   the car's deform parts (entity + 0x100, the list C_WrapperCar::GetPartCount counts, the same list
//           C_WrapperActorDeform's Get/SetPartEnergy index): per part its damage, 0 = intact .. 254 = no energy left
//           (1 - energy / its maximum), 255 = gone (the part's flag 0x200000: broken off / destroyed). Body panels,
//           doors, hood, trunk, bumpers, lights, windows, the engine (part type 13, C_WrapperCar::GetMotorDamage)
//           and the wheels' parts are in it; which type is which is not known beyond the engine.
//   wheels  per wheel (vehicle part + 0x4F0: axles of two): 0 ok, 1 tyre burst (flag 0x10), 2 wheel off (0x40)
//   flags   exploded (entity + 0x1280 bit 0x10000: C_WrapperCar::GetDamage returns 100 then), burning (entity +
//           0x1EF0 bit 0x80: the countdown to the explosion runs, burnLeft s left, entity + 0x1FB8)
// Authority: the driver's game for a player car (the last driver's while it is parked), the host for his traffic.
// Everyone else shows a copy and only ever raises its damage to the authority's (raise-only); a repair on the
// authority (a part's damage went down, a tyre came back, the explosion was undone) moves the car's epoch on, and a
// copy that sees a newer epoch repairs itself first. Damage a copy takes on its own game (a guest shooting the host's
// tyres) is reported to the authority, which applies it to the real car and sends it back to everyone.
// Echoes (0.15.0 in-game test: a copy's wreck came back to the host as a report, and the host read the drop that
// followed as a repair): only damage a copy really took on its own game is reported. A copy never reports anything
// while it or the authority's car is exploded, never reports the exploded flag (it never blows up by itself), and
// for echoQuiet s after we wrote cascading damage on it (an explosion, a fire, a wheel, a part broken off) whatever
// new damage it shows is taken as the game's reaction to our own writes (absorbed, not reported). The holder ignores
// reports of a car that is exploded on its game and the exploded flag in any report, and a car that stays exploded is
// never "repaired" (a wreck's parts change by themselves); for repairGrace s after it wrote damage on its own car
// (a report, a blast, a new entity) a drop is no repair either.
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace mc::game::car_damage {

constexpr uint16_t kMsgDamage = 0x0250;   // CarDamage: authority -> everyone (Broadcast; traffic: host -> guests)
constexpr uint16_t kMsgReport = 0x0251;   // CarDamageReport: a copy's game -> the authority (SendTo / SendToHost)
constexpr uint16_t kMsgRequest = 0x0252;  // CarDamageRequest: a copy's game -> the authority: send me the whole state
// 0x0253 is CarLook (game/car_look.h kMsgLook)

constexpr uint8_t kGone = 255;       // part damage: the part is gone (broken off, destroyed)
constexpr uint8_t kMaxDamage = 254;  // part damage: no energy left, the part still there
constexpr size_t kMaxParts = 250;
constexpr size_t kMaxWheels = 16;
constexpr uint8_t kTolerance = 3;    // quanta (1/254) a copy may differ before anything is set or reported

enum Flag : uint8_t { kExploded = 1, kBurning = 2 };
enum Wheel : uint8_t { kWheelOk = 0, kWheelBurst = 1, kWheelOff = 2 };

// 0..254 from a part's energy and its maximum (a bad maximum reads as intact)
uint8_t Quantize(float energy, float max);
// the energy that stands for damage d (0..254) of a part with that maximum
float EnergyOf(uint8_t d, float max);

struct Snapshot {
    uint32_t layout = 0;          // Layout() of the part types: the same model has the same part list
    std::vector<uint8_t> parts;   // damage per part (see above)
    std::vector<uint8_t> wheels;  // Wheel per wheel
    uint8_t flags = 0;            // Flag bits
    float burnLeft = 0;           // s to the explosion while burning
    bool Damaged() const;         // anything at all
    std::string Describe() const; // "parts 3/41 (max 120, gone 1) tyres 1 burst exploded" for the logs
};

// FNV-1a over the part count and each part's type: two cars with the same layout can exchange part indexes
uint32_t Layout(const std::vector<int32_t>& types);

// A car on the network. Player cars: their car key (keyOwner, vehId: the first driver and the car's GUID on his
// game, docs/protocol.md "Shared cars"). The host's traffic: its net id (game/traffic_model.h) on the wire; inside the
// host's own Book it is kept by his entity id (kind kHostTraffic) and translated at the network edge.
struct CarRef {
    enum Kind : uint8_t { kPlayer = 0, kTraffic = 1, kHostTraffic = 2 };
    uint8_t kind = kPlayer;
    uint8_t owner = 0;  // kPlayer: keyOwner
    uint32_t id = 0;    // kPlayer: vehId; kTraffic: net id; kHostTraffic: the host's entity id
    bool operator<(const CarRef& o) const;
    bool operator==(const CarRef& o) const { return kind == o.kind && owner == o.owner && id == o.id; }
    std::string Text() const;  // "3:12345" (player), "t17" (traffic net), "h56330" (host traffic entity)
    static bool Parse(const std::string& s, CarRef& out);  // the Text() forms
};

// One CarDamage or CarDamageReport body (the same layout for both).
struct Update {
    CarRef ref;
    uint16_t epoch = 0;
    uint8_t flags = 0;        // Flag bits of the car, plus the message bits below
    float burnLeft = 0;
    uint32_t layout = 0;
    uint8_t partCount = 0;    // parts the car has (the list below names only some)
    std::vector<uint8_t> wheels;
    std::vector<std::pair<uint8_t, uint8_t>> parts;  // (index, damage)
    bool full = false;        // every damaged part is listed: the rest are intact
    bool repaired = false;    // the epoch moved on with this one (the authority's car was repaired)
};
// message bits next to the car flags in the flags byte
constexpr uint8_t kMsgFull = 0x10, kMsgRepaired = 0x20;

std::vector<uint8_t> Write(const Update& u);         // the message body (wire format: docs/protocol.md)
bool Read(const uint8_t* data, size_t len, Update& u);  // false: malformed (nothing to apply)
std::vector<uint8_t> WriteRequest(const CarRef& ref);
bool ReadRequest(const uint8_t* data, size_t len, CarRef& ref);

// The parts that changed from `was` to `now` (index, new value): what the authority sends. A different layout or
// part count sends every damaged part (full).
std::vector<std::pair<uint8_t, uint8_t>> Diff(const Snapshot& was, const Snapshot& now);
// A full update of `s` (every damaged part).
Update Full(const CarRef& ref, uint16_t epoch, const Snapshot& s);
// `u` merged into `into` raise-only (u's parts, wheels and flags only ever add damage); a full update of a different
// layout replaces it. Returns true when anything rose.
bool MergeUp(Snapshot& into, const Update& u);
// The car was repaired from `was` to `now` (same layout): a part lost 24 or more quanta of damage, a gone part came
// back, a tyre came back or the explosion was undone. Small drops (rounding) are no repair.
bool Repaired(const Snapshot& was, const Snapshot& now);

// What to do on one car this round (car_damage_sync.cpp carries it out on the game).
struct Apply {
    uint32_t entity = 0;
    bool repair = false;                               // C_WrapperCar::Repair first (Lua)
    std::vector<std::pair<uint8_t, uint8_t>> parts;    // set part index to that damage (255: energy 0)
    std::vector<std::pair<uint8_t, uint8_t>> wheels;   // wheel index -> kWheelBurst / kWheelOff
    bool burn = false;                                 // start the countdown (burnLeft s)
    float burnLeft = 0;
    bool explode = false;                              // blow it up now
    bool copy = false;  // on a copy: its explosion only through the one explode routine (ExplodeOnce), never native
    bool Empty() const { return !repair && parts.empty() && wheels.empty() && !burn && !explode; }
};
// What a copy (`local`) needs to show `auth`, raise-only; `done` remembers what was set already (so a part the game
// does not take is not set again every round until the authority raises it further).
struct Applied {
    std::vector<uint8_t> parts, wheels;
    bool burn = false, explode = false;
    void Clear() { parts.clear(); wheels.clear(); burn = explode = false; }
};
Apply Plan(const Snapshot& local, const Snapshot& auth, Applied& done);
// What `local` has beyond `auth` (index, local value), the wheels and flags it has beyond: a copy's own damage.
Update Excess(const CarRef& ref, uint16_t epoch, const Snapshot& local, const Snapshot& auth);
bool Empty(const Update& u);  // no parts, no wheel and no flag beyond

// ---- a copy never blows up by itself --------------------------------------------------------------------------------
// Only the game that holds a car's damage has a real explosion of it. On every other game the car (a copy, or a real
// car another game holds) blows up only when the holder's game says so: car damage's exploded flag (CarDamage) or the
// explosions sync's CarExploded, whichever arrives first, and a wrecked car coming back (CarParked). All of them end in
// one routine (car_damage.lua D.Explode: SetExploded and a visual-only picture with the damage guard's zone), which
// asks this book first: the first ask for an entity goes through, every later one is refused. A copy that burns keeps
// burning (the flames show) but its countdown is held above zero (HoldFire) so it never ends in a blast of its own.
class ExplodeOnce {
public:
    bool First(uint32_t entity) { return entity != 0 && done_.insert(entity).second; }
    bool Done(uint32_t entity) const { return done_.count(entity) != 0; }
    void Clear() { done_.clear(); }  // a new world (entity ids are per world), the session's end
    size_t Size() const { return done_.size(); }

private:
    std::set<uint32_t> done_;
};
constexpr float kHoldFire = 3.0f;  // s: a burning copy's countdown is put back up to this whenever it falls below
// The countdown value a held copy gets: `left` when it is above `hold`, else `hold` (also for NaN / negative values).
float HoldFire(float left, float hold = kHoldFire);

// ---- another game's explosion on the cars we hold ------------------------------------------------------------------
// Cars stay destroyable by anyone's explosion: an explosion on another game reaches us as Explosion (0x0240, a
// damage-free picture here), and the game that HOLDS a car's damage puts the blast's effect on its real car itself
// (the native writes: parts, tyres, the fire, the explosion); the usual CarDamage then brings it to every copy. The
// falloff is ours (the game's own explosion damage curve was not found statically; UNVERIFIED constants):
//   within kBlastKill (4 m)            the car explodes (native, after ExplodeOnce)
//   kBlastKill .. kBlastReach (10 m)   every part takes up to kBlastParts quanta, linear to 0 at kBlastReach; within
//                                      kBlastTyres (6 m) every tyre bursts; within kBlastFire (5 m) the car catches
//                                      fire (kBlastBurn s to its own explosion)
//   a molotov (fire only)              within kFireReach (3 m) the car catches fire and its parts take up to
//                                      kFireParts quanta; no explosion
// Players and NPCs are not touched by this (their damage is decided once, by the sender's game).
constexpr float kBlastKill = 4.0f, kBlastReach = 10.0f, kBlastTyres = 6.0f, kBlastFire = 5.0f, kBlastBurn = 8.0f;
constexpr int kBlastParts = 200;
constexpr float kFireReach = 3.0f, kFireBurn = 10.0f;
constexpr int kFireParts = 40;
struct BlastEffect {
    bool explode = false;
    bool burn = false;
    float burnLeft = 0;
    uint8_t parts = 0;  // quanta every part is raised by (to at most kMaxDamage)
    bool tyres = false; // every tyre bursts
    bool Empty() const { return !explode && !burn && parts == 0 && !tyres; }
};
// The effect of a blast `distance` m away; `fireOnly`: a molotov's fire (Explosion kind 2) rather than a blast.
BlastEffect BlastOn(float distance, bool fireOnly);
// What `local` becomes under `e` (raise-only: nothing gets better); Plan(local, BlastTarget(...)) gives the writes.
Snapshot BlastTarget(const Snapshot& local, const BlastEffect& e);

// ---- the per-game book of every car's damage ------------------------------------------------------------------------
enum class Role : uint8_t { Authority, Copy };

struct CarInput {        // one car this round (from the Lua glue and the native read)
    CarRef ref;
    uint32_t entity = 0;  // this game's entity id for it
    Role role = Role::Copy;
    int authPeer = -1;    // Copy: the player whose game is the authority (-1 unknown: nobody to ask or tell)
    bool quiet = false;   // Copy: moved by SetPos lately (pinned / snapped): damage it takes now is not reported
    bool read = false;    // the snapshot below could be read
    Snapshot local;
};

struct Outbox {
    std::vector<Update> broadcast;                    // CarDamage to everyone (traffic: host -> guests)
    std::vector<std::pair<int, Update>> to;           // CarDamage to one player (answers to requests)
    std::vector<std::pair<int, Update>> reports;      // CarDamageReport to the authority
    std::vector<std::pair<int, CarRef>> requests;     // CarDamageRequest to the authority
    std::vector<Apply> applies;
    std::vector<uint32_t> holdFire;                   // copies that burn here: their countdown is held (HoldFire)
    std::vector<std::string> logs;
    std::vector<std::string> trace;                   // "CDx ..." record bodies (type first)
};

struct BookConfig {
    double fullEvery = 15.0;     // s: a damaged car's whole state again (late copies, lost ones)
    double requestEvery = 3.0;   // s between requests while a copy has no state from the authority
    double settle = 0.4;         // s a copy's own damage must stand before it is reported
    double repairWait = 0.6;     // s after a Repair before a copy reports or plans again
    double quietRepairGap = 20;  // s between two repairs of a copy that took damage while it was moved by SetPos
    int quietRepairMin = 30;     // quanta of such damage (summed) before a copy is repaired and set again
    double forget = 30.0;        // s a car may be missing from the rounds before it is forgotten
    double blastQuiet = 1.5;     // s after we put another game's blast on our car: copies' reports of it are dropped
    double echoQuiet = 1.5;      // s after cascading writes on a copy (explode, fire, wheels, a gone part): what it
                                 // shows new is our own writes' echo, not reported
    double repairGrace = 1.0;    // s after the holder wrote damage on its own car: a drop is not read as a repair
};

class Book {
public:
    explicit Book(BookConfig cfg = {}) : cfg_(cfg) {}
    void OnDamage(const Update& u, int from, double now);   // CarDamage arrived
    void OnReport(const Update& u, int from, double now);   // CarDamageReport arrived (we may be the authority)
    void OnRequest(const CarRef& ref, int from);            // CarDamageRequest arrived
    // We put another game's blast on our car `ref` (the holder): the copies' reports of the same blast (their own
    // games' real blast hit them too, reported 0.4 s later) are dropped for blastQuiet s, so it is not applied twice.
    void OnRemoteBlast(const CarRef& ref, double now);
    void Round(const std::vector<CarInput>& cars, double now, Outbox& out);
    void OnPlayerLeft(int id);
    void Clear() { cars_.clear(); }

    struct Stats {
        uint32_t cars = 0, authority = 0, copies = 0, damaged = 0;
        uint64_t sent = 0, fulls = 0, reports = 0, requests = 0, received = 0, applied = 0, repairs = 0,
                 explosions = 0, reportsIn = 0, blasts = 0, blastDropped = 0,
                 echoesAbsorbed = 0,  // copy: rounds whose new damage was taken as an echo of our writes (not sent)
                 echoesDropped = 0;   // holder: reports dropped as echoes (car exploded here, exploded flag)
    };
    const Stats& GetStats() const { return stats_; }
    // test access
    bool Known(const CarRef& ref, Snapshot* auth = nullptr, uint16_t* epoch = nullptr) const;

private:
    struct Car {
        uint32_t entity = 0;
        Role role = Role::Copy;
        int authPeer = -1;
        double seen = 0;
        uint16_t epoch = 0;          // the newest epoch known for this car
        // authority
        bool haveSent = false;
        Snapshot sent;               // what everyone has been told
        Snapshot prev;               // last round's own read (repair detection)
        bool havePrev = false;
        double fullAt = -1e9;
        std::set<int> wantFull;      // players that asked for the whole state
        Snapshot raise;              // reports to apply to our car (raise-only)
        bool haveRaise = false;
        // copy
        bool haveAuth = false;       // a full state from the authority arrived
        Snapshot auth;
        uint16_t appliedEpoch = 0;
        bool appliedEpochSet = false;
        Applied done;
        double requestAt = -1e9;
        double waitUntil = 0;        // after a Repair
        double excessAt = -1;        // own damage first seen
        Snapshot reported;           // what we reported beyond auth
        int quietExcess = 0;
        double quietRepairAt = -1e9;
        bool layoutWarned = false;
        bool fresh = false;          // a new entity: checked once for damage the car does not have
        double blastUntil = -1e9;    // authority: copies' reports dropped until then (OnRemoteBlast)
        double wroteAt = -1e9;       // authority: we last wrote damage on our own car (repairGrace)
        double echoUntil = -1e9;     // copy: what it shows new until then is an echo of our cascading writes
    };
    void AuthorityRound(const CarInput& in, Car& c, bool rebound, double now, Outbox& out);
    void CopyRound(const CarInput& in, Car& c, bool rebound, double now, Outbox& out);

    BookConfig cfg_;
    std::map<CarRef, Car> cars_;
    Stats stats_;
};

}  // namespace mc::game::car_damage
