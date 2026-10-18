#include "game/seats.h"

#include <algorithm>
#include <cstdio>
#include <sstream>

namespace mc::game::seats {

namespace {

bool Claimed(int seat, const std::vector<Claim>& claims, int exceptPlayer = -1) {
    for (const Claim& c : claims)
        if (c.seat == seat && static_cast<int>(c.player) != exceptPlayer) return true;
    return false;
}

bool Free(uint32_t mask, int seat) { return seat >= 0 && seat < 32 && (mask & (1u << seat)) != 0; }

}  // namespace

int RideSeat(int seatCount, uint32_t freeMask, const std::vector<Claim>& claims) {
    const int n = std::clamp(seatCount, 0, kMaxSeats);
    for (int s = 1; s < n; ++s)
        if (Free(freeMask, s) && !Claimed(s, claims)) return s;
    return -1;
}

int YieldSeat(uint8_t local, int seat, int seatCount, uint32_t freeMask, const std::vector<Claim>& claims) {
    if (seat <= 0 || seat >= kMaxSeats) return kStay;  // the wheel is player sync's driver rule, not this one
    bool lowerHasIt = false;
    for (const Claim& c : claims)
        if (c.seat == seat && c.player < local) lowerHasIt = true;
    if (!lowerHasIt) return kStay;
    const int n = std::clamp(seatCount, 0, kMaxSeats);
    for (int s = 1; s < n; ++s)
        if (s != seat && Free(freeMask, s) && !Claimed(s, claims)) return s;
    return kNoSeat;
}

std::vector<Claim> ParseClaims(const std::string& s) {
    std::vector<Claim> out;
    std::istringstream in(s);
    std::string item;
    while (in >> item) {
        unsigned id = 0;
        int seat = 0;
        if (std::sscanf(item.c_str(), "%u:%d", &id, &seat) != 2 || id > 255 || seat < 0 || seat > kGettingIn) continue;
        if (seat == kGettingIn) continue;  // getting in or out: no seat yet
        out.push_back({static_cast<uint8_t>(id), seat});
    }
    return out;
}

}  // namespace mc::game::seats
