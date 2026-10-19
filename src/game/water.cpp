#include "game/water.h"

#include <cmath>

namespace mc::game::water {

const char* Name(State s) {
    switch (s) {
        case State::Swim: return "swim";
        case State::Under: return "under";
        default: return "dry";
    }
}

void Tracker::Reset() {
    state_ = State::Dry;
    surface_ = 0;
    lastMsg_ = -1e9;
    highT_ = -1;
    repeats_ = 0;
    messages_ = 0;
    haveZ_ = false;
}

Change Tracker::Set(State to, const char* why) {
    Change c;
    if (to == state_) return c;
    c.changed = true;
    c.from = state_;
    c.to = to;
    c.why = why;
    state_ = to;
    highT_ = -1;
    if (to == State::Dry) {
        repeats_ = 0;
        messages_ = 0;
        lastMsg_ = -1e9;
    }
    return c;
}

Change Tracker::Update(const Sample& s) {
    const bool jumped = haveZ_ && std::fabs(s.z - lastZ_) > t_.teleport;
    haveZ_ = true;
    lastZ_ = s.z;
    if (jumped && state_ != State::Dry) return Set(State::Dry, "teleport");
    if (!s.onFoot) return Set(State::Dry, "in a vehicle");

    // a getter that answers rules (the message only counted)
    if (s.getter >= 0) {
        if (s.deepWater) ++messages_;
        const State g = s.getter >= 2 ? State::Under : s.getter == 1 ? State::Swim : State::Dry;
        if (g != State::Dry && state_ == State::Dry) surface_ = s.z;
        return Set(g, "getter");
    }

    Change c;
    if (s.deepWater) {
        ++messages_;
        if (state_ == State::Dry) {
            surface_ = s.z;
            repeats_ = 0;
            c = Set(State::Swim, "IN_DEEP_WATER");
        } else {
            if (s.t - lastMsg_ <= t_.repeatGap) ++repeats_;
            // a message near the top while he swims (a repeating one): the surface is at least that high
            if (s.z > surface_ && s.z < surface_ + t_.exitRise) surface_ = s.z;
        }
        lastMsg_ = s.t;
    }
    if (state_ == State::Dry) return c;

    // the messages repeated and stopped: out
    if (repeats_ >= 2 && s.t - lastMsg_ > 2 * t_.repeatGap) return Set(State::Dry, "messages stopped");
    // climbed out: well above the surface for a moment
    if (s.z > surface_ + t_.exitRise) {
        if (highT_ < 0) highT_ = s.t;
        if (s.t - highT_ >= t_.exitTime) return Set(State::Dry, "climbed out");
    } else {
        highT_ = -1;
    }
    if (state_ == State::Swim && s.z < surface_ - t_.underDepth) return Set(State::Under, "dived");
    if (state_ == State::Under && s.z > surface_ - t_.underBack) return Set(State::Swim, "surfaced");
    return c;
}

}  // namespace mc::game::water
