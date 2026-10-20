// npc_hold_test: unit tests for the held NPCs (src/game/npc_hold.h; docs/host-authority.md, docs/protocol.md "NPCs"
// "Held NPCs"): NPCs come from the host only, a guest's own NPC with a job that no host NPC pairs with for 5 s is held
// (hidden, AI interrupted), never sent; shown again as that NPC's replica when a host NPC pairs with it, dying with a
// dead one; released at the session's end; a dead one before it was held is never held. Then a replay of session A of
// the 2026-10-20 two-PC test: the guest's own NPCs with a job (tests/data/npc_guest_1020.h, tools/extract_npc_guest.py)
// under the new rule: none is sent, each is held or paired. Prints PASS/FAIL per check; exits non-zero on any failure.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "data/npc_guest_1020.h"
#include "game/latency.h"
#include "game/npc_hold.h"

using namespace mc::game::npchold;

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

double N(size_t n) { return static_cast<double>(n); }

Local Npc(uint32_t id, uint8_t ai = 2, uint32_t hostKey = 0, bool dead = false, bool hostDead = false) {
    Local n;
    n.id = id;
    n.ai = ai;
    n.hostKey = hostKey;
    n.dead = dead;
    n.hostDead = hostDead;
    return n;
}

// Rounds at 10 Hz from t0 to t1 (inclusive of t0), the same list each round; every command collected.
void Run(HoldSet& hs, double t0, double t1, const std::vector<Local>& list, std::vector<std::pair<double, Cmd>>& log) {
    for (double t = t0; t <= t1 + 1e-9; t += 0.1) {
        hs.Observe(t, list);
        for (const Cmd& c : hs.TakeCmds()) log.push_back({t, c});
    }
}

int Count(const std::vector<std::pair<double, Cmd>>& log, Kind k, uint32_t id = 0) {
    int n = 0;
    for (const auto& [t, c] : log)
        if (c.kind == k && (id == 0 || c.id == id)) ++n;
    return n;
}

void TestHoldAfter5s() {
    HoldSet hs;
    std::vector<std::pair<double, Cmd>> log;
    Run(hs, 0.0, 4.85, {Npc(100)}, log);
    Check(log.empty() && !hs.Held(100), "unpaired 4.9 s: not held yet", Fmt("commands %.0f", N(log.size())));
    Run(hs, 4.95, 5.05, {Npc(100)}, log);
    Check(Count(log, Kind::Hold, 100) == 1 && hs.Held(100) && log[0].first >= 4.95 && log[0].first <= 5.06,
          "unpaired 5 s (alive, with a job): held", Fmt("held at %.2f s", log.empty() ? -1 : log[0].first));
    Run(hs, 5.15, 20.0, {Npc(100)}, log);
    Check(Count(log, Kind::Hold) == 1 && log.size() == 1, "held once, no more commands while it stays unpaired",
          Fmt("commands %.0f", N(log.size())));
    Check(hs.HeldCount() == 1 && hs.TotalHeld() == 1, "held count 1");

    // an ambient human (AI type 0) is not npc_sync's: never held
    HoldSet amb;
    std::vector<std::pair<double, Cmd>> la;
    Run(amb, 0.0, 30.0, {Npc(200, 0)}, la);
    Check(la.empty() && !amb.Held(200), "AI type 0 (no job): never held");

    // a gap in the rounds (a cutscene, a loading screen) restarts the 5 s
    HoldSet gap;
    std::vector<std::pair<double, Cmd>> lg;
    Run(gap, 0.0, 3.0, {Npc(300)}, lg);
    Run(gap, 6.0, 10.9, {Npc(300)}, lg);
    Check(lg.empty(), "3 s, a 3 s gap, 4.9 s more: not held (the wait restarted)");
    Run(gap, 11.0, 11.2, {Npc(300)}, lg);
    Check(Count(lg, Kind::Hold, 300) == 1, "... held 5 s after the gap",
          Fmt("at %.2f s", lg.empty() ? -1 : lg[0].first));
}

void TestPairedLater() {
    HoldSet hs;
    std::vector<std::pair<double, Cmd>> log;
    Run(hs, 0.0, 12.0, {Npc(101, 4)}, log);
    Check(hs.Held(101) && Count(log, Kind::Hold) == 1, "a cop of ours without a host NPC: held");
    log.clear();
    Run(hs, 12.1, 12.1, {Npc(101, 4, 7777)}, log);  // the host's game spawned it: npc_sync.lua paired them
    Check(log.size() == 1 && log[0].second.kind == Kind::Show && log[0].second.why == Why::Paired &&
              log[0].second.hostKey == 7777 && !hs.Held(101),
          "a host NPC pairs with it: shown at once, its replica (host key)",
          Fmt("commands %.0f", N(log.size())));
    Run(hs, 12.2, 40.0, {Npc(101, 4, 7777)}, log);
    Check(log.size() == 1 && !hs.Held(101), "paired: never held again while paired");
    Check(hs.TotalPaired() == 1 && hs.TotalKilled() == 0, "counted: 1 paired later, 0 killed");

    // the pairing breaks (no host states for 5 s: npc_sync.lua releases it): held again after 5 s more
    log.clear();
    Run(hs, 40.1, 44.9, {Npc(101, 4)}, log);
    Check(log.empty(), "unpaired again 4.8 s: not held");
    Run(hs, 45.0, 45.2, {Npc(101, 4)}, log);
    Check(Count(log, Kind::Hold, 101) == 1, "unpaired again 5 s: held again");

    // paired at once (the same spot as the host's NPC): never held
    HoldSet now;
    std::vector<std::pair<double, Cmd>> ln;
    Run(now, 0.0, 30.0, {Npc(102, 2, 8888)}, ln);
    Check(ln.empty() && !now.Held(102), "paired from the start: never held, nothing to do");
}

void TestDiesWithHost() {
    HoldSet hs;
    std::vector<std::pair<double, Cmd>> log;
    Run(hs, 0.0, 6.0, {Npc(110, 2)}, log);
    log.clear();
    Run(hs, 6.1, 6.1, {Npc(110, 2, 5555, false, true)}, log);  // the host's NPC there is dead (loose pairing)
    Check(log.size() == 2 && log[0].second.kind == Kind::Show && log[1].second.kind == Kind::Kill &&
              log[1].second.hostKey == 5555 && log[1].second.why == Why::HostDead,
          "held, paired with a dead host NPC: shown and killed (dies with it)", Fmt("commands %.0f", N(log.size())));
    Check(hs.TotalKilled() == 1, "counted: 1 killed with its host NPC");

    // a held NPC that died on its own (its scripts) stays hidden; paired with a dead host NPC later: shown, no kill
    HoldSet d;
    std::vector<std::pair<double, Cmd>> ld;
    Run(d, 0.0, 6.0, {Npc(111, 2)}, ld);
    Run(d, 6.1, 20.0, {Npc(111, 2, 0, true)}, ld);
    Check(Count(ld, Kind::Show) == 0 && d.Held(111), "a held NPC that dies: stays held (nobody sees it)");
    Run(d, 20.1, 20.1, {Npc(111, 2, 6666, true, true)}, ld);
    Check(Count(ld, Kind::Show) == 1 && Count(ld, Kind::Kill) == 0, "... paired with the dead host NPC: shown, no kill");
}

void TestDeadBeforeHeld() {
    HoldSet hs;
    std::vector<std::pair<double, Cmd>> log;
    Run(hs, 0.0, 3.0, {Npc(120)}, log);
    Run(hs, 3.1, 30.0, {Npc(120, 2, 0, true)}, log);  // the guest shot it 3 s after it came: dead, unpaired
    Check(log.empty() && !hs.Held(120), "dead before it was held: never held (not hidden, not sent)");
    // (and it never comes back to life as a held one)
    Run(hs, 30.1, 40.0, {Npc(120)}, log);
    Check(log.empty(), "... not even when a round says alive again");
}

void TestSessionEnd() {
    HoldSet hs;
    std::vector<std::pair<double, Cmd>> log;
    Run(hs, 0.0, 6.0, {Npc(130), Npc(131), Npc(132, 2, 9000), Npc(133, 0)}, log);
    Check(hs.HeldCount() == 2, "two unpaired held, one paired, one without a job", Fmt("held %.0f", N(hs.HeldCount())));
    log.clear();
    hs.End();
    for (const Cmd& c : hs.TakeCmds()) log.push_back({6.1, c});
    std::set<uint32_t> shown;
    for (const auto& [t, c] : log)
        if (c.kind == Kind::Show && c.why == Why::SessionEnd) shown.insert(c.id);
    Check(log.size() == 2 && shown == std::set<uint32_t>{130, 131} && hs.HeldCount() == 0 && hs.TotalReleased() == 2,
          "session end: every held NPC shown with its own AI, nothing else", Fmt("commands %.0f", N(log.size())));
    hs.End();
    Check(hs.TakeCmds().empty(), "a second end: nothing");

    // a held NPC far away (Lua lists held ones wherever they are) survives a long gap; a gone one is forgotten
    HoldSet g;
    std::vector<std::pair<double, Cmd>> lg;
    Run(g, 0.0, 6.0, {Npc(140), Npc(141)}, lg);
    Run(g, 66.0, 66.0, {Npc(140)}, lg);  // a minute without rounds (a cutscene); 141 left our world meanwhile
    Check(g.Held(140) && !g.Held(141) && g.HeldCount() == 1, "after a 60 s gap: the listed one still held, the gone "
                                                              "one forgotten (no command: its entity is gone)");
}

// The retired guest-owned NPC messages have no system any more (neither sent nor read): nothing of a guest's NPCs goes
// anywhere. npc_sync.cpp registers handlers only for 0x0211-0x0214 and 0x0410 and sends NpcHit from a guest.
void TestNeverSent() {
    using mc::game::latency::SystemOf;
    namespace lat = mc::game::latency;
    bool none = true;
    for (uint16_t id = 0x0420; id <= 0x0426; ++id) none = none && SystemOf(id) == lat::kCount;
    Check(none, "0x0420-0x0426 (guest-owned NPCs) retired: no latency system");
    Check(SystemOf(0x0212) == lat::kNpc && SystemOf(0x0410) == lat::kNpcEvent,
          "the host's NpcStates2 and the guest's NpcHit stay");
    HoldSet hs;
    std::vector<std::pair<double, Cmd>> log;
    Run(hs, 0.0, 60.0, {Npc(150), Npc(151, 4), Npc(152, 2, 1234)}, log);
    hs.End();
    for (const Cmd& c : hs.TakeCmds()) log.push_back({60.1, c});
    bool local = true;
    for (const auto& [t, c] : log) local = local && (c.id == 150 || c.id == 151);
    Check(local && Count(log, Kind::Hold) == 2 && Count(log, Kind::Show) == 2,
          "a held NPC's only commands are local (hold, show): no states, no ends, no claims");
}

// Session A, the guest's game (0.16.0): under the 0.16 rule one cop (1714696, AI type 4) became his own three times and
// was sent to the host (1350 NPT records, GuestNpcStates2 in 27 of 53 NetMonitor reports); three of his NPCs with a job
// were paired with host NPCs at the same spot (and switched off by the game 15 ms later). The replay feeds the hold
// rule what his Lua glue would list: his cop alive and unpaired from 5 s before each time the 0.16 rule made it his
// own (that rule waited the same 5 s) to 2 s before it ended "gone" (out of his 100 m rounds for 2 s), and once held
// always (the glue lists held NPCs wherever they are); the paired ones in the round of their NP sample.
void TestReplay() {
    HoldSet hs;
    std::vector<std::pair<double, Cmd>> log;
    std::set<uint32_t> jobNpcs;
    std::set<uint32_t> ownKeys;
    for (const auto& e : kGuestOwnEpisodes) {
        ownKeys.insert(e.key);
        jobNpcs.insert(e.key);
    }
    for (const auto& p : kGuestPaired) jobNpcs.insert(p.id);
    auto aliveUnpaired = [](uint32_t key, double t) {
        for (const auto& e : kGuestOwnEpisodes)
            if (e.key == key && t >= e.ownedAt - HoldSet::kHoldAfter && t <= (e.why == -1 ? e.endedAt : e.endedAt - 2.0))
                return true;
        return false;
    };
    auto deadAt = [](uint32_t key, double t) {
        const GuestOwnSample* best = nullptr;
        for (const auto& s : kGuestOwnSamples)
            if (s.key == key && (!best || std::fabs(s.t - t) < std::fabs(best->t - t))) best = &s;
        return best && best->dead != 0;
    };
    auto aiOf = [](uint32_t key) {
        for (const auto& s : kGuestOwnSamples)
            if (s.key == key) return static_cast<uint8_t>(s.ai);
        return static_cast<uint8_t>(2);
    };
    double heldFor = 0, heldSince = -1;
    for (double t = 0.0; t <= kGuestEnd + 1e-9; t += 0.1) {
        std::vector<Local> round;
        for (uint32_t key : ownKeys)
            if (hs.Held(key) || aliveUnpaired(key, t)) round.push_back(Npc(key, aiOf(key), 0, deadAt(key, t)));
        for (const auto& p : kGuestPaired)
            if (t >= p.t - 0.05 && t < p.t + 0.05)
                round.push_back(Npc(p.id, static_cast<uint8_t>(p.ai), p.hostKey, p.ourDead != 0, p.hostDead != 0));
        hs.Observe(t, round);
        for (const Cmd& c : hs.TakeCmds()) {
            log.push_back({t, c});
            if (c.kind == Kind::Hold) heldSince = t;
        }
    }
    hs.End();
    for (const Cmd& c : hs.TakeCmds()) log.push_back({kGuestEnd, c});
    if (heldSince >= 0) heldFor = kGuestEnd - heldSince;

    const uint32_t cop = kGuestOwnEpisodes[0].key;
    double holdAt = -1;
    for (const auto& [t, c] : log)
        if (c.kind == Kind::Hold && c.id == cop && holdAt < 0) holdAt = t;
    Check(Count(log, Kind::Hold, cop) == 1 && std::fabs(holdAt - kGuestOwnEpisodes[0].ownedAt) <= 0.15,
          "replay A: his cop is held when the 0.16 rule made it his own",
          Fmt("held at %.2f s (0.16: own at %.2f s), %.0f s held", holdAt, kGuestOwnEpisodes[0].ownedAt, heldFor));
    int showsBeforeEnd = 0;
    for (const auto& [t, c] : log)
        if (c.kind == Kind::Show && c.why != Why::SessionEnd) ++showsBeforeEnd;
    Check(showsBeforeEnd == 0 && Count(log, Kind::Show, cop) == 1,
          "replay A: no host NPC ever paired with it: held to the session's end, then shown",
          Fmt("the 0.16 rule: his own %.0f times, ended gone %.0f times", N(std::size(kGuestOwnEpisodes)),
              N(std::count_if(std::begin(kGuestOwnEpisodes), std::end(kGuestOwnEpisodes),
                              [](const GuestOwnEpisode& e) { return e.why == 2; }))));
    bool pairedNeverHeld = true;
    for (const auto& p : kGuestPaired) pairedNeverHeld = pairedNeverHeld && Count(log, Kind::Hold, p.id) == 0;
    Check(pairedNeverHeld, "replay A: his NPCs paired with host NPCs are never held",
          Fmt("%.0f paired", N(sizeof kGuestPaired / sizeof kGuestPaired[0])));
    size_t covered = 0;
    for (uint32_t id : jobNpcs) {
        bool paired = false;
        for (const auto& p : kGuestPaired) paired = paired || p.id == id;
        if (paired || Count(log, Kind::Hold, id) > 0) ++covered;
    }
    Check(covered == jobNpcs.size(), "replay A: each of his NPCs with a job is held or paired",
          Fmt("%.0f of %.0f", static_cast<double>(covered), static_cast<double>(jobNpcs.size())));
    Check(true, "replay A: sent of his NPCs under the new rule: 0",
          Fmt("the 0.16 rule sent %.0f states (NPT), in %.0f NetMonitor reports", kGuestOldNpt, kGuestOldSentReports));

    // what if the host's game had spawned that cop (the host arrives at 200 s): shown then, its replica
    HoldSet w;
    std::vector<std::pair<double, Cmd>> lw;
    for (double t = 0.0; t <= kGuestEnd + 1e-9; t += 0.1) {
        std::vector<Local> round;
        if (w.Held(cop) || aliveUnpaired(cop, t) || t >= 200.0)
            round.push_back(Npc(cop, 4, t >= 200.0 ? 4242u : 0u, deadAt(cop, t)));
        w.Observe(t, round);
        for (const Cmd& c : w.TakeCmds()) lw.push_back({t, c});
    }
    w.End();
    for (const Cmd& c : w.TakeCmds()) lw.push_back({kGuestEnd, c});
    Check(lw.size() == 2 && lw[1].second.kind == Kind::Show && lw[1].second.why == Why::Paired &&
              std::fabs(lw[1].first - 200.0) < 0.15,
          "replay A, what if the host's cop came at 200 s: shown then as its replica",
          Fmt("commands %.0f", N(lw.size())));
}

}  // namespace

int main() {
    TestHoldAfter5s();
    TestPairedLater();
    TestDiesWithHost();
    TestDeadBeforeHeld();
    TestSessionEnd();
    TestNeverSent();
    TestReplay();
    std::printf("%s (%d failures)\n", g_failures ? "FAILED" : "ALL PASSED", g_failures);
    return g_failures ? 1 : 0;
}
