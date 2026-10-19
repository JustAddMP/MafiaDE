#include "game/seats.h"

#include <algorithm>
#include <cmath>
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

const char* PlaceName(Place p) {
    switch (p) {
        case Place::Wait: return "wait";
        case Place::AtCar: return "at_car";
        case Place::Aside: return "aside";
        case Place::Hold: return "hold";
        case Place::Release: return "release";
    }
    return "?";
}

Place PlaceCopy(bool haveCopy, bool aside, float blockerDist, double blockedFor, bool spotFound) {
    if (haveCopy) {
        if (!aside) return Place::AtCar;
        return (blockerDist < 0 || blockerDist >= kReleaseDist) ? Place::Release : Place::Hold;
    }
    if (blockerDist < 0 || blockerDist > kBlockDist) return Place::AtCar;
    if (blockedFor < kBlockWait || !spotFound) return Place::Wait;
    return Place::Aside;
}

namespace {

void Frame(float hx, float hy, float& fx, float& fy) {
    const float n = std::sqrt(hx * hx + hy * hy);
    if (!(n > 1e-4f) || !std::isfinite(n)) {
        fx = 1, fy = 0;
    } else {
        fx = hx / n, fy = hy / n;
    }
}

}  // namespace

Pt AsideCandidate(float tx, float ty, float hx, float hy, int index) {
    float fx, fy;
    Frame(hx, hy, fx, fy);
    const float rx = fy, ry = -fx;  // to the right of the heading
    // beside first, then behind / ahead, then further out to the sides
    static const float kAcross[kAsideCandidates] = {6.0f, -6.0f, 0.0f, 0.0f, 9.0f, -9.0f};
    static const float kAlong[kAsideCandidates] = {0.0f, 0.0f, -7.5f, 7.5f, 0.0f, 0.0f};
    if (index < 0 || index >= kAsideCandidates) return {tx, ty};
    return {tx + rx * kAcross[index] + fx * kAlong[index], ty + ry * kAcross[index] + fy * kAlong[index]};
}

Spot AsideSpot(float tx, float ty, float hx, float hy, const std::vector<Pt>& cars, uint32_t rejected) {
    for (int i = 0; i < kAsideCandidates; ++i) {
        if (rejected & (1u << i)) continue;
        const Pt c = AsideCandidate(tx, ty, hx, hy, i);
        bool clear = true;
        for (const Pt& o : cars) {
            const float dx = o.x - c.x, dy = o.y - c.y;
            if (dx * dx + dy * dy < kAsideClear * kAsideClear) {
                clear = false;
                break;
            }
        }
        if (clear) {
            Spot s;
            s.ok = true, s.index = i, s.x = c.x, s.y = c.y;
            return s;
        }
    }
    return {};
}

std::vector<Pt> ParsePoints(const std::string& s) {
    std::vector<Pt> out;
    std::istringstream in(s);
    std::string item;
    while (in >> item) {
        float x = 0, y = 0;
        if (std::sscanf(item.c_str(), "%f,%f", &x, &y) != 2 || !std::isfinite(x) || !std::isfinite(y)) continue;
        Pt p;
        p.x = x, p.y = y;
        out.push_back(p);
    }
    return out;
}

}  // namespace mc::game::seats
