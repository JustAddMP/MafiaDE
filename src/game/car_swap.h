#pragma once
// A real car swapped for a copy, and how it comes back (docs/game-api.md 15 "Mission car back", docs/protocol.md
// "Shared cars"). A car that the local player has driven, or a mission car of this game, does not keep an NPC at its
// wheel (docs/game-api.md 7), so when a remote player drives it this game hides the real car (Deactivate) and shows a
// fresh copy (player_sync.lua M.Swap). The real one must come back, or this game's mission loses its car: the
// mission scripts reference that entity, not the copy. This machine decides when; the Lua glue does it
// (player_sync.lua M.SwapTick / M.Restore) and gives it what it sees every tick.
//
// Rules (unit-tested in tests/car_swap_test.cpp):
//   - Somebody drives the copy (a remote player, or we ourselves): nothing happens; the copy is the car.
//   - Nobody drives it (Parking): the copy is locked for the local player (he would get into the copy instead of his
//     own car). The real car comes back at the copy's place once
//       * the copy is empty: no puppet and not the local player in it, no remote player getting in/out or riding in
//         it (despawning a car removes whoever sits in it); NPCs alone in it are waited for at most kOtherWait,
//       * and it has come to rest: the last driver's CarParked arrived (the copy is put at that final transform
//         first), or without one the copy stood still for kStillTime once kParkFallback has passed (the message is
//         lost, the driver left the session), at the latest after kHardCap.
//     The session being over counts as parked (nobody will send one any more).
//   - CarParked said the car was destroyed: restored, then set exploded (RestoreWrecked; the glue uses SetExploded,
//     no second blast) so this game's mission sees what the driver's game saw.
//   - The copy could not be spawned: the real car comes back at once (Restore at the last known transform).
//   - A new world (checkpoint load, quit): the real car, if it still exists, is shown where it is (RestoreHere);
//     otherwise it is forgotten (Forget). The real car being gone at any time is Forget too.
// After any action the machine is Done; the next swap of that car starts a fresh one.
#include <cstdint>

namespace mc::game::carswap {

constexpr double kParkSlack = 3.0;     // s: a CarParked this long before the driver got out still counts (it is
                                       // reliable, his state is not: the two can arrive in either order)
constexpr double kParkFallback = 10.0; // s without a CarParked before a still copy is restored anyway
constexpr double kHardCap = 20.0;      // s: restored even if the copy never comes to rest
constexpr float kStillSpeed = 0.5f;    // m/s
constexpr double kStillTime = 1.0;     // s below kStillSpeed
constexpr double kOtherWait = 8.0;     // s waiting for NPCs (not players) to leave the copy

// Input bits (the Lua glue passes them as one number)
enum Bit : uint32_t {
    kBegin = 1 << 0,         // first step after a swap: start over
    kOrigValid = 1 << 1,     // the hidden real car still exists (the same entity)
    kCopyValid = 1 << 2,     // the copy stands in for it
    kCopyPending = 1 << 3,   // the copy is still being spawned
    kRemoteDriver = 1 << 4,  // a remote player sits at the copy's wheel
    kLocalDriver = 1 << 5,   // the local player does
    kRemoteAboard = 1 << 6,  // a remote player's state names this car without driving it (getting in/out, passenger)
    kPuppetInside = 1 << 7,  // a puppet is in one of the copy's seats
    kLocalInside = 1 << 8,   // the local player is in one of its seats
    kOtherInside = 1 << 9,   // somebody else (an NPC) is
    kWrecked = 1 << 10,      // the CarParked at parkedAt said the car was destroyed
    kWorldGone = 1 << 11,    // a new world: the old one's entities are gone or stale
    kSessionOver = 1 << 12,  // no session any more
};

struct Input {
    double now = 0;
    uint32_t bits = 0;
    float copySpeed = 0;   // m/s
    double parkedAt = -1;  // time of the latest CarParked for this car (received, or sent by us as its last driver)
    bool Has(uint32_t b) const { return (bits & b) != 0; }
};

enum class Phase : uint8_t { Driven, Parking, Done };
enum class Action : uint8_t { None, Restore, RestoreHere, RestoreWrecked, Forget };

struct Output {
    Action action = Action::None;
    bool lockCopy = false;    // keep the local player out of the copy
    Phase phase = Phase::Driven;
    const char* why = "";     // the reason for this step's state, constant text
    bool changed = false;     // phase, action or reason differ from the previous step (log/trace it)
};

const char* PhaseName(Phase p);
const char* ActionName(Action a);

class Machine {
public:
    Output Step(const Input& in);
    Phase phase() const { return phase_; }

private:
    Output Finish(Action a, const char* why);
    Phase phase_ = Phase::Driven;
    double parkingSince_ = -1;
    double stillSince_ = -1;
    double otherSince_ = -1;
    const char* lastWhy_ = nullptr;
};

}  // namespace mc::game::carswap
