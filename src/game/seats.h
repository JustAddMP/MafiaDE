#pragma once
// Seats of a car that three or four players share (driver + passengers, docs/protocol.md "Seats with several
// players"). Pure logic, no game or Lua calls: unit-tested by tests/seats_test.cpp; player_sync.lua (M.RideAlong,
// M.SeatTick) asks it through mc_rideseat / mc_yieldseat (src/game/player_sync.cpp).
//
// Every game sees the same car (the driver's car or its copy) with the same seat numbers, but each player picks his
// seat on his own game: a ride along (E next to a car somebody else drives) took "the first free passenger seat
// here", which on a game where another passenger's puppet was not seated yet (hidden, loading, a moment of lag) was
// the very seat that passenger already sat in on his own game. Two rules keep the seats apart:
//   - RideSeat: a passenger seat that is free here *and* no other player's state names (pp.seat of a puppet whose
//     state is in this car). The driver seat is never offered (the game would put the player at the wheel).
//   - YieldSeat: two players in the same passenger seat (both pressed E at the same moment, each game saw the seat
//     free): the lower player id keeps it, the other slides to another seat that is free and unclaimed (SlideToSeat),
//     or stays where he is when there is none (then the higher id's puppet is hidden on the others' games, as before).
// Seats are 0-based here (0 = driver), as GetSeatOwner counts them; GetOnVehicle / SlideToSeat take seat + 1.
#include <cstdint>
#include <string>
#include <vector>

namespace mc::game::seats {

constexpr int kMaxSeats = 8;   // GetSeatCount is clamped to this
constexpr int kGettingIn = 9;  // PlayerState seat 9: getting in or out, claims no seat

struct Claim {
    uint8_t player = 0;
    int seat = 0;  // 0 driver, 1.. passengers, kGettingIn = none
};

// freeMask: bit s set = seat s is free on this game (no human in it). -1 = no seat for a ride along.
int RideSeat(int seatCount, uint32_t freeMask, const std::vector<Claim>& claims);

// The local player (id `local`) sits in passenger seat `seat`. Returns the seat to slide to, kStay when he keeps his
// seat (nobody with a lower id claims it), kNoSeat when he should yield but there is no other seat.
constexpr int kStay = -1;
constexpr int kNoSeat = -2;
int YieldSeat(uint8_t local, int seat, int seatCount, uint32_t freeMask, const std::vector<Claim>& claims);

// "id:seat id:seat ..." (the Lua side's claim list) -> claims; malformed items are skipped.
std::vector<Claim> ParseClaims(const std::string& s);

// ---------------------------------------------------------------------------------------------------------------
// One wheel, one driver (the 2026-10-19 two-PC test: both players drove the same car for 100 s, its damage bounced
// between the games). When the driver's puppet leaves seat 0 on a game (thrown out, dead in the water, respawning),
// that game slides its own player from the passenger seat to the wheel, and the local player then "took the wheel";
// the other game did the same with its own reading, and neither gave way ("drives it too; we keep it").
// The rules, the same on every game:
//   - a seat-0 reading here while another player's state still names seat 0 of this car, and he already drove it on
//     this game, is the game's slide, not a takeover: we give way (Yield);
//   - both claim the wheel and neither drove it here before the other (they got in at the same moment, or each game
//     slid its own player over before it saw the other's state): the car's holder keeps it (the key's owner, the
//     player who first drove it: "1:4746250" -> player 1); when neither is the holder, the lower player id keeps it.
// The one who gives way reports seat 9 (getting in, claims nothing), slides to a free passenger seat or gets out
// (player_sync.lua M.WheelTick), and the keeper's game alone holds the car's damage (car_damage.lua follows the
// driver).
// ---------------------------------------------------------------------------------------------------------------
constexpr int kNobody = -1;

// Of two players claiming the wheel of a car whose key owner is `holder`: who keeps it.
uint8_t WheelKeeper(uint8_t holder, uint8_t a, uint8_t b);

enum class Wheel : uint8_t {
    Keep,   // we keep (or take) the wheel; the other player gives way on his game
    Yield,  // we give way: a slide, or the other one is the keeper
};
const char* WheelName(Wheel w);

// We (`local`) read seat 0 of a car (key owner `holder`) while `remote`'s state names seat 0 of it too. driverHere:
// who drives it on this game so far (local, remote, another id, or kNobody).
Wheel LocalWheel(uint8_t local, uint8_t holder, int driverHere, uint8_t remote);

// ---------------------------------------------------------------------------------------------------------------
// Where a remote player's car copy stands on this game (player_sync.lua M.CarTick, mc_copyplace / mc_asidespot).
// A copy is spawned where his car is; before this fix it waited ("another car stands where it is") for as long as any
// car of this game stood within kBlockDist of that spot, because a copy spawned into a car shoves it away (a shoved
// mission car failed the mission). After a reload both games load the same checkpoint and put each player into his
// own copy of the one mission car at the very same spot (Taxi cp_020: the guest's taxi 2 cm from the host's, the
// host in his): his car never "moved away" and his puppet stayed hidden (veh-hidden) for minutes. Now:
//   - blocked for less than kBlockWait: wait (the blocker is usually leaving, or his car is);
//   - blocked longer: the copy is spawned beside his car (AsideSpot, a free spot next to it) and held there, his
//     puppet seated in it; it is not driven to his transform while that is still within kReleaseDist of the
//     blocker (it would be pushed into it);
//   - his car clear of every other car by kReleaseDist: the copy goes onto his car (released: driven as usual,
//     the vehicle controller snaps it over).
// ---------------------------------------------------------------------------------------------------------------
constexpr float kBlockDist = 3.0f;      // m: a car this close to his car's spot blocks the copy (ADOPT_DIST in Lua)
constexpr double kBlockWait = 2.0;      // s blocked before the copy goes beside his car
constexpr float kReleaseDist = 4.5f;    // m: an aside copy goes onto his car once no other car is this close to it
constexpr float kAsideClear = 5.6f;     // m: an aside spot has no other car's centre this close (any direction: a
                                        // car's heading is not trusted for this, UNVERIFIED which axis is forward)

enum class Place : uint8_t {
    Wait,     // no copy yet: his spot is taken, wait a little
    AtCar,    // spawn (or keep) the copy at his car
    Aside,    // no copy yet, blocked too long: spawn it at the aside spot
    Hold,     // the copy stands aside: keep it there (not driven to his transform)
    Release,  // the copy stands aside and his car is clear now: drive it onto his car
};
const char* PlaceName(Place p);

// haveCopy: the copy exists (or is being spawned); aside: it was spawned / is held aside; blockerDist: distance from
// his car's spot to the nearest other car of this game (-1 none known); blockedFor: s since his spot was first seen
// blocked (0 when not blocked); spotFound: AsideSpot found a free spot.
Place PlaceCopy(bool haveCopy, bool aside, float blockerDist, double blockedFor, bool spotFound);

struct Pt {
    float x = 0, y = 0;
};
struct Spot {
    bool ok = false;
    int index = -1;  // which candidate (0 right, 1 left, 2 behind, 3 ahead, 4 far right, 5 far left)
    float x = 0, y = 0;
};
constexpr int kAsideCandidates = 6;
// The candidate spot `index` beside his car at tx,ty with heading hx,hy (need not be normalised; (0,0) = +x)
Pt AsideCandidate(float tx, float ty, float hx, float hy, int index);
// The first candidate that no other car (cars) stands on; `rejected` = bits of candidates the caller found unusable
// (a wall in the way, no ground).
Spot AsideSpot(float tx, float ty, float hx, float hy, const std::vector<Pt>& cars, uint32_t rejected = 0);

// "x,y x,y ..." (the Lua side's car list) -> points; malformed items are skipped.
std::vector<Pt> ParsePoints(const std::string& s);

}  // namespace mc::game::seats
