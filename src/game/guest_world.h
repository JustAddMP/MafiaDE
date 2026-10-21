#pragma once
// The guest's own world off (docs/host-authority.md section 1, work package A). Under host authority the host's game
// owns traffic, police and pedestrians; a guest's game shows only copies of them. So on a guest's game (never the
// host's) the game's own world is switched off for the whole session:
//   - ambient traffic spawning off   game.traffic:SetEnableAmbientTrafficSpawning(false)
//   - police spawning prevented      game.police:PreventPoliceSpawning(true)
//   - the pedestrian budget 0        game.traffic:SetMaxHumanElements(0) (its old value read first from
//                                    [traffic manager + 0x100], game/guest_world_sync.cpp), or the fallback:
//                                    BlockSpawnersInSphere(pos, 150, true) every 20 m we move (the 0.16 ped_sync code)
//   - the cars and humans the game already made: hidden (Deactivate, + ShowModel(false) for humans) at once while the
//     loading screen is up and on the first ticks in the world, later only while out of view.
// Every switch is applied again after every world load (a load may reset them) and every kReassert s (other glue may
// switch ambient traffic back on: traffic_sync.lua's 0.16 region code does in "our own region"). When the session
// ends (or the guest leaves) everything goes back: spawning on, the budget to the value read, the spawner spots
// unblocked, every hidden entity shown.
//
// Pure logic, no game or OS calls (tests/guest_world_test.cpp). game/guest_world_sync.cpp feeds it once per script
// tick and carries out its Actions through src/game/guest_world.lua; the hide verdict (Judge) is asked by that glue
// for every car and human it scans (the Lua binding mc_gw_judge).
//
// The one rule that matters most: the hide verdict never touches anything of ours. 0.16's traffic_sync ClearLocal hid
// ped_sync's copy spawner (a crewed car 40 m under the player whose driver reads like a traffic generator's) and the
// pedestrian coverage of the 2026-10-20 session A fell to 18 %. Here an entity is "ours" when any of the glue's sets
// says so (puppets, the puppet factory, traffic copies and their crews, ped copies and their spawner, npc_sync's copies,
// replicas and their spawner, player cars, our own car) or when it looks like a spawner of ours (a fixed car, a car
// far below us, a car while one of our factories is loading one, a crewed car at a loading copy's spawn spot, an
// entity first seen moments ago).
#include <cstdint>
#include <string>
#include <vector>

namespace mc::game::guest_world {

struct Vec3 {
    float x = 0, y = 0, z = 0;
};

// ---- tuning --------------------------------------------------------------------------------------------------------
constexpr double kReassert = 5.0;       // s: the switches applied again while in the world
constexpr double kAtOnceFor = 3.0;      // s after the first tick in a world: hidden regardless of view
constexpr double kScanAtOnce = 0.25;    // s between scans while hiding at once
constexpr double kScanInterval = 1.0;   // s between scans later (and the GW trace)
constexpr double kFreshFor = 2.0;        // s: an entity first seen this recently is not hidden (our glue registers its
                                        //    own spawns within a tick or two; a spawner car is seen before that)
constexpr float kBlockRadius = 150.0f;  // m: BlockSpawnersInSphere radius (fallback)
constexpr float kBlockStep = 20.0f;     // m: blocked again when we moved this far from every blocked spot
constexpr double kBlockRepeat = 10.0;   // s: the spot we stand on blocked again (spawners streamed in meanwhile)
constexpr size_t kBlockMax = 128;       // spots remembered for the unblock at the end
constexpr float kBlockForgetFar = 300.0f;  // m: past kBlockMax, the oldest spot this far from us is unblocked early
constexpr float kBelowUs = 20.0f;       // m: a car this far below our player is a spawner of ours (they live 40 m under
                                        //    a player: player_sync / npc_sync / ped_sync SpawnCar(z - 40))
constexpr float kCountCarRange = 150.0f;   // m: own moving cars counted for world.guest_own_cars
constexpr float kCountPedRange = 80.0f;    // m: own ambient humans counted for world.guest_own_peds
constexpr float kMovingSpeed = 1.0f;       // m/s: a car faster than this is a moving car (the count)
constexpr int kBudgetMax = 4096;           // a budget read above this (or below 0) is not believed

// ---- what the glue knows each tick ------------------------------------------------------------------------------------
enum class Phase : uint8_t {
    None = 0,     // no world: the menus, a world being torn down
    Loading = 1,  // the loading screen is up (the active player exists)
    InWorld = 2   // in a loaded world (player_sync.lua M.Player())
};
const char* PhaseName(Phase p);

// How the guest's own pedestrians are switched off (ini `guest_world_peds`).
enum class PedSwitch : uint8_t {
    Budget = 0,  // SetMaxHumanElements(0) (default); falls back to Block when the call fails
    Block = 1,   // BlockSpawnersInSphere around us every 20 m (when the probe shows the budget stops our own spawns)
    Off = 2      // the pedestrians are left alone (only hidden ones)
};
PedSwitch PedSwitchOf(const std::string& ini);  // "budget" / "block" / "off" (anything else: Budget)
const char* PedSwitchName(PedSwitch s);

struct Frame {
    double now = 0;
    bool active = false;   // the session is running
    bool isHost = false;   // we host it (then nothing is ever switched)
    bool enabled = true;   // ini `guest_world` (default on)
    Phase phase = Phase::None;
    uint32_t worldKey = 0; // the glue's world key (save id | first in-world tick), hashed; 0 unknown
    bool havePos = false;
    Vec3 pos;              // our player (or his car)
    bool otherOn = false;  // another glue switched ambient traffic on since the last tick (re-apply now)
};

// What the glue does this tick.
enum class HideMode : uint8_t {
    None = 0,      // nothing hidden (host, not applied, session over)
    AtOnce = 1,    // loading screen / the first kAtOnceFor s in a world: hidden in view or not
    OutOfView = 2  // later: hidden only while out of view
};
const char* HideModeName(HideMode m);

struct Actions {
    bool readBudget = false;   // read the budget before switching (first apply in a world, Budget mode)
    bool switchOff = false;    // traffic spawning off, police prevented
    bool budgetZero = false;   // SetMaxHumanElements(0) (with switchOff, Budget mode)
    bool restore = false;      // spawning back on, police allowed, every hidden entity shown
    bool budgetRestore = false;  // SetMaxHumanElements(budgetValue) (with restore)
    int budgetValue = -1;
    bool scan = false;         // scan the cars and humans this tick (hide, count)
    HideMode hide = HideMode::None;
    std::vector<Vec3> block, unblock;  // BlockSpawnersInSphere(pos, kBlockRadius, true / false)
    bool trace = false;        // GW record due (1 Hz, guest)
    const char* why = "";      // for the log: why the switches are applied / restored
};

// The state machine: when to switch, re-apply per world, restore at the end; the fallback.
class Policy {
public:
    explicit Policy(PedSwitch peds = PedSwitch::Budget, int budgetDefault = -1);
    void SetPedSwitch(PedSwitch s);  // the ini value (main thread, before Tick); Block sticks once a call failed
    Actions Tick(const Frame& f);

    // results of the last Actions
    void OnBudgetRead(int value);  // the game's value before we set it (-1: not readable)
    void OnSwitched(bool trafficOk, bool policeOk, bool budgetOk);
    void OnRestored(bool budgetOk);

    bool Applied() const { return applied_; }        // our switches stand now
    bool RestorePending() const { return restorePending_; }
    PedSwitch PedMode() const { return effectivePeds_; }
    bool BudgetFailed() const { return budgetFailed_; }
    int SavedBudget() const { return savedBudget_; }  // -1 unknown
    int BudgetState() const { return budgetState_; }  // 0 untouched, 1 set 0, 2 restored, 3 failed, 4 restore unknown
    uint32_t Worlds() const { return worlds_; }        // worlds seen (loads)
    uint32_t Applies() const { return applies_; }      // first applies (one per world while a guest)
    uint32_t Reasserts() const { return reasserts_; }
    uint32_t Restores() const { return restores_; }
    double InWorldFor(double now) const { return inWorldSince_ < 0 ? -1 : now - inWorldSince_; }
    size_t BlockedSpots() const { return spots_.size(); }
    Phase LastPhase() const { return phase_; }

private:
    void BlockTick(const Frame& f, Actions& a);
    void TakeSpots(Actions& a);

    PedSwitch peds_, effectivePeds_;
    int budgetDefault_;
    bool budgetFailed_ = false;
    Phase phase_ = Phase::None;
    uint32_t inWorldKey_ = 0;
    uint32_t worlds_ = 0, appliedWorld_ = 0, applies_ = 0, reasserts_ = 0, restores_ = 0;
    bool applied_ = false, restorePending_ = false, wasGuest_ = false, askedBudget_ = false;
    int savedBudget_ = -1, budgetState_ = 0;
    double inWorldSince_ = -1, worldStart_ = -1, reassertAt_ = 0, scanAt_ = -1e9, traceAt_ = -1e9;
    std::vector<Vec3> spots_;
    double spotAt_ = -1e9;
};

// ---- the hide verdict ---------------------------------------------------------------------------------------------
// Facts the glue reads about one entity (bits). The "ours" bits come from the other glue's tables; any of them keeps it.
enum Fact : uint32_t {
    kCar = 1u << 0,
    kHuman = 1u << 1,
    kActive = 1u << 2,          // IsActive() (an inactive one is the game's or already hidden)
    // ours (any keeps it)
    kPuppet = 1u << 3,          // player_sync's puppet (or a retired puppet entity)
    kPuppetFactory = 1u << 4,   // player_sync's puppet factory car / driver
    kTrafficCopy = 1u << 5,     // traffic_sync's copy of a host car (MafiaCoop.Traffic.copies)
    kCopyCrew = 1u << 6,        // a traffic copy's crew (MafiaCoop.Traffic.IsCopyCrew / npc_sync's tcrew)
    kPedOwned = 1u << 7,        // MafiaCoop.PedOwned (ped copies, adopted / hidden by ped_sync, its spawner's driver)
    kPedSpawner = 1u << 8,      // ped_sync's copy spawner car
    kNpcCopy = 1u << 9,         // npc_sync's spawned copy, replica or held NPC
    kNpcSpawner = 1u << 10,     // npc_sync's copy spawner car / driver
    kPlayerCar = 1u << 11,      // player_sync's car (a player's car or its copy, MafiaCoop.KeyOf)
    kOwnVehicle = 1u << 12,     // the car our player sits in
    kOurs = kPuppet | kPuppetFactory | kTrafficCopy | kCopyCrew | kPedOwned | kPedSpawner | kNpcCopy | kNpcSpawner |
            kPlayerCar | kOwnVehicle,
    // looks like a spawner of ours (keeps it too)
    kFixed = 1u << 13,          // car: IsCarPosFixed() (every spawner car is FixCarPos'd; traffic never is)
    kFactoryBusy = 1u << 14,    // car: one of our factories is obtaining / spawning its car right now
    kNearLoadingCopy = 1u << 15,  // car: within a few m of a traffic copy being spawned
    kFresh = 1u << 16,          // first seen less than kFreshFor s ago
    // what it is
    kGeneratorDriver = 1u << 17,  // car: seat 0 holds a traffic / police generator human (behavior_object_prime-NN-)
    kTramOrBoat = 1u << 18,     // car: a tram, train or boat (left alone this round, host-authority D4)
    kAmbient = 1u << 19,        // human: AI type 0, not in a vehicle, not a mission spawner's name (IsAmbientHuman's
                                //   name / AI / vehicle part)
    kMissionName = 1u << 20,    // human: behavior_object_prime-NN-<suffix> with a suffix (a mission spawner's)
    kInVehicle = 1u << 21,      // human: in a vehicle (a crew is hidden with its car)
    kInView = 1u << 22,         // in view of the camera (Judge computes it when the glue passes the camera)
    kSpawnedLate = 1u << 23,    // car: first seen when our switches had stood for kLateAfter s in this world: made
                                //   despite them (2026-10-21 one-PC runs: a police car with a generator driver
                                //   appeared next to the guest driving his car, kept in view 7-10 s,
                                //   world.guest_own_cars 1): hidden even in view, it was only just made
    kPolice = 1u << 24          // car: a police car (model *_p: shubert_e_six_p, ...). The guest's game has no police of
                                //   its own (the host's are copies); one made before the switches stood, met later far
                                //   from the start (the 05:37 / 05:51 one-PC runs: 450 m out for the leash, 13 m/s in
                                //   view for 6 s, world.guest_own_cars 1): hidden even in view
};
constexpr double kLateAfter = 10.0;  // s after our switches stood in a world (the glue sets kSpawnedLate)

enum class Verdict : uint8_t {
    KeepOurs = 0,     // ours (or looks like a spawner of ours)
    KeepOther = 1,    // not the guest's own ambient world (parked, mission, tram, inactive ...)
    KeepInView = 2,   // the guest's own ambient car / human, kept while in view
    Hide = 3          // the guest's own ambient car / human: hide it now
};
const char* VerdictName(Verdict v);
inline bool IsOwnAmbient(Verdict v) { return v == Verdict::KeepInView || v == Verdict::Hide; }

struct Entity {
    uint32_t facts = 0;
    Vec3 pos;
    float speed = 0;  // m/s
};

struct View {
    bool have = false;   // the camera was read
    Vec3 cam, dir;       // camera position, direction (need not be unit)
    bool haveMe = false;
    Vec3 me;             // our player
};

// Is p in view of the camera? Within nearDist always, beyond farDist never, else inside a cone of halfAngleDeg around
// the direction. No occlusion test (seen through a wall: it just waits longer). No camera: everything is in view.
bool InView(const View& v, const Vec3& p, float halfAngleDeg, float nearDist, float farDist);

// The verdict for one entity. `mode` from the Actions; `reason` (optional) a short word for the log.
Verdict Judge(const Entity& e, HideMode mode, const View& view, const char** reason = nullptr);

// Facts the verdict derives from the view (kInView, below us) before deciding; exposed for the tests and the glue.
uint32_t WithView(uint32_t facts, const Vec3& pos, const View& view);

// Counting for the GW trace / world.guest_own_cars / world.guest_own_peds: an own ambient car moving within
// kCountCarRange, an own ambient human within kCountPedRange, still active after this scan.
bool CountsAsOwnCar(const Entity& e, Verdict v, const View& view);
bool CountsAsOwnPed(const Entity& e, Verdict v, const View& view);

// Hash of the glue's world key (FNV-1a 32; 0 kept for "none").
uint32_t KeyHash(const std::string& s);

}  // namespace mc::game::guest_world
