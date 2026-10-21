#include "game/puppet_guard.h"

namespace mc::game::puppet_guard {

void FastFoot::Reset() {
    hide_ = false;
    since_ = -1;
    peak_ = 0;
}

FastFoot::Out FastFoot::Update(double t, bool onFoot, float speed) {
    Out o;
    if (!onFoot) {  // in a vehicle or swimming: not this guard's business; shown again at once
        o.changed = hide_;
        if (hide_) o.peak = peak_;
        Reset();
        return o;
    }
    if (!hide_) {
        if (speed > t_.on) {
            if (since_ < 0) since_ = t;
            if (t - since_ >= t_.onTime) {
                hide_ = true;
                since_ = -1;
                peak_ = speed;
                ++times_;
                o.changed = true;
            }
        } else {
            since_ = -1;
        }
    } else {
        if (speed > peak_) peak_ = speed;
        if (speed < t_.off) {
            if (since_ < 0) since_ = t;
            if (t - since_ >= t_.offTime) {
                hide_ = false;
                since_ = -1;
                o.changed = true;
            }
        } else {
            since_ = -1;
        }
    }
    o.hide = hide_;
    o.peak = peak_;
    return o;
}

std::string HazardText(uint32_t bits) {
    static const char* const kNames[] = {"seat", "water", "in_water", "fall", "fire", "blast", "hidden", "down"};
    std::string s;
    for (int i = 0; i < 8; ++i) {
        if (!(bits & (1u << i))) continue;
        if (!s.empty()) s += "+";
        s += kNames[i];
    }
    return s.empty() ? "none" : s;
}

void Shield::Reset() {
    on_ = false;
    lastT_ = -1e9;
    last_ = 0;
}

ShieldOut Shield::Update(double t, uint32_t hazards) {
    ShieldOut o;
    const bool was = on_;
    if (hazards & kDown) {
        on_ = false;  // his player is down: the puppet must be able to die now
    } else if (hazards) {
        on_ = true;
        lastT_ = t;
        last_ = hazards;
        o.hazards = hazards;
    } else if (on_ && t - lastT_ >= hold_) {
        on_ = false;
    }
    o.on = on_;
    o.changed = on_ != was;
    return o;
}

void SeatLoop::Reset() {
    count_ = recent_ = 0;
}

bool SeatLoop::Off(double t) {
    // the newest few get-off times, a ring of 8 (n_ is at most 8)
    times_[count_ % 8] = t;
    ++count_;
    recent_ = 0;
    for (int i = 0; i < count_ && i < 8; ++i)
        if (t - times_[(count_ - 1 - i) % 8] < window_) ++recent_;
    return recent_ >= n_;
}

}  // namespace mc::game::puppet_guard
