// door_test: unit tests for doors on every game (src/game/door_model.h): the wire format of DoorEvent / DoorApply /
// DoorSnapshot / DoorSnapRequest, the identity of a door across games (name + position), the door table (seen,
// gone, changes, echoes of what we applied), remembered changes for doors that stream in later, the host's
// authority (locked doors, the guests' own scripts), snapshots, and a host + two guests walk-through.
// Prints PASS/FAIL per check; exits non-zero on any failure.
#include <cmath>
#include <map>
#include <cstdio>
#include <string>
#include <vector>

#include "game/door_model.h"

using namespace mc::game::doors;
using mc::net::ByteReader;
using mc::net::ByteWriter;

namespace {

int g_failures = 0;

void Check(bool ok, const char* name, const std::string& detail = {}) {
    std::printf("%s  %-64s %s\n", ok ? "PASS" : "FAIL", name, detail.c_str());
    std::fflush(stdout);
    if (!ok) ++g_failures;
}

Vec3 P(float x, float y, float z) {
    Vec3 v;
    v.x = x;
    v.y = y;
    v.z = z;
    return v;
}

constexpr uint8_t kClosedLocked = kLocked;
constexpr uint8_t kClosedUnlocked = 0;
constexpr uint8_t kOpenUnlocked = kOpen;

void TestWire() {
    Event e;
    e.seq = 513;
    e.by = 2;
    e.act = Act::Kick;
    e.state = kOpenUnlocked;
    e.door.name = NameHash("door_front_safehouse_01");
    e.door.pos = P(-1203.25f, 455.5f, 3.75f);
    e.from = P(-1204.0f, 456.0f, 3.7f);
    ByteWriter w;
    WriteEvent(w, e);
    Check(w.Span().size() == 33, "DoorEvent body is 33 bytes", std::to_string(w.Span().size()));
    ByteReader r(w.Span());
    Event b;
    const bool ok = ReadEvent(r, b);
    Check(ok && b.seq == 513 && b.by == 2 && b.act == Act::Kick && b.state == kOpenUnlocked &&
              b.door.name == e.door.name && b.door.pos.x == e.door.pos.x && b.from.z == e.from.z && r.AtEnd(),
          "DoorEvent round trip");

    ByteReader shortR(w.Span().subspan(0, 20));
    Check(!ReadEvent(shortR, b), "a truncated DoorEvent is refused");

    std::vector<uint8_t> bad(w.Span().begin(), w.Span().end());
    bad[3] = 9;  // act
    ByteReader badAct(bad.data(), bad.size());
    Check(!ReadEvent(badAct, b), "an unknown act is refused");
    bad[3] = 3;
    bad[4] = 7;  // state with unknown bits
    ByteReader badState(bad.data(), bad.size());
    Check(!ReadEvent(badState, b), "unknown state bits are refused");

    Event nan = e;
    nan.door.pos.x = std::nanf("");
    ByteWriter wn;
    WriteEvent(wn, nan);
    ByteReader rn(wn.Span());
    Check(!ReadEvent(rn, b), "a position that is not a number is refused");

    std::vector<SnapEntry> list;
    for (int i = 0; i < 5; ++i) list.push_back({{NameHash("door_" + std::to_string(i)), P(i * 3.0f, 1, 2)},
                                                static_cast<uint8_t>(i % 4)});
    ByteWriter ws;
    WriteSnapshot(ws, list);
    Check(ws.Span().size() == 1 + 5 * 17, "DoorSnapshot: 17 bytes per door", std::to_string(ws.Span().size()));
    std::vector<SnapEntry> back;
    ByteReader rs(ws.Span());
    Check(ReadSnapshot(rs, back) && back.size() == 5 && back[3].state == 3 && back[4].door.pos.x == 12.0f,
          "DoorSnapshot round trip");
    std::vector<SnapEntry> many(200, list[0]);
    ByteWriter wm;
    WriteSnapshot(wm, many);
    ByteReader rm(wm.Span());
    Check(ReadSnapshot(rm, back) && back.size() == kMaxSnapshot, "a snapshot is cut to its maximum",
          std::to_string(back.size()));
    const uint8_t tooMany[] = {200};
    ByteReader rt(tooMany, 1);
    Check(!ReadSnapshot(rt, back) && back.empty(), "a snapshot claiming more than the maximum is refused");

    SnapRequest q;
    q.pos = P(10, 20, 30);
    q.reason = Reason::Reconnect;
    ByteWriter wq;
    WriteRequest(wq, q);
    SnapRequest qb;
    ByteReader rq(wq.Span());
    Check(wq.Span().size() == 13 && ReadRequest(rq, qb) && qb.reason == Reason::Reconnect && qb.pos.y == 20,
          "DoorSnapRequest round trip");
    const uint8_t badReason[13] = {};
    ByteReader rb(badReason, sizeof badReason);
    Check(!ReadRequest(rb, qb), "a request without a reason is refused");
}

void TestIdentity() {
    Check(NameHash("Door_Front_01") == NameHash("door_front_01"), "names compare without case");
    Check(NameHash("") == 0 && NameHash("-") == 0 && NameHash("a") != 0, "no name hashes to 0");
    const Ref a{NameHash("door_a"), P(100, 200, 5)};
    Check(Same(a, {a.name, P(100.6f, 200.5f, 5.1f)}), "the same name 0.8 m away is the same door");
    Check(!Same(a, {a.name, P(101.5f, 200, 5)}), "the same name 1.5 m away is another door");
    Check(!Same(a, {NameHash("door_b"), P(100, 200, 5)}), "another name at the same spot is another door");
    const Ref n{0, P(5, 5, 5)};
    Check(Same(n, {0, P(5.2f, 5, 5)}) && !Same(n, {0, P(5.5f, 5, 5)}), "doors without a name match only 0.3 m apart");

    // two doors of one name side by side (a double door): each finds its own
    Doors d;
    std::vector<Apply> out;
    d.Seen(1001, "door_double", P(0, 0, 0), 0, 0, out);
    d.Seen(1002, "door_double", P(1.4f, 0, 0), 0, 0, out);
    uint32_t id = 0;
    Check(d.Find({NameHash("door_double"), P(0.3f, 0, 0)}, &id) && id == 1001, "a double door: the left one");
    Check(d.Find({NameHash("door_double"), P(1.2f, 0, 0)}, &id) && id == 1002, "a double door: the right one");
    Check(!d.Find({NameHash("door_double"), P(5, 0, 0)}, &id), "no door there");
}

void TestActs() {
    Check(DiffAct(0, kOpen) == Act::Open && DiffAct(kOpen, 0) == Act::Close && DiffAct(kLocked, 0) == Act::Unlock &&
              DiffAct(0, kLocked) == Act::Lock && DiffAct(kLocked, kOpen) == Act::Open &&
              DiffAct(kOpen, kLocked) == Act::Set && DiffAct(3, 3) == Act::None,
          "acts of a change");
    Check(StateAfter(Act::Kick, kClosedLocked, 0) == kOpenUnlocked && StateAfter(Act::Lock, kOpen, 0) == kLocked &&
              StateAfter(Act::Lockpick, kClosedLocked, 0) == 0 && StateAfter(Act::Set, 0, 3) == 3 &&
              StateAfter(Act::Close, 1, 0) == 0,
          "state after an act");
    Check(StateName(kOpen) == "open,unlocked" && StateName(kLocked) == "closed,locked", "state names");
    Check(ActFromCode(3) == Act::Kick && ActFromCode(8) == Act::None, "act codes");
}

// Guest: our player's changes go out, our world's stay; what the host sends is applied and its echo not sent back.
void TestGuestTable() {
    Doors g;
    g.SetHost(false);
    std::vector<Apply> out;
    g.Seen(10, "door_shop", P(10, 0, 0), kClosedUnlocked, 0, out);
    g.Seen(11, "door_back", P(20, 0, 0), kClosedLocked, 0, out);
    Check(out.empty() && g.GetStats().doors == 2, "two doors seen, nothing to do");

    Event ev;
    Verdict v = g.Changed(10, kOpenUnlocked, true, Act::Open, P(9, 0, 0), 2, 1.0, ev, out);
    Check(v == Verdict::Send && ev.by == 2 && ev.act == Act::Open && ev.state == kOpenUnlocked &&
              ev.door.name == NameHash("door_shop") && ev.from.x == 9,
          "our player opened a door: it goes out", VerdictName(v));
    v = g.Changed(10, kOpenUnlocked, true, Act::Open, P(9, 0, 0), 2, 1.1, ev, out);
    Check(v == Verdict::Same, "the same state again is no change");
    v = g.Changed(10, kClosedUnlocked, false, Act::None, P(0, 0, 0), 2, 2.0, ev, out);
    Check(v == Verdict::WorldKept && out.empty(), "an NPC closed it on our game: stays local");
    v = g.Changed(99, kOpen, true, Act::Open, P(0, 0, 0), 2, 2.0, ev, out);
    Check(v == Verdict::Unknown, "a door we do not know is ignored");

    // the host kicks door_back open
    Event h;
    h.by = 0;
    h.act = Act::Kick;
    h.state = kOpenUnlocked;
    h.door = {NameHash("door_back"), P(20.2f, 0.1f, 0)};
    h.from = P(21, 0, 0);
    g.Remote(h, 3.0, out);
    Check(out.size() == 1 && out[0].local == 11 && out[0].act == Act::Kick && out[0].state == kOpenUnlocked &&
              out[0].from.x == 21 && std::string(out[0].why) == "remote",
          "the host's kick is applied to our door");
    Check(g.Expecting(11), "and its echo is expected");
    out.clear();
    // our poll sees it unlocked first (still closed), then open: both are the echo
    v = g.Changed(11, kClosedUnlocked, false, Act::None, P(0, 0, 0), 2, 3.2, ev, out);
    Check(v == Verdict::Echo, "the unlock step of the kick is the echo", VerdictName(v));
    v = g.Changed(11, kOpenUnlocked, false, Act::None, P(0, 0, 0), 2, 3.4, ev, out);
    Check(v == Verdict::Echo && !g.Expecting(11), "the open door is the echo, nothing goes back", VerdictName(v));
    v = g.Changed(11, kClosedUnlocked, true, Act::Close, P(20, 1, 0), 2, 5.0, ev, out);
    Check(v == Verdict::Send && ev.act == Act::Close, "we close it afterwards: that goes out");

    // a remote change that does not show in time is reported once
    h.state = kOpenUnlocked;
    g.Remote(h, 6.0, out);
    Check(out.size() == 1, "the host opens it again");
    out.clear();
    Check(g.Expire(7.0).empty(), "not overdue yet");
    const auto unmet = g.Expire(6.0 + kEchoWindow + 0.1);
    Check(unmet.size() == 1 && unmet[0] == 11 && g.GetStats().unmet == 1, "overdue: reported as unmet");

    // a remote change of a door in that state already does nothing
    h.state = kClosedUnlocked;
    h.act = Act::Close;
    g.Remote(h, 8.0, out);
    Check(out.empty(), "a door already in that state is left alone");

    // a change against the expectation (the host opened it, an NPC here locked it meanwhile) is a real change
    h.state = kOpenUnlocked;
    h.act = Act::Open;
    g.Remote(h, 9.0, out);
    out.clear();
    v = g.Changed(11, kClosedLocked, false, Act::None, P(0, 0, 0), 2, 9.2, ev, out);
    // (not an echo; the host decided "unlocked", so our own world's lock is taken off again: only the lock)
    Check(v == Verdict::Revert && out.size() == 1 && out[0].state == kClosedUnlocked,
          "a change away from what we set is not an echo", VerdictName(v));
}

// Doors that are not loaded when a change comes are set when they stream in.
void TestMemory() {
    Doors g;
    std::vector<Apply> out;
    Event h;
    h.by = 1;
    h.act = Act::Open;
    h.state = kOpenUnlocked;
    h.door = {NameHash("door_far"), P(500, 500, 2)};
    g.Remote(h, 1.0, out);
    uint8_t st = 0;
    Check(out.empty() && g.GetStats().pending == 1 && g.Remembered(h.door, &st) && st == kOpenUnlocked,
          "a change of a door we have not loaded is remembered");
    g.Seen(77, "door_far", P(500.3f, 500, 2), kClosedLocked, 50.0, out);
    Check(out.size() == 1 && out[0].local == 77 && out[0].state == kOpenUnlocked && std::string(out[0].why) == "memory",
          "it is set when it streams in");
    out.clear();
    g.Gone(77);
    g.Seen(78, "door_far", P(500.3f, 500, 2), kOpenUnlocked, 60.0, out);
    Check(out.empty(), "streamed in again already in that state: nothing to do");

    Doors cap;
    for (uint32_t i = 0; i < kMaxMemory + 10; ++i) {
        Event e = h;
        e.door = {NameHash("door_" + std::to_string(i)), P(static_cast<float>(i) * 5, 0, 0)};
        cap.Remote(e, static_cast<double>(i), out);
    }
    Check(cap.MemorySize() == kMaxMemory && !cap.Remembered({NameHash("door_0"), P(0, 0, 0)}, nullptr) &&
              cap.Remembered({NameHash("door_265"), P(1325, 0, 0)}, nullptr),
          "the memory keeps the newest changes", std::to_string(cap.MemorySize()));
    cap.Clear();
    Check(cap.MemorySize() == 0 && cap.GetStats().doors == 0, "a new world forgets everything");
}

// Host: its own and its world's changes go out; a guest may not open a door that is locked here.
void TestHost() {
    Doors h;
    h.SetHost(true);
    std::vector<Apply> out;
    h.Seen(5, "door_story", P(0, 0, 0), kClosedLocked, 10.0, out);
    h.Seen(6, "door_plain", P(5, 0, 0), kClosedUnlocked, 10.0, out);
    Event ev;
    Verdict v = h.Changed(6, kOpenUnlocked, false, Act::None, P(0, 0, 0), 0, 11.0, ev, out);
    Check(v == Verdict::Send && ev.by == kWorld && ev.act == Act::Open, "an NPC on the host's game opened a door: sent",
          VerdictName(v));
    v = h.Changed(6, kClosedUnlocked, true, Act::Close, P(5, 1, 0), 0, 12.0, ev, out);
    Check(v == Verdict::Send && ev.by == 0 && ev.act == Act::Close, "the host's player closed it: sent as his");

    Event g;
    g.by = 1;
    g.act = Act::Open;
    g.state = kOpenUnlocked;
    g.door = {NameHash("door_story"), P(0.2f, 0, 0)};
    uint8_t hs = 0xEE;
    h.Fresh(5, 12.0);
    Check(!h.Accept(g, 12.5, &hs) && hs == kClosedLocked, "a guest opening a door locked here (live) is refused");
    Check(h.Accept(g, 12.0 + kFreshFor + 1, &hs), "not polled lately: the host cannot tell, accepted");
    g.act = Act::Kick;
    Check(h.Accept(g, 12.5, &hs), "a kick (it breaks the lock) is accepted");
    g.act = Act::Lockpick;
    g.state = kClosedUnlocked;
    Check(h.Accept(g, 12.5, &hs), "a lock pull is accepted");
    g.door = {NameHash("door_unknown"), P(1000, 0, 0)};
    g.act = Act::Open;
    Check(h.Accept(g, 12.5, &hs), "a door the host has not loaded is accepted");

    // the accepted kick is applied here, its echo is not sent again
    g.act = Act::Kick;
    g.state = kOpenUnlocked;
    g.door = {NameHash("door_story"), P(0.2f, 0, 0)};
    h.Remote(g, 13.0, out);
    Check(out.size() == 1 && out[0].local == 5 && out[0].by == 1, "the guest's kick is applied on the host's game");
    out.clear();
    v = h.Changed(5, kOpenUnlocked, false, Act::None, P(0, 0, 0), 0, 13.4, ev, out);
    Check(v == Verdict::Echo, "its echo is not broadcast again", VerdictName(v));
}

void TestSnapshot() {
    Doors h;
    h.SetHost(true);
    std::vector<Apply> out;
    for (int i = 0; i < 10; ++i)
        h.Seen(100 + i, "door_" + std::to_string(i), P(static_cast<float>(i) * 10, 0, 0),
               static_cast<uint8_t>(i % 2 ? kOpenUnlocked : kClosedLocked), 100.0, out);
    // a door far away that a guest opened (remembered), one of ours stale (not polled lately)
    Event far;
    far.by = 2;
    far.act = Act::Open;
    far.state = kOpenUnlocked;
    far.door = {NameHash("door_far"), P(900, 0, 0)};
    h.Remote(far, 101.0, out);
    std::vector<SnapEntry> s = h.Snapshot(P(0, 0, 0), 25.0f, 101.0, kMaxSnapshot);
    Check(s.size() == 4 && s[0].door.name == NameHash("door_0") && s[1].door.name == NameHash("door_1") &&
              s[2].door.name == NameHash("door_2") && s[3].door.name == NameHash("door_far"),
          "the live doors within the radius nearest first, then the remembered ones", std::to_string(s.size()));
    s = h.Snapshot(P(0, 0, 0), 25.0f, 100.0 + kFreshFor + 5, kMaxSnapshot);
    Check(s.size() == 1 && s[0].door.name == NameHash("door_far"), "stale doors are not sent, the remembered are");
    h.Fresh(101, 200.0);
    s = h.Snapshot(P(0, 0, 0), 25.0f, 200.5, 1);
    Check(s.size() == 1 && s[0].door.name == NameHash("door_1"), "the maximum is kept");
    // a remembered door that is also live is sent once
    Event near = far;
    near.door = {NameHash("door_1"), P(10, 0, 0)};
    h.Remote(near, 200.6, out);
    s = h.Snapshot(P(0, 0, 0), 25.0f, 200.7, kMaxSnapshot);
    int ones = 0;
    for (const auto& e : s) ones += e.door.name == NameHash("door_1") ? 1 : 0;
    Check(ones == 1, "a door is in a snapshot once");

    // the guest applies only what differs, remembers all
    Doors g;
    g.Seen(7, "door_0", P(0.1f, 0, 0), kClosedLocked, 1.0, out);
    g.Seen(8, "door_1", P(10, 0.2f, 0), kClosedUnlocked, 1.0, out);
    out.clear();
    std::vector<SnapEntry> in = {{{NameHash("door_0"), P(0, 0, 0)}, kClosedLocked},
                                 {{NameHash("door_1"), P(10, 0, 0)}, kOpenUnlocked},
                                 {{NameHash("door_far"), P(900, 0, 0)}, kOpenUnlocked}};
    g.ApplySnapshot(in, 2.0, out);
    Check(out.size() == 1 && out[0].local == 8 && out[0].act == Act::Set && out[0].state == kOpenUnlocked &&
              std::string(out[0].why) == "snapshot",
          "the snapshot sets the door that differs");
    Check(g.MemorySize() == 3, "and remembers every entry (door_far when it streams in)");
}

// Guest: his own mission scripts change the lock of a door the host decided: put back, a few times a minute at most.
void TestRevert() {
    Doors g;
    std::vector<Apply> out;
    g.Seen(3, "door_story", P(0, 0, 0), kClosedUnlocked, 0, out);
    Event h;
    h.by = kWorld;
    h.act = Act::Lock;
    h.state = kClosedLocked;
    h.door = {NameHash("door_story"), P(0, 0, 0)};
    g.Remote(h, 1.0, out);
    Check(out.size() == 1 && out[0].state == kClosedLocked, "the host's script locked the story door");
    out.clear();
    Event ev;
    g.Changed(3, kClosedLocked, false, Act::None, P(0, 0, 0), 1, 1.2, ev, out);  // the echo
    Verdict v = g.Changed(3, kClosedUnlocked, false, Act::None, P(0, 0, 0), 1, 10.0, ev, out);
    Check(v == Verdict::Revert && out.size() == 1 && out[0].state == kClosedLocked &&
              std::string(out[0].why) == "revert",
          "our own script unlocked it: locked again", VerdictName(v));
    int reverts = 1;
    for (int i = 0; i < 5; ++i) {
        out.clear();
        g.Changed(3, kClosedLocked, false, Act::None, P(0, 0, 0), 1, 11.0 + i, ev, out);  // our put-back shows
        v = g.Changed(3, kClosedUnlocked, false, Act::None, P(0, 0, 0), 1, 11.5 + i, ev, out);
        if (v == Verdict::Revert) ++reverts;
    }
    Check(reverts == kMaxReverts && v == Verdict::WorldKept, "at most a few put-backs a minute",
          std::to_string(reverts));
    out.clear();
    g.Changed(3, kClosedLocked, false, Act::None, P(0, 0, 0), 1, 80.0, ev, out);
    v = g.Changed(3, kClosedUnlocked, false, Act::None, P(0, 0, 0), 1, 80.5, ev, out);
    Check(v == Verdict::Revert, "a minute later it is put back again");
    // the open bit is not reverted (NPCs walk through doors on every game)
    Doors g2;
    g2.Seen(4, "door_x", P(0, 0, 0), kClosedUnlocked, 0, out);
    h.door = {NameHash("door_x"), P(0, 0, 0)};
    h.state = kClosedUnlocked;
    h.act = Act::Close;
    g2.Remote(h, 1.0, out);
    out.clear();
    v = g2.Changed(4, kOpenUnlocked, false, Act::None, P(0, 0, 0), 1, 5.0, ev, out);
    Check(v == Verdict::WorldKept && out.empty(), "an NPC opening a host-decided door is left alone");
}

// A host and two guests, the network in between as door_sync.cpp does it.
struct Game {
    Doors d;
    uint8_t id;
    std::vector<Apply> applies;
    std::vector<Event> sent;
    std::map<uint32_t, uint8_t> world;  // the doors as the game has them (local id -> state)
};

void Poll(Game& g, uint32_t local, bool ours, Act act, double now) {
    Event ev;
    if (g.d.Changed(local, g.world[local], ours, act, P(0, 0, 0), g.id, now, ev, g.applies) == Verdict::Send)
        g.sent.push_back(ev);
}

// carry out the applies (the door ends in the wanted state), then the poll sees them
void Run(Game& g, double now) {
    std::vector<Apply> a;
    a.swap(g.applies);
    for (const Apply& x : a) {
        g.world[x.local] = x.state;
        Poll(g, x.local, false, Act::None, now);
    }
}

void TestSession() {
    Game host{{}, 0}, g1{{}, 1}, g2{{}, 2};
    host.d.SetHost(true);
    std::vector<Apply> none;
    // the same three doors on every game, other entity ids
    const char* names[] = {"door_shop_front", "door_shop_back", "door_garage"};
    for (int i = 0; i < 3; ++i) {
        const Vec3 pos = P(100.0f + i * 4, 50, 1);
        const uint8_t st = i == 1 ? kClosedLocked : kClosedUnlocked;
        host.world[10 + i] = st;
        g1.world[200 + i] = st;
        g2.world[3000 + i] = st;
        host.d.Seen(10 + i, names[i], pos, st, 0, none);
        g1.d.Seen(200 + i, names[i], P(pos.x + 0.05f, pos.y, pos.z), st, 0, none);
        g2.d.Seen(3000 + i, names[i], P(pos.x, pos.y - 0.04f, pos.z), st, 0, none);
    }
    auto hostReceives = [&](Game& from, double now) {
        for (Event ev : from.sent) {
            ev.by = from.id;
            uint8_t hs = 0;
            if (host.d.Accept(ev, now, &hs)) {
                host.d.Remote(ev, now, host.applies);
                for (Game* g : {&g1, &g2})
                    if (g->id != ev.by) g->d.Remote(ev, now, g->applies);
            } else {
                Event fix = ev;
                fix.by = kWorld;
                fix.act = Act::Set;
                fix.state = hs;
                from.d.Remote(fix, now, from.applies);
            }
        }
        from.sent.clear();
    };
    auto hostSends = [&](double now) {
        for (const Event& ev : host.sent)
            for (Game* g : {&g1, &g2}) g->d.Remote(ev, now, g->applies);
        host.sent.clear();
    };

    // guest 1 kicks the locked back door open
    g1.world[201] = kOpenUnlocked;
    Poll(g1, 201, true, Act::Kick, 1.0);
    Check(g1.sent.size() == 1 && g1.sent[0].act == Act::Kick, "guest 1 kicked the back door: it goes to the host");
    hostReceives(g1, 1.1);
    Run(host, 1.2);
    Run(g2, 1.2);
    Run(g1, 1.2);
    Check(host.world[11] == kOpenUnlocked && g2.world[3001] == kOpenUnlocked && host.sent.empty() &&
              g2.sent.empty() && g1.sent.empty(),
          "it is open on every game, nothing echoes");

    // the host opens the front door by hand
    host.world[10] = kOpenUnlocked;
    Poll(host, 10, true, Act::Open, 2.0);
    hostSends(2.1);
    Run(g1, 2.2);
    Run(g2, 2.2);
    Check(g1.world[200] == kOpenUnlocked && g2.world[3000] == kOpenUnlocked && g1.sent.empty() && g2.sent.empty(),
          "the host's door opens on both guests");

    // the host's story locks the garage; guest 2's own script unlocks it later: his game locks it again
    host.world[12] = kClosedLocked;
    Poll(host, 12, false, Act::None, 3.0);
    hostSends(3.1);
    Run(g1, 3.2);
    Run(g2, 3.2);
    Check(g1.world[202] == kClosedLocked && g2.world[3002] == kClosedLocked, "the host's script locked it everywhere");
    g2.world[3002] = kClosedUnlocked;
    Poll(g2, 3002, false, Act::None, 20.0);
    Run(g2, 20.1);
    Check(g2.world[3002] == kClosedLocked && g2.sent.empty(), "guest 2's own unlock is put back, nothing sent");

    // guest 2's game had it unlocked anyway and he opens it by hand: the host refuses, his door is put back
    g2.world[3002] = kOpenUnlocked;
    host.d.Fresh(12, 21.0);
    Poll(g2, 3002, true, Act::Open, 21.0);
    hostReceives(g2, 21.1);
    Run(g2, 21.2);
    Run(host, 21.2);
    Run(g1, 21.2);
    Check(host.world[12] == kClosedLocked && g1.world[202] == kClosedLocked && g2.world[3002] == kClosedLocked,
          "opening a door the host keeps locked is undone on his game");

    // a snapshot for a guest who comes back: the open doors and the locked garage
    const std::vector<SnapEntry> snap = host.d.Snapshot(P(104, 50, 1), kSnapshotRadius, 21.5, kMaxSnapshot);
    Check(snap.size() == 3, "the snapshot has the three doors", std::to_string(snap.size()));
    Game g3{{}, 3};
    for (int i = 0; i < 3; ++i) {
        g3.world[50 + i] = i == 1 ? kClosedLocked : kClosedUnlocked;  // a fresh world: the game's own states
        g3.d.Seen(50 + i, names[i], P(100.0f + i * 4, 50, 1), g3.world[50 + i], 22.0, none);
    }
    g3.d.ApplySnapshot(snap, 22.0, g3.applies);
    Run(g3, 22.1);
    Check(g3.world[50] == kOpenUnlocked && g3.world[51] == kOpenUnlocked && g3.world[52] == kClosedLocked &&
              g3.sent.empty(),
          "the returning guest's doors are as the host's");
}

}  // namespace

int main() {
    TestWire();
    TestIdentity();
    TestActs();
    TestGuestTable();
    TestMemory();
    TestHost();
    TestSnapshot();
    TestRevert();
    TestSession();
    std::printf("%s (%d failure%s)\n", g_failures ? "FAILED" : "ALL PASSED", g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
