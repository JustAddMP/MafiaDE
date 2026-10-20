// player_motion_test: unit tests for src/game/player_motion.h (docs/sync-architecture.md section 4 targets "foot.*" and
// "car.*"). Recorded paths from the 0.15.0 two-PC traces (tests/data: the host on foot and driving, 10 Hz) are replayed
// as a 60 Hz sender (positions on a curve through the rows, velocity smoothed like the sender's own, his reported
// mode), sent over a simulated link (14 ms + jitter), received into PlayerTrack (PlayoutClock + replica Stream), and
// drive:
//   - on foot: FootDriver against a simulated AI walker as the game's: its own walk / run / sprint speeds (1.3 / 2.2 /
//     3.3 m/s at multiplier 1, measured in the traces) off by +-20 %, acting on commands 150-250 ms late, turning at
//     most 360 deg/s, accelerating 10 m/s^2, braking to arrive at its walk point. Error = puppet vs the player at the
//     same moment (sync_compare's "foot" numbers): p95, mean, p95 while his state says idle, gait match (commanded
//     gait = the mode the puppet is driven from), snaps per minute.
//   - his car: CarDriver against a simulated copy whose own physics reaches only 85 % of his speed (or 115 %) 300 ms
//     late, accelerates 6 m/s^2 and brakes 9 m/s^2, steering with his yaw rate 300 ms late; SetPos / SetRot move it,
//     SetSpeed gives it a speed along its heading. p95 copy vs car, pinned share, copy / driver speed (median).
// Plus the PlayerTrack basics (two cars are never blended, the lead) and the rest blend. Prints PASS/FAIL per check;
// exits non-zero on any failure. Built by build.cmd (out\player_motion_test.exe), run by tools\make_release.cmd.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <string>
#include <vector>

#include "data/car_path.h"
#include "data/foot_2pc.h"
#include "data/foot_mixed.h"
#include "data/foot_sprint.h"
#include "game/player_motion.h"

using namespace mc::game::pmotion;
using mc::net::motion::Gait;
using mc::net::motion::LocomotionCommand;
using mc::net::motion::LocomotionInput;
using mc::net::motion::VehicleCommand;
using mc::net::motion::VehicleInput;

namespace {

int g_failures = 0;

void Check(bool ok, const char* name, const std::string& detail = {}) {
    std::printf("%s  %-74s %s\n", ok ? "PASS" : "FAIL", name, detail.c_str());
    std::fflush(stdout);
    if (!ok) ++g_failures;
}

std::string Fmt(const char* f, double a, double b = 0, double c = 0, double d = 0) {
    char buf[200];
    std::snprintf(buf, sizeof buf, f, a, b, c, d);
    return buf;
}

constexpr float kPi = 3.14159265f;
constexpr double kDt = 1.0 / 60;

double Pct(std::vector<float> v, double p) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    const double k = (v.size() - 1) * p / 100;
    const size_t lo = static_cast<size_t>(std::floor(k)), hi = static_cast<size_t>(std::ceil(k));
    return v[lo] + (v[hi] - v[lo]) * (k - lo);
}

double Mean(const std::vector<float>& v) {
    double s = 0;
    for (float x : v) s += x;
    return v.empty() ? 0 : s / v.size();
}

// deterministic pseudo random 0..1
struct Rng {
    uint32_t s = 12345;
    float Next() {
        s = s * 1664525u + 1013904223u;
        return static_cast<float>((s >> 8) & 0xFFFFFF) / static_cast<float>(0x1000000);
    }
};

// ---- a recorded path: 10 Hz rows -> positions at any time (cubic Hermite through the rows with their velocities) ----
struct Path {
    std::vector<float> x, y, vx, vy, extra;  // extra: mode (foot) or yaw degrees (car)
    bool rowVel = false;  // the curve's tangents are the rows' velocities (a car's: its physics' own)
    double Seconds() const { return (x.size() - 1) * 0.1; }
    template <size_t N>
    static Path From(const int (&rows)[N][5], bool yaw) {
        Path p;
        float cx = 0, cy = 0;
        for (size_t i = 0; i < N; ++i) {
            cx += rows[i][0] / 100.0f;
            cy += rows[i][1] / 100.0f;
            p.x.push_back(cx);
            p.y.push_back(cy);
            p.vx.push_back(rows[i][2] / 100.0f);
            p.vy.push_back(rows[i][3] / 100.0f);
            p.extra.push_back(yaw ? rows[i][4] / 100.0f : static_cast<float>(rows[i][4]));
        }
        p.rowVel = yaw;
        return p;
    }
    // position at t (s from the start)
    void At(double t, float& px, float& py) const {
        t = std::clamp(t, 0.0, Seconds());
        size_t i = std::min(static_cast<size_t>(t / 0.1), x.size() - 2);
        const float u = static_cast<float>((t - i * 0.1) / 0.1);
        // on foot the rows' velocities are the sender's smoothed ones (they lag): the curve's tangents come from the
        // positions; a car's are its physics' own
        auto tan = [&](size_t k, const std::vector<float>& v) {
            if (rowVel) return &v == &x ? vx[k] : vy[k];
            const size_t a = k == 0 ? 0 : k - 1, b = std::min(k + 1, v.size() - 1);
            return (v[b] - v[a]) / (static_cast<float>(b - a) * 0.1f);
        };
        auto herm = [&](const std::vector<float>& v) {
            const float p0 = v[i], p1 = v[i + 1], m0 = tan(i, v) * 0.1f, m1 = tan(i + 1, v) * 0.1f;
            const float u2 = u * u, u3 = u2 * u;
            return (2 * u3 - 3 * u2 + 1) * p0 + (u3 - 2 * u2 + u) * m0 + (-2 * u3 + 3 * u2) * p1 + (u3 - u2) * m1;
        };
        px = herm(x);
        py = herm(y);
    }
    float Extra(double t) const {  // the row at or before t (the sender's mode changes at a sample)
        const size_t i = std::min(static_cast<size_t>(std::max(0.0, t) / 0.1), extra.size() - 1);
        return extra[i];
    }
    float YawAt(double t) const {  // degrees, interpolated the short way
        t = std::clamp(t, 0.0, Seconds());
        size_t i = std::min(static_cast<size_t>(t / 0.1), x.size() - 2);
        const float u = static_cast<float>((t - i * 0.1) / 0.1);
        float d = extra[i + 1] - extra[i];
        while (d > 180) d -= 360;
        while (d < -180) d += 360;
        return extra[i] + d * u;
    }
};

// ---- the link: a 60 Hz sender, 14 ms + jitter, delivered at the receiver's frames (in order) ----
struct Packet {
    double sampleT, arrive;
    float px, py, vx, vy;
    int mode;
    float yaw;
};

std::vector<Packet> SendPath(const Path& path, bool car, Rng& rng) {
    std::vector<Packet> out;
    float svx = 0, svy = 0, lx = 0, ly = 0;
    bool have = false;
    double lastArrive = 0;
    for (double t = 0; t <= path.Seconds(); t += kDt) {
        Packet p{};
        p.sampleT = t;
        path.At(t, p.px, p.py);
        if (car) {
            // the car's velocity is its physics' own (exact): the curve's derivative
            float ax, ay;
            path.At(t + 0.005, ax, ay);
            p.vx = (ax - p.px) / 0.005f;
            p.vy = (ay - p.py) / 0.005f;
            p.yaw = path.YawAt(t);
        } else {
            // the sender's own velocity: smoothed per sample (player_sync.cpp UpdateLocalMotion, 0.3 per sample)
            if (have) {
                svx += ((p.px - lx) / static_cast<float>(kDt) - svx) * 0.3f;
                svy += ((p.py - ly) / static_cast<float>(kDt) - svy) * 0.3f;
            }
            lx = p.px;
            ly = p.py;
            have = true;
            p.vx = svx;
            p.vy = svy;
            p.mode = static_cast<int>(path.Extra(t));
        }
        // one way: 14 ms + up to 12 ms jitter, 5 % of the states another 20-40 ms late (frame hitches)
        double a = t + 0.014 + 0.012 * rng.Next();
        if (rng.Next() < 0.05) a += 0.02 + 0.02 * rng.Next();
        p.arrive = std::max(a, lastArrive);
        lastArrive = p.arrive;
        out.push_back(p);
    }
    return out;
}

// ---- the game's AI walker (the puppet) ----
struct AiWalker {
    float base[4] = {0, 1.3f, 2.2f, 3.3f};  // m/s at multiplier 1 (the game's AI, 0.15.0 traces)
    float err[4] = {1, 1, 1, 1};            // its real speeds off by this factor
    float lag = 0.2f;
    float x = 0, y = 0, hx = 1, hy = 0, speed = 0;
    Gait gait = Gait::Stand;
    float mult = 1, tx = 0, ty = 0;
    bool walking = false;
    struct Pending {
        double at;
        Gait gait;
        float mult;
        float tx, ty;
    };
    std::deque<Pending> q;
    double clock = 0;
    // How the game's AI answers our position fixes. The 2026-10-20 two-PC traces: the puppet's own walk (the
    // controller's measurement, every frame's nudge taken out) was 0.6-2.2 m/s at RUN x1.8 while it was nudged on
    // nearly every frame, where 0.15.0 (nudged only beyond 0.9 m) ran ~4 m/s there. Which of these the game really
    // does is not known, so the controller is checked against all three:
    //   0 as commanded: a SetPos does not touch its walk
    //   1 hampered: within 50 ms of a SetPos its walk reaches only `hamper` of its speed
    //   2 erratic: its walk reaches a share of its speed that wanders between 0.3 and 1 (0.4 s time constant)
    int model = 0;
    float hamper = 0.3f;
    double movedAt = -1;
    float eff = 1;
    uint32_t rs = 99;
    double walkMetres = 0, fixMetres = 0;  // what its own walk and our fixes moved it
    static constexpr float kTurn = 360.0f * kPi / 180, kAccel = 10.0f;
    static constexpr float kArrive = 0.15f;  // m: a walk command is done this close to its point (then it stands)

    void Apply(const LocomotionCommand& c, float targetX, float targetY) {
        if (c.fix == LocomotionCommand::Fix::Snap) {
            x = targetX;
            y = targetY;
            speed = 0;
            movedAt = clock;
        } else if (c.fix == LocomotionCommand::Fix::Nudge) {
            x += c.nudge.x;
            y += c.nudge.y;
            fixMetres += std::sqrt(c.nudge.x * c.nudge.x + c.nudge.y * c.nudge.y);
            movedAt = clock;
        }
        if (c.reissue) q.push_back({clock + lag, c.gait, c.speedMult, c.moveTo.x, c.moveTo.y});
        if (c.turn && !walking) {
            hx = c.face[0];
            hy = c.face[1];
        }
    }
    void Step(float dt) {
        clock += dt;
        while (!q.empty() && q.front().at <= clock) {
            const Pending& p = q.front();
            // the glue: a stop is a walk to the point the C++ side gave (where the puppet comes to rest), at x1
            gait = p.gait == Gait::Stand ? Gait::Walk : p.gait;
            mult = p.gait == Gait::Stand ? 1.0f : p.mult;
            walking = true;
            tx = p.tx;
            ty = p.ty;
            q.pop_front();
        }
        float want = 0;
        const float dx = tx - x, dy = ty - y;
        const float d = std::sqrt(dx * dx + dy * dy);
        if (walking && d > kArrive) {
            const int g = static_cast<int>(gait);
            want = base[g] * err[g] * mult;
            const float a = std::atan2(hx * dy - hy * dx, hx * dx + hy * dy);
            const float r = std::clamp(a, -kTurn * dt, kTurn * dt);
            const float cr = std::cos(r), sr = std::sin(r);
            const float nx = hx * cr - hy * sr, ny = hx * sr + hy * cr;
            hx = nx;
            hy = ny;
            want *= std::max(0.3f, std::cos(a));         // slows down while turning hard
            want = std::min(want, std::sqrt(2 * kAccel * d));  // arrives at its walk point
            if (model == 1 && movedAt >= 0 && clock - movedAt < 0.05) want *= hamper;
            if (model == 2) {
                rs = rs * 1664525u + 1013904223u;
                const float n = static_cast<float>((rs >> 8) & 0xFFFFFF) / static_cast<float>(0x1000000) - 0.5f;
                eff = std::clamp(eff + (0.65f - eff) * dt / 0.4f + n * 2.0f * std::sqrt(dt), 0.3f, 1.0f);
                want *= eff;
            }
        } else if (walking) {
            walking = false;
        }
        speed += std::clamp(want - speed, -kAccel * dt, kAccel * dt);
        walkMetres += std::fabs(speed) * dt;
        if (walkHz <= 0) {
            x += hx * speed * dt;
            y += hy * speed * dt;
            return;
        }
        // the game moves its humans at its own rate, not every frame: the walk shows in steps
        pendX += hx * speed * dt;
        pendY += hy * speed * dt;
        if (clock >= nextWalk) {
            nextWalk = clock + 1.0 / walkHz;
            x += pendX;
            y += pendY;
            pendX = pendY = 0;
        }
    }
    float walkHz = 0;  // 0: its walk moves it every frame
    float pendX = 0, pendY = 0;
    double nextWalk = 0;
};


struct FootRun {
    std::vector<float> all, stand;
    int samples = 0, gaitMatch = 0, snaps = 0;
    double restMetres = 0;
};

void RunFoot(const Path& path, float lag, const float err[4], uint32_t seed, FootRun& r) {
    Rng rng;
    rng.s = seed;
    const std::vector<Packet> pk = SendPath(path, false, rng);
    PlayerTrack track;
    track.SetOneWay(0.014);  // the game: rtt / 2 of the session
    FootDriver drv;
    AiWalker w;
    w.lag = lag;
    for (int g = 0; g < 4; ++g) w.err[g] = err[g];
    path.At(0, w.x, w.y);
    size_t next = 0;
    double now = 0.05;
    bool started = false;
    while (now < path.Seconds() - 0.3) {
        while (next < pk.size() && pk[next].arrive <= now) {  // the render thread hands received states over
            const Packet& p = pk[next++];
            track.Observe(p.sampleT, now);
            track.PushFoot(p.sampleT, {p.px, p.py, 0}, {p.vx, p.vy, 0}, static_cast<uint32_t>(p.mode));
        }
        const float frame = static_cast<float>(kDt) * (0.9f + 0.2f * rng.Next());  // the game's frames
        track.Update(now);
        const mc::net::replica::Out o = track.Foot(now);
        if (o.valid) {
            LocomotionInput in;
            in.target = o.pos;
            in.targetVel = {o.vel.x, o.vel.y, 0};
            const float sp = std::sqrt(o.vel.x * o.vel.x + o.vel.y * o.vel.y);
            in.heading[0] = sp > 0.1f ? o.vel.x / sp : 1.0f;
            in.heading[1] = sp > 0.1f ? o.vel.y / sp : 0.0f;
            in.gaitHint = static_cast<int8_t>(o.bits);
            in.sprint = o.bits == 3;
            in.haveCopy = true;
            in.copy = {w.x, w.y, 0};
            in.copyHeading[0] = w.hx;
            in.copyHeading[1] = w.hy;
            in.dt = started ? frame : 0.0f;
            started = true;
            const LocomotionCommand c = drv.Update(in);
            w.Apply(c, o.pos.x, o.pos.y);
            // measured like sync_compare: after 1 s (the puppet's first walk), puppet vs the player now
            if (now > 1.0) {
                float tx, ty;
                path.At(now, tx, ty);
                const float e = std::sqrt((w.x - tx) * (w.x - tx) + (w.y - ty) * (w.y - ty));
                r.all.push_back(e);
                if (o.bits == 0) r.stand.push_back(e);
                ++r.samples;
                if (static_cast<uint32_t>(c.gait) == o.bits) ++r.gaitMatch;
                if (c.fix == LocomotionCommand::Fix::Snap) ++r.snaps;
            }
        }
        w.Step(frame);
        now += frame;
    }
    r.restMetres += drv.RestMetres();
}

void TestFoot() {
    const Path mixed = Path::From(kFootMixed, false);
    const Path sprint = Path::From(kFootSprint, false);
    struct Variant {
        const char* name;
        float lag;
        float err[4];
    };
    const Variant variants[] = {
        {"AI 150 ms late, walk -20 %, run +20 %, sprint -15 %", 0.15f, {1, 0.8f, 1.2f, 0.85f}},
        {"AI 200 ms late, its speeds as measured", 0.20f, {1, 1, 1, 1}},
        {"AI 250 ms late, walk +20 %, run -20 %, sprint +15 %", 0.25f, {1, 1.2f, 0.8f, 1.15f}},
    };
    for (const Variant& v : variants) {
        FootRun r;
        RunFoot(mixed, v.lag, v.err, 777, r);
        RunFoot(sprint, v.lag, v.err, 4242, r);
        const std::string n = std::string("foot (") + v.name + "): ";
        const double minutes = r.samples * kDt / 60.0;
        Check(Pct(r.all, 95) <= 0.8, (n + "foot.p95 <= 0.8 m").c_str(), Fmt("%.2f m", Pct(r.all, 95)));
        Check(Mean(r.all) <= 0.35, (n + "foot.mean <= 0.35 m").c_str(), Fmt("%.2f m", Mean(r.all)));
        Check(Pct(r.stand, 95) <= 0.3, (n + "foot.stand.p95 <= 0.3 m").c_str(),
              Fmt("%.2f m (%.0f samples)", Pct(r.stand, 95), static_cast<double>(r.stand.size())));
        Check(r.gaitMatch >= r.samples * 0.9, (n + "foot.gait >= 90 %").c_str(),
              Fmt("%.1f %%", 100.0 * r.gaitMatch / std::max(1, r.samples)));
        Check(r.snaps / minutes <= 0.5, (n + "foot.snaps_per_min <= 0.5").c_str(),
              Fmt("%.2f (%d snaps in %.1f min)", r.snaps / minutes, r.snaps, minutes));
        std::printf("      (max %.2f m, p50 %.2f m, rest blend %.2f m in all)\n", Pct(r.all, 100), Pct(r.all, 50),
                    r.restMetres);
    }
}

// ---- the real two-PC test (2026-10-20) replayed --------------------------------------------------------------------
// Every on-foot stretch of tests/data/foot_2pc.h (the motorbike riders left out): the states as the sender sent them,
// handed to PlayerTrack at the moment the receiver really got them, the receiver's frames at its own frame rate
// (114-250 fps: the commands, and so the nudges, run every frame), FootDriver against the AI walker in four answers
// to a SetPos. Error = puppet vs his real position at the same moment, sampled at 60 Hz as sync_compare's PUP.
// Measured in game on these stretches (0.16.0): p95 1.53 / 1.53 / 1.52 / 1.36 m, mean 0.61 / 0.62 / 0.77 / 0.35 m,
// 67-94 % of a runner's motion our nudges. The 0.16.0 FootDriver replayed against the hampered walker: p95 1.56 /
// 1.27 / 1.42 / 1.07 m (all 1.29), mean 0.36 m, 82 % nudged - the walker that reproduces the test; as commanded 0.57
// m, erratic 0.77 m.
struct Replay2pc {
    std::vector<float> all, stand;
    int samples = 0, gaitMatch = 0, snaps = 0;
    double walk = 0, fix = 0, seconds = 0;
    void Add(const Replay2pc& o) {
        all.insert(all.end(), o.all.begin(), o.all.end());
        stand.insert(stand.end(), o.stand.begin(), o.stand.end());
        samples += o.samples;
        gaitMatch += o.gaitMatch;
        snaps += o.snaps;
        walk += o.walk;
        fix += o.fix;
        seconds += o.seconds;
    }
};

struct AiAnswer {
    const char* name;
    int model;
    float hamper;
    float walkHz;
};

void RunFoot2pc(const Foot2pcSeg& sg, const AiAnswer& ai, Replay2pc& r) {
    const int(*rows)[8] = kFoot2pc + sg.first;
    const int n = sg.count;
    int ti = 0;
    auto truthAt = [&](double t, float& x, float& y) {  // his real position (sender clock), linear between samples
        while (ti + 1 < n && rows[ti + 1][0] / 1000.0 < t) ++ti;
        if (ti + 1 >= n) {
            x = rows[n - 1][2] / 100.0f;
            y = rows[n - 1][3] / 100.0f;
            return;
        }
        const double t0 = rows[ti][0] / 1000.0, t1 = rows[ti + 1][0] / 1000.0;
        const float u = static_cast<float>(std::clamp((t - t0) / std::max(1e-6, t1 - t0), 0.0, 1.0));
        x = (rows[ti][2] + (rows[ti + 1][2] - rows[ti][2]) * u) / 100.0f;
        y = (rows[ti][3] + (rows[ti + 1][3] - rows[ti][3]) * u) / 100.0f;
    };
    Rng rng;
    rng.s = 1000u + static_cast<uint32_t>(sg.first);
    PlayerTrack track;
    track.SetOneWay(0.0125);  // the measured rtt / 2
    FootDriver drv;
    AiWalker w;
    w.model = ai.model;
    w.hamper = ai.hamper;
    w.walkHz = ai.walkHz;
    w.lag = 0.2f;
    w.x = rows[0][2] / 100.0f;
    w.y = rows[0][3] / 100.0f;
    int next = 0;
    double now = rows[0][1] / 1000.0;
    double nextSample = sg.leadInMs / 1000.0;
    bool started = false;
    const double end = rows[n - 1][0] / 1000.0;
    const double frameDt = 1.0 / std::max(30, sg.receiverFps);
    while (now < end) {
        while (next < n && rows[next][1] / 1000.0 <= now) {
            const int* p = rows[next++];
            track.Observe(p[0] / 1000.0, now);
            track.PushFoot(p[0] / 1000.0, {p[2] / 100.0f, p[3] / 100.0f, 0}, {p[4] / 100.0f, p[5] / 100.0f, 0},
                           static_cast<uint32_t>(p[6]));
        }
        const float frame = static_cast<float>(frameDt) * (0.8f + 0.4f * rng.Next());
        track.Update(now);
        const mc::net::replica::Out o = track.Foot(now);
        if (o.valid) {
            LocomotionInput in;
            in.target = o.pos;
            in.targetVel = {o.vel.x, o.vel.y, 0};
            const float sp = std::sqrt(o.vel.x * o.vel.x + o.vel.y * o.vel.y);
            in.heading[0] = sp > 0.1f ? o.vel.x / sp : 1.0f;
            in.heading[1] = sp > 0.1f ? o.vel.y / sp : 0.0f;
            in.gaitHint = static_cast<int8_t>(o.bits);
            in.sprint = o.bits == 3;
            in.haveCopy = true;
            in.copy = {w.x, w.y, 0};
            in.copyHeading[0] = w.hx;
            in.copyHeading[1] = w.hy;
            in.dt = started ? frame : 0.0f;
            started = true;
            const LocomotionCommand c = drv.Update(in);
            w.Apply(c, o.pos.x, o.pos.y);
            if (now >= nextSample) {
                nextSample += 1.0 / 60;
                float tx, ty;
                truthAt(now, tx, ty);
                const float e = std::sqrt((w.x - tx) * (w.x - tx) + (w.y - ty) * (w.y - ty));
                r.all.push_back(e);
                if (o.bits == 0) r.stand.push_back(e);
                ++r.samples;
                if (static_cast<uint32_t>(c.gait) == o.bits) ++r.gaitMatch;
            }
            if (c.fix == LocomotionCommand::Fix::Snap && now >= sg.leadInMs / 1000.0) ++r.snaps;
        }
        w.Step(frame);
        now += frame;
    }
    r.walk += w.walkMetres;
    r.fix += w.fixMetres;
    r.seconds += end - sg.leadInMs / 1000.0;
}

void TestFoot2pc() {
    static const AiAnswer kAnswers[] = {
        {"its walk as commanded", 0, 1, 0},
        {"its walk hampered by a SetPos (x0.15 for 50 ms, reproduces the test)", 1, 0.15f, 0},
        {"its walk erratic (0.3-1 of its speed)", 2, 1, 0},
        {"its walk as commanded, moved by the game at 30 Hz only", 0, 1, 30},
        {"hampered, and moved by the game at 30 Hz only", 1, 0.15f, 30},
    };
    for (const AiAnswer& ai : kAnswers) {
        Replay2pc all, dir;
        const char* last = "";
        const std::string n = std::string("two-PC replay (") + ai.name + "): ";
        auto flush = [&](const char* name) {
            if (!dir.samples) return;
            Check(Pct(dir.all, 95) <= 0.8, (n + name + " foot.p95 <= 0.8 m").c_str(),
                  Fmt("%.2f m (mean %.2f m, stand p95 %.2f m)", Pct(dir.all, 95), Mean(dir.all), Pct(dir.stand, 95)));
            dir = Replay2pc{};
        };
        for (const Foot2pcSeg& sg : kFoot2pcSegs) {
            if (std::string(sg.name) != last) {
                flush(last);
                last = sg.name;
            }
            Replay2pc one;
            RunFoot2pc(sg, ai, one);
            dir.Add(one);
            all.Add(one);
        }
        flush(last);
        const double minutes = all.seconds / 60.0;
        Check(Pct(all.all, 95) <= 0.8, (n + "foot.p95 <= 0.8 m").c_str(), Fmt("%.2f m", Pct(all.all, 95)));
        Check(Mean(all.all) <= 0.35, (n + "foot.mean <= 0.35 m").c_str(), Fmt("%.2f m", Mean(all.all)));
        Check(Pct(all.stand, 95) <= 0.3, (n + "foot.stand.p95 <= 0.3 m").c_str(),
              Fmt("%.2f m (%.0f samples)", Pct(all.stand, 95), static_cast<double>(all.stand.size())));
        Check(all.gaitMatch >= all.samples * 0.9, (n + "foot.gait >= 90 %").c_str(),
              Fmt("%.1f %%", 100.0 * all.gaitMatch / std::max(1, all.samples)));
        Check(all.snaps / minutes <= 0.5, (n + "foot.snaps_per_min <= 0.5").c_str(),
              Fmt("%.2f (%d snaps in %.1f min)", all.snaps / minutes, all.snaps, minutes));
        std::printf("      (max %.2f m, p50 %.2f m; moved by our fixes: %.0f %% of %.0f m)\n", Pct(all.all, 100),
                    Pct(all.all, 50), 100.0 * all.fix / std::max(1e-6, all.fix + all.walk), all.fix + all.walk);
    }
}

// ---- the rest blend ---------------------------------------------------------------------------------------------
void TestRest() {
    FootDriver drv;
    LocomotionInput in;
    in.gaitHint = 0;
    in.haveCopy = true;
    in.target = {0.5f, 0, 0};
    in.copy = {0, 0, 0};
    in.dt = static_cast<float>(kDt);
    const LocomotionCommand c = drv.Update(in);
    Check(c.gait == Gait::Stand, "he stands 0.5 m away: the puppet stands (no walk for the last bit)");
    const float slid = std::sqrt(c.nudge.x * c.nudge.x + c.nudge.y * c.nudge.y);
    Check(c.fix == LocomotionCommand::Fix::Nudge && drv.Resting() &&
              std::fabs(slid - 0.15f * static_cast<float>(kDt)) < 1e-4f,
          "... slid onto his spot at 0.15 m/s", Fmt("%.4f m this tick", std::sqrt(c.nudge.x * c.nudge.x)));
    // it gets there: within the dead zone after a few seconds, then left alone
    float x = 0;
    for (int i = 0; i < 300; ++i) {
        in.copy = {x, 0, 0};
        const LocomotionCommand k = drv.Update(in);
        if (k.fix == LocomotionCommand::Fix::Nudge) x += k.nudge.x;
    }
    Check(std::fabs(0.5f - x) <= 0.11f, "... within 0.1 m of his spot after 5 s", Fmt("%.3f m off", 0.5f - x));
    in.copy = {x, 0, 0};
    const LocomotionCommand still = drv.Update(in);
    Check(still.fix == LocomotionCommand::Fix::None, "... then left alone");
    in.target = {2, 0, 0};
    in.copy = {0, 0, 0};
    FootDriver far;
    Check(far.Update(in).gait == Gait::Walk, "2 m away while he stands: walks there");
    // he moves: no rest blend, the controller's own rules
    FootDriver mv;
    in.gaitHint = 2;
    in.target = {0.5f, 0, 0};
    in.targetVel = {4, 0, 0};
    const LocomotionCommand run = mv.Update(in);
    Check(run.gait == Gait::Run && !mv.Resting(), "he runs: his gait, no rest blend");
    // a correction the caller applied instead (traversal Catchup: a blend after a move) is not taken for walking
    FootDriver ap;
    LocomotionInput st;
    st.gaitHint = 0;
    st.haveCopy = true;
    st.target = {0, 0, 0};
    st.dt = static_cast<float>(kDt);
    LocomotionCommand applied = ap.Update(st);
    applied.fix = LocomotionCommand::Fix::Nudge;
    applied.nudge = {0.2f, 0, 0};
    ap.Applied(applied);
    st.copy = {0.2f, 0, 0};
    ap.Update(st);
    Check(ap.CopySpeed() < 0.01f, "a correction applied by the caller is not walking (copy speed)",
          Fmt("%.2f m/s", ap.CopySpeed()));
}

// ---- PlayerTrack ------------------------------------------------------------------------------------------------
void TestTrack() {
    PlayerTrack tr;
    const float q[4] = {0, 0, 0, 1};
    double t = 0;
    for (int i = 0; i < 30; ++i, t += kDt) {
        tr.Observe(t, t + 0.02);
        tr.PushFoot(t, {static_cast<float>(t * 5), 0, 0}, {5, 0, 0}, 2);
        tr.PushCar(t, 11, {static_cast<float>(t * 5), 0, 0}, {5, 0, 0}, q);
    }
    // he gets out and into another car 100 m away: the new car is never blended with the old one
    for (int i = 0; i < 30; ++i, t += kDt) {
        tr.Observe(t, t + 0.02);
        tr.PushFoot(t, {100, 0, 0}, {0, 0, 0}, 0);
        if (i == 10) tr.NoCar();
        if (i >= 20) tr.PushCar(t, 22, {100, 50, 0}, {0, 0, 0}, q);
    }
    double worst = 0;
    for (double now = t - 0.1; now < t + 0.05; now += 0.005) {
        tr.Update(now);
        const auto c = tr.Car(now);
        if (c.valid && c.pos.y < 49.9f) worst = std::max(worst, static_cast<double>(50 - c.pos.y));
    }
    Check(worst == 0, "another car: its track starts fresh (no sweep from the old car)", Fmt("%.1f m", worst));
    // the lead: a steady 5 m/s walker is shown where he is now (within the smallest one-way trip, 20 ms here)
    PlayerTrack lead;
    double s = 0, err = 0;
    for (int i = 0; i < 240; ++i, s += kDt) {
        lead.Observe(s, s + 0.02);
        lead.PushFoot(s, {static_cast<float>(s * 5), 0, 0}, {5, 0, 0}, 2);
        lead.Update(s + 0.02);
        if (i > 120) err = std::max(err, std::fabs(lead.Foot(s + 0.02).pos.x - s * 5));
    }
    Check(err <= 5 * 0.02 + 0.02, "lead: shown where he is now (minus the one-way trip)", Fmt("%.3f m", err));
}

// ---- his car ----------------------------------------------------------------------------------------------------
struct CopyCar {
    float x = 0, y = 0, yaw = 0, speed = 0;  // yaw radians; speed along the heading
    float power = 0.85f;                      // its own physics reaches this share of his speed
    float lag = 0.3f;
    static constexpr float kAccel = 6, kBrake = 9;
    // (his speed, his path's curvature) as his controls act on the copy, lag later: its steering gives the same
    // curvature at its own speed (yaw rate = curvature x speed), its throttle `power` x his speed
    std::deque<std::pair<float, float>> hist;
    void Step(float dt, float hisSpeed, float hisYawRate) {
        hist.push_back({hisSpeed, hisSpeed > 1.0f ? hisYawRate / hisSpeed : 0.0f});
        const size_t lagTicks = static_cast<size_t>(lag / dt + 0.5f);
        while (hist.size() > lagTicks + 1) hist.pop_front();
        const float want = hist.front().first * power;
        speed += std::clamp(want - speed, -kBrake * dt, kAccel * dt);
        yaw += hist.front().second * speed * dt;
        x += std::cos(yaw) * speed * dt;
        y += std::sin(yaw) * speed * dt;
    }
};

// A car rotation as Mafia DE reports it (pmotion::CarForward: the car's +y rotated by the conjugate) for a heading
// `yawRad` in the ground plane, and back.
void YawQuat(float yawRad, float q[4]) {
    const float th = yawRad - kPi / 2;  // conj(q) turns +y (90 deg) to the heading
    q[0] = q[1] = 0;
    q[2] = -std::sin(th / 2);
    q[3] = std::cos(th / 2);
}

float QuatYaw(const float q[4]) { return 2 * std::atan2(-q[2], q[3]) + kPi / 2; }

struct CarRun {
    std::vector<float> err, ratio;
    int samples = 0, pinned = 0, pins = 0, speedFixes = 0;
};

void RunCar(const Path& path, float power, float lag, uint32_t seed, CarRun& r) {
    Rng rng;
    rng.s = seed;
    const std::vector<Packet> pk = SendPath(path, true, rng);
    PlayerTrack track;
    track.SetOneWay(0.014);
    CarDriver drv;
    CopyCar copy;
    copy.power = power;
    copy.lag = lag;
    path.At(0, copy.x, copy.y);
    copy.yaw = path.YawAt(0) * kPi / 180;
    size_t next = 0;
    double now = 0.05;
    float lastYaw = copy.yaw;
    bool started = false;
    while (now < path.Seconds() - 0.3) {
        while (next < pk.size() && pk[next].arrive <= now) {
            const Packet& p = pk[next++];
            float q[4];
            YawQuat(p.yaw * kPi / 180, q);
            track.Observe(p.sampleT, now);
            track.PushFoot(p.sampleT, {p.px, p.py, 0}, {p.vx, p.vy, 0});
            track.PushCar(p.sampleT, 7, {p.px, p.py, 0}, {p.vx, p.vy, 0}, q);
        }
        const float frame = static_cast<float>(kDt) * (0.9f + 0.2f * rng.Next());
        track.Update(now);
        const mc::net::replica::Out o = track.Car(now);
        if (o.valid) {
            VehicleInput in;
            in.target = o.pos;
            in.targetVel = o.vel;
            for (int k = 0; k < 4; ++k) in.targetRot[k] = o.rot[k];
            in.haveCopy = true;
            in.copy = {copy.x, copy.y, 0};
            in.copyVel = {std::cos(copy.yaw) * copy.speed, std::sin(copy.yaw) * copy.speed, 0};
            YawQuat(copy.yaw, in.copyRot);
            in.power = 0.5f;
            in.dt = started ? frame : 0.0f;
            started = true;
            const CarCommand c = drv.Update(in);
            const VehicleCommand& vc = c.vc;
            const float want = std::sqrt(vc.vel.x * vc.vel.x + vc.vel.y * vc.vel.y);
            if (vc.fix == VehicleCommand::Fix::Pin || vc.fix == VehicleCommand::Fix::Snap) {
                copy.x = o.pos.x;
                copy.y = o.pos.y;
                copy.yaw = QuatYaw(vc.rot);
                copy.speed = want;  // the glue gives the speed after every move (SetSpeed, km/h)
            } else if (vc.fix == VehicleCommand::Fix::Blend) {
                copy.x += vc.nudge.x;
                copy.y += vc.nudge.y;
                copy.yaw = QuatYaw(vc.rot);
                copy.speed = want;
            } else if (c.giveSpeed) {
                copy.speed = want;
            }
            if (now > 1.0) {
                float tx, ty;
                path.At(now, tx, ty);
                r.err.push_back(std::sqrt((copy.x - tx) * (copy.x - tx) + (copy.y - ty) * (copy.y - ty)));
                if (vc.tspeed > 2) r.ratio.push_back(vc.cspeed / vc.tspeed);
                ++r.samples;
                if (vc.pinned) ++r.pinned;
                if (vc.justPinned) ++r.pins;
            }
        }
        // his speed and yaw rate now (what his controls do), for the copy's own physics
        float ax, ay, bx, by;
        path.At(now, ax, ay);
        path.At(now + 0.01, bx, by);
        const float hisSpeed = std::sqrt((bx - ax) * (bx - ax) + (by - ay) * (by - ay)) / 0.01f;
        const float hisYaw = path.YawAt(now) * kPi / 180;
        float dy = hisYaw - lastYaw;
        while (dy > kPi) dy -= 2 * kPi;
        while (dy < -kPi) dy += 2 * kPi;
        lastYaw = hisYaw;
        copy.Step(frame, hisSpeed, dy / frame);
        now += frame;
    }
    r.speedFixes += static_cast<int>(drv.SpeedFixes());
}

void TestCar() {
    const Path car = Path::From(kCarPath, true);
    struct Variant {
        const char* name;
        float power, lag;
    };
    const Variant variants[] = {
        {"copy physics 85 % of his speed, 150 ms late", 0.85f, 0.15f},
        {"copy physics 115 % of his speed, 150 ms late", 1.15f, 0.15f},
        {"copy physics 85 % of his speed, 300 ms late", 0.85f, 0.3f},
    };
    for (const Variant& v : variants) {
        CarRun r;
        RunCar(car, v.power, v.lag, 99, r);
        const std::string n = std::string("car (") + v.name + "): ";
        Check(Pct(r.err, 95) <= 1.0, (n + "car.p95 <= 1.0 m").c_str(), Fmt("%.2f m", Pct(r.err, 95)));
        Check(r.pinned <= r.samples * 0.10, (n + "car.pinned <= 10 %").c_str(),
              Fmt("%.1f %% (%d pins)", 100.0 * r.pinned / std::max(1, r.samples), r.pins));
        const double med = Pct(r.ratio, 50);
        Check(med >= 0.9 && med <= 1.1, (n + "car.speed_ratio 0.9..1.1").c_str(), Fmt("%.2f", med));
        std::printf("      (max %.2f m, p50 %.2f m, speed fixes %d)\n", Pct(r.err, 100), Pct(r.err, 50),
                    r.speedFixes);
    }
    // the preset: a copy inside 0.5 m and turned less than 3 degrees is left alone (no SetPos, no SetRot)
    CarDriver d;
    VehicleInput in;
    in.haveCopy = true;
    in.target = {10, 0, 0};
    in.copy = {9.7f, 0, 0};
    in.targetVel = in.copyVel = {10, 0, 0};
    in.dt = static_cast<float>(kDt);
    const CarCommand c = d.Update(in);
    Check(c.vc.fix == VehicleCommand::Fix::None && !c.giveSpeed, "car 0.3 m off at his speed: nothing set on the copy");
    in.copyVel = {6, 0, 0};
    const CarCommand s = d.Update(in);
    Check(s.vc.fix == VehicleCommand::Fix::None && s.giveSpeed, "... 4 m/s too slow: given his speed, not moved");
    in.copy = {8.5f, 0, 0};
    in.copyVel = {10, 0, 0};
    const CarCommand b = d.Update(in);
    const float nl = std::sqrt(b.vc.nudge.x * b.vc.nudge.x);
    Check(b.vc.fix == VehicleCommand::Fix::Blend && nl > 0 && nl <= 0.5f * 10 * static_cast<float>(kDt) + 1e-4f,
          "1.5 m off at 10 m/s: blended at most 50 % of his speed (CarPreset)", Fmt("%.3f m this tick", nl));
    in.copy = {4, 0, 0};
    Check(d.Update(in).vc.fix == VehicleCommand::Fix::Snap, "6 m off: snapped");
}

}  // namespace


int main() {
    TestTrack();
    TestRest();
    TestFoot();
    TestFoot2pc();
    TestCar();
    std::printf("%s (%d failure%s)\n", g_failures ? "FAILED" : "ALL PASSED", g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
