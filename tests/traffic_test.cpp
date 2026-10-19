// traffic_test: unit tests for the traffic sync model (src/game/traffic_model.h): the wire format, an authority's
// per-player tracking (interest, admission cap, spawn / despawn / changed-only states, sharing), a receiver's table
// (copies near us, interpolation through core's replica::Stream, removal, hand-over, bumps, several authorities), the
// regions (whose traffic runs where), a guest authority's cars through the relay to a second guest, the hand-over when
// players converge, and a recorded car path (the 0.15.0 two-PC trace) through the whole chain with simulated copy
// physics against the true-error target. Prints PASS/FAIL per check; exits non-zero on any failure.
#include "game/latency.h"
#include "game/traffic_model.h"
#include "traffic_path.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace mc;
using namespace mc::game::traffic;

namespace {

int g_failures = 0;

void Check(bool ok, const char* name, const std::string& detail = {}) {
    std::printf("%s  %-56s %s\n", ok ? "PASS" : "FAIL", name, detail.c_str());
    std::fflush(stdout);
    if (!ok) ++g_failures;
}

template <class A = double, class B = double, class C = double>
std::string Fmt(const char* fmt, A a = 0, B b = 0, C c = 0) {
    char buf[256];
    std::snprintf(buf, sizeof buf, fmt, static_cast<double>(a), static_cast<double>(b), static_cast<double>(c));
    return buf;
}

struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed) {}
    uint32_t Next() {
        s = s * 6364136223846793005ull + 1442695040888963407ull;
        return static_cast<uint32_t>(s >> 33);
    }
    float Uniform(float lo, float hi) { return lo + (hi - lo) * static_cast<float>(Next() & 0xFFFFFF) / 16777216.0f; }
};

float QuatAngleDeg(const float a[4], const float b[4]) {
    float dot = 0;
    for (int i = 0; i < 4; ++i) dot += a[i] * b[i];
    dot = std::fabs(dot);
    if (dot > 1) dot = 1;
    return 2 * std::acos(dot) * 180.0f / 3.14159265f;
}

void YawQuat(float yawRad, float q[4]) {  // rotation about z
    q[0] = 0;
    q[1] = 0;
    q[2] = std::sin(yawRad * 0.5f);
    q[3] = std::cos(yawRad * 0.5f);
}

HostCar Car(uint32_t key, float x, float y, float vx = 0, float vy = 0) {
    HostCar c;
    c.key = key;
    c.model = "lassiter_v16";
    c.plate = "LH-" + std::to_string(key);
    c.color[0] = 3;
    c.color[1] = 7;
    c.st.pos = {x, y, 5};
    c.st.vel = {vx, vy, 0};
    YawQuat(0.3f, c.st.rot);
    c.st.flags = kControls;
    c.st.power = 0.5f;
    c.st.steer = -0.25f;
    return c;
}

// ---- wire ----
void TestQuat() {
    Rng r(1);
    float worst = 0;
    bool unit = true;
    for (int i = 0; i < 20000; ++i) {
        float q[4] = {r.Uniform(-1, 1), r.Uniform(-1, 1), r.Uniform(-1, 1), r.Uniform(-1, 1)};
        float l = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
        if (l < 0.05f) continue;
        for (float& v : q) v /= l;
        float o[4];
        UnpackQuat(PackQuat(q), o);
        worst = std::max(worst, QuatAngleDeg(q, o));
        const float ol = std::sqrt(o[0] * o[0] + o[1] * o[1] + o[2] * o[2] + o[3] * o[3]);
        if (std::fabs(ol - 1) > 0.002f) unit = false;
    }
    Check(worst < 0.25f, "quat: smallest-three round trip within 0.25 deg", Fmt("worst %.3f deg", worst));
    Check(unit, "quat: unpacked quaternions are unit length");
    float q[4] = {0.1f, -0.2f, 0.3f, -0.9f}, n[4];
    for (int i = 0; i < 4; ++i) n[i] = -q[i];
    float a[4], b[4];
    UnpackQuat(PackQuat(q), a);
    UnpackQuat(PackQuat(n), b);
    Check(QuatAngleDeg(a, b) < 0.01f, "quat: q and -q pack to the same rotation");
    float bad[4] = {NAN, 0, 0, 0}, o[4];
    UnpackQuat(PackQuat(bad), o);
    Check(std::fabs(o[3]) > 0.999f, "quat: NaN input packs to identity");
}

void TestStates() {
    std::vector<CarState> in;
    Rng r(7);
    for (int i = 0; i < 100; ++i) {
        CarState s;
        s.net = static_cast<uint16_t>(i + 1);
        s.pos = {r.Uniform(-300, 300) + 1000, r.Uniform(-300, 300) - 2000, r.Uniform(-5, 30)};
        s.vel = {r.Uniform(-30, 30), r.Uniform(-30, 30), r.Uniform(-2, 2)};
        YawQuat(r.Uniform(-3, 3), s.rot);
        s.steer = r.Uniform(-1, 1);
        s.power = r.Uniform(0, 1);
        s.brake = r.Uniform(0, 1);
        s.flags = static_cast<uint8_t>(r.Next() & 31);
        in.push_back(s);
    }
    auto packets = PackStates(123456, in);
    Check(packets.size() == 3, "states: 100 cars -> 3 packets of at most 40", Fmt("%.0f packets", packets.size()));
    bool sizeOk = true;
    for (const auto& p : packets)
        if (p.size() > kStatesHeader + kMaxPerPacket * kStateBytes) sizeOk = false;
    Check(sizeOk && packets[0].size() == kStatesHeader + 40 * kStateBytes, "states: 22 bytes per car + 17 header",
          Fmt("%.0f bytes", packets[0].size()));
    std::vector<CarState> out;
    float maxPos = 0, maxVel = 0, maxCtl = 0, maxRot = 0;
    bool flagsOk = true, netOk = true, timeOk = true;
    size_t at = 0;
    for (const auto& p : packets) {
        net::ByteReader rd(p.data(), p.size());
        uint32_t ms = 0;
        std::vector<CarState> part;
        if (!ReadStates(rd, ms, part)) {
            Check(false, "states: packet reads back");
            return;
        }
        if (ms != 123456) timeOk = false;
        for (const CarState& s : part) {
            const CarState& o = in[at++];
            if (s.net != o.net) netOk = false;
            maxPos = std::max({maxPos, std::fabs(s.pos.x - o.pos.x), std::fabs(s.pos.y - o.pos.y),
                               std::fabs(s.pos.z - o.pos.z)});
            maxVel = std::max({maxVel, std::fabs(s.vel.x - o.vel.x), std::fabs(s.vel.y - o.vel.y)});
            maxCtl = std::max({maxCtl, std::fabs(s.steer - o.steer), std::fabs(s.power - o.power),
                               std::fabs(s.brake - o.brake)});
            maxRot = std::max(maxRot, QuatAngleDeg(s.rot, o.rot));
            if (s.flags != o.flags) flagsOk = false;
        }
    }
    Check(at == in.size() && netOk && timeOk, "states: every car back, in order, with the host time");
    Check(maxPos <= 1.0f / 64 + 1e-3f, "states: position within 1/64 m", Fmt("%.4f m", maxPos));
    Check(maxVel <= 0.006f, "states: velocity within 0.005 m/s", Fmt("%.4f", maxVel));
    Check(maxCtl <= 0.005f && flagsOk, "states: controls and flags", Fmt("%.4f", maxCtl));
    Check(maxRot < 0.25f, "states: rotation", Fmt("%.3f deg", maxRot));

    std::vector<CarState> spread(2);
    spread[0].net = 1;
    spread[0].pos = {0, 0, 0};
    spread[1].net = 2;
    spread[1].pos = {1500, 0, 0};  // beyond +-1023 m of the first: a packet of its own
    auto sp = PackStates(1, spread);
    std::vector<CarState> o2;
    uint32_t ms = 0;
    bool ok = sp.size() == 2;
    if (ok) {
        net::ByteReader rd(sp[1].data(), sp[1].size());
        ok = ReadStates(rd, ms, o2) && o2.size() == 1 && std::fabs(o2[0].pos.x - 1500) < 0.05f;
    }
    Check(ok, "states: a car out of the origin's range starts a new packet");

    std::vector<uint8_t> trunc = packets[0];
    trunc.resize(trunc.size() - 5);
    net::ByteReader rd(trunc.data(), trunc.size());
    Check(!ReadStates(rd, ms, o2), "states: a truncated packet is refused");
}

void TestSpawns() {
    std::vector<SpawnInfo> in(2);
    in[0].net = 5;
    in[0].kind = kPolice;
    in[0].model = "bolt_v8_police";
    in[0].color[0] = 1;
    in[0].color[1] = 2;
    in[0].plate = "LH-123";
    in[0].st.pos = {12.5f, -800.25f, 3};
    YawQuat(1.0f, in[0].st.rot);
    in[0].st.vel = {3, 4, 0};
    in[0].st.flags = kSiren;
    in[1].net = 6;
    in[1].model = "shubert_e_six";
    in[1].plate = "bad plate!";  // not a token: sent empty
    auto buf = PackSpawns(in);
    net::ByteReader r(buf.data(), buf.size());
    std::vector<SpawnInfo> out;
    const bool ok = ReadSpawns(r, out);
    Check(ok && out.size() == 2, "spawns: round trip");
    if (ok && out.size() == 2) {
        Check(out[0].net == 5 && out[0].kind == kPolice && out[0].model == "bolt_v8_police" && out[0].plate == "LH-123" &&
                  out[0].color[0] == 1 && out[0].color[1] == 2 && out[0].st.flags == kSiren &&
                  std::fabs(out[0].st.pos.y + 800.25f) < 1e-3f && std::fabs(out[0].st.vel.y - 4) < 0.01f,
              "spawns: fields");
        Check(out[1].plate.empty() && out[1].color[0] == -1, "spawns: an unsafe plate goes as none");
    }
    SpawnInfo bad;
    bad.net = 9;
    bad.model = "x y";  // spaces would break the Lua line
    auto b2 = PackSpawns({bad});
    net::ByteReader r2(b2.data(), b2.size());
    Check(!ReadSpawns(r2, out), "spawns: an unsafe model is refused");
    auto d = PackDespawns({1, 2, 65535});
    net::ByteReader r3(d.data(), d.size());
    std::vector<uint16_t> nets;
    Check(ReadDespawns(r3, nets) && nets.size() == 3 && nets[2] == 65535, "despawns: round trip");
}

// ---- host ----
struct HostRun {
    std::set<uint16_t> live;  // spawned on the guest
    uint32_t spawns = 0, despawns = 0, states = 0;
    void Apply(const std::vector<PeerOut>& out, uint32_t peer) {
        for (const auto& po : out) {
            if (po.peer != peer) continue;
            for (const auto& b : po.spawns) {
                net::ByteReader r(b.data(), b.size());
                std::vector<SpawnInfo> s;
                ReadSpawns(r, s);
                for (auto& x : s) live.insert(x.net), ++spawns;
            }
            for (const auto& b : po.despawns) {
                net::ByteReader r(b.data(), b.size());
                std::vector<uint16_t> n;
                ReadDespawns(r, n);
                for (auto x : n) live.erase(x), ++despawns;
            }
            states += po.sent;
        }
    }
};

void TestHost() {
    HostTraffic h;
    std::vector<HostCar> cars;
    for (uint32_t i = 0; i < 40; ++i) cars.push_back(Car(1000 + i, 10.0f + i * 12.0f, 0));  // 10 .. 478 m
    h.SetCars(cars);
    h.SetView(1, {0, 0, 5}, 0);
    std::vector<PeerOut> out;
    HostRun run;
    h.Tick(0, 0, out);
    run.Apply(out, 1);
    // within 260 m: 10 + 12i <= 260 -> i <= 20: 21 cars, under the cap of 24
    Check(run.live.size() == 21, "host: guest gets the cars within 260 m", Fmt("%.0f", run.live.size()));
    bool nearest = true;
    for (uint16_t n : run.live)
        if (h.KeyOf(n) > 1020) nearest = false;
    Check(nearest, "host: those are the nearest ones");
    Check(h.NetOf(1000) != 0 && h.KeyOf(h.NetOf(1000)) == 1000, "host: net id <-> entity id");

    // a few ticks later nothing moved: states only now and then (changed-only)
    uint32_t skipped = 0, sent = 0;
    for (int t = 1; t <= 10; ++t) {
        h.SetCars(cars);
        h.SetView(1, {0, 0, 5}, t * 0.05);
        h.Tick(t * 0.05, t * 50, out);
        run.Apply(out, 1);
        for (auto& po : out) skipped += po.skipped, sent += po.sent;
    }
    Check(skipped > 0 && sent == 0, "host: unchanged cars are not sent again", Fmt("skipped %.0f sent %.0f", skipped, sent));
    // moving cars are sent
    for (auto& c : cars) c.st.pos.y += 0.5f, c.st.vel.y = 10;
    h.SetCars(cars);
    h.SetView(1, {0, 0, 5}, 0.6);
    h.Tick(0.6, 600, out);
    sent = 0;
    for (auto& po : out) sent += po.sent;
    Check(sent > 0, "host: moved cars are sent", Fmt("%.0f", sent));
    // after refresh (1.5 s) an unchanged car is sent again
    double t = 0.6;
    uint32_t refreshed = 0;
    for (int i = 0; i < 40; ++i) {
        t += 0.05;
        h.SetCars(cars);
        h.SetView(1, {0, 0, 5}, t);
        h.Tick(t, static_cast<uint32_t>(t * 1000), out);
        if (t > 2.2)
            for (auto& po : out) refreshed += po.sent;
    }
    Check(refreshed > 0, "host: an unchanged car is refreshed after 1.5 s");

    // a car leaves the host's list: despawned on the guest
    const uint16_t gone = h.NetOf(1000);
    std::vector<HostCar> fewer(cars.begin() + 1, cars.end());
    h.SetCars(fewer);
    h.SetView(1, {0, 0, 5}, t + 0.05);
    h.Tick(t + 0.05, 0, out);
    run.Apply(out, 1);
    Check(!run.live.count(gone) && h.NetOf(1000) == 0, "host: a car gone on the host is despawned");

    // the guest drives off: everything despawned
    t += 0.1;
    h.SetView(1, {5000, 5000, 5}, t);
    h.Tick(t, 0, out);
    run.Apply(out, 1);
    Check(run.live.empty(), "host: out of range -> despawned", Fmt("%.0f left", run.live.size()));

    // no view for 2 s: sees nothing
    h.SetView(1, {0, 0, 5}, t + 0.1);
    h.Tick(t + 0.1, 0, out);
    run.Apply(out, 1);
    const size_t back = run.live.size();
    h.Tick(t + 3, 0, out);
    run.Apply(out, 1);
    Check(back > 0 && run.live.empty(), "host: a guest without a viewpoint gets nothing");
}

void TestHostCap() {
    HostConfig cfg;
    cfg.maxPerPeer = 5;
    HostTraffic h(cfg);
    std::vector<HostCar> cars;
    for (uint32_t i = 0; i < 8; ++i) cars.push_back(Car(1 + i, 160.0f + i * 5.0f, 0));  // all beyond evictFar
    h.SetCars(cars);
    h.SetView(7, {0, 0, 5}, 0);
    std::vector<PeerOut> out;
    HostRun run;
    h.Tick(0, 0, out);
    run.Apply(out, 7);
    Check(run.live.size() == 5, "host cap: at most maxPerPeer cars", Fmt("%.0f", run.live.size()));
    cars.push_back(Car(99, 30, 0));  // a car right next to the guest
    h.SetCars(cars);
    h.SetView(7, {0, 0, 5}, 0.05);
    h.Tick(0.05, 50, out);
    run.Apply(out, 7);
    const uint16_t nearNet = h.NetOf(99);
    Check(run.live.size() == 5 && run.live.count(nearNet) && run.despawns == 1,
          "host cap: a near car takes the place of the farthest", Fmt("%.0f live, %.0f despawns", run.live.size(), run.despawns));
    Check(!run.live.count(h.NetOf(5)), "host cap: the farthest admitted one went");

    // two guests: each his own set
    h.SetView(8, {195, 0, 5}, 0.1);
    h.SetView(7, {0, 0, 5}, 0.1);
    h.Tick(0.1, 100, out);
    HostRun r8;
    r8.Apply(out, 8);
    Check(r8.live.size() == 5 && h.Stats(8) && h.Stats(8)->admitted == 5, "host: a second guest gets his own cars");
    h.RemovePeer(8);
    h.Tick(0.15, 150, out);
    bool none8 = true;
    for (auto& po : out)
        if (po.peer == 8) none8 = false;
    Check(none8 && !h.Stats(8), "host: a guest that left gets nothing");
}

// ---- guest ----
SpawnInfo Spawn(uint16_t net, float x, float y) {
    SpawnInfo s;
    s.net = net;
    s.model = "lassiter_v16";
    s.st.net = net;
    s.st.pos = {x, y, 0};
    return s;
}

CarState State(uint16_t net, float x, float y, float vx, float vy) {
    CarState s;
    s.net = net;
    s.pos = {x, y, 0};
    s.vel = {vx, vy, 0};
    s.flags = kControls;
    s.power = 0.4f;
    return s;
}

size_t Count(const std::vector<GuestCmd>& c, GuestCmd::Type t) {
    size_t n = 0;
    for (const auto& x : c)
        if (x.type == t) ++n;
    return n;
}

void TestGuest() {
    GuestTraffic g;
    std::vector<GuestCmd> cmds;
    std::vector<GuestBump> bumps;
    for (uint16_t n = 1; n <= 30; ++n) g.OnSpawn(Spawn(n, n * 7.0f, 0), 0);
    g.SetLocal({0, 0, 0}, false);
    g.Tick(0.01, 0.01f, cmds, bumps);
    Check(Count(cmds, GuestCmd::Type::Add) == 0, "receiver: not in the world -> no copies");
    g.SetLocal({0, 0, 0}, true);
    g.Tick(0.02, 0.01f, cmds, bumps);
    Check(Count(cmds, GuestCmd::Type::Add) == 20, "receiver: copies for the nearest 20",
          Fmt("%.0f adds", Count(cmds, GuestCmd::Type::Add)));
    bool nearestFirst = true;
    for (const auto& c : cmds)
        if (c.type == GuestCmd::Type::Add && c.key > 20) nearestFirst = false;
    Check(nearestFirst, "receiver: nearest first");

    // interpolation: states 0.1 s apart (host time), at 10 m/s along x
    GuestTraffic g2;
    g2.OnSpawn(Spawn(1, 0, 0), 0);
    g2.OnStates(1000, {State(1, 0, 0, 10, 0)}, 10.0);
    g2.OnStates(1100, {State(1, 1, 0, 10, 0)}, 10.1);
    CarState t;
    // render time = 10.15 - offset(9.0) - 0.12 = 1.03 host s -> 0.3 m, plus the lead 10 x 0.12 = 1.2 m
    const bool have = g2.Target(1, 10.15, t);
    Check(have && std::fabs(t.pos.x - 1.5f) < 0.02f, "receiver: interpolated target plus the lead", Fmt("x %.3f", t.pos.x));
    g2.Target(1, 10.6, t);  // beyond the newest: extrapolated (at most 0.5 s) + lead
    Check(std::fabs(t.pos.x - (1 + 10 * 0.38f + 1.2f)) < 0.05f, "receiver: extrapolated past the newest state",
          Fmt("x %.3f", t.pos.x));
    g2.OnStates(1050, {State(1, 0.5f, 0, 10, 0)}, 10.2);
    Check(g2.Stats().dropsOld == 1, "receiver: an older state is dropped");

    // drive: a report on target, then knocked 5 m off -> a bump
    g2.SetLocal({0, 0, 0}, true);
    g2.Tick(10.2, 0.016f, cmds, bumps);
    Check(Count(cmds, GuestCmd::Type::Add) == 1, "receiver: copy added");
    g2.OnStates(1200, {State(1, 2, 0, 0, 0)}, 10.3);
    g2.OnStates(1300, {State(1, 2, 0, 0, 0)}, 10.4);
    CopyReport rep;
    rep.pos = {2, 0, 0};
    g2.SetReport(1, rep, 10.5);
    g2.Tick(10.5, 0.016f, cmds, bumps);
    Check(Count(cmds, GuestCmd::Type::Drive) == 1 && bumps.empty(), "receiver: driven, on target");
    rep.pos = {7, 0, 0};
    g2.SetReport(1, rep, 10.52);
    g2.Tick(10.52, 0.016f, cmds, bumps);
    Check(bumps.size() == 1 && bumps[0].error > 4.5f, "receiver: knocked 5 m off -> bump reported");
    bool blended = false;
    for (const auto& c : cmds)
        if (c.type == GuestCmd::Type::Drive && c.vc.fix == net::motion::VehicleCommand::Fix::Blend &&
            c.vc.nudge.x < 0)
            blended = true;
    Check(blended, "receiver: 5 m off (inside the traffic snap distance) -> blended back");
    rep.pos = {12, 0, 0};
    g2.SetReport(1, rep, 10.54);
    g2.Tick(10.54, 0.016f, cmds, bumps);
    bool snapped = false;
    for (const auto& c : cmds)
        if (c.type == GuestCmd::Type::Drive && c.vc.fix == net::motion::VehicleCommand::Fix::Snap) snapped = true;
    Check(snapped, "receiver: knocked 10 m off -> put back (snap)");

    // despawn: removed after the grace time, not at once
    g2.OnDespawn(1, 10.6);
    g2.Tick(10.7, 0.016f, cmds, bumps);
    const bool early = Count(cmds, GuestCmd::Type::Remove) == 0 && Count(cmds, GuestCmd::Type::Drive) == 0;
    g2.Tick(11.7, 0.016f, cmds, bumps);
    Check(early && Count(cmds, GuestCmd::Type::Remove) == 1, "receiver: despawn after the grace time");

    // release: player sync took the car; never removed or driven by us
    GuestTraffic g3;
    g3.OnSpawn(Spawn(4, 10, 0), 0);
    g3.OnStates(0, {State(4, 10, 0, 0, 0)}, 0);
    g3.SetLocal({0, 0, 0}, true);
    g3.Tick(0.1, 0.016f, cmds, bumps);
    g3.Release(4);
    g3.Tick(0.2, 0.016f, cmds, bumps);
    const bool quiet = cmds.empty();
    g3.OnDespawn(4, 0.3);
    g3.Tick(2.0, 0.016f, cmds, bumps);
    Check(quiet && cmds.empty() && g3.Stats().known == 0, "receiver: a released copy is left to player sync");

    // stale: no state for 5 s -> removed
    GuestTraffic g4;
    g4.OnSpawn(Spawn(2, 10, 0), 0);
    g4.OnStates(0, {State(2, 10, 0, 0, 0)}, 0);
    g4.SetLocal({0, 0, 0}, true);
    g4.Tick(0.1, 0.016f, cmds, bumps);
    g4.Tick(6.0, 0.016f, cmds, bumps);
    Check(Count(cmds, GuestCmd::Type::Remove) == 1, "receiver: a car without states for 5 s loses its copy");
    // back to our own traffic: every copy goes
    g4.OnStates(100, {State(2, 10, 0, 0, 0)}, 6.1);
    g4.Tick(6.2, 0.016f, cmds, bumps);
    const bool readded = Count(cmds, GuestCmd::Type::Add) == 1;
    g4.SetLocal({1000, 0, 0}, true);
    g4.Tick(6.3, 0.016f, cmds, bumps);
    Check(readded && Count(cmds, GuestCmd::Type::Remove) == 1, "receiver: we drive off (290 m) -> its copy goes");

    // far copies are driven every third tick only
    GuestTraffic g5;
    g5.OnSpawn(Spawn(1, 20, 0), 0);
    g5.OnSpawn(Spawn(2, 200, 0), 0);
    g5.OnStates(0, {State(1, 20, 0, 0, 0), State(2, 200, 0, 0, 0)}, 0);
    g5.SetLocal({0, 0, 0}, true);
    size_t near = 0, far = 0;
    for (int i = 0; i < 9; ++i) {
        g5.Tick(0.1 + i * 0.016, 0.016f, cmds, bumps);
        for (const auto& c : cmds)
            if (c.type == GuestCmd::Type::Drive) (c.key == 1 ? near : far)++;
    }
    Check(near == 9 && far == 3, "receiver: far copies are driven every third tick", Fmt("near %.0f far %.0f", near, far));

    // a model change on a respawn: the old copy goes, a new one comes
    SpawnInfo other = Spawn(1, 20, 0);
    other.model = "bolt_ace";
    g5.OnSpawn(other, 0.5);
    g5.OnStates(600, {State(1, 20, 0, 0, 0)}, 0.6);
    g5.Tick(0.6, 0.016f, cmds, bumps);
    Check(Count(cmds, GuestCmd::Type::Remove) == 1 && Count(cmds, GuestCmd::Type::Add) == 1,
          "receiver: another model under the same net id -> respawned");
}

// ---- end to end: host -> bytes -> guest ----
// The copies' playout delay adapts (net/playout.h): 20 Hz host states over a quiet link (arrivals on the guest's
// 60 fps frames) settle well below the old fixed 120 ms; with 80 ms of jitter it grows to cover it.
void TestAdaptiveDelay() {
    for (int jitterMs : {0, 80}) {
        GuestTraffic g;
        g.OnSpawn(Spawn(1, 0, 0), 0);
        std::vector<GuestCmd> cmds;
        std::vector<GuestBump> bumps;
        uint32_t seed = 12345;
        auto rnd = [&seed] {
            seed = seed * 1664525u + 1013904223u;
            return static_cast<double>(seed >> 8) / 16777216.0;
        };
        struct P {
            double arrive;
            uint32_t ms;
        };
        std::vector<P> inflight;
        double nextSend = 0;
        int late = 0, total = 0;
        for (double t = 0; t < 8; t += 1 / 60.0) {
            while (nextSend <= t) {
                inflight.push_back({nextSend + 0.03 + rnd() * jitterMs / 1000.0, static_cast<uint32_t>(nextSend * 1000)});
                nextSend += 0.05;
            }
            const double local = 100 + t;
            for (size_t i = 0; i < inflight.size();) {
                if (inflight[i].arrive <= t) {
                    const float x = inflight[i].ms / 1000.0f * 10;
                    if (t > 3) {
                        ++total;
                        // arrived after the render time passed it: it was shown by extrapolation
                        if (inflight[i].ms / 1000.0 < local - (100 - 0.0) - g.PlayoutDelay() - 0.03) ++late;
                    }
                    g.OnStates(inflight[i].ms, {State(1, x, 0, 10, 0)}, local);
                    inflight.erase(inflight.begin() + static_cast<ptrdiff_t>(i));
                } else {
                    ++i;
                }
            }
            g.Tick(local, 1 / 60.0f, cmds, bumps);
        }
        const double d = g.PlayoutDelay() * 1000;
        if (jitterMs == 0)
            Check(d > 40 && d < 90, "receiver: adaptive delay at 20 Hz below the old 120 ms", Fmt("%.1f ms", d));
        else
            Check(d > 100 && d < 200, "receiver: adaptive delay covers 80 ms jitter", Fmt("%.1f ms", d));
    }
}

void TestEndToEnd() {
    HostTraffic h;
    GuestTraffic g;
    std::vector<HostCar> cars;
    for (uint32_t i = 0; i < 10; ++i) cars.push_back(Car(500 + i, 20.0f + i * 10, 0, 0, 8));
    std::vector<PeerOut> out;
    std::vector<GuestCmd> cmds;
    std::vector<GuestBump> bumps;
    double now = 0;
    float worst = 0;
    size_t copies = 0;
    for (int tick = 0; tick < 60; ++tick) {
        now = tick * 0.05;
        for (auto& c : cars) c.st.pos.y = static_cast<float>(8 * now);
        h.SetCars(cars);
        h.SetView(1, {0, 0, 5}, now);
        h.Tick(now, static_cast<uint32_t>(now * 1000), out);
        const double arrive = now + 0.04;  // 40 ms one way
        for (const auto& po : out) {
            for (const auto& b : po.spawns) {
                net::ByteReader r(b.data(), b.size());
                std::vector<SpawnInfo> s;
                if (ReadSpawns(r, s))
                    for (auto& x : s) g.OnSpawn(x, arrive);
            }
            for (const auto& b : po.states) {
                net::ByteReader r(b.data(), b.size());
                uint32_t ms = 0;
                std::vector<CarState> s;
                if (ReadStates(r, ms, s)) g.OnStates(ms, s, arrive);
            }
        }
        g.SetLocal({0, 0, 0}, true);
        g.Tick(arrive, 0.05f, cmds, bumps);
        copies = g.Stats().copies;
        if (tick > 10) {
            for (const auto& c : cars) {
                CarState t;
                if (g.Target(h.NetOf(c.key), arrive, t))
                    worst = std::max(worst, std::fabs(t.pos.y - static_cast<float>(8 * now)));  // minus the trip
            }
        }
    }
    Check(copies == 10, "end to end: every host car has a copy", Fmt("%.0f", copies));
    Check(worst < 0.15f, "end to end: target = the host's car one trip ago", Fmt("worst %.3f m", worst));
}

}  // namespace

namespace {

// ---- several guests: each guest his own interest set, net ids shared, a drop or a reconnect touches only him ----
void TestHostThreeGuests() {
    HostTraffic h;
    std::vector<HostCar> cars;
    for (uint32_t i = 0; i < 40; ++i) cars.push_back(Car(2000 + i, i * 12.0f, 0, 0, 8));  // 0 .. 468 m, moving
    std::vector<PeerOut> out;
    HostRun r1, r2, r3;
    auto tick = [&](double t, bool g1 = true, bool g2 = true, float g3x = 5000) {
        for (auto& c : cars) c.st.pos.y += 0.4f;
        h.SetCars(cars);
        if (g1) h.SetView(1, {0, 0, 5}, t);
        if (g2) h.SetView(2, {300, 0, 5}, t);
        h.SetView(3, {g3x, 0, 5}, t);
        h.Tick(t, static_cast<uint32_t>(t * 1000), out);
        r1.Apply(out, 1);
        r2.Apply(out, 2);
        r3.Apply(out, 3);
    };
    double t = 0;
    for (int i = 0; i < 20; ++i) tick(t += 0.05);
    Check(r1.live.size() == 22 && r2.live.size() == 24 && r3.live.empty(),
          "3 guests: each gets the cars near him (guest 3 far away: none)",
          Fmt("%.0f / %.0f / %.0f", r1.live.size(), r2.live.size(), r3.live.size()));
    size_t shared = 0;
    for (uint16_t n : r1.live) shared += r2.live.count(n);
    Check(shared > 0 && h.NetOf(2015) != 0 && r1.live.count(h.NetOf(2015)) && r2.live.count(h.NetOf(2015)),
          "3 guests: a car near two guests has one net id for both", Fmt("%.0f shared", shared));
    const auto* s1 = h.Stats(1);
    const auto* s2 = h.Stats(2);
    const auto* s3 = h.Stats(3);
    Check(s1 && s2 && s3 && s1->bytes > 0 && s2->bytes > 0 && s3->bytes == 0 && s1->states > 0,
          "3 guests: per-guest byte totals", Fmt("%.0f / %.0f bytes", s1 ? s1->bytes : 0, s2 ? s2->bytes : 0));
    // guest 1 loses his connection (OnPlayerDropped -> RemovePeer): guest 2 sees no change
    const uint32_t d2 = r2.despawns, sp2 = r2.spawns;
    h.RemovePeer(1);
    for (int i = 0; i < 10; ++i) tick(t += 0.05, false);
    Check(!h.Stats(1) && r2.despawns == d2 && r2.spawns == sp2 && r2.live.size() == 24,
          "3 guests: one drops, the others' cars stay as they were");
    // he is back: a fresh peer, all his cars spawned again (he removed his copies when the connection went)
    HostRun back;
    r1 = back;
    for (int i = 0; i < 10; ++i) tick(t += 0.05);
    Check(r1.live.size() == 22 && r1.spawns == 22, "3 guests: back after a reconnect, his cars are spawned anew",
          Fmt("%.0f live, %.0f spawns", r1.live.size(), r1.spawns));
    // guest 3 drives over to the others: he gets cars too, nobody else loses any
    const uint32_t d1 = r1.despawns;
    for (int i = 0; i < 10; ++i) tick(t += 0.05, true, true, 150);
    Check(!r3.live.empty() && r1.despawns == d1 && r2.live.size() == 24, "3 guests: a guest who comes near gets cars too",
          Fmt("%.0f", r3.live.size()));
    // with 4 players (3 guests) the state budget is per guest: each stays within it
    const double secs = t;
    bool within = true;
    for (uint32_t p = 1; p <= 3; ++p)
        if (const auto* s = h.Stats(p)) within &= s->bytes / secs <= HostConfig{}.bytesPerSecond * 1.5;
    Check(within, "3 guests: every guest within his own state budget");
}

float Dist3(const Vec3& a, const Vec3& b) {
    return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z));
}

// The host pauses his game (game/peer_pause.h; the 2026-10-19 two-PC test: 6-10 "knocked ... put back, reported to the
// host" within 0.5 s after each of his pauses). A car at 10 m/s; 2 s of states, an 8 s pause without states, then
// states again from where it stopped. The copy follows its target (reports on target) except where it is pushed.
void TestHostPause() {
    GuestTraffic g;
    std::vector<GuestCmd> cmds;
    std::vector<GuestBump> bumps;
    g.OnSpawn(Spawn(1, 0, 0), 10.0);
    g.SetLocal({0, 0, 0}, true);
    CopyReport rep;
    auto onTarget = [&](double now, float push) {
        CarState t;
        if (!g.Target(1, now, t)) return;
        rep.pos = {t.pos.x + push, t.pos.y, t.pos.z};
        for (int k = 0; k < 4; ++k) rep.rot[k] = t.rot[k];
        g.SetReport(1, rep, now);
    };
    size_t bumpsSeen = 0;
    double t = 10.0;
    for (; t < 12.0; t += 0.05) {
        g.OnStates(static_cast<uint32_t>((t - 10.0) * 1000), {State(1, 10 * static_cast<float>(t - 10.0), 0, 10, 0)},
                   t);
        onTarget(t, 0);
        g.Tick(t, 0.05f, cmds, bumps);
        bumpsSeen += bumps.size();
    }
    Check(bumpsSeen == 0, "pause: driven on target before the pause");
    const float stopX = 10 * static_cast<float>(t - 0.05 - 10.0);

    // paused: no states for 8 s
    g.SetHostPaused(true, t);
    bool allPinned = true, stillHeld = true, anyDrive = false;
    size_t removes = 0;
    Vec3 held{};
    bool haveHeld = false;
    for (; t < 20.0; t += 0.05) {
        g.Tick(t, 0.05f, cmds, bumps);
        bumpsSeen += bumps.size();
        removes += Count(cmds, GuestCmd::Type::Remove);
        for (const auto& c : cmds) {
            if (c.type != GuestCmd::Type::Drive) continue;
            anyDrive = true;
            if (c.vc.fix != net::motion::VehicleCommand::Fix::Pin || c.target.vel.x != 0 || c.target.power != 0 ||
                c.target.brake != 1 || !(c.target.flags & kHandbrake))
                allPinned = false;
            if (!haveHeld) {
                held = c.target.pos;
                haveHeld = true;
            } else if (Dist3(held, c.target.pos) > 1e-4f) {
                stillHeld = false;
            }
            // the copy obeys, except for one push of 5 m in the middle of the pause
            rep.pos = c.target.pos;
            if (t > 15 && t < 15.06) rep.pos.x += 5;
            g.SetReport(1, rep, t);
        }
    }
    Check(anyDrive && allPinned, "pause: the copy is pinned, braked, without speed");
    Check(stillHeld && haveHeld && std::fabs(held.x - stopX) < 3.5f, "pause: held where it stood",
          Fmt("held %.2f, host stopped at %.2f", held.x, stopX));
    Check(removes == 0, "pause: 8 s without states, the copy is not stale");
    Check(bumpsSeen == 0, "pause: a push while paused is no knock report");
    Check(g.HostPaused() && g.Stats().frozen, "pause: stats say frozen");

    // resumed: the host's car goes on from where it stood (his clock ran on: host time + 8 s)
    g.SetHostPaused(false, t);
    const double resumeAt = t;
    const double hostT0 = 2.0 + (t - 12.0);
    const uint32_t quietBefore = g.Stats().quietBumps;
    bool knockedQuiet = false, reportedEarly = false;
    for (; t < resumeAt + 3.0; t += 0.05) {
        const double since = t - resumeAt;
        g.OnStates(static_cast<uint32_t>((hostT0 + since) * 1000),
                   {State(1, stopX + 10 * static_cast<float>(since), 0, 10, 0)}, t);
        // on target, but knocked 5 m in the first second (what the resume looked like in the test), and 2.8 s on
        // (2 s after the first: one report per car every 2 s)
        const bool knock = (since > 0.6 && since < 0.66) || (since > 2.8 && since < 2.86);
        onTarget(t, knock ? 5.0f : 0.0f);
        g.Tick(t, 0.05f, cmds, bumps);
        if (since < 1.0) {
            if (g.Stats().quietBumps > quietBefore) knockedQuiet = true;
            if (!bumps.empty()) reportedEarly = true;
        } else {
            bumpsSeen += bumps.size();
        }
    }
    Check(!reportedEarly, "resume: no knock reported in the first second");
    Check(knockedQuiet, "resume: the knock in the first second is counted as quiet");
    Check(bumpsSeen == 1, "resume: a knock after the grace is reported again", Fmt("%.0f", bumpsSeen));
    Check(!g.HostPaused() && !g.Stats().frozen, "resume: not frozen");

    // our own pause (a 1 s gap between our ticks): the copies were left behind, no knock reports for 1 s
    GuestTraffic l;
    l.OnSpawn(Spawn(1, 0, 0), 10.0);
    l.SetLocal({0, 0, 0}, true);
    CopyReport lr;
    auto lOn = [&](double now, float push) {
        CarState tt;
        if (!l.Target(1, now, tt)) return;
        lr.pos = {tt.pos.x + push, tt.pos.y, tt.pos.z};
        for (int k = 0; k < 4; ++k) lr.rot[k] = tt.rot[k];
        l.SetReport(1, lr, now);
    };
    size_t lb = 0;
    for (double u = 10.0; u < 14.0; u += 0.05) {
        l.OnStates(static_cast<uint32_t>((u - 10.0) * 1000), {State(1, 10 * static_cast<float>(u - 10.0), 0, 10, 0)},
                   u);
        if (u > 12.0 && u < 13.0) continue;  // our game stood: no tick, no report
        lOn(u, (u > 13.2 && u < 13.26) ? 5.0f : 0.0f);
        l.Tick(u, 0.05f, cmds, bumps);
        lb += bumps.size();
    }
    Check(l.Stats().localGaps == 1, "local pause: the tick gap is seen");
    Check(lb == 0 && l.Stats().quietBumps >= 1, "local pause: the knock right after it is not reported",
          Fmt("bumps %.0f quiet %.0f", lb, l.Stats().quietBumps));
}

// The host's "no driver" decision: a car only counts as driverless after 1 s without a driver read while slow.
void TestDriverWatch() {
    {
        DriverWatch w;  // a flickering read (the 0.15.0 test: cars flagged while driven on)
        bool any = false;
        for (int i = 0; i < 200; ++i) {
            const double t = i * 0.05;
            const bool seen = (i % 30) >= 15;  // 0.7 s missing, then read, ...
            any |= w.Update(t, seen, 0.0f);
        }
        Check(!any, "driver: missing 0.7 s at a time (flicker) is never driverless");
    }
    {
        DriverWatch w;  // no driver read, but the car drives on at 10 m/s (someone we cannot read drives it)
        bool any = false;
        for (int i = 0; i < 200; ++i) any |= w.Update(i * 0.05, false, 10.0f);
        Check(!any && w.Missing(), "driver: a moving car without a driver read is not driverless (10 s at 10 m/s)");
    }
    {
        DriverWatch w;  // a stopped car whose driver got out
        bool at09 = false, at10 = false;
        for (int i = 0; i <= 20; ++i) {
            const double t = i * 0.05;
            const bool d = w.Update(t, false, 0.3f);
            if (i == 18) at09 = d;
            if (i == 20) at10 = d;
        }
        Check(!at09 && at10, "driver: standing without a driver: driverless after 1.0 s, not at 0.9 s");
        Check(w.Update(1.05, false, 3.0f), "driver: pushed to 3 m/s (below 'driven again', 4 m/s): still driverless");
        Check(!w.Update(1.1, false, 6.0f), "driver: driverless car speeding up (6 m/s) is driven again");
        bool early = false;
        for (int i = 0; i <= 18; ++i) early |= w.Update(1.15 + i * 0.05, false, 0.0f);  // rounds 0.05 s apart
        Check(!early, "driver: ... and needs 1 s slow again");
        Check(w.Update(2.2, false, 0.0f), "driver: ... after which it is driverless again");
        Check(!w.Update(2.25, true, 0.0f) && !w.Missing(), "driver: a driver read clears it at once");
    }
    {
        DriverWatch w;  // the host's game pauses (no rounds for 8 s) while a slow car's driver is not read
        bool before = false;
        for (int i = 0; i <= 10; ++i) before |= w.Update(i * 0.05, false, 0.0f);  // 0.5 s missing
        const bool afterGap = w.Update(8.5, false, 0.0f);
        bool soon = false;
        for (int i = 1; i <= 18; ++i) soon |= w.Update(8.5 + i * 0.05, false, 0.0f);
        bool later = false;
        for (int i = 19; i <= 22; ++i) later |= w.Update(8.5 + i * 0.05, false, 0.0f);
        Check(!before && !afterGap && !soon, "driver: the host's pause is not time without a driver (watch restarts)");
        Check(later, "driver: ... driverless 1 s after the resume");
        DriverWatch d;  // a car declared before the pause stays declared over it
        for (int i = 0; i <= 22; ++i) d.Update(i * 0.05, false, 0.0f);
        Check(d.Declared() && d.Update(9.0, false, 0.0f), "driver: declared before the pause: still after it");
    }
    {
        DriverWatch w;  // the driver dies at 12 m/s; the car rolls to a stop over 4 s
        double declaredAt = -1;
        for (int i = 0; i <= 120; ++i) {
            const double t = i * 0.05;
            const float v = std::max(0.0f, 12.0f - 3.0f * static_cast<float>(t));
            if (w.Update(t, false, v) && declaredAt < 0) declaredAt = t;
        }
        // slow (<= 2 m/s) from t = 3.33: driverless 1 s later
        Check(declaredAt > 4.3 && declaredAt < 4.45, "driver: a rolling car is driverless 1 s after it got slow",
              Fmt("at %.2f s", declaredAt));
    }
    Check(!CrewGetsOut(kNoDriver, 8.0f, 0.0f), "crew: host car driverless but rolling at 8 m/s: crew stays in");
    Check(!CrewGetsOut(kNoDriver, 0.5f, 6.0f), "crew: our copy still moving (6 m/s): crew stays in");
    Check(!CrewGetsOut(kControls, 0.0f, 0.0f), "crew: no driverless flag: crew stays in");
    Check(CrewGetsOut(kNoDriver, 0.5f, 0.4f), "crew: both stand, host car driverless: the driver gets out");
    Check(CrewGetsOut(kNoDriver, 0.0f, -1.0f), "crew: no report of the copy yet, host car stands: out");
}

// End to end: the host's raw read becomes the sent kNoDriver only once confirmed; the guest's Drive command carries
// kCrewOut only while both cars stand.
void TestOrphanEndToEnd() {
    HostTraffic h;
    GuestTraffic g;
    std::vector<PeerOut> out;
    std::vector<GuestCmd> cmds;
    std::vector<GuestBump> bumps;
    HostCar car = Car(4242, 20, 0, 0, 10);
    uint8_t lastSent = 0;
    bool sentNoDriverMoving = false;
    double sentAt = -1;
    const double base = 100.0;
    for (int i = 0; i <= 120; ++i) {
        const double t = i * 0.05;
        // driver read flickers for the first 2 s (moving at 10 m/s), then it is gone and the car brakes to a stop
        const bool read = t < 2.0 ? (i % 10) < 5 : false;
        const float v = t < 2.0 ? 10.0f : std::max(0.0f, 10.0f - 5.0f * static_cast<float>(t - 2.0));
        car.st.vel = {0, v, 0};
        car.st.pos.y += v * 0.05f;
        car.st.flags = static_cast<uint8_t>(kControls | (read ? 0 : kNoDriver));
        h.SetCars({car}, t);
        h.SetView(1, {0, 0, 5}, t);
        h.Tick(t, static_cast<uint32_t>(t * 1000), out);
        for (const auto& po : out) {
            for (const auto& b : po.spawns) {
                net::ByteReader r(b.data(), b.size());
                std::vector<SpawnInfo> s;
                ReadSpawns(r, s);
                for (const auto& x : s) g.OnSpawn(x, base + t);
            }
            for (const auto& b : po.states) {
                net::ByteReader r(b.data(), b.size());
                uint32_t ms = 0;
                std::vector<CarState> st;
                ReadStates(r, ms, st);
                for (const auto& s : st) {
                    lastSent = s.flags;
                    if ((s.flags & kNoDriver) && v > kOrphanSlow) sentNoDriverMoving = true;
                    if ((s.flags & kNoDriver) && sentAt < 0) sentAt = t;
                }
                g.OnStates(ms, st, base + t);
            }
        }
        g.SetLocal({0, 0, 0}, true);
        g.Tick(base + t, 0.05f, cmds, bumps);
    }
    Check(!sentNoDriverMoving, "orphan: never sent as driverless while it moves");
    // slow (<= 2 m/s) from t = 3.6: driverless from 4.6
    Check(sentAt > 4.5 && sentAt < 4.75 && (lastSent & kNoDriver), "orphan: sent as driverless 1 s after it stopped",
          Fmt("at %.2f s", sentAt));
    Check(h.Orphaned() == 1 && h.DeclaredNow() == 1, "orphan: counted once", Fmt("%.0f", h.Orphaned()));
    // the guest: a copy that stands gets kCrewOut, one that still moves (our report) does not
    const double now = base + 6.02;
    CopyReport rep;
    CarState tgt;
    g.Target(h.NetOf(4242), now, tgt);
    rep.pos = tgt.pos;
    rep.vel = {0, 5, 0};
    g.SetReport(h.NetOf(4242), rep, now);
    g.Tick(now + 0.02, 0.016f, cmds, bumps);
    bool out1 = false, drove = false;
    for (const auto& c : cmds)
        if (c.type == GuestCmd::Type::Drive) drove = true, out1 |= (c.target.flags & kCrewOut) != 0;
    Check(drove && !out1, "orphan: our copy still rolling at 5 m/s: no kCrewOut");
    rep.vel = {0, 0, 0};
    g.SetReport(h.NetOf(4242), rep, now + 0.04);
    g.Tick(now + 0.04, 0.016f, cmds, bumps);
    bool out2 = false;
    for (const auto& c : cmds)
        if (c.type == GuestCmd::Type::Drive) out2 |= (c.target.flags & kCrewOut) != 0;
    Check(out2, "orphan: both stand: kCrewOut in the Drive command");
}

}  // namespace

namespace {

// ---- the receiver's target through core's replica::Stream equals the 0.15 interpolation ----------------------------
// RefPresent is GuestTraffic::Present of 0.15 (snapshots in a deque, the playout clock, Hermite, the lead, the
// driver's controls of the later state, the flags of the newest), copied here as the reference.
struct RefTraffic {
    struct Snap {
        double t;
        CarState s;
    };
    std::deque<Snap> snaps;
    net::playout::PlayoutClock clock;
    int64_t clockMs = 0;
    uint32_t lastRaw = 0;
    bool have = false;
    static net::playout::PlayoutTuning Tuning() {
        net::playout::PlayoutTuning t;
        t.startDelay = 0.12;
        return t;
    }
    RefTraffic() : clock(Tuning()) {}
    void OnStates(uint32_t ms, const CarState& s, double now) {
        if (!have) {
            clockMs = ms;
            have = true;
        } else {
            clockMs += static_cast<int32_t>(ms - lastRaw);
        }
        lastRaw = ms;
        const double t = static_cast<double>(clockMs) / 1000.0;
        clock.Observe(t, now);
        if (!snaps.empty() && t <= snaps.back().t) return;
        snaps.push_back({t, s});
        while (snaps.size() > 8) snaps.pop_front();
    }
    bool Present(double now, CarState& out) const {
        const double rt = clock.RenderTime(now);
        const Snap* later = &snaps.back();
        if (rt <= snaps.front().t) {
            out = snaps.front().s;
            later = &snaps.front();
        } else if (rt >= snaps.back().t) {
            out = snaps.back().s;
            const float k = static_cast<float>(std::min(rt - snaps.back().t, 0.5));
            out.pos.x += out.vel.x * k;
            out.pos.y += out.vel.y * k;
            out.pos.z += out.vel.z * k;
        } else {
            for (size_t i = 0; i + 1 < snaps.size(); ++i) {
                const Snap& a = snaps[i];
                const Snap& b = snaps[i + 1];
                if (rt < a.t || rt >= b.t) continue;
                later = &b;
                const float dx = a.s.pos.x - b.s.pos.x, dy = a.s.pos.y - b.s.pos.y, dz = a.s.pos.z - b.s.pos.z;
                if (std::sqrt(dx * dx + dy * dy + dz * dz) > 25.0f) {
                    out = rt - a.t < (b.t - a.t) * 0.5 ? a.s : b.s;
                    break;
                }
                const float u = static_cast<float>((rt - a.t) / std::max(1e-6, b.t - a.t));
                out = b.s;
                auto lerp = [u](float x, float y) { return x + (y - x) * u; };
                out.pos = net::playout::Hermite(a.s.pos, a.s.vel, b.s.pos, b.s.vel, b.t - a.t, u);
                out.vel = {lerp(a.s.vel.x, b.s.vel.x), lerp(a.s.vel.y, b.s.vel.y), lerp(a.s.vel.z, b.s.vel.z)};
                net::motion::Nlerp(a.s.rot, b.s.rot, u, out.rot);
                break;
            }
        }
        const float lead = static_cast<float>(std::max(0.0, clock.Delay()));
        Vec3 d = {out.vel.x * lead, out.vel.y * lead, out.vel.z * lead};
        const float len = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
        const float k = len > 2.0f ? 2.0f / len : 1.0f;
        out.pos.x += d.x * k;
        out.pos.y += d.y * k;
        out.pos.z += d.z * k;
        out.flags = snaps.back().s.flags;
        out.steer = later->s.steer;
        out.power = later->s.power;
        out.brake = later->s.brake;
        return true;
    }
};

void TestMatches015() {
    Rng rng(99);
    GuestConfig cfg;
    cfg.maxLead = 2.0f;  // the 0.15 lead limit (9 m now, TestRecordedPath)
    ReceiverTraffic g(cfg);
    RefTraffic ref;
    const double base = 50.0;
    g.OnSpawn(Spawn(7, 0, 0), base);
    std::vector<GuestCmd> cmds;
    std::vector<GuestBump> bumps;
    struct InFlight {
        double arrive;
        uint32_t ms;
        CarState s;
    };
    std::vector<InFlight> fly;
    CarState truth = State(7, 0, 0, 10, 0);
    float yaw = 0;
    double nextSend = 0;
    float worstP = 0, worstV = 0, worstR = 0, worstC = 0;
    int flagsDiff = 0, compared = 0;
    for (double t = 0; t < 30; t += 1 / 60.0) {
        // the authority's car: speed and heading change, a jump now and then (a teleport), controls change
        while (nextSend <= t) {
            truth.vel.x = 10 * std::cos(yaw) + rng.Uniform(-1, 1);
            truth.vel.y = 10 * std::sin(yaw) + rng.Uniform(-1, 1);
            yaw += rng.Uniform(-0.2f, 0.2f);
            truth.pos.x += truth.vel.x * 0.05f;
            truth.pos.y += truth.vel.y * 0.05f;
            if (rng.Uniform(0, 1) < 0.01f) truth.pos.x += 40;
            YawQuat(yaw, truth.rot);
            truth.steer = rng.Uniform(-1, 1);
            truth.power = rng.Uniform(0, 1);
            truth.brake = rng.Uniform(0, 1);
            truth.flags = static_cast<uint8_t>(kControls | (rng.Uniform(0, 1) < 0.3f ? kHandbrake : 0));
            const CarState q = Quantised(truth);
            fly.push_back({nextSend + 0.02 + rng.Uniform(0, 0.06f), static_cast<uint32_t>(nextSend * 1000) + 777, q});
            nextSend += 0.05;
        }
        for (size_t i = 0; i < fly.size();) {
            if (fly[i].arrive <= t) {
                g.OnStates(fly[i].ms, {fly[i].s}, base + t);
                ref.OnStates(fly[i].ms, fly[i].s, base + t);
                fly.erase(fly.begin() + static_cast<std::ptrdiff_t>(i));
            } else {
                ++i;
            }
        }
        g.SetLocal({0, 0, 0}, false);
        g.Tick(base + t, 1 / 60.0f, cmds, bumps);  // the stream's render clock (as 0.15 Tick did)
        ref.clock.Update(base + t);
        if (ref.snaps.empty()) continue;
        CarState a, b;
        if (!g.Target(7, base + t, a) || !ref.Present(base + t, b)) continue;
        ++compared;
        worstP = std::max(worstP, Dist3(a.pos, b.pos));
        worstV = std::max(worstV, Dist3(a.vel, b.vel));
        worstR = std::max(worstR, QuatAngleDeg(a.rot, b.rot));
        worstC = std::max({worstC, std::fabs(a.steer - b.steer), std::fabs(a.power - b.power), std::fabs(a.brake - b.brake)});
        flagsDiff += a.flags != b.flags;
    }
    Check(compared > 1500 && worstP < 1e-3f && worstV < 1e-3f && worstR < 0.2f && worstC < 1e-5f && flagsDiff == 0,
          "replica: the target equals the 0.15 interpolation",
          Fmt("%.0f ticks, pos %.2g m, vel %.2g", compared, worstP, worstV) +
              Fmt(", rot %.2g deg, controls %.2g, flags %.0f", worstR, worstC, flagsDiff));
}

// ---- regions: whose traffic runs where ------------------------------------------------------------------------------
using net::authority::PlayerSpot;

void TestRegions() {
    auto spots = [](std::initializer_list<PlayerSpot> l) { return std::vector<PlayerSpot>(l); };
    RegionFollow g;  // guest 1
    double t = 0;
    g.Update(1, 0, spots({{0, {300, 0, 0}, true}, {1, {0, 0, 0}, true}}), t);
    Check(g.Own() && g.Authority() == 1, "region: host 300 m away -> our own traffic (join at 250)");
    const bool changed = g.Update(1, 0, spots({{0, {240, 0, 0}, true}, {1, {0, 0, 0}, true}}), t += 1);
    Check(changed && !g.Own() && g.Authority() == 0 && g.Since(t) == 0, "region: host at 240 m -> his region");
    g.Update(1, 0, spots({{0, {390, 0, 0}, true}, {1, {0, 0, 0}, true}}), t += 1);
    Check(g.Authority() == 0, "region: host at 390 m -> still his (leave at 400)");
    g.Update(1, 0, spots({{0, {420, 0, 0}, true}, {1, {0, 0, 0}, true}}), t += 1);
    Check(g.Own(), "region: host at 420 m -> our own again");
    g.Update(1, 0, spots({{0, {100, 0, 0}, true}, {1, {0, 0, 0}, false}}), t += 1);
    Check(g.Own(), "region: we are not in the world (loading): we keep what we had");
    g.Update(1, 0, spots({{0, {100, 0, 0}, true}, {1, {0, 0, 0}, true}}), t += 1);
    Check(g.Authority() == 0, "region: back in the world near the host -> his");
    RegionFollow h;  // the host
    h.Update(0, 0, spots({{0, {0, 0, 0}, true}, {1, {10, 0, 0}, true}}), 0);
    Check(h.Own(), "region: the host's game always runs its own traffic");

    // two guests together far from the host: the lower id's region
    RegionFollow b, c;
    auto all = [&](Vec3 host, Vec3 pb, Vec3 pc) {
        return spots({{0, host, true}, {1, pb, true}, {2, pc, true}});
    };
    b.Update(1, 0, all({0, 0, 0}, {5000, 0, 0}, {5100, 0, 0}), 10);
    c.Update(2, 0, all({0, 0, 0}, {5000, 0, 0}, {5100, 0, 0}), 10);
    Check(b.Own() && !c.Own() && c.Authority() == 1, "region: two guests far from the host -> the lower id's");
    // guest 2 walks away from guest 1: his own beyond 400 m
    c.Update(2, 0, all({0, 0, 0}, {5000, 0, 0}, {5350, 0, 0}), 11);
    Check(c.Authority() == 1, "region: 350 m from guest 1 -> still his");
    c.Update(2, 0, all({0, 0, 0}, {5000, 0, 0}, {5450, 0, 0}), 12);
    Check(c.Own(), "region: 450 m from guest 1 -> his own");
    // the host arrives: both in the host's region
    c.Update(2, 0, all({0, 0, 0}, {5000, 0, 0}, {5100, 0, 0}), 13);
    b.Update(1, 0, all({4900, 0, 0}, {5000, 0, 0}, {5100, 0, 0}), 14);
    c.Update(2, 0, all({4900, 0, 0}, {5000, 0, 0}, {5100, 0, 0}), 14);
    Check(b.Authority() == 0 && c.Authority() == 0, "region: the host arrives -> both in his region",
          Fmt("guest 1: %.0f, guest 2: %.0f", b.Authority(), c.Authority()));
    // the host out of the world (a loading screen) while we follow him: our own, as the 0.15 guest did without him
    b.Update(1, 0, spots({{0, {4900, 0, 0}, false}, {1, {5000, 0, 0}, true}}), 15);
    Check(b.Own(), "region: the host not in the world -> our own");
}

// ---- a guest's traffic: guest 1 (authority) -> the host relays -> guest 2; and the host himself ----------------------
// Core's relay delivers a guest -> guest message with the real sender; the body carries the authority (must be it).
struct Delivery {
    ReceiverTraffic* recv;
    uint32_t spawns = 0, despawns = 0;
    size_t bytes = 0;
    bool refused = false;
    void Apply(const PeerOut& po, uint8_t from, bool v2, double now) {
        auto prefix = [&](net::ByteReader& r) {
            if (!v2) return true;
            const uint8_t a = r.U8();
            if (a != from) refused = true;
            return a == from;
        };
        for (const auto& b0 : po.spawns) {
            const auto b = v2 ? WithAuthority(from, b0) : b0;
            bytes += b.size();
            net::ByteReader r(b.data(), b.size());
            std::vector<SpawnInfo> s;
            if (prefix(r) && ReadSpawns(r, s))
                for (auto& x : s) recv->OnSpawn(from, x, now), ++spawns;
        }
        for (const auto& b0 : po.despawns) {
            const auto b = v2 ? WithAuthority(from, b0) : b0;
            bytes += b.size();
            net::ByteReader r(b.data(), b.size());
            std::vector<uint16_t> n;
            if (prefix(r) && ReadDespawns(r, n))
                for (auto x : n) recv->OnDespawn(from, x, now), ++despawns;
        }
        for (const auto& b0 : po.states) {
            const auto b = v2 ? WithAuthority(from, b0) : b0;
            bytes += b.size();
            net::ByteReader r(b.data(), b.size());
            uint32_t ms = 0;
            std::vector<CarState> st;
            if (prefix(r) && ReadStates(r, ms, st)) recv->OnStates(from, ms, st, now);
        }
    }
};

void TestGuestAuthorityRelay() {
    AuthorityTraffic b1;                 // guest 1's own traffic
    ReceiverTraffic host, g2;            // the host's and guest 2's copies
    Delivery toHost{&host}, toG2{&g2};
    std::vector<HostCar> cars;
    for (uint32_t i = 0; i < 12; ++i) cars.push_back(Car(9000 + i, 5000.0f + i * 15.0f, 0, 0, 6));  // 5000 .. 5165
    std::vector<PeerOut> out;
    std::vector<GuestCmd> cmds;
    std::vector<GuestBump> bumps;
    Vec3 hostAt{0, 0, 5}, g2At{5050, 0, 5};
    std::set<uint32_t> hostKeys, g2Keys;
    auto step = [&](double t) {
        for (auto& c : cars) c.st.pos.y += 0.3f;
        b1.SetCars(cars, t);
        b1.SetView(0, hostAt, t);
        b1.SetView(2, g2At, t);
        b1.Tick(t, static_cast<uint32_t>(t * 1000), out);
        for (const auto& po : out) (po.peer == 0 ? toHost : toG2).Apply(po, 1, true, t + 0.03);
        host.SetLocal(hostAt);
        g2.SetLocal(g2At);
        for (auto* r : {&host, &g2}) {
            r->Tick(t + 0.03, 0.05f, cmds, bumps);
            for (const auto& c : cmds) {
                if (c.type == GuestCmd::Type::Add) (r == &host ? hostKeys : g2Keys).insert(c.key);
                if (c.type == GuestCmd::Type::Remove) (r == &host ? hostKeys : g2Keys).erase(c.key);
            }
        }
    };
    double t = 0;
    for (int i = 0; i < 20; ++i) step(t += 0.05);
    bool allOf1 = !g2Keys.empty();
    for (uint32_t k : g2Keys) allOf1 &= RefAuthority(k) == 1 && b1.KeyOf(RefNet(k)) >= 9000;
    Check(g2Keys.size() == 12 && allOf1 && !toG2.refused, "relay: guest 2 shows guest 1's cars (authority 1)",
          Fmt("%.0f copies", g2Keys.size()));
    Check(hostKeys.empty() && toHost.spawns == 0, "relay: the host 5 km away gets none of them");
    // the host comes near: he gets guest 1's cars too, under the same keys as guest 2
    hostAt = {5100, 30, 5};
    for (int i = 0; i < 20; ++i) step(t += 0.05);
    size_t same = 0;
    for (uint32_t k : hostKeys) same += g2Keys.count(k);
    Check(hostKeys.size() == 12 && same == 12, "relay: the host near: the same cars, the same keys",
          Fmt("%.0f / %.0f", hostKeys.size(), same));
    // guest 2 drives off: only he loses them (despawned, removed after the grace)
    g2At = {9000, 0, 5};
    for (int i = 0; i < 40; ++i) step(t += 0.05);
    Check(g2Keys.empty() && toG2.despawns == 12 && hostKeys.size() == 12, "relay: guest 2 away: his copies go, the host's stay",
          Fmt("guest 2 %.0f, host %.0f", g2Keys.size(), hostKeys.size()));
    // budget per receiver (the relay costs guest 1's relay budget, the states his per-player budget)
    const double kbps = static_cast<double>(toHost.bytes) / t / 1024.0;
    Check(kbps < HostConfig{}.bytesPerSecond / 1024.0, "relay: within the per-player budget", Fmt("%.1f KB/s", kbps));
    // guest 1 leaves the session: his copies go everywhere
    host.RemoveAuthority(1);
    host.Tick(t + 1, 0.05f, cmds, bumps);
    Check(Count(cmds, GuestCmd::Type::Remove) == 12 && host.Stats().known == 0, "relay: guest 1 leaves -> his copies go");
    // a body whose authority is not its sender is refused (a guest cannot send as another)
    Delivery liar{&g2};
    PeerOut po;
    po.spawns.push_back(PackSpawns({Spawn(5, 0, 0)}));
    liar.Apply(po, 1, true, 0);  // WithAuthority(1) from 1: fine
    std::vector<uint8_t> forged = WithAuthority(3, po.spawns[0]);
    net::ByteReader fr(forged.data(), forged.size());
    Check(!liar.refused && fr.U8() == 3, "relay: the authority byte is the first byte (checked against the sender)");
}

// ---- hand-over: players converge and part; no pops ----------------------------------------------------------------------
// Two games, the host (0) and guest 1, each a small traffic generator (cars around its player while its own spawning
// runs; off in another's region), an AuthorityTraffic for its cars, a ReceiverTraffic for the other's, a RegionFollow;
// copies report exactly at their target (the Lua side). Guest 1 stands; the host drives to him, stays, drives away.
struct World {
    struct SimCar {
        uint32_t ent;
        Vec3 pos, vel;
        bool hidden = false;
    };
    uint8_t id;
    Vec3 me;
    std::vector<SimCar> cars;
    uint32_t nextEnt;
    AuthorityTraffic auth;
    ReceiverTraffic recv;
    RegionFollow region;
    std::map<uint32_t, uint16_t> netOf;    // entity -> the net id it had (a car keeps one for its life)
    std::set<uint32_t> copies;             // our copies of the other's cars (keys)
    uint32_t netChanged = 0, popped = 0;   // a car's net id changed while it lived / a copy removed in view, car alive
    Rng rng;
    World(uint8_t i, Vec3 at) : id(i), me(at), nextEnt(i * 100000u + 1), rng(7 + i) {}
    void Generate(bool on, double offFor, const Vec3& other) {
        for (auto& c : cars) {
            c.pos.x += c.vel.x * 0.05f;
            c.pos.y += c.vel.y * 0.05f;
        }
        // the game: cars far from us go; with our spawning on new ones come at 100-150 m (out of view)
        cars.erase(std::remove_if(cars.begin(), cars.end(),
                                  [&](const SimCar& c) { return Dist3(c.pos, me) > 220 || c.hidden; }),
                   cars.end());
        if (on)
            while (cars.size() < 14) {
                const float a = rng.Uniform(0, 6.28f), r = rng.Uniform(100, 150), h = rng.Uniform(0, 6.28f);
                cars.push_back({nextEnt++, {me.x + r * std::cos(a), me.y + r * std::sin(a), 0},
                                {8 * std::cos(h), 8 * std::sin(h), 0}});
            }
        else if (offFor >= 6)  // in another's region: ours are hidden 6 s on (CLEAR_WAIT), once out of everyone's
            for (auto& c : cars)  // sight (70 m from us and from him)
                if (Dist3(c.pos, me) > 70 && Dist3(c.pos, other) > 70) c.hidden = true;
        cars.erase(std::remove_if(cars.begin(), cars.end(), [](const SimCar& c) { return c.hidden; }), cars.end());
    }
    std::vector<HostCar> Report() const {
        std::vector<HostCar> v;
        for (const auto& c : cars) {
            HostCar h = Car(c.ent, c.pos.x, c.pos.y, c.vel.x, c.vel.y);
            h.st.pos.z = 0;
            v.push_back(h);
        }
        return v;
    }
    const SimCar* Find(uint32_t ent) const {
        for (const auto& c : cars)
            if (c.ent == ent) return &c;
        return nullptr;
    }
};

void TestHandOver() {
    World host(0, {-1200, 0, 0}), guest(1, {0, 0, 0});
    std::vector<PeerOut> out;
    std::vector<GuestCmd> cmds;
    std::vector<GuestBump> bumps;
    uint32_t unsharedLate = 0, unsharedSamples = 0;
    uint64_t due = 0, shown = 0;
    bool guestOffNear = false, guestOnAfter = false, leftoverShared = true;
    double switchedAt = -1;
    std::vector<uint32_t> leftovers;  // the guest's cars near him when the host's region took over
    uint32_t leftoversNearHost = 0;
    for (int tick = 0; tick < 2400; ++tick) {  // 120 s
        const double t = tick * 0.05;
        // the host drives to the guest at 15 m/s, stays 40 s, drives away
        if (t < 75) host.me.x = std::min(-30.0f, -1200.0f + 15.0f * static_cast<float>(t));
        if (t > 80) host.me.x = -30.0f - 15.0f * static_cast<float>(t - 80);
        const std::vector<PlayerSpot> spots = {{0, host.me, true}, {1, guest.me, true}};
        for (World* w : {&host, &guest}) {
            World& other = w == &host ? guest : host;
            w->region.Update(w->id, 0, spots, t);
            w->Generate(w->region.Own(), w->region.Since(t), other.me);
            w->auth.SetCars(w->Report(), t);
            for (const auto& c : w->cars) {
                const uint16_t n = w->auth.NetOf(c.ent);
                auto it = w->netOf.find(c.ent);
                if (it == w->netOf.end())
                    w->netOf[c.ent] = n;
                else if (it->second != n)
                    ++w->netChanged;
            }
            w->auth.SetView(other.id, other.me, t);
            w->auth.Tick(t, static_cast<uint32_t>(t * 1000), out);
            Delivery d{&other.recv};
            for (const auto& po : out) d.Apply(po, w->id, w->id != 0, t);
        }
        if (!guest.region.Own() && switchedAt < 0) {
            switchedAt = t;
            for (const auto& c : guest.cars)
                if (Dist3(c.pos, guest.me) <= 150) leftovers.push_back(c.ent);
        }
        if (switchedAt >= 0 && t > switchedAt + 3 && t < switchedAt + 3.06) {
            // 3 s after the switch: every leftover still alive on the guest's game and within the host's copy range
            // (240 m) has a copy on the host's
            for (uint32_t e : leftovers) {
                const World::SimCar* car = guest.Find(e);
                if (car && Dist3(car->pos, host.me) < 220 && !host.copies.count(RefKey(1, guest.auth.NetOf(e)))) {
                    leftoverShared = false;
                    std::printf("      leftover %u at %.0f m from the host has no copy there\n", e,
                                Dist3(car->pos, host.me));
                }
                if (car && Dist3(car->pos, host.me) < 220) ++leftoversNearHost;
            }
        }
        for (World* w : {&host, &guest}) {
            World& other = w == &host ? guest : host;
            w->recv.SetLocal(w->me);
            w->recv.Tick(t + 0.02, 0.05f, cmds, bumps);
            for (const auto& c : cmds) {
                if (c.type == GuestCmd::Type::Add) w->copies.insert(c.key);
                if (c.type == GuestCmd::Type::Remove) {
                    w->copies.erase(c.key);
                    // a pop: removed while its car still lives on its game within 150 m of us
                    const uint32_t ent = other.auth.KeyOf(RefNet(c.key));
                    const World::SimCar* car = ent ? other.Find(ent) : nullptr;
                    if (car && Dist3(car->pos, w->me) < 150) ++w->popped;
                }
            }
            for (uint32_t k : w->copies) {  // the Lua side: every copy reports, on its target
                CarState tg;
                if (!w->recv.Target(k, t + 0.02, tg)) continue;
                CopyReport r;
                r.pos = tg.pos;
                r.vel = tg.vel;
                w->recv.SetReport(k, r, t + 0.02);
            }
            // TRL's "near": our cars within 70 m of us that the other player near them has not had for 2 s
            const size_t un = w->auth.Unshared(w->me, 70, t, 2.0);
            if (w->region.Since(t) >= 10 && t > 10) {
                ++unsharedSamples;
                unsharedLate = std::max<uint32_t>(unsharedLate, static_cast<uint32_t>(un));
            }
        }
        // shown: while the players are within 250 m, the other's cars within 150 m with a copy that reports
        if (Dist3(host.me, guest.me) < 250 && t > switchedAt + 2 && switchedAt >= 0) {
            due += guest.recv.Stats().due + host.recv.Stats().due;
            shown += guest.recv.Stats().shown + host.recv.Stats().shown;
        }
        if (t > 60 && t < 75 && !guest.region.Own()) guestOffNear = true;
        if (t > 118 && guest.region.Own()) guestOnAfter = true;
    }
    Check(switchedAt > 0 && guestOffNear && guestOnAfter, "hand-over: the guest's spawning off near the host, on after",
          Fmt("switched at %.1f s", switchedAt));
    Check(host.netChanged == 0 && guest.netChanged == 0, "hand-over: a car keeps its authority and id while it lives");
    Check(leftoverShared && leftoversNearHost > 0, "hand-over: the guest's own cars near him are shared, not left driving",
          Fmt("%.0f leftovers, %.0f of them near the host", leftovers.size(), leftoversNearHost));
    Check(host.popped == 0 && guest.popped == 0, "hand-over: no copy vanishes within 150 m while its car lives",
          Fmt("host %.0f, guest %.0f", host.popped, guest.popped));
    Check(unsharedSamples > 100 && unsharedLate == 0, "hand-over: traffic.unshared_near 0 after 10 s",
          Fmt("max %.0f of %.0f samples", unsharedLate, unsharedSamples));
    const double pct = due ? 100.0 * static_cast<double>(shown) / static_cast<double>(due) : 0;
    Check(due > 0 && pct >= 95.0, "hand-over: traffic.shown >= 95 % near each other", Fmt("%.1f %%", pct));
}

// ---- a recorded car path through the whole chain: traffic.true_p95 ------------------------------------------------
// The host's car of the 0.15.0 two-PC trace (tests/traffic_path.h: 45 s, 12.9 m/s mean, 29 max, turns) is a traffic
// car of an authority. Its states go through AuthorityTraffic (interest, quantisation, the wire), a network (14 ms one
// way + up to 25 ms jitter, 1 % loss), a receiver (Stream per authority, playout, VehicleController + Corrector) at
// 60 fps, and a simulated copy: SetPos for a pin / snap, the nudge for a blend, then SetRot and SetSpeed (the speed
// along the car's heading, 3 % short of it) as RV.Drive does, then its own motion. The true error is the copy against
// the authority's car at the same moment (sync_compare's traffic.true_p95, target 1.5 m).
Vec3 Forward(const float q[4]) {  // the car's heading: (0, 1, 0) by the conjugate of its quaternion (Mafia DE)
    const float x = -q[0], y = -q[1], z = -q[2], w = q[3];
    const float vx = 0, vy = 1, vz = 0;
    const float tx = 2 * (y * vz - z * vy), ty = 2 * (z * vx - x * vz), tz = 2 * (x * vy - y * vx);
    return {vx + w * tx + (y * tz - z * ty), vy + w * ty + (z * tx - x * tz), vz + w * tz + (x * ty - y * tx)};
}

struct PathTruth {
    size_t n = sizeof(kRecordedCarPath) / sizeof(kRecordedCarPath[0]);
    double End() const { return kRecordedCarPath[n - 1].t; }
    void At(double t, CarState& s) const {
        size_t i = 0;
        while (i + 2 < n && kRecordedCarPath[i + 1].t < t) ++i;
        const auto& a = kRecordedCarPath[i];
        const auto& b = kRecordedCarPath[i + 1];
        const double span = std::max(1e-6, b.t - a.t);
        const float u = static_cast<float>(std::clamp((t - a.t) / span, 0.0, 1.0));
        const Vec3 pa{a.p[0], a.p[1], a.p[2]}, pb{b.p[0], b.p[1], b.p[2]};
        const Vec3 va{a.v[0], a.v[1], a.v[2]}, vb{b.v[0], b.v[1], b.v[2]};
        s.pos = net::playout::Hermite(pa, va, pb, vb, span, u);
        s.vel = {va.x + (vb.x - va.x) * u, va.y + (vb.y - va.y) * u, va.z + (vb.z - va.z) * u};
        net::motion::Nlerp(a.q, b.q, u, s.rot);
    }
};

struct PathResult {
    std::vector<float> trueErr, targetErr, copyErr;
    uint32_t snaps = 0, pins = 0, blends = 0;
    float P95(std::vector<float> v) const {
        if (v.empty()) return -1;
        std::sort(v.begin(), v.end());
        return v[static_cast<size_t>(0.95 * static_cast<double>(v.size() - 1))];
    }
};

PathResult RunPath(float viewDist, uint64_t seed, const GuestConfig& cfg = {}) {
    PathResult res;
    PathTruth truth;
    AuthorityTraffic auth;
    ReceiverTraffic recv(cfg);
    Rng rng(seed);
    const double clockGap = 1000.0;  // receiver clock - authority clock (s)
    struct InFlight {
        double arrive;
        PeerOut po;
    };
    std::vector<InFlight> fly;
    std::vector<PeerOut> out;
    std::vector<GuestCmd> cmds;
    std::vector<GuestBump> bumps;
    Delivery to{&recv};
    // the copy on the receiver's game
    bool exists = false;
    double spawnAt = -1;
    Vec3 cp, cv;
    float cq[4] = {0, 0, 0, 1};
    Vec3 lastTarget;
    double nextAuth = 0, frame = 0;
    const double step = 1 / 60.0;
    while (frame < truth.End() - 0.5) {
        // authority rounds at 20 Hz on its own clock
        while (nextAuth <= frame) {
            CarState s;
            truth.At(nextAuth, s);
            HostCar hc = Car(4711, 0, 0);
            hc.st.pos = s.pos;
            hc.st.vel = s.vel;
            for (int k = 0; k < 4; ++k) hc.st.rot[k] = s.rot[k];
            hc.st.flags = 0;  // controls unknown (the path has none): the copy goes by its speed
            auth.SetCars({hc}, nextAuth);
            // the receiving player rides beside the car at viewDist (his position as the authority sees it)
            auth.SetView(1, {s.pos.x + viewDist, s.pos.y, s.pos.z}, nextAuth);
            auth.Tick(nextAuth, static_cast<uint32_t>(nextAuth * 1000) + 123456, out);
            for (const auto& po : out) {
                if (rng.Uniform(0, 1) < 0.01f && po.spawns.empty() && po.despawns.empty()) continue;  // lost
                const double owd = 0.014 + rng.Uniform(0, 1) * rng.Uniform(0, 1) * 0.025;
                fly.push_back({nextAuth + owd, po});
            }
            nextAuth += 0.05;
        }
        // receiver frame (local clock = authority clock + clockGap; frames 16.7 ms +- 2 ms)
        const double local = frame + clockGap;
        for (size_t i = 0; i < fly.size();) {
            if (fly[i].arrive <= frame) {
                to.Apply(fly[i].po, 1, true, fly[i].arrive + clockGap);
                fly.erase(fly.begin() + static_cast<std::ptrdiff_t>(i));
            } else {
                ++i;
            }
        }
        CarState now;
        truth.At(frame, now);
        recv.SetLocal({now.pos.x + viewDist, now.pos.y, now.pos.z});
        const float dt = static_cast<float>(step);
        recv.Tick(local, dt, cmds, bumps);
        const uint32_t key = RefKey(1, auth.NetOf(4711));
        for (const auto& c : cmds) {
            if (c.key != key) continue;
            if (c.type == GuestCmd::Type::Add) {
                spawnAt = frame + 0.5;  // obtain + spawn take a while
                lastTarget = c.target.pos;
            } else if (c.type == GuestCmd::Type::Remove) {
                exists = false;
                spawnAt = -1;
            } else if (c.type == GuestCmd::Type::Drive) {
                lastTarget = c.target.pos;
                if (!exists) continue;
                using Fix = net::motion::VehicleCommand::Fix;
                if (c.vc.fix == Fix::Pin || c.vc.fix == Fix::Snap) {
                    cp = c.target.pos;
                    if (c.vc.fix == Fix::Snap) ++res.snaps;
                } else if (c.vc.fix == Fix::Blend) {
                    cp.x += c.vc.nudge.x;
                    cp.y += c.vc.nudge.y;
                    cp.z += c.vc.nudge.z;
                }
                if (c.vc.justPinned) ++res.pins;
                if (c.vc.fix != Fix::None) {
                    for (int k = 0; k < 4; ++k) cq[k] = c.vc.rot[k];
                    const float sp = 0.97f * std::sqrt(c.vc.vel.x * c.vc.vel.x + c.vc.vel.y * c.vc.vel.y +
                                                       c.vc.vel.z * c.vc.vel.z);
                    const Vec3 f = Forward(cq);
                    cv = {f.x * sp, f.y * sp, c.vc.vel.z};
                }
            }
        }
        if (!exists && spawnAt >= 0 && frame >= spawnAt) {
            exists = true;
            cp = lastTarget;
            cv = {};
            CarState tg;
            if (recv.Target(key, local, tg))
                for (int k = 0; k < 4; ++k) cq[k] = tg.rot[k];
        }
        if (exists) {
            CopyReport r;
            r.pos = cp;
            r.vel = cv;
            for (int k = 0; k < 4; ++k) r.rot[k] = cq[k];
            recv.SetReport(key, r, local);
            CarState tg;
            if (frame > 3 && recv.Target(key, local, tg)) {
                res.trueErr.push_back(Dist3(cp, now.pos));
                res.targetErr.push_back(Dist3(tg.pos, now.pos));
                res.copyErr.push_back(Dist3(cp, tg.pos));
            }
            // its own motion until the next frame (a little side slip on curves)
            cp.x += cv.x * dt + rng.Uniform(-0.01f, 0.01f);
            cp.y += cv.y * dt + rng.Uniform(-0.01f, 0.01f);
            cp.z += cv.z * dt;
        }
        frame += step + rng.Uniform(-0.002f, 0.002f);
    }
    res.blends = static_cast<uint32_t>(recv.Stats().blends);
    return res;
}

// The 0.15 settings for comparison (printed, not checked): lead at most 2 m, the controller's 8/s nudge everywhere.
GuestConfig Config015() {
    GuestConfig c;
    c.maxLead = 2.0f;
    c.correction.deadZone = 0;
    c.correction.gain = 8;
    c.correction.speedShare = 1000;
    c.correction.restSpeed = 1000;
    c.correction.snapDist = 4;
    c.correction.stuckTime = 1e9f;
    return c;
}

void TestRecordedPath() {
    // Tuning aid (how TrafficCorrection() was chosen): TRSWEEP=1 prints a grid of corrector settings. In the
    // 2026-10-19 sweep the speed share mattered most (0.3: p95 2.4-3.4 m, 0.5: 1.4-2.4, 0.7: 0.7-1.4), the dead zone
    // next (0.3 / 0.5 / 1.0), driving far copies more often hardly at all; a lead of at most 2 m left copies 150 m
    // away (sent every ~0.2 s, playout ~0.25 s) 4.6 m behind their car.
    if (std::getenv("TRSWEEP")) {
        for (float dz : {0.3f, 0.5f, 1.0f})
            for (float gain : {2.0f, 6.0f, 8.0f})
                for (float share : {0.3f, 0.5f, 0.7f}) {
                    GuestConfig c;
                    c.correction.deadZone = dz;
                    c.correction.gain = gain;
                    c.correction.speedShare = share;
                    const PathResult a = RunPath(30, 4242, c), b = RunPath(150, 4242, c);
                    std::printf("SWEEP dz %.2f gain %.0f share %.1f: 30 m p95 %.2f (copy %.2f) pins %u | 150 m p95 %.2f "
                                "(target %.2f copy %.2f) pins %u\n",
                                dz, gain, share, a.P95(a.trueErr), a.P95(a.copyErr), a.pins, b.P95(b.trueErr),
                                b.P95(b.targetErr), b.P95(b.copyErr), b.pins);
                }
    }
    for (float d : {30.0f, 150.0f}) {
        const PathResult o = RunPath(d, 4242, Config015());
        std::printf("INFO  recorded path at %.0f m with the 0.15 settings: p95 %.2f m (target vs car %.2f, copy vs "
                    "target %.2f), snaps %u pins %u\n",
                    d, o.P95(o.trueErr), o.P95(o.targetErr), o.P95(o.copyErr), o.snaps, o.pins);
        const PathResult r = RunPath(d, 4242);
        const float p95 = r.P95(r.trueErr);
        char name[96];
        std::snprintf(name, sizeof name, "recorded path at %.0f m: traffic.true_p95 <= 1.5 m", d);
        Check(r.trueErr.size() > 1500 && p95 >= 0 && p95 <= 1.5f, name,
              Fmt("p95 %.2f m (target vs car %.2f, copy vs target %.2f)", p95, r.P95(r.targetErr), r.P95(r.copyErr)) +
                  Fmt(", snaps %.0f pins %.0f blends %.0f", r.snaps, r.pins, r.blends));
    }
}

// ---- the authority's sharing count (TRL near) --------------------------------------------------------------------
// the latency trace's systems (game/latency.cpp): a guest's traffic states are "traffic", the rest "traffic-event"
void TestLatencySystems() {
    using namespace mc::game;
    Check(latency::SystemOf(0x0226) == latency::kTraffic && latency::SystemOf(0x0222) == latency::kTraffic &&
              latency::SystemOf(0x0224) == latency::kTrafficEvent && latency::SystemOf(0x0225) == latency::kTrafficEvent &&
              latency::SystemOf(0x0227) == latency::kTrafficEvent,
          "latency: 0x0226 traffic, 0x0224 / 0x0225 / 0x0227 traffic-event");
}

void TestUnshared() {
    HostConfig cfg;
    cfg.maxPerPeer = 3;
    AuthorityTraffic a(cfg);
    std::vector<HostCar> cars;
    for (uint32_t i = 0; i < 6; ++i) cars.push_back(Car(100 + i, 10.0f + i * 5.0f, 0));  // 10 .. 35 m from us
    std::vector<PeerOut> out;
    size_t mine = 0;
    double t = 0;
    a.SetCars(cars, t);
    a.SetView(1, {0, 0, 5}, t);
    a.Tick(t, 0, out);
    Check(a.Unshared({0, 0, 0}, 70, t, 2.0, &mine) == 0 && mine == 6, "unshared: none within the grace");
    for (int i = 0; i < 50; ++i) {
        t += 0.05;
        a.SetCars(cars, t);
        a.SetView(1, {0, 0, 5}, t);
        a.Tick(t, static_cast<uint32_t>(t * 1000), out);
    }
    Check(a.Unshared({0, 0, 0}, 70, t, 2.0) == 3, "unshared: cars a near player cannot get (his cap) count after 2 s",
          Fmt("%.0f", a.Unshared({0, 0, 0}, 70, t, 2.0)));
    a.SetView(1, {5000, 0, 5}, t + 0.05);
    a.Tick(t + 0.05, 0, out);
    Check(a.Unshared({0, 0, 0}, 70, t + 3, 2.0) == 0, "unshared: nobody near them: nothing unshared");
}

}  // namespace

int main() {
    TestQuat();
    TestStates();
    TestSpawns();
    TestHost();
    TestHostCap();
    TestGuest();
    TestAdaptiveDelay();
    TestEndToEnd();
    TestHostThreeGuests();
    TestHostPause();
    TestDriverWatch();
    TestOrphanEndToEnd();
    TestMatches015();
    TestRegions();
    TestGuestAuthorityRelay();
    TestUnshared();
    TestLatencySystems();
    TestHandOver();
    TestRecordedPath();
    std::printf("%s (%d failure%s)\n", g_failures ? "FAILED" : "OK", g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
