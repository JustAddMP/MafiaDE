// ped_test: unit tests for the pedestrian sync model (src/game/ped_model.h): the wire format, the region authority's
// sender (who owns which pedestrian, interest admission and rates, spawn / gone / death, relay through the host to a
// second guest), the one-to-one adoption matching, the receiver (adopt first, spawn second, hide the surplus only out
// of sight, release, show again, deaths), and recorded-like pedestrian walks through the whole chain (authority ->
// lossy, jittery link -> replica::Stream -> LocomotionController -> a simulated AI walker) asserting the targets of
// docs/sync-architecture.md section 4 (ped.p95 <= 1.5 m, ped.coverage >= 80 %). The 2026-10-20 two-PC traces
// (tests/data/ped_trace.h) are replayed through the same chain: every recorded pedestrian's real walk, the copy's
// recorded answer (a copy that never closed its gap), the tram riders, the deaths, and session A's coverage with the
// guest's real walk and its copy spawner hidden mid-session - before (0.16.0 rules) and after. Prints PASS/FAIL per
// check; exits non-zero on any failure.
#include "game/ped_model.h"

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

#include "data/ped_trace.h"

using namespace mc;
using namespace mc::game::ped;
using net::motion::Gait;
using net::motion::LocomotionCommand;

namespace {

int g_failures = 0;

void Check(bool ok, const char* name, const std::string& detail = {}) {
    std::printf("%s  %-60s %s\n", ok ? "PASS" : "FAIL", name, detail.c_str());
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

float Dist2(const Vec3& a, const Vec3& b) {
    return std::hypot(a.x - b.x, a.y - b.y);
}

float Pct(std::vector<float> v, float p) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    return v[std::min(v.size() - 1, static_cast<size_t>(p / 100.0f * static_cast<float>(v.size() - 1) + 0.5f))];
}

LocalPed Ped(uint32_t id, float x, float y, uint8_t flags = 0) {
    LocalPed p;
    p.id = id;
    p.pos = {x, y, 0};
    p.flags = flags;
    p.look = id * 7;
    return p;
}

// ---- codec ---------------------------------------------------------------------------------------------------------
void TestCodec() {
    std::vector<PedState> in;
    Rng rng(7);
    for (int i = 0; i < 120; ++i) {
        PedState s;
        s.net = static_cast<uint16_t>(i + 1);
        // the first 60 near one place, the rest 3 km away: two origins
        const float base = i < 60 ? 100.0f : 3100.0f;
        s.pos = {base + rng.Uniform(-400, 400), -250 + rng.Uniform(-400, 400), 12 + rng.Uniform(-5, 5)};
        s.vel = {rng.Uniform(-6, 6), rng.Uniform(-6, 6), 0};
        const float a = rng.Uniform(-3.14f, 3.14f);
        s.heading[0] = std::cos(a);
        s.heading[1] = std::sin(a);
        s.flags = static_cast<uint8_t>(i % 16);
        s.gait = static_cast<uint8_t>(i % 4);
        in.push_back(s);
    }
    const auto packets = PackStates(123456789u, in);
    size_t total = 0, maxBytes = 0;
    float maxPos = 0, maxVel = 0, maxHead = 0;
    bool fieldsOk = true;
    std::vector<PedState> all;
    for (const auto& b : packets) {
        maxBytes = std::max(maxBytes, b.size());
        net::ByteReader r(b);
        uint32_t ms = 0;
        std::vector<PedState> out;
        if (!ReadStates(r, ms, out) || ms != 123456789u) fieldsOk = false;
        total += out.size();
        all.insert(all.end(), out.begin(), out.end());
    }
    for (size_t i = 0; i < all.size() && i < in.size(); ++i) {
        const PedState& a = in[i];
        const PedState& b = all[i];
        maxPos = std::max(maxPos,
                          std::fabs(a.pos.x - b.pos.x) + std::fabs(a.pos.y - b.pos.y) + std::fabs(a.pos.z - b.pos.z));
        maxVel = std::max(maxVel, std::fabs(a.vel.x - b.vel.x) + std::fabs(a.vel.y - b.vel.y));
        maxHead = std::max(maxHead, std::fabs(a.heading[0] - b.heading[0]) + std::fabs(a.heading[1] - b.heading[1]));
        if (a.net != b.net || a.flags != b.flags || a.gait != b.gait) fieldsOk = false;
    }
    Check(total == in.size() && fieldsOk, "states: every state back, net / flags / gait exact",
          Fmt("%.0f of %.0f in %.0f packets", total, in.size(), packets.size()));
    Check(packets.size() >= 4, "states: split by origin and by the 50 per packet cap",
          Fmt("%.0f packets", packets.size()));
    Check(maxBytes <= kStatesHeader + kMaxPerPacket * kStateBytes && maxBytes <= 1100, "states: packet size bound",
          Fmt("max %.0f bytes", maxBytes));
    Check(maxPos <= 3 * (0.5f / kPosScale) + 1e-3f, "states: position within 1/64 m per axis", Fmt("%.4f m", maxPos));
    Check(maxVel <= 0.011f && maxHead <= 0.03f, "states: velocity 1/100 m/s, heading 1/127",
          Fmt("vel %.4f head %.4f", maxVel, maxHead));
    Check(kStateBytes == 16 && packets[0].size() == kStatesHeader + 50 * kStateBytes, "states: 16 bytes per pedestrian",
          Fmt("%.0f bytes for 50", packets[0].size()));

    std::vector<PedSpawnInfo> sp;
    for (int i = 0; i < 30; ++i) {
        PedSpawnInfo s;
        s.net = static_cast<uint16_t>(500 + i);
        s.cls = static_cast<uint8_t>(i);
        s.look = 0xDEADBEEFu + static_cast<uint32_t>(i);
        s.st.pos = {1234.5f + i, -88.25f, 3.5f};
        s.st.heading[0] = 0;
        s.st.heading[1] = -1;
        s.st.flags = kDead;
        sp.push_back(s);
    }
    const auto spb = PackSpawns(sp);
    net::ByteReader sr(spb);
    std::vector<PedSpawnInfo> spo;
    const bool spOk = ReadSpawns(sr, spo);
    Check(spOk && spo.size() == kMaxSpawnsPerMessage && spo[3].net == 503 && spo[3].look == 0xDEADBEEFu + 3 &&
              spo[3].st.pos.x == 1237.5f && spo[3].st.flags == kDead && spo[3].st.heading[1] < -0.99f &&
              spb.size() == 1 + kMaxSpawnsPerMessage * 22,
          "spawn: round trip (capped at 24 per message, 22 bytes each)", Fmt("%.0f bytes", spb.size()));
    const auto gb = PackGone({3, 9, 65535});
    net::ByteReader gr(gb);
    std::vector<uint16_t> go;
    Check(ReadGone(gr, go) && go == std::vector<uint16_t>({3, 9, 65535}), "gone: round trip");
    const auto db = PackDeath(77, {1, 2, 3});
    net::ByteReader dr(db);
    uint16_t dn = 0;
    Vec3 dp;
    Check(ReadDeath(dr, dn, dp) && dn == 77 && dp.y == 2, "death: round trip");
    const auto kb = PackKill(4242);
    net::ByteReader kr(kb);
    uint16_t kn = 0;
    Check(ReadKill(kr, kn) && kn == 4242, "kill: round trip");
    // truncated / bad input
    std::vector<uint8_t> cut(packets[0].begin(), packets[0].begin() + 40);
    net::ByteReader cr(cut);
    uint32_t ms = 0;
    std::vector<PedState> co;
    std::vector<uint8_t> badGait = PackStates(1, {in[0]})[0];
    badGait.back() = 9;
    net::ByteReader br(badGait);
    Check(!ReadStates(cr, ms, co) && !ReadStates(br, ms, co), "states: truncated / bad gait refused");
}

void TestRules() {
    uint8_t g = 0;
    std::string seq;
    for (float s : {0.2f, 0.6f, 0.4f, 0.25f, 1.5f, 2.3f, 2.5f, 2.1f, 1.9f, 5.0f, 4.5f, 4.2f, 0.0f}) {
        g = GaitOf(s, g);
        seq += static_cast<char>('0' + g);
    }
    Check(seq == "0110112213320", "gait: thresholds with hysteresis", seq);
    // 4.5 after sprint stays sprint (down at 4.3), 4.2 -> run, 0 -> stand
    Check(GaitOf(4.5f, 3) == 3 && GaitOf(4.2f, 3) == 2 && GaitOf(NAN, 2) == 0, "gait: sprint hysteresis, NaN");
    const Vec3 cam{0, 0, 2}, dir{1, 0, 0};
    Check(InView(cam, dir, {30, 5, 0}) && !InView(cam, dir, {-30, 0, 0}) && InView(cam, dir, {-2, 1, 1}) &&
              !InView(cam, dir, {200, 0, 0}) && !InView(cam, dir, {10, 30, 0}),
          "view: cone, near always, far never");
    const auto t = LocomotionForPed();
    Check(t.nudgeStart == 0.5f && t.snapDist == 5.0f && t.stuckTime == 3.0f && t.nudgeSpeedShare == 0.35f,
          "locomotion tuning = Corrector::ForNpc rules");
}

// ---- authority -----------------------------------------------------------------------------------------------------
struct Inbox {
    std::vector<PedSpawnInfo> spawns;
    std::vector<uint16_t> gones, deaths;
    std::vector<PedState> states;
    size_t stateMsgs = 0;
    uint64_t bytes = 0;
};

void Deliver(const PeerOut& po, Inbox& in) {
    for (const auto& b : po.spawns) {
        net::ByteReader r(b);
        std::vector<PedSpawnInfo> l;
        if (ReadSpawns(r, l)) in.spawns.insert(in.spawns.end(), l.begin(), l.end());
        in.bytes += b.size();
    }
    for (const auto& b : po.gones) {
        net::ByteReader r(b);
        std::vector<uint16_t> l;
        if (ReadGone(r, l)) in.gones.insert(in.gones.end(), l.begin(), l.end());
        in.bytes += b.size();
    }
    for (const auto& b : po.deaths) {
        net::ByteReader r(b);
        uint16_t n = 0;
        Vec3 p;
        if (ReadDeath(r, n, p)) in.deaths.push_back(n);
        in.bytes += b.size();
    }
    for (const auto& b : po.states) {
        net::ByteReader r(b);
        uint32_t ms = 0;
        std::vector<PedState> l;
        if (ReadStates(r, ms, l)) in.states.insert(in.states.end(), l.begin(), l.end());
        ++in.stateMsgs;
        in.bytes += b.size();
    }
}

void TestAuthority() {
    // host 0 at the origin, guest 1 at (30, 0): the host is the region authority around both (within 250 m of him)
    PedAuthority host;
    std::vector<PlayerView> players = {{0, {0, 0, 0}, true}, {1, {30, 0, 0}, true}};
    host.SetPlayers(players, 0, 0);
    std::vector<LocalPed> peds = {Ped(8 + 256 * 1, 32, 2), Ped(8 + 256 * 2, 92, 0), Ped(8 + 256 * 3, 140, 0),
                                  Ped(8 + 256 * 4, 30, -40)};
    std::map<uint8_t, Inbox> inbox;
    double t = 0;
    for (int i = 0; i < 40; ++i, t += 0.05) {  // 2 s at 20 Hz rounds
        peds[0].pos.x = 32 + 0.065f * i;       // walks at 1.3 m/s
        peds[3].pos.x = 30 + 0.06f * i;        // and so does the one 40 m from him
        host.SetPeds(peds, t);
        std::vector<PeerOut> out;
        host.Tick(t, static_cast<uint32_t>(t * 1000), out);
        for (const auto& po : out) Deliver(po, inbox[po.peer]);
    }
    const Inbox& g = inbox[1];
    std::set<uint16_t> spawned;
    for (const auto& s : g.spawns) spawned.insert(s.net);
    const uint16_t nearNet = host.NetOf(peds[0].id), farNet = host.NetOf(peds[2].id), edgeNet = host.NetOf(peds[3].id);
    Check(host.OwnedCount() == 4 && host.OwnerOf(peds[2].id) == 0, "authority: the host owns the pedestrians near him");
    Check(spawned.count(nearNet) && spawned.count(edgeNet) && !spawned.count(farNet) &&
              !spawned.count(host.NetOf(peds[1].id)),
          "interest: within 50 m of the guest spawned to him, 40 m+ away not", Fmt("%.0f spawned", spawned.size()));
    int nearStates = 0, edgeStates = 0;
    for (const auto& s : g.states) {
        if (s.net == nearNet) ++nearStates;
        if (s.net == edgeNet) ++edgeStates;
    }
    Check(nearStates >= 17 && nearStates <= 22, "rate: a pedestrian 2 m from the guest ~10 Hz",
          Fmt("%.0f in 2 s", nearStates));
    Check(edgeStates >= 5 && edgeStates < nearStates - 4, "rate: one 40 m away slower (3-5 Hz)",
          Fmt("%.0f in 2 s", edgeStates));
    Check(inbox.count(0) == 0, "authority: nothing sent to ourselves");
    PedState walked;
    for (const auto& s : g.states)
        if (s.net == nearNet) walked = s;
    Check(walked.gait == 1 && std::fabs(walked.vel.x - 1.3f) < 0.15f && walked.heading[1] >= 0,
          "measured velocity and gait of a walker", Fmt("vx %.2f gait %.0f", walked.vel.x, walked.gait));
    // death: once, reliably, and the state carries the flag
    peds[0].flags = kDead;
    size_t deaths0 = g.deaths.size();
    for (int i = 0; i < 10; ++i, t += 0.05) {
        host.SetPeds(peds, t);
        std::vector<PeerOut> out;
        host.Tick(t, static_cast<uint32_t>(t * 1000), out);
        for (const auto& po : out) Deliver(po, inbox[po.peer]);
    }
    bool deadState = false;
    for (const auto& s : inbox[1].states)
        if (s.net == nearNet && (s.flags & kDead)) deadState = true;
    Check(inbox[1].deaths.size() == deaths0 + 1 && deadState, "death: one PedDeath, states carry the flag");
    // the guest walks away (60 m+ from the pedestrian): gone
    players[1].pos = {200, 0, 0};
    host.SetPlayers(players, 0, 0);
    for (int i = 0; i < 4; ++i, t += 0.05) {
        host.SetPeds(peds, t);
        std::vector<PeerOut> out;
        host.Tick(t, static_cast<uint32_t>(t * 1000), out);
        for (const auto& po : out) Deliver(po, inbox[po.peer]);
    }
    Check(std::count(inbox[1].gones.begin(), inbox[1].gones.end(), nearNet) == 1, "interest: PedGone when he left");
    // a pedestrian that leaves the list is gone for everyone who had it; a player who left gets nothing
    host.RemovePeer(1);
    Check(host.Stats(1) == nullptr, "a player who left is forgotten");

    // the guest's own authority: near the host he owns nothing; 600 m away he owns his pedestrians
    PedAuthority guest;
    std::vector<PlayerView> gp = {{0, {0, 0, 0}, true}, {1, {30, 0, 0}, true}};
    guest.SetPlayers(gp, 0, 1);
    guest.SetPeds({Ped(1000 + 8, 35, 3), Ped(2000 + 8, 20, -5)}, 0);
    Check(guest.OwnedCount() == 0 && guest.Foreign().size() == 2, "region: near the host the guest's are foreign");
    gp[1].pos = {600, 0, 0};
    PedAuthority far;
    far.SetPlayers(gp, 0, 1);
    far.SetPeds({Ped(1000 + 8, 605, 3)}, 0);
    Check(far.OwnedCount() == 1 && far.Foreign().empty(), "region: 600 m from the host the guest is the authority");
    // hysteresis: a pedestrian the guest owns stays his while the host comes to 200 m of it (Keep: 400 m release)
    gp[0].pos = {420, 0, 0};
    far.SetPlayers(gp, 0, 1);
    far.SetPeds({Ped(1000 + 8, 606, 3)}, 0.1);
    Check(far.OwnerOf(1000 + 8) == 1, "region: kept by its owner when the host comes near (hysteresis)");
    // ... but a new pedestrian there is the host's (within 250 m of him)
    far.SetPeds({Ped(1000 + 8, 606, 3), Ped(3000 + 8, 560, 0)}, 0.2);
    Check(far.OwnerOf(3000 + 8) == 0, "region: a new one near the host is the host's");
}

// Relay: a guest authority (1) sends to the host (0) and to another guest (2): one PeerOut per receiver (core's SendTo
// carries guest -> guest through the host). Receivers key by (authority, net), so the host's net 1 and the guest's
// net 1 are different pedestrians.
void TestRelay() {
    PedAuthority g1;
    std::vector<PlayerView> players = {{0, {700, 0, 0}, true}, {1, {0, 0, 0}, true}, {2, {10, 0, 0}, true}};
    g1.SetPlayers(players, 0, 1);
    g1.SetPeds({Ped(8 + 256, 5, 0), Ped(8 + 512, 2, 3), Ped(8 + 768, 12, 2)}, 0);  // the last is nearer guest 2: his
    std::vector<PeerOut> out;
    g1.Tick(0, 1000, out);
    std::set<uint8_t> to;
    for (const auto& po : out) to.insert(po.peer);
    Check(to == std::set<uint8_t>({2}) && g1.OwnedCount() == 2,
          "relay: guest authority sends to the guest near him (not the far host)");
    PedReceiver rx;
    Inbox in;
    for (const auto& po : out) Deliver(po, in);
    for (const auto& s : in.spawns) rx.OnSpawn(1, s, 0);
    rx.OnStates(1, 1000, in.states, 0);
    PedSpawnInfo hs;
    hs.net = in.spawns[0].net;  // the same net id from the host
    rx.OnSpawn(0, hs, 0);
    Check(
        PedReceiver::Key(1, hs.net) != PedReceiver::Key(0, hs.net) && rx.Stats().states == 2 && rx.Stats().spawns == 3,
        "relay: (authority, net) keys apart", Fmt("states %.0f spawns %.0f", rx.Stats().states, rx.Stats().spawns));
}

void TestMatch() {
    std::vector<Vec3> r = {{0, 0, 0}, {10, 0, 0}, {20, 0, 0}};
    std::vector<Vec3> l = {{1, 0, 0}, {0.5f, 0, 0}, {10, 2, 0}, {40, 0, 0}};
    std::vector<bool> rs = {true, true, false}, ls = {true, true, true, false};
    auto m = MatchNearest(r, rs, l, ls, 3.0f, 45.0f);
    // r0 <- l1 (0.5), r1 <- l2 (2), r2 <- l3 (20 m, both unseen); l0 unused
    std::map<size_t, size_t> got;
    for (const auto& p : m) got[p.remote] = p.local;
    Check(m.size() == 3 && got[0] == 1 && got[1] == 2 && got[2] == 3, "match: nearest first, one-to-one");
    std::vector<bool> ls2 = {true, true, true, true};
    auto m2 = MatchNearest(r, rs, l, ls2, 3.0f, 45.0f);
    Check(m2.size() == 2, "match: far only when neither is in sight");
    auto m3 = MatchNearest(r, rs, l, ls, 3.0f, 45.0f);
    bool same = m3.size() == m.size();
    for (size_t i = 0; same && i < m.size(); ++i) same = m3[i].remote == m[i].remote && m3[i].local == m[i].local;
    Check(same, "match: deterministic");
}

// ---- receiver ------------------------------------------------------------------------------------------------------
std::vector<PedState> StatesAt(const std::map<uint16_t, Vec3>& pos, float vx = 0) {
    std::vector<PedState> s;
    for (const auto& [net, at] : pos) {
        PedState p;
        p.net = net;
        p.pos = at;
        p.vel = {vx, 0, 0};
        p.gait = vx > 0.5f ? 1 : 0;
        s.push_back(p);
    }
    return s;
}

void TestReceiver() {
    ReceiverConfig cfg;
    PedReceiver rx(cfg);
    net::authority::RegionAuthority region;
    std::vector<net::authority::PlayerSpot> spots = {{0, {0, 0, 0}, true}, {1, {5, 0, 0}, true}};
    region.SetPlayers(spots, 0);
    rx.SetRegion(region, 1);
    // camera at us looking +x: x > 0 in view
    rx.SetLocal({5, 0, 0}, Camera{{5, 0, 1.7f}, {1, 0, 0}, true});
    // the host's pedestrians: 1 in view 2 m from a local human; 2 behind us 20 m from a local one; 3 in view, nobody
    std::map<uint16_t, Vec3> rp = {{1, {20, 0, 0}}, {2, {-15, 0, 0}}, {3, {30, 10, 0}}};
    std::vector<LocalPed> locals = {Ped(100, 22, 0), Ped(200, -35, 0), Ped(300, 25, -12), Ped(400, -10, 20)};
    double now = 0;
    std::vector<RecvCmd> cmds;
    std::map<uint32_t, uint32_t> adopted;
    std::set<uint32_t> hidden;
    std::set<uint32_t> spawnAsked;
    auto step = [&](double dt) {
        now += dt;
        rx.OnStates(0, static_cast<uint32_t>(now * 1000) + 5000, StatesAt(rp), now);
        rx.SetCandidates(locals);
        cmds.clear();
        rx.Tick(now, static_cast<float>(dt), cmds);
        for (const auto& c : cmds) {
            if (c.type == RecvCmd::Type::Adopt) adopted[c.key] = c.local;
            if (c.type == RecvCmd::Type::Hide) hidden.insert(c.local);
            if (c.type == RecvCmd::Type::Spawn) spawnAsked.insert(c.key);
        }
    };
    for (int i = 0; i < 43; ++i) step(0.1);
    Check(adopted.size() == 2 && adopted[PedReceiver::Key(0, 1)] == 100 && adopted[PedReceiver::Key(0, 2)] == 200,
          "adopt: near one in sight, far one only out of sight", Fmt("%.0f adopted", adopted.size()));
    Check(hidden == std::set<uint32_t>({400}), "hide: the surplus out of sight only (in-sight 300 kept)",
          Fmt("%.0f hidden", hidden.size()));
    Check(spawnAsked == std::set<uint32_t>({PedReceiver::Key(0, 3)}), "spawn: the one nobody can stand for");
    Check(rx.Stats().nearby == 3 && rx.Stats().nearReplica == 2 && rx.Stats().loading == 1, "coverage counts",
          Fmt("near %.0f rep %.0f loading %.0f", rx.Stats().nearby, rx.Stats().nearReplica, rx.Stats().loading));
    // stable: no re-adoption, no double use over many ticks
    size_t again = 0;
    for (int i = 0; i < 20; ++i) {
        step(0.1);
        for (const auto& c : cmds)
            if (c.type == RecvCmd::Type::Adopt) ++again;
    }
    Check(again == 0, "adopt: stable over time");
    rx.Spawned(PedReceiver::Key(0, 3), 900);
    step(0.1);
    Check(rx.Stats().nearReplica == 3 && rx.Stats().spawned == 1, "spawned copy counts as a replica");
    // drives: every replica gets one
    std::set<uint32_t> driven;
    for (const auto& c : cmds)
        if (c.type == RecvCmd::Type::Drive) driven.insert(c.key);
    Check(driven.size() == 3, "drive: every replica every tick", Fmt("%.0f", driven.size()));
    // our replica died here: told once
    CopyReport dead;
    dead.pos = rp[1];
    dead.dead = true;
    rx.SetReport(PedReceiver::Key(0, 1), dead, now);
    step(0.1);
    Check(rx.DiedHere(PedReceiver::Key(0, 1)) && !rx.DiedHere(PedReceiver::Key(0, 1)), "died here: once");
    // the authority's death: a Kill for its replica
    rx.OnDeath(0, 2, rp[2], now);
    step(0.1);
    bool killed = false;
    for (const auto& c : cmds)
        if (c.type == RecvCmd::Type::Kill && c.key == PedReceiver::Key(0, 2) && c.local == 200) killed = true;
    Check(killed, "death: the replica is killed");
    // gone: the adopted one is released at once, the spawned copy when nobody looks (it is in view: after the grace)
    rx.OnGone(0, 1, now);
    rx.OnGone(0, 3, now);
    rp.erase(1);
    rp.erase(3);
    CopyReport seenCopy;
    seenCopy.pos = {30, 10, 0};
    rx.SetReport(PedReceiver::Key(0, 3), seenCopy, now);
    step(0.1);
    bool relAdopted = false, relSpawned = false;
    for (const auto& c : cmds) {
        if (c.type == RecvCmd::Type::Release && c.key == PedReceiver::Key(0, 1)) relAdopted = c.how == How::Adopted;
        if (c.type == RecvCmd::Type::Release && c.key == PedReceiver::Key(0, 3)) relSpawned = true;
    }
    Check(relAdopted && !relSpawned, "gone: adopted released at once, a copy in view waits");
    for (int i = 0; i < 35 && !relSpawned; ++i) {
        rx.SetReport(PedReceiver::Key(0, 3), seenCopy, now);
        step(0.1);
        for (const auto& c : cmds)
            if (c.type == RecvCmd::Type::Release && c.key == PedReceiver::Key(0, 3)) relSpawned = true;
    }
    Check(relSpawned, "gone: a copy in view goes after the grace");
    // hidden humans come back where the region is ours again: the host leaves (600 m away)
    spots[0].pos = {900, 0, 0};
    region.SetPlayers(spots, 0);
    rx.SetRegion(region, 1);
    rx.SetLocal({5, 0, 0}, Camera{{5, 0, 1.7f}, {1, 0, 0}, true});
    bool shown = false;
    for (int i = 0; i < 30 && !shown; ++i) {
        step(0.1);
        for (const auto& c : cmds)
            if (c.type == RecvCmd::Type::Show && c.local == 400) shown = true;
    }
    Check(shown && !rx.IsHidden(400), "show: hidden human back when the region is ours (out of sight)");
    // the authority leaves: everything of his goes
    rx.ForgetAuthority(0);
    rp.clear();
    std::vector<RecvCmd> out;
    rx.SetCandidates({});
    rx.Tick(now + 0.1, 0.1f, out);
    size_t rel = 0;
    for (const auto& c : out)
        if (c.type == RecvCmd::Type::Release) ++rel;
    Check(rel >= 1 && rx.Stats().adopted == 0, "authority left: replicas released", Fmt("%.0f released", rel));
}

// ---- end to end: one pedestrian's walk through the whole chain -------------------------------------------------------
// The game's AI walk of the replica: commands act after a lag, gait speeds off by up to 20 %, a turn rate, braking.
struct AiWalker {
    float realSpeed[4] = {0, 1.3f * 0.85f, 4.0f * 1.15f, 6.0f * 0.9f};
    float lag = 0.18f;
    float x = 0, y = 0, hx = 1, hy = 0, speed = 0;
    Gait gait = Gait::Stand;
    float mult = 1, tx = 0, ty = 0;
    bool walking = false;
    struct Pending {
        float at;
        Gait gait;
        float mult;
        bool reissue;
        float tx, ty;
    };
    std::deque<Pending> queue;
    float clock = 0;
    int snaps = 0;
    static constexpr float kTurn = 300.0f * 3.14159265f / 180;
    float kAccel = 6.0f;  // m/s^2

    void Apply(const LocomotionCommand& c, const Vec3& target) {
        if (c.fix == LocomotionCommand::Fix::Snap) {
            x = target.x;
            y = target.y;
            speed = 0;
            ++snaps;
        } else if (c.fix == LocomotionCommand::Fix::Nudge) {
            x += c.nudge.x;
            y += c.nudge.y;
        }
        queue.push_back({clock + lag, c.gait, c.speedMult, c.reissue,
                         c.reissue && c.gait == Gait::Stand ? target.x : c.moveTo.x,
                         c.reissue && c.gait == Gait::Stand ? target.y : c.moveTo.y});
    }
    void Step(float dt) {
        clock += dt;
        while (!queue.empty() && queue.front().at <= clock) {
            const Pending& p = queue.front();
            if (p.reissue) {
                gait = p.gait == Gait::Stand ? Gait::Walk : p.gait;
                walking = true;
                tx = p.tx;
                ty = p.ty;
            }
            mult = p.gait == Gait::Stand ? 1.0f : p.mult;
            queue.pop_front();
        }
        float want = 0;
        const float dx = tx - x, dy = ty - y, d = std::sqrt(dx * dx + dy * dy);
        if (walking && d > 0.1f) {
            want = realSpeed[static_cast<int>(gait)] * mult;
            const float a = std::atan2(hx * dy - hy * dx, hx * dx + hy * dy);
            const float r = std::clamp(a, -kTurn * dt, kTurn * dt);
            const float c = std::cos(r), s = std::sin(r);
            const float nx = hx * c - hy * s, ny = hx * s + hy * c;
            hx = nx;
            hy = ny;
            want *= std::max(0.3f, std::cos(a));
            want = std::min(want, std::sqrt(2 * kAccel * d));
        } else if (walking) {
            walking = false;
        }
        if (speed < want)
            speed = std::min(want, speed + kAccel * dt);
        else
            speed = std::max(want, speed - kAccel * dt);
        x += hx * speed * dt;
        y += hy * speed * dt;
    }
};

// A recorded-like pedestrian: strolls, stops, turns, panics (runs with a weave), walks on, stops again.
struct Path {
    struct Seg {
        float secs, speed, turnDegPerS;
    };
    std::vector<Seg> segs = {{8, 1.3f, 0},  {3, 0, 0},       {6, 1.2f, 15}, {1.5f, 0, 0},   {5, 4.2f, 0},
                             {3, 4.0f, 60}, {8, 1.25f, -10}, {2, 0, 0},     {10, 1.35f, 5}, {2, 0, 0}};
    float x = 0, y = 0, dir = 0, speed = 0;
    float vx = 0, vy = 0;
    void Step(float t, float dt) {
        float acc = 0;
        const Seg* s = &segs.back();
        for (const Seg& g : segs) {
            if (t < acc + g.secs) {
                s = &g;
                break;
            }
            acc += g.secs;
        }
        const float a = s->speed > speed ? 2.5f : 3.5f;  // m/s^2, a person
        if (speed < s->speed)
            speed = std::min(s->speed, speed + a * dt);
        else
            speed = std::max(s->speed, speed - a * dt);
        dir += s->turnDegPerS * 3.14159265f / 180 * dt;
        if (s->speed > 3.5f) dir += 0.6f * std::sin(t * 4.0f) * dt;  // weave while running
        vx = std::cos(dir) * speed;
        vy = std::sin(dir) * speed;
        x += vx * dt;
        y += vy * dt;
    }
    float Length() const {
        float a = 0;
        for (const Seg& g : segs) a += g.secs;
        return a;
    }
};

struct Packet {
    double at;
    uint16_t id;
    std::vector<uint8_t> body;
};

void TestEndToEnd(float jitterMs, float lossPct, uint64_t seed, const char* label) {
    Rng rng(seed);
    PedAuthority auth;
    ReceiverConfig rc;
    PedReceiver rx(rc);
    Path path;
    path.x = 100;
    path.y = 50;
    AiWalker w;
    w.x = path.x + 1.5f;  // the receiver's own human that will be adopted (2 m off, in sight)
    w.y = path.y + 1.0f;
    const uint32_t localId = 4000 + 8, myOwnId = 99 * 256 + 8;
    const double authEpoch = 1234.5, recvEpoch = 77.25;  // the two clocks differ
    std::deque<Packet> wire;
    std::vector<float> errs, errsMoving;
    const float dt = 1.0f / 60.0f;
    const float total = path.Length();
    double roundAt = 0;
    bool adopted = false;
    for (float t = 0; t < total; t += dt) {
        path.Step(t, dt);
        const Vec3 truth{path.x, path.y, 0};
        // the receiving player walks along 8 m behind
        const Vec3 me{path.x - 8, path.y, 0};
        // authority rounds at 20 Hz
        if (t - roundAt >= 0.05 - 1e-6) {
            roundAt = t;
            const double an = authEpoch + t;
            auth.SetPlayers({{0, {path.x + 20, path.y, 0}, true}, {1, me, true}}, 0, 0);
            LocalPed lp = Ped(myOwnId, path.x, path.y);
            const float sp = std::hypot(path.vx, path.vy);
            if (sp > 0.1f) {
                lp.heading[0] = path.vx / sp;
                lp.heading[1] = path.vy / sp;
            }
            auth.SetPeds({lp}, an);
            std::vector<PeerOut> out;
            auth.Tick(an, static_cast<uint32_t>(static_cast<uint64_t>(an * 1000.0)), out);
            for (auto& po : out) {
                const double delay = 0.025 + rng.Uniform(0, jitterMs / 1000.0f);
                for (auto& b : po.spawns) wire.push_back({t + 0.025, kMsgSpawn, b});  // reliable: no loss
                for (auto& b : po.states)
                    if (rng.Uniform(0, 100) >= lossPct) wire.push_back({t + delay, kMsgStates, b});
            }
        }
        // the link
        const double rnow = recvEpoch + t;
        std::stable_sort(wire.begin(), wire.end(), [](const Packet& a, const Packet& b) { return a.at < b.at; });
        while (!wire.empty() && wire.front().at <= t) {
            const Packet& p = wire.front();
            net::ByteReader r(p.body);
            if (p.id == kMsgSpawn) {
                std::vector<PedSpawnInfo> l;
                if (ReadSpawns(r, l))
                    for (const auto& s : l) rx.OnSpawn(0, s, rnow);
            } else {
                uint32_t ms = 0;
                std::vector<PedState> l;
                if (ReadStates(r, ms, l)) rx.OnStates(0, ms, l, rnow);
            }
            wire.pop_front();
        }
        // the receiver's tick (60 fps): its candidate, the replica report, the commands -> the AI walker
        rx.SetLocal(me, Camera{});
        rx.SetCandidates(adopted ? std::vector<LocalPed>{} : std::vector<LocalPed>{Ped(localId, w.x, w.y)});
        CopyReport rep;
        rep.pos = {w.x, w.y, 0};
        rep.heading[0] = w.hx;
        rep.heading[1] = w.hy;
        if (adopted) rx.SetReport(PedReceiver::Key(0, 1), rep, rnow);
        std::vector<RecvCmd> cmds;
        rx.Tick(rnow, dt, cmds);
        for (const auto& c : cmds) {
            if (c.type == RecvCmd::Type::Adopt && c.local == localId) adopted = true;
            if (c.type == RecvCmd::Type::Drive) w.Apply(c.lc, c.target.pos);
        }
        w.Step(dt);
        if (adopted && t > 2.0f) {
            const float e = Dist2({w.x, w.y, 0}, truth);
            errs.push_back(e);
            if (std::hypot(path.vx, path.vy) > 0.5f) errsMoving.push_back(e);
        }
    }
    const float p95 = Pct(errs, 95), p50 = Pct(errs, 50),
                mx = errs.empty() ? 0 : *std::max_element(errs.begin(), errs.end());
    char name[96];
    std::snprintf(name, sizeof name, "end to end %s: ped.p95 <= 1.5 m", label);
    Check(adopted && p95 <= 1.5f, name,
          Fmt("p50 %.2f p95 %.2f max %.2f m", p50, p95, mx) + Fmt(", moving p95 %.2f, snaps %.0f, delay %.0f ms",
                                                                  Pct(errsMoving, 95), w.snaps,
                                                                  rx.PlayoutDelay(0) * 1000));
}

// Coverage: 30 of the authority's pedestrians strolling within 40 m of the receiver, 30 humans of his own around
// (the same street, the same density), the camera turning as a player looks around, spawned copies ready 0.5 s after
// they are asked for.
void TestCoverage(bool lookAround, float minCoverage, const char* name) {
    Rng rng(lookAround ? 11 : 12);
    PedReceiver rx;
    net::authority::RegionAuthority region;
    std::vector<net::authority::PlayerSpot> spots = {{0, {10, 0, 0}, true}, {1, {0, 0, 0}, true}};
    region.SetPlayers(spots, 0);
    rx.SetRegion(region, 1);
    std::vector<Vec3> rp, rv;
    for (int i = 0; i < 30; ++i) {
        const float a = rng.Uniform(0, 6.283f), d = rng.Uniform(3, 38);
        rp.push_back({std::cos(a) * d, std::sin(a) * d, 0});
        const float b = rng.Uniform(0, 6.283f);
        rv.push_back({std::cos(b) * 1.2f, std::sin(b) * 1.2f, 0});
    }
    std::map<uint32_t, LocalPed> locals;
    for (int i = 0; i < 30; ++i) {
        const float a = rng.Uniform(0, 6.283f), d = rng.Uniform(3, 45);
        const uint32_t id = static_cast<uint32_t>(i + 1) * 256 + 8;
        locals[id] = Ped(id, std::cos(a) * d, std::sin(a) * d);
    }
    std::map<uint32_t, double> pendingSpawn;  // key -> ready at
    uint32_t nextLocal = 9000 * 256 + 8;
    std::vector<float> cover;
    const float dt = 1.0f / 30.0f;
    for (float t = 0; t < 20; t += dt) {
        for (int i = 0; i < 30; ++i) {
            rp[i].x += rv[i].x * dt;
            rp[i].y += rv[i].y * dt;
            if (std::hypot(rp[i].x, rp[i].y) > 38) {  // stays in the street near us
                rv[i].x = -rv[i].x;
                rv[i].y = -rv[i].y;
            }
        }
        if (static_cast<int>(t / dt) % 3 == 0) {
            std::vector<PedState> st;
            for (int i = 0; i < 30; ++i) {
                PedState s;
                s.net = static_cast<uint16_t>(i + 1);
                s.pos = rp[i];
                s.vel = rv[i];
                s.gait = 1;
                st.push_back(s);
            }
            rx.OnStates(0, static_cast<uint32_t>(t * 1000) + 100000, st, t);
        }
        const float camA = lookAround ? t * 0.6f : 0.0f;  // ~35 deg/s
        rx.SetLocal({0, 0, 0}, Camera{{0, 0, 1.7f}, {std::cos(camA), std::sin(camA), -0.1f}, true});
        std::vector<LocalPed> free;
        for (const auto& [id, p] : locals) free.push_back(p);
        rx.SetCandidates(free);
        std::vector<RecvCmd> cmds;
        rx.Tick(t, dt, cmds);
        for (const auto& c : cmds) {
            if (c.type == RecvCmd::Type::Adopt || c.type == RecvCmd::Type::Hide) locals.erase(c.local);
            if (c.type == RecvCmd::Type::Spawn) pendingSpawn[c.key] = t + 0.5;
        }
        for (auto it = pendingSpawn.begin(); it != pendingSpawn.end();) {
            if (it->second <= t) {
                rx.Spawned(it->first, nextLocal);
                nextLocal += 256;
                it = pendingSpawn.erase(it);
            } else {
                ++it;
            }
        }
        if (t > 10 && rx.Stats().nearby > 0)
            cover.push_back(100.0f * static_cast<float>(rx.Stats().nearReplica) /
                            static_cast<float>(rx.Stats().nearby));
    }
    float mean = 0;
    for (float c : cover) mean += c;
    mean /= std::max<size_t>(1, cover.size());
    const ReceiverStats& st = rx.Stats();
    Check(mean >= minCoverage, name,
          Fmt("coverage %.1f %%, adopted %.0f spawned %.0f", mean, st.adopted, st.spawned) +
              Fmt(", hidden %.0f, own left %.0f", st.hidden, locals.size()));
    Check(st.spawned + st.loading <= 10, "coverage: at most 10 spawned copies", Fmt("%.0f", st.spawned + st.loading));
}

// ---- the 2026-10-20 two-PC traces replayed (tests/data/ped_trace.h) ----------------------------------------------------
struct TPt {
    double t;  // s on the authority's clock since the session's T0
    float x, y;
};
using Track = std::vector<TPt>;

// Position at t, linear between rows; false in a gap longer than maxGap (the pedestrian was not sent then).
bool At(const Track& tr, double t, float& x, float& y, double maxGap = 1.0) {
    auto it = std::lower_bound(tr.begin(), tr.end(), t, [](const TPt& p, double v) { return p.t < v; });
    if (it == tr.begin() || it == tr.end()) {
        if (it != tr.end() && std::fabs(it->t - t) < 1e-6) {
            x = it->x;
            y = it->y;
            return true;
        }
        return false;
    }
    const TPt& b = *it;
    const TPt& a = *(it - 1);
    if (b.t - a.t > maxGap) return false;
    const float k = static_cast<float>((t - a.t) / std::max(1e-6, b.t - a.t));
    x = a.x + (b.x - a.x) * k;
    y = a.y + (b.y - a.y) * k;
    return true;
}

template <size_t N>
std::map<int, Track> Tracks(const int (&rows)[N][4]) {
    std::map<int, Track> out;
    for (const auto& r : rows) out[r[0]].push_back({r[1] / 1000.0, r[2] / 100.0f, r[3] / 100.0f});
    for (auto& [net, tr] : out)
        std::sort(tr.begin(), tr.end(), [](const TPt& a, const TPt& b) { return a.t < b.t; });
    return out;
}

// What the recorded copy did: the true error of every replica record (its spot vs the truth at its target time, as
// sync_compare's ped.p95), and whether it ever closed a gap (1 s windows starting > 1 m behind a target walking
// faster than 0.8 m/s: closing = the gap shrank by more than 0.2 m/s).
struct Recorded {
    std::vector<float> errs;
    int windows = 0, closing = 0;
    bool deaf() const { return windows >= 10 && closing * 10 < windows; }
};

Recorded RecordedOf(const Track& truth, const Track& rep, double deadAt) {
    Recorded r;
    std::vector<std::pair<double, float>> e;
    for (const TPt& p : rep) {
        if (p.t >= deadAt) break;
        float x, y;
        if (!At(truth, p.t, x, y)) continue;
        const float d = std::hypot(p.x - x, p.y - y);
        r.errs.push_back(d);
        e.push_back({p.t, d});
    }
    for (size_t i = 0, j = 0; i < e.size(); ++i) {
        if (e[i].second < 1.0f) continue;
        while (j < e.size() && e[j].first < e[i].first + 1.0) ++j;
        if (j >= e.size() || e[j].first > e[i].first + 1.3) continue;
        float x0, y0, x1, y1;
        if (!At(truth, e[i].first, x0, y0) || !At(truth, e[j].first, x1, y1)) continue;
        const double dt = e[j].first - e[i].first;
        if (std::hypot(x1 - x0, y1 - y0) / dt < 0.8) continue;
        ++r.windows;
        if ((e[j].second - e[i].second) / dt < -0.2) ++r.closing;
    }
    return r;
}

// One recorded pedestrian through the whole chain: its truth rows (5 Hz, interpolated to the authority's 20 Hz
// rounds) -> PedAuthority -> a 30 ms link -> PedReceiver (no local human to adopt: spawned copies, made 0.1 s after
// they are asked for) -> LocomotionController -> the simulated copy. The receiving player walks 8 m beside it.
struct ReplayOut {
    std::vector<float> errs;  // the copy vs the truth at its target time, 10 Hz, while alive and shown
    int snaps = 0, lagSnaps = 0, unresponsive = 0, spawns = 0;
    double sharedS = 0, ridingShownS = 0;  // s the receiver had a replica; ... while the truth moved > 8 m/s
    uint64_t rides = 0;
};

// The game's AI walk of a spawned copy, as the recorded copies show it: 1.5-1.7 m/s at multiplier 1.
AiWalker CopyWalker() {
    AiWalker w;
    w.realSpeed[1] = 1.6f;
    w.realSpeed[2] = 4.0f;
    w.realSpeed[3] = 6.0f;
    w.lag = 0.2f;
    return w;
}

// `deafTrack`: the recorded copy of a pedestrian whose copy never answered (A net 81): the replayed copy walks exactly
// as that one did (its own pace, deaf to multipliers and nudges), moved only by our snaps and respawns.
ReplayOut ReplayOne(const Track& truth, double deadAt, const Track* deafTrack, const ReceiverConfig& rc,
                    const AuthorityConfig& ac) {
    ReplayOut o;
    PedAuthority auth(ac);
    PedReceiver rx(rc);
    AiWalker w = CopyWalker();
    float offX = 0, offY = 0;  // deaf: our snaps' shift of the recorded copy
    const double kCopyLag = 0.15;  // the recorded copy's rows are stamped with its target's time (rT): about now - this
    auto deafAt = [&](double t, float& x, float& y) { return deafTrack && At(*deafTrack, t - kCopyLag, x, y); };
    const uint32_t pedId = 77 * 256 + 8, copyId = 5000 * 256 + 8;
    struct Pk {
        double at;
        uint16_t id;
        std::vector<uint8_t> body;
    };
    std::deque<Pk> wire;
    const double t0 = truth.front().t, t1 = std::min(truth.back().t, deadAt);
    const float dt = 1.0f / 60.0f;
    double roundAt = -1, sampleAt = 0, spawnAt = -1;
    uint32_t spawnKey = 0;
    bool have = false;
    float meX = 0, meY = 0;
    for (double t = t0; t < t1; t += dt) {
        float px, py;
        const bool present = At(truth, t, px, py);
        if (present) {
            meX = px + 6.0f;
            meY = py + 5.0f;
        }
        if (t - roundAt >= 0.05 - 1e-9) {
            roundAt = t;
            auth.SetPlayers({{0, {meX - 3, meY, 0}, true}, {1, {meX, meY, 0}, true}}, 0, 0);
            std::vector<LocalPed> peds;
            if (present) {
                LocalPed lp = Ped(pedId, px, py);
                float qx, qy;
                if (At(truth, t + 0.1, qx, qy) && std::hypot(qx - px, qy - py) > 0.01f) {
                    const float l = std::hypot(qx - px, qy - py);
                    lp.heading[0] = (qx - px) / l;
                    lp.heading[1] = (qy - py) / l;
                }
                peds.push_back(lp);
            }
            auth.SetPeds(peds, 1000.0 + t);
            std::vector<PeerOut> out;
            auth.Tick(1000.0 + t, static_cast<uint32_t>((1000.0 + t) * 1000.0), out);
            for (auto& po : out) {
                for (auto& b : po.gones) wire.push_back({t + 0.03, kMsgGone, b});
                for (auto& b : po.spawns) wire.push_back({t + 0.03, kMsgSpawn, b});
                for (auto& b : po.states) wire.push_back({t + 0.03, kMsgStates, b});
            }
        }
        const double rnow = 50.0 + t;
        while (!wire.empty() && wire.front().at <= t) {
            const Pk& p = wire.front();
            net::ByteReader r(p.body);
            if (p.id == kMsgSpawn) {
                std::vector<PedSpawnInfo> l;
                if (ReadSpawns(r, l))
                    for (const auto& s : l) rx.OnSpawn(0, s, rnow);
            } else if (p.id == kMsgGone) {
                std::vector<uint16_t> l;
                if (ReadGone(r, l))
                    for (uint16_t n : l) rx.OnGone(0, n, rnow);
            } else {
                uint32_t ms = 0;
                std::vector<PedState> l;
                if (ReadStates(r, ms, l)) rx.OnStates(0, ms, l, rnow);
            }
            wire.pop_front();
        }
        rx.SetLocal({meX, meY, 0}, Camera{});
        rx.SetCandidates({});
        if (have) {
            CopyReport rep;
            rep.pos = {w.x, w.y, 0};
            rep.heading[0] = w.hx;
            rep.heading[1] = w.hy;
            rx.SetReport(spawnKey, rep, rnow);
        }
        if (spawnAt >= 0 && t >= spawnAt) {
            rx.Spawned(spawnKey, copyId);
            have = true;
            spawnAt = -1;
        }
        std::vector<RecvCmd> cmds;
        rx.Tick(rnow, dt, cmds);
        for (const auto& c : cmds) {
            if (c.type == RecvCmd::Type::Spawn) {
                spawnKey = c.key;
                spawnAt = t + 0.1;
                const float hx = w.hx, hy = w.hy;
                w = CopyWalker();
                w.x = c.target.pos.x;
                w.y = c.target.pos.y;
                w.hx = hx;
                w.hy = hy;
                float rx0, ry0;
                if (deafAt(t, rx0, ry0)) {
                    offX = c.target.pos.x - rx0;
                    offY = c.target.pos.y - ry0;
                }
                ++o.spawns;
            } else if (c.type == RecvCmd::Type::Release) {
                have = false;
            } else if (c.type == RecvCmd::Type::Drive && have) {
                float rx0, ry0;
                if (c.lc.fix == LocomotionCommand::Fix::Snap) {
                    ++o.snaps;
                    if (deafAt(t, rx0, ry0)) {
                        offX = c.target.pos.x - rx0;
                        offY = c.target.pos.y - ry0;
                    }
                }
                w.Apply(c.lc, c.target.pos);
            }
        }
        if (have) {
            w.Step(dt);
            float rx0, ry0;
            if (deafAt(t, rx0, ry0)) {
                w.x = rx0 + offX;
                w.y = ry0 + offY;
            }
        }
        if (t >= sampleAt) {
            sampleAt = t + 0.1;
            for (const ReplicaInfo& i : rx.Replicas(rnow)) {
                if (!i.haveCopy || i.dead) continue;
                o.sharedS += 0.1;
                const double tt = i.targetT - 1000.0;  // the authority clock time its target stood for
                float tx, ty;
                if (!At(truth, tt, tx, ty)) continue;
                o.errs.push_back(std::hypot(w.x - tx, w.y - ty));
                float ax, ay;
                if (At(truth, tt - 0.25, ax, ay) && std::hypot(tx - ax, ty - ay) / 0.25 > 8.0) o.ridingShownS += 0.1;
            }
        }
    }
    o.lagSnaps = static_cast<int>(rx.Stats().lagSnaps);
    o.unresponsive = static_cast<int>(rx.Stats().unresponsive);
    o.rides = auth.Rides();
    return o;
}

// 0.16.0's rules (for the "before" runs): riders shared; no lag snap, no spawner health, 10 copies, 2 loading.
AuthorityConfig OldAuthority() {
    AuthorityConfig c;
    c.rideSpeed = 1e9f;
    return c;
}

ReceiverConfig OldRules() {
    ReceiverConfig c;
    c.lagTime = 1e9;
    c.maxSpawned = 10;
    c.maxLoading = 2;
    c.spawnWaitNone = 2.5;  // no camera on the guest's side of the replay: every spot counted as in sight
    c.spawnerFails = 1 << 30;
    c.massLoss = 1 << 30;
    return c;
}

template <size_t N>
double DeathOf(const int (&deaths)[N][3], int net) {
    double at = 1e18;
    for (const auto& d : deaths)
        if (d[0] == net && d[1] >= 0) at = std::min(at, d[1] / 1000.0);
    return at;
}

template <size_t NT, size_t NR, size_t ND>
void TestRecordedDrift(const char* tag, const int (&truthRows)[NT][4], const int (&repRows)[NR][4],
                       const int (&deaths)[ND][3], float wantP95) {
    const auto truth = Tracks(truthRows);
    const auto reps = Tracks(repRows);
    std::vector<float> recAll, before, after;
    int deafNets = 0, lagSnaps = 0, unresp = 0;
    uint64_t rides = 0;
    double ridingBefore = 0, ridingAfter = 0;
    std::string deafList;
    for (const auto& [net, rep] : reps) {
        auto ti = truth.find(net);
        if (ti == truth.end() || ti->second.size() < 4) continue;
        const double deadAt = DeathOf(deaths, net);
        const Recorded rec = RecordedOf(ti->second, rep, deadAt);
        if (rec.errs.empty()) continue;
        recAll.insert(recAll.end(), rec.errs.begin(), rec.errs.end());
        const bool deaf = rec.deaf();
        if (deaf) {
            ++deafNets;
            deafList += " " + std::to_string(net);
        }
        const Track* deafTrack = deaf ? &rep : nullptr;
        const ReplayOut b = ReplayOne(ti->second, deadAt, deafTrack, OldRules(), OldAuthority());
        const ReplayOut a = ReplayOne(ti->second, deadAt, deafTrack, ReceiverConfig{}, AuthorityConfig{});
        before.insert(before.end(), b.errs.begin(), b.errs.end());
        after.insert(after.end(), a.errs.begin(), a.errs.end());
        lagSnaps += a.lagSnaps;
        unresp += a.unresponsive;
        rides += a.rides;
        ridingBefore += b.ridingShownS;
        ridingAfter += a.ridingShownS;
    }
    const std::string n = std::string("recorded ") + tag + ": ";
    std::printf("      %s recorded copies: p50 %.2f p95 %.2f max %.2f m (%zu samples, as sync_compare); copies that "
                "never closed a gap:%s\n",
                tag, Pct(recAll, 50), Pct(recAll, 95), Pct(recAll, 100), recAll.size(),
                deafList.empty() ? " none" : deafList.c_str());
    std::printf("      %s replayed, 0.16.0 rules: p50 %.2f p95 %.2f max %.2f m; riders shown %.1f s\n", tag,
                Pct(before, 50), Pct(before, 95), Pct(before, 100), ridingBefore);
    Check(Pct(after, 95) <= wantP95, (n + "replayed ped.p95 within target").c_str(),
          Fmt("p50 %.2f p95 %.2f max %.2f m", Pct(after, 50), Pct(after, 95), Pct(after, 100)) +
              Fmt(" (target %.1f; lag snaps %.0f, released unresponsive %.0f)", wantP95, lagSnaps, unresp));
    Check(Pct(after, 95) < Pct(before, 95) || Pct(before, 95) <= wantP95, (n + "better than 0.16.0's rules").c_str(),
          Fmt("p95 %.2f -> %.2f m", Pct(before, 95), Pct(after, 95)));
    Check(ridingAfter <= 0.3, (n + "no copy follows a rider").c_str(),
          Fmt("%.1f s shown while its pedestrian rode (0.16.0: %.1f s), rides seen %.0f", ridingAfter, ridingBefore,
              static_cast<double>(rides)));
}

// Session A's coverage on the guest: every pedestrian the host sent him (its real walk), the guest's real walk, no
// local human to adopt (our spawners were blocked: PDL amb=0), the copy spawner ready 2.6 s after the first request
// (as recorded), then hidden at kPedASpawnerHiddenMs: every copy gone, every CreateObject failing until a new spawner
// (0.16.0: the Lua's 30 s idle reset; now the receiver asks for one).
struct CoverOut {
    double coverage = 0;
    uint64_t fails = 0, resets = 0;
    double brokenS = 0;
};

CoverOut ReplayCoverageA(const ReceiverConfig& rc, const AuthorityConfig& ac, bool luaIdleOnly) {
    CoverOut o;
    const auto truth = Tracks(kPedATruth);
    Track guest;
    for (const auto& g : kPedAGuest) guest.push_back({g[0] / 1000.0, g[1] / 100.0f, g[2] / 100.0f});
    PedAuthority auth(ac);
    PedReceiver rx(rc);
    struct Pk {
        double at;
        uint16_t id;
        std::vector<uint8_t> body;
    };
    std::deque<Pk> wire;
    const double hiddenAt = kPedASpawnerHiddenMs / 1000.0;
    double readyAt = -1, lastMade = -1e9, firstAsk = -1;
    bool broken = false;
    std::map<uint32_t, uint32_t> copies;  // key -> local id
    std::vector<std::pair<uint32_t, double>> asked;
    uint32_t nextId = 9000 * 256 + 8;
    uint64_t near = 0, rep = 0;
    double brokenSince = -1;
    const float dt = 1.0f / 30.0f;
    double roundAt = -1, sampleAt = 0;
    const double tEnd = guest.back().t;
    for (double t = guest.front().t; t < tEnd; t += dt) {
        float gx, gy;
        if (!At(guest, t, gx, gy, 2.0)) continue;
        if (t - roundAt >= 0.05 - 1e-9) {
            roundAt = t;
            auth.SetPlayers({{0, {gx + 3, gy, 0}, true}, {1, {gx, gy, 0}, true}}, 0, 0);
            std::vector<LocalPed> peds;
            // (a standing pedestrian's unchanged state went out once a second, PDT is 1 in 2: rows 2 s apart)
            for (const auto& [net, tr] : truth) {
                float px, py;
                if (At(tr, t, px, py, 2.5)) peds.push_back(Ped(static_cast<uint32_t>(net) * 256 + 8, px, py));
            }
            auth.SetPeds(peds, 1000.0 + t);
            std::vector<PeerOut> out;
            auth.Tick(1000.0 + t, static_cast<uint32_t>((1000.0 + t) * 1000.0), out);
            for (auto& po : out) {
                for (auto& b : po.gones) wire.push_back({t + 0.03, kMsgGone, b});
                for (auto& b : po.spawns) wire.push_back({t + 0.03, kMsgSpawn, b});
                for (auto& b : po.states) wire.push_back({t + 0.03, kMsgStates, b});
            }
        }
        const double rnow = 50.0 + t;
        while (!wire.empty() && wire.front().at <= t) {
            const Pk& p = wire.front();
            net::ByteReader r(p.body);
            if (p.id == kMsgSpawn) {
                std::vector<PedSpawnInfo> l;
                if (ReadSpawns(r, l))
                    for (const auto& s : l) rx.OnSpawn(0, s, rnow);
            } else if (p.id == kMsgGone) {
                std::vector<uint16_t> l;
                if (ReadGone(r, l))
                    for (uint16_t n : l) rx.OnGone(0, n, rnow);
            } else {
                uint32_t ms = 0;
                std::vector<PedState> l;
                if (ReadStates(r, ms, l)) rx.OnStates(0, ms, l, rnow);
            }
            wire.pop_front();
        }
        // the game's side: the spawner hidden; the old Lua's idle reset 30 s after its last copy
        if (!broken && t >= hiddenAt && hiddenAt > 0 && brokenSince < 0) {
            broken = true;
            brokenSince = t;
            for (const auto& [key, id] : copies) rx.LocalGone(id, rnow);
            copies.clear();
        }
        if (broken && luaIdleOnly && t - std::max(lastMade, brokenSince) > 30.0) {
            broken = false;
            readyAt = t + 0.1;
            o.brokenS += t - brokenSince;
        }
        rx.SetLocal({gx, gy, 0}, Camera{});
        rx.SetCandidates({});
        for (const auto& [key, id] : copies) {
            CopyReport cr;
            float x = gx, y = gy;
            const auto tr = truth.find(static_cast<int>(key & 0xFFFF));
            if (tr != truth.end()) At(tr->second, t - 0.15, x, y, 2.5);
            cr.pos = {x, y, 0};
            rx.SetReport(key, cr, rnow);
        }
        // the requests: failed at once by a hidden spawner, else made one per tick once the spawner is ready
        if (broken) {
            for (const auto& [key, at] : asked) rx.SpawnFailed(key, rnow, SpawnFail::Create);
            asked.clear();
        } else if (!asked.empty() && readyAt >= 0 && t >= readyAt) {
            const uint32_t key = asked.front().first;
            asked.erase(asked.begin());
            copies[key] = nextId;
            rx.Spawned(key, nextId);
            nextId += 256;
            lastMade = t;
        }
        std::vector<RecvCmd> cmds;
        rx.Tick(rnow, dt, cmds);
        for (const auto& c : cmds) {
            if (c.type == RecvCmd::Type::Spawn) {
                if (firstAsk < 0) {
                    firstAsk = t;
                    readyAt = t + 2.6;
                }
                asked.push_back({c.key, t});
            } else if (c.type == RecvCmd::Type::Release) {
                copies.erase(c.key);
                asked.erase(std::remove_if(asked.begin(), asked.end(),
                                           [&](const std::pair<uint32_t, double>& a) { return a.first == c.key; }),
                            asked.end());
            } else if (c.type == RecvCmd::Type::ResetSpawner) {
                for (const auto& [key, at] : asked) rx.SpawnFailed(key, rnow, SpawnFail::Reset);
                asked.clear();
                if (broken) o.brokenS += t - brokenSince;
                broken = false;
                readyAt = t + 0.1;
            }
        }
        if (t >= sampleAt) {
            sampleAt = t + 1.0;
            near += rx.Stats().nearby;
            rep += rx.Stats().nearReplica;
            if (std::getenv("PED_DEBUG"))
                std::printf("   t=%.0f near=%u rep=%u spawned=%u loading=%u known=%u fails=%llu broken=%d\n", t,
                            rx.Stats().nearby, rx.Stats().nearReplica, rx.Stats().spawned, rx.Stats().loading,
                            rx.Stats().known, static_cast<unsigned long long>(rx.Stats().spawnFails), broken ? 1 : 0);
        }
    }
    o.coverage = near ? 100.0 * static_cast<double>(rep) / static_cast<double>(near) : 0;
    o.fails = rx.Stats().spawnFails;
    o.resets = rx.Stats().spawnerResets;
    return o;
}

void TestRecordedCoverage() {
    const CoverOut b = ReplayCoverageA(OldRules(), OldAuthority(), true);
    const CoverOut a = ReplayCoverageA(ReceiverConfig{}, AuthorityConfig{}, false);
    std::printf("      session A coverage recorded 18.2 %% (139 spawn failures); replayed with 0.16.0's rules %.1f %% (%.0f "
                "failures, spawner broken %.0f s)\n",
                b.coverage, static_cast<double>(b.fails), b.brokenS);
    Check(a.coverage >= 80.0, "recorded A: replayed ped.coverage >= 80 %",
          Fmt("%.1f %% (0.16.0 rules: %.1f %%)", a.coverage, b.coverage) +
              Fmt(", %.0f failures, %.0f spawner resets", static_cast<double>(a.fails),
                  static_cast<double>(a.resets)) +
              Fmt(", spawner broken %.1f s", a.brokenS));
    Check(a.resets >= 1 && a.brokenS <= 1.0, "recorded A: the hidden spawner replaced within 1 s",
          Fmt("%.2f s (0.16.0: %.0f s)", a.brokenS, b.brokenS));
}

// The copy spawner's health on its own: CreateObject failing twice in a row, or every copy gone at once, asks for a
// new spawner (once; not while copies of it live); a reset's failures are retried at once.
void TestSpawnerHealth() {
    ReceiverConfig cfg;
    PedReceiver rx(cfg);
    double now = 0;
    std::map<uint16_t, Vec3> rp;
    for (uint16_t n = 1; n <= 4; ++n) rp[n] = {static_cast<float>(n) * 3, 5, 0};
    std::vector<RecvCmd> cmds;
    auto step = [&](double dt) {
        now += dt;
        rx.OnStates(0, static_cast<uint32_t>(now * 1000) + 5000, StatesAt(rp), now);
        rx.SetLocal({0, 0, 0}, Camera{});
        rx.SetCandidates({});
        cmds.clear();
        rx.Tick(now, static_cast<float>(dt), cmds);
    };
    auto count = [&](RecvCmd::Type ty) {
        size_t n = 0;
        for (const auto& c : cmds)
            if (c.type == ty) ++n;
        return n;
    };
    std::vector<uint32_t> keys;
    size_t spawnCmds = 0;
    for (int i = 0; i < 10; ++i) {
        step(0.1);
        for (const auto& c : cmds)
            if (c.type == RecvCmd::Type::Spawn) keys.push_back(c.key);
        spawnCmds += count(RecvCmd::Type::Spawn);
    }
    Check(spawnCmds == 4, "spawner: nothing to adopt -> every pedestrian asked for at once",
          Fmt("%.0f spawn requests in 1 s", spawnCmds));
    uint32_t id = 100 * 256 + 8;
    for (uint32_t k : keys) rx.Spawned(k, id += 256);
    step(0.1);
    Check(rx.Stats().spawned == 4, "spawner: 4 copies");
    // every copy gone at once (the spawner was hidden)
    for (uint32_t i = 0; i < 4; ++i) rx.LocalGone(100 * 256 + 8 + (i + 1) * 256, now);
    step(0.05);
    Check(count(RecvCmd::Type::ResetSpawner) == 1 && cmds.front().type == RecvCmd::Type::ResetSpawner &&
              cmds.front().why == RecvCmd::Why::CopiesLost,
          "spawner: every copy gone at once -> a new spawner", Fmt("%.0f lost", static_cast<double>(rx.Stats().lost)));
    // asked again soon; two CreateObject failures in a row -> another reset, but not within the 5 s gap
    keys.clear();
    for (int i = 0; i < 10; ++i) {
        step(0.1);
        for (const auto& c : cmds)
            if (c.type == RecvCmd::Type::Spawn) keys.push_back(c.key);
    }
    Check(keys.size() == 4, "spawner: asked again within 1 s", Fmt("%.0f", keys.size()));
    for (uint32_t k : keys) rx.SpawnFailed(k, now, SpawnFail::Create);
    step(0.1);
    Check(count(RecvCmd::Type::ResetSpawner) == 0, "spawner: no second reset within 5 s");
    size_t resets = 0;
    for (int i = 0; i < 60; ++i) {
        step(0.1);
        resets += count(RecvCmd::Type::ResetSpawner);
    }
    Check(resets == 1 && rx.Stats().spawnerResets == 2, "spawner: CreateObject failing -> a new spawner after the gap",
          Fmt("%.0f resets", static_cast<double>(rx.Stats().spawnerResets)));
    Check(rx.Stats().failsBy[static_cast<size_t>(SpawnFail::Create)] == 4, "spawner: failures counted by reason");
    // a living copy keeps its spawner: failures alone do not reset it then
    rx.Clear();
    keys.clear();
    for (int i = 0; i < 10; ++i) {
        step(0.1);
        for (const auto& c : cmds)
            if (c.type == RecvCmd::Type::Spawn) keys.push_back(c.key);
    }
    Check(keys.size() == 4, "spawner: (after Clear) asked for again", Fmt("%.0f", keys.size()));
    if (keys.empty()) return;
    rx.Spawned(keys[0], 555 * 256 + 8);
    for (size_t i = 1; i < keys.size(); ++i) rx.SpawnFailed(keys[i], now, SpawnFail::Create);
    resets = 0;
    for (int i = 0; i < 80; ++i) {
        step(0.1);
        resets += count(RecvCmd::Type::ResetSpawner);
    }
    Check(resets == 0, "spawner: not replaced while a copy of it lives");
}

// A tram passenger (session B, net 256: 11-12 m/s for 6 s): not shared while it rides, shared again after it walks.
void TestRider() {
    const auto truth = Tracks(kPedBTruth);
    const Track& tr = truth.at(256);
    PedAuthority auth;
    const double t0 = tr.front().t;
    int roundsShared = 0, rounds = 0;
    for (double t = t0; t < tr.back().t; t += 0.05) {
        float x, y;
        if (!At(tr, t, x, y)) continue;
        ++rounds;
        auth.SetPlayers({{0, {x + 5, y, 0}, true}, {1, {x, y + 5, 0}, true}}, 0, 0);
        auth.SetPeds({Ped(256 * 256 + 8, x, y)}, 1000 + t);
        std::vector<PeerOut> out;
        auth.Tick(1000 + t, static_cast<uint32_t>((1000 + t) * 1000), out);
        if (auth.OwnedCount() > 0) ++roundsShared;
    }
    // it rode the whole time it was recorded: shared only until the second fast round
    Check(rounds > 50 && roundsShared <= 2 && auth.Rides() == 1,
          "rider: a tram passenger (B net 256, 11-12 m/s) is not shared",
          Fmt("shared %.0f of %.0f rounds, rides %.0f", roundsShared, rounds, static_cast<double>(auth.Rides())));
    // and back once it walks: a walker after 3 s below 8 m/s is shared again
    PedAuthority a2;
    a2.SetPlayers({{0, {5, 0, 0}, true}, {1, {0, 5, 0}, true}}, 0, 0);
    double t = 0;
    float x = 0;
    for (; t < 1.0; t += 0.05, x += 0.6f) a2.SetPeds({Ped(9 * 256 + 8, x, 0)}, t);  // 12 m/s
    const bool notOwned = a2.OwnedCount() == 0 && a2.RidingCount() == 1 && a2.Foreign().empty();
    for (; t < 5.0; t += 0.05, x += 0.07f) a2.SetPeds({Ped(9 * 256 + 8, x, 0)}, t);  // 1.4 m/s
    Check(notOwned && a2.OwnedCount() == 1 && a2.RidingCount() == 0, "rider: shared again once it walks 3 s");
}

}  // namespace

int main() {
    TestCodec();
    TestRules();
    TestAuthority();
    TestRelay();
    TestMatch();
    TestReceiver();
    TestEndToEnd(15, 0, 1, "quiet link");
    TestEndToEnd(60, 2, 2, "60 ms jitter, 2 % loss");
    TestCoverage(true, 80.0f, "coverage: looking around >= 80 % (ped.coverage)");
    TestCoverage(false, 60.0f, "coverage: staring one way >= 60 %");
    TestSpawnerHealth();
    TestRider();
    TestRecordedDrift("A", kPedATruth, kPedAReplica, kPedADeaths, 1.5f);
    TestRecordedDrift("B", kPedBTruth, kPedBReplica, kPedBDeaths, 1.5f);
    TestRecordedCoverage();
    std::printf("%s (%d failure%s)\n", g_failures ? "FAILED" : "OK", g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
