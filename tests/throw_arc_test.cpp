// throw_arc_test: unit tests for the flight of thrown explosives on every game (src/game/throw_model.h): the Throw /
// ThrowFlight wire format (round trips, malformed messages refused) and the Explosion's new throw number (old and new
// layout), the arc math (ballistic positions, the launch from two samples, the gravity from three, the aim model),
// the receiver's flight playback (arc only, landing, interpolated points, before the first point, extrapolation,
// end point), the Book (duplicates, points before their throw, the explosion ending the right flight by seq or by
// distance, timeouts, a player leaving), the thrower's Tracker (launch measured on the projectile, 20 Hz points,
// bounces, the end point, the aim model when no projectile is found, giving up) and the explosion detector naming the
// throw; and a whole exchange walked through from release to explosion.
// Prints PASS/FAIL per check; exits non-zero on any failure.
#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

#include "game/explosion_model.h"
#include "game/throw_model.h"

using namespace mc::game;
using namespace mc::game::arc;
using boom::Vec3;

namespace {

int g_failures = 0;

void Check(bool ok, const char* name, const std::string& detail = {}) {
    std::printf("%s  %-74s %s\n", ok ? "PASS" : "FAIL", name, detail.c_str());
    std::fflush(stdout);
    if (!ok) ++g_failures;
}

Vec3 P(float x, float y, float z) { return {x, y, z}; }
bool Near(const Vec3& a, const Vec3& b, float eps) { return boom::Dist(a, b) <= eps; }
std::string Str(const Vec3& v) {
    char b[96];
    std::snprintf(b, sizeof b, "(%.3f %.3f %.3f)", v.x, v.y, v.z);
    return b;
}

ThrowMsg SampleThrow() {
    ThrowMsg t;
    t.seq = 0x00ABCDEF;
    t.kind = Kind::Molotov;
    t.flags = kFire | kFromEntity | kHasLanding;
    t.origin = P(-943.5f, -233.25f, 5.5f);
    t.vel = P(8.0f, 9.0f, 6.5f);
    t.gravity = 9.81f;
    t.delayMs = 40;
    t.flightMs = 1500;
    t.landing = P(-930.0f, -219.0f, 3.0f);
    t.weapon = "exp_molotov";
    return t;
}

template <typename T, typename F>
bool RoundTrip(const std::vector<uint8_t>& buf, T& out, F read) {
    mc::net::ByteReader r(buf.data(), buf.size());
    return read(r, out);
}

void TestWire() {
    const ThrowMsg t = SampleThrow();
    const auto buf = PackThrow(t);
    ThrowMsg back;
    const bool ok = RoundTrip(buf, back, ReadThrow);
    Check(ok && back.seq == t.seq && back.kind == t.kind && back.flags == t.flags && Near(back.origin, t.origin, 1e-4f) &&
              Near(back.vel, t.vel, 1e-4f) && back.gravity == t.gravity && back.delayMs == 40 && back.flightMs == 1500 &&
              Near(back.landing, t.landing, 1e-4f) && back.weapon == "exp_molotov",
          "Throw round trip");

    auto bad = [&](const char* name, auto edit) {
        ThrowMsg m = SampleThrow();
        edit(m);
        const auto b = PackThrow(m);
        ThrowMsg o;
        Check(!RoundTrip(b, o, ReadThrow), name);
    };
    bad("Throw: kind out of range refused", [](ThrowMsg& m) { m.kind = static_cast<Kind>(9); });
    bad("Throw: 200 m/s refused", [](ThrowMsg& m) { m.vel = P(200, 0, 0); });
    bad("Throw: gravity 0 refused", [](ThrowMsg& m) { m.gravity = 0; });
    bad("Throw: NaN origin refused", [](ThrowMsg& m) { m.origin.x = std::numeric_limits<float>::quiet_NaN(); });
    bad("Throw: far-away landing refused", [](ThrowMsg& m) { m.landing.z = 1e7f; });
    {
        ThrowMsg o;
        std::vector<uint8_t> cut(buf.begin(), buf.begin() + 20);
        Check(!RoundTrip(cut, o, ReadThrow), "Throw: truncated refused");
    }
    {
        ThrowMsg m = SampleThrow();
        m.flags = 0xFF;
        m.weapon = "bad name!";  // not a safe name: sent empty
        ThrowMsg o;
        const bool r = RoundTrip(PackThrow(m), o, ReadThrow);
        Check(r && o.flags == (kFire | kFromEntity | kHasLanding) && o.weapon.empty(), "Throw: unknown flags masked, unsafe name sent empty");
        m.flags = kFire;
        Check(RoundTrip(PackThrow(m), o, ReadThrow) && o.flightMs == 0, "Throw: no landing flag -> flightMs 0");
    }

    FlightMsg f;
    f.seq = 77;
    f.points = {{50, 0, P(1, 2, 3)}, {100, kBounce, P(4, 5, 6)}, {150, kEnd, P(7, 8, 9)}};
    FlightMsg fb;
    Check(RoundTrip(PackFlight(f), fb, ReadFlight) && fb.seq == 77 && fb.points.size() == 3 && fb.points[1].tMs == 100 &&
              fb.points[1].flags == kBounce && fb.points[2].flags == kEnd && Near(fb.points[2].pos, P(7, 8, 9), 1e-5f),
          "ThrowFlight round trip (3 points)");
    FlightMsg many;
    many.seq = 1;
    for (int i = 0; i < 12; ++i) many.points.push_back({static_cast<uint16_t>(i * 50), 0, P(0, 0, static_cast<float>(i))});
    Check(RoundTrip(PackFlight(many), fb, ReadFlight) && fb.points.size() == kMaxPointsPerMsg,
          "ThrowFlight: at most 8 points per message");
    {
        mc::net::ByteWriter w(16);
        w.U32(5);
        w.U8(0);
        FlightMsg o;
        mc::net::ByteReader r(w.Span().data(), w.Span().size());
        Check(!ReadFlight(r, o), "ThrowFlight: zero points refused");
    }
    {
        FlightMsg nan;
        nan.seq = 2;
        nan.points = {{0, 0, P(std::numeric_limits<float>::infinity(), 0, 0)}};
        FlightMsg o;
        Check(!RoundTrip(PackFlight(nan), o, ReadFlight), "ThrowFlight: infinite point refused");
    }

    // the Explosion's throw number: new layout carries it, the old one (without it) reads as 0
    boom::Explosion e;
    e.seq = 9;
    e.kind = Kind::Grenade;
    e.pos = P(1, 2, 3);
    e.weapon = "exp_grenade";
    e.throwSeq = 0x00123456;
    boom::Explosion eb;
    const auto eBuf = boom::PackExplosion(e);
    Check(RoundTrip(eBuf, eb, boom::ReadExplosion) && eb.throwSeq == 0x00123456 && eb.seq == 9,
          "Explosion carries the throw number");
    std::vector<uint8_t> old(eBuf.begin(), eBuf.end() - 4);  // what a sender before the grenade arc sends
    Check(RoundTrip(old, eb, boom::ReadExplosion) && eb.throwSeq == 0 && eb.weapon == "exp_grenade",
          "Explosion without the throw number (older sender) still read, throw 0");
}

void TestMath() {
    const Vec3 o = P(10, 20, 5), v = P(3, 4, 6);
    const Vec3 at = Ballistic(o, v, 9.81f, 1.0);
    Check(Near(at, P(13, 24, 5 + 6 - 4.905f), 1e-4f), "Ballistic position after 1 s", Str(at));
    const Vec3 vl = BallisticVel(v, 9.81f, 0.5);
    Check(std::fabs(vl.z - (6 - 4.905f)) < 1e-4f && vl.x == 3, "Ballistic velocity after 0.5 s");
    // two samples of a real flight give its launch back
    const double t1 = 0.0, t2 = 0.033;
    const Vec3 est = EstimateLaunch(Ballistic(o, v, 9.81f, t1), t1, Ballistic(o, v, 9.81f, t2), t2, 9.81f);
    Check(Near(est, v, 1e-3f), "EstimateLaunch recovers the launch velocity", Str(est));
    Check(Near(EstimateLaunch(o, 1.0, o, 1.0, 9.81f), P(0, 0, 0), 0), "EstimateLaunch: same instant -> zero");
    float g = 0;
    const bool fit = FitGravity(Ballistic(o, v, 9.81f, 0.0), 0.0, Ballistic(o, v, 9.81f, 0.1), 0.1,
                                Ballistic(o, v, 9.81f, 0.2), 0.2, g);
    Check(fit && std::fabs(g - 9.81f) < 0.05f, "FitGravity measures 9.81", std::to_string(g));
    const bool fit2 = FitGravity(Ballistic(o, v, 14.0f, 0.0), 0.0, Ballistic(o, v, 14.0f, 0.07), 0.07,
                                 Ballistic(o, v, 14.0f, 0.2), 0.2, g);
    Check(fit2 && std::fabs(g - 14.0f) < 0.05f, "FitGravity measures another gravity (uneven steps)", std::to_string(g));
    // a bounce between the samples (going up again fast): not a plausible gravity
    Check(!FitGravity(P(0, 0, 1), 0.0, P(0, 0, 0), 0.1, P(0, 0, 2), 0.2, g), "FitGravity refuses a bounce in between");
    Check(!FitGravity(o, 0.0, o, 0.001, o, 0.002, g), "FitGravity refuses samples too close in time");

    // the aim model: a level camera direction is thrown 15 deg up
    const Vec3 a = AimLaunch(P(0, 1, 0), 16.0f, 0.2617994f);
    float yaw = 0, pitch = 0;
    YawPitch(a, yaw, pitch);
    Check(std::fabs(boom::Dist(a, P(0, 0, 0)) - 16.0f) < 1e-3f && std::fabs(pitch - 15.0f) < 0.05f && std::fabs(yaw) < 0.05f,
          "AimLaunch: speed kept, 15 deg up, yaw kept", Str(a));
    const Vec3 up = AimLaunch(P(0, 0, 1), 10.0f, 0.5f);
    YawPitch(up, yaw, pitch);
    Check(pitch <= 85.01f && pitch > 80.0f, "AimLaunch: straight up clamped to 85 deg", std::to_string(pitch));
    YawPitch(P(1, 0, 0), yaw, pitch);
    Check(std::fabs(yaw - 90.0f) < 1e-3f && std::fabs(pitch) < 1e-3f, "YawPitch: +x is yaw 90");
}

Flight MakeFlight(const ThrowMsg& t, double t0) {
    Flight f;
    f.from = 2;
    f.t = t;
    f.t0 = t0;
    return f;
}

void TestPlayback() {
    ThrowMsg t = SampleThrow();
    t.flags = kFromEntity;  // no landing
    t.flightMs = 0;
    Flight f = MakeFlight(t, 100.0);
    Visual v = PositionAt(f, 0.5);
    Check(Near(v.pos, Ballistic(t.origin, t.vel, t.gravity, 0.5), 1e-4f) && !v.held && std::string(v.src) == "arc",
          "no points: the ballistic arc");
    v = PositionAt(f, 5.0);
    Check(v.held && Near(v.pos, Ballistic(t.origin, t.vel, t.gravity, 3.0), 1e-3f), "no points, no landing: held after 3 s");
    t.flags = kHasLanding;
    t.flightMs = 1200;
    f = MakeFlight(t, 100.0);
    v = PositionAt(f, 1.3);
    Check(v.held && Near(v.pos, t.landing, 1e-5f) && std::string(v.src) == "landing", "landing predicted: held there after flightMs");

    // streamed points: the true flight bends off the sent arc (a wall, a different speed)
    t = SampleThrow();
    t.flags = kFromEntity;
    f = MakeFlight(t, 0.0);
    f.points = {{100, 0, P(0, 0, 10)}, {200, 0, P(1, 0, 10)}, {300, 0, P(2, 0, 9.5f)}};
    v = PositionAt(f, 0.25);
    Check(Near(v.pos, P(1.5f, 0, 9.75f), 1e-4f) && std::string(v.src) == "points", "between two points: interpolated",
          Str(v.pos));
    v = PositionAt(f, 0.1);
    Check(Near(v.pos, P(0, 0, 10), 1e-4f), "at the first point: exactly there");
    v = PositionAt(f, 0.0);
    Check(Near(v.pos, t.origin, 1e-3f), "at age 0: the origin (the bend toward the first point starts at 0)", Str(v.pos));
    v = PositionAt(f, 0.05);
    {
        const Vec3 arcAt = Ballistic(t.origin, t.vel, t.gravity, 0.05), arcFirst = Ballistic(t.origin, t.vel, t.gravity, 0.1);
        const Vec3 want = P(arcAt.x + (0 - arcFirst.x) * 0.5f, arcAt.y + (0 - arcFirst.y) * 0.5f,
                            arcAt.z + (10 - arcFirst.z) * 0.5f);
        Check(Near(v.pos, want, 1e-3f), "before the first point: the arc bent halfway toward it", Str(v.pos));
    }
    // past the last point: carried on with the velocity of the last two points (1 m/0.1 s, falling), then held
    v = PositionAt(f, 0.4);
    const Vec3 exp = Ballistic(P(2, 0, 9.5f), BallisticVel(EstimateLaunch(P(1, 0, 10), 0.2, P(2, 0, 9.5f), 0.3, t.gravity),
                                                            t.gravity, 0.1),
                               t.gravity, 0.1);
    Check(Near(v.pos, exp, 1e-3f) && !v.held && std::string(v.src) == "extrapolated", "past the last point: extrapolated",
          Str(v.pos) + " vs " + Str(exp));
    Check(v.pos.x > 2.5f, "extrapolation keeps going the way it went");
    v = PositionAt(f, 3.0);
    Check(v.held && v.pos.z >= 9.5f - 1.5f - 1e-4f, "extrapolation held after 0.5 s, not through the ground");
    f.points.push_back({350, kEnd, P(2.3f, 0, 9.0f)});
    v = PositionAt(f, 2.0);
    Check(v.held && Near(v.pos, P(2.3f, 0, 9.0f), 1e-5f) && std::string(v.src) == "end", "end point: held there");
    f.points.back().flags = kBounce;
    v = PositionAt(f, 2.0);
    Check(Near(v.pos, P(2.3f, 0, 9.0f), 1e-5f), "after a bounce point: held at the bounce (no velocity known)");
}

void TestBook() {
    Book b;
    ThrowMsg t = SampleThrow();
    t.seq = 10;
    t.delayMs = 100;
    Check(b.Add(2, t, 50.0) && b.Count() == 1, "Book: a throw added");
    Check(!b.Add(2, t, 50.1), "Book: the same throw again (relayed twice) ignored");
    Check(b.Find(2, 10) && std::fabs(b.Find(2, 10)->t0 - 49.9) < 1e-9, "Book: origin time = receive time - delay");
    ThrowMsg t2 = t;
    Check(b.Add(3, t2, 50.0) && b.Count() == 2, "Book: the same seq from another player is another flight");

    // points that overtook their throw
    FlightMsg early;
    early.seq = 11;
    early.points = {{50, 0, P(1, 1, 1)}};
    b.AddPoints(2, early, 60.0);
    ThrowMsg t3 = t;
    t3.seq = 11;
    t3.delayMs = 0;
    b.Add(2, t3, 60.05);
    Check(b.Find(2, 11) && b.Find(2, 11)->points.size() == 1, "Book: points that came before their throw are attached");
    FlightMsg dup = early;
    b.AddPoints(2, dup, 60.1);
    Check(b.Find(2, 11)->points.size() == 1, "Book: a point received twice is kept once");
    FlightMsg more;
    more.seq = 11;
    more.points = {{150, 0, P(3, 3, 3)}, {100, kBounce, P(2, 2, 2)}};
    b.AddPoints(2, more, 60.2);
    const Flight* f = b.Find(2, 11);
    Check(f->points.size() == 3 && f->points[1].tMs == 100 && f->points[2].tMs == 150 && f->bounces == 1,
          "Book: points kept in time order, bounces counted");

    // the explosion that names its throw ends exactly that one
    Ended e;
    Check(b.EndByExplosion(2, 11, P(3, 3, 3), 61.0, e) && e.seq == 11 && e.from == 2 && e.why == EndWhy::Explosion &&
              e.points == 3 && !b.Find(2, 11),
          "Book: explosion with throw seq ends that flight");
    Check(!b.EndByExplosion(2, 11, P(3, 3, 3), 61.0, e), "Book: a second explosion for it finds nothing");
    Check(!b.EndByExplosion(2, 999, P(3, 3, 3), 61.0, e) && b.Count() == 2, "Book: an unknown throw seq ends nothing");
    // without a seq: the sender's nearest flight within reach
    Book b2;
    ThrowMsg a = SampleThrow(), c = SampleThrow();
    a.seq = 1, a.flags = kHasLanding, a.flightMs = 500, a.landing = P(0, 0, 0), a.delayMs = 0;
    c.seq = 2, c.flags = kHasLanding, c.flightMs = 500, c.landing = P(100, 0, 0), c.delayMs = 0;
    b2.Add(5, a, 0.0);
    b2.Add(5, c, 0.0);
    b2.Add(6, a, 0.0);
    Check(b2.EndByExplosion(5, 0, P(98, 1, 0), 2.0, e) && e.seq == 2 && e.from == 5, "Book: no seq -> the sender's nearest flight");
    Check(!b2.EndByExplosion(5, 0, P(500, 0, 0), 2.0, e), "Book: no seq and nothing within 30 m -> none");
    Check(b2.EndByExplosion(5, 0, P(1, 0, 0), 2.0, e) && e.seq == 1 && b2.Find(6, 1), "Book: another sender's flight is not taken");

    // timeouts and the end point
    Book b3;
    ThrowMsg x = SampleThrow();
    x.seq = 1, x.delayMs = 0;
    b3.Add(2, x, 0.0);
    x.seq = 2;
    b3.Add(2, x, 0.0);
    FlightMsg end;
    end.seq = 2;
    end.points = {{900, kEnd, P(5, 5, 5)}};
    b3.AddPoints(2, end, 1.0);
    std::vector<Ended> out;
    b3.Tick(3.9, out);
    Check(out.empty() && b3.Count() == 2, "Book: an ended flight is held a while for its explosion");
    b3.Tick(4.1, out);
    Check(out.size() == 1 && out[0].seq == 2 && out[0].why == EndWhy::EndPoint && Near(out[0].pos, P(5, 5, 5), 1e-4f),
          "Book: end point + 3 s without an explosion -> gone");
    out.clear();
    b3.Tick(kMaxFlight + 0.1, out);
    Check(out.size() == 1 && out[0].seq == 1 && out[0].why == EndWhy::Timeout && b3.Count() == 0, "Book: 15 s -> timeout");

    // a player leaves: his flights go
    Book b4;
    b4.Add(2, x, 0.0);
    x.seq = 3;
    b4.Add(3, x, 0.0);
    out.clear();
    b4.Forget(2, 1.0, out);
    Check(out.size() == 1 && out[0].why == EndWhy::Left && b4.Count() == 1, "Book: a leaving player's flights go");
    // capacity
    Book b5;
    for (uint32_t i = 1; i <= kMaxFlights + 4; ++i) {
        ThrowMsg m = SampleThrow();
        m.seq = i;
        m.delayMs = 0;
        b5.Add(2, m, static_cast<double>(i));
    }
    Check(b5.Count() == kMaxFlights && !b5.Find(2, 1) && b5.Find(2, kMaxFlights + 4), "Book: full -> the oldest goes");
    std::vector<Visual> vis;
    b5.Visuals(30.0, vis);
    Check(vis.size() == kMaxFlights, "Book: one visual per flight");
}

void TestTracker() {
    Tracker tr;
    std::vector<Out> out;
    std::vector<Summary> done;
    const Vec3 o = P(100, 200, 31.6f), v = P(5, 10, 6);
    tr.Begin(7, Kind::Grenade, "exp_grenade", 10.0, P(100, 199.5f, 30));
    tr.Model(7, P(100, 200.2f, 31.7f), P(4, 12, 5), true, P(110, 225, 30), 1600);
    Check(tr.Active(7) && tr.Current() == 7, "Tracker: a throw begun");
    tr.Sample(7, 10.10, o, out);
    Check(out.empty(), "Tracker: one sample is not enough for a launch");
    tr.Sample(7, 10.133, Ballistic(o, v, kGravity, 0.033), out);
    Check(out.size() == 1 && out[0].type == Out::Type::Throw && out[0].reliable, "Tracker: two samples -> the Throw (reliable)");
    if (!out.empty()) {
        const ThrowMsg& m = out[0].t;
        Check(m.seq == 7 && m.kind == Kind::Grenade && (m.flags & kFromEntity) && !(m.flags & kHasLanding) &&
                  Near(m.origin, o, 1e-4f) && Near(m.vel, v, 2e-3f) && m.delayMs == 33 && m.weapon == "exp_grenade",
              "Tracker: origin = first sample, velocity measured, no model landing", Str(m.vel));
    }
    out.clear();
    // samples every 1/60 s: points at 20 Hz
    int points = 0;
    for (int i = 3; i <= 62; ++i) {
        const double t = 10.10 + i / 60.0;
        tr.Sample(7, t, Ballistic(o, v, kGravity, t - 10.10), out);
    }
    for (const Out& x : out) {
        Check(x.type == Out::Type::Points && !x.reliable, "Tracker: flight points unreliable");
        points += static_cast<int>(x.f.points.size());
        if (x.type != Out::Type::Points || x.reliable) break;
    }
    Check(points >= 18 && points <= 21, "Tracker: ~20 points per second", std::to_string(points));
    if (!out.empty() && !out.back().f.points.empty()) {
        const Point& last = out.back().f.points.back();
        Check(std::fabs(last.tMs / 1000.0 - (62 / 60.0 - 0.0)) < 0.06, "Tracker: point times since the origin", std::to_string(last.tMs));
    }
    out.clear();
    tr.Bounce(7, 11.2, P(112, 222, 30), out);
    Check(out.size() == 1 && out[0].f.points.size() == 1 && out[0].f.points[0].flags == kBounce, "Tracker: a bounce is a point");
    out.clear();
    const Vec3 boomAt = P(113, 224, 30.1f);
    tr.End(7, 12.0, &boomAt, "explosion", out, done);
    Check(out.size() == 1 && out[0].reliable && out[0].f.points.size() == 1 && out[0].f.points[0].flags == kEnd &&
              Near(out[0].f.points[0].pos, boomAt, 1e-5f),
          "Tracker: the end point, reliable, where it exploded");
    Check(done.size() == 1 && done[0].seq == 7 && done[0].sent && done[0].fromEntity && done[0].samples == 62 &&
              std::string(done[0].why) == "explosion" && std::fabs(done[0].gravity - kGravity) < 0.1f &&
              done[0].modelSpeed > 13.0f && done[0].bounces == 1,
          "Tracker: summary (samples, gravity measured, model vs real)", std::to_string(done[0].gravity));
    Check(!tr.Active(7) && !tr.Any(), "Tracker: over");

    // no projectile found: the aim model's launch after kEntityWait
    out.clear();
    done.clear();
    tr.Begin(8, Kind::Molotov, "exp_molotov", 20.0, P(0, 0, 0));
    tr.Model(8, P(0, 0, 1.6f), P(0, 12, 4), true, P(0, 18, 0), 1400);
    tr.Tick(20.3, out, done);
    Check(out.empty(), "Tracker: waiting for the projectile");
    tr.Tick(20.0 + kEntityWait + 0.01, out, done);
    Check(out.size() == 1 && out[0].type == Out::Type::Throw && !(out[0].t.flags & kFromEntity) &&
              (out[0].t.flags & kHasLanding) && (out[0].t.flags & kFire) && out[0].t.flightMs == 1400 &&
              Near(out[0].t.origin, P(0, 0, 1.6f), 1e-5f) && out[0].t.delayMs == 610,
          "Tracker: aim model sent (landing, fire), delay = since the release");
    out.clear();
    tr.Sample(8, 21.0, P(0, 9, 3), out);  // the projectile found late: its positions correct the receivers' arc
    Check(out.size() == 1 && out[0].type == Out::Type::Points && out[0].f.points[0].tMs == 1000,
          "Tracker: a late projectile streams points relative to the release");
    out.clear();
    tr.Tick(20.0 + kTrackMax + 0.1, out, done);
    Check(done.size() == 1 && std::string(done[0].why) == "timeout" && !done[0].fromEntity && !tr.Any(),
          "Tracker: followed at most 15 s");

    // no projectile and no model: given up, nothing sent
    out.clear();
    done.clear();
    tr.Begin(9, Kind::Unknown, "", 30.0, P(0, 0, 0));
    tr.Tick(32.1, out, done);
    Check(out.empty() && done.size() == 1 && !done[0].sent && std::string(done[0].why) == "no_projectile_no_model",
          "Tracker: no projectile, no model -> nothing sent");
    // a jump between the first two readings (the entity put into the hand first): re-anchored
    out.clear();
    done.clear();
    tr.Begin(10, Kind::Grenade, "exp_grenade", 40.0, P(0, 0, 0));
    tr.Sample(10, 40.10, P(0, 0, -50), out);
    tr.Sample(10, 40.12, P(0, 0.2f, 1.6f), out);
    Check(out.empty(), "Tracker: an impossible launch speed is not sent");
    tr.Sample(10, 40.14, P(0, 0.5f, 1.7f), out);
    Check(out.size() == 1 && Near(out[0].t.origin, P(0, 0.2f, 1.6f), 1e-5f), "Tracker: re-anchored on the next samples");
    // End before anything was sent: summary only
    out.clear();
    tr.Begin(11, Kind::Grenade, "exp_grenade", 50.0, P(0, 0, 0));
    tr.End(11, 50.1, nullptr, "gone", out, done);
    Check(out.empty() && !done.empty() && !done.back().sent, "Tracker: ended before sending -> nothing sent");
}

void TestDetectorNamesThrow() {
    boom::Zones zones;
    boom::Detector d(zones);
    boom::Throw th;
    th.t = 10.0;
    th.weapon = "exp_grenade";
    th.pos = P(0, 0, 0);
    th.seq = 4242;
    d.OnThrow(th);
    boom::Sound s;
    s.t = 13.0;
    s.type = boom::kSoundExplosion;
    s.pos = P(15, 5, 0);
    d.OnSound(s);
    uint32_t seq = 1;
    std::vector<boom::Seen> seen;
    d.Tick(13.5, seq, seen);
    Check(seen.size() == 1 && seen[0].send && seen[0].e.throwSeq == 4242 && std::fabs(seen[0].sinceThrow - 3.0) < 1e-6,
          "Detector: the explosion of our throw names its throw number and fuse");
    // someone else's explosion names none
    boom::Sound o = s;
    o.t = 20.0;
    o.owner = boom::Owner::Other;
    o.pos = P(500, 0, 0);
    d.OnSound(o);
    seen.clear();
    d.Tick(20.5, seq, seen);
    Check(seen.size() == 1 && !seen[0].send && seen[0].e.throwSeq == 0, "Detector: not ours -> no throw number");
}

// the whole thing: our release -> Throw + points over the wire -> the other game's Book -> our Explosion ends it there
void TestExchange() {
    Tracker tr;
    Book book;
    std::vector<Out> out;
    std::vector<Summary> done;
    const Vec3 o = P(-500, 300, 12), v = P(9, 3, 7);
    tr.Begin(31, Kind::Grenade, "exp_grenade", 0.0, P(-500, 300, 10.4f));
    double latency = 0.08;
    auto deliver = [&](double sendAt) {
        for (const Out& x : out) {
            if (x.type == Out::Type::Throw) {
                ThrowMsg m;
                const auto b = PackThrow(x.t);
                mc::net::ByteReader r(b.data(), b.size());
                if (ReadThrow(r, m)) book.Add(1, m, sendAt + latency);
            } else {
                FlightMsg m;
                const auto b = PackFlight(x.f);
                mc::net::ByteReader r(b.data(), b.size());
                if (ReadFlight(r, m)) book.AddPoints(1, m, sendAt + latency);
            }
        }
        out.clear();
    };
    double maxErr = 0;
    for (int i = 0; i <= 90; ++i) {  // 1.5 s at 60 Hz
        const double t = 0.05 + i / 60.0;
        Vec3 truth = Ballistic(o, v, kGravity, t - 0.05);
        tr.Sample(31, t, truth, out);
        deliver(t);
        // the receiver shows it kPlaybackDelay behind its own receive time: compare with the truth then
        std::vector<Visual> vis;
        const double rnow = t + latency;
        book.Visuals(rnow, vis);
        if (!vis.empty() && i > 2) {
            const double showAge = rnow - book.Find(1, 31)->t0 - kPlaybackDelay;
            const Vec3 want = Ballistic(o, v, kGravity, std::max(0.0, showAge));
            maxErr = std::max(maxErr, static_cast<double>(boom::Dist(vis[0].pos, want)));
        }
    }
    Check(book.Find(1, 31) != nullptr, "exchange: the other game has the flight");
    Check(maxErr < 0.25, "exchange: shown within 0.25 m of the true flight (at the playback delay)", std::to_string(maxErr));
    // the explosion: the thrower decides it (its detector names throw 31), the receiver ends that flight
    const Vec3 at = Ballistic(o, v, kGravity, 1.6);
    tr.End(31, 1.7, &at, "explosion", out, done);
    deliver(1.7);
    boom::Explosion e;
    e.seq = 5;
    e.kind = Kind::Grenade;
    e.pos = at;
    e.throwSeq = 31;
    const auto eb = boom::PackExplosion(e);
    boom::Explosion got;
    mc::net::ByteReader r(eb.data(), eb.size());
    Ended end;
    const bool read = boom::ReadExplosion(r, got);
    Check(read && book.EndByExplosion(1, got.throwSeq, got.pos, 1.7 + latency + 0.2, end) && end.seq == 31 &&
              book.Count() == 0,
          "exchange: the Explosion naming throw 31 ends its flight there");
    Check(boom::Dist(end.pos, at) < 1.0f, "exchange: last shown next to the explosion", std::to_string(boom::Dist(end.pos, at)));
}

void TestAimLog() {
    AimLog a;
    AimSample s;
    int taken = 0;
    for (int i = 0; i < 60; ++i) {  // 1 s at 60 Hz
        s.t = 5.0 + i / 60.0;
        if (a.Take(s, 0.2)) ++taken;
    }
    Check(a.On() && taken == 5, "AimLog: 5 records per second at a 0.2 s interval", std::to_string(taken));
    double lasted = 0;
    size_t n = 0;
    Check(a.Stop(6.0, lasted, n) && n == 60 && std::fabs(lasted - 1.0) < 1e-9 && !a.On(), "AimLog: stop gives the summary");
    Check(!a.Stop(6.1, lasted, n), "AimLog: stop twice -> nothing");
    s.t = 7.0;
    Check(a.Take(s, 0.2) && a.Since() == 7.0, "AimLog: a new aim starts with a record at once");
}

}  // namespace

int main() {
    TestWire();
    TestMath();
    TestPlayback();
    TestBook();
    TestTracker();
    TestDetectorNamesThrow();
    TestExchange();
    TestAimLog();
    if (g_failures) {
        std::printf("\n%d check(s) FAILED\n", g_failures);
        return 1;
    }
    std::printf("\nall checks passed\n");
    return 0;
}
