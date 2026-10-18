// seats_test: unit tests for the seat rules of a car that three or four players share (src/game/seats.h): the
// ride-along seat (free here and claimed by no other player's state), the yield when two players took the same
// passenger seat (the lower id keeps it), the claim list the Lua side passes. Simulates the four games of a session
// (host + 3 guests) seeing one car. Prints PASS/FAIL per check; exits non-zero on any failure.
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "game/seats.h"

using namespace mc::game::seats;

namespace {

int g_failures = 0;

void Check(bool ok, const char* name, const std::string& detail = {}) {
    std::printf("%s  %-64s %s\n", ok ? "PASS" : "FAIL", name, detail.c_str());
    std::fflush(stdout);
    if (!ok) ++g_failures;
}

uint32_t Mask(std::initializer_list<int> freeSeats) {
    uint32_t m = 0;
    for (int s : freeSeats) m |= 1u << s;
    return m;
}

// One car as the four games of a session see it. Each player sits where his own game put him (`seatOf`); on every
// other game his puppet sits in that seat unless it is not seated there yet (`unseated`) or another human is in it.
struct Car {
    int count = 4;
    std::map<uint8_t, int> seatOf;          // player -> his seat on his own game (absent: not in the car)
    std::map<uint8_t, bool> unseated;       // his puppet is not seated on the other games yet (hidden, loading)

    // Seats free on player `viewer`'s game: nobody he sees sits there.
    uint32_t FreeOn(uint8_t viewer) const {
        uint32_t m = 0;
        for (int s = 0; s < count; ++s) {
            bool taken = false;
            for (auto [p, seat] : seatOf) {
                if (seat != s) continue;
                if (p == viewer) taken = true;
                else if (!unseated.count(p) || !unseated.at(p)) taken = true;
            }
            if (!taken) m |= 1u << s;
        }
        return m;
    }
    // The claims player `viewer` gets from the others' states.
    std::vector<Claim> ClaimsFor(uint8_t viewer) const {
        std::vector<Claim> out;
        for (auto [p, seat] : seatOf)
            if (p != viewer) out.push_back({p, seat});
        return out;
    }
};

void TestRideSeat() {
    Check(RideSeat(4, Mask({1, 2, 3}), {}) == 1, "ride: the first free passenger seat");
    Check(RideSeat(4, Mask({0, 1, 2, 3}), {}) == 1, "ride: never the driver seat");
    Check(RideSeat(4, Mask({1, 2, 3}), {{2, 1}}) == 2, "ride: a seat another player's state names is skipped");
    Check(RideSeat(4, Mask({2, 3}), {{1, 1}, {2, 2}}) == 3, "ride: free here but claimed: the next one");
    Check(RideSeat(4, Mask({1}), {{3, 1}}) == -1, "ride: the only free seat is claimed: none");
    Check(RideSeat(2, Mask({1}), {}) == 1 && RideSeat(2, Mask({}), {}) == -1, "ride: a two-seater");
    Check(RideSeat(4, Mask({1, 2, 3}), {{2, kGettingIn}}) == 1, "ride: a player getting in claims no seat");
    Check(RideSeat(99, 0xFFFFFFFFu, {}) == 1 && RideSeat(-3, 0xFF, {}) == -1, "ride: bad seat counts are clamped");
}

void TestYield() {
    Check(YieldSeat(2, 1, 4, Mask({2, 3}), {{1, 1}}) == 2, "yield: a lower id in our seat: we slide to a free one");
    Check(YieldSeat(1, 1, 4, Mask({2, 3}), {{2, 1}}) == kStay, "yield: a higher id in our seat: we keep it");
    Check(YieldSeat(3, 1, 4, Mask({2, 3}), {{2, 1}, {0, 0}, {1, 2}}) == 3,
          "yield: the free seat another player claims is skipped");
    Check(YieldSeat(3, 1, 4, Mask({}), {{2, 1}}) == kNoSeat, "yield: no seat left: kNoSeat");
    Check(YieldSeat(2, 0, 4, Mask({1, 2, 3}), {{1, 0}}) == kStay, "yield: the wheel is not this rule's");
    Check(YieldSeat(2, 2, 4, Mask({1, 3}), {{1, 1}, {3, 3}}) == kStay, "yield: nobody else in our seat: stay");
}

void TestParse() {
    const auto c = ParseClaims("0:0 2:1 3:9 x:1 1:-2 4:3");
    Check(c.size() == 3 && c[0].player == 0 && c[0].seat == 0 && c[1].player == 2 && c[1].seat == 1 &&
              c[2].player == 4 && c[2].seat == 3,
          "claims: parsed, seat 9 and garbage skipped");
    Check(ParseClaims("").empty(), "claims: empty list");
}

// Host (0) drives; guest 1 sits in seat 1 on his game; guest 3 presses E while guest 1's puppet is not seated on
// guest 3's game yet. Before: guest 3 took seat 1 too. Now: seat 2.
void TestThreePlayersOneCar() {
    Car car;
    car.seatOf[0] = 0;
    car.seatOf[1] = 1;
    car.unseated[1] = true;  // guest 1's puppet is not in its seat on the others' games yet
    const uint32_t free3 = car.FreeOn(3);
    Check((free3 & Mask({1})) != 0, "4 players: seat 1 looks free on guest 3's game (puppet not seated)");
    const int s3 = RideSeat(car.count, free3, car.ClaimsFor(3));
    Check(s3 == 2, "4 players: guest 3's ride along takes seat 2, not guest 1's", std::to_string(s3));
    car.seatOf[3] = s3;
    car.unseated[1] = false;
    // guest 2 rides along as well: seat 3
    const int s2 = RideSeat(car.count, car.FreeOn(2), car.ClaimsFor(2));
    Check(s2 == 3, "4 players: guest 2 gets the last seat", std::to_string(s2));
    car.seatOf[2] = s2;
    // nobody yields: every seat has one player
    bool stays = true;
    for (uint8_t p = 1; p <= 3; ++p)
        stays &= YieldSeat(p, car.seatOf[p], car.count, car.FreeOn(p), car.ClaimsFor(p)) == kStay;
    Check(stays, "4 players: driver + 3 passengers, nobody yields");
    // a fifth would-be passenger (a guest re-joining with a new id) finds no seat
    Check(RideSeat(car.count, car.FreeOn(4), car.ClaimsFor(4)) == -1, "4 players: a full car offers no seat");
}

// Guests 1 and 2 press E at the same moment: each game saw seat 1 free, both sit in it. Guest 2 (higher id) slides to
// seat 2; guest 1 keeps seat 1; after the slide the claims agree on every game.
void TestSimultaneousRide() {
    Car car;
    car.count = 4;
    car.seatOf[0] = 0;
    const int a = RideSeat(car.count, car.FreeOn(1), car.ClaimsFor(1));
    const int b = RideSeat(car.count, car.FreeOn(2), car.ClaimsFor(2));
    Check(a == 1 && b == 1, "simultaneous: both pick seat 1 (neither saw the other yet)");
    car.seatOf[1] = a;
    car.seatOf[2] = b;
    car.unseated[1] = car.unseated[2] = true;  // each one's puppet cannot sit in a seat the local player holds
    const int y1 = YieldSeat(1, 1, car.count, car.FreeOn(1), car.ClaimsFor(1));
    const int y2 = YieldSeat(2, 1, car.count, car.FreeOn(2), car.ClaimsFor(2));
    Check(y1 == kStay, "simultaneous: the lower id keeps seat 1");
    Check(y2 == 2, "simultaneous: the higher id slides to seat 2", std::to_string(y2));
    car.seatOf[2] = y2;
    car.unseated[1] = car.unseated[2] = false;
    Check(YieldSeat(2, 2, car.count, car.FreeOn(2), car.ClaimsFor(2)) == kStay &&
              YieldSeat(1, 1, car.count, car.FreeOn(1), car.ClaimsFor(1)) == kStay,
          "simultaneous: settled, no more slides");
    // three at once in a two-seater + driver: the highest finds no seat and stays (his puppet hidden elsewhere)
    Car small;
    small.count = 2;
    small.seatOf[0] = 0;
    small.seatOf[1] = 1;
    small.seatOf[3] = 1;
    small.unseated[1] = small.unseated[3] = true;
    Check(YieldSeat(3, 1, small.count, small.FreeOn(3), small.ClaimsFor(3)) == kNoSeat &&
              YieldSeat(1, 1, small.count, small.FreeOn(1), small.ClaimsFor(1)) == kStay,
          "simultaneous: two-seater, the higher id has no seat to go to");
}

}  // namespace

int main() {
    TestRideSeat();
    TestYield();
    TestParse();
    TestThreePlayersOneCar();
    TestSimultaneousRide();
    std::printf("%s: %d failure(s)\n", g_failures ? "FAILED" : "OK", g_failures);
    return g_failures ? 1 : 0;
}
