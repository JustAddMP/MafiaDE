#pragma once
// Time of day and weather on every game: the host's game is the clock (docs/protocol.md "WorldEnv").
//
//   - Host: the glue (world_env.lua) reads the game's time of day (hours 0..24), time flow speed and weather set
//     every script tick. WorldEnv 0x0200 goes out at once to every newcomer (also a guest back after a lost
//     connection: keyed by (id, joinSeq)), at once on a change of the weather or the flow speed, and every 2 s. It
//     carries the sample's time on the session clock, so a guest knows how old it is.
//   - Guest: predicts the host's time now (sample + its age x the game's rate x the flow). The first time after a
//     join, a reconnect or a new world of ours it sets the host's time at once (when 0.5 game minutes or more off);
//     from then on it converges without jumps: below 0.5 game minutes nothing; up to 15 game minutes the clock runs up to 25 % faster or slower for a
//     few seconds (SetTimeFlowSpeed) until it is within 0.1 game minutes, then the host's flow is set back; beyond
//     that (or when the host's time stands still, flow 0, or a catch-up would take over 30 s) SetTime once. The
//     game's rate (game hours per second per flow unit) is measured from the host's samples first (three WorldEnv,
//     ~4 s); until then only errors beyond 15 game minutes are corrected. The weather set is set at once when it differs.
//
// The rules are the pure classes below (HostSender, Follower; tests/world_env_test.cpp). WorldEnvSync is the glue:
// messages on the render thread, the game on GameMainThread (tick callback), a mutex in between.
// Trace: WEH (host, 1 Hz), WEG (guest, 1 Hz), docs/sync-trace.md.
#include "net/session.h"
#include "net/wire.h"
#include "render/overlay.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace mc::game {

namespace worldenv {

constexpr uint16_t kMsgWorldEnv = 0x0200;  // host -> guests, reliable

constexpr float kDeadMin = 0.5f;        // game minutes: closer than this, nothing is done
constexpr float kStopMin = 0.1f;        // a catch-up runs until the clock is this close
constexpr float kFlowMaxMin = 15.0f;    // game minutes: further off, SetTime once
constexpr float kMaxShare = 0.25f;      // the flow speed is changed by at most this share of the host's
constexpr float kShareStep = 0.01f;     // ... in steps of 1 % (a new flow speed only when the step changes)
constexpr float kTauS = 2.0f;           // catch-up time constant (s): the error shrinks as exp(-t / kTauS)
constexpr float kMaxCatchUpS = 30.0f;   // a catch-up that would take longer at kMaxShare: SetTime instead
constexpr float kSetCooldownS = 2.0f;   // after a SetTime: nothing else until the game shows it
constexpr float kFrozenFlow = 1e-3f;    // a flow speed this small: the time stands still (missions, Free Ride)
constexpr double kSendPeriodS = 2.0;    // host: WorldEnv every 2 s (and at once on a change / newcomer)
constexpr float kMaxAgeS = 600.0f;      // an older sample (clock trouble) counts as this old
// Game hours per real second per unit of flow speed. docs/game-api.md 8: "flow 20 = 1 game hour per ~3 s", i.e.
// 1/60. UNVERIFIED (one observation); the follower measures the real rate from the host's samples and uses that.
constexpr double kDefaultRate = 1.0 / 60.0;

inline double Wrap24(double h) {
    h = std::fmod(h, 24.0);
    if (h < 0) h += 24.0;
    return h;
}
// a - b in hours, the short way round the clock: (-12, 12]
inline double HourDiff(double a, double b) {
    double d = Wrap24(a - b);
    if (d > 12.0) d -= 24.0;
    return d;
}

// a weather set name as the game gives it: printable ASCII, no spaces, at most 96 bytes
inline bool WeatherToken(const std::string& s) {
    if (s.empty() || s.size() > 96) return false;
    for (unsigned char c : s)
        if (c <= 0x20 || c >= 0x7F) return false;
    return true;
}

// ---- WorldEnv body: f32 timeOfDay, f32 flowSpeed, str weatherSet, [u32 sampleMs] ----------------------------------
// sampleMs (since the world-join change): the host's sample time on the session clock. Older readers stop after the
// weather string (they never checked for the end), older hosts do not send it (stamped = false).
struct Env {
    float t = 0;      // hours 0..24
    float flow = 0;   // time flow speed
    std::string weather;
    uint32_t sampleMs = 0;
    bool stamped = false;
};

inline void Write(net::ByteWriter& w, const Env& e) {
    w.F32(e.t);
    w.F32(e.flow);
    w.Str(e.weather);
    w.U32(e.sampleMs);
}

inline bool Read(net::ByteReader& r, Env& e) {
    e.t = r.F32();
    e.flow = r.F32();
    e.weather = r.Str(96);
    e.stamped = false;
    if (r.Ok() && r.Remaining() >= 4) {
        e.sampleMs = r.U32();
        e.stamped = true;
    }
    return r.Ok() && std::isfinite(e.t) && e.t >= 0 && e.t <= 24.5f && std::isfinite(e.flow) &&
           std::fabs(e.flow) <= 1e4f && WeatherToken(e.weather);
}

// ---- host: when WorldEnv goes out ------------------------------------------------------------------------------------
class HostSender {
public:
    struct Plan {
        bool broadcast = false;            // to everyone (a change, or the 2 s period)
        std::vector<net::PlayerId> to;     // newcomers only (when not broadcasting anyway)
        const char* why = "";              // "change", "period", "newcomer"
    };

    // A reading of the host's game (every script tick while in the world).
    void Sample(float t, float flow, const std::string& weather, uint32_t sampleMs) {
        if (have_ && (weather != env_.weather || std::fabs(flow - env_.flow) > 1e-5f)) changed_ = true;
        if (!have_) changed_ = true;
        env_.t = t;
        env_.flow = flow;
        env_.weather = weather;
        env_.sampleMs = sampleMs;
        env_.stamped = true;
        have_ = true;
    }
    bool Have() const { return have_; }
    const Env& Current() const { return env_; }

    // Render thread, every tick. guests = the other players as (id, joinSeq).
    Plan Tick(double now, const std::vector<std::pair<net::PlayerId, uint32_t>>& guests) {
        Plan p;
        std::set<std::pair<net::PlayerId, uint32_t>> cur(guests.begin(), guests.end());
        for (const auto& g : cur)
            if (!seen_.count(g)) pending_.insert(g.first);
        seen_.swap(cur);
        // a guest that left before he got it is not waited for
        for (auto it = pending_.begin(); it != pending_.end();) {
            bool there = false;
            for (const auto& g : seen_) there = there || g.first == *it;
            it = there ? std::next(it) : pending_.erase(it);
        }
        if (!have_ || guests.empty()) return p;  // newcomers stay pending until we have a reading
        if (changed_ || now - lastSent_ >= kSendPeriodS) {
            p.broadcast = true;
            p.why = changed_ ? "change" : "period";
            changed_ = false;
            lastSent_ = now;
            pending_.clear();
            return p;
        }
        if (!pending_.empty()) {
            p.to.assign(pending_.begin(), pending_.end());
            p.why = "newcomer";
            pending_.clear();
        }
        return p;
    }

    void Reset() {
        *this = HostSender();
    }

private:
    Env env_;
    bool have_ = false, changed_ = false;
    double lastSent_ = -1e9;
    std::set<std::pair<net::PlayerId, uint32_t>> seen_;
    std::set<net::PlayerId> pending_;
};

// ---- guest: following the host's clock -------------------------------------------------------------------------------
enum class Act : uint8_t { None, Flow, Set };
inline const char* ActName(Act a) { return a == Act::Set ? "set" : a == Act::Flow ? "flow" : "none"; }

struct Decision {
    bool valid = false;        // a host sample is known
    Act act = Act::None;       // what we do about the time now
    double hostNow = 0;        // the host's time now as predicted (hours)
    double errMin = 0;         // host - ours, game minutes (positive: we are behind)
    bool setTime = false;      // SetTime(setTo)
    double setTo = 0;
    bool setFlow = false;      // SetTimeFlowSpeed(flow)
    float flow = 0;
    bool setWeather = false;   // SetWeatherSet(weather)
    std::string weather;
    bool started = false;      // a catch-up started this tick (for the log)
    bool finished = false;     // a catch-up ended this tick (in step again)
};

class Follower {
public:
    // A WorldEnv from the host. recvMs: our session clock at its arrival (taken as the sample time when the host did
    // not stamp it; the caller passes its send stamp when that is known).
    void OnHost(const Env& e, uint32_t recvMs) {
        host_ = e;
        if (!e.stamped) host_.sampleMs = recvMs;
        have_ = true;
        if (e.stamped) Learn(host_);  // an unstamped sample's time is only its arrival: too rough for the rate
    }
    bool HasHost() const { return have_; }
    const Env& Host() const { return host_; }
    double Rate() const { return rate_; }
    bool RateMeasured() const { return rateMeasured_; }
    bool Adjusting() const { return adjusting_; }

    // The host's time now (hours) as predicted from his newest sample.
    double Predict(uint32_t nowMs) const {
        double age = static_cast<double>(static_cast<int32_t>(nowMs - host_.sampleMs)) / 1000.0;
        age = std::clamp(age, 0.0, static_cast<double>(kMaxAgeS));
        const double flow = host_.flow > kFrozenFlow ? host_.flow : 0.0;
        return Wrap24(host_.t + age * rate_ * flow);
    }

    // Every script tick in the world: our game's reading now. nowMs: session clock, nowS: any steady clock (s).
    Decision Update(uint32_t nowMs, double nowS, float localT, float localFlow, const std::string& localWeather) {
        Decision d;
        if (!have_) return d;
        d.valid = true;
        d.hostNow = Predict(nowMs);
        d.errMin = HourDiff(d.hostNow, localT) * 60.0;
        const double absErr = std::fabs(d.errMin);

        if (!host_.weather.empty() && host_.weather != localWeather &&
            (host_.weather != weatherAsked_ || nowS - weatherAt_ >= 2.0)) {
            d.setWeather = true;
            d.weather = host_.weather;
            weatherAsked_ = host_.weather;
            weatherAt_ = nowS;
        }

        float want = host_.flow;
        bool set = false;
        if (nowS < setUntil_) {
            // a SetTime is taking effect: nothing else meanwhile
        } else if (!synced_) {
            // the first sync after a join, a reconnect or a new world of ours: set at once (we just came in; a
            // catch-up of several minutes would show the wrong time for half a minute)
            set = absErr > kDeadMin;
        } else if (host_.flow <= kFrozenFlow) {
            if (adjusting_) d.finished = true;
            adjusting_ = false;
            set = absErr > kDeadMin;
        } else {
            const double perS = rate_ * host_.flow * 60.0;  // game minutes per real second at the host's flow
            // Until the game's rate is measured (the first few seconds) the age compensation and the catch-up
            // speed rest on a guess: only a big error is set then, a small one waits.
            if (absErr > kFlowMaxMin || (rateMeasured_ && absErr > kMaxShare * perS * kMaxCatchUpS)) {
                set = true;
            } else {
                if (adjusting_ && absErr <= kStopMin) {
                    adjusting_ = false;
                    d.finished = true;
                } else if (!adjusting_ && absErr > kDeadMin && rateMeasured_) {
                    adjusting_ = true;
                    d.started = true;
                }
                if (adjusting_) {
                    // proportional: the error closes at err / kTauS per second (never past zero), capped
                    double share = d.errMin / (kTauS * perS);
                    share = std::clamp(share, -static_cast<double>(kMaxShare), static_cast<double>(kMaxShare));
                    double steps = std::round(std::fabs(share) / kShareStep);
                    if (steps < 1) steps = 1;
                    share = (share < 0 ? -1.0 : 1.0) * steps * kShareStep;
                    want = static_cast<float>(host_.flow * (1.0 + share));
                }
            }
        }
        if (set) {
            d.setTime = true;
            d.setTo = d.hostNow;
            setUntil_ = nowS + kSetCooldownS;
            if (adjusting_) d.finished = true;
            adjusting_ = false;
            want = host_.flow;
        }
        if (nowS >= setUntil_) synced_ = true;
        d.act = set ? Act::Set : adjusting_ ? Act::Flow : Act::None;

        const bool differs = std::fabs(localFlow - want) > 1e-4f * std::max(1.0f, std::fabs(want));
        if (!differs) {
            applied_ = want;
        } else if (want != applied_ || nowS - appliedAt_ >= 1.0) {
            d.setFlow = true;
            d.flow = want;
            applied_ = want;
            appliedAt_ = nowS;
        }
        return d;
    }

    // Our game loaded a new world (or we were out of it for a while): the next decision is a first sync again.
    void NewWorld() {
        synced_ = false;
        adjusting_ = false;
    }
    bool Synced() const { return synced_; }

    // The session ended (or we left the world): the flow speed to put back when ours is a catch-up's, else < 0.
    float Restore() {
        const bool ours = adjusting_ || (applied_ >= 0 && have_ && std::fabs(applied_ - host_.flow) > 1e-6f);
        adjusting_ = false;
        if (!ours || !have_) return -1.0f;
        applied_ = host_.flow;
        return host_.flow;
    }

    void Reset() {
        const double rate = rate_;
        const bool measured = rateMeasured_;
        *this = Follower();
        rate_ = rate;  // the game's rate stays what it is
        rateMeasured_ = measured;
    }

private:
    // The game's rate (game hours per real second per flow unit) from the host's own samples: the hours his clock
    // moved between two WorldEnv over the session time between their samples x his flow speed (unchanged in between,
    // at least 1 s apart). Not from our own clock: our catch-ups, SetTimes and the game's own jumps would be in it.
    // The first value needs two observations that agree within 10 %; after that one more than 2x off (a jump of
    // the host's: a script, a SetTime) is ignored and the rest smoothed.
    void Learn(const Env& e) {
        if (!haveRef_ || e.flow != ref_.flow || e.flow <= kFrozenFlow) {
            ref_ = e;
            haveRef_ = true;
            return;
        }
        const double dt = static_cast<double>(static_cast<int32_t>(e.sampleMs - ref_.sampleMs)) / 1000.0;
        if (dt < 1.0) return;  // keep the older one as the reference
        const double dh = HourDiff(e.t, ref_.t);
        ref_ = e;
        if (dt > 30.0 || dh <= 0) return;
        const double obs = dh / (dt * e.flow);
        if (!rateMeasured_) {
            if (obs < 1e-5 || obs > 1.0) return;
            if (firstObs_ > 0 && std::fabs(obs / firstObs_ - 1.0) < 0.1) {
                rate_ = (obs + firstObs_) / 2;
                rateMeasured_ = true;
            } else {
                firstObs_ = obs;
            }
            return;
        }
        if (obs < rate_ / 2 || obs > rate_ * 2) return;
        rate_ = rate_ * 0.7 + obs * 0.3;
    }

    Env host_;
    bool have_ = false;
    double rate_ = kDefaultRate;
    bool rateMeasured_ = false;
    Env ref_;  // the host's sample the next rate observation is measured from
    bool haveRef_ = false;
    double firstObs_ = 0;
    bool adjusting_ = false;
    bool synced_ = false;  // the first sync after Reset / NewWorld is done
    double setUntil_ = -1e9;
    float applied_ = -1.0f;
    double appliedAt_ = -1e9;
    std::string weatherAsked_;
    double weatherAt_ = -1e9;
};

}  // namespace worldenv

class WorldEnvSync {
public:
    static void Install();  // once, after the Lua bridge
    static void Attach(net::Session& session);
    static void Detach();
    static void Tick(float dt);
    static void GetStatus(std::vector<render::OverlayLine>& out);
};

}  // namespace mc::game
