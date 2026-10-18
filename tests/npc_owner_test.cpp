// npc_owner_test: unit tests for the NPC ownership logic between games (src/game/npc_owner.h): the shared NPC state
// codec, the guest's choice of the NPCs only his game has (pairing window, caps, deaths, hand-back, copies never sent),
// the host's mirrors of them (mirror ids, claims when the host's own game spawns the NPC, refusals, timeouts, dead
// mirrors) and a guest-host round trip over the wire format. Prints PASS/FAIL per check; exits non-zero on any failure.
#include <cstdio>
#include <string>
#include <vector>

#include "game/npc_owner.h"
#include "net/wire.h"

using namespace mc::game::npcown;

namespace {

int g_failures = 0;

void Check(bool ok, const char* name, const std::string& detail = {}) {
    std::printf("%s  %-64s %s\n", ok ? "PASS" : "FAIL", name, detail.c_str());
    std::fflush(stdout);
    if (!ok) ++g_failures;
}

NpcState Npc(uint32_t key, float x, float y, uint8_t ai = 2, uint32_t tag = 7, bool dead = false) {
    NpcState s;
    s.key = key;
    s.x = x;
    s.y = y;
    s.z = 10;
    s.hx = 0;
    s.hy = 1;
    s.hp = dead ? 0 : 200;
    s.flags = dead ? 1 : 0;
    s.ai = ai;
    s.tag = tag;
    s.weapon = "p_mast_a_v1";
    s.profile = 1234567890123ull;
    s.shots = 3;
    return s;
}

LocalNpc Local(const NpcState& s, float dist, uint32_t hostKey = 0) {
    LocalNpc n;
    n.s = s;
    n.dist = dist;
    n.hostKey = hostKey;
    return n;
}

template <class T, class P>
size_t CountIf(const std::vector<T>& v, P p) {
    size_t n = 0;
    for (const auto& x : v) n += p(x) ? 1 : 0;
    return n;
}

// ---- codec --------------------------------------------------------------------------------------------------------------
void TestCodec() {
    NpcState a = Npc(4000000000u, -123.25f, 456.5f, 2, 4294967295u);
    NpcState b;
    Check(ParseState(FormatState(a), b), "state line parses");
    Check(b.key == a.key && b.tag == a.tag && b.profile == a.profile && b.weapon == a.weapon && b.hp == a.hp &&
              b.shots == a.shots && b.ai == a.ai && b.x == a.x,
          "state line round trip (u32 key and tag, u64 profile)");
    NpcState bad;
    Check(!ParseState("12 nan 0 0 0 1 100 0 2 0 - 0 0", bad), "a NaN position is refused");
    NpcState w;
    ParseState("5 1 2 3 0 1 100 0 2 0 bad;weapon 0 0", w);
    Check(w.weapon == "-", "an unsafe weapon name becomes -");

    mc::net::ByteWriter wr;
    WriteState(wr, a);
    mc::net::ByteReader rd(wr.Span());
    NpcState c;
    Check(ReadState(rd, c) && c.key == a.key && c.profile == a.profile && c.tag == a.tag && rd.AtEnd(),
          "wire round trip");
    mc::net::ByteReader shortRd(wr.Span().subspan(0, 10));
    NpcState d;
    Check(!ReadState(shortRd, d), "a short record is refused");

    std::vector<NpcState> many;
    for (uint32_t i = 0; i < 40; ++i) many.push_back(Npc(i + 1, 1.0f * i, 0));
    auto msgs = PackStates(many);
    size_t total = 0;
    bool sizes = true;
    for (const auto& m : msgs) {
        total += m[0];
        sizes &= m[0] <= kPerPacket && m.size() <= kMaxStatesBytes;
    }
    Check(msgs.size() == 3 && total == 40 && sizes, "40 states: 3 messages of at most 16, under 1000 bytes",
          std::to_string(msgs.size()) + " messages");
    for (auto& s : many) s.weapon = std::string(48, 'w');
    msgs = PackStates(many);
    sizes = true;
    total = 0;
    for (const auto& m : msgs) {
        total += m[0];
        sizes &= m.size() <= kMaxStatesBytes;
    }
    Check(total == 40 && sizes, "long weapon names: fewer per message, still under 1000 bytes",
          std::to_string(msgs.size()) + " messages");
    Check(PackStates({}).empty(), "no states, no message");
}

// ---- guest --------------------------------------------------------------------------------------------------------------
void TestGuestWindow() {
    GuestOwner g;
    const NpcState enemy = Npc(10, 0, 0), civ = Npc(11, 1, 0, 0), paired = Npc(12, 2, 0);
    double t = 0;
    for (; t < GuestOwner::kOwnWait - 0.2; t += 0.1) g.Observe(t, {Local(enemy, 5), Local(civ, 6), Local(paired, 7, 900)});
    Check(!g.Owned(10) && g.TakeStates().empty(), "not ours before the pairing window");
    for (; t < GuestOwner::kOwnWait + 0.3; t += 0.1) g.Observe(t, {Local(enemy, 5), Local(civ, 6), Local(paired, 7, 900)});
    Check(g.Owned(10), "an unpaired enemy is ours after the window");
    Check(!g.Owned(11), "a civilian (AI type 0) is never ours");
    Check(!g.Owned(12), "an NPC paired with a host NPC is never ours");
    auto ch = g.TakeOwnedChanges();
    Check(ch.size() == 1 && ch[0].first == 10 && ch[0].second, "one ownership change reported");
    auto st = g.TakeStates();
    Check(st.size() == 1 && st[0].key == 10, "its state is sent");

    // a pairing in the window restarts it
    GuestOwner g2;
    g2.Observe(0, {Local(enemy, 5)});
    g2.Observe(3, {Local(enemy, 5)});
    g2.Observe(3.5, {Local(enemy, 5, 77)});
    g2.Observe(4, {Local(enemy, 5)});
    g2.Observe(6, {Local(enemy, 5)});
    Check(!g2.Owned(10), "paired for a moment in the window: the wait starts again");
    g2.Observe(9.1, {Local(enemy, 5)});
    Check(g2.Owned(10), "... and it is ours 5 s after that");
}

void TestGuestCaps() {
    GuestOwner g;
    std::vector<LocalNpc> list;
    for (uint32_t i = 0; i < 30; ++i) list.push_back(Local(Npc(100 + i, 1.0f * i, 0), 100.0f - i));  // farthest first
    for (double t = 0; t <= 6; t += 0.1) g.Observe(t, list);
    Check(g.OwnedCount() == GuestOwner::kMaxOwned, "at most kMaxOwned NPCs are ours",
          std::to_string(g.OwnedCount()));
    auto st = g.TakeStates();
    bool sorted = true;
    for (size_t i = 1; i < st.size(); ++i) sorted &= st[i - 1].key > st[i].key;  // higher key = nearer here
    Check(st.size() <= GuestOwner::kMaxSend && sorted, "at most kMaxSend states per round, nearest first",
          std::to_string(st.size()));
}

void TestGuestCopies() {
    GuestOwner g;
    g.SetCopies({50});
    const NpcState copy = Npc(50, 0, 0), own = Npc(51, 3, 0);
    for (double t = 0; t <= 6; t += 0.1) g.Observe(t, {Local(copy, 1), Local(own, 2)});
    auto st = g.TakeStates();
    Check(!g.Owned(50) && CountIf(st, [](const NpcState& s) { return s.key == 50; }) == 0,
          "our spawned copy of a host NPC is never ours, never sent");
    Check(g.Owned(51), "the other one is ours");
    g.TakeEnds();
    g.SetCopies({50, 51});
    auto ends = g.TakeEnds();
    Check(!g.Owned(51) && ends.size() == 1 && ends[0].reason == EndReason::Gone,
          "an id that turns out to be a copy ends (gone)");
}

void TestGuestEnds() {
    GuestOwner g;
    NpcState a = Npc(20, 0, 0), b = Npc(21, 5, 0), c = Npc(22, 9, 0);
    double t = 0;
    for (; t <= 6; t += 0.1) g.Observe(t, {Local(a, 1), Local(b, 2), Local(c, 3)});
    Check(g.OwnedCount() == 3, "three ours");
    g.TakeEnds();
    g.TakeOwnedChanges();

    // a: the host's game has it now (our pairing found it)
    a.hp = 120;
    g.Observe(t, {Local(a, 1, 555), Local(b, 2), Local(c, 3)});
    auto ends = g.TakeEnds();
    Check(ends.size() == 1 && ends[0].key == 20 && ends[0].reason == EndReason::HandedBack && ends[0].hostKey == 555 &&
              ends[0].hp == 120,
          "paired after all: handed back with the host key and its health");
    auto ch = g.TakeOwnedChanges();
    Check(ch.size() == 1 && ch[0].first == 20 && !ch[0].second, "the change says not ours");

    // b: dies after the host hit it
    t += 0.1;
    g.HostHit(t, 21);
    b.flags = 1;
    b.hp = 0;
    g.Observe(t + 0.5, {Local(a, 1, 555), Local(b, 2), Local(c, 3)});
    ends = g.TakeEnds();
    Check(ends.size() == 1 && ends[0].key == 21 && ends[0].reason == EndReason::Died && ends[0].byHost,
          "died 0.5 s after the host's hit: the host's kill");
    g.Observe(t + 0.6, {Local(a, 1, 555), Local(b, 2), Local(c, 3)});
    Check(g.TakeEnds().empty(), "a death is sent once");
    b.flags = 0;
    b.hp = 50;  // (never happens, but a body that "lives" again is not ours again)
    for (double u = t + 0.7; u < t + 7; u += 0.1) g.Observe(u, {Local(a, 1, 555), Local(b, 2), Local(c, 3)});
    Check(!g.Owned(21), "a dead NPC is never ours again");
    t += 7;

    // c: gone from our world
    g.TakeEnds();
    g.Observe(t, {});
    Check(g.Owned(22) && g.TakeEnds().empty(), "out of one round: still ours");
    g.Observe(t + GuestOwner::kGoneAfter + 0.1, {});
    ends = g.TakeEnds();
    Check(ends.size() == 1 && ends[0].key == 22 && ends[0].reason == EndReason::Gone, "gone after kGoneAfter s");

    // a death by our own side (no host hit)
    GuestOwner g2;
    NpcState d = Npc(30, 0, 0);
    for (double u = 0; u <= 6; u += 0.1) g2.Observe(u, {Local(d, 1)});
    d.flags = 1;
    g2.Observe(7, {Local(d, 1)});
    ends = g2.TakeEnds();
    Check(ends.size() == 1 && ends[0].reason == EndReason::Died && !ends[0].byHost, "died of our own hits: not the host's kill");

    // died before it was ours: nothing
    GuestOwner g3;
    NpcState e = Npc(31, 0, 0);
    g3.Observe(0, {Local(e, 1)});
    e.flags = 1;
    for (double u = 0.1; u <= 8; u += 0.1) g3.Observe(u, {Local(e, 1)});
    Check(!g3.Owned(31) && g3.TakeEnds().empty(), "died before it was ours: never sent");
}

void TestGuestClaim() {
    GuestOwner g;
    NpcState a = Npc(40, 0, 0);
    for (double t = 0; t <= 6; t += 0.1) g.Observe(t, {Local(a, 1)});
    Check(g.Claim(40, 900), "a claim of ours is carried out");
    Check(!g.Claim(41, 900), "a claim of an NPC that is not ours is not");
    g.TakeEnds();
    g.Refuse(40, 900);
    auto ends = g.TakeEnds();
    Check(ends.size() == 1 && ends[0].reason == EndReason::Refused && ends[0].hostKey == 900 && g.Owned(40),
          "refused: the host is told, it stays ours");
}

// ---- host ---------------------------------------------------------------------------------------------------------------
void TestHostMirrors() {
    HostMirrors h;
    h.OnState(0, 1, Npc(10, 0, 0));
    h.OnState(0, 2, Npc(10, 50, 0));  // the same key on another guest's game: another NPC
    h.OnState(0, 0, Npc(11, 0, 0));   // from the host itself: ignored
    h.OnState(0, 1, Npc(12, 0, 0, 2, 7, true));  // dead at first sight: no mirror
    auto cmds = h.TakeCmds();
    Check(h.Count() == 2 && cmds.size() == 2 && cmds[0].mid != cmds[1].mid, "a mirror per (guest, key)",
          std::to_string(h.Count()));
    uint8_t from = 0;
    uint32_t key = 0;
    Check(h.Lookup(cmds[1].mid, from, key) && from == 2 && key == 10, "a mirror id leads back to guest and key");
    for (uint32_t i = 0; i < 40; ++i) h.OnState(0, 3, Npc(1000 + i, 1.0f * i, 0));
    size_t of3 = 0;
    for (const auto& c : h.TakeCmds()) {
        uint8_t f = 0;
        uint32_t k = 0;
        if (h.Lookup(c.mid, f, k) && f == 3) ++of3;
    }
    Check(of3 == HostMirrors::kMaxPerGuest, "at most kMaxPerGuest mirrors per guest", std::to_string(of3));
    h.Expire(HostMirrors::kRelease + 0.5);
    auto ends = h.TakeCmds();
    Check(h.Count() == 0 && CountIf(ends, [](const MirrorCmd& c) {
              return c.kind == MirrorCmd::Kind::End && c.reason == EndReason::Gone;
          }) == ends.size() && !ends.empty(),
          "no state for kRelease s: the mirrors go");

    HostMirrors h2;
    h2.OnState(0, 1, Npc(10, 0, 0));
    h2.OnState(0, 2, Npc(11, 0, 0));
    h2.TakeCmds();
    h2.OnPlayerLeft(1);
    Check(h2.Count() == 1, "a guest who left takes his mirrors along");
}

// The host's own NPCs of this round, kept for `secs` from t0 (10 Hz).
void Natives(HostMirrors& h, double t0, double secs, const std::vector<NpcState>& list) {
    for (double t = t0; t <= t0 + secs + 1e-9; t += 0.1) h.ObserveNatives(t, list);
}

uint32_t OnlyMid(HostMirrors& h) {
    auto c = h.TakeCmds();
    return c.empty() ? 0 : c[0].mid;
}

void TestHostClaims() {
    {  // the same spot: claimed once the native exists kClaimWait s
        HostMirrors h;
        h.OnState(0, 1, Npc(10, 100, 100));
        const uint32_t mid = OnlyMid(h);
        h.ObserveNatives(0.1, {Npc(500, 100.5f, 100)});
        h.ObserveNatives(1.0, {Npc(500, 100.5f, 100)});
        Check(h.TakeClaims().empty(), "no claim before the native existed kClaimWait s");
        h.OnState(1.5, 1, Npc(10, 100, 100));
        Natives(h, 1.1, 2.0, {Npc(500, 100.5f, 100)});
        auto cl = h.TakeClaims();
        Check(cl.size() == 1 && cl[0].to == 1 && cl[0].guestKey == 10 && cl[0].hostKey == 500 && cl[0].mid == mid &&
                  !cl[0].dead,
              "same spot: claimed for that guest's NPC");
        Natives(h, 3.2, 1.0, {Npc(500, 100.5f, 100)});
        Check(h.TakeClaims().empty(), "a pending claim is not sent again");
        h.TakeCmds();
        GuestEnd e;
        e.key = 10;
        e.reason = EndReason::HandedBack;
        e.hostKey = 500;
        e.hp = 90;
        h.OnEnd(4.3, 1, e);
        auto cmds = h.TakeCmds();
        Check(h.Count() == 0 && cmds.size() == 1 && cmds[0].kind == MirrorCmd::Kind::End &&
                  cmds[0].reason == EndReason::HandedBack && cmds[0].hostKey == 500 && cmds[0].hp == 90,
              "handed back: the mirror goes (its copy with it), health for our NPC");
        h.OnState(4.4, 1, Npc(10, 100, 100));
        Check(h.Count() == 0, "a late state after the end makes no new mirror (no doubles)");
        h.Expire(4.3 + HostMirrors::kEndedMemory + 0.1);
        h.OnState(4.3 + HostMirrors::kEndedMemory + 0.2, 1, Npc(10, 100, 100));
        Check(h.Count() == 1, "... later it may be the guest's again");
    }
    {  // kinds and distances
        HostMirrors h;
        h.OnState(0, 1, Npc(10, 0, 0, 2, 7));
        h.TakeCmds();
        std::vector<NpcState> nat = {Npc(600, 1, 0, 2, 8),     // same spot, other tag
                                     Npc(601, 20, 0, 0, 7),    // civilian
                                     Npc(602, 45, 0, 2, 7),    // same tag, too far
                                     Npc(603, 30, 0, 2, 7)};   // same tag, 30 m: this one
        for (double t = 0; t <= 3; t += 0.1) {
            h.OnState(t, 1, Npc(10, 0, 0, 2, 7));
            h.ObserveNatives(t, nat);
        }
        auto cl = h.TakeClaims();
        Check(cl.size() == 1 && cl[0].hostKey == 603, "same tag within kClaimDist wins; other tag, civilian, too far do not",
              cl.empty() ? "none" : std::to_string(cl[0].hostKey));
        NpcState up = Npc(604, 10, 0, 2, 7);
        up.z = 30;
        HostMirrors h2;
        for (double t = 0; t <= 3; t += 0.1) {
            h2.OnState(t, 1, Npc(10, 0, 0, 2, 7));
            h2.ObserveNatives(t, {up});
        }
        Check(h2.TakeClaims().empty(), "another floor (20 m up) is not it");
    }
    {  // two mirrors, two natives: nearest pairs, each native once
        HostMirrors h;
        std::vector<NpcState> nat = {Npc(700, 10, 0, 2, 7), Npc(701, 0.5f, 0, 2, 7)};
        for (double t = 0; t <= 3; t += 0.1) {
            h.OnState(t, 1, Npc(1, 0, 0, 2, 7));
            h.OnState(t, 1, Npc(2, 12, 0, 2, 7));
            h.ObserveNatives(t, nat);
        }
        auto cl = h.TakeClaims();
        bool ok = cl.size() == 2;
        for (const auto& c : cl) ok &= (c.guestKey == 1 && c.hostKey == 701) || (c.guestKey == 2 && c.hostKey == 700);
        Check(ok, "two mirrors, two natives: the same spot first, then the nearest");
    }
    {  // refusals and timeouts
        HostMirrors h;
        std::vector<NpcState> nat = {Npc(800, 1, 0, 2, 7)};
        double t = 0;
        for (; t <= 3; t += 0.1) {
            h.OnState(t, 1, Npc(10, 0, 0, 2, 7));
            h.ObserveNatives(t, nat);
        }
        Check(h.TakeClaims().size() == 1, "claimed");
        GuestEnd r;
        r.key = 10;
        r.reason = EndReason::Refused;
        r.hostKey = 800;
        h.OnEnd(t, 1, r);
        for (double u = t; u <= t + 5; u += 0.1) {
            h.OnState(u, 1, Npc(10, 0, 0, 2, 7));
            h.ObserveNatives(u, nat);
        }
        Check(h.TakeClaims().empty(), "refused: that pair is not tried again soon");
        nat.push_back(Npc(801, 2, 0, 2, 7));
        for (double u = t + 5; u <= t + 8; u += 0.1) {
            h.OnState(u, 1, Npc(10, 0, 0, 2, 7));
            h.ObserveNatives(u, nat);
        }
        auto cl = h.TakeClaims();
        Check(cl.size() == 1 && cl[0].hostKey == 801, "another native may be it");
        t += 8;
        for (double u = t; u <= t + HostMirrors::kClaimTimeout + 0.5; u += 0.1) {
            h.OnState(u, 1, Npc(10, 0, 0, 2, 7));
            h.Expire(u);
        }
        nat = {Npc(801, 2, 0, 2, 7)};
        for (double u = t + 4; u <= t + 6; u += 0.1) {
            h.OnState(u, 1, Npc(10, 0, 0, 2, 7));
            h.ObserveNatives(u, nat);
        }
        Check(h.TakeClaims().empty(), "no answer in kClaimTimeout s counts as refused");
    }
    {  // feedback guard: our copies are never natives
        HostMirrors h;
        h.SetCopies({900});
        for (double t = 0; t <= 3; t += 0.1) {
            h.OnState(t, 1, Npc(10, 0, 0, 2, 7));
            h.ObserveNatives(t, {Npc(900, 0, 0, 2, 7)});
        }
        Check(h.TakeClaims().empty() && h.IsCopy(900), "our own copy of the mirror never claims it");
    }
}

void TestHostDead() {
    HostMirrors h;
    h.OnState(0, 1, Npc(10, 0, 0, 2, 7));
    const uint32_t mid = OnlyMid(h);
    GuestEnd d;
    d.key = 10;
    d.reason = EndReason::Died;
    h.OnEnd(1, 1, d);
    auto cmds = h.TakeCmds();
    Check(cmds.size() == 1 && cmds[0].kind == MirrorCmd::Kind::Died && cmds[0].mid == mid, "died on the guest's game");
    h.OnState(1.1, 1, Npc(10, 0, 0, 2, 7));
    Check(h.TakeCmds().empty(), "dead stays dead");
    h.Expire(30);
    Check(h.Count() == 1, "a dead mirror is remembered");
    Natives(h, 30, 3.0, {Npc(950, 5, 0, 2, 7)});
    auto cl = h.TakeClaims();
    cmds = h.TakeCmds();
    Check(cl.size() == 1 && cl[0].dead && cl[0].hostKey == 950, "our late spawn of a dead mirror is claimed (dead)");
    Check(CountIf(cmds, [](const MirrorCmd& c) { return c.kind == MirrorCmd::Kind::KillNative && c.hostKey == 950; }) ==
                  1 &&
              h.Count() == 0,
          "... it dies here too and the record goes");
    HostMirrors h2;
    h2.OnState(0, 1, Npc(10, 0, 0, 2, 7));
    h2.OnEnd(1, 1, d);
    h2.Expire(1 + HostMirrors::kDeadMemory + 1);
    Check(h2.Count() == 0, "a dead mirror is forgotten after kDeadMemory s");
}

// ---- guest and host together, over the wire format ---------------------------------------------------------------------
void TestRoundTrip() {
    GuestOwner g;
    HostMirrors h;
    NpcState enemy = Npc(42, 10, 10, 2, 7);
    size_t mirrorsSeen = 0, copies = 0;
    uint32_t pairedHost = 0;
    bool claimSeen = false, handedBack = false, oneCopyWhileAlone = true;
    for (double t = 0; t < 20; t += 0.1) {
        // guest round: our enemy; the host's NPC 4242 appears on the host's game at t = 9 (the host arrives)
        g.Observe(t, {Local(enemy, 3, pairedHost)});
        for (auto& buf : PackStates(g.TakeStates())) {
            mc::net::ByteReader r(buf);
            const uint8_t n = r.U8();
            for (uint8_t i = 0; i < n; ++i) {
                NpcState s;
                if (ReadState(r, s)) h.OnState(t, 1, s);
            }
        }
        for (const auto& e : g.TakeEnds()) {
            handedBack |= e.reason == EndReason::HandedBack && e.hostKey == 4242;
            h.OnEnd(t, 1, e);
        }
        std::vector<NpcState> natives;
        if (t >= 9) natives.push_back(Npc(4242, 11, 10, 2, 7));
        h.ObserveNatives(t, natives);
        h.Expire(t);
        for (const auto& c : h.TakeCmds()) {
            if (c.kind == MirrorCmd::Kind::State && copies == 0) ++mirrorsSeen, ++copies;  // the host spawns its copy
            if (c.kind == MirrorCmd::Kind::End) copies = 0;                                 // ... and drops it
        }
        for (const auto& c : h.TakeClaims()) {
            claimSeen = true;
            if (g.Claim(c.guestKey, c.hostKey)) pairedHost = c.hostKey;  // the Lua side pairs them
        }
        if (t > 6 && t < 9) oneCopyWhileAlone &= copies == 1;  // while only the guest's game has it
    }
    Check(mirrorsSeen == 1 && oneCopyWhileAlone, "the guest's own enemy was shown on the host, once");
    Check(claimSeen && pairedHost == 4242 && handedBack, "the host's late spawn claimed it, the guest handed it back");
    Check(copies == 0 && h.Count() == 0 && !g.Owned(42), "after the hand-back: no copy, no mirror, the host's (no doubles)");
}

}  // namespace

int main() {
    TestCodec();
    TestGuestWindow();
    TestGuestCaps();
    TestGuestCopies();
    TestGuestEnds();
    TestGuestClaim();
    TestHostMirrors();
    TestHostClaims();
    TestHostDead();
    TestRoundTrip();
    std::printf("%s: %d failure(s)\n", g_failures ? "FAILED" : "OK", g_failures);
    return g_failures ? 1 : 0;
}
