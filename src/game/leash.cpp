#include "game/leash.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace mc::game::leash {

Config FromIni(int warnM, int maxM, int countdownS) {
    Config c;
    c.warnM = static_cast<uint16_t>(std::clamp(warnM, 0, 65535));
    c.maxM = static_cast<uint16_t>(std::clamp(maxM, 0, 65535));
    c.countdownS = static_cast<uint8_t>(std::clamp(countdownS, 0, 255));
    if (c.maxM != 0 && c.maxM < c.warnM) c.maxM = c.warnM;
    c.flags = c.warnM > 0 ? 1 : 0;
    return c;
}

void Write(net::ByteWriter& w, const Config& c) {
    w.U16(c.warnM);
    w.U16(c.maxM);
    w.U8(c.countdownS);
    w.U8(c.flags);
}

bool Read(net::ByteReader& r, Config& c) {
    Config n;
    n.warnM = r.U16();
    n.maxM = r.U16();
    n.countdownS = r.U8();
    n.flags = r.U8();
    if (r.Failed()) return false;
    if (n.maxM != 0 && n.maxM < n.warnM) n.maxM = n.warnM;
    c = n;
    return true;
}

const char* EventName(EventKind k) {
    switch (k) {
    case EventKind::None: return "none";
    case EventKind::Warn: return "warn";
    case EventKind::Countdown: return "countdown";
    case EventKind::Pulled: return "pulled";
    case EventKind::Back: return "back";
    }
    return "?";
}

void Write(net::ByteWriter& w, const Event& e) {
    w.U8(static_cast<uint8_t>(e.kind));
    w.U16(e.distM);
}

bool Read(net::ByteReader& r, Event& e) {
    const uint8_t k = r.U8();
    const uint16_t d = r.U16();
    if (r.Failed() || k < 1 || k > 4) return false;
    e.kind = static_cast<EventKind>(k);
    e.distM = d;
    return true;
}

const char* StateName(State s) {
    switch (s) {
    case State::Near: return "near";
    case State::Warn: return "warn";
    case State::Countdown: return "countdown";
    case State::Paused: return "paused";
    case State::Pulling: return "pulling";
    case State::Pulled: return "pulled";
    case State::Suspended: return "suspended";
    }
    return "?";
}

const char* WhyName(Why w) {
    switch (w) {
    case Why::None: return "-";
    case Why::Off: return "off";
    case Why::Loading: return "loading";
    case Why::AfterLoad: return "afterload";
    case Why::FailReload: return "failreload";
    case Why::Joining: return "joining";
    case Why::NoHost: return "nohost";
    case Why::HostPaused: return "hostpaused";
    case Why::Cutscene: return "cutscene";
    case Why::SelfDown: return "selfdown";
    case Why::HostDown: return "hostdown";
    case Why::HostTeleport: return "hostteleport";
    case Why::HostCar: return "hostcar";
    case Why::Passenger: return "passenger";
    }
    return "?";
}

void Settle::Start(double now, float x, float y, float z) {
    at_ = now;
    x_ = x;
    y_ = y;
    z_ = z;
    n_ = 0;
}

bool Settle::Update(double now, float z) {
    if (!Active(now) || z >= z_ - kSettleDropM) return false;
    ++n_;
    ++holds_;
    return true;
}

std::string Banner(State s, float dist, double left) {
    char buf[96];
    switch (s) {
    case State::Warn: {
        if (dist < 0) return {};
        const long m = std::lround(dist / 10.0f) * 10;  // in 10 m steps: no flicker of the last digit
        std::snprintf(buf, sizeof buf, "FAR FROM THE HOST - THE STREETS THIN OUT (%ld M)", m);
        return buf;
    }
    case State::Countdown:
    case State::Pulling:
        std::snprintf(buf, sizeof buf, "TOO FAR FROM THE HOST - BACK IN %d S",
                      s == State::Pulling ? 0 : static_cast<int>(std::ceil(std::max(0.0, left))));
        return buf;
    case State::Paused: return "THE HOST PAUSED";
    case State::Pulled: return "PULLED BACK TO THE HOST";
    default: return {};
    }
}

Why Leash::Suspension(const Input& in, float dist) {
    const double now = in.now;
    if (!config_.On()) return Why::Off;
    if (!in.inWorld) return Why::Loading;
    if (now - notInWorldAt_ < kAfterLoadS) return Why::AfterLoad;
    if (in.failReload) return Why::FailReload;
    if (settledAt_ < 0 || now - settledAt_ < kAfterJoinS) return Why::Joining;
    if (!in.hostSeen || dist < 0) return Why::NoHost;
    if (in.hostPaused) return Why::HostPaused;
    if (in.cutscene || now - cutsceneAt_ < kAfterCutsceneS) return Why::Cutscene;
    if (in.selfDown) return Why::SelfDown;
    if (in.hostDown) return Why::HostDown;
    if (now - teleportAt_ < kAfterTeleportS) return Why::HostTeleport;
    if (in.hostCar) return Why::HostCar;
    if (in.passenger) return Why::Passenger;
    return Why::None;
}

void Leash::Emit(Output& out, EventKind k, float dist) {
    Event e;
    e.kind = k;
    e.distM = static_cast<uint16_t>(std::clamp(std::lround(std::max(0.0f, dist)), 0L, 65535L));
    out.events.push_back(e);
    repeatAt_ = lastNow_ + kRepeatS;
}

void Leash::Reset() {
    const Config c = config_;
    *this = Leash();
    config_ = c;
}

void Leash::PullDone(bool ok, double now) {
    if (state_ != State::Pulling) return;
    if (ok) {
        Go(State::Pulled, now);
        pullFailed_ = false;
        pendingPulled_ = true;
        return;
    }
    Go(State::Countdown, now);  // at 0 s: tried again after kPullRetryS
    left_ = 0;
    retryAt_ = now + kPullRetryS;
    pullFailed_ = true;
}

Output Leash::Update(const Input& in) {
    Output out;
    const double now = in.now;
    const double dt = lastNow_ < 0 ? 0.0 : std::max(0.0, now - lastNow_);
    lastNow_ = now;
    // what the suspensions need to remember
    if (!in.inWorld) notInWorldAt_ = now;
    if (in.cutscene) cutsceneAt_ = now;
    if (in.hostSeen) {
        if (havePrevHost_ && std::hypot(in.hostX - prevHostX_, in.hostY - prevHostY_) > kTeleportM) teleportAt_ = now;
        havePrevHost_ = true;
        prevHostX_ = in.hostX;
        prevHostY_ = in.hostY;
    } else {
        havePrevHost_ = false;
    }
    if (!in.joinSettled) settledAt_ = -1;
    else if (settledAt_ < 0) settledAt_ = now;

    const float dist = in.hostSeen ? std::hypot(in.x - in.hostX, in.y - in.hostY) : -1.0f;
    out.dist = dist;
    const State before = state_;
    const Why whyBefore = why_;
    if (pendingPulled_) {
        pendingPulled_ = false;
        Emit(out, EventKind::Pulled, pullDist_);
    }
    if (state_ == State::Pulling && now - pullAt_ > kPullWaitS) PullDone(false, now);  // no answer from the glue

    if (state_ != State::Pulling) {
        const Why w = Suspension(in, dist);
        if (w != Why::None) {
            if (w == Why::HostPaused && (state_ == State::Warn || state_ == State::Countdown)) {
                resume_ = state_;  // frozen where it stands
                Go(State::Paused, now);
            } else if (!(w == Why::HostPaused && state_ == State::Paused)) {
                if (state_ != State::Suspended) Go(State::Suspended, now);
                left_ = 0;
            }
            why_ = w;
        } else {
            why_ = Why::None;
            if (state_ == State::Paused) state_ = resume_;  // the host is back: the countdown goes on from its value
            if (state_ == State::Suspended) Go(State::Near, now);
            if (state_ == State::Pulled && now - stateAt_ >= kPulledBannerS) Go(State::Near, now);
            const float warn = config_.warnM, mx = config_.maxM;
            const bool pullOn = config_.PullOn();
            auto toCountdown = [&] {
                Go(State::Countdown, now);
                left_ = config_.countdownS;
                retryAt_ = -1e9;
                ++countdowns_;
                Emit(out, EventKind::Countdown, dist);
            };
            switch (state_) {
            case State::Near:
                if (dist >= warn) {
                    if (pullOn && dist > mx) {
                        toCountdown();
                    } else {
                        Go(State::Warn, now);
                        ++warnings_;
                        Emit(out, EventKind::Warn, dist);
                    }
                }
                break;
            case State::Warn:
                if (dist < warn - kHysteresisM) {
                    Go(State::Near, now);
                    Emit(out, EventKind::Back, dist);
                } else if (pullOn && dist > mx) {
                    toCountdown();
                }
                break;
            case State::Countdown:
                if (!pullOn || dist < mx - kHysteresisM) {
                    if (dist < warn - kHysteresisM) {
                        Go(State::Near, now);
                        Emit(out, EventKind::Back, dist);
                    } else {
                        Go(State::Warn, now);
                        Emit(out, EventKind::Warn, dist);
                    }
                    break;
                }
                left_ = std::max(0.0, left_ - dt);
                // at 0 s the pull, but only while we really are beyond max (between max - 20 m and max it waits)
                if (left_ <= 0 && dist >= mx && now >= retryAt_) {
                    Go(State::Pulling, now);
                    pullAt_ = now;
                    pullDist_ = dist;
                    out.pull = true;
                    ++pulls_;
                }
                break;
            default: break;
            }
            // while far: the current event again now and then (the host's panel shows the distance)
            if (out.events.empty() && now >= repeatAt_) {
                if (state_ == State::Warn) Emit(out, EventKind::Warn, dist);
                else if (state_ == State::Countdown) Emit(out, EventKind::Countdown, dist);
            }
        }
    }
    out.state = state_;
    out.why = why_;
    out.left = state_ == State::Countdown || state_ == State::Paused ? left_ : 0.0;
    out.changed = state_ != before || why_ != whyBefore;
    out.banner = Banner(state_, dist, left_);
    return out;
}

ConfigSender::Plan ConfigSender::Tick(const Config& c, const std::vector<std::pair<uint32_t, uint32_t>>& guests) {
    Plan p;
    const bool changed = haveLast_ && c != last_;
    last_ = c;
    haveLast_ = true;
    if (changed && !guests.empty()) {
        p.broadcast = true;
        sent_ = guests;
        return p;
    }
    std::vector<std::pair<uint32_t, uint32_t>> keep;
    for (const auto& g : guests) {
        if (std::find(sent_.begin(), sent_.end(), g) == sent_.end()) p.to.push_back(g.first);
        keep.push_back(g);
    }
    sent_ = std::move(keep);
    return p;
}

}  // namespace mc::game::leash
