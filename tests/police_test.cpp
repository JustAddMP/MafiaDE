// police_test: unit tests for the police sync model (src/game/police_model.h): the wire format of PoliceState,
// PoliceEvent and PoliceChase, the Lua state line, when our own state is sent, the host's targets (a wanted guest near
// the host, hysteresis, release hold, offences announced, chase messages) and the guest's answer to a chase (wanted
// level held, own zone started, released) and the guest's zone decay held while the host's police are on him (released
// when the chase ends; a zone started again when it decays or decayed while they are still on him). Prints PASS/FAIL per check; exits non-zero on any failure.
#include "game/police_model.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

using namespace mc;
using namespace mc::game::police;

namespace {

int g_failures = 0;

void Check(bool ok, const char* name, const std::string& detail = {}) {
    std::printf("%s  %-60s %s\n", ok ? "PASS" : "FAIL", name, detail.c_str());
    std::fflush(stdout);
    if (!ok) ++g_failures;
}

WantedState Wanted(uint8_t level, bool hunt, float x = 0, float y = 0) {
    WantedState s;
    s.flags = kInWorld | kWanted | (hunt ? kHunt : 0);
    s.level = level;
    s.zone = {x, y, 5};
    return s;
}

WantedState Clean() {
    WantedState s;
    s.flags = kInWorld;
    return s;
}

int Count(const std::vector<HostCmd>& v, HostCmd::Type t, uint8_t id) {
    int n = 0;
    for (const auto& c : v)
        if (c.type == t && c.id == id) ++n;
    return n;
}

const HostCmd* FindCmd(const std::vector<HostCmd>& v, HostCmd::Type t, uint8_t id) {
    for (const auto& c : v)
        if (c.type == t && c.id == id) return &c;
    return nullptr;
}

int CountG(const std::vector<GuestCmd>& v, GuestCmd::Type t) {
    int n = 0;
    for (const auto& c : v)
        if (c.type == t) ++n;
    return n;
}

const GuestCmd* FindG(const std::vector<GuestCmd>& v, GuestCmd::Type t) {
    for (const auto& c : v)
        if (c.type == t) return &c;
    return nullptr;
}

// no ForceMin and no Zone (Hold refreshes may be there)
bool NoLevelOrZone(const std::vector<GuestCmd>& v) {
    return CountG(v, GuestCmd::Type::ForceMin) == 0 && CountG(v, GuestCmd::Type::Zone) == 0;
}

WantedState Decaying(WantedState s) {
    s.flags |= kDecaying;
    return s;
}

Chase OnUs(uint8_t level, bool hunt, uint8_t cops, uint8_t seers, float nearest) {
    Chase c;
    c.active = true;
    c.hunt = hunt;
    c.level = level;
    c.cops = cops;
    c.seers = seers;
    c.nearest = nearest;
    return c;
}

void TestWire() {
    WantedState s = Wanted(3, true, 123.5f, -77.25f);
    s.flags |= kSeen | kDecaying;
    net::ByteWriter w;
    WriteState(w, s, 4242);
    Check(w.Size() == kStateBytes, "state: size", std::to_string(w.Size()));
    net::ByteReader r(w.Span());
    WantedState t;
    uint16_t seq = 0;
    const bool ok = ReadState(r, t, seq);
    Check(ok && seq == 4242 && t.flags == s.flags && t.level == 3 && t.zone.x == 123.5f && t.zone.y == -77.25f,
          "state: round trip");
    Check(t.Wanted() && t.Hunt(), "state: wanted + hunt");

    // a later version with more bytes is still read
    net::ByteWriter w2;
    WriteState(w2, s, 1);
    w2.U32(0xDEADBEEF);
    net::ByteReader r2(w2.Span());
    Check(ReadState(r2, t, seq) && seq == 1, "state: trailing bytes ignored");

    // truncated / bad
    net::ByteReader r3(w.Data(), w.Size() - 1);
    Check(!ReadState(r3, t, seq), "state: truncated rejected");
    net::ByteWriter w4;
    w4.U8(0);  // version 0
    w4.U16(1);
    w4.U8(1);
    w4.U8(1);
    w4.F32(0);
    w4.F32(0);
    w4.F32(0);
    net::ByteReader r4(w4.Span());
    Check(!ReadState(r4, t, seq), "state: version 0 rejected");
    net::ByteWriter w5;
    w5.U8(1);
    w5.U16(1);
    w5.U8(1);
    w5.U8(200);  // level out of range
    w5.F32(0);
    w5.F32(0);
    w5.F32(0);
    net::ByteReader r5(w5.Span());
    Check(!ReadState(r5, t, seq), "state: level out of range rejected");
    net::ByteWriter w6;
    w6.U8(1);
    w6.U16(1);
    w6.U8(1);
    w6.U8(1);
    w6.F32(NAN);
    w6.F32(0);
    w6.F32(0);
    net::ByteReader r6(w6.Span());
    Check(!ReadState(r6, t, seq), "state: NaN zone rejected");

    // events
    net::ByteWriter we;
    WriteEvent(we, {Event::Arrested, 7});
    net::ByteReader re(we.Span());
    EventMsg e;
    Check(ReadEvent(re, e) && e.kind == Event::Arrested && e.arg == 7, "event: round trip");
    net::ByteWriter we2;
    we2.U8(static_cast<uint8_t>(Event::Last));
    we2.U32(0);
    net::ByteReader re2(we2.Span());
    Check(!ReadEvent(re2, e), "event: unknown kind rejected");
    net::ByteWriter we3;
    we3.U8(0);
    we3.U32(0);
    net::ByteReader re3(we3.Span());
    Check(!ReadEvent(re3, e), "event: kind 0 rejected");

    // names
    bool names = true;
    for (int i = 1; i < static_cast<int>(Event::Last); ++i)
        if (EventFromName(EventName(static_cast<Event>(i))) != static_cast<Event>(i)) names = false;
    Check(names, "event: every name maps back");
    Check(EventFromName("PLAYER_AREST") == Event::PlayerArrest && EventFromName("nope") == Event::None,
          "event: game spelling / unknown");
    Check(IsOffenceEvent(Event::Offender) && IsOffenceEvent(Event::EnterHunt) && !IsOffenceEvent(Event::Arrested) &&
              !IsOffenceEvent(Event::LostPolice),
          "event: offence events");

    // chase
    Chase c;
    c.active = true;
    c.hunt = true;
    c.level = 2;
    c.cops = 5;
    c.seers = 2;
    c.nearest = 31.5f;
    net::ByteWriter wc;
    WriteChase(wc, c);
    net::ByteReader rc(wc.Span());
    Chase d;
    Check(ReadChase(rc, d) && d.active && d.hunt && d.level == 2 && d.cops == 5 && d.seers == 2 && d.nearest == 31.5f,
          "chase: round trip");
    net::ByteReader rc2(wc.Data(), 3);
    Check(!ReadChase(rc2, d), "chase: truncated rejected");
}

void TestLine() {
    WantedState s;
    Check(ParseStateLine("131 2 10.50 -20.25 3.00", s) && s.flags == 131 && s.level == 2 && s.zone.y == -20.25f,
          "line: parsed");
    Check(!ParseStateLine("1 2 3", s), "line: too short rejected");
    Check(!ParseStateLine("300 1 0 0 0", s), "line: flags out of range rejected");
    Check(!ParseStateLine("1 1 1e9 0 0", s), "line: coordinate out of range rejected");
    Check(!ParseStateLine(nullptr, s), "line: null rejected");
    Check(ParseStateLine("129 99 0 0 0", s) && s.level == kMaxLevel, "line: level clamped");
}

void TestSame() {
    Check(SameState(Clean(), Clean()), "same: clean == clean");
    WantedState a = Wanted(1, false, 0, 0), b = Wanted(1, false, 5, 0), c = Wanted(1, false, 15, 0);
    Check(SameState(a, b), "same: zone moved 5 m");
    Check(!SameState(a, c), "same: zone moved 15 m");
    Check(!SameState(a, Wanted(2, false)), "same: level differs");
    Check(!SameState(a, Wanted(1, true)), "same: type differs");
    WantedState cz = Clean(), cz2 = Clean();
    cz2.zone = {500, 0, 0};
    Check(SameState(cz, cz2), "same: clean ignores the zone centre");
    Check(Transition(Clean(), a) == Event::BecameWanted && Transition(a, Clean()) == Event::LostPolice &&
              Transition(a, c) == Event::None,
          "transition: wanted / lost / none");
    WantedState out;  // not in a world
    Check(Transition(out, a) == Event::None && Transition(a, out) == Event::None, "transition: not across worlds");
    Check(Describe(a).find("wanted search level 1") == 0, "describe", Describe(a));
}

void TestSender() {
    Sender s;
    bool rel = false;
    double t = 100;
    Check(s.Due(t, Clean(), rel) && rel, "sender: first state at once (reliable)");
    Check(!s.Due(t + 0.05, Clean(), rel), "sender: unchanged not again soon");
    Check(!s.Due(t + 0.1, Wanted(1, false), rel), "sender: a change within the min gap waits");
    Check(s.Due(t + 0.25, Wanted(1, false), rel) && rel, "sender: the change after the gap (reliable)");
    const uint16_t seq = s.Seq();
    Check(!s.Due(t + 0.9, Wanted(1, false), rel), "sender: wanted heartbeat not before 1 s");
    Check(s.Due(t + 1.3, Wanted(1, false), rel) && !rel && s.Seq() == seq + 1, "sender: wanted heartbeat (unreliable)");
    Check(s.Due(t + 1.6, Clean(), rel) && rel, "sender: clean again (reliable)");
    Check(!s.Due(t + 4.0, Clean(), rel), "sender: clean heartbeat not before 3 s");
    Check(s.Due(t + 4.7, Clean(), rel) && !rel, "sender: clean heartbeat");
    s.Reset();
    Check(s.Due(t + 4.71, Clean(), rel) && rel, "sender: after a reset at once");
}

void TestHostTargets() {
    HostPolice h;
    std::vector<HostCmd> cmds;
    std::vector<ChaseOut> ch;
    double t = 10;
    h.SetHost({0, 0, 0});
    // a clean guest near us: no target
    h.OnState(1, Clean(), t);
    h.SetPuppet(1, {50, 0, 0}, t);
    h.Tick(t, cmds, ch);
    Check(cmds.empty() && ch.empty(), "host: clean guest, nothing");
    // he becomes wanted (search): target + one announce
    h.OnState(1, Wanted(1, false), t + 0.1);
    cmds.clear();
    h.Tick(t + 0.1, cmds, ch);
    Check(Count(cmds, HostCmd::Type::Target, 1) == 1 && Count(cmds, HostCmd::Type::Announce, 1) == 1,
          "host: wanted near us -> target + announce");
    const HostCmd* tc = FindCmd(cmds, HostCmd::Type::Target, 1);
    Check(tc && tc->level == 1 && !tc->hunt, "host: target carries level / search");
    cmds.clear();
    h.Tick(t + 0.2, cmds, ch);
    Check(Count(cmds, HostCmd::Type::Target, 1) == 1 && Count(cmds, HostCmd::Type::Announce, 1) == 0,
          "host: target every tick, announce once");
    // escalation to hunt: announced again (after 1 s)
    h.OnState(1, Wanted(1, true), t + 0.5);
    cmds.clear();
    h.Tick(t + 0.5, cmds, ch);
    Check(Count(cmds, HostCmd::Type::Announce, 1) == 0, "host: escalation within 1 s waits");
    cmds.clear();
    h.Tick(t + 1.2, cmds, ch);
    const HostCmd* an = FindCmd(cmds, HostCmd::Type::Announce, 1);
    Check(an && an->hunt, "host: escalation to hunt announced");
    cmds.clear();
    h.Tick(t + 1.3, cmds, ch);
    Check(Count(cmds, HostCmd::Type::Announce, 1) == 0, "host: no repeat without a new offence");
    // an offence event from his game: announced, but not more than every kAnnounceGap
    h.OnEvent(1, {Event::Offender, 0}, t + 2);
    h.SetPuppet(1, {50, 0, 0}, t + 2);
    h.OnState(1, Wanted(1, true), t + 2);
    cmds.clear();
    h.Tick(t + 2, cmds, ch);
    Check(Count(cmds, HostCmd::Type::Announce, 1) == 0, "host: offence within the gap waits");
    h.SetPuppet(1, {50, 0, 0}, t + 4);
    h.SetPuppet(1, {50, 0, 0}, t + 6);
    h.OnState(1, Wanted(1, true), t + 6);
    cmds.clear();
    h.Tick(t + 1.2 + kAnnounceGap + 0.01, cmds, ch);
    Check(Count(cmds, HostCmd::Type::Announce, 1) == 1, "host: offence announced after the gap");
    h.OnEvent(1, {Event::Arrested, 1}, t + 7);
    h.SetPuppet(1, {50, 0, 0}, t + 13);
    h.OnState(1, Wanted(1, true), t + 13);
    cmds.clear();
    h.Tick(t + 13, cmds, ch);
    Check(Count(cmds, HostCmd::Type::Target, 1) == 1, "host: still a target");
    Check(Count(cmds, HostCmd::Type::Announce, 1) == 0, "host: a non-offence event announces nothing");
}

void TestHostRelease() {
    HostPolice h;
    std::vector<HostCmd> cmds;
    std::vector<ChaseOut> ch;
    double t = 0;
    h.SetHost({0, 0, 0});
    h.OnState(2, Wanted(2, true), t);
    h.SetPuppet(2, {100, 0, 0}, t);
    h.Tick(t, cmds, ch);
    Check(Count(cmds, HostCmd::Type::Target, 2) == 1, "release: target");
    // clean: kept for kReleaseHold
    h.OnState(2, Clean(), t + 1);
    h.SetPuppet(2, {100, 0, 0}, t + 1);
    cmds.clear();
    h.Tick(t + 1, cmds, ch);
    Check(Count(cmds, HostCmd::Type::Target, 2) == 1, "release: clean but held");
    h.SetPuppet(2, {100, 0, 0}, t + 3);
    cmds.clear();
    h.Tick(t + 1 + kReleaseHold - 0.1, cmds, ch);
    Check(Count(cmds, HostCmd::Type::Target, 2) == 1, "release: still held just before the hold");
    cmds.clear();
    h.Tick(t + 1 + kReleaseHold + 0.01, cmds, ch);
    Check(Count(cmds, HostCmd::Type::Target, 2) == 0, "release: released after the hold");
    // wanted again but far: no target; hysteresis
    h.OnState(2, Wanted(1, false), t + 5);
    h.SetPuppet(2, {kNearLeave + 50, 0, 0}, t + 4.9);
    cmds.clear();
    h.Tick(t + 4.9, cmds, ch);
    Check(Count(cmds, HostCmd::Type::Target, 2) == 0, "near: far away: his own police");
    h.SetPuppet(2, {kNearEnter + 20, 0, 0}, t + 5);
    cmds.clear();
    h.Tick(t + 5, cmds, ch);
    Check(Count(cmds, HostCmd::Type::Target, 2) == 0, "near: beyond the enter range: his own police");
    h.SetPuppet(2, {kNearEnter - 10, 0, 0}, t + 5.1);
    cmds.clear();
    h.Tick(t + 5.1, cmds, ch);
    Check(Count(cmds, HostCmd::Type::Target, 2) == 1, "near: within the enter range: ours");
    h.SetPuppet(2, {(kNearEnter + kNearLeave) / 2, 0, 0}, t + 5.2);
    cmds.clear();
    h.Tick(t + 5.2, cmds, ch);
    Check(Count(cmds, HostCmd::Type::Target, 2) == 1, "near: between enter and leave: kept");
    h.SetPuppet(2, {kNearLeave + 1, 0, 0}, t + 5.3);
    cmds.clear();
    h.Tick(t + 5.3, cmds, ch);
    Check(Count(cmds, HostCmd::Type::Target, 2) == 0, "near: beyond leave: released at once");
    // stale state / stale puppet
    h.SetPuppet(2, {10, 0, 0}, t + 6);
    h.OnState(2, Wanted(1, false), t + 6);
    cmds.clear();
    h.Tick(t + 6, cmds, ch);
    Check(Count(cmds, HostCmd::Type::Target, 2) == 1, "stale: target again");
    h.SetPuppet(2, {10, 0, 0}, t + 6 + kStateStale + 0.5);
    cmds.clear();
    h.Tick(t + 6 + kStateStale + 0.5, cmds, ch);
    Check(Count(cmds, HostCmd::Type::Target, 2) == 0, "stale: no state for kStateStale: released");
    h.OnState(2, Wanted(1, false), t + 20);
    cmds.clear();
    h.Tick(t + 20 + kPuppetStale + 0.5, cmds, ch);
    Check(Count(cmds, HostCmd::Type::Target, 2) == 0, "stale: no puppet position: no target");
    // not in a world: not a target
    h.SetPuppet(2, {10, 0, 0}, t + 30);
    WantedState nw = Wanted(1, false);
    nw.flags &= static_cast<uint8_t>(~kInWorld);
    h.OnState(2, nw, t + 30);
    cmds.clear();
    h.Tick(t + 30, cmds, ch);
    Check(Count(cmds, HostCmd::Type::Target, 2) == 0, "world: a guest outside a world is no target");
    // id 0 (the host) is never a guest
    h.OnState(0, Wanted(1, false), t + 31);
    h.SetPuppet(0, {1, 0, 0}, t + 31);
    Check(h.Find(0) == nullptr, "host id 0 ignored");
    h.RemovePeer(2);
    Check(h.Find(2) == nullptr, "peer removed");
}

void TestHostChase() {
    HostPolice h;
    std::vector<HostCmd> cmds;
    std::vector<ChaseOut> ch;
    double t = 0;
    h.SetHost({0, 0, 0});
    h.OnState(1, Wanted(2, true), t);
    h.SetPuppet(1, {30, 0, 0}, t);
    h.Tick(t, cmds, ch);
    Check(ch.empty(), "chase: a target without police near: no chase");
    h.SetReport(1, 3, 5, 12.5f, t + 0.1);  // seers above cops: clamped
    ch.clear();
    h.Tick(t + 0.1, cmds, ch);
    Check(ch.size() == 1 && ch[0].reliable && ch[0].chase.active && ch[0].chase.cops == 3 && ch[0].chase.seers == 3 &&
              ch[0].chase.hunt && ch[0].chase.level == 2,
          "chase: starts (reliable)");
    ch.clear();
    h.Tick(t + 0.3, cmds, ch);
    Check(ch.empty(), "chase: not again within the interval");
    ch.clear();
    h.Tick(t + 0.61, cmds, ch);
    Check(ch.size() == 1 && !ch[0].reliable && ch[0].chase.active, "chase: update (unreliable)");
    // the report goes stale: chase over (reliable end)
    h.SetPuppet(1, {30, 0, 0}, t + 3);
    h.OnState(1, Wanted(2, true), t + 3);
    ch.clear();
    h.Tick(t + 0.1 + kReportStale + 0.1, cmds, ch);
    Check(ch.size() == 1 && ch[0].reliable && !ch[0].chase.active, "chase: ends when no police are reported");
    ch.clear();
    h.Tick(t + 3.5, cmds, ch);
    Check(ch.empty(), "chase: ended stays quiet");
    // police again, then the target ends
    h.SetReport(1, 1, 0, 40, t + 3.6);
    ch.clear();
    h.Tick(t + 3.6, cmds, ch);
    Check(ch.size() == 1 && ch[0].chase.active && ch[0].chase.seers == 0, "chase: again");
    h.OnState(1, Clean(), t + 3.7);
    h.SetPuppet(1, {30, 0, 0}, t + 3.7);
    h.SetReport(1, 1, 0, 40, t + 3.7 + kReleaseHold);
    h.SetPuppet(1, {30, 0, 0}, t + 3.7 + kReleaseHold);
    ch.clear();
    h.Tick(t + 3.7, cmds, ch);
    h.Tick(t + 3.71 + kReleaseHold, cmds, ch);
    bool ended = !ch.empty() && !ch.back().chase.active && ch.back().reliable;
    Check(ended, "chase: ends with the target");
}

void TestGuest() {
    GuestPolice g;
    std::vector<GuestCmd> out;
    double t = 50;
    g.SetLocal(Clean());
    g.Tick(t, out);
    Check(out.empty() && g.Forced() == 0 && !g.Held(), "guest: nothing without a chase");
    Chase c;
    c.active = true;
    c.hunt = true;
    c.level = 2;
    c.cops = 2;
    c.seers = 0;
    g.OnChase(c, t);
    g.Tick(t, out);
    {
        const GuestCmd* f = FindG(out, GuestCmd::Type::ForceMin);
        Check(f && f->level == 2 && g.Forced() == 2 && CountG(out, GuestCmd::Type::Zone) == 0,
              "guest: chase -> level held");
    }
    out.clear();
    g.Tick(t + 0.1, out);
    Check(out.empty(), "guest: held once");
    // they see us and our game has no zone: one zone, then not again before kZoneRetry
    c.seers = 1;
    g.OnChase(c, t + 0.5);
    out.clear();
    g.Tick(t + 0.5, out);
    {
        const GuestCmd* z = FindG(out, GuestCmd::Type::Zone);
        Check(CountG(out, GuestCmd::Type::Zone) == 1 && z->hunt && z->level == 2 && z->why == ZoneWhy::Seen &&
                  CountG(out, GuestCmd::Type::ForceMin) == 0,
              "guest: seen, no zone -> zone");
    }
    out.clear();
    g.OnChase(c, t + 1);
    g.Tick(t + 1, out);
    Check(CountG(out, GuestCmd::Type::Zone) == 0, "guest: zone not again soon");
    g.SetLocal(Wanted(2, true));
    g.OnChase(c, t + 1 + kZoneRetry);
    out.clear();
    g.Tick(t + 1 + kZoneRetry, out);
    Check(NoLevelOrZone(out), "guest: our game has a zone: none started");
    // level up
    c.level = 4;
    g.OnChase(c, t + 7);
    out.clear();
    g.Tick(t + 7, out);
    {
        const GuestCmd* f = FindG(out, GuestCmd::Type::ForceMin);
        Check(f && f->level == 4 && CountG(out, GuestCmd::Type::ForceMin) == 1, "guest: level up");
    }
    // the chase ends: held kForceHold, then released
    Chase off;
    g.OnChase(off, t + 8);
    out.clear();
    g.Tick(t + 8, out);
    Check(NoLevelOrZone(out) && g.Forced() == 4, "guest: ended, still held");
    out.clear();
    g.Tick(t + 7 + kForceHold + 0.1, out);
    {
        const GuestCmd* f = FindG(out, GuestCmd::Type::ForceMin);
        Check(f && f->level == 0 && g.Forced() == 0, "guest: released after the hold");
    }
    // the host goes silent mid-chase: stale -> released after the hold
    g.SetLocal(Clean());
    c.level = 1;
    c.seers = 0;
    g.OnChase(c, t + 20);
    out.clear();
    g.Tick(t + 20, out);
    Check(g.Forced() == 1 && CountG(out, GuestCmd::Type::Zone) == 0, "guest: chased again (clean, not seen: no zone)");
    out.clear();
    g.Tick(t + 20 + kChaseStale + 0.1, out);
    Check(NoLevelOrZone(out) && !g.Chased(t + 20 + kChaseStale + 0.1), "guest: stale chase, level still held");
    out.clear();
    g.Tick(t + 20 + kChaseStale + kForceHold + 0.2, out);
    {
        const GuestCmd* f = FindG(out, GuestCmd::Type::ForceMin);
        Check(f && f->level == 0, "guest: stale chase released");
    }
    // session end while held
    g.OnChase(c, t + 40);
    out.clear();
    g.Tick(t + 40, out);
    out.clear();
    g.Reset(out);
    {
        const GuestCmd* f = FindG(out, GuestCmd::Type::ForceMin);
        const GuestCmd* h = FindG(out, GuestCmd::Type::Hold);
        Check(f && f->level == 0 && h && !h->hold && !h->follow && g.Forced() == 0 && !g.Held() &&
                  !g.Chased(t + 40),
              "guest: reset releases (level and decay)");
    }
    out.clear();
    g.Reset(out);
    Check(out.empty(), "guest: reset with nothing held is quiet");
    // level 0 in a chase is held at 1 at least; not in a world: no zone
    Chase z;
    z.active = true;
    z.level = 0;
    z.seers = 1;
    z.cops = 1;
    WantedState nw;  // not in a world
    g.SetLocal(nw);
    g.OnChase(z, t + 60);
    out.clear();
    g.Tick(t + 60, out);
    {
        const GuestCmd* f = FindG(out, GuestCmd::Type::ForceMin);
        Check(f && f->level == 1 && CountG(out, GuestCmd::Type::Zone) == 0,
              "guest: level at least 1, no zone outside a world");
    }
}

// The bug of the 0.15 test: the guest's own zone decayed in ~20 s while the host's police chased his puppet. Now his
// zone's decay is held while the chase goes on and released when it ends.
void TestGuestDecayHold() {
    GuestPolice g;
    std::vector<GuestCmd> out;
    double t = 100;
    g.SetLocal(Wanted(2, false));
    g.Tick(t, out);
    Check(out.empty() && !g.Held(), "decay: wanted on our own, no chase: our game decides");
    // the host's police are near our puppet (2 cops, none saw it, 60 m): held, not following
    g.OnChase(OnUs(2, false, 2, 0, 60), t + 1);
    out.clear();
    g.Tick(t + 1, out);
    {
        const GuestCmd* h = FindG(out, GuestCmd::Type::Hold);
        Check(h && h->hold && !h->follow && !h->refresh && g.Held() && g.Holds() == 1, "decay: chase -> held");
    }
    out.clear();
    g.OnChase(OnUs(2, false, 2, 0, 60), t + 1.5);
    g.Tick(t + 1.5, out);
    Check(CountG(out, GuestCmd::Type::Hold) == 0, "decay: held once (no repeat within kHoldRefresh)");
    // 25 s of chase (the old decay time): the hold stays, set again every kHoldRefresh (quiet), never released
    int refreshes = 0, releases = 0;
    for (double s = 2; s <= 26; s += 0.5) {
        g.OnChase(OnUs(2, false, 2, 0, 60), t + s);
        out.clear();
        g.Tick(t + s, out);
        for (const auto& c : out)
            if (c.type == GuestCmd::Type::Hold) {
                if (c.hold && c.refresh) ++refreshes;
                if (!c.hold) ++releases;
            }
    }
    Check(g.Held() && releases == 0 && refreshes >= 10 && refreshes <= 13,
          "decay: held for the whole chase, refreshed quietly", std::to_string(refreshes) + " refreshes");
    // a hunt: a cop sees us -> the red zone follows us; he loses sight but one is close (30 m) -> still follows;
    // only far ones (70 m) -> held, not following
    g.SetLocal(Wanted(3, true));
    g.OnChase(OnUs(3, true, 3, 1, 70), t + 27);
    out.clear();
    g.Tick(t + 27, out);
    {
        const GuestCmd* h = FindG(out, GuestCmd::Type::Hold);
        Check(h && h->hold && h->follow && !h->refresh && g.Following(), "decay: hunt, seen -> zone follows us");
    }
    g.OnChase(OnUs(3, true, 3, 0, 30), t + 27.5);
    out.clear();
    g.Tick(t + 27.5, out);
    Check(CountG(out, GuestCmd::Type::Hold) == 0 && g.Following(), "decay: hunt, a cop close -> still follows");
    g.OnChase(OnUs(3, true, 3, 0, 70), t + 28);
    out.clear();
    g.Tick(t + 28, out);
    {
        const GuestCmd* h = FindG(out, GuestCmd::Type::Hold);
        Check(h && h->hold && !h->follow && !h->refresh && !g.Following(), "decay: hunt, cops far -> held, no follow");
    }
    // a search (blue) zone of ours never follows, even when seen
    g.SetLocal(Wanted(3, false));
    g.OnChase(OnUs(3, true, 3, 2, 10), t + 29);
    out.clear();
    g.Tick(t + 29, out);
    Check(!g.Following() && g.Held(), "decay: our zone is blue -> no follow");
    // the chase ends (no cops near: the host sends inactive): held kForceHold more, then released once
    g.OnChase(Chase{}, t + 30);
    out.clear();
    g.Tick(t + 30, out);
    {
        const GuestCmd* h = FindG(out, GuestCmd::Type::Hold);
        Check(g.Held() && (!h || (h->hold && h->refresh)), "decay: chase ended, held a moment longer");
    }
    out.clear();
    g.Tick(t + 29 + kForceHold + 0.1, out);
    {
        const GuestCmd* h = FindG(out, GuestCmd::Type::Hold);
        Check(h && !h->hold && !h->follow && !h->refresh && !g.Held(), "decay: released when the chase really ended");
    }
    out.clear();
    g.Tick(t + 40, out);
    Check(out.empty(), "decay: released once, then quiet (our game decays as always)");
    // the host goes silent mid-chase: released after kChaseStale + kForceHold
    g.OnChase(OnUs(2, false, 1, 0, 50), t + 50);
    out.clear();
    g.Tick(t + 50, out);
    Check(g.Held(), "decay: chased again -> held");
    out.clear();
    g.Tick(t + 50 + kChaseStale + 0.1, out);
    Check(g.Held(), "decay: stale chase, still held");
    out.clear();
    g.Tick(t + 50 + kChaseStale + kForceHold + 0.2, out);
    Check(!g.Held() && FindG(out, GuestCmd::Type::Hold) && !FindG(out, GuestCmd::Type::Hold)->hold,
          "decay: stale chase released");
}

// Our zone decays or decayed away while the host's police are still on us: a fresh one here (once per decay; never
// after a fine, an arrest or our death, never when offences are off, never without host cops near us).
void TestGuestRefresh() {
    std::vector<GuestCmd> out;
    double t = 200;
    {
        GuestPolice g;
        g.SetLocal(Decaying(Wanted(2, false)));
        g.OnChase(OnUs(2, false, 2, 0, 50), t);
        g.Tick(t, out);
        const GuestCmd* z = FindG(out, GuestCmd::Type::Zone);
        Check(z && z->why == ZoneWhy::Refresh && z->level == 2 && !z->hunt, "refresh: decaying while chased -> zone");
        out.clear();
        g.OnChase(OnUs(2, false, 2, 0, 50), t + kZoneRetry + 1);
        g.Tick(t + kZoneRetry + 1, out);
        Check(CountG(out, GuestCmd::Type::Zone) == 0, "refresh: once per decay");
        // the decay ended (fresh zone), then decays again: one more
        g.SetLocal(Wanted(3, true));
        g.OnChase(OnUs(2, false, 2, 0, 50), t + 13);
        out.clear();
        g.Tick(t + 13, out);
        g.SetLocal(Decaying(Wanted(3, true)));
        g.OnChase(OnUs(2, false, 2, 0, 50), t + 14);
        out.clear();
        g.Tick(t + 14, out);
        const GuestCmd* z2 = FindG(out, GuestCmd::Type::Zone);
        Check(z2 && z2->why == ZoneWhy::Refresh && z2->level == 3 && z2->hunt,
              "refresh: a new decay -> again (our level and hunt kept)");
    }
    {
        // decaying, but no host cop near our puppet: our game decides (no chase message is active without cops; a
        // chase with cops 0 does not refresh either)
        GuestPolice g;
        g.SetLocal(Decaying(Wanted(2, false)));
        g.OnChase(OnUs(2, false, 0, 0, 0), t);
        out.clear();
        g.Tick(t, out);
        Check(CountG(out, GuestCmd::Type::Zone) == 0, "refresh: no cops near -> none");
        g.OnChase(Chase{}, t + 1);
        out.clear();
        g.Tick(t + 10, out);
        Check(CountG(out, GuestCmd::Type::Zone) == 0, "refresh: no chase -> none");
    }
    {
        // offences off (a mission) or a fine going on: none
        GuestPolice g;
        WantedState s = Decaying(Wanted(2, false));
        s.flags |= kOffencesOff;
        g.SetLocal(s);
        g.OnChase(OnUs(2, false, 2, 0, 50), t);
        out.clear();
        g.Tick(t, out);
        Check(CountG(out, GuestCmd::Type::Zone) == 0, "refresh: offences off -> none");
        s = Decaying(Wanted(2, false));
        s.flags |= kFining;
        g.SetLocal(s);
        g.OnChase(OnUs(2, false, 2, 0, 50), t + 6);
        out.clear();
        g.Tick(t + 6, out);
        Check(CountG(out, GuestCmd::Type::Zone) == 0, "refresh: being fined -> none");
    }
    {
        // the zone decayed away (clean now) while they are still on us: started again at our level
        GuestPolice g;
        g.SetLocal(Wanted(3, true));
        g.OnChase(OnUs(2, true, 2, 0, 60), t);
        out.clear();
        g.Tick(t, out);
        g.SetLocal(Clean());
        g.OnChase(OnUs(2, true, 2, 0, 60), t + 1);
        out.clear();
        g.Tick(t + 1, out);
        const GuestCmd* z = FindG(out, GuestCmd::Type::Zone);
        Check(z && z->why == ZoneWhy::Revive && z->level == 3 && z->hunt, "revive: decayed while chased -> zone again");
        out.clear();
        g.OnChase(OnUs(2, true, 2, 0, 60), t + 2);
        g.Tick(t + 2, out);
        Check(CountG(out, GuestCmd::Type::Zone) == 0, "revive: not again before kZoneRetry");
    }
    {
        // clean because we were fined / arrested / died: never started again
        const Event ends[] = {Event::FineFinished, Event::Arrested, Event::PlayerDeath, Event::HumanArrested};
        for (Event e : ends) {
            GuestPolice g;
            g.SetLocal(Wanted(2, false));
            g.OnChase(OnUs(2, false, 2, 0, 60), t);
            out.clear();
            g.Tick(t, out);
            g.OnOwnEvent(e, t + 0.5);
            g.SetLocal(Clean());
            g.OnChase(OnUs(2, false, 2, 0, 60), t + 1);
            out.clear();
            g.Tick(t + 1, out);
            Check(CountG(out, GuestCmd::Type::Zone) == 0, (std::string("revive: not after ") + EventName(e)).c_str());
        }
        // the last wanted state said "being arrested": not either
        GuestPolice g;
        WantedState s = Wanted(2, false);
        s.flags |= kArresting;
        g.SetLocal(s);
        g.OnChase(OnUs(2, false, 2, 0, 60), t);
        out.clear();
        g.Tick(t, out);
        g.SetLocal(Clean());
        g.OnChase(OnUs(2, false, 2, 0, 60), t + 1);
        out.clear();
        g.Tick(t + 1, out);
        Check(CountG(out, GuestCmd::Type::Zone) == 0, "revive: not when the last state was an arrest");
    }
    {
        // clean through this whole chase (never wanted in it), not seen: no zone (we did nothing on our game)
        GuestPolice g;
        g.SetLocal(Clean());
        g.OnChase(OnUs(2, false, 3, 0, 20), t);
        out.clear();
        g.Tick(t, out);
        Check(CountG(out, GuestCmd::Type::Zone) == 0 && g.Held(), "revive: never wanted in this chase -> none");
        // a previous chase's wanted state does not count once that chase is over
        GuestPolice h;
        h.SetLocal(Wanted(2, false));
        h.OnChase(OnUs(2, false, 2, 0, 60), t);
        h.Tick(t, out);
        h.OnChase(Chase{}, t + 1);
        h.Tick(t + 1 + kForceHold + 0.1, out);  // the chase is fully over
        h.SetLocal(Clean());
        h.OnChase(OnUs(2, false, 2, 0, 60), t + 30);
        out.clear();
        h.Tick(t + 30, out);
        Check(CountG(out, GuestCmd::Type::Zone) == 0, "revive: an old chase's wanted state does not count");
    }
    Check(std::string(ZoneWhyName(ZoneWhy::Seen)) == "seen" && std::string(ZoneWhyName(ZoneWhy::Refresh)) == "refresh" &&
              std::string(ZoneWhyName(ZoneWhy::Revive)) == "revive",
          "zone why names");
}

}  // namespace

int main() {
    TestWire();
    TestLine();
    TestSame();
    TestSender();
    TestHostTargets();
    TestHostRelease();
    TestHostChase();
    TestGuest();
    TestGuestDecayHold();
    TestGuestRefresh();
    std::printf("%s (%d failure%s)\n", g_failures ? "FAILED" : "ALL PASSED", g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
