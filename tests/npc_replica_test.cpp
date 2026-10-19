// npc_replica_test: unit tests for one authority per NPC (src/game/npc_replica.h): the compact NpcStates2 / NpcInfo /
// NpcLeave codec, the ambient-human contract and the relayed-hit rule, the authority's gait, interest per receiving
// player (admission with hysteresis, rates by distance, idle NPCs slower, the owner never gets his own, relays only to
// the named receivers, infos on enter and change, leaves), the NPC bandwidth per guest at 48 NPCs in range, and the
// whole path of two host cops of the 2026-10-19 two-PC test (session 1, their positions from the guest's NP records)
// through the publisher, a network with latency, jitter and loss, the codec, the replica stream and the locomotion
// controller + Corrector driving a simulated AI walker (command lag, wrong walk speeds, turn and acceleration
// limits): replica vs the host's NPC at the same moment, p95 <= 1.0 m and max <= 4 m. Also a paired local NPC whose
// AI keeps walking off is given up (hidden, a copy shown). Prints PASS/FAIL per check; exits non-zero on any failure.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <deque>
#include <map>
#include <random>
#include <string>
#include <vector>

#include "game/latency.h"
#include "game/npc_replica.h"
#include "net/wire.h"

using namespace mc::game::npcrep;
using mc::net::motion::Gait;
using mc::net::motion::LocomotionCommand;

namespace {

int g_failures = 0;

void Check(bool ok, const char* name, const std::string& detail = {}) {
    std::printf("%s  %-66s %s\n", ok ? "PASS" : "FAIL", name, detail.c_str());
    std::fflush(stdout);
    if (!ok) ++g_failures;
}

std::string Fmt(const char* f, double a, double b = 0, double c = 0, double d = 0) {
    char buf[200];
    std::snprintf(buf, sizeof buf, f, a, b, c, d);
    return buf;
}

double Pct(std::vector<double> v, double p) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    const double k = (v.size() - 1) * p / 100.0;
    const size_t lo = static_cast<size_t>(std::floor(k)), hi = static_cast<size_t>(std::ceil(k));
    return v[lo] + (v[hi] - v[lo]) * (k - lo);
}

NpcState Npc(uint32_t key, float x, float y, uint8_t ai = 4) {
    NpcState s;
    s.key = key;
    s.x = x;
    s.y = y;
    s.z = 3;
    s.hx = 1;
    s.hy = 0;
    s.hp = 200;
    s.ai = ai;
    s.tag = 77;
    s.weapon = "p_mast_a_v1";
    s.profile = 1234567890123ull;
    return s;
}

Item It(const NpcState& s, PlayerId owner = 0, uint32_t mask = 0xFFFFFFFFu) {
    Item i;
    i.s = s;
    i.owner = owner;
    i.cls = NameClass::Spawner;
    i.peerMask = mask;
    return i;
}

// ---- codec -----------------------------------------------------------------------------------------------------------
void TestCodec() {
    std::vector<Fast> in;
    for (int i = 0; i < 60; ++i) {
        Fast f;
        f.net = static_cast<uint16_t>(100 + i);
        f.x = -700.123f + i * 3.3f;
        f.y = -290.456f - i * 1.7f;
        f.z = 4.071f + (i % 3) * 0.5f;
        f.hx = std::cos(i * 0.3f);
        f.hy = std::sin(i * 0.3f);
        f.vx = 3.456f - i * 0.1f;
        f.vy = -1.234f;
        f.hp = static_cast<uint16_t>(150 + i);
        f.flags = static_cast<uint8_t>(i % 4);
        f.shots = static_cast<uint8_t>(i * 7);
        f.gait = static_cast<uint8_t>(i % 4);
        in.push_back(f);
    }
    Fast far = in[0];  // 500 m away: does not fit the first origin
    far.net = 999;
    far.x += 500;
    in.push_back(far);
    const auto bufs = PackStates(123456789u, in);
    std::vector<Fast> got;
    size_t biggest = 0;
    bool timesOk = true;
    for (const auto& b : bufs) {
        biggest = std::max(biggest, b.size());
        mc::net::ByteReader r(b);
        StatesMsg m;
        if (!ReadStates(r, m)) {
            got.clear();
            break;
        }
        timesOk = timesOk && m.timeMs == 123456789u;
        got.insert(got.end(), m.npcs.begin(), m.npcs.end());
    }
    Check(bufs.size() == 3 && got.size() == in.size() && timesOk, "states: 61 NPCs (one 500 m away) in 3 messages",
          Fmt("%.0f messages, %.0f NPCs", static_cast<double>(bufs.size()), static_cast<double>(got.size())));
    Check(biggest <= kStatesHeader + kMaxPerStates * kFastBytes && biggest <= 1000, "states: a message fits 1000 bytes",
          Fmt("%.0f bytes", static_cast<double>(biggest)));
    float maxPos = 0, maxVel = 0, maxHead = 0;
    bool fieldsOk = true;
    for (const Fast& a : in) {
        auto b = std::find_if(got.begin(), got.end(), [&](const Fast& f) { return f.net == a.net; });
        if (b == got.end()) {
            fieldsOk = false;
            continue;
        }
        maxPos = std::max({maxPos, std::fabs(a.x - b->x), std::fabs(a.y - b->y), std::fabs(a.z - b->z)});
        maxVel = std::max({maxVel, std::fabs(a.vx - b->vx), std::fabs(a.vy - b->vy)});
        maxHead = std::max({maxHead, std::fabs(a.hx - b->hx), std::fabs(a.hy - b->hy)});
        fieldsOk = fieldsOk && a.hp == b->hp && a.flags == b->flags && a.shots == b->shots && a.gait == b->gait;
    }
    Check(fieldsOk && maxPos <= 0.006f && maxVel <= 0.006f && maxHead <= 0.005f,
          "states: round trip (1 cm, 1 cm/s, heading x127, discrete fields)",
          Fmt("pos %.4f m, vel %.4f m/s, heading %.4f", maxPos, maxVel, maxHead));
    Check(kFastBytes == (bufs[0].size() - kStatesHeader) / 48, "states: 19 bytes per NPC");
    {
        std::vector<uint8_t> cut = bufs[0];
        cut.resize(cut.size() - 5);
        mc::net::ByteReader r(cut);
        StatesMsg m;
        Check(!ReadStates(r, m), "states: a cut message is refused");
        std::vector<uint8_t> big = bufs[0];
        big[16] = 200;  // count beyond kMaxPerStates
        mc::net::ByteReader r2(big);
        Check(!ReadStates(r2, m), "states: a count beyond 48 is refused");
    }
    std::vector<Info> infos;
    for (int i = 0; i < 70; ++i)
        infos.push_back({static_cast<uint16_t>(i + 1), 4584968u + 256u * i, static_cast<uint8_t>(i % 5), 13908013u,
                         i % 2 ? "p_mast_a_v1" : "bad weapon!", 16922695808871587265ull,
                         static_cast<NameClass>(i % 3)});
    std::vector<Info> back;
    size_t ib = 0;
    for (const auto& b : PackInfos(infos)) {
        ib = std::max(ib, b.size());
        mc::net::ByteReader r(b);
        std::vector<Info> part;
        if (ReadInfos(r, part)) back.insert(back.end(), part.begin(), part.end());
    }
    bool infoOk = back.size() == infos.size();
    for (size_t i = 0; infoOk && i < infos.size(); ++i) {
        Info want = infos[i];
        if (want.weapon == "bad weapon!") want.weapon = "-";
        infoOk = back[i] == want;
    }
    Check(infoOk && ib <= 1000, "infos: 70 round trip (several messages, unsafe weapon -> '-')",
          Fmt("%.0f back, largest %.0f bytes", static_cast<double>(back.size()), static_cast<double>(ib)));
    std::vector<uint16_t> nets;
    for (int i = 0; i < 300; ++i) nets.push_back(static_cast<uint16_t>(i * 3 + 1));
    std::vector<uint16_t> nb;
    for (const auto& b : PackLeaves(nets)) {
        mc::net::ByteReader r(b);
        std::vector<uint16_t> part;
        if (ReadLeaves(r, part)) nb.insert(nb.end(), part.begin(), part.end());
    }
    Check(nb == nets, "leaves: 300 net ids round trip");
}

// ---- rules -----------------------------------------------------------------------------------------------------------
void TestRules() {
    Check(ClassOfName("behavior_object_prime-1f-spawner_cp_010_salieri") == NameClass::Spawner &&
              ClassOfName("behavior_object_prime-3A-machinegunner_Kitchen") == NameClass::Spawner,
          "names: a mission spawner's human");
    Check(ClassOfName("behavior_object_prime-2c-") == NameClass::Generator &&
              ClassOfName("behavior_object_prime-2c-behavior_object_prime-11-") == NameClass::Generator,
          "names: a generator's human / a puppet (generated name after it)");
    Check(ClassOfName("") == NameClass::Unknown && ClassOfName("frank") == NameClass::Unknown &&
              ClassOfName("behavior_object_prime-xyz") == NameClass::Unknown,
          "names: anything else is unknown");
    Check(IsAmbientHuman(false, false, 0, NameClass::Generator) && IsAmbientHuman(false, false, 0, NameClass::Unknown),
          "ambient: AI type 0, on foot, no spawner name");
    Check(!IsAmbientHuman(false, false, 0, NameClass::Spawner), "ambient: a mission civilian (spawner name) is ours");
    Check(!IsAmbientHuman(false, false, 4, NameClass::Generator) && !IsAmbientHuman(false, false, 2, NameClass::Unknown),
          "ambient: police / enemies are ours");
    Check(!IsAmbientHuman(false, true, 0, NameClass::Generator), "ambient: a human in a vehicle is not ambient");
    Check(!IsAmbientHuman(true, false, 0, NameClass::Generator), "ambient: players, puppets, copies are not ambient");
    Check(!SkipRelayedHit(HitRole::Replica, true, true), "hits: a replica never skips the authority's hit");
    Check(SkipRelayedHit(HitRole::Own, true, true), "hits: our own hostile NPC (its hit happened here) skips it");
    Check(!SkipRelayedHit(HitRole::Own, true, false) && !SkipRelayedHit(HitRole::Own, false, true) &&
              !SkipRelayedHit(HitRole::None, true, true),
          "hits: not hostile / dead / nothing here: applied");
    uint8_t g = 0;
    std::vector<uint8_t> seq;
    for (float s : {0.0f, 0.4f, 0.6f, 2.3f, 2.6f, 2.3f, 2.0f, 5.0f, 5.5f, 4.9f, 4.6f, 0.3f, 6.0f}) {
        g = GaitOf(s, g);
        seq.push_back(g);
    }
    const std::vector<uint8_t> want = {0, 0, 1, 1, 2, 2, 1, 2, 3, 3, 2, 0, 3};
    std::string got;
    for (uint8_t x : seq) got += static_cast<char>('0' + x);
    Check(seq == want, "gait: from speed with hysteresis", got);
}

// ---- interest ---------------------------------------------------------------------------------------------------------
void TestInterest() {
    Publisher pub;
    pub.SetPeers({1, 2});
    pub.SetViewpoints(1, {{0, 0, 3}});
    pub.SetViewpoints(2, {{1000, 0, 3}});
    // NPC 8 near guest 1 (10 m), 264 at 110 m from him, 520 near guest 2, 776 between (500 m from both)
    std::vector<Item> round = {It(Npc(8, 10, 0)), It(Npc(264, 110, 0)), It(Npc(520, 1005, 0)), It(Npc(776, 500, 0))};
    std::map<PlayerId, std::map<uint32_t, int>> sent;
    std::map<PlayerId, std::vector<Info>> infos;
    double t = 0;
    uint32_t ms = 1000;
    for (int i = 0; i < 150; ++i) {  // 10 s of 15 Hz rounds, the NPCs walking slowly (+0.6 m/s along y)
        t += 1.0 / 15;
        ms += 67;
        for (Item& it : round) it.s.y += 0.04f;
        pub.Round(ms, t, round, true, 0);
        for (const PeerOut& po : pub.Tick(t)) {
            for (const Info& in : po.infos) infos[po.peer].push_back(in);
            for (const auto& g : po.states)
                for (const Fast& f : g.npcs) {
                    for (const Info& in : infos[po.peer])
                        if (in.net == f.net) ++sent[po.peer][in.key];
                }
        }
    }
    const double near1 = sent[1][8] / 10.0, far1 = sent[1][264] / 10.0;
    Check(near1 >= 13 && near1 <= 15.5, "interest: an NPC 10 m from a guest: ~15 Hz", Fmt("%.1f Hz", near1));
    Check(far1 >= 4.0 && far1 <= 6.0, "interest: one at 110 m: ~5 Hz", Fmt("%.1f Hz", far1));
    Check(sent[1].count(520) == 0 && sent[2][520] > 100, "interest: each guest gets the NPCs around him");
    Check(sent[1].count(776) == 0 && sent[2].count(776) == 0, "interest: nobody gets one 500 m from every guest");
    Check(infos[1].size() == 2 && infos[2].size() == 1, "interest: one info per NPC and guest (no repeats)",
          Fmt("%.0f / %.0f", static_cast<double>(infos[1].size()), static_cast<double>(infos[2].size())));

    // idle: an NPC that stands is sent half as often
    {
        Publisher p2;
        p2.SetPeers({1});
        p2.SetViewpoints(1, {{0, 0, 3}});
        std::vector<Item> r2 = {It(Npc(8, 60, 0)), It(Npc(264, 60, 20))};
        int moving = 0, idle = 0;
        double tt = 0;
        uint32_t m2 = 5000;
        for (int i = 0; i < 1200; ++i) {  // rounds at 60 Hz here (finer than the intervals)
            tt += 1.0 / 60;
            m2 += 17;
            r2[0].s.y += 0.025f;  // 1.5 m/s
            p2.Round(m2, tt, r2, true, 0);
            for (const PeerOut& po : p2.Tick(tt))
                for (const auto& g : po.states)
                    for (const Fast& f : g.npcs) (f.net == p2.NetOf(8) ? moving : idle)++;
        }
        Check(idle * 1.6 < moving && idle * 2.6 > moving, "interest: an idle NPC (60 m) half as often as a moving one",
              Fmt("moving %.0f, idle %.0f in 20 s", moving, idle));
    }
    // hysteresis: enters within 120 m, stays until beyond 140 m; then a leave
    {
        Publisher p3;
        p3.SetPeers({1});
        p3.SetViewpoints(1, {{0, 0, 3}});
        Item a = It(Npc(8, 130, 0));
        double tt = 0;
        uint32_t m3 = 100;
        auto step = [&](float x) {
            a.s.x = x;
            tt += 0.1;
            m3 += 100;
            p3.Round(m3, tt, {a}, true, 0);
            int st = 0, lv = 0;
            for (const PeerOut& po : p3.Tick(tt)) {
                for (const auto& g : po.states) st += static_cast<int>(g.npcs.size());
                lv += static_cast<int>(po.leaves.size());
            }
            return std::make_pair(st, lv);
        };
        bool none130 = true;
        for (int i = 0; i < 10; ++i) none130 = none130 && step(130).first == 0;
        int got118 = 0;
        for (int i = 0; i < 10; ++i) got118 += step(118).first;
        int got135 = 0;
        for (int i = 0; i < 20; ++i) got135 += step(135).first;
        int leaves = 0, after = 0;
        for (int i = 0; i < 10; ++i) {
            auto r = step(145);
            leaves += r.second;
            after += r.first;
        }
        Check(none130 && got118 > 0 && got135 > 0 && leaves == 1 && after == 0,
              "interest: enter at 120 m, kept to 140 m, one leave beyond",
              Fmt("118 m: %.0f, 135 m: %.0f, leaves %.0f", got118, got135, leaves));
    }
    // the owner never gets his own; a relay goes only to the receivers it names; a changed field -> a new info; a gone
    // NPC (complete round without it) -> a leave; a dead NPC at most once a second
    {
        Publisher p4;
        p4.SetPeers({1, 2});
        p4.SetViewpoints(1, {{0, 0, 3}});
        p4.SetViewpoints(2, {{5, 0, 3}});
        NpcState own = Npc(1000, 3, 0);
        NpcState relay = Npc(0x1234F1u, 4, 0);  // guest 1's own NPC, relayed
        NpcState dead = Npc(2000, 2, 1);
        dead.flags = kDead;
        dead.hp = 0;
        int ownTo1 = 0, relayTo1 = 0, relayTo2 = 0, deadTo1 = 0, infoChanges = 0, leaves = 0;
        double tt = 0;
        uint32_t m4 = 100;
        for (int i = 0; i < 60; ++i) {
            tt += 1.0 / 15;
            m4 += 67;
            if (i == 30) own.weapon = "shotgun_a";
            std::vector<Item> items = {It(own), It(relay, 1, 1u << 2), It(dead)};
            if (i >= 45) items.erase(items.begin());  // NPC 1000 is gone
            p4.Round(m4, tt, items, true, 0);
            for (const PeerOut& po : p4.Tick(tt)) {
                for (const Info& in : po.infos)
                    if (in.key == 1000 && in.weapon == "shotgun_a" && po.peer == 1) ++infoChanges;
                if (po.peer == 1) leaves += static_cast<int>(po.leaves.size());
                for (const auto& g : po.states)
                    for (const Fast& f : g.npcs) {
                        if (po.peer == 1 && f.net == p4.NetOf(1000)) ++ownTo1;
                        if (f.net == p4.NetOf(0x1234F1u)) (po.peer == 1 ? relayTo1 : relayTo2)++;
                        if (po.peer == 1 && f.net == p4.NetOf(2000)) ++deadTo1;
                    }
            }
        }
        Check(relayTo1 == 0 && relayTo2 > 20, "relay: never back to its owner, only to the receiver named",
              Fmt("to its owner %.0f, to guest 2 %.0f", relayTo1, relayTo2));
        Check(ownTo1 > 20 && infoChanges == 1, "info: a changed weapon goes once", Fmt("%.0f", infoChanges));
        Check(leaves == 1, "leave: the NPC gone from a complete round", Fmt("%.0f", leaves));
        Check(deadTo1 >= 3 && deadTo1 <= 5, "dead: its state about once a second", Fmt("%.0f in 4 s", deadTo1));
    }
}

// ---- bandwidth: 48 NPCs in range of a guest ----------------------------------------------------------------------------
void TestBandwidth() {
    Publisher pub;
    pub.SetPeers({1});
    std::mt19937 rng(7);
    std::uniform_real_distribution<float> u(0, 1);
    std::vector<Item> items;
    std::vector<float> dirs;
    for (int i = 0; i < 48; ++i) {  // 0-118 m around the guest, two thirds moving (walk / run), a third standing
        const float r = 5 + 113 * std::sqrt(u(rng)), a = 6.283f * u(rng);
        items.push_back(It(Npc(static_cast<uint32_t>(8 + 256 * i), r * std::cos(a), r * std::sin(a))));
        dirs.push_back(i % 3 == 0 ? 0.0f : (i % 3 == 1 ? 1.4f : 3.8f));
    }
    double t = 0, bytes = 0, msgs = 0;
    uint32_t ms = 10;
    const int rounds = 15 * 60;
    for (int i = 0; i < rounds; ++i) {
        t += 1.0 / 15;
        ms += 67;
        pub.SetViewpoints(1, {{0, 0, 3}});
        for (size_t k = 0; k < items.size(); ++k) {  // walking in circles around the guest (stay in range)
            const float x = items[k].s.x, y = items[k].s.y, r = std::hypot(x, y), sp = dirs[k] / 15.0f;
            items[k].s.x = x - y / r * sp;
            items[k].s.y = y + x / r * sp;
        }
        pub.Round(ms, t, items, true, 0);
        for (const PeerOut& po : pub.Tick(t)) {
            for (const auto& g : po.states)
                for (const auto& b : PackStates(g.timeMs, g.npcs)) {
                    bytes += b.size() + 10;  // + core's message header (id, length, stamp)
                    ++msgs;
                }
            for (const auto& b : PackInfos(po.infos)) bytes += b.size() + 10;
        }
    }
    const double kbs = bytes / (rounds / 15.0) / 1000.0;
    Check(kbs <= 8.0, "bandwidth: 48 NPCs in range of a guest: NPC states <= 8 KB/s",
          Fmt("%.2f KB/s, %.1f messages/s (was ~21 KB/s)", kbs, msgs / (rounds / 15.0)));
}

// ---- end to end: host cops of session 1 -> replicas on the guest ----------------------------------------------------------
struct Waypoint {
    double t, x, y, z;
};
// cop 7661064: its host positions in the guest's NP records (2 Hz), session 1 (-204821), on foot
const Waypoint kCop7661064[] = {
    {0.000, -301.47, -445.56, 2.11},
    {0.504, -301.47, -445.56, 2.11},
    {1.012, -301.47, -445.56, 2.11},
    {1.514, -301.47, -445.56, 2.11},
    {2.018, -301.47, -445.56, 2.11},
    {2.523, -301.13, -445.60, 2.11},
    {3.026, -300.85, -445.64, 2.12},
    {3.534, -300.85, -445.64, 2.12},
    {4.038, -300.85, -445.64, 2.12},
    {4.539, -300.97, -445.64, 2.11},
    {5.042, -301.48, -445.63, 2.11},
    {5.543, -301.66, -445.17, 2.10},
    {6.052, -302.71, -443.66, 2.04},
    {6.556, -303.92, -441.93, 1.99},
    {7.062, -305.37, -439.84, 1.93},
    {7.565, -306.65, -438.02, 1.91},
    {8.071, -307.87, -436.93, 1.95},
    {8.572, -308.69, -437.24, 1.94},
    {9.075, -309.52, -437.07, 1.92},
    {9.578, -311.65, -436.61, 1.86},
    {10.079, -311.92, -436.55, 1.87},
    {10.583, -311.92, -436.55, 1.87},
    {11.089, -311.94, -436.55, 1.87},
    {11.592, -310.98, -436.05, 1.89},
    {12.099, -309.56, -435.02, 1.92},
    {12.600, -309.09, -435.07, 1.93},
    {13.105, -309.08, -435.07, 1.93},
    {13.609, -309.09, -435.08, 1.93},
    {14.115, -309.38, -436.35, 1.93},
    {14.616, -309.42, -436.77, 1.93},
    {15.116, -308.10, -435.79, 1.95},
    {15.622, -306.43, -433.96, 1.94},
    {16.122, -305.21, -432.09, 1.92},
    {16.628, -304.51, -430.69, 1.93},
    {17.132, -303.53, -429.01, 1.94},
    {17.641, -303.37, -428.56, 1.94},
    {18.144, -303.24, -428.24, 1.94},
    {18.644, -302.30, -426.95, 1.95},
    {19.151, -301.04, -425.25, 1.94},
    {19.654, -300.60, -422.79, 1.96},
    {20.156, -300.29, -420.50, 1.98},
    {20.659, -300.11, -419.35, 1.98},
    {21.161, -300.89, -419.69, 1.98},
    {21.664, -301.02, -419.82, 1.98},
    {22.165, -301.01, -419.82, 1.98},
    {22.669, -300.16, -419.03, 1.97},
    {23.171, -300.13, -417.10, 1.97},
    {23.672, -300.11, -414.85, 1.96},
    {24.177, -299.97, -412.57, 2.81},
    {24.683, -300.07, -410.53, 3.23},
    {25.185, -300.28, -407.98, 3.21},
    {25.690, -300.45, -405.85, 3.22},
    {26.191, -300.66, -403.37, 3.22},
    {26.692, -300.76, -402.75, 3.22},
    {27.198, -300.84, -402.85, 3.22},
    {27.702, -301.31, -404.70, 3.22},
    {28.207, -301.85, -406.80, 3.21},
    {28.709, -302.41, -409.01, 3.22},
    {29.212, -303.03, -411.44, 3.21},
    {29.717, -303.90, -413.23, 3.21},
    {30.222, -306.32, -413.83, 3.21},
    {30.725, -308.56, -414.37, 3.20},
    {31.225, -310.62, -414.89, 3.21},
    {31.730, -312.90, -415.15, 3.20},
    {32.233, -315.42, -414.98, 3.19},
    {32.736, -317.63, -414.83, 3.19},
    {33.238, -319.83, -414.68, 3.19},
    {33.741, -322.29, -414.52, 3.19},
    {34.246, -324.51, -414.41, 3.19},
    {34.753, -327.02, -414.55, 3.18},
    {35.254, -329.20, -414.68, 3.19},
    {35.762, -331.46, -414.81, 3.18},
    {36.268, -333.98, -414.96, 3.27},
    {36.775, -335.99, -414.36, 3.18},
    {37.282, -337.45, -412.64, 3.17},
    {37.787, -338.86, -411.02, 3.17},
    {38.287, -340.79, -410.50, 3.16},
    {38.788, -341.07, -412.64, 2.80},
    {39.294, -341.26, -414.89, 1.95},
    {39.798, -341.40, -417.08, 1.88},
    {40.306, -341.53, -419.34, 1.86},
    {40.812, -341.68, -421.78, 1.84},
    {41.315, -341.67, -424.11, 1.85},
    {41.819, -341.57, -426.23, 1.85},
    {42.327, -341.25, -428.79, 1.86},
    {42.833, -340.97, -430.96, 1.87},
    {43.337, -341.78, -430.45, 1.81},
    {43.842, -342.87, -429.64, 1.77},
    {44.348, -342.72, -429.91, 1.78},
    {44.850, -342.48, -431.44, 1.88},
    {45.350, -342.70, -433.25, 1.85},
    {45.852, -344.61, -434.37, 1.89},
    {46.356, -345.16, -436.70, 1.85},
    {46.861, -345.44, -438.21, 1.82},
    {47.363, -346.18, -438.30, 1.82},
    {47.863, -346.83, -438.39, 1.82},
    {48.370, -347.61, -438.49, 1.82},
    {48.874, -348.26, -438.57, 1.81},
    {49.381, -348.95, -438.66, 1.81},
    {49.886, -349.62, -438.75, 1.81},
    {50.393, -350.38, -438.84, 1.81},
    {50.895, -351.04, -438.93, 1.81},
    {51.397, -351.68, -439.01, 1.81},
    {51.902, -352.33, -439.10, 1.82},
    {52.403, -353.02, -439.35, 1.84},
    {52.904, -353.50, -439.82, 1.85},
    {53.411, -354.07, -440.36, 1.87},
    {53.916, -354.54, -440.82, 1.88},
    {54.420, -355.00, -441.26, 1.89},
    {54.925, -355.53, -441.77, 1.91},
    {55.430, -355.60, -441.84, 1.91},
};
// cop 4584968: its host positions in the guest's NP records (2 Hz), session 1 (-204821), on foot
const Waypoint kCop4584968[] = {
    {0.000, -719.16, -286.74, 4.10},
    {0.500, -719.16, -286.74, 4.10},
    {1.005, -719.24, -286.78, 4.10},
    {1.511, -719.95, -286.93, 4.11},
    {2.012, -721.30, -287.20, 4.12},
    {2.517, -723.04, -288.45, 4.11},
    {3.020, -724.90, -289.80, 4.10},
    {3.524, -726.67, -291.07, 4.09},
    {4.036, -729.25, -291.00, 4.10},
    {4.546, -731.50, -290.62, 4.11},
    {5.055, -733.77, -289.72, 4.13},
    {5.562, -735.92, -288.99, 4.16},
    {6.074, -738.03, -288.51, 4.16},
    {6.582, -740.55, -288.26, 4.14},
    {7.089, -742.79, -288.39, 4.13},
    {7.591, -744.90, -288.80, 4.12},
    {8.099, -746.35, -289.09, 4.02},
    {8.607, -746.53, -289.13, 4.02},
    {9.117, -746.56, -289.13, 4.02},
    {9.620, -747.15, -289.31, 4.02},
    {10.121, -748.51, -289.55, 4.06},
    {10.624, -750.82, -290.02, 4.09},
    {11.126, -752.93, -290.63, 4.09},
    {11.629, -755.03, -291.09, 4.08},
    {12.141, -757.56, -291.50, 4.08},
    {12.643, -759.71, -291.85, 4.08},
    {13.147, -761.50, -292.14, 4.02},
    {13.648, -762.60, -292.32, 4.02},
    {14.160, -762.60, -292.32, 4.02},
    {14.662, -762.54, -291.74, 4.02},
    {15.165, -762.92, -290.97, 4.02},
    {15.665, -763.77, -290.17, 4.02},
    {16.173, -765.14, -290.36, 4.02},
    {16.678, -767.12, -289.71, 4.02},
    {17.180, -767.74, -289.92, 4.02},
    {17.681, -767.08, -289.75, 4.02},
    {18.183, -765.97, -289.45, 4.02},
    {18.685, -764.68, -288.88, 4.02},
    {19.187, -762.74, -287.94, 4.03},
    {19.690, -760.72, -287.03, 4.02},
    {20.198, -758.27, -286.34, 4.02},
    {20.701, -756.10, -285.83, 4.02},
    {21.207, -753.63, -285.29, 4.02},
    {21.710, -751.99, -283.88, 4.03},
    {22.212, -750.53, -282.25, 4.05},
    {22.717, -748.83, -280.34, 4.07},
    {23.226, -747.35, -278.70, 4.09},
    {23.726, -746.07, -277.23, 4.02},
    {24.229, -744.98, -276.02, 4.02},
    {24.729, -744.20, -275.17, 4.02},
};

// The host's NPC between the waypoints: a game human (at most 10 m/s^2, as the walker below and core's net_motion
// walker) following a smooth curve through them (Catmull-Rom). The 2 Hz records say where it was, not how it moved in
// between; the curve alone would accelerate at up to 23 m/s^2 at the stops, which no human body here does.
struct Path {
    std::vector<Waypoint> w;
    std::vector<float> px, py, pz;  // the NPC every 1/120 s
    static constexpr double kStep = 1.0 / 120;
    explicit Path(std::vector<Waypoint> wp) : w(std::move(wp)) {
        double x = w[0].x, y = w[0].y, vx = 0, vy = 0;
        for (double t = 0; t <= w.back().t + kStep; t += kStep) {
            double cx, cy, cz, nx, ny, nz;
            Curve(t, cx, cy, cz);
            Curve(t + kStep, nx, ny, nz);
            // wanted velocity: along the curve, plus a pull back onto it; the change bounded by the body's limit
            const double wx = (nx - cx) / kStep + 4 * (cx - x), wy = (ny - cy) / kStep + 4 * (cy - y);
            double ax = (wx - vx) / kStep, ay = (wy - vy) / kStep;
            const double al = std::hypot(ax, ay);
            if (al > 10) {
                ax *= 10 / al;
                ay *= 10 / al;
            }
            vx += ax * kStep;
            vy += ay * kStep;
            x += vx * kStep;
            y += vy * kStep;
            px.push_back(static_cast<float>(x));
            py.push_back(static_cast<float>(y));
            pz.push_back(static_cast<float>(cz));
        }
    }
    double Duration() const { return w.back().t; }
    void Curve(double t, double& x, double& y, double& z) const {
        if (t <= w.front().t || t >= w.back().t) {
            const Waypoint& e = t <= w.front().t ? w.front() : w.back();
            x = e.x, y = e.y, z = e.z;
            return;
        }
        size_t i = 0;
        while (i + 1 < w.size() && w[i + 1].t < t) ++i;
        const Waypoint& p1 = w[i];
        const Waypoint& p2 = w[i + 1];
        const Waypoint& p0 = i > 0 ? w[i - 1] : p1;
        const Waypoint& p3 = i + 2 < w.size() ? w[i + 2] : p2;
        const double u = (t - p1.t) / (p2.t - p1.t), u2 = u * u, u3 = u2 * u;
        auto cr = [&](double a, double b, double c, double d) {
            return 0.5 * (2 * b + (-a + c) * u + (2 * a - 5 * b + 4 * c - d) * u2 + (-a + 3 * b - 3 * c + d) * u3);
        };
        x = cr(p0.x, p1.x, p2.x, p3.x);
        y = cr(p0.y, p1.y, p2.y, p3.y);
        z = cr(p0.z, p1.z, p2.z, p3.z);
    }
    void At(double t, float& x, float& y, float& z) const {
        const double k = std::clamp(t / kStep, 0.0, static_cast<double>(px.size() - 1));
        const size_t i = std::min(static_cast<size_t>(k), px.size() - 2);
        const float u = static_cast<float>(k - i);
        x = px[i] + (px[i + 1] - px[i]) * u;
        y = py[i] + (py[i + 1] - py[i]) * u;
        z = pz[i] + (pz[i + 1] - pz[i]) * u;
    }
};

// An AI human walked by the game (MoveVec to a point with a gait and a speed multiplier): the command takes effect
// `lag` s later, its real gait speeds differ from the controller's first guess, it turns at most 360 deg/s, accelerates
// at most 10 m/s^2, slows down in turns and brakes to arrive at its walk point. A `rogue` one (a paired local NPC whose
// own AI was not stopped) walks off toward its own goal half of the time.
struct Walker {
    float realSpeed[4] = {0, 1.5f * 0.85f, 4.0f * 1.15f, 6.5f * 0.85f};
    float lag = 0.15f;
    float x = 0, y = 0, hx = 1, hy = 0, speed = 0;
    int gait = 0;
    float mult = 1, tx = 0, ty = 0;
    bool walking = false, rogue = false;
    float clock = 0;
    // m/s^2: as core's net_motion walker (the player's body: an NPC is the same human in this game)
    static constexpr float kAccel = 10.0f;
    struct Pending {
        float at;
        int gait;
        float mult;
        bool reissue;
        float tx, ty;
    };
    std::deque<Pending> queue;

    void Apply(const LocomotionCommand& c, const Vec3& target) {
        if (c.fix == LocomotionCommand::Fix::Snap) {
            x = target.x;
            y = target.y;
            speed = 0;
        } else if (c.fix == LocomotionCommand::Fix::Nudge) {
            x += c.nudge.x;
            y += c.nudge.y;
        }
        queue.push_back({clock + lag, static_cast<int>(c.gait), c.speedMult, c.reissue, c.moveTo.x, c.moveTo.y});
    }
    void Step(float dt) {
        clock += dt;
        while (!queue.empty() && queue.front().at <= clock) {
            const Pending& p = queue.front();
            if (p.reissue) {
                gait = p.gait == 0 ? 1 : p.gait;
                walking = true;
                tx = p.tx;
                ty = p.ty;
            }
            mult = p.gait == 0 ? 1.0f : p.mult;
            queue.pop_front();
        }
        if (rogue && std::fmod(clock, 8.0f) > 4.0f) {  // its own AI: runs toward its own goal
            walking = true;
            gait = 2;
            mult = 1;
            tx = x + 30;
            ty = y + 30;
        }
        float want = 0, dx = tx - x, dy = ty - y;
        const float d = std::sqrt(dx * dx + dy * dy);
        if (walking && d > 0.1f) {
            want = realSpeed[gait] * mult;
            const float a = std::atan2(hx * dy - hy * dx, hx * dx + hy * dy);
            const float lim = 6.283f * dt;
            const float r = std::clamp(a, -lim, lim);
            const float c = std::cos(r), s = std::sin(r);
            const float nx = hx * c - hy * s, ny = hx * s + hy * c;
            hx = nx;
            hy = ny;
            want *= std::max(0.3f, std::cos(a));
            want = std::min(want, std::sqrt(2 * kAccel * d));
        } else if (walking) {
            walking = false;
        }
        const float acc = kAccel * dt;
        speed += std::clamp(want - speed, -acc, acc);
        x += hx * speed * dt;
        y += hy * speed * dt;
    }
};

struct RunResult {
    std::vector<double> errs, moving, tgt;
    double maxErr = 0;
    int snaps = 0, refusals = 0;
    double bytesPerS = 0;
};

// One host NPC on `path`, a guest `guestDist` m beside it (his viewpoint), the network (one-way 25 ms + up to 15 ms
// jitter, `loss` of the unreliable states lost), the guest's frames at 60 Hz.
RunResult RunPath(const Path& path, float guestDist, double loss, bool rogue, unsigned seed) {
    Publisher pub;
    pub.SetPeers({1});
    ReplicaSet rs;
    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> u(0, 1);
    struct InFlight {
        double at;
        std::vector<uint8_t> buf;
        int kind;  // 0 states, 1 infos, 2 leaves
    };
    std::vector<InFlight> wire;
    Walker w;
    w.rogue = rogue;
    bool spawned = false;
    RunResult res;
    const double frame = 1.0 / 60, round = 1.0 / 15;
    double nextRound = 0, bytes = 0;
    const double t0 = 1.0;  // the host clock starts at 1 s (ms > 0)
    for (double t = 0; t < path.Duration(); t += frame) {
        const double now = t0 + t;
        float x, y, z;
        path.At(t, x, y, z);
        if (t >= nextRound) {  // host round: the NPC as the host's game has it now
            nextRound += round;
            NpcState s = Npc(4584968, x, y);
            s.z = z;
            pub.SetViewpoints(1, {{x + guestDist, y, z}});
            pub.Round(static_cast<uint32_t>(now * 1000), now, {It(s)}, true, 0);
            for (const PeerOut& po : pub.Tick(now)) {
                const double lat = 0.025 + 0.015 * u(rng);
                for (const auto& b : PackInfos(po.infos)) wire.push_back({now + lat, b, 1});
                for (const auto& g : po.states)
                    for (const auto& b : PackStates(g.timeMs, g.npcs)) {
                        bytes += b.size();
                        if (u(rng) >= loss) wire.push_back({now + 0.025 + 0.015 * u(rng), b, 0});
                    }
            }
        }
        // guest: what arrived by now
        std::stable_sort(wire.begin(), wire.end(), [](const InFlight& a, const InFlight& b) { return a.at < b.at; });
        while (!wire.empty() && wire.front().at <= now) {
            mc::net::ByteReader r(wire.front().buf);
            if (wire.front().kind == 0) {
                StatesMsg m;
                if (ReadStates(r, m)) rs.OnStates(m, now);
            } else if (wire.front().kind == 1) {
                std::vector<Info> in;
                if (ReadInfos(r, in)) rs.OnInfos(in);
            }
            wire.erase(wire.begin());
        }
        rs.Update(now);
        Target tg;
        if (!rs.TargetOf(4584968, now, tg)) continue;
        if (!spawned) {  // the replica appears where the NPC is (paired at the same spot / spawned there)
            spawned = true;
            w.x = tg.pos.x;
            w.y = tg.pos.y;
        }
        Report rep;
        rep.key = 4584968;
        rep.pos = {w.x, w.y, z};
        rep.hx = w.hx;
        rep.hy = w.hy;
        rep.kind = 1;
        rep.at = now;
        rs.OnReport(rep);
        for (const Drive& d : rs.Commands(now, static_cast<float>(frame))) {
            w.Apply(d.lc, d.target);
            if (d.lc.fix == LocomotionCommand::Fix::Snap) ++res.snaps;
            if (d.refuse) ++res.refusals;
        }
        w.Step(static_cast<float>(frame));
        if (t > 2.0) {  // after the first states and the first walk command
            float hx, hy, hz;
            path.At(t + frame, hx, hy, hz);  // the host's NPC at the moment the replica is seen (end of the frame)
            const double e = std::hypot(w.x - hx, w.y - hy);
            res.errs.push_back(e);
            float px, py, pz;
            path.At(t, px, py, pz);
            if (std::hypot(hx - px, hy - py) / frame > 0.5) res.moving.push_back(e);
            res.maxErr = std::max(res.maxErr, e);
            float qx, qy, qz;
            path.At(t, qx, qy, qz);
            res.tgt.push_back(std::hypot(tg.pos.x - qx, tg.pos.y - qy));
        }
    }
    res.bytesPerS = bytes / path.Duration();
    return res;
}

void TestCops() {
    struct Case {
        const char* name;
        const Path* path;
        float dist;
        double loss;
    };
    const Path a(std::vector<Waypoint>(std::begin(kCop7661064), std::end(kCop7661064)));
    const Path b(std::vector<Waypoint>(std::begin(kCop4584968), std::end(kCop4584968)));
    const Case cases[] = {
        {"cop 4584968 (24.7 s chase), guest 10 m away", &b, 10, 0},
        {"cop 4584968, guest 100 m away (~6 Hz)", &b, 100, 0},
        {"cop 4584968, guest 10 m away, 2 % loss", &b, 10, 0.02},
        {"cop 7661064 (55 s, stop and go), guest 25 m away", &a, 25, 0},
        {"cop 7661064, guest 80 m away, 2 % loss", &a, 80, 0.02},
    };
    std::vector<double> all;
    double worst = 0;
    unsigned seed = 11;
    for (const Case& c : cases) {
        const RunResult r = RunPath(*c.path, c.dist, c.loss, false, seed++);
        const double p95 = Pct(r.errs, 95), mx = r.maxErr;
        all.insert(all.end(), r.errs.begin(), r.errs.end());
        worst = std::max(worst, mx);
        // per run a regression guard (a stop-and-go run alone is ~1.0-1.2 m); the target is over all of them below
        std::string name = std::string("replica: ") + c.name;
        Check(p95 <= 1.25 && mx <= 4.0 && r.refusals == 0 && r.snaps == 0, name.c_str(),
              Fmt("p95 %.2f m, max %.2f m, moving p95 %.2f m, snaps %.0f", p95, mx, Pct(r.moving, 95), r.snaps) +
                  Fmt(", target p95 %.2f m", Pct(r.tgt, 95)));
    }
    Check(Pct(all, 95) <= 1.0 && worst <= 4.0, "replica: npc.p95 <= 1.0 m and npc.max <= 4 m over all runs",
          Fmt("p95 %.2f m, max %.2f m (session 1: p95 55-60 m, max 252 m)", Pct(all, 95), worst));
    const RunResult rogue = RunPath(b, 10, 0, true, 99);
    Check(rogue.refusals == 1, "replica: a paired NPC whose own AI walks off is given up once (hidden, a copy shown)",
          Fmt("refusals %.0f, snaps %.0f", rogue.refusals, rogue.snaps));
}

// ---- receiver bookkeeping ---------------------------------------------------------------------------------------------------
void TestReceiver() {
    ReplicaSet rs;
    Info i1{5, 4584968, 4, 77, "p_mast_a_v1", 9, NameClass::Spawner};
    rs.OnInfos({i1});
    StatesMsg m;
    m.timeMs = 1000;
    Fast f;
    f.net = 5;
    f.x = 10;
    f.y = 20;
    f.z = 3;
    f.hp = 150;
    m.npcs = {f};
    rs.OnStates(m, 1.0);
    m.timeMs = 1067;
    m.npcs[0].x = 10.1f;
    rs.OnStates(m, 1.067);
    rs.Update(1.1);
    Target t;
    Check(rs.TargetOf(4584968, 1.1, t) && t.info.ai == 4 && t.hp == 150 && std::fabs(t.pos.x - 10.05f) < 0.2f,
          "receiver: a target from info + states");
    // states for a net id without an info yet are kept, shown once the info comes
    m.timeMs = 1134;
    m.npcs[0].net = 6;
    rs.OnStates(m, 1.134);
    rs.Update(1.15);
    const size_t before = rs.Targets(1.15).size();
    rs.OnInfos({{6, 777, 2, 1, "-", 0, NameClass::Spawner}});
    Check(before == 1 && rs.Targets(1.15).size() == 2, "receiver: states before their info wait for it");
    // the net id reused for another NPC: the old one is gone
    rs.OnInfos({{6, 888, 2, 1, "-", 0, NameClass::Spawner}});
    Check(!rs.TargetOf(777, 1.15, t) && rs.InfoOf(888) != nullptr, "receiver: a net id given to another NPC");
    rs.OnLeaves({5});
    Check(!rs.TargetOf(4584968, 1.15, t), "receiver: a leave removes it");
    // a death shows at once (not after the interpolation delay)
    rs.OnInfos({i1});
    m.timeMs = 1200;
    m.npcs[0].net = 5;
    m.npcs[0].flags = 0;
    rs.OnStates(m, 1.2);
    m.timeMs = 1267;
    m.npcs[0].flags = kDead;
    m.npcs[0].hp = 0;
    rs.OnStates(m, 1.267);
    rs.Update(1.27);
    Check(rs.TargetOf(4584968, 1.27, t) && (t.flags & kDead) && t.hp == 0, "receiver: a death shows at once");
    // gone after `forget` s without states
    rs.Update(5.0);
    Check(!rs.TargetOf(4584968, 5.0, t), "receiver: forgotten 3 s after the last state");
}

}  // namespace

// our message ids in the latency trace: states are the "npc" stream, infos / leaves "npc-event"
void TestIds() {
    using mc::game::latency::SystemOf;
    namespace lat = mc::game::latency;
    Check(SystemOf(0x0212) == lat::kNpc && SystemOf(0x0424) == lat::kNpc, "latency: NpcStates2 / GuestNpcStates2 = npc");
    Check(SystemOf(0x0213) == lat::kNpcEvent && SystemOf(0x0214) == lat::kNpcEvent &&
              SystemOf(0x0425) == lat::kNpcEvent && SystemOf(0x0426) == lat::kNpcEvent,
          "latency: NpcInfo / NpcLeave (both ways) = npc-event");
}

int main() {
    TestIds();
    TestCodec();
    TestRules();
    TestInterest();
    TestBandwidth();
    TestReceiver();
    TestCops();
    std::printf("%s (%d failures)\n", g_failures ? "FAILED" : "ALL PASSED", g_failures);
    return g_failures ? 1 : 0;
}
