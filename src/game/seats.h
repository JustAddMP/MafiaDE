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

}  // namespace mc::game::seats
