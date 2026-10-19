// world_env_test: unit tests for the time of day and weather sync (src/game/world_env.h): the WorldEnv body with its
// sample time (and that an older reader / an older host still works), the host's send plan (at once to a newcomer
// keyed by (id, joinSeq), on a change, every 2 s), and the guest's clock following the host's without jumps: a small
// error closes by running the clock up to 25 % faster or slower (no overshoot, the flow speed put back exactly), the
// wrap at 24 h, one SetTime for a big error or a standing clock, the age of the sample compensated, the game's rate
// learnt from the host's samples. The 2026-10-19 two-PC test had the time corrected only beyond 3 game minutes, with a
// visible jump each time. Prints PASS/FAIL per check; exits non-zero on any failure.
#include "game/world_env.h"

#include <cmath>
#include <cstdio>
#include <deque>
#include <string>
#include <vector>

using namespace mc;
using namespace mc::game::worldenv;

namespace {

int g_failures = 0;

void Check(bool ok, const char* name, const std::string& detail = {}) {
    std::printf("%s  %-70s %s\n", ok ? "PASS" : "FAIL", name, detail.c_str());
    std::fflush(stdout);
    if (!ok) ++g_failures;
}

std::string F(const char* fmt, double a, double b = 0, double c = 0) {
    char buf[160];
    std::snprintf(buf, sizeof buf, fmt, a, b, c);
    return buf;
}

void TestWire() {
    Env e;
    e.t = 14.25f;
    e.flow = 1.5f;
    e.weather = "_default_game_rainy";
    e.sampleMs = 123456789u;
    net::ByteWriter w;
    Write(w, e);
    net::ByteReader r(w.Span());
    Env o;
    Check(Read(r, o) && o.stamped && o.sampleMs == 123456789u && o.weather == e.weather && o.t == e.t &&
              o.flow == e.flow,
          "wire: round trip with the sample time");

    // an older reader: f32 f32 str, then it stops (it never checked for the end): the trailing u32 is ignored
    net::ByteReader old(w.Span());
    const float t = old.F32(), flow = old.F32();
    const std::string weather = old.Str(96);
    Check(old.Ok() && t == e.t && flow == e.flow && weather == e.weather && old.Remaining() == 4,
          "wire: an older reader reads the same three fields (4 bytes left over)");

    // an older host: no sample time
    net::ByteWriter w2;
    w2.F32(9.0f);
    w2.F32(0.0f);
    w2.Str("day_cycle_01");
    net::ByteReader r2(w2.Span());
    Env o2;
    Check(Read(r2, o2) && !o2.stamped && o2.t == 9.0f, "wire: an older host's body reads, unstamped");

    net::ByteWriter w3;
    w3.F32(25.0f);
    w3.F32(1.0f);
    w3.Str("x");
    net::ByteReader r3(w3.Span());
    Env o3;
    Check(!Read(r3, o3), "wire: a time beyond 24.5 h is refused");
    net::ByteWriter w4;
    w4.F32(10.0f);
    w4.F32(1.0f);
    w4.Str("two words");
    net::ByteReader r4(w4.Span());
    Check(!Read(r4, o3), "wire: a weather name with a space is refused");
}

void TestClockMath() {
    Check(std::fabs(HourDiff(0.1, 23.9) - 0.2) < 1e-9, "clock: 00:06 is 12 minutes after 23:54");
    Check(std::fabs(HourDiff(23.9, 0.1) + 0.2) < 1e-9, "clock: 23:54 is 12 minutes before 00:06");
    Check(std::fabs(Wrap24(24.5) - 0.5) < 1e-9 && std::fabs(Wrap24(-0.5) - 23.5) < 1e-9, "clock: wrap");
}

// One guest following a host. Both games' clocks run at `rate` hours per second per flow unit; the host samples every
// tick, sends every 2 s (and the first at once), the message arrives `lagMs` later. The guest's commands take effect
// one tick later (the glue carries them out at the start of its next call), as in the game.
struct Sim {
    double rate = kDefaultRate;
    double hostT = 12.0, guestT = 12.0;
    float hostFlow = 1.0f, guestFlow = 1.0f;
    std::string hostWeather = "day_cycle_01", guestWeather = "day_cycle_01";
    uint32_t lagMs = 30;
    double now = 100.0;  // s, both the session clock and our steady clock
    Follower f;
    std::deque<std::pair<uint32_t, Env>> wire;  // (arrives at ms, body)
    double lastSend = -1e9;
    Decision pending;  // to carry out at the next tick
    bool havePending = false;
    // results
    int sets = 0, flowSets = 0, weatherSets = 0;
    double minErr = 1e9, maxErr = -1e9, maxShare = 0;
    std::vector<double> errs;

    uint32_t Ms() const { return static_cast<uint32_t>(now * 1000.0); }

    void Step(double dt) {
        // a frame of both games, then the guest's glue call: it carries out last tick's commands and reads the game
        now += dt;
        hostT = Wrap24(hostT + rate * hostFlow * dt);
        guestT = Wrap24(guestT + rate * guestFlow * dt);
        if (havePending) {
            if (pending.setTime) guestT = pending.setTo, ++sets;
            if (pending.setFlow) guestFlow = pending.flow, ++flowSets;
            if (pending.setWeather) guestWeather = pending.weather, ++weatherSets;
            havePending = false;
        }
        if (now - lastSend >= kSendPeriodS) {
            lastSend = now;
            Env e;
            e.t = static_cast<float>(hostT);
            e.flow = hostFlow;
            e.weather = hostWeather;
            e.sampleMs = Ms();
            e.stamped = true;
            wire.push_back({Ms() + lagMs, e});
        }
        while (!wire.empty() && wire.front().first <= Ms()) {
            f.OnHost(wire.front().second, wire.front().first);
            wire.pop_front();
        }
        const Decision d = f.Update(Ms(), now, static_cast<float>(guestT), guestFlow, guestWeather);
        if (d.valid) {
            const double trueErr = HourDiff(hostT, guestT) * 60.0;
            errs.push_back(trueErr);
            minErr = std::min(minErr, trueErr);
            maxErr = std::max(maxErr, trueErr);
        }
        if (d.setFlow && hostFlow > 0) maxShare = std::max(maxShare, std::fabs(d.flow / hostFlow - 1.0));
        pending = d;
        havePending = true;
    }
    void Run(double seconds, double dt = 1.0 / 60.0) {
        const int n = static_cast<int>(seconds / dt + 0.5);
        for (int i = 0; i < n; ++i) Step(dt);
    }
    double TrueErr() const { return HourDiff(hostT, guestT) * 60.0; }
    // in the session for 5 s with the host's time (the first sync done, the game's rate measured), then the guest's
    // clock drifts by `min` game minutes (positive: behind) and the counters start again
    void Drift(double min) {
        Run(5);
        guestT = Wrap24(guestT - min / 60.0);
        sets = flowSets = weatherSets = 0;
        minErr = 1e9;
        maxErr = -1e9;
        maxShare = 0;
        errs.clear();
    }
};

void TestCatchUpBehind() {
    Sim s;
    s.Drift(5.0);  // 5 game minutes behind during play, flow 1 (1 game minute per second)
    s.Run(40);
    Check(s.sets == 0, "behind 5 min: no SetTime (no jump)", std::to_string(s.sets));
    Check(std::fabs(s.TrueErr()) <= kStopMin + 0.02, "behind 5 min: within 0.1 game minutes after 40 s",
          F("%.3f min", s.TrueErr()));
    Check(s.minErr > -0.05, "behind 5 min: no overshoot (never ahead)", F("min %.3f min", s.minErr));
    Check(s.maxShare <= kMaxShare + 1e-6 && s.maxShare >= kMaxShare - 1e-6,
          "behind 5 min: the clock ran at most 25 % faster (and did use it)", F("%.3f", s.maxShare));
    Check(s.guestFlow == s.hostFlow, "behind 5 min: the host's flow speed is back exactly", F("%.5f", s.guestFlow));
    // how fast: at 25 % of 1 game minute per second, 4.5 minutes take ~18 s, then the proportional tail
    double at = -1;
    for (size_t i = 0; i < s.errs.size(); ++i)
        if (std::fabs(s.errs[i]) <= kDeadMin) {
            at = static_cast<double>(i) / 60.0;
            break;
        }
    Check(at > 0 && at < 25, "behind 5 min: within 0.5 game minutes after ~20 s", F("%.1f s", at));
}

void TestCatchUpAhead() {
    Sim s;
    s.hostFlow = s.guestFlow = 20.0f;  // a fast day: 20 game minutes per second
    s.Drift(-8.0);                     // 8 game minutes ahead
    s.Run(10);
    Check(s.sets == 0, "ahead 8 min at flow 20: no SetTime", std::to_string(s.sets));
    Check(std::fabs(s.TrueErr()) <= kStopMin + 0.05, "ahead 8 min at flow 20: in step after 10 s",
          F("%.3f min", s.TrueErr()));
    Check(s.maxErr < 0.05, "ahead 8 min at flow 20: no overshoot (never behind)", F("max %.3f min", s.maxErr));
    Check(s.guestFlow == 20.0f, "ahead 8 min at flow 20: flow speed 20 again", F("%.5f", s.guestFlow));
}

void TestDeadZone() {
    Sim s;
    s.Drift(0.3);  // 0.3 game minutes: left alone
    s.Run(10);
    Check(s.sets == 0 && s.flowSets == 0, "0.3 min off: nothing is done",
          F("sets %.0f, flow sets %.0f", s.sets, s.flowSets));
}

void TestWrap() {
    Sim s;
    s.hostT = s.guestT = 23.9;
    s.Drift(-2.4);  // at 23:58 the guest's clock runs 2.4 game minutes ahead: past midnight, not 24 h behind
    s.Run(1.0 / 60.0 * 3);
    const double err0 = s.errs.empty() ? 99 : s.errs.front();
    Check(err0 < -2.0 && err0 > -3.0, "wrap: 23:59 vs 00:02 is 2.4 minutes ahead", F("%.2f min", err0));
    s.Run(30);
    Check(s.sets == 0, "wrap: no SetTime across midnight", std::to_string(s.sets));
    Check(std::fabs(s.TrueErr()) <= kStopMin + 0.02 && s.hostT < 12, "wrap: in step after midnight",
          F("%.3f min at %.3f h", s.TrueErr(), s.hostT));
}

void TestBigJump() {
    Sim s;
    s.Drift(-60.0);  // an hour off during play
    s.Run(20);
    Check(s.sets == 1, "60 min off: exactly one SetTime", std::to_string(s.sets));
    Check(std::fabs(s.TrueErr()) <= kDeadMin, "60 min off: in step afterwards", F("%.3f min", s.TrueErr()));
    Check(s.guestFlow == s.hostFlow, "60 min off: flow speed untouched / back", F("%.5f", s.guestFlow));
}

void TestFirstSync() {
    Sim s;
    s.guestT = 12.0 - 2.0 / 60.0;  // we come in 2 game minutes behind
    s.Run(10);
    Check(s.sets == 1 && std::fabs(s.TrueErr()) <= kDeadMin && !s.f.Adjusting(),
          "first sync: 2 min off at the join: one SetTime, no catch-up", F("sets %.0f, err %.3f", s.sets, s.TrueErr()));
    // a new world of ours (a load): the next sync is a first one again
    s.guestT = Wrap24(s.guestT + 3.0 / 60.0);
    s.f.NewWorld();
    s.sets = 0;
    s.Run(3);
    Check(s.sets == 1, "first sync: after a new world of ours, set again", std::to_string(s.sets));
    Sim q;
    q.guestT = 12.0 - 0.2 / 60.0;
    q.Run(5);
    Check(q.sets == 0, "first sync: 0.2 min off: nothing", std::to_string(q.sets));
}

void TestFrozen() {
    Sim s;
    s.hostFlow = s.guestFlow = 0.0f;  // missions and the Free Ride save of the test: the clock stands still
    s.hostT = 14.0;
    s.guestT = 14.0 + 2.0 / 60.0;
    s.Run(10);
    Check(s.sets == 1 && std::fabs(s.TrueErr()) < 0.01, "flow 0, 2 min off: one SetTime, then exact",
          F("sets %.0f, err %.3f", s.sets, s.TrueErr()));
    Sim q;
    q.hostFlow = q.guestFlow = 0.0f;
    q.guestT = 12.0 + 0.3 / 60.0;
    q.Run(10);
    Check(q.sets == 0 && q.flowSets == 0, "flow 0, 0.3 min off: nothing", F("sets %.0f", q.sets));
}

void TestFlowChange() {
    Sim s;
    s.guestFlow = 0.0f;  // our mission script stopped our clock; the host's runs
    s.Run(3);
    Check(s.guestFlow == 1.0f, "flow: the host's flow speed is taken", F("%.3f", s.guestFlow));
    s.hostFlow = 5.0f;  // the host's changes
    s.lastSend = -1e9;  // sent at once on a change
    s.Run(3);
    Check(s.guestFlow == 5.0f && s.sets == 0, "flow: a change of the host's follows", F("%.3f", s.guestFlow));
}

void TestAge() {
    Follower f;
    Env e;
    e.t = 10.0f;
    e.flow = 1.0f;
    e.weather = "w";
    e.sampleMs = 50000;
    e.stamped = true;
    f.OnHost(e, 50000);
    // 6 s later at 1 game minute per second: 6 minutes on
    Check(std::fabs(f.Predict(56000) - (10.0 + 6.0 / 60.0)) < 1e-6, "age: a 6 s old sample is 6 game minutes on",
          F("%.5f h", f.Predict(56000)));
    // a guest whose clock is exactly the host's now does nothing
    const Decision d = f.Update(56000, 1.0, static_cast<float>(10.0 + 6.0 / 60.0), 1.0f, "w");
    Check(d.valid && d.act == Act::None && !d.setTime && !d.setFlow && std::fabs(d.errMin) < 0.01,
          "age: compensated, the guest is in step", F("%.3f min", d.errMin));
    // without the compensation the same reading would have been 6 minutes off (a catch-up for nothing)
    Follower g;
    e.flow = 0.0f;  // a standing clock: no compensation
    g.OnHost(e, 50000);
    Check(std::fabs(g.Predict(56000) - 10.0) < 1e-6, "age: a standing clock is not moved on");
    // a sample from the future (clock trouble) counts as age 0
    Check(std::fabs(f.Predict(49000) - 10.0) < 1e-6, "age: a sample from the future counts as now");

    Sim s;
    s.lagMs = 400;  // a slow link: 400 ms is 0.4 game minutes at flow 1
    s.hostFlow = s.guestFlow = 2.0f;
    s.Run(20);
    Check(s.flowSets == 0 && s.sets == 0, "age: a 400 ms link at flow 2 causes no correction",
          F("flow sets %.0f, err %.3f", s.flowSets, s.TrueErr()));
}

void TestRateLearning() {
    Sim s;
    s.rate = kDefaultRate * 3;  // the game's real rate is three times the guess
    s.Drift(4.0);
    s.Run(60);
    Check(s.f.RateMeasured() && std::fabs(s.f.Rate() / s.rate - 1.0) < 0.05, "rate: learnt from the host's samples",
          F("%.6f (true %.6f)", s.f.Rate(), s.rate));
    Check(std::fabs(s.TrueErr()) <= kDeadMin && s.sets == 0 && s.minErr > -0.1,
          "rate: in step without a jump or an overshoot", F("%.3f min", s.TrueErr()));
    Check(s.guestFlow == s.hostFlow, "rate: flow speed back", F("%.4f", s.guestFlow));
    // a jump of the host's own clock (a script, a SetTime there): one SetTime here, the rate is not thrown off
    s.hostT = Wrap24(s.hostT + 2.0);
    s.sets = 0;
    s.Run(10);
    Check(s.sets == 1 && std::fabs(s.f.Rate() / s.rate - 1.0) < 0.05, "rate: a jump of the host's clock is no rate",
          F("sets %.0f, rate %.6f", s.sets, s.f.Rate()));
}

void TestSlowCatchUpSets() {
    Sim s;
    s.hostFlow = s.guestFlow = 0.1f;  // 0.1 game minutes per second: 10 minutes would take 400 s at 25 %
    s.Drift(10.0);
    s.Run(1);
    Check(s.sets == 1, "slow clock, 10 min off: SetTime (a catch-up would take minutes)", std::to_string(s.sets));
}

void TestRestore() {
    Sim s;
    s.Drift(5.0);
    s.Run(2);
    Check(s.f.Adjusting() && s.guestFlow > s.hostFlow, "restore: a catch-up runs", F("%.3f", s.guestFlow));
    const float back = s.f.Restore();
    Check(back == 1.0f, "restore: the session ends: the host's flow speed goes back", F("%.3f", back));
    Check(s.f.Restore() < 0, "restore: only once");
    Sim q;
    q.Run(3);
    Check(q.f.Restore() < 0, "restore: nothing to put back when no catch-up ran");
    // after a reset the game's rate is kept, the host's sample is not
    s.f.Reset();
    Check(!s.f.HasHost(), "restore: a reset forgets the host's sample");
}

void TestWeather() {
    Sim s;
    s.guestWeather = "_default_game";
    s.hostWeather = "_default_game_rainy";
    s.Run(1);
    Check(s.weatherSets == 1 && s.guestWeather == "_default_game_rainy", "weather: set at once when it differs");
    s.Run(5);
    Check(s.weatherSets == 1, "weather: once", std::to_string(s.weatherSets));
    // a game that does not take it: asked again every 2 s, not every tick
    Follower f;
    Env e;
    e.t = 12;
    e.flow = 1;
    e.weather = "a";
    e.sampleMs = 0;
    e.stamped = true;
    f.OnHost(e, 0);
    int asks = 0;
    for (int i = 0; i < 300; ++i) asks += f.Update(static_cast<uint32_t>(i * 16), i / 60.0, 12.0f, 1.0f, "b").setWeather;
    Check(asks == 3, "weather: a refused set is asked again every 2 s", std::to_string(asks));
}

void TestHostSender() {
    using G = std::vector<std::pair<net::PlayerId, uint32_t>>;  // (id, joinSeq)
    auto P = [](int id, int seq) { return std::make_pair(static_cast<net::PlayerId>(id), static_cast<uint32_t>(seq)); };
    HostSender h;
    auto p = h.Tick(0.0, G{P(1, 1)});
    Check(!p.broadcast && p.to.empty(), "host: nothing before the first reading");
    h.Sample(10.0f, 1.0f, "w", 1000);
    p = h.Tick(0.1, G{P(1, 1)});
    Check(p.broadcast, "host: the first reading goes out at once (to everyone)");
    p = h.Tick(0.2, G{P(1, 1)});
    Check(!p.broadcast && p.to.empty(), "host: not again at once");
    p = h.Tick(1.0, G{P(1, 1), P(2, 2)});
    Check(!p.broadcast && p.to.size() == 1 && p.to[0] == 2, "host: a newcomer gets it at once, only he");
    p = h.Tick(1.1, G{P(1, 1), P(2, 2)});
    Check(p.to.empty() && !p.broadcast, "host: once");
    p = h.Tick(1.2, G{P(1, 3), P(2, 2)});
    Check(p.to.size() == 1 && p.to[0] == 1, "host: back after a lost connection (new joinSeq): again at once");
    h.Sample(10.01f, 1.0f, "w", 1300);
    p = h.Tick(1.3, G{P(1, 3), P(2, 2)});
    Check(!p.broadcast, "host: the clock moving on is no change");
    h.Sample(10.02f, 1.0f, "rain", 1400);
    p = h.Tick(1.4, G{P(1, 3), P(2, 2)});
    Check(p.broadcast && std::string(p.why) == "change", "host: a weather change goes out at once");
    h.Sample(10.03f, 2.0f, "rain", 1500);
    p = h.Tick(1.5, G{P(1, 3), P(2, 2)});
    Check(p.broadcast && std::string(p.why) == "change", "host: a flow change goes out at once");
    p = h.Tick(3.4, G{P(1, 3), P(2, 2)});
    Check(!p.broadcast, "host: not before 2 s");
    p = h.Tick(3.6, G{P(1, 3), P(2, 2)});
    Check(p.broadcast && std::string(p.why) == "period", "host: every 2 s");
    Check(h.Current().sampleMs == 1500 && h.Current().stamped, "host: the body carries the sample time");
    // a newcomer before the first reading waits for it
    HostSender k;
    k.Tick(0.0, G{P(3, 1)});
    k.Tick(0.5, G{P(3, 1)});
    k.Sample(1.0f, 0.0f, "w", 10);
    p = k.Tick(0.6, G{P(3, 1)});
    Check(p.broadcast || (p.to.size() == 1 && p.to[0] == 3), "host: a newcomer before the first reading gets it then");
}

}  // namespace

int main() {
    TestWire();
    TestClockMath();
    TestCatchUpBehind();
    TestCatchUpAhead();
    TestDeadZone();
    TestFirstSync();
    TestWrap();
    TestBigJump();
    TestFrozen();
    TestFlowChange();
    TestAge();
    TestRateLearning();
    TestSlowCatchUpSets();
    TestRestore();
    TestWeather();
    TestHostSender();
    std::printf("%s (%d failure%s)\n", g_failures ? "FAILED" : "OK", g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
