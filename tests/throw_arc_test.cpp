// throw_arc_test: unit tests for the flight of thrown explosives on every game (src/game/throw_model.h): the Throw
// wire format (round trip, malformed messages refused) and the Explosion's throw number (old and new layout), the arc
// math (ballistic positions, the launch from two samples, the gravity from three, the aim model), the receivers'
// simulation in a made-up world (a floor and a wall: a bounce off the wall, bounces on the floor, rolling, settling,
// a molotov stopping at its first hit, the thrower's own body ignored, no ray cast available, one cast per step), the
// Book (duplicates, the explosion ending the right flight by seq or by distance and snapping to its true position,
// the fuse, timeouts, a player leaving), the thrower's Tracker (launch measured from three samples, from two when the
// window ends, the aim model when no projectile is read, nothing at all) and the explosion detector naming the throw;
// and a whole exchange walked through from release to explosion.
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
    std::printf("%s  %-76s %s\n", ok ? "PASS" : "FAIL", name, detail.c_str());
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

// ---- a made-up world: a floor at z = 0 and (optionally) a wall at x = wallX ----------------------------------------
struct World {
    float wallX = 1e9f;
    size_t casts = 0;
    int operator()(const Vec3& a, const Vec3& b, Vec3& hit) {
        ++casts;
        float best = 2.0f;
        if (a.z >= 0 && b.z < 0) best = std::min(best, a.z / (a.z - b.z));
        if (a.x < wallX && b.x >= wallX) best = std::min(best, (wallX - a.x) / (b.x - a.x));
        if (best > 1.0f) return 0;
        hit = {a.x + (b.x - a.x) * best, a.y + (b.y - a.y) * best, a.z + (b.z - a.z) * best};
        return 1;
    }
};

ThrowMsg SampleThrow() {
    ThrowMsg t;
    t.seq = 0x00ABCDEF;
    t.kind = Kind::Molotov;
    t.flags = kFire | kFromEntity;
    t.origin = P(-943.5f, -233.25f, 5.5f);
    t.vel = P(8.0f, 9.0f, 6.5f);
    t.gravity = 9.81f;
    t.delayMs = 40;
    t.fuseMs = 3500;
    t.weapon = "exp_molotov";
    return t;
}

bool ReadBack(const std::vector<uint8_t>& buf, ThrowMsg& out) {
    mc::net::ByteReader r(buf.data(), buf.size());
    return ReadThrow(r, out);
}

void TestWire() {
    const ThrowMsg t = SampleThrow();
    const auto buf = PackThrow(t);
    ThrowMsg back;
    Check(ReadBack(buf, back) && back.seq == t.seq && back.kind == t.kind && back.flags == t.flags &&
              Near(back.origin, t.origin, 1e-4f) && Near(back.vel, t.vel, 1e-4f) && back.gravity == t.gravity &&
              back.delayMs == 40 && back.fuseMs == 3500 && back.weapon == "exp_molotov",
          "Throw round trip");
    Check(buf.size() <= 50, "Throw is small (one message per throw)", std::to_string(buf.size()) + " bytes");
    auto bad = [&](const char* name, auto edit) {
        ThrowMsg m = SampleThrow();
        edit(m);
        ThrowMsg o;
        Check(!ReadBack(PackThrow(m), o), name);
    };
    bad("Throw: kind out of range refused", [](ThrowMsg& m) { m.kind = static_cast<Kind>(9); });
    bad("Throw: 200 m/s refused", [](ThrowMsg& m) { m.vel = P(200, 0, 0); });
    bad("Throw: gravity 0 refused", [](ThrowMsg& m) { m.gravity = 0; });
    bad("Throw: NaN origin refused", [](ThrowMsg& m) { m.origin.x = std::numeric_limits<float>::quiet_NaN(); });
    {
        ThrowMsg o;
        Check(!ReadBack(std::vector<uint8_t>(buf.begin(), buf.begin() + 20), o), "Throw: truncated refused");
        ThrowMsg m = SampleThrow();
        m.flags = 0xFF;
        m.weapon = "bad name!";
        Check(ReadBack(PackThrow(m), o) && o.flags == (kFire | kFromEntity) && o.weapon.empty(),
              "Throw: unknown flags masked, unsafe name sent empty");
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
    {
        mc::net::ByteReader r(eBuf.data(), eBuf.size());
        Check(boom::ReadExplosion(r, eb) && eb.throwSeq == 0x00123456 && eb.seq == 9, "Explosion carries the throw number");
    }
    {
        std::vector<uint8_t> old(eBuf.begin(), eBuf.end() - 4);  // what a sender before the grenade arc sends
        mc::net::ByteReader r(old.data(), old.size());
        Check(boom::ReadExplosion(r, eb) && eb.throwSeq == 0 && eb.weapon == "exp_grenade",
              "Explosion without the throw number (older sender) still read, throw 0");
    }
}

void TestMath() {
    const Vec3 o = P(10, 20, 5), v = P(3, 4, 6);
    Check(Near(Ballistic(o, v, 9.81f, 1.0), P(13, 24, 5 + 6 - 4.905f), 1e-4f), "Ballistic position after 1 s");
    const Vec3 est = EstimateLaunch(Ballistic(o, v, 9.81f, 0.0), 0.0, Ballistic(o, v, 9.81f, 0.033), 0.033, 9.81f);
    Check(Near(est, v, 1e-3f), "EstimateLaunch recovers the launch velocity", Str(est));
    float g = 0;
    Check(FitGravity(Ballistic(o, v, 9.81f, 0.0), 0.0, Ballistic(o, v, 9.81f, 0.1), 0.1, Ballistic(o, v, 9.81f, 0.2), 0.2,
                     g) &&
              std::fabs(g - 9.81f) < 0.05f,
          "FitGravity measures 9.81", std::to_string(g));
    Check(!FitGravity(P(0, 0, 1), 0.0, P(0, 0, 0), 0.1, P(0, 0, 2), 0.2, g), "FitGravity refuses a bounce in between");
    const Vec3 a = AimLaunch(P(0, 1, 0), 16.0f, 0.2617994f);
    float yaw = 0, pitch = 0;
    YawPitch(a, yaw, pitch);
    Check(std::fabs(boom::Dist(a, P(0, 0, 0)) - 16.0f) < 1e-3f && std::fabs(pitch - 15.0f) < 0.05f && std::fabs(yaw) < 0.05f,
          "AimLaunch: speed kept, 15 deg up, yaw kept", Str(a));
    YawPitch(AimLaunch(P(0, 0, 1), 10.0f, 0.5f), yaw, pitch);
    Check(pitch <= 85.01f && pitch > 80.0f, "AimLaunch: straight up clamped to 85 deg", std::to_string(pitch));
}

// runs a simulation at 30 ticks a second for `seconds`
void Run(Sim& s, World& w, double seconds, double tick = 1.0 / 30.0) {
    RayFn ray = [&w](const Vec3& a, const Vec3& b, Vec3& hit) { return w(a, b, hit); };
    for (double t = tick; t <= seconds + 1e-9; t += tick) s.Step(t, ray);
}

void TestSim() {
    // a lob onto the floor: bounces, rolls, settles on it, then no more casts
    {
        World w;
        Sim s;
        s.Start(P(0, 0, 1.6f), P(6, 0, 4), kGravity, false);
        Run(s, w, 8.0);
        Check(s.settled && s.bounces >= 1 && std::fabs(s.pos.z - kSkin) < 1e-3f && s.pos.x > 3.0f && s.pos.x < 20.0f,
              "floor: lands, bounces, rolls and settles on the floor", Str(s.pos) + " bounces " + std::to_string(s.bounces));
        const size_t casts = s.casts;
        Run(s, w, 10.0);
        Check(s.casts == casts, "floor: settled = no more ray casts");
        Check(casts <= 8 * 30 + 2 * static_cast<size_t>(s.bounces), "floor: at most one cast per tick (+ a probe per hit)",
              std::to_string(casts));
    }
    // straight at a wall: bounces back off it, lands in front of it, settles
    {
        World w;
        w.wallX = 5.0f;
        Sim s;
        s.Start(P(0, 0, 1.6f), P(12, 0, 3), kGravity, false);
        bool hitWall = false, through = false;
        RayFn ray = [&w](const Vec3& a, const Vec3& b, Vec3& hit) { return w(a, b, hit); };
        for (double t = 1.0 / 30; t <= 8.0; t += 1.0 / 30) {
            s.Step(t, ray);
            if (s.vel.x < 0) hitWall = true;
            if (s.pos.x >= 5.0f) through = true;
        }
        Check(!through, "wall: never passes through the wall");
        Check(hitWall && s.bounces >= 2, "wall: bounced back off it (then the floor)", std::to_string(s.bounces));
        Check(s.settled && s.pos.x < 5.0f && s.pos.x > 0.0f && std::fabs(s.pos.z - kSkin) < 1e-3f,
              "wall: settles on the floor in front of the wall", Str(s.pos));
    }
    // a bounce keeps restitution x the speed into the surface
    {
        World w;
        Sim s;
        s.Start(P(0, 0, 1.0f), P(0, 0, -10), kGravity, false);
        RayFn ray = [&w](const Vec3& a, const Vec3& b, Vec3& hit) { return w(a, b, hit); };
        s.Step(0.1, ray);
        const float impact = std::sqrt(100.0f + 2 * kGravity * 1.0f);  // speed after falling 1 m
        Check(s.bounces == 1 && s.vel.z > 0 && std::fabs(s.vel.z - kRestitution * impact) < 0.3f && !s.rolling,
              "a hard drop bounces up with the restitution", std::to_string(s.vel.z));
    }
    // a molotov stops where it first hits
    {
        World w;
        Sim s;
        s.Start(P(0, 0, 1.6f), P(6, 0, 4), kGravity, true);
        Run(s, w, 4.0);
        Check(s.settled && s.bounces == 1 && std::fabs(s.pos.z - kSkin) < 1e-3f, "molotov: stops at its first hit", Str(s.pos));
    }
    // the thrower's own body right at the origin is not a hit
    {
        World w;
        Sim s;
        s.Start(P(0, 0, 1.6f), P(6, 0, 4), kGravity, false);
        bool first = true;
        RayFn ray = [&](const Vec3& a, const Vec3& b, Vec3& hit) {
            if (first) {
                first = false;
                hit = P(0.1f, 0, 1.6f);
                return 1;
            }
            return w(a, b, hit);
        };
        s.Step(1.0 / 30, ray);
        Check(s.bounces == 0 && s.pos.x > 0.15f, "a hit next to the origin in the first moment (the thrower) is ignored");
    }
    // no ray cast: the plain arc, held far below
    {
        Sim s;
        s.Start(P(0, 0, 1.6f), P(6, 0, 4), kGravity, false);
        RayFn none = [](const Vec3&, const Vec3&, Vec3&) { return -1; };
        for (double t = 1.0 / 30; t <= 10.0; t += 1.0 / 30) s.Step(t, none);
        Check(s.noRay && s.settled && s.casts == 1 && s.bounces == 0, "no ray cast available: the plain arc, one try");
        s.Start(P(0, 0, 1.6f), P(6, 0, 4), kGravity, false);
        for (double t = 1.0 / 30; t <= 0.5; t += 1.0 / 30) s.Step(t, none);
        Check(Near(s.pos, Ballistic(P(0, 0, 1.6f), P(6, 0, 4), kGravity, s.t), 1e-3f), "no ray cast: on the ballistic arc");
    }
    // a long tick catches up in steps of at most kMaxStep
    {
        World w;
        Sim s;
        s.Start(P(0, 0, 50), P(1, 0, 0), kGravity, false);
        RayFn ray = [&w](const Vec3& a, const Vec3& b, Vec3& hit) { return w(a, b, hit); };
        s.Step(1.0, ray);
        Check(std::fabs(s.t - kMaxStep) < 1e-9 && s.casts == 1, "a 1 s tick advances one step of 0.1 s (one cast)");
    }
}

void TestBook() {
    World w;
    RayFn ray = [&w](const Vec3& a, const Vec3& b, Vec3& hit) { return w(a, b, hit); };
    Book b;
    ThrowMsg t = SampleThrow();
    t.kind = Kind::Grenade;
    t.flags = kFromEntity;
    t.fuseMs = 0;
    t.seq = 10;
    t.delayMs = 100;
    t.origin = P(0, 0, 1.6f);
    t.vel = P(6, 0, 4);
    Check(b.Add(2, t, 50.0) && b.Count() == 1, "Book: a throw added");
    Check(!b.Add(2, t, 50.1), "Book: the same throw again (relayed twice) ignored");
    Check(b.Find(2, 10) && std::fabs(b.Find(2, 10)->t0 - 49.9) < 1e-9, "Book: origin time = receive time - delay");
    Check(b.Add(3, t, 50.0) && b.Count() == 2, "Book: the same seq from another player is another flight");
    for (double now = 50.0; now <= 51.0; now += 1.0 / 30) b.Step(now, ray);
    std::vector<Visual> vis;
    b.Visuals(51.0, vis);
    Check(vis.size() == 2 && std::string(vis[0].src) != "boom", "Book: each flight simulated and shown");
    // the explosion that names its throw ends exactly that one, the marker jumps to the true position
    Ended e;
    const Vec3 boomAt = P(7.5f, 0.3f, 0.2f);
    Check(b.EndByExplosion(2, 10, boomAt, 51.0, e) && e.seq == 10 && e.from == 2 && e.why == EndWhy::Explosion &&
              std::fabs(e.off - boom::Dist(e.pos, boomAt)) < 1e-4f,
          "Book: explosion with throw seq ends that flight (offset logged)", std::to_string(e.off));
    vis.clear();
    b.Visuals(51.1, vis);
    bool snapped = false;
    for (const Visual& v : vis)
        if (v.from == 2 && v.seq == 10) snapped = Near(v.pos, boomAt, 1e-5f) && std::string(v.src) == "boom" && v.held;
    Check(snapped, "Book: the marker shows at the explosion's true position");
    Check(!b.EndByExplosion(2, 10, boomAt, 51.1, e), "Book: a second explosion for it finds nothing");
    std::vector<Ended> out;
    b.Tick(51.0 + kExplodedShow + 0.01, out);
    Check(out.empty() && !b.Find(2, 10) && b.Count() == 1, "Book: removed after a moment, silently (reported already)");
    Check(!b.EndByExplosion(2, 999, boomAt, 52.0, e) && b.Count() == 1, "Book: an unknown throw seq ends nothing");

    // without a seq: the sender's nearest flight within reach
    Book b2;
    ThrowMsg a = t, c = t;
    a.seq = 1, a.delayMs = 0, a.origin = P(0, 0, 1.6f);
    c.seq = 2, c.delayMs = 0, c.origin = P(100, 0, 1.6f);
    b2.Add(5, a, 0.0);
    b2.Add(5, c, 0.0);
    b2.Add(6, a, 0.0);
    Check(b2.EndByExplosion(5, 0, P(98, 1, 0), 0.5, e) && e.seq == 2 && e.from == 5, "Book: no seq -> the sender's nearest flight");
    Check(!b2.EndByExplosion(5, 0, P(500, 0, 0), 0.5, e), "Book: no seq and nothing within 30 m -> none");
    Check(b2.EndByExplosion(5, 0, P(1, 0, 0), 0.5, e) && e.seq == 1 && b2.Find(6, 1), "Book: another sender's flight is not taken");

    // the fuse, the timeout, a player leaving, capacity
    Book b3;
    ThrowMsg f = t;
    f.seq = 1, f.delayMs = 0, f.fuseMs = 2000;
    b3.Add(2, f, 0.0);
    f.seq = 2, f.fuseMs = 0;
    b3.Add(2, f, 0.0);
    out.clear();
    b3.Tick(2.9, out);
    Check(out.empty(), "Book: inside fuse + grace: kept");
    b3.Tick(3.1, out);
    Check(out.size() == 1 && out[0].seq == 1 && out[0].why == EndWhy::Fuse, "Book: fuse + 1 s without an explosion -> gone");
    out.clear();
    b3.Tick(kMaxFlight + 0.1, out);
    Check(out.size() == 1 && out[0].seq == 2 && out[0].why == EndWhy::Timeout && b3.Count() == 0, "Book: no fuse: 15 s -> timeout");
    Book b4;
    b4.Add(2, f, 0.0);
    f.seq = 3;
    b4.Add(3, f, 0.0);
    out.clear();
    b4.Forget(2, 1.0, out);
    Check(out.size() == 1 && out[0].why == EndWhy::Left && b4.Count() == 1, "Book: a leaving player's flights go");
    Book b5;
    for (uint32_t i = 1; i <= kMaxFlights + 4; ++i) {
        ThrowMsg m = t;
        m.seq = i;
        m.delayMs = 0;
        b5.Add(2, m, static_cast<double>(i));
    }
    Check(b5.Count() == kMaxFlights && !b5.Find(2, 1) && b5.Find(2, kMaxFlights + 4), "Book: full -> the oldest goes");
}

void TestTracker() {
    Tracker tr;
    std::vector<Out> out;
    std::vector<Summary> done;
    const Vec3 o = P(100, 200, 31.6f), v = P(5, 10, 6);
    // three samples within the window: measured, sent at the third, then the throw is let go
    tr.Begin(7, Kind::Grenade, "exp_grenade", 10.0, P(100, 199.5f, 30));
    tr.Model(7, P(100, 200.2f, 31.7f), P(4, 12, 5));
    tr.Sample(7, 10.05, o, out, done);
    tr.Sample(7, 10.083, Ballistic(o, v, kGravity, 0.033), out, done);
    Check(out.empty() && tr.Active(7), "Tracker: two samples, still measuring");
    tr.Sample(7, 10.15, Ballistic(o, v, kGravity, 0.10), out, done);
    Check(out.size() == 1 && !tr.Active(7), "Tracker: the third sample sends the Throw and ends the measure");
    if (!out.empty()) {
        const ThrowMsg& m = out[0].t;
        Check(m.seq == 7 && m.kind == Kind::Grenade && (m.flags & kFromEntity) && Near(m.origin, o, 1e-4f) &&
                  Near(m.vel, v, 2e-3f) && m.delayMs == 100 && m.weapon == "exp_grenade",
              "Tracker: origin = first sample, launch measured, delay since it", Str(m.vel));
    }
    Check(done.size() == 1 && std::string(done[0].why) == "measured" && done[0].fromEntity &&
              std::fabs(done[0].gravity - kGravity) < 0.1f && done[0].modelSpeed > 13.0f && done[0].samples == 3,
          "Tracker: summary (gravity measured, model vs real)", std::to_string(done[0].gravity));
    out.clear();
    tr.Sample(7, 10.2, o, out, done);
    Check(out.empty(), "Tracker: later samples of a sent throw are ignored");

    // two samples when the window ends: measured from those
    done.clear();
    tr.Begin(8, Kind::Grenade, "exp_grenade", 20.0, P(0, 0, 0));
    tr.Sample(8, 20.1, P(0, 0, 1.6f), out, done);
    tr.Sample(8, 20.15, Ballistic(P(0, 0, 1.6f), P(0, 10, 5), kGravity, 0.05), out, done);
    tr.Tick(20.29, out, done);
    Check(out.empty(), "Tracker: before the window ends nothing is sent");
    tr.Tick(20.0 + kMeasureWindow, out, done);
    Check(out.size() == 1 && (out[0].t.flags & kFromEntity) && Near(out[0].t.vel, P(0, 10, 5), 2e-3f) &&
              std::string(done.back().why) == "measured" && done.back().gravity == 0,
          "Tracker: window over with two samples -> measured (no gravity)");

    // no projectile read: the aim model, a molotov's fire flag, delay = since the release
    out.clear();
    tr.Begin(9, Kind::Molotov, "exp_molotov", 30.0, P(0, 0, 0));
    tr.Model(9, P(0, 0, 1.6f), P(0, 12, 4));
    tr.Sample(9, 30.1, P(0, 0, 1.6f), out, done);  // one sample is not a launch
    tr.Tick(30.0 + kMeasureWindow + 0.01, out, done);
    Check(out.size() == 1 && !(out[0].t.flags & kFromEntity) && (out[0].t.flags & kFire) &&
              Near(out[0].t.origin, P(0, 0, 1.6f), 1e-5f) && Near(out[0].t.vel, P(0, 12, 4), 1e-5f) &&
              out[0].t.delayMs == 310 && std::string(done.back().why) == "model",
          "Tracker: measured-launch fallback: the aim model sent");

    // nothing at all: nothing sent
    out.clear();
    tr.Begin(10, Kind::Unknown, "", 40.0, P(0, 0, 0));
    tr.Tick(40.4, out, done);
    Check(out.empty() && !done.back().sent && std::string(done.back().why) == "no_projectile_no_model" && !tr.Any(),
          "Tracker: no projectile, no model -> nothing sent");

    // a jump in the first readings (the entity put into the hand first): re-anchored
    tr.Begin(11, Kind::Grenade, "exp_grenade", 50.0, P(0, 0, 0));
    tr.Sample(11, 50.05, P(0, 0, -50), out, done);
    tr.Sample(11, 50.07, P(0, 0.2f, 1.6f), out, done);
    tr.Sample(11, 50.09, P(0, 0.4f, 1.7f), out, done);
    Check(out.empty(), "Tracker: an impossible launch speed restarts the measure");
    tr.Sample(11, 50.11, P(0, 0.6f, 1.78f), out, done);
    Check(out.size() == 1 && Near(out[0].t.origin, P(0, 0.2f, 1.6f), 1e-5f), "Tracker: re-anchored on the next samples");

    // our explosion inside the window (an impact): sent from what there is
    out.clear();
    tr.Begin(12, Kind::Molotov, "exp_molotov", 60.0, P(0, 0, 0));
    tr.Sample(12, 60.05, P(0, 0, 1.6f), out, done);
    tr.Sample(12, 60.10, P(0, 0.5f, 1.65f), out, done);
    tr.End(12, 60.12, out, done);
    Check(out.size() == 1 && !tr.Active(12), "Tracker: an explosion before the window ends sends at once");
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
    boom::Sound o = s;
    o.t = 20.0;
    o.owner = boom::Owner::Other;
    o.pos = P(500, 0, 0);
    d.OnSound(o);
    seen.clear();
    d.Tick(20.5, seq, seen);
    Check(seen.size() == 1 && !seen[0].send && seen[0].e.throwSeq == 0, "Detector: not ours -> no throw number");
}

// the whole thing: release -> 3 samples -> one Throw over the wire -> the other game simulates it in its world ->
// our Explosion (naming the throw) ends it there at the true position
void TestExchange() {
    World thrower, receiver;
    thrower.wallX = receiver.wallX = 9.0f;
    Tracker tr;
    Book book;
    std::vector<Out> out;
    std::vector<Summary> done;
    const Vec3 o = P(0, 0, 1.6f), v = P(7, 1, 4);
    tr.Begin(31, Kind::Grenade, "exp_grenade", 0.0, P(0, 0, 0));
    // the "real" projectile, simulated on the thrower's game the same way (stands in for the game's physics)
    Sim real;
    real.Start(o, v, kGravity, false);
    RayFn tRay = [&thrower](const Vec3& a, const Vec3& b, Vec3& hit) { return thrower(a, b, hit); };
    const double latency = 0.08;
    size_t bytes = 0, msgs = 0;
    for (int i = 1; i <= 3; ++i) {
        const double t = 0.05 + i / 30.0;
        if (i == 1) tr.Sample(31, 0.05, o, out, done);
        real.Step(t - 0.05, tRay);
        tr.Sample(31, t, real.pos, out, done);
    }
    for (const Out& x : out) {
        const auto b = PackThrow(x.t);
        bytes += b.size();
        ++msgs;
        ThrowMsg m;
        mc::net::ByteReader r(b.data(), b.size());
        if (ReadThrow(r, m)) book.Add(1, m, 0.05 + 3 / 30.0 + latency);
    }
    Check(msgs == 1, "exchange: one message for the whole throw", std::to_string(bytes) + " bytes");
    RayFn rRay = [&receiver](const Vec3& a, const Vec3& b, Vec3& hit) { return receiver(a, b, hit); };
    for (double t = 0.05 + 3 / 30.0; t < 3.0; t += 1.0 / 30) {
        real.Step(t - 0.05, tRay);
        book.Step(t + latency, rRay);
    }
    const Flight* f = book.Find(1, 31);
    Check(f != nullptr, "exchange: the other game simulates the flight");
    if (f) {
        Check(f->sim.bounces >= 2, "exchange: it bounced off the wall and the floor there too", std::to_string(f->sim.bounces));
        Check(boom::Dist(f->sim.pos, real.pos) < 1.0f, "exchange: it ends up within 1 m of the real one",
              Str(f->sim.pos) + " vs " + Str(real.pos));
        Check(f->sim.casts < 3.0 * 30 * 2, "exchange: ray casts bounded", std::to_string(f->sim.casts));
    }
    boom::Explosion e;
    e.seq = 5;
    e.kind = Kind::Grenade;
    e.pos = real.pos;
    e.throwSeq = 31;
    const auto eb = boom::PackExplosion(e);
    boom::Explosion got;
    mc::net::ByteReader r(eb.data(), eb.size());
    Ended end;
    Check(boom::ReadExplosion(r, got) && book.EndByExplosion(1, got.throwSeq, got.pos, 3.0 + latency, end) && end.seq == 31,
          "exchange: the Explosion naming throw 31 ends its flight there");
    std::vector<Visual> vis;
    book.Visuals(3.1, vis);
    Check(vis.size() == 1 && Near(vis[0].pos, real.pos, 1e-5f), "exchange: the marker snaps to the true position");
}

void TestAimLog() {
    AimLog a;
    AimSample s;
    int taken = 0;
    for (int i = 0; i < 60; ++i) {
        s.t = 5.0 + i / 60.0;
        if (a.Take(s, 0.2)) ++taken;
    }
    Check(a.On() && taken == 5, "AimLog: 5 records per second at a 0.2 s interval", std::to_string(taken));
    double lasted = 0;
    size_t n = 0;
    Check(a.Stop(6.0, lasted, n) && n == 60 && std::fabs(lasted - 1.0) < 1e-9 && !a.On(), "AimLog: stop gives the summary");
    Check(!a.Stop(6.1, lasted, n), "AimLog: stop twice -> nothing");
}

}  // namespace

int main() {
    TestWire();
    TestMath();
    TestSim();
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
