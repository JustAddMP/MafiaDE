// leash_test: unit tests for the distance leash (src/game/leash.h, docs/host-authority.md section 3): a replay of both
// players' positions at 1 Hz of the 2026-10-20 sessions (tests/data/leash_1020.h, tools/extract_leash.py): session A
// (at most 98 m apart) gives no warning, session B (the guest drove from 205 m to 674 m from the host in ~25 s) a
// warning at 300 m, the countdown at 400 m and one pull; then every suspension rule, the hysteresis, the host's
// config (wire, ini, who gets it), warning-only mode, the banners and a failed pull; after a pull, the hold on the
// spot while our game loads the ground there (the 2026-10-20 15:00 one-PC run: we fell through the street). Prints
// PASS/FAIL per check; exits non-zero on any failure.
#include "game/leash.h"

#include "data/leash_1020.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace mc;
using namespace mc::game::leash;

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

// ---- replay ----------------------------------------------------------------------------------------------------------
struct Replay {
    uint32_t warns = 0, countdowns = 0, pulls = 0, pulledEv = 0, backs = 0, falsePulls = 0;
    double warnAt = -1, countdownAt = -1, pullAt = -1;
    float warnDist = -1, countdownDist = -1, pullDist = -1, maxDist = 0;
};

// 10 Hz between the 1 Hz rows (linear). After a pull the guest stands 15 m behind the host and goes on from there
// with the moves the real guest made (his recorded path shifted by the pull).
Replay Run(const LeashRow* rows, size_t n, const Config& cfg = Config()) {
    Replay r;
    Leash l;
    l.SetConfig(cfg);
    float offX = 0, offY = 0;
    bool answer = false;
    for (size_t i = 0; i + 1 < n; ++i) {
        const LeashRow& a = rows[i];
        const LeashRow& b = rows[i + 1];
        for (int k = 0; k < 10; ++k) {
            const float u = k / 10.0f;
            Input in;
            in.now = a.t + (b.t - a.t) * u;
            in.inWorld = true;
            in.hostSeen = true;
            in.hostX = a.hx + (b.hx - a.hx) * u;
            in.hostY = a.hy + (b.hy - a.hy) * u;
            in.x = a.gx + (b.gx - a.gx) * u + offX;
            in.y = a.gy + (b.gy - a.gy) * u + offY;
            in.hostPaused = a.hostPaused != 0;
            in.hostDown = a.hostHp <= 0;
            in.selfDown = a.guestHp <= 0;
            in.hostCar = a.sameCar != 0;
            if (answer) {
                l.PullDone(true, in.now);
                answer = false;
            }
            const Output o = l.Update(in);
            if (o.dist > r.maxDist) r.maxDist = o.dist;
            for (const Event& e : o.events) {
                if (e.kind == EventKind::Warn && r.warnAt < 0) {
                    r.warnAt = in.now;
                    r.warnDist = o.dist;
                }
                if (e.kind == EventKind::Countdown && r.countdownAt < 0) {
                    r.countdownAt = in.now;
                    r.countdownDist = o.dist;
                }
                if (e.kind == EventKind::Pulled) ++r.pulledEv;
                if (e.kind == EventKind::Back) ++r.backs;
            }
            if (o.pull) {
                ++r.pulls;
                if (o.why != Why::None || o.dist < cfg.maxM) ++r.falsePulls;
                if (r.pullAt < 0) {
                    r.pullAt = in.now;
                    r.pullDist = o.dist;
                }
                // the glue: 15 m behind the host (the host's heading: from his last move; here simply south of him)
                offX += in.hostX - in.x;
                offY += in.hostY - 15.0f - in.y;
                answer = true;
            }
        }
    }
    r.warns = l.Warnings();
    r.countdowns = l.Countdowns();
    return r;
}

void TestReplay() {
    const Replay a = Run(kSessionA, std::size(kSessionA));
    Check(a.maxDist > 90 && a.maxDist < 100, "session A: the players at most ~98 m apart", F("%.1f m", a.maxDist));
    Check(a.warns == 0 && a.countdowns == 0 && a.pulls == 0, "session A: no warning, no countdown, no pull",
          F("warn %.0f countdown %.0f pull %.0f", a.warns, a.countdowns, a.pulls));

    const Replay b = Run(kSessionB, std::size(kSessionB));
    float rawMax = 0;
    for (const LeashRow& row : kSessionB) rawMax = std::max(rawMax, std::hypot(row.gx - row.hx, row.gy - row.hy));
    Check(rawMax > 600, "session B: the guest drove more than 600 m away (recorded)", F("%.1f m", rawMax));
    Check(b.maxDist < 600, "session B: with the leash he got no further than the pull", F("%.1f m", b.maxDist));
    Check(b.warns == 1 && b.warnDist >= 300 && b.warnDist < 315, "session B: one warning, at 300 m",
          F("%.0f warning(s), first at %.1f m (t %.1f s)", b.warns, b.warnDist, b.warnAt));
    Check(b.countdowns == 1 && b.countdownDist > 400 && b.countdownDist < 420, "session B: one countdown, at 400 m",
          F("%.0f countdown(s), first at %.1f m (t %.1f s)", b.countdowns, b.countdownDist, b.countdownAt));
    Check(b.pulls == 1 && b.pulledEv == 1, "session B: one pull", F("%.0f pull(s), %.0f pulled event(s)", b.pulls,
                                                                    b.pulledEv));
    Check(std::fabs(b.pullAt - b.countdownAt - 10.0) < 0.15, "session B: the pull 10 s after the countdown began",
          F("%.1f s after", b.pullAt - b.countdownAt));
    Check(b.falsePulls == 0 && b.pullDist >= 400, "session B: no false pull (no suspension, beyond 400 m)",
          F("pulled at %.1f m", b.pullDist));
    // the same drive with the host's config 500 / 600 m (docs/host-authority.md D1)
    const Replay b2 = Run(kSessionB, std::size(kSessionB), FromIni(500, 600, 10));
    Check(b2.warns == 1 && b2.warnDist >= 500 && b2.warnDist < 520 && b2.countdownDist > 600,
          "session B, host config 500/600: warning at 500 m, countdown at 600 m",
          F("warn %.1f m, countdown %.1f m, pulls %.0f", b2.warnDist, b2.countdownDist, b2.pulls));
    // warning only
    const Replay b3 = Run(kSessionB, std::size(kSessionB), FromIni(300, 400, 0));
    Check(b3.warns == 1 && b3.countdowns == 0 && b3.pulls == 0, "session B, warning only (countdown 0): no pull",
          F("warn %.0f countdown %.0f pull %.0f", b3.warns, b3.countdowns, b3.pulls));
}

// ---- a guest standing 450 m from the host, everything else normal ------------------------------------------------------
struct Sim {
    Leash l;
    Input in;
    double t = 0;
    uint32_t pulls = 0;
    std::vector<Event> events;
    Output last;
    Sim() {
        in.inWorld = true;
        in.hostSeen = true;
        in.x = 450;
        in.joinSettled = true;
    }
    // runs for s seconds at 10 Hz; answers every pull with ok
    void Run(double s, bool answer = true) {
        const int n = static_cast<int>(std::lround(s * 10));
        for (int i = 0; i < n; ++i) {
            t += 0.1;
            in.now = t;
            last = l.Update(in);
            for (const Event& e : last.events) events.push_back(e);
            if (last.pull) {
                ++pulls;
                if (answer) {
                    l.PullDone(true, t);
                    in.x = 0;
                    in.y = -15;
                }
            }
        }
    }
    bool Has(EventKind k) const {
        for (const Event& e : events)
            if (e.kind == k) return true;
        return false;
    }
};

void TestSuspensions() {
    {
        Sim s;
        s.Run(19.9);
        Check(s.pulls == 0 && s.last.why == Why::Joining, "join: suspended for 20 s after it was complete",
              WhyName(s.last.why));
        s.Run(10.6);
        Check(s.pulls == 1, "join: then the countdown and the pull (450 m)", F("%.0f pull(s)", s.pulls));
    }
    struct Rule {
        const char* name;
        Why why;
        void (*set)(Input&);
    };
    const Rule rules[] = {
        {"suspended: our world not loaded", Why::Loading, [](Input& i) { i.inWorld = false; }},
        {"suspended: a fail-flow reload", Why::FailReload, [](Input& i) { i.failReload = true; }},
        {"suspended: join / reconnect not complete", Why::Joining, [](Input& i) { i.joinSettled = false; }},
        {"suspended: no state of the host here", Why::NoHost, [](Input& i) { i.hostSeen = false; }},
        {"suspended: the host paused", Why::HostPaused, [](Input& i) { i.hostPaused = true; }},
        {"suspended: a cutscene", Why::Cutscene, [](Input& i) { i.cutscene = true; }},
        {"suspended: we are downed / dead", Why::SelfDown, [](Input& i) { i.selfDown = true; }},
        {"suspended: the host is downed / dead", Why::HostDown, [](Input& i) { i.hostDown = true; }},
        {"suspended: in the host's car", Why::HostCar, [](Input& i) { i.hostCar = true; }},
        {"suspended: passenger of another guest's car", Why::Passenger, [](Input& i) { i.passenger = true; }},
    };
    for (const Rule& r : rules) {
        Sim s;
        s.Run(25);  // join window over, nothing else: the countdown runs at 450 m
        s.events.clear();
        r.set(s.in);
        s.Run(60);
        Check(s.pulls == 0 && s.last.why == r.why && s.last.banner.empty() == (r.why != Why::HostPaused) &&
                  !s.Has(EventKind::Countdown),
              r.name, std::string(WhyName(s.last.why)) + " / '" + s.last.banner + "'");
    }
    {
        Sim s;
        s.Run(25);
        s.in.x = 100;  // near: nothing
        s.Run(1);
        s.in.inWorld = false;  // a loading screen
        s.Run(5);
        s.in.inWorld = true;
        s.in.x = 450;
        s.Run(4.8);
        Check(s.last.why == Why::AfterLoad && s.pulls == 0, "suspended: 5 s after our world loaded",
              WhyName(s.last.why));
        s.Run(0.4);
        Check(s.last.state == State::Countdown, "after the load: the countdown", StateName(s.last.state));
    }
    {
        Sim s;
        s.in.x = 100;
        s.Run(25);
        s.in.cutscene = true;
        s.Run(10);
        s.in.cutscene = false;
        s.in.x = 450;
        s.Run(4.8);
        Check(s.last.why == Why::Cutscene, "suspended: 5 s after a cutscene", WhyName(s.last.why));
        s.Run(0.4);
        Check(s.last.state == State::Countdown, "after the cutscene: the countdown", StateName(s.last.state));
    }
    {
        Sim s;
        s.in.x = 100;
        s.Run(25);
        s.in.hostX = 60;  // the host jumps 60 m in one state: a teleport
        s.in.x = 510;     // the guest is 450 m from the new spot
        s.Run(14.8);
        Check(s.last.why == Why::HostTeleport && s.pulls == 0, "suspended: 15 s after the host jumped > 50 m",
              WhyName(s.last.why));
        s.Run(0.4);
        Check(s.last.state == State::Countdown, "after the host's teleport: the countdown", StateName(s.last.state));
        Sim w;
        w.in.x = 100;
        w.Run(25);
        w.in.hostX = 40;  // 40 m in one state: not a teleport (a fast car's states are ~2 m apart)
        w.in.x = 140;
        w.Run(0.2);
        Check(w.last.why == Why::None, "a 40 m jump is not a teleport", WhyName(w.last.why));
    }
    {
        // the host pauses mid-countdown: frozen, THE HOST PAUSED; after the resume the countdown goes on
        Sim s;
        s.Run(25);  // the countdown began at 20 s: 5 s in
        const double leftBefore = s.last.left;
        s.in.hostPaused = true;
        s.Run(60);
        Check(s.pulls == 0 && s.last.state == State::Paused && s.last.banner == "THE HOST PAUSED" &&
                  std::fabs(s.last.left - leftBefore) < 0.01,
              "host paused: the countdown freezes, banner THE HOST PAUSED",
              s.last.banner + F(" (%.1f s left, before %.1f)", s.last.left, leftBefore));
        s.in.hostPaused = false;
        s.Run(leftBefore - 0.3);
        Check(s.pulls == 0 && s.last.state == State::Countdown, "host back: the countdown goes on from its value",
              StateName(s.last.state));
        s.Run(0.6);
        Check(s.pulls == 1, "host back: pulled when it runs out", F("%.0f pull(s)", s.pulls));
    }
    {
        // a passenger and his driver: only the driver's leash pulls (the passenger's is suspended, his car moves)
        Sim drv, pas;
        drv.Run(31);
        pas.in.passenger = true;
        pas.Run(31);
        Check(drv.pulls == 1 && pas.pulls == 0, "passengers: the driver is pulled, his passenger's leash waits",
              F("driver %.0f, passenger %.0f", drv.pulls, pas.pulls));
    }
}

void TestHysteresis() {
    Sim s;
    s.in.x = 100;
    s.Run(25);
    s.in.x = 301;
    s.Run(0.2);
    Check(s.last.state == State::Warn && s.events.size() == 1 && s.events[0].kind == EventKind::Warn &&
              s.events[0].distM == 301,
          "301 m: warning (event warn 301 m)", StateName(s.last.state));
    s.in.x = 290;
    s.Run(1);
    Check(s.last.state == State::Warn, "290 m: still the warning (hysteresis 20 m)", StateName(s.last.state));
    s.in.x = 410;
    s.Run(0.2);
    Check(s.last.state == State::Countdown && s.events.back().kind == EventKind::Countdown,
          "410 m: the countdown (event countdown)", StateName(s.last.state));
    s.in.x = 390;
    s.Run(1);
    Check(s.last.state == State::Countdown, "390 m: still counting down", StateName(s.last.state));
    s.in.x = 379;
    s.Run(0.2);
    Check(s.last.state == State::Warn && s.events.back().kind == EventKind::Warn,
          "379 m: the countdown stops, back to the warning", StateName(s.last.state));
    s.in.x = 285;
    s.Run(0.2);
    Check(s.last.state == State::Warn, "285 m: still the warning", StateName(s.last.state));
    s.in.x = 279;
    s.Run(0.2);
    Check(s.last.state == State::Near && s.events.back().kind == EventKind::Back && s.last.banner.empty(),
          "279 m: back (event back), no banner", StateName(s.last.state));
    // between max - 20 m and max when the countdown runs out: no pull until he is beyond max again
    s.in.x = 450;
    s.Run(5);
    s.in.x = 390;
    s.Run(8);
    Check(s.pulls == 0 && s.last.state == State::Countdown && s.last.left == 0,
          "390 m at 0 s: no pull (under 400 m), waits", F("%.1f s left", s.last.left));
    s.in.x = 401;
    s.Run(0.2);
    Check(s.pulls == 1, "401 m at 0 s: pulled", F("%.0f pull(s)", s.pulls));
}

void TestBanners() {
    Check(Banner(State::Warn, 341, 0) == "FAR FROM THE HOST - THE STREETS THIN OUT (340 M)", "banner: warning (340 M)",
          Banner(State::Warn, 341, 0));
    Check(Banner(State::Countdown, 420, 10) == "TOO FAR FROM THE HOST - BACK IN 10 S", "banner: countdown 10 S");
    Check(Banner(State::Countdown, 420, 3.2) == "TOO FAR FROM THE HOST - BACK IN 4 S", "banner: counts down (ceil)");
    Check(Banner(State::Paused, 420, 3) == "THE HOST PAUSED", "banner: THE HOST PAUSED");
    Check(Banner(State::Pulled, 5, 0) == "PULLED BACK TO THE HOST", "banner: PULLED BACK TO THE HOST");
    Check(Banner(State::Near, 5, 0).empty() && Banner(State::Suspended, 500, 0).empty(), "banner: none near / suspended");

    Sim s;
    s.Run(20.25);
    Check(s.last.banner == "TOO FAR FROM THE HOST - BACK IN 10 S", "countdown starts at BACK IN 10 S", s.last.banner);
    s.Run(10.1);
    Check(s.pulls == 1 && s.last.banner == "PULLED BACK TO THE HOST", "pulled: PULLED BACK TO THE HOST",
          s.last.banner);
    s.Run(2.5);
    Check(s.last.banner == "PULLED BACK TO THE HOST", "... for 3 s", s.last.banner);
    s.Run(0.6);
    Check(s.last.banner.empty() && s.last.state == State::Near, "... then gone", StateName(s.last.state));
    Check(s.Has(EventKind::Pulled), "pulled: event pulled to the host");
}

void TestPullFails() {
    Sim s;
    s.Run(30.5, false);  // nobody answers
    Check(s.pulls == 1 && s.last.state == State::Pulling, "pull out, no answer yet", StateName(s.last.state));
    s.l.PullDone(false, s.t);
    s.Run(1.9, false);
    Check(s.pulls == 1 && s.last.state == State::Countdown, "a failed pull: back to 0 s, tried again after 2 s",
          F("%.0f pull(s)", s.pulls));
    s.Run(0.2, false);
    Check(s.pulls == 2, "... the second try", F("%.0f pull(s)", s.pulls));
    s.Run(3.2, false);  // no answer at all within 3 s: failed, and again 2 s later
    s.Run(2.0, false);
    Check(s.pulls == 3, "no answer within 3 s: counted as failed, tried again", F("%.0f pull(s)", s.pulls));
}

void TestEventsRepeat() {
    Sim s;
    s.in.x = 100;
    s.Run(25);
    s.in.x = 350;
    s.Run(16);
    size_t warns = 0;
    for (const Event& e : s.events) warns += e.kind == EventKind::Warn;
    Check(warns == 4, "while far: the event again every 5 s (the host's panel)",
          F("%.0f warn events in 16 s", static_cast<double>(warns)));
}

void TestConfig() {
    net::ByteWriter w(8);
    Config c = FromIni(250, 350, 12);
    Write(w, c);
    Check(w.Size() == 6, "LeashConfig: 6 bytes", std::to_string(w.Size()));
    net::ByteReader r(w.Span());
    Config got;
    Check(Read(r, got) && got == c && got.warnM == 250 && got.maxM == 350 && got.countdownS == 12 && got.On(),
          "LeashConfig: round trip");
    net::ByteWriter w2(4);
    w2.U16(300);
    net::ByteReader r2(w2.Span());
    Check(!Read(r2, got), "LeashConfig: a short body is rejected");

    net::ByteWriter we(4);
    Write(we, Event{EventKind::Countdown, 420});
    net::ByteReader re(we.Span());
    Event e;
    Check(we.Size() == 3 && Read(re, e) && e.kind == EventKind::Countdown && e.distM == 420,
          "LeashEvent: 3 bytes, round trip");
    net::ByteWriter we2(4);
    we2.U8(9);
    we2.U16(1);
    net::ByteReader re2(we2.Span());
    Check(!Read(re2, e), "LeashEvent: an unknown kind is rejected");

    Check(FromIni(300, 200, 10).maxM == 300, "ini: a max under warn is raised to warn");
    Check(!FromIni(0, 400, 10).On(), "ini: leash_warn_m = 0 turns the leash off");
    Check(FromIni(300, 400, 0).On() && !FromIni(300, 400, 0).PullOn(), "ini: leash_countdown_s = 0 is warning only");
    Check(FromIni(-5, 99999, 999).warnM == 0 && FromIni(-5, 99999, 999).maxM == 65535 &&
              FromIni(-5, 99999, 999).countdownS == 255,
          "ini: values clamped to the wire's range");

    // off: no banner, no events
    Sim off;
    off.l.SetConfig(FromIni(0, 400, 10));
    off.Run(40);
    Check(off.pulls == 0 && off.events.empty() && off.last.why == Why::Off, "off: nothing", WhyName(off.last.why));
    // the guest uses the host's values (a LeashConfig that arrives mid-session)
    Sim g;
    g.in.x = 330;
    g.Run(25);
    Check(g.last.state == State::Warn, "default config: 330 m is a warning", StateName(g.last.state));
    g.l.SetConfig(FromIni(350, 450, 10));
    g.in.x = 320;
    g.Run(0.2);
    Check(g.last.state == State::Near, "the host's 350/450: 320 m is near now", StateName(g.last.state));
    g.in.x = 455;
    g.Run(0.2);
    Check(g.last.state == State::Countdown, "the host's 350/450: 455 m counts down", StateName(g.last.state));

    // who gets it
    ConfigSender cs;
    std::vector<std::pair<uint32_t, uint32_t>> guests = {{1, 1}};
    auto p = cs.Tick(Config(), guests);
    Check(!p.broadcast && p.to.size() == 1 && p.to[0] == 1, "host: a new guest gets the config");
    p = cs.Tick(Config(), guests);
    Check(!p.broadcast && p.to.empty(), "host: once");
    guests = {{1, 1}, {2, 2}};
    p = cs.Tick(Config(), guests);
    Check(p.to.size() == 1 && p.to[0] == 2, "host: the next guest gets it");
    guests = {{1, 3}, {2, 2}};
    p = cs.Tick(Config(), guests);
    Check(p.to.size() == 1 && p.to[0] == 1, "host: a guest back after a lost connection (new joinSeq) gets it again");
    p = cs.Tick(FromIni(500, 600, 10), guests);
    Check(p.broadcast && p.to.empty(), "host: a changed ini goes to everyone");
    p = cs.Tick(FromIni(500, 600, 10), guests);
    Check(!p.broadcast && p.to.empty(), "host: then nothing");
}

}  // namespace

// After the pull (Settle). The recorded fall (guest trace 145536, LR at ~3 Hz after the pull at 15:05:13.0): put at
// z 2.86 beside the host, then z 1.06, -3.46, -9.19, -15.20 (falling through the street: our game had not loaded the
// ground there yet), downed at health 1. Model of the game: we fall from rest under gravity whenever there is no
// ground and each HOLD puts us back on the spot; the ground is there `loadS` s after the pull.
void TestSettle() {
    {
        Settle s;
        s.Start(0, -951.04f, -219.57f, 2.86f);
        const double t[] = {0.05, 0.35, 0.71, 1.06, 1.40};
        const float z[] = {2.86f, 1.06f, -3.46f, -9.19f, -15.20f};
        std::string holds;
        for (int i = 0; i < 5; ++i) holds += s.Update(t[i], z[i]) ? '1' : '0';
        Check(holds[0] == '0' && holds[1] == '1', "settle: the recorded fall is caught at its first reading under the "
              "spot (1.8 m down)", holds);
    }
    for (double loadS : {0.0, 0.8, 1.5, 2.5, 6.0, 9.0}) {  // 6 / 9 s: the 06:25 run's car (ground later than 3 s)
        Settle s;
        const float spot = 2.86f;
        s.Start(0, 0, 0, spot);
        float z = spot, vz = 0, minZ = spot;
        int holds = 0;
        bool hold = false;
        for (double t = 0; t < 12.0; t += 1.0 / 60) {
            if (hold) {  // the glue's HOLD on the next tick
                z = spot;
                vz = 0;
                hold = false;
            }
            if (t < loadS) {  // no ground yet: falling (once it is there we stand where the last hold put us)
                vz -= 9.81f / 60;
                z += vz / 60;
            }
            minZ = std::min(minZ, z);
            if (s.Update(t, z)) {
                hold = true;
                ++holds;
            }
        }
        char name[120];
        std::snprintf(name, sizeof name, "settle: the ground loads %.1f s after the pull: never ~2 m under the spot",
                      loadS);
        Check(spot - minZ <= kSettleDropM + 0.6f && holds <= kSettleMax, name,
              F("lowest %.2f m under it, %.0f holds", spot - minZ, holds));
    }
    {
        Settle s;  // the ground is there: a pull onto a kerb, a slope 1 m down the street - nothing held
        s.Start(0, 0, 0, 3.0f);
        bool any = false;
        for (double t = 0; t < 4.0; t += 1.0 / 60) any |= s.Update(t, 3.0f - static_cast<float>(t) * 0.3f);
        Check(!any, "settle: walking down 1 m after the pull: no hold");
        Settle late;
        late.Start(0, 0, 0, 3.0f);
        Check(!late.Update(kSettleS + 0.1, -20.0f), "settle: only within kSettleS of the pull (a fall later is the game's)");
        Settle none;
        Check(!none.Update(1.0, -20.0f), "settle: no pull, no hold");
    }
}

int main() {
    TestSettle();
    TestReplay();
    TestSuspensions();
    TestHysteresis();
    TestBanners();
    TestPullFails();
    TestEventsRepeat();
    TestConfig();
    std::printf("%s (%d failure%s)\n", g_failures ? "FAILED" : "OK", g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
