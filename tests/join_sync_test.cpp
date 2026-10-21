// join_sync_test: unit tests for the join / reconnect completeness report (src/game/join_sync.h): first arrivals per
// system and per player, "complete" once every required part is here, "missing" 15 s after we are in the world, a
// late complete, optional systems (NPCs, traffic) never waited for, car copies that asked for their damage, marks that
// come in the same Poll as the activation, a reconnect, and the JoinReport body. The 2026-10-19 two-PC test had no
// way to tell how long a newcomer waited for his world. Prints PASS/FAIL per check; exits non-zero on any failure.
#include "game/join_sync.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace mc;
using namespace mc::game::join;

namespace {

int g_failures = 0;

void Check(bool ok, const char* name, const std::string& detail = {}) {
    std::printf("%s  %-70s %s\n", ok ? "PASS" : "FAIL", name, detail.c_str());
    std::fflush(stdout);
    if (!ok) ++g_failures;
}

using Ev = Tracker::Event;

const Ev* Find(const std::vector<Ev>& v, Ev::Kind k) {
    for (const Ev& e : v)
        if (e.kind == k) return &e;
    return nullptr;
}

void MarkAll(Tracker& t, double now, const std::vector<uint32_t>& others) {
    for (uint32_t id : others) {
        t.Mark(System::Player, std::to_string(id), now);
        t.Mark(System::Police, std::to_string(id), now);
    }
    t.Mark(System::Mission, {}, now);
    t.Mark(System::WorldEnv, {}, now);
}

void TestComplete() {
    Tracker t;
    t.Begin(10.0, false, {0});
    t.InWorld(12.0);
    t.Mark(System::WorldEnv, {}, 10.1);
    t.Mark(System::Mission, {}, 10.2);
    t.Mark(System::Player, "0", 10.3);
    t.Mark(System::Player, "0", 10.4);  // only the first counts
    auto ev = t.Poll(10.5);
    Check(ev.size() == 3 && ev[0].kind == Ev::First && ev[0].sys == System::WorldEnv &&
              std::fabs(ev[0].after - 0.1) < 1e-9,
          "first arrivals: one event each, after = since the start", std::to_string(ev.size()));
    Check(!Find(ev, Ev::Complete), "not complete without police and doors");
    t.Mark(System::Police, "0", 11.0);
    t.Mark(System::Npcs, {}, 11.5);
    t.Mark(System::Doors, {}, 15.2);
    ev = t.Poll(15.3);
    const Ev* c = Find(ev, Ev::Complete);
    Check(c && std::fabs(c->after - 3.2) < 1e-9 && !c->late, "complete when the last required part came (doors), timed from the world's load",
          c ? std::to_string(c->after) : "none");
    Check(c && c->none == "traffic", "complete: optional systems that sent nothing are named", c ? c->none : "");
    Check(t.Complete() && t.Poll(40).empty(), "complete: no missing report afterwards");
    Check(std::fabs(t.FirstAfter(System::Npcs) - 1.5) < 1e-9 && t.FirstAfter(System::Traffic) < 0,
          "first times per system for the report");
}

void TestMissing() {
    Tracker t;
    t.Begin(0.0, false, {0, 2});
    t.InWorld(20.0);  // loading took 20 s: the 15 s count from here
    MarkAll(t, 1.0, {0});  // player 2 never sends anything
    t.Mark(System::Doors, {}, 24.0);
    auto ev = t.Poll(30.0);
    Check(!Find(ev, Ev::Missing) && !Find(ev, Ev::Complete), "missing: not before 15 s in the world");
    ev = t.Poll(35.0);
    const Ev* m = Find(ev, Ev::Missing);
    Check(m && m->missing == "player:2,police:2" && std::fabs(m->after - 15.0) < 1e-9,
          "missing: after 15 s in the world, names what is missing", m ? m->missing : "none");
    Check(t.Poll(36.0).empty(), "missing: reported once");
    t.Mark(System::Player, "2", 40.0);
    t.Mark(System::Police, "2", 41.0);
    ev = t.Poll(41.5);
    const Ev* c = Find(ev, Ev::Complete);
    Check(c && c->late && std::fabs(c->after - 21.0) < 1e-9, "missing: a late complete follows",
          c ? std::to_string(c->after) : "none");
}

void TestNotInWorld() {
    Tracker t;
    t.Begin(0.0, false, {0});
    MarkAll(t, 0.5, {0});
    t.Mark(System::Doors, {}, 0.6);
    Check(!Find(t.Poll(1.0), Ev::Complete), "menu: not complete before we are in the world");
    Check(t.Poll(100.0).size() == 0, "menu: no missing report while we are not in the world");
    t.InWorld(100.0);
    const Ev* c = nullptr;
    auto ev = t.Poll(100.1);
    c = Find(ev, Ev::Complete);
    Check(c && c->after == 0.0, "menu: complete as soon as we are in the world (0 s: everything came in the menu)",
          c ? std::to_string(c->after) : "none");
}

void TestCarDamage() {
    Tracker t;
    t.Begin(0.0, false, {0});
    t.InWorld(0.0);
    MarkAll(t, 0.5, {0});
    t.Mark(System::Doors, {}, 3.5);
    t.Expect(System::CarDamage, "0:1234");
    t.Expect(System::CarDamage, "t17");
    t.Mark(System::CarDamage, "0:1234", 4.0);
    Check(!Find(t.Poll(4.1), Ev::Complete), "cars: waits for every copy that asked");
    t.Mark(System::CarDamage, "t17", 6.0);
    const Ev* c = nullptr;
    auto ev = t.Poll(6.1);
    c = Find(ev, Ev::Complete);
    Check(c && std::fabs(c->after - 6.0) < 1e-9, "cars: complete with the last answer", c ? std::to_string(c->after) : "");
    t.Expect(System::CarDamage, "0:999");  // after complete: not waited for
    Check(t.Missing().empty(), "cars: a copy asking after the world is complete is not waited for");

    // the 2026-10-21 05:37 run's rejoin: a host traffic car's copy asked, then was despawned before the answer
    Tracker f;
    f.Begin(0.0, false, {0});
    f.InWorld(0.0);
    MarkAll(f, 0.5, {0});
    f.Mark(System::Doors, {}, 3.5);
    f.Expect(System::CarDamage, "t125");
    f.Expect(System::CarDamage, "t126");
    f.Mark(System::CarDamage, "t126", 4.0);
    Check(!Find(f.Poll(4.1), Ev::Complete), "cars: t125 not answered yet: waited for");
    f.Forget(System::CarDamage, "t125");
    auto fe = f.Poll(4.2);
    const Ev* fc = Find(fe, Ev::Complete);
    Check(fc && !fc->late && fc->after < 5.0, "cars: its copy despawned: no longer waited for (complete, not 'still missing')",
          fc ? std::to_string(fc->after) : "none");
}

void TestEarlyMarks() {
    // a handler runs in the Poll that makes the session active, before JoinSync's tick calls Begin
    Tracker t;
    t.Mark(System::WorldEnv, {}, 5.0);
    t.Mark(System::Mission, {}, 5.0);
    Check(t.Poll(5.0).empty(), "early: nothing reported before Begin");
    t.Begin(5.01, false, {0});
    auto ev = t.Poll(5.02);
    Check(ev.size() == 2 && ev[0].after == 0.0, "early: kept and reported at 0 s", std::to_string(ev.size()));
}

void TestReconnect() {
    Tracker t;
    t.Begin(0.0, false, {0});
    t.InWorld(0.0);
    MarkAll(t, 0.5, {0});
    t.Mark(System::Doors, {}, 3.5);
    Check(Find(t.Poll(4.0), Ev::Complete) != nullptr, "reconnect: first join complete");
    t.Reset();  // the host is gone
    t.Begin(60.0, true, {0});
    t.InWorld(60.0);
    Check(t.Reconnect() && !t.Complete() && t.Missing() == "player:0,mission,worldenv,doors,police:0",
          "reconnect: everything is waited for again", t.Missing());
    MarkAll(t, 60.4, {0});
    t.Mark(System::Doors, {}, 63.4);
    const Ev* c = nullptr;
    auto ev = t.Poll(64.0);
    c = Find(ev, Ev::Complete);
    Check(c && std::fabs(c->after - 3.4) < 1e-6, "reconnect: complete, timed from the rejoin",
          c ? std::to_string(c->after) : "none");
}

void TestNoOthers() {
    Tracker t;
    t.Begin(0.0, false, {});
    t.InWorld(1.0);
    t.Mark(System::Mission, {}, 0.1);
    t.Mark(System::WorldEnv, {}, 0.1);
    t.Mark(System::Doors, {}, 4.0);
    Check(Find(t.Poll(4.0), Ev::Complete) != nullptr, "no other player listed: the host's world alone completes");
}

void TestReport() {
    Report r;
    r.reconnect = true;
    r.complete = false;
    r.afterMs = 15000;
    r.firstMs.fill(-1);
    r.firstMs[static_cast<size_t>(System::WorldEnv)] = 120;
    r.firstMs[static_cast<size_t>(System::Doors)] = 3400;
    r.missing = "player:2,police:2";
    net::ByteWriter w;
    WriteReport(w, r);
    net::ByteReader rd(w.Span());
    Report o;
    Check(ReadReport(rd, o) && o.reconnect && !o.complete && o.afterMs == 15000 &&
              o.firstMs[static_cast<size_t>(System::Doors)] == 3400 &&
              o.firstMs[static_cast<size_t>(System::Player)] == -1 && o.missing == r.missing,
          "JoinReport: round trip");
    Check(w.Size() == 1 + 4 + 1 + kSystems * 5 + 1 + r.missing.size(), "JoinReport: size",
          std::to_string(w.Size()));
    // a newer guest's report with a system we do not know
    net::ByteWriter w2;
    w2.U8(2);
    w2.U32(5000);
    w2.U8(2);
    w2.U8(1);
    w2.I32(100);
    w2.U8(40);
    w2.I32(200);
    w2.Str("");
    net::ByteReader rd2(w2.Span());
    Report o2;
    Check(ReadReport(rd2, o2) && o2.complete && o2.firstMs[1] == 100, "JoinReport: unknown systems are skipped");
    net::ByteReader rd3(w2.Span().subspan(0, 5));
    Check(!ReadReport(rd3, o2), "JoinReport: a cut body is refused");
}

}  // namespace

int main() {
    TestComplete();
    TestMissing();
    TestNotInWorld();
    TestCarDamage();
    TestEarlyMarks();
    TestReconnect();
    TestNoOthers();
    TestReport();
    std::printf("%s (%d failure%s)\n", g_failures ? "FAILED" : "OK", g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
