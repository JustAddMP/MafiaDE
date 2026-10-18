// traffic_test: unit tests for the traffic sync model (src/game/traffic_model.h): the wire format, the host's per-guest
// tracking (interest, admission cap, spawn / despawn / changed-only states) and the guest's table (copies near us,
// interpolation, removal, hand-over, bumps). Prints PASS/FAIL per check; exits non-zero on any failure.
#include "game/traffic_model.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
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
    g.SetLocal({0, 0, 0}, false, {});
    g.Tick(0.01, 0.01f, cmds, bumps);
    Check(!g.HostMode() && Count(cmds, GuestCmd::Type::Add) == 0, "guest: no host known -> our own traffic, no copies");
    g.SetLocal({0, 0, 0}, true, {300, 0, 0});
    Check(!g.HostMode(), "guest: host 300 m away -> still our own traffic (enter at 250)");
    g.SetLocal({0, 0, 0}, true, {100, 0, 0});
    g.Tick(0.02, 0.01f, cmds, bumps);
    Check(g.HostMode(), "guest: host near -> host traffic");
    Check(Count(cmds, GuestCmd::Type::Add) == 20, "guest: copies for the nearest 20",
          Fmt("%.0f adds", Count(cmds, GuestCmd::Type::Add)));
    bool nearestFirst = true;
    for (const auto& c : cmds)
        if (c.type == GuestCmd::Type::Add && c.net > 20) nearestFirst = false;
    Check(nearestFirst, "guest: nearest first");
    g.SetLocal({0, 0, 0}, true, {350, 0, 0});
    Check(g.HostMode(), "guest: host at 350 m keeps host traffic (leave at 400)");
    g.SetLocal({0, 0, 0}, true, {100, 0, 0});

    // interpolation: states 0.1 s apart (host time), at 10 m/s along x
    GuestTraffic g2;
    g2.OnSpawn(Spawn(1, 0, 0), 0);
    g2.OnStates(1000, {State(1, 0, 0, 10, 0)}, 10.0);
    g2.OnStates(1100, {State(1, 1, 0, 10, 0)}, 10.1);
    CarState t;
    // render time = 10.15 - offset(9.0) - 0.12 = 1.03 host s -> 0.3 m, plus the lead 10 x 0.12 = 1.2 m
    const bool have = g2.Target(1, 10.15, t);
    Check(have && std::fabs(t.pos.x - 1.5f) < 0.02f, "guest: interpolated target plus the lead", Fmt("x %.3f", t.pos.x));
    g2.Target(1, 10.6, t);  // beyond the newest: extrapolated (at most 0.5 s) + lead
    Check(std::fabs(t.pos.x - (1 + 10 * 0.38f + 1.2f)) < 0.05f, "guest: extrapolated past the newest state",
          Fmt("x %.3f", t.pos.x));
    g2.OnStates(1050, {State(1, 0.5f, 0, 10, 0)}, 10.2);
    Check(g2.Stats().dropsOld == 1, "guest: an older state is dropped");

    // drive: a report on target, then knocked 5 m off -> a bump
    g2.SetLocal({0, 0, 0}, true, {0, 0, 0});
    g2.Tick(10.2, 0.016f, cmds, bumps);
    Check(Count(cmds, GuestCmd::Type::Add) == 1, "guest: copy added");
    g2.OnStates(1200, {State(1, 2, 0, 0, 0)}, 10.3);
    g2.OnStates(1300, {State(1, 2, 0, 0, 0)}, 10.4);
    CopyReport rep;
    rep.pos = {2, 0, 0};
    g2.SetReport(1, rep, 10.5);
    g2.Tick(10.5, 0.016f, cmds, bumps);
    Check(Count(cmds, GuestCmd::Type::Drive) == 1 && bumps.empty(), "guest: driven, on target");
    rep.pos = {7, 0, 0};
    g2.SetReport(1, rep, 10.52);
    g2.Tick(10.52, 0.016f, cmds, bumps);
    Check(bumps.size() == 1 && bumps[0].error > 4.5f, "guest: knocked 5 m off -> bump reported");
    bool snapped = false;
    for (const auto& c : cmds)
        if (c.type == GuestCmd::Type::Drive && c.vc.fix == net::motion::VehicleCommand::Fix::Snap) snapped = true;
    Check(snapped, "guest: the knocked copy is put back on the host's car");

    // despawn: removed after the grace time, not at once
    g2.OnDespawn(1, 10.6);
    g2.Tick(10.7, 0.016f, cmds, bumps);
    const bool early = Count(cmds, GuestCmd::Type::Remove) == 0 && Count(cmds, GuestCmd::Type::Drive) == 0;
    g2.Tick(11.7, 0.016f, cmds, bumps);
    Check(early && Count(cmds, GuestCmd::Type::Remove) == 1, "guest: despawn after the grace time");

    // release: player sync took the car; never removed or driven by us
    GuestTraffic g3;
    g3.OnSpawn(Spawn(4, 10, 0), 0);
    g3.OnStates(0, {State(4, 10, 0, 0, 0)}, 0);
    g3.SetLocal({0, 0, 0}, true, {0, 0, 0});
    g3.Tick(0.1, 0.016f, cmds, bumps);
    g3.Release(4);
    g3.Tick(0.2, 0.016f, cmds, bumps);
    const bool quiet = cmds.empty();
    g3.OnDespawn(4, 0.3);
    g3.Tick(2.0, 0.016f, cmds, bumps);
    Check(quiet && cmds.empty() && g3.Stats().known == 0, "guest: a released copy is left to player sync");

    // stale: no state for 5 s -> removed
    GuestTraffic g4;
    g4.OnSpawn(Spawn(2, 10, 0), 0);
    g4.OnStates(0, {State(2, 10, 0, 0, 0)}, 0);
    g4.SetLocal({0, 0, 0}, true, {0, 0, 0});
    g4.Tick(0.1, 0.016f, cmds, bumps);
    g4.Tick(6.0, 0.016f, cmds, bumps);
    Check(Count(cmds, GuestCmd::Type::Remove) == 1, "guest: a car without states for 5 s loses its copy");
    // back to our own traffic: every copy goes
    g4.OnStates(100, {State(2, 10, 0, 0, 0)}, 6.1);
    g4.Tick(6.2, 0.016f, cmds, bumps);
    const bool readded = Count(cmds, GuestCmd::Type::Add) == 1;
    g4.SetLocal({0, 0, 0}, true, {1000, 0, 0});
    g4.Tick(6.3, 0.016f, cmds, bumps);
    Check(readded && !g4.HostMode() && Count(cmds, GuestCmd::Type::Remove) == 1,
          "guest: far from the host -> copies go, our own traffic back");

    // far copies are driven every third tick only
    GuestTraffic g5;
    g5.OnSpawn(Spawn(1, 20, 0), 0);
    g5.OnSpawn(Spawn(2, 200, 0), 0);
    g5.OnStates(0, {State(1, 20, 0, 0, 0), State(2, 200, 0, 0, 0)}, 0);
    g5.SetLocal({0, 0, 0}, true, {0, 0, 0});
    size_t near = 0, far = 0;
    for (int i = 0; i < 9; ++i) {
        g5.Tick(0.1 + i * 0.016, 0.016f, cmds, bumps);
        for (const auto& c : cmds)
            if (c.type == GuestCmd::Type::Drive) (c.net == 1 ? near : far)++;
    }
    Check(near == 9 && far == 3, "guest: far copies are driven every third tick", Fmt("near %.0f far %.0f", near, far));

    // a model change on a respawn: the old copy goes, a new one comes
    SpawnInfo other = Spawn(1, 20, 0);
    other.model = "bolt_ace";
    g5.OnSpawn(other, 0.5);
    g5.OnStates(600, {State(1, 20, 0, 0, 0)}, 0.6);
    g5.Tick(0.6, 0.016f, cmds, bumps);
    Check(Count(cmds, GuestCmd::Type::Remove) == 1 && Count(cmds, GuestCmd::Type::Add) == 1,
          "guest: another model under the same net id -> respawned");
}

// ---- end to end: host -> bytes -> guest ----
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
        g.SetLocal({0, 0, 0}, true, {0, 0, 0});
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

int main() {
    TestQuat();
    TestStates();
    TestSpawns();
    TestHost();
    TestHostCap();
    TestGuest();
    TestEndToEnd();
    std::printf("%s (%d failure%s)\n", g_failures ? "FAILED" : "OK", g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
