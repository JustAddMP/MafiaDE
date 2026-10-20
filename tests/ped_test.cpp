// ped_test: unit tests for the pedestrian sync model (src/game/ped_model.h) under host authority
// (docs/host-authority.md WP-C): the wire format, the host's sender (interest 80 / 90 m, 48 per guest nearest first,
// rates, spawn / gone / death, the budget, a guest's game sharing nothing, several guests), a guest's copies-only
// receiver (every pedestrian of the host gets a copy; the pool: a gone pedestrian's copy hidden and given to the next
// one, the handover when every copy is in use, a cancelled request's late copy; the six-look rotation by net id and
// a profile that never loads dropped from it; deaths; the spawner's health), recorded-like walks through the whole
// chain (host -> lossy, jittery link -> replica::Stream -> the puppets' FootDriver -> a simulated AI walker) asserting
// ped.p95 <= 1.5 m and ped.coverage >= 80 %, and the 2026-10-20 two-PC traces (tests/data/ped_trace.h) replayed:
// every recorded pedestrian's real walk and its copy's recorded answer, the tram riders, and both whole sessions
// through the host's interest to a copies-only guest walking his real way with a model of the game's copy spawner
// (one profile load at a time, 0.8-2.5 s each, session A's hidden spawner) - before (0.16.0's rules,
// feat/sync-peds) and after; and every pedestrian of the 2026-10-20 one-PC runs (tests/data/ped_onepc_1020.h) through
// the chain with copies whose AI run does not reach the speed (as there). Prints PASS/FAIL per check; exits non-zero
// on any failure.
#include "game/ped_model.h"
#include "game/player_motion.h"

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

#include "data/ped_onepc_1020.h"
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
    const auto f = mc::game::pmotion::FootTuning();
    Check(t.snapDist == 5.0f && t.snapHeight == 3.0f && t.stuckDist == 0.8f && t.stuckTime == 3.0f &&
              t.walkSpeed == f.walkSpeed && t.runSpeed == f.runSpeed && t.stopDist == f.stopDist,
          "locomotion tuning = the puppets' speeds with Corrector::ForNpc's snap / stuck rules");
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

void Run(PedAuthority& a, std::map<uint8_t, Inbox>& inbox, const std::vector<LocalPed>& peds, double& t, int rounds,
         double step = 0.05) {
    for (int i = 0; i < rounds; ++i, t += step) {
        a.SetPeds(peds, t);
        std::vector<PeerOut> out;
        a.Tick(t, static_cast<uint32_t>(t * 1000), out);
        for (const auto& po : out) Deliver(po, inbox[po.peer]);
    }
}

void TestAuthority() {
    // host 0 at the origin, guest 1 at (30, 0)
    PedAuthority host;
    std::vector<PlayerView> players = {{0, {0, 0, 0}, true}, {1, {30, 0, 0}, true}};
    host.SetPlayers(players, 0, 0);
    // 2 m from the guest, 62 m (inside 80 now, outside 0.16's 50), 110 m (out), 75 m (in, at the edge)
    std::vector<LocalPed> peds = {Ped(8 + 256 * 1, 32, 2), Ped(8 + 256 * 2, 92, 0), Ped(8 + 256 * 3, 140, 0),
                                  Ped(8 + 256 * 4, 30, -75)};
    std::map<uint8_t, Inbox> inbox;
    double t = 0;
    for (int i = 0; i < 40; ++i, t += 0.05) {  // 2 s at 20 Hz rounds
        peds[0].pos.x = 32 + 0.065f * i;       // walks at 1.3 m/s
        peds[3].pos.x = 30 + 0.06f * i;        // and so does the one 75 m from him
        host.SetPeds(peds, t);
        std::vector<PeerOut> out;
        host.Tick(t, static_cast<uint32_t>(t * 1000), out);
        for (const auto& po : out) Deliver(po, inbox[po.peer]);
    }
    const Inbox& g = inbox[1];
    std::set<uint16_t> spawned;
    for (const auto& s : g.spawns) spawned.insert(s.net);
    const uint16_t nearNet = host.NetOf(peds[0].id), midNet = host.NetOf(peds[1].id),
                   farNet = host.NetOf(peds[2].id), edgeNet = host.NetOf(peds[3].id);
    Check(host.Shares() && host.OwnedCount() == 4, "host: shares every ambient human of his");
    Check(spawned.count(nearNet) && spawned.count(midNet) && spawned.count(edgeNet) && !spawned.count(farNet),
          "interest: within 80 m of the guest spawned to him (62 m and 75 m too), 110 m not",
          Fmt("%.0f spawned", spawned.size()));
    int nearStates = 0, edgeStates = 0;
    for (const auto& s : g.states) {
        if (s.net == nearNet) ++nearStates;
        if (s.net == edgeNet) ++edgeStates;
    }
    Check(nearStates >= 17 && nearStates <= 22, "rate: a pedestrian 2 m from the guest ~10 Hz",
          Fmt("%.0f in 2 s", nearStates));
    Check(edgeStates >= 5 && edgeStates < nearStates - 4, "rate: one 75 m away slower (~3 Hz)",
          Fmt("%.0f in 2 s", edgeStates));
    Check(inbox.count(0) == 0, "host: nothing sent to ourselves");
    PedState walked;
    for (const auto& s : g.states)
        if (s.net == nearNet) walked = s;
    Check(walked.gait == 1 && std::fabs(walked.vel.x - 1.3f) < 0.15f && walked.heading[1] >= 0,
          "measured velocity and gait of a walker", Fmt("vx %.2f gait %.0f", walked.vel.x, walked.gait));
    // death: once, reliably, and the state carries the flag
    peds[0].flags = kDead;
    size_t deaths0 = g.deaths.size();
    Run(host, inbox, peds, t, 10);
    bool deadState = false;
    for (const auto& s : inbox[1].states)
        if (s.net == nearNet && (s.flags & kDead)) deadState = true;
    Check(inbox[1].deaths.size() == deaths0 + 1 && deadState, "death: one PedDeath, states carry the flag");
    // the guest walks away (90 m+ from every pedestrian): gone
    players[1].pos = {300, 0, 0};
    host.SetPlayers(players, 0, 0);
    Run(host, inbox, peds, t, 4);
    Check(std::count(inbox[1].gones.begin(), inbox[1].gones.end(), nearNet) == 1, "interest: PedGone when he left");
    host.RemovePeer(1);
    Check(host.Stats(1) == nullptr, "a player who left is forgotten");

    // a guest's game never shares pedestrians (docs/host-authority.md: no world state from a guest)
    PedAuthority guest;
    guest.SetPlayers({{0, {0, 0, 0}, true}, {1, {30, 0, 0}, true}, {2, {700, 0, 0}, true}}, 0, 1);
    std::map<uint8_t, Inbox> gin;
    double gt = 0;
    Run(guest, gin, {Ped(1000 + 8, 35, 3), Ped(2000 + 8, 20, -5), Ped(3000 + 8, 702, 0)}, gt, 20);
    Check(!guest.Shares() && guest.OwnedCount() == 0 && gin.empty() && guest.Stats(0) == nullptr &&
              guest.Stats(2) == nullptr,
          "guest: shares nothing (not with the host, not with another guest 700 m away)");
}

// The cap and the budget: 60 pedestrians walking within 80 m of the guest: the nearest 48 are his, the bytes stay
// well under the 16 KB/s budget.
void TestCapAndBudget() {
    PedAuthority host;
    host.SetPlayers({{0, {0, 0, 0}, true}, {1, {0, 0, 0}, true}}, 0, 0);
    Rng rng(5);
    std::vector<LocalPed> peds;
    std::vector<Vec3> vel;
    for (int i = 0; i < 60; ++i) {
        const float a = rng.Uniform(0, 6.283f), d = 3 + 74.0f * static_cast<float>(i) / 60.0f;
        peds.push_back(Ped(static_cast<uint32_t>(i + 1) * 256 + 8, std::cos(a) * d, std::sin(a) * d));
        const float b = rng.Uniform(0, 6.283f);
        vel.push_back({std::cos(b) * 1.3f, std::sin(b) * 1.3f, 0});
    }
    std::map<uint8_t, Inbox> inbox;
    double t = 0;
    std::set<uint16_t> got;
    std::vector<LocalPed> atFirst;
    for (int i = 0; i < 200; ++i, t += 0.05) {  // 10 s
        for (size_t k = 0; k < peds.size(); ++k) {
            peds[k].pos.x += vel[k].x * 0.05f * (i < 4 ? 0 : 1);
            peds[k].pos.y += vel[k].y * 0.05f * (i < 4 ? 0 : 1);
        }
        host.SetPeds(peds, t);
        std::vector<PeerOut> out;
        host.Tick(t, static_cast<uint32_t>(t * 1000), out);
        for (const auto& po : out) Deliver(po, inbox[po.peer]);
        if (got.empty() && !inbox[1].spawns.empty()) {  // the first admission: who came in
            for (const auto& s : inbox[1].spawns) got.insert(s.net);
            atFirst = peds;
        }
    }
    const auto* st = host.Stats(1);
    float maxIn = 0, minOut = 1e9f;
    for (const LocalPed& p : atFirst) {
        const float d = std::hypot(p.pos.x, p.pos.y);
        if (d > 80) continue;
        if (got.count(host.NetOf(p.id)))
            maxIn = std::max(maxIn, d);
        else
            minOut = std::min(minOut, d);
    }
    Check(st && st->admitted == 48 && got.size() == 48, "cap: 48 pedestrians for one guest (of 60 within 80 m)",
          Fmt("%.0f admitted of %.0f relevant", st ? st->admitted : 0, st ? st->relevant : 0));
    Check(maxIn <= minOut, "cap: the nearest first",
          Fmt("farthest admitted %.1f m, nearest left out %.1f m", maxIn, minOut));
    const double kbs = static_cast<double>(inbox[1].bytes) / 1024.0 / 10.0;
    Check(kbs <= 8.0, "budget: 48 walkers well under 16 KB/s", Fmt("%.2f KB/s", kbs));
}

// Several guests: each gets his own pedestrians from the host (one PeerOut each); no guest -> guest relay exists.
void TestTwoGuests() {
    PedAuthority host;
    host.SetPlayers({{0, {0, 0, 0}, true}, {1, {10, 0, 0}, true}, {2, {300, 0, 0}, true}}, 0, 0);
    std::map<uint8_t, Inbox> inbox;
    double t = 0;
    Run(host, inbox, {Ped(8 + 256, 12, 0), Ped(8 + 512, 305, 3), Ped(8 + 768, 150, 0)}, t, 4);
    std::set<uint16_t> g1, g2;
    for (const auto& s : inbox[1].spawns) g1.insert(s.net);
    for (const auto& s : inbox[2].spawns) g2.insert(s.net);
    Check(g1 == std::set<uint16_t>({host.NetOf(8 + 256)}) && g2 == std::set<uint16_t>({host.NetOf(8 + 512)}),
          "two guests: each his own near pedestrians, straight from the host");
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

// A small rig around one PedReceiver: the host's pedestrians (rp) sent every step, the commands collected, copies
// reported where their pedestrian is (unless `away`: then 200 m off, out of anybody's sight).
struct Rig {
    PedReceiver rx;
    std::map<uint16_t, Vec3> rp;
    std::vector<RecvCmd> cmds, all;
    std::map<uint32_t, uint32_t> copyOf;  // key -> local (as the Lua holds it)
    std::map<uint16_t, Vec3> last;        // where each pedestrian was last (a gone one's copy stays there)
    double now = 0;
    bool reportAway = false;
    explicit Rig(const ReceiverConfig& c = {}) : rx(c) {}
    void Step(double dt = 0.1) {
        now += dt;
        for (const auto& [n, p] : rp) last[n] = p;
        rx.OnStates(0, static_cast<uint32_t>(now * 1000) + 5000, StatesAt(rp), now);
        rx.SetLocal({0, 0, 0}, Camera{{0, 0, 1.7f}, {1, 0, 0}, true});
        for (const auto& [key, local] : copyOf) {
            auto it = last.find(static_cast<uint16_t>(key & 0xFFFF));
            CopyReport r;
            r.pos = reportAway || it == last.end() ? Vec3{-200, 0, 0} : it->second;
            rx.SetReport(key, r, now);
        }
        cmds.clear();
        rx.Tick(now, static_cast<float>(dt), cmds);
        for (const auto& c : cmds) {
            if (c.type == RecvCmd::Type::Reuse) copyOf[c.key] = c.local;
            if (c.type == RecvCmd::Type::Hide || c.type == RecvCmd::Type::Remove)
                for (auto it = copyOf.begin(); it != copyOf.end();)
                    it = it->second == c.local ? copyOf.erase(it) : std::next(it);
        }
        all.insert(all.end(), cmds.begin(), cmds.end());
    }
    size_t Count(RecvCmd::Type t, const std::vector<RecvCmd>* list = nullptr) const {
        size_t n = 0;
        for (const auto& c : list ? *list : cmds)
            if (c.type == t) ++n;
        return n;
    }
    void Made(uint32_t key, uint32_t local) {
        rx.Spawned(key, local, now);
        copyOf[key] = local;
    }
};

void TestReceiver() {
    Rig g;
    // the host's pedestrians: 1 and 2 in view in front of us, 3 behind us
    g.rp = {{1, {20, 0, 0}}, {2, {15, 5, 0}}, {3, {-30, 10, 0}}};
    std::map<uint32_t, uint8_t> asked;
    for (int i = 0; i < 5; ++i) {
        g.Step();
        for (const auto& c : g.cmds)
            if (c.type == RecvCmd::Type::Spawn) asked[c.key] = c.profile;
    }
    Check(asked.size() == 3 && g.Count(RecvCmd::Type::Spawn, &g.all) == 3,
          "copies: every pedestrian of the host asked for once (nothing adopted, nothing hidden of ours)",
          Fmt("%.0f asked", asked.size()));
    Check(asked[PedReceiver::Key(0, 1)] == g.rx.ProfileFor(1) && asked[PedReceiver::Key(0, 3)] == 3 % 6,
          "copies: the profile by net id");
    Check(g.rx.Stats().nearby == 3 && g.rx.Stats().nearReplica == 0 && g.rx.Stats().loading == 3, "coverage counts",
          Fmt("near %.0f rep %.0f loading %.0f", g.rx.Stats().nearby, g.rx.Stats().nearReplica, g.rx.Stats().loading));
    g.Made(PedReceiver::Key(0, 1), 101 * 256 + 8);
    g.Made(PedReceiver::Key(0, 2), 102 * 256 + 8);
    g.Made(PedReceiver::Key(0, 3), 103 * 256 + 8);
    g.Step();
    Check(g.rx.Stats().nearReplica == 3 && g.rx.Stats().copies == 3 && g.rx.PoolSize() == 3, "spawned copies count");
    std::set<uint32_t> driven;
    for (const auto& c : g.cmds)
        if (c.type == RecvCmd::Type::Drive) driven.insert(c.key);
    Check(driven.size() == 3, "drive: every copy every tick", Fmt("%.0f", driven.size()));
    // our copy died here: told once
    CopyReport dead;
    dead.pos = g.rp[1];
    dead.dead = true;
    g.rx.SetReport(PedReceiver::Key(0, 1), dead, g.now);
    Check(g.rx.DiedHere(PedReceiver::Key(0, 1)) && !g.rx.DiedHere(PedReceiver::Key(0, 1)), "died here: once");
    // the host's death: a Kill for its copy
    g.rx.OnDeath(0, 2, g.rp[2], g.now);
    g.Step();
    bool killed = false;
    for (const auto& c : g.cmds)
        if (c.type == RecvCmd::Type::Kill && c.key == PedReceiver::Key(0, 2) && c.local == 102 * 256 + 8) killed = true;
    Check(killed, "death: the copy is killed");
    // gone: pedestrian 3 (behind us): its copy goes to the pool, hidden at once (nobody looks); 2 (a body) removed
    g.rx.OnGone(0, 3, g.now);
    g.rx.OnGone(0, 2, g.now);
    g.rp.erase(3);
    g.rp.erase(2);
    g.Step();
    bool hid3 = false, rem2 = false;
    for (const auto& c : g.cmds) {
        if (c.type == RecvCmd::Type::Hide && c.local == 103 * 256 + 8) hid3 = true;
        if (c.type == RecvCmd::Type::Remove && c.local == 102 * 256 + 8 && c.why == RecvCmd::Why::Body) rem2 = true;
    }
    Check(hid3 && g.rx.IsFree(103 * 256 + 8) && g.rx.Stats().free == 1, "pool: a gone pedestrian's copy hidden, kept");
    Check(rem2 && g.rx.PoolSize() == 2, "pool: a body is removed, never reused");
    // a new pedestrian: the pool copy is put there and shown, nothing spawned
    g.rp[7] = {10, -3, 0};
    for (int i = 0; i < 4; ++i) g.Step();
    bool reused = false;
    for (const auto& c : g.all)
        if (c.type == RecvCmd::Type::Reuse && c.key == PedReceiver::Key(0, 7) && c.local == 103 * 256 + 8 &&
            Dist2(c.target.pos, {10, -3, 0}) < 0.5f)
            reused = true;
    size_t spawns = 0;
    for (const auto& c : g.all)
        if (c.type == RecvCmd::Type::Spawn && c.key == PedReceiver::Key(0, 7)) ++spawns;
    Check(reused && spawns == 0 && g.rx.Stats().reuses == 1 && !g.rx.IsFree(103 * 256 + 8),
          "pool: the next new pedestrian gets the hidden copy (put at its spot), no spawn");
    // a gone pedestrian's copy in view waits for nobody looking, at the latest the grace
    g.rx.OnGone(0, 7, g.now);
    g.rp.erase(7);
    g.all.clear();
    g.reportAway = false;
    g.Step();
    Check(g.Count(RecvCmd::Type::Hide) == 0 && g.rx.IsFree(103 * 256 + 8), "pool: a copy in view is not hidden at once");
    // (its last spot, 10 m in front of us, stays in view)
    for (int i = 0; i < 35 && g.Count(RecvCmd::Type::Hide, &g.all) == 0; ++i) g.Step();
    Check(g.Count(RecvCmd::Type::Hide, &g.all) == 1, "pool: ... hidden after the grace");
    // the host leaves: every pedestrian of his goes, copies to the pool
    g.rx.ForgetAuthority(0);
    g.rp.clear();
    g.Step();
    Check(g.rx.Stats().copies == 0 && g.rx.Stats().known == 0, "the host left: no copy stands for anybody");
}

// Every copy in use (maxCopies 3): a pedestrian near us takes the copy of a far one (out of sight), not the other way.
void TestPoolFull() {
    ReceiverConfig c;
    c.maxCopies = 3;
    Rig g(c);
    g.rp = {{1, {10, 0, 0}}, {2, {-20, 0, 0}}, {3, {-70, 0, 0}}};
    for (int i = 0; i < 4; ++i) g.Step();
    g.Made(PedReceiver::Key(0, 1), 201 * 256 + 8);
    g.Made(PedReceiver::Key(0, 2), 202 * 256 + 8);
    g.Made(PedReceiver::Key(0, 3), 203 * 256 + 8);
    g.Step();
    g.all.clear();
    g.rp[4] = {-5, 3, 0};  // a new one 6 m from us
    g.rp[5] = {-60, 2, 0};  // and one 60 m away: no copy for it (not 15 m nearer than anybody's)
    for (int i = 0; i < 5; ++i) g.Step();
    bool handed = false;
    for (const auto& cmd : g.all)
        if (cmd.type == RecvCmd::Type::Reuse && cmd.key == PedReceiver::Key(0, 4) && cmd.local == 203 * 256 + 8 &&
            cmd.why == RecvCmd::Why::Handover)
            handed = true;
    Check(handed && g.Count(RecvCmd::Type::Spawn, &g.all) == 0 && g.rx.Stats().handovers == 1,
          "pool full: the near one takes the copy of the farthest (70 m), no spawn beyond 3",
          Fmt("handovers %.0f", static_cast<double>(g.rx.Stats().handovers)));
    // poolKeep 1: two pedestrians go: one copy kept hidden, the other removed
    ReceiverConfig k;
    k.poolKeep = 1;
    Rig h(k);
    h.rp = {{1, {-10, 0, 0}}, {2, {-20, 0, 0}}};
    for (int i = 0; i < 4; ++i) h.Step();
    h.Made(PedReceiver::Key(0, 1), 301 * 256 + 8);
    h.Made(PedReceiver::Key(0, 2), 302 * 256 + 8);
    h.Step();
    h.all.clear();
    h.rx.OnGone(0, 1, h.now);
    h.rx.OnGone(0, 2, h.now);
    h.rp.clear();
    h.Step();
    Check(h.Count(RecvCmd::Type::Hide, &h.all) == 1 && h.Count(RecvCmd::Type::Remove, &h.all) == 1 &&
              h.rx.PoolSize() == 1,
          "pool: at most poolKeep free copies kept");
    // a request cancelled while loading: the copy that comes anyway joins the pool
    Rig q;
    q.rp = {{9, {5, 5, 0}}};
    for (int i = 0; i < 4; ++i) q.Step();
    q.rx.OnGone(0, 9, q.now);
    q.rp.clear();
    q.Step();
    bool cancelled = false;
    for (const auto& cmd : q.cmds)
        if (cmd.type == RecvCmd::Type::Remove && cmd.local == 0 && cmd.why == RecvCmd::Why::Cancelled) cancelled = true;
    q.rx.Spawned(PedReceiver::Key(0, 9), 401 * 256 + 8, q.now);
    q.Step();
    Check(cancelled && q.rx.IsFree(401 * 256 + 8), "pool: a cancelled request's late copy is kept as a free one");
}

// The six looks: by net id; a profile that does not load (twice, before it ever worked) leaves the rotation; one that
// worked stays; the last one always stays.
void TestRotation() {
    Check(kCopyProfileCount == 6 && std::string(kCopyProfiles[0]) == "characters/traffic/traffic_base_ambient_combined" &&
              std::string(kCopyProfiles[5]) == "characters/traffic/traffic_workers_male",
          "rotation: six civilian profiles (docs/game-api.md 19)");
    Rig g;
    for (uint16_t n = 1; n <= 12; ++n) g.rp[n] = {static_cast<float>(n) * 2, 3, 0};
    std::map<uint16_t, uint8_t> prof;
    ReceiverConfig c;
    c.maxLoading = 12;
    Rig r(c);
    r.rp = g.rp;
    for (int i = 0; i < 4; ++i) {
        r.Step();
        for (const auto& cmd : r.cmds)
            if (cmd.type == RecvCmd::Type::Spawn) prof[static_cast<uint16_t>(cmd.key)] = cmd.profile;
    }
    std::set<uint8_t> used;
    bool byNet = prof.size() == 12;
    for (const auto& [n, p] : prof) {
        used.insert(p);
        if (p != n % 6) byNet = false;
    }
    Check(byNet && used.size() == 6, "rotation: every look in use, the same net -> the same look",
          Fmt("%.0f looks over %.0f copies", used.size(), prof.size()));
    r.Made(PedReceiver::Key(0, 6), 606 * 256 + 8);  // net 6 -> look 0: it works
    // profile 2 (nets 2, 8) does not load: the Lua fails both requests at once (counted once); its pedestrians get
    // another look while it rests
    r.rx.SpawnFailed(PedReceiver::Key(0, 2), r.now, SpawnFail::NoProfile);
    r.rx.SpawnFailed(PedReceiver::Key(0, 8), r.now, SpawnFail::NoProfile);
    Check(r.rx.ProfileLive(2), "rotation: one failed load is not enough");
    r.all.clear();
    for (int i = 0; i < 10; ++i) r.Step();
    uint8_t meanwhile = 255;
    for (const auto& cmd : r.all)
        if (cmd.type == RecvCmd::Type::Spawn && cmd.key == PedReceiver::Key(0, 2)) meanwhile = cmd.profile;
    Check(meanwhile != 2 && meanwhile != 255, "rotation: a look that failed once rests (its pedestrians get another)",
          Fmt("net 2: look %.0f", meanwhile));
    // a minute later it is tried again (new pedestrians 14 and 20 -> look 2) and fails again: dropped
    r.Step(61.0);
    r.rp[14] = {30, 3, 0};
    r.rp[20] = {32, 3, 0};
    r.all.clear();
    for (int i = 0; i < 5; ++i) r.Step();
    uint8_t retry = 255;
    for (const auto& cmd : r.all)
        if (cmd.type == RecvCmd::Type::Spawn && cmd.key == PedReceiver::Key(0, 14)) retry = cmd.profile;
    r.rx.SpawnFailed(PedReceiver::Key(0, 14), r.now, SpawnFail::NoProfile);
    r.rx.SpawnFailed(PedReceiver::Key(0, 20), r.now, SpawnFail::NoProfile);
    r.all.clear();
    for (int i = 0; i < 5; ++i) r.Step();
    uint8_t after = 255;
    for (const auto& cmd : r.all)
        if (cmd.type == RecvCmd::Type::Spawn && cmd.key == PedReceiver::Key(0, 14)) after = cmd.profile;
    bool never2 = true;
    for (uint16_t n = 1; n < 200; ++n)
        if (r.rx.ProfileFor(n) == 2) never2 = false;
    Check(retry == 2 && !r.rx.ProfileLive(2) && never2 && after != 2 && after != 255 &&
              r.rx.Stats().profilesDropped == 1 && r.rx.Stats().profilesLive == 5,
          "rotation: a look failing twice before it worked is dropped, its pedestrians get another",
          Fmt("net 14: look %.0f -> %.0f", retry, after));
    // profile 0 worked once: failures afterwards never drop it
    Check(r.rx.ProfileWorked(0), "rotation: a copy made marks its look as working");
    for (int i = 0; i < 3; ++i) {
        r.now += 2;
        r.rx.SpawnFailed(PedReceiver::Key(0, 12), r.now, SpawnFail::NoProfile);
    }
    Check(r.rx.ProfileLive(0), "rotation: a look that worked stays");
    // every other one fails: the last one stays
    Rig z;
    z.rp = {{1, {3, 3, 0}}};
    for (int round = 0; round < 200; ++round) {
        z.Step(2.0);
        for (const auto& cmd : z.cmds)
            if (cmd.type == RecvCmd::Type::Spawn) z.rx.SpawnFailed(cmd.key, z.now, SpawnFail::NoProfile);
    }
    Check(z.rx.Stats().profilesLive == 1 && z.rx.Stats().profilesDropped == 5,
          "rotation: when every look fails, the last one stays",
          Fmt("%.0f left", static_cast<double>(z.rx.Stats().profilesLive)));
}
// ---- end to end: one pedestrian's walk through the whole chain ------------------------------------------------------
// The game's AI walk of the replica: commands act after a lag, gait speeds off by up to 20 %, a turn rate, braking.
struct AiWalker {
    float realSpeed[4] = {0, 1.3f * 0.85f, 4.0f * 1.15f, 6.0f * 0.9f};
    float cap[4] = {0, 1e9f, 1e9f, 1e9f};  // m/s the AI reaches at most in a gait, whatever the multiplier
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
        (void)target;  // a stop walks to the command's point too (ped_sync.lua P.Drive: the FootDriver's stop point)
        queue.push_back({clock + lag, c.gait, c.speedMult, c.reissue, c.moveTo.x, c.moveTo.y});
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
            want = std::min(realSpeed[static_cast<int>(gait)] * mult, cap[static_cast<int>(gait)]);
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
// ---- the 2026-10-20 two-PC traces replayed (tests/data/ped_trace.h) -------------------------------------------------
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

void TestEndToEnd(float jitterMs, float lossPct, uint64_t seed, const char* label) {
    Rng rng(seed);
    PedAuthority auth;
    ReceiverConfig rc;
    PedReceiver rx(rc);
    Path path;
    path.x = 100;
    path.y = 50;
    AiWalker w;
    const uint32_t copyId = 4000 * 256 + 8, myOwnId = 99 * 256 + 8;
    const double authEpoch = 1234.5, recvEpoch = 77.25;  // the two clocks differ
    std::deque<Packet> wire;
    std::vector<float> errs, errsMoving;
    const float dt = 1.0f / 60.0f;
    const float total = path.Length();
    double roundAt = 0, spawnAt = -1;
    uint32_t key = 0;
    bool have = false;
    for (float t = 0; t < total; t += dt) {
        path.Step(t, dt);
        const Vec3 truth{path.x, path.y, 0};
        // the receiving player walks along 8 m behind
        const Vec3 me{path.x - 8, path.y, 0};
        // the host's rounds at 20 Hz
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
        // the guest's tick (60 fps): the copy's report, the commands -> the AI walker; the copy comes 0.3 s after it
        // is asked for (where its pedestrian was then)
        rx.SetLocal(me, Camera{});
        if (have) {
            CopyReport rep;
            rep.pos = {w.x, w.y, 0};
            rep.heading[0] = w.hx;
            rep.heading[1] = w.hy;
            rx.SetReport(key, rep, rnow);
        }
        if (spawnAt >= 0 && t >= spawnAt) {
            rx.Spawned(key, copyId, rnow);
            have = true;
            spawnAt = -1;
        }
        std::vector<RecvCmd> cmds;
        rx.Tick(rnow, dt, cmds);
        for (const auto& c : cmds) {
            if (c.type == RecvCmd::Type::Spawn) {
                key = c.key;
                spawnAt = t + 0.3;
                w.x = c.target.pos.x;
                w.y = c.target.pos.y;
            }
            if (c.type == RecvCmd::Type::Drive) w.Apply(c.lc, c.target.pos);
        }
        if (have) w.Step(dt);
        if (have && t > 2.0f) {
            const float e = Dist2({w.x, w.y, 0}, truth);
            errs.push_back(e);
            if (std::hypot(path.vx, path.vy) > 0.5f) errsMoving.push_back(e);
        }
    }
    const float p95 = Pct(errs, 95), p50 = Pct(errs, 50),
                mx = errs.empty() ? 0 : *std::max_element(errs.begin(), errs.end());
    char name[96];
    std::snprintf(name, sizeof name, "end to end %s: ped.p95 <= 1.5 m", label);
    Check(have && p95 <= 1.5f, name,
          Fmt("p50 %.2f p95 %.2f max %.2f m", p50, p95, mx) + Fmt(", moving p95 %.2f, snaps %.0f, delay %.0f ms",
                                                                  Pct(errsMoving, 95), w.snaps,
                                                                  rx.PlayoutDelay(0) * 1000));
}

// Coverage: 30 of the host's pedestrians strolling within 40 m of the guest, the camera turning as a player looks
// around (or staring one way), copies ready 0.5 s after they are asked for.
void TestCoverage(bool lookAround, float minCoverage, const char* name) {
    Rng rng(lookAround ? 11 : 12);
    PedReceiver rx;
    std::vector<Vec3> rp, rv;
    for (int i = 0; i < 30; ++i) {
        const float a = rng.Uniform(0, 6.283f), d = rng.Uniform(3, 38);
        rp.push_back({std::cos(a) * d, std::sin(a) * d, 0});
        const float b = rng.Uniform(0, 6.283f);
        rv.push_back({std::cos(b) * 1.2f, std::sin(b) * 1.2f, 0});
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
        std::vector<RecvCmd> cmds;
        rx.Tick(t, dt, cmds);
        for (const auto& c : cmds)
            if (c.type == RecvCmd::Type::Spawn) pendingSpawn[c.key] = t + 0.5;
        for (auto it = pendingSpawn.begin(); it != pendingSpawn.end();) {
            if (it->second <= t) {
                rx.Spawned(it->first, nextLocal, t);
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
    Check(mean >= minCoverage, name, Fmt("coverage %.1f %%, copies %.0f", mean, st.copies));
    Check(rx.PoolSize() + st.loading <= 40, "coverage: at most 40 copies", Fmt("%.0f", rx.PoolSize() + st.loading));
}

// The copy spawner's health on its own: CreateObject failing twice in a row, or every copy gone at once, asks for a
// new spawner (once; not while copies of it live, free ones included); a reset's failures are retried at once; a
// reset the Lua made itself (PFR) is not answered with another one.
void TestSpawnerHealth() {
    Rig g;
    for (uint16_t n = 1; n <= 4; ++n) g.rp[n] = {static_cast<float>(n) * 3, 5, 0};
    std::vector<uint32_t> keys;
    for (int i = 0; i < 10; ++i) {
        g.Step();
        for (const auto& c : g.cmds)
            if (c.type == RecvCmd::Type::Spawn) keys.push_back(c.key);
    }
    Check(keys.size() == 4, "spawner: every pedestrian asked for within 1 s", Fmt("%.0f spawn requests", keys.size()));
    uint32_t id = 100 * 256 + 8;
    for (uint32_t k : keys) g.Made(k, id += 256);
    g.Step();
    Check(g.rx.Stats().copies == 4, "spawner: 4 copies");
    // every copy gone at once (the spawner was hidden)
    for (uint32_t i = 0; i < 4; ++i) g.rx.LocalGone(100 * 256 + 8 + (i + 1) * 256, g.now);
    g.copyOf.clear();
    g.Step(0.05);
    Check(g.Count(RecvCmd::Type::ResetSpawner) == 1 && g.cmds.front().type == RecvCmd::Type::ResetSpawner &&
              g.cmds.front().why == RecvCmd::Why::CopiesLost,
          "spawner: every copy gone at once -> a new spawner",
          Fmt("%.0f lost", static_cast<double>(g.rx.Stats().lost)));
    // asked again soon; two CreateObject failures in a row -> another reset, but not within the 5 s gap
    keys.clear();
    for (const auto& c : g.cmds)
        if (c.type == RecvCmd::Type::Spawn) keys.push_back(c.key);
    for (int i = 0; i < 10; ++i) {
        g.Step();
        for (const auto& c : g.cmds)
            if (c.type == RecvCmd::Type::Spawn) keys.push_back(c.key);
    }
    Check(keys.size() == 4, "spawner: asked again within 1 s", Fmt("%.0f", keys.size()));
    for (uint32_t k : keys) g.rx.SpawnFailed(k, g.now, SpawnFail::Create);
    g.Step();
    Check(g.Count(RecvCmd::Type::ResetSpawner) == 0, "spawner: no second reset within 5 s");
    size_t resets = 0;
    for (int i = 0; i < 60; ++i) {
        g.Step();
        resets += g.Count(RecvCmd::Type::ResetSpawner);
    }
    Check(resets == 1 && g.rx.Stats().spawnerResets == 2, "spawner: CreateObject failing -> a new spawner after the gap",
          Fmt("%.0f resets", static_cast<double>(g.rx.Stats().spawnerResets)));
    Check(g.rx.Stats().failsBy[static_cast<size_t>(SpawnFail::Create)] == 4, "spawner: failures counted by reason");
    // a living copy keeps its spawner (a free one in the pool too): failures alone do not reset it then
    g.rx.Clear();
    g.copyOf.clear();
    keys.clear();
    for (int i = 0; i < 10; ++i) {
        g.Step();
        for (const auto& c : g.cmds)
            if (c.type == RecvCmd::Type::Spawn) keys.push_back(c.key);
    }
    Check(keys.size() == 4, "spawner: (after Clear) asked for again", Fmt("%.0f", keys.size()));
    if (keys.empty()) return;
    g.Made(keys[0], 555 * 256 + 8);
    g.rx.OnGone(0, static_cast<uint16_t>(keys[0]), g.now);  // its pedestrian goes: the copy is a free one now
    g.rp.erase(static_cast<uint16_t>(keys[0]));
    for (size_t i = 1; i < keys.size(); ++i) g.rx.SpawnFailed(keys[i], g.now, SpawnFail::Create);
    resets = 0;
    for (int i = 0; i < 80; ++i) {
        g.Step();
        resets += g.Count(RecvCmd::Type::ResetSpawner);
        for (const auto& c : g.cmds)
            if (c.type == RecvCmd::Type::Spawn) g.rx.SpawnFailed(c.key, g.now, SpawnFail::Create);
    }
    Check(resets == 0 && g.rx.PoolSize() >= 1, "spawner: not replaced while a copy of it lives (a pool one too)");
    // the Lua reset its spawner itself: its copies' PGLs, then PFR: no reset asked on top
    Rig h;
    h.rp = {{1, {3, 3, 0}}, {2, {5, 5, 0}}, {3, {7, 7, 0}}};
    for (int i = 0; i < 4; ++i) h.Step();
    h.Made(PedReceiver::Key(0, 1), 701 * 256 + 8);
    h.Made(PedReceiver::Key(0, 2), 702 * 256 + 8);
    h.Made(PedReceiver::Key(0, 3), 703 * 256 + 8);
    h.Step();
    h.all.clear();
    for (uint32_t l : {701u, 702u, 703u}) h.rx.LocalGone(l * 256 + 8, h.now);
    h.rx.SpawnerWasReset(h.now);
    h.copyOf.clear();
    for (int i = 0; i < 30; ++i) h.Step();
    Check(h.Count(RecvCmd::Type::ResetSpawner, &h.all) == 0 && h.Count(RecvCmd::Type::Spawn, &h.all) == 3,
          "spawner: a reset of the Lua's own is not answered with another; its pedestrians asked again");
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
    const bool notOwned = a2.OwnedCount() == 0 && a2.RidingCount() == 1;
    for (; t < 5.0; t += 0.05, x += 0.07f) a2.SetPeds({Ped(9 * 256 + 8, x, 0)}, t);  // 1.4 m/s
    Check(notOwned && a2.OwnedCount() == 1 && a2.RidingCount() == 0, "rider: shared again once it walks 3 s");
}


// `deafTrack`: the recorded copy of a pedestrian whose copy never answered (A net 81): the replayed copy walks exactly
// as that one did (its own pace, deaf to multipliers and nudges), moved only by our snaps and respawns.
ReplayOut ReplayOne(const Track& truth, double deadAt, const Track* deafTrack, const ReceiverConfig& rc,
                    const AuthorityConfig& ac, const AiWalker& proto = CopyWalker()) {
    ReplayOut o;
    PedAuthority auth(ac);
    PedReceiver rx(rc);
    AiWalker w = proto;
    float offX = 0, offY = 0;  // deaf: our snaps' shift of the recorded copy
    const double kCopyLag = 0.15;  // the recorded copy's rows are stamped with its target's time (rT): about now - this
    auto deafAt = [&](double t, float& x, float& y) { return deafTrack && At(*deafTrack, t - kCopyLag, x, y); };
    const uint32_t pedId = 77 * 256 + 8;
    uint32_t copyId = 5000 * 256 + 8;  // a new id per spawned copy
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
        if (have) {
            CopyReport rep;
            rep.pos = {w.x, w.y, 0};
            rep.heading[0] = w.hx;
            rep.heading[1] = w.hy;
            rx.SetReport(spawnKey, rep, rnow);
        }
        if (spawnAt >= 0 && t >= spawnAt) {
            rx.Spawned(spawnKey, copyId, rnow);
            have = true;
            spawnAt = -1;
        }
        std::vector<RecvCmd> cmds;
        rx.Tick(rnow, dt, cmds);
        for (const auto& c : cmds) {
            if (c.type == RecvCmd::Type::Spawn) {
                spawnKey = c.key;
                copyId += 256;
                spawnAt = t + 0.1;
                const float hx = w.hx, hy = w.hy;
                w = proto;
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
            } else if (c.type == RecvCmd::Type::Remove && c.local == copyId) {
                have = false;  // removed (unresponsive): the next Spawn makes another
            } else if (c.type == RecvCmd::Type::Hide && c.local == copyId) {
                have = false;  // its pedestrian went: in the pool
            } else if (c.type == RecvCmd::Type::Reuse && c.local == copyId) {
                spawnKey = c.key;  // back from the pool: put at its spot
                have = true;
                const float hx = w.hx, hy = w.hy;
                w = proto;
                w.x = c.target.pos.x;
                w.y = c.target.pos.y;
                w.hx = hx;
                w.hy = hy;
                float rx0, ry0;
                if (deafAt(t, rx0, ry0)) {
                    offX = c.target.pos.x - rx0;
                    offY = c.target.pos.y - ry0;
                }
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

// 0.16.0's rules (for the "before" runs): riders shared, 50 / 60 m and 40 per guest; no lag snap, no spawner health,
// 10 copies, 2 loading, removed when their pedestrian went, copies only near (45 m), a long wait (no camera on the
// guest's side of the replay: every spot counted as in sight).
AuthorityConfig OldAuthority() {
    AuthorityConfig c;
    c.rideSpeed = 1e9f;
    c.enterRadius = 50;
    c.leaveRadius = 60;
    c.maxPerPeer = 40;
    return c;
}

ReceiverConfig OldRules() {
    ReceiverConfig c;
    c.lagTime = 1e9;
    c.maxCopies = 10;
    c.poolKeep = 0;
    c.maxLoading = 2;
    c.spawnRadius = 45;
    c.spawnWait = 2.5;
    c.spawnerFails = 1 << 30;
    c.massLoss = 1 << 30;
    return c;
}

// feat/sync-peds (652cbf9, the WIP this package started from) as a copies-only receiver: riders not shared, lag
// snap, spawner health, 24 copies, 4 loading, copies within 45 m, removed when their pedestrian went (no pool),
// 50 / 60 m and 40 per guest, one profile.
AuthorityConfig SyncPedsAuthority() {
    AuthorityConfig c;
    c.enterRadius = 50;
    c.leaveRadius = 60;
    c.maxPerPeer = 40;
    return c;
}

ReceiverConfig SyncPedsRules() {
    ReceiverConfig c;
    c.maxCopies = 24;
    c.poolKeep = 0;
    c.spawnRadius = 45;
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

// ---- a whole session replayed: the host's interest -> a copies-only guest with the game's spawner ------------------
// The game's copy spawner on a guest as ped_sync.lua drives it (the test model of docs/host-authority.md WP-C): made
// obtainS after the first request (session A: 2.6 s from the first request to "copy spawner ready"), one profile load
// at a time (0.8-2.5 s each; a profile that never loads times out after 8 s and fails its requests), requests for the
// loaded profile first, then the look most requests wait for, one copy per tick; a request that has waited 2 s is
// made with the loaded look (a crowd that comes at once is not kept waiting for six loads). `broken`: every CreateObject fails (session A from kPedASpawnerHiddenMs:
// the spawner hidden, its copies gone). Reset: a new spawner, obtainS again, nothing loaded.
struct SpawnerSim {
    struct Req {
        uint32_t key;
        uint8_t prof;
        Vec3 at;
        double asked;
    };
    Rng rng{99};
    bool exists = false, broken = false, oneProfile = false;
    double readyAt = 0, loadDone = 0, obtainS = 2.6;
    int loaded = -1, loading = -1;
    std::set<int> bad;  // profiles that never load
    std::deque<Req> queue;
    uint64_t loads = 0, made = 0;

    void Ask(const RecvCmd& c, double t) {
        if (!exists) {
            exists = true;
            readyAt = t + obtainS;
        }
        queue.push_back({c.key, static_cast<uint8_t>(oneProfile ? 0 : c.profile), c.target.pos, t});
    }
    void Cancel(uint32_t key) {
        queue.erase(std::remove_if(queue.begin(), queue.end(), [&](const Req& r) { return r.key == key; }),
                    queue.end());
    }
    void Reset(double t, double rnow, PedReceiver& rx) {
        for (const Req& q : queue) rx.SpawnFailed(q.key, rnow, SpawnFail::Reset);
        queue.clear();
        broken = false;
        loaded = loading = -1;
        exists = true;
        readyAt = t + obtainS;
        rx.SpawnerWasReset(rnow);  // the Lua's PFR after its FacReset
    }
    double lookWait = 2.0;  // s: an older request is made with the loaded look (ped_sync.lua LOOK_WAIT)
    template <class F>
    void Tick(double t, double rnow, PedReceiver& rx, F&& onMade) {
        if (!exists || t < readyAt) return;
        if (loading >= 0) {
            if (t < loadDone) return;
            if (bad.count(loading)) {
                for (auto it = queue.begin(); it != queue.end();) {
                    if (it->prof == loading) {
                        rx.SpawnFailed(it->key, rnow, SpawnFail::NoProfile);
                        it = queue.erase(it);
                    } else {
                        ++it;
                    }
                }
            } else {
                loaded = loading;
            }
            loading = -1;
            return;
        }
        if (queue.empty()) return;
        auto it = std::find_if(queue.begin(), queue.end(), [&](const Req& r) { return r.prof == loaded; });
        if (it == queue.end() && loaded >= 0 && t - queue.front().asked >= lookWait) it = queue.begin();
        if (it == queue.end()) {  // the look most requests wait for (ties: the oldest request's)
            int count[256] = {};
            for (const Req& q : queue) ++count[q.prof];
            loading = queue.front().prof;
            for (const Req& q : queue)
                if (count[q.prof] > count[loading]) loading = q.prof;
            loadDone = t + (bad.count(loading) ? 8.0 : rng.Uniform(0.8f, 2.5f));
            ++loads;
            return;
        }
        const Req r = *it;
        queue.erase(it);
        if (broken) {
            rx.SpawnFailed(r.key, rnow, SpawnFail::Create);
            return;
        }
        ++made;
        onMade(r, static_cast<uint8_t>(loaded));
    }
};

struct SessionOpts {
    double hiddenAt = -1;  // s: the spawner hidden then (session A's failure)
    bool oneProfile = false;
    std::set<int> badProfiles;
};

struct SessionOut {
    double coverage = 0, minutes = 0, failsPerMin = 0;
    std::vector<float> errs;
    uint64_t fails = 0, resets = 0, made = 0, reuses = 0, handovers = 0, loads = 0, removes = 0;
    uint32_t maxPool = 0, profilesLive = 0;
    double bytesPerS = 0;
};

template <size_t NT, size_t NG>
SessionOut ReplaySession(const int (&truthRows)[NT][4], const int (&guestRows)[NG][3], const ReceiverConfig& rc,
                         const AuthorityConfig& ac, const SessionOpts& opt) {
    SessionOut o;
    const auto truth = Tracks(truthRows);
    Track guest;
    for (const auto& g : guestRows) guest.push_back({g[0] / 1000.0, g[1] / 100.0f, g[2] / 100.0f});
    double truthEnd = 0;
    for (const auto& [net, tr] : truth) truthEnd = std::max(truthEnd, tr.back().t);
    PedAuthority auth(ac);
    PedReceiver rx(rc);
    SpawnerSim sp;
    sp.oneProfile = opt.oneProfile;
    sp.bad = opt.badProfiles;
    struct Pk {
        double at;
        uint16_t id;
        std::vector<uint8_t> body;
    };
    std::deque<Pk> wire;
    struct CopySim {
        AiWalker w;
        uint32_t key = 0;
    };
    std::map<uint32_t, CopySim> copies;
    uint32_t nextId = 9000 * 256 + 8;
    uint64_t near = 0, rep = 0, bytes = 0;
    bool hidden = false;
    const float dt = 1.0f / 30.0f;
    double roundAt = -1, sampleAt = 0, errAt = 0;
    float gx = 0, gy = 0;
    const double t0 = guest.front().t, tEnd = std::min(guest.back().t, truthEnd + 2.0);
    for (double t = t0; t < tEnd; t += dt) {
        float x, y;
        if (At(guest, t, x, y, 2.0)) {
            gx = x;
            gy = y;
        }
        // the host's rounds (20 Hz): his pedestrians as recorded (a standing one's rows can be 2 s apart: PDT is 1 in
        // 2 sent states, an unchanged state went out once a second)
        if (t - roundAt >= 0.05 - 1e-9) {
            roundAt = t;
            auth.SetPlayers({{0, {gx + 3, gy, 0}, true}, {1, {gx, gy, 0}, true}}, 0, 0);
            std::vector<LocalPed> peds;
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
                for (auto& b : po.deaths) wire.push_back({t + 0.03, kMsgDeath, b});
                for (auto& b : po.states) wire.push_back({t + 0.03, kMsgStates, b});
                for (const auto* l : {&po.gones, &po.spawns, &po.deaths, &po.states})
                    for (const auto& b : *l) bytes += b.size();
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
            } else if (p.id == kMsgStates) {
                uint32_t ms = 0;
                std::vector<PedState> l;
                if (ReadStates(r, ms, l)) rx.OnStates(0, ms, l, rnow);
            }
            wire.pop_front();
        }
        // session A's failure: the copy spawner hidden: every copy gone with it, CreateObject failing until a reset
        if (!hidden && opt.hiddenAt > 0 && t >= opt.hiddenAt) {
            hidden = true;
            sp.broken = true;
            for (const auto& [local, c] : copies) rx.LocalGone(local, rnow);
            copies.clear();
        }
        rx.SetLocal({gx, gy, 0}, Camera{});
        for (const auto& [local, c] : copies) {
            if (c.key == 0) continue;
            CopyReport cr;
            cr.pos = {c.w.x, c.w.y, 0};
            cr.heading[0] = c.w.hx;
            cr.heading[1] = c.w.hy;
            rx.SetReport(c.key, cr, rnow);
        }
        sp.Tick(t, rnow, rx, [&](const SpawnerSim::Req& r, uint8_t look) {
            CopySim c;
            c.w = CopyWalker();
            c.w.x = r.at.x;  // where its pedestrian was when it was asked for (the Lua's queued spot)
            c.w.y = r.at.y;
            c.key = r.key;
            const uint32_t id = nextId;
            nextId += 256;
            copies[id] = c;
            rx.Spawned(r.key, id, rnow, look);
            if (rx.IsFree(id)) copies[id].key = 0;  // its pedestrian went meanwhile: a free one
        });
        std::vector<RecvCmd> cmds;
        rx.Tick(rnow, dt, cmds);
        for (const auto& c : cmds) {
            switch (c.type) {
                case RecvCmd::Type::Spawn: sp.Ask(c, t); break;
                case RecvCmd::Type::Reuse: {
                    auto it = copies.find(c.local);
                    if (it == copies.end()) break;
                    const float hx = it->second.w.hx, hy = it->second.w.hy;
                    it->second.w = CopyWalker();
                    it->second.w.x = c.target.pos.x;
                    it->second.w.y = c.target.pos.y;
                    it->second.w.hx = hx;
                    it->second.w.hy = hy;
                    it->second.key = c.key;
                    break;
                }
                case RecvCmd::Type::Hide: {
                    auto it = copies.find(c.local);
                    if (it != copies.end()) it->second.key = 0;
                    break;
                }
                case RecvCmd::Type::Remove:
                    if (c.local)
                        copies.erase(c.local);
                    else
                        sp.Cancel(c.key);
                    break;
                case RecvCmd::Type::Drive: {
                    auto it = copies.find(c.local);
                    if (it != copies.end() && it->second.key == c.key) it->second.w.Apply(c.lc, c.target.pos);
                    break;
                }
                case RecvCmd::Type::ResetSpawner: sp.Reset(t, rnow, rx); break;
                default: break;
            }
        }
        for (auto& [local, c] : copies)
            if (c.key != 0) c.w.Step(dt);
        o.maxPool = std::max<uint32_t>(o.maxPool, static_cast<uint32_t>(rx.PoolSize()));
        if (t >= sampleAt) {
            sampleAt = t + 1.0;
            near += rx.Stats().nearby;
            rep += rx.Stats().nearReplica;
            if (std::getenv("PED_DEBUG"))
                std::printf("   t=%.0f near=%u rep=%u copies=%u free=%u loading=%u known=%u fails=%llu\n", t,
                            rx.Stats().nearby, rx.Stats().nearReplica, rx.Stats().copies, rx.Stats().free,
                            rx.Stats().loading, rx.Stats().known,
                            static_cast<unsigned long long>(rx.Stats().spawnFails));
        }
        if (t >= errAt) {  // PDR at 10 Hz: every shown copy vs its pedestrian at the time its target stood for
            errAt = t + 0.1;
            for (const ReplicaInfo& i : rx.Replicas(rnow)) {
                if (!i.haveCopy || i.dead) continue;
                auto c = copies.find(i.local);
                // the host's net id -> his human -> the recorded pedestrian
                const uint32_t id = auth.IdOf(static_cast<uint16_t>(i.key & 0xFFFF));
                auto tr = truth.find(static_cast<int>((id - 8) / 256));
                if (c == copies.end() || id == 0 || tr == truth.end()) continue;
                float tx, ty;
                if (!At(tr->second, i.targetT - 1000.0, tx, ty, 2.5)) continue;
                o.errs.push_back(std::hypot(c->second.w.x - tx, c->second.w.y - ty));
            }
        }
    }
    o.coverage = near ? 100.0 * static_cast<double>(rep) / static_cast<double>(near) : 0;
    const ReceiverStats& st = rx.Stats();
    o.fails = st.spawnFails;
    o.resets = st.spawnerResets;
    o.made = sp.made;
    o.loads = sp.loads;
    o.reuses = st.reuses;
    o.handovers = st.handovers;
    o.removes = st.removes;
    o.profilesLive = st.profilesLive;
    o.minutes = (tEnd - t0) / 60.0;
    o.failsPerMin = o.minutes > 0 ? static_cast<double>(o.fails) / o.minutes : 0;
    o.bytesPerS = (tEnd - t0) > 0 ? static_cast<double>(bytes) / (tEnd - t0) : 0;
    return o;
}

std::string SessionLine(const SessionOut& o) {
    return Fmt("coverage %.1f %%, p95 %.2f m", o.coverage, Pct(o.errs, 95)) +
           Fmt(" (p50 %.2f, max %.2f), copy fails %.2f/min", Pct(o.errs, 50), Pct(o.errs, 100), o.failsPerMin) +
           Fmt(", made %.0f, reused %.0f, loads %.0f", static_cast<double>(o.made), static_cast<double>(o.reuses),
               static_cast<double>(o.loads)) +
           Fmt(", pool max %.0f, resets %.0f, %.2f KB/s", o.maxPool, static_cast<double>(o.resets), o.bytesPerS / 1024);
}

// Sessions A and B of 2026-10-20 replayed: the host's real pedestrians (PDT) through his interest (80 / 90 m, 48)
// to a copies-only guest walking (B: driving) his real way, with the spawner model; session A with its failure (the
// spawner hidden mid-session). Before: 0.16.0's rules, and feat/sync-peds's copies without a pool (one profile).
// The recorded coverage (PDC on the guest): A 18.2 %, B 82.1 %.
template <size_t NT, size_t NG>
void TestSession(const char* tag, const int (&truthRows)[NT][4], const int (&guestRows)[NG][3], double hiddenAt) {
    SessionOpts failing;
    failing.hiddenAt = hiddenAt;
    SessionOpts one = failing;
    one.oneProfile = true;
    const SessionOut old = ReplaySession(truthRows, guestRows, OldRules(), OldAuthority(), one);
    const SessionOut wip = ReplaySession(truthRows, guestRows, SyncPedsRules(), SyncPedsAuthority(), one);
    const SessionOut now = ReplaySession(truthRows, guestRows, ReceiverConfig{}, AuthorityConfig{}, failing);
    const std::string n = std::string("session ") + tag + ": ";
    std::printf("      %s before, 0.16.0 rules:        %s\n", tag, SessionLine(old).c_str());
    std::printf("      %s before, feat/sync-peds:      %s\n", tag, SessionLine(wip).c_str());
    std::printf("      %s after, host-peds (pool, 6 looks): %s\n", tag, SessionLine(now).c_str());
    Check(now.coverage >= 80.0, (n + "replayed ped.coverage >= 80 %").c_str(),
          Fmt("%.1f %% (0.16.0 rules %.1f %%, feat/sync-peds %.1f %%)", now.coverage, old.coverage, wip.coverage));
    Check(Pct(now.errs, 95) <= 1.5f, (n + "replayed ped.p95 <= 1.5 m").c_str(),
          Fmt("%.2f m over %.0f samples", Pct(now.errs, 95), now.errs.size()));
    Check(now.failsPerMin <= 2.0, (n + "ped.copy_fail_per_min <= 2").c_str(),
          Fmt("%.2f (%.0f failures in %.1f min)", now.failsPerMin, static_cast<double>(now.fails), now.minutes));
    Check(now.maxPool <= 40, (n + "at most 40 copies").c_str(), Fmt("%.0f", now.maxPool));
}

// The pool on session A's real streets: copies are reused more often than new ones are made; with two looks that
// never load (each try stops the spawner for the 8 s timeout) the rotation rests them after a failure and drops them
// after the second, and the coverage holds.
void TestSessionPool() {
    SessionOpts opt;
    const SessionOut a = ReplaySession(kPedATruth, kPedAGuest, ReceiverConfig{}, AuthorityConfig{}, opt);
    Check(a.reuses > a.made, "pool: session A reuses more copies than it spawns",
          Fmt("%.0f reused, %.0f spawned", static_cast<double>(a.reuses), static_cast<double>(a.made)));
    SessionOpts bad;
    bad.badProfiles = {1, 4};
    const SessionOut b = ReplaySession(kPedATruth, kPedAGuest, ReceiverConfig{}, AuthorityConfig{}, bad);
    Check(b.coverage >= 80.0 && b.failsPerMin <= 2.0,
          "rotation: two looks that never load (8 s timeouts) rest / drop out, coverage holds",
          Fmt("coverage %.1f %%, copy fails %.2f/min", b.coverage, b.failsPerMin) +
              Fmt(", %.0f looks left", b.profilesLive));
}

// ---- the 2026-10-20 one-PC runs (tests/data/ped_onepc_1020.h) -------------------------------------------------------
// The copies' AI there (14:36 / 15:00, sync_compare on the guest's PDR against the host's PDT): copies of running
// pedestrians went 2.84 m/s (median) against their pedestrians' 4.72 at whatever multiplier they were given - as the
// puppets' AI did (RUN x1.8 measured 2.94 m/s, 2026-10-20 13:55 run) - walkers kept pace. The walker below: the
// puppets' AI speeds at multiplier 1 (FootTuning) and at most ~3 m/s running, ~3.6 m/s sprinting.
AiWalker OnePcWalker() {
    AiWalker w = CopyWalker();
    w.realSpeed[1] = 1.4f;
    w.realSpeed[2] = 2.2f;
    w.realSpeed[3] = 3.3f;
    w.cap[1] = 2.2f;
    w.cap[2] = 3.0f;
    w.cap[3] = 3.6f;
    w.lag = 0.2f;
    return w;
}

// A recorded track as the host's game had the pedestrian: split where it jumped (faster than 12 m/s between two
// rows: his game moved it, sync_compare ped_truth_at), and the rows of a standing one (sent again only every ~2 s
// while unchanged) filled in, so it is there all the time it stood.
std::vector<Track> AsTheHostHadIt(const Track& tr) {
    std::vector<Track> out(1);
    for (size_t i = 0; i < tr.size(); ++i) {
        if (i > 0) {
            const TPt& a = tr[i - 1];
            const TPt& b = tr[i];
            const double dt = b.t - a.t;
            const float d = std::hypot(b.x - a.x, b.y - a.y);
            if (dt > 0 && d / dt > 12.0f) {
                out.emplace_back();
            } else if (dt > 0.9 && dt <= 2.6 && d / dt < 0.3f) {
                for (double t = a.t + 0.5; t < b.t - 0.05; t += 0.5) {
                    const float k = static_cast<float>((t - a.t) / dt);
                    out.back().push_back({t, a.x + (b.x - a.x) * k, a.y + (b.y - a.y) * k});
                }
            }
        }
        out.back().push_back(tr[i]);
    }
    return out;
}

// Every pedestrian of a one-PC run through the chain (ReplayOne: the host's sender, the link, the receiver, a copy
// walking like the one-PC copies). The run's own ped.p95 (its copies were driven by the NPC blend, 0.16-style stop
// walk to the pedestrian's spot): 2.05 m (run 1), 1.72 m (run 2).
template <size_t NT>
void TestOnePc(const char* tag, const int (&truthRows)[NT][4], float recordedP95) {
    std::vector<float> errs;
    int peds = 0, snaps = 0, lagSnaps = 0;
    for (const auto& [net, tr] : Tracks(truthRows)) {
        for (const Track& part : AsTheHostHadIt(tr)) {
            if (part.size() < 4) continue;
            const ReplayOut o = ReplayOne(part, 1e18, nullptr, ReceiverConfig{}, AuthorityConfig{}, OnePcWalker());
            errs.insert(errs.end(), o.errs.begin(), o.errs.end());
            snaps += o.snaps;
            lagSnaps += o.lagSnaps;
            ++peds;
        }
    }
    const std::string n = std::string("one-PC run ") + tag + ": ";
    Check(Pct(errs, 95) <= 1.5f, (n + "replayed ped.p95 <= 1.5 m (copies whose AI run is too slow)").c_str(),
          Fmt("p50 %.2f p95 %.2f max %.2f m", Pct(errs, 50), Pct(errs, 95), Pct(errs, 100)) +
              Fmt(" (recorded in the run %.2f m; %.0f tracks", recordedP95, peds) +
              Fmt(", %.0f samples, snaps %.0f", static_cast<double>(errs.size()), snaps) +
              Fmt(", lag snaps %.0f)", lagSnaps));
}

}  // namespace

int main() {
    TestCodec();
    TestRules();
    TestAuthority();
    TestCapAndBudget();
    TestTwoGuests();
    TestReceiver();
    TestPoolFull();
    TestRotation();
    TestEndToEnd(15, 0, 1, "quiet link");
    TestEndToEnd(60, 2, 2, "60 ms jitter, 2 % loss");
    TestCoverage(true, 80.0f, "coverage: looking around >= 80 % (ped.coverage)");
    TestCoverage(false, 80.0f, "coverage: staring one way >= 80 %");
    TestSpawnerHealth();
    TestRider();
    TestRecordedDrift("A", kPedATruth, kPedAReplica, kPedADeaths, 1.5f);
    TestRecordedDrift("B", kPedBTruth, kPedBReplica, kPedBDeaths, 1.5f);
    TestSession("A", kPedATruth, kPedAGuest, kPedASpawnerHiddenMs / 1000.0);
    TestSession("B", kPedBTruth, kPedBGuest, -1);
    TestSessionPool();
    TestOnePc("1 (14:36)", kPedOnePc1Truth, 2.05f);
    TestOnePc("2 (15:00)", kPedOnePc2Truth, 1.72f);
    std::printf("%s (%d failure%s)\n", g_failures ? "FAILED" : "OK", g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
