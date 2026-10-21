// guest_world_test: unit tests for the guest's own world off (src/game/guest_world.h, docs/host-authority.md work
// package A): the switches on the first tick of a world (its loading screen or the world itself), again after a reload,
// every 5 s and when other glue switched ambient traffic back on; never on the host; everything back when the session
// ends (also when it ends between worlds: on the next loading screen); the pedestrian budget read before it is set and
// put back to that value (or the ini's, or left to the next load); the fallback (BlockSpawnersInSphere every 20 m)
// when the budget call fails; the hide verdict for every entity class - never anything of ours, the 0.16 ClearLocal
// case (ped_sync's copy spawner, a crewed car 40 m under the player) included - and the view cone; and a replay of the
// 2026-10-20 guest sessions A and B (tests/data/guest_world_1020.h, tools/extract_guest_world.py). Prints PASS/FAIL
// per check; exits non-zero on any failure.
#include "game/guest_world.h"

#include "data/guest_world_1020.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace mc::game::guest_world;

namespace {

int g_failures = 0;

void Check(bool ok, const char* name, const std::string& detail = {}) {
    std::printf("%s  %-72s %s\n", ok ? "PASS" : "FAIL", name, detail.c_str());
    std::fflush(stdout);
    if (!ok) ++g_failures;
}

Frame Guest(double now, Phase ph, uint32_t key = 7) {
    Frame f;
    f.now = now;
    f.active = true;
    f.isHost = false;
    f.phase = ph;
    f.worldKey = ph == Phase::InWorld ? key : 0;
    f.havePos = ph == Phase::InWorld;
    return f;
}

// ---- the state machine ---------------------------------------------------------------------------------------------
void TestFirstWorld() {
    Policy p;
    // the guest loads the host's world: the first tick with the loading screen up switches
    Actions a = p.Tick(Guest(0.0, Phase::None));
    Check(!a.switchOff && a.hide == HideMode::None, "menus: nothing switched, nothing hidden");
    a = p.Tick(Guest(1.0, Phase::Loading));
    Check(a.switchOff && a.readBudget && a.budgetZero, "first loading tick: switched off, budget read first, then 0");
    Check(a.hide == HideMode::AtOnce && a.scan, "loading screen: hidden at once, scanned");
    p.OnBudgetRead(60);
    p.OnSwitched(true, true, true);
    Check(p.Applied() && p.SavedBudget() == 60 && p.BudgetState() == 1, "budget 60 kept, set to 0",
          std::to_string(p.SavedBudget()));
    a = p.Tick(Guest(1.1, Phase::Loading));
    Check(!a.switchOff, "the next tick: not switched again");
    Check(!a.scan, "at once: scanned every 0.25 s, not every tick");
    a = p.Tick(Guest(1.3, Phase::Loading));
    Check(a.scan, "at once: the next scan 0.25 s later");
    // the world: still at once for 3 s, then only out of view
    a = p.Tick(Guest(4.0, Phase::InWorld));
    Check(!a.switchOff && a.hide == HideMode::AtOnce, "first tick in the world (same world): no new apply, at once");
    a = p.Tick(Guest(6.9, Phase::InWorld));
    Check(a.hide == HideMode::AtOnce, "2.9 s in the world: still at once");
    Check(a.switchOff && a.readBudget, "5 s after the apply: switched again (budget read again too)");
    p.OnBudgetRead(0);  // our own 0: never the value to go back to
    p.OnSwitched(true, true, true);
    a = p.Tick(Guest(7.1, Phase::InWorld));
    Check(a.hide == HideMode::OutOfView && !a.switchOff, "3.1 s in the world: only out of view");
    Check(p.SavedBudget() == 60, "a reading of 0 (ours) does not replace 60");
    Check(p.Applies() == 1 && p.Reasserts() == 1 && p.Worlds() == 1, "one world, one apply, one re-apply");
    a = p.Tick(Guest(7.5, Phase::InWorld));
    Check(!a.scan, "out of view: scanned once a second");
    a = p.Tick(Guest(8.2, Phase::InWorld));
    Check(a.scan && a.trace, "out of view: the scan and the GW record a second later");
}

void TestJoinInWorld() {
    // the session starts while the guest is already in a world (a join into Free Ride, a reconnect as a new player)
    Policy p;
    Frame f = Guest(0.0, Phase::InWorld);
    f.active = false;
    Actions a = p.Tick(f);
    Check(!a.switchOff, "in the world, no session: nothing");
    f.now = 10.0;
    p.Tick(f);
    a = p.Tick(Guest(10.5, Phase::InWorld));
    Check(a.switchOff && a.hide == HideMode::OutOfView,
          "session starts 10 s into a world: switched at once, hidden out of view only");
}

void TestReload() {
    Policy p;
    p.Tick(Guest(0.0, Phase::Loading));
    p.OnBudgetRead(60);
    p.OnSwitched(true, true, true);
    p.Tick(Guest(2.0, Phase::InWorld, 11));
    // a fail-flow reload / checkpoint: the loading screen again
    Actions a = p.Tick(Guest(30.0, Phase::Loading));
    Check(a.switchOff && a.readBudget, "reload: switched again on the first loading tick");
    Check(a.hide == HideMode::AtOnce, "reload: hidden at once again");
    p.OnBudgetRead(72);  // the load put the game's own value back
    p.OnSwitched(true, true, true);
    Check(p.SavedBudget() == 72 && p.Applies() == 2 && p.Worlds() == 2, "reload: a second apply, the newest value kept",
          std::to_string(p.SavedBudget()));
    // a new world without a loading screen seen (the key changed): applied again
    p.Tick(Guest(32.0, Phase::InWorld, 12));
    a = p.Tick(Guest(40.0, Phase::InWorld, 13));
    Check(a.switchOff && p.Applies() == 3, "a new world key in the world: applied again");
    // through the menus (None) into a world: applied again
    p.Tick(Guest(50.0, Phase::None));
    a = p.Tick(Guest(51.0, Phase::InWorld, 14));
    Check(a.switchOff && p.Applies() == 4, "menus, then a world: applied again");
    a = p.Tick(Guest(51.1, Phase::None));
    Check(!a.switchOff && !a.restore && a.hide == HideMode::None, "between worlds: nothing (no restore while a guest)");
}

void TestNeverOnHost() {
    Policy p;
    int switched = 0, restores = 0, hides = 0;
    for (int i = 0; i < 3000; ++i) {
        Frame f = Guest(i * 0.033, i < 200 ? Phase::Loading : i < 2000 ? Phase::InWorld : i < 2100 ? Phase::Loading : Phase::InWorld);
        f.isHost = true;
        const Actions a = p.Tick(f);
        switched += a.switchOff ? 1 : 0;
        restores += a.restore ? 1 : 0;
        hides += a.hide != HideMode::None ? 1 : 0;
    }
    Check(switched == 0 && restores == 0 && hides == 0 && !p.Applied(), "host through two loads: nothing switched, "
          "hidden or restored", std::to_string(switched) + "/" + std::to_string(restores) + "/" + std::to_string(hides));
    // turned off in the ini: the same
    Policy q;
    Frame f = Guest(0.0, Phase::InWorld);
    f.enabled = false;
    Check(!q.Tick(f).switchOff, "guest_world=0: nothing switched");
}

void TestRestore() {
    Policy p;
    p.Tick(Guest(0.0, Phase::Loading));
    p.OnBudgetRead(60);
    p.OnSwitched(true, true, true);
    p.Tick(Guest(1.0, Phase::InWorld));
    Frame end = Guest(20.0, Phase::InWorld);
    end.active = false;
    Actions a = p.Tick(end);
    Check(a.restore && a.budgetRestore && a.budgetValue == 60, "session end in the world: everything back, budget 60",
          std::to_string(a.budgetValue));
    Check(!a.switchOff && a.hide == HideMode::None, "session end: nothing switched off or hidden any more");
    p.OnRestored(true);
    Check(p.BudgetState() == 2 && !p.Applied() && p.Restores() == 1, "restored once");
    a = p.Tick(Guest(20.1, Phase::InWorld));
    Check(a.switchOff && p.Applies() == 2, "a new session in the same world: applied again from the start");
    // we become the host (a new session as host): everything back
    Frame h = Guest(30.0, Phase::InWorld);
    h.isHost = true;
    a = p.Tick(h);
    Check(a.restore, "the guest hosts now: everything back");

    // the session ends between worlds: back on the next loading screen (the game's calls need a world)
    Policy q;
    q.Tick(Guest(0.0, Phase::InWorld));
    q.OnBudgetRead(64);
    q.OnSwitched(true, true, true);
    q.Tick(Guest(5.0, Phase::None));
    Frame menu = Guest(6.0, Phase::None);
    menu.active = false;
    a = q.Tick(menu);
    Check(!a.restore && q.RestorePending(), "session ends in the menus: the restore waits");
    Frame load = Guest(9.0, Phase::Loading);
    load.active = false;
    a = q.Tick(load);
    Check(a.restore && a.budgetValue == 64 && !q.RestorePending(), "... done on the next loading screen");
    a = q.Tick(load);
    Check(!a.restore, "... once");
}

void TestBudgetUnknown() {
    Policy p;  // no ini default
    p.Tick(Guest(0.0, Phase::InWorld));
    p.OnBudgetRead(-1);
    p.OnSwitched(true, true, true);
    Frame end = Guest(10.0, Phase::InWorld);
    end.active = false;
    Actions a = p.Tick(end);
    Check(a.restore && !a.budgetRestore && p.BudgetState() == 4,
          "budget not readable, no ini value: spawning back on, the budget left to the next world load");
    Policy q(PedSwitch::Budget, 60);  // guest_ped_budget=60 (the host's logged value)
    q.Tick(Guest(0.0, Phase::InWorld));
    q.OnBudgetRead(-1);
    q.OnSwitched(true, true, true);
    a = q.Tick(end);
    Check(a.restore && a.budgetRestore && a.budgetValue == 60, "budget not readable: the ini's guest_ped_budget");
    Policy r(PedSwitch::Budget, 60);
    r.Tick(Guest(0.0, Phase::InWorld));
    r.OnBudgetRead(55);
    r.OnSwitched(true, true, true);
    a = r.Tick(end);
    Check(a.budgetValue == 55, "a reading wins over the ini value");
    Policy s(PedSwitch::Budget, 9999);
    s.Tick(Guest(0.0, Phase::InWorld));
    s.OnBudgetRead(99999);  // not believed
    s.OnSwitched(true, true, true);
    a = s.Tick(end);
    Check(!a.budgetRestore, "an unbelievable reading (99999) and ini (9999): left to the next load");
}

float Dist2(const Vec3& a, const Vec3& b) { return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y)); }

void TestFallback() {
    Policy p;
    Frame f = Guest(0.0, Phase::InWorld);
    Actions a = p.Tick(f);
    Check(a.budgetZero, "budget mode: SetMaxHumanElements(0) asked");
    p.OnBudgetRead(60);
    p.OnSwitched(true, true, false);  // the call failed
    Check(p.PedMode() == PedSwitch::Block && p.BudgetFailed() && p.BudgetState() == 3,
          "budget call failed: the fallback (blocking) from now on");
    // walking 100 m along x: a block every 20 m
    std::vector<Vec3> blocks;
    int budgetCalls = 0, reapplies = 0;
    for (int i = 1; i <= 100; ++i) {
        f = Guest(i * 0.1, Phase::InWorld);
        f.pos = {static_cast<float>(i), 0, 0};
        a = p.Tick(f);
        blocks.insert(blocks.end(), a.block.begin(), a.block.end());
        budgetCalls += a.budgetZero ? 1 : 0;
        reapplies += a.switchOff ? 1 : 0;
    }
    Check(budgetCalls == 0 && reapplies >= 1, "the re-apply in block mode does not touch the budget",
          std::to_string(reapplies) + " re-apply");
    Check(blocks.size() == 5, "100 m walked: 5 spots blocked (every 20 m)", std::to_string(blocks.size()));
    bool spaced = true;
    for (size_t i = 1; i < blocks.size(); ++i) spaced = spaced && Dist2(blocks[i], blocks[i - 1]) >= kBlockStep - 0.01f;
    Check(spaced, "spots at least 20 m apart");
    // standing still for 25 s: the spot blocked again every 10 s (spawners streamed in meanwhile)
    size_t again = 0;
    for (int i = 0; i < 250; ++i) {
        f = Guest(10.0 + i * 0.1, Phase::InWorld);
        f.pos = {100, 0, 0};
        a = p.Tick(f);
        again += a.block.size();
    }
    Check(again == 2 || again == 3, "standing 25 s: blocked again every 10 s", std::to_string(again));
    Check(p.BlockedSpots() == 5, "the repeats are not remembered twice", std::to_string(p.BlockedSpots()));
    Frame end = Guest(40.0, Phase::InWorld);
    end.active = false;
    a = p.Tick(end);
    Check(a.restore && a.unblock.size() == 5 && p.BlockedSpots() == 0, "session end: every blocked spot unblocked",
          std::to_string(a.unblock.size()));
    Check(!a.budgetRestore, "the failed budget is not put back (it was never set)");
    // the ini chose block from the start: the budget is never touched
    Policy q(PedSwitch::Block);
    a = q.Tick(Guest(0.0, Phase::InWorld));
    Check(a.switchOff && !a.readBudget && !a.budgetZero && a.block.size() == 1,
          "guest_world_peds=block: switched off, no budget call, a spot blocked at once");
    Policy o(PedSwitch::Off);
    a = o.Tick(Guest(0.0, Phase::InWorld));
    Check(a.switchOff && !a.budgetZero && a.block.empty(), "guest_world_peds=off: pedestrians left alone");
    Check(PedSwitchOf("Block") == PedSwitch::Block && PedSwitchOf(" budget ") == PedSwitch::Budget &&
              PedSwitchOf("off") == PedSwitch::Off && PedSwitchOf("??") == PedSwitch::Budget,
          "the ini value read");
    // a long drive: the oldest far spot goes once more than 128 are remembered
    Policy l(PedSwitch::Block);
    size_t maxSpots = 0, early = 0;
    for (int i = 0; i < 4000; ++i) {
        f = Guest(i * 0.1, Phase::InWorld);
        f.pos = {static_cast<float>(i * 2), 0, 0};
        a = l.Tick(f);
        early += a.unblock.size();
        if (l.BlockedSpots() > maxSpots) maxSpots = l.BlockedSpots();
    }
    Check(maxSpots <= kBlockMax + 1 && early > 0, "8 km driven: at most 129 spots remembered, far ones unblocked early",
          std::to_string(maxSpots) + " / " + std::to_string(early));
}

void TestOtherOn() {
    Policy p;
    p.Tick(Guest(0.0, Phase::InWorld));
    p.OnSwitched(true, true, true);
    Frame f = Guest(1.0, Phase::InWorld);
    f.otherOn = true;  // traffic_sync.lua's T.LocalTraffic(true, "our own region")
    const Actions a = p.Tick(f);
    Check(a.switchOff, "another part switched ambient traffic on: off again at once");
}

// ---- the hide verdict -------------------------------------------------------------------------------------------------
View Camera() {
    View v;
    v.have = true;
    v.cam = {0, -3, 2};
    v.dir = {0, 1, 0};
    v.haveMe = true;
    v.me = {0, 0, 0};
    return v;
}

void TestView() {
    const View v = Camera();
    Check(InView(v, {0, 50, 0}, 60, 4, 150), "view: 50 m ahead is seen");
    Check(!InView(v, {0, -50, 0}, 60, 4, 150), "view: 50 m behind is not");
    Check(InView(v, {0, -5, 2}, 60, 4, 150), "view: 2 m behind the camera is (near)");
    Check(!InView(v, {0, 200, 0}, 60, 4, 150), "view: a human 200 m ahead is too far");
    Check(InView(v, {0, 200, 0}, 60, 8, 250), "view: a car 200 m ahead is seen (cars: 250 m)");
    Check(!InView(v, {100, 10, 0}, 60, 4, 150), "view: 84 degrees to the side is not");
    View none;
    Check(InView(none, {0, -50, 0}, 60, 4, 150), "view: no camera read: everything counts as seen");
}

const char* ModeName(HideMode m) { return HideModeName(m); }

void TestVerdicts() {
    const View v = Camera();
    const Vec3 behind{0, -40, 0}, ahead{0, 40, 0};
    const uint32_t genCar = kCar | kActive | kGeneratorDriver;
    const uint32_t ambHuman = kHuman | kActive | kAmbient;
    // the guest's own ambient world
    Check(Judge({genCar, behind, 10}, HideMode::AtOnce, v) == Verdict::Hide, "traffic car (generator driver): at once");
    Check(Judge({genCar, ahead, 10}, HideMode::AtOnce, v) == Verdict::Hide, "traffic car in view: hidden at once too");
    Check(Judge({genCar, behind, 10}, HideMode::OutOfView, v) == Verdict::Hide, "traffic car out of view: hidden");
    Check(Judge({genCar, ahead, 10}, HideMode::OutOfView, v) == Verdict::KeepInView, "traffic car in view: kept");
    Check(Judge({genCar, ahead, 10}, HideMode::None, v) == Verdict::KeepInView, "nothing hidden in mode none");
    Check(Judge({ambHuman, behind, 0}, HideMode::OutOfView, v) == Verdict::Hide, "ambient human out of view: hidden");
    Check(Judge({ambHuman, ahead, 0}, HideMode::OutOfView, v) == Verdict::KeepInView, "ambient human in view: kept");
    Check(Judge({ambHuman, ahead, 0}, HideMode::AtOnce, v) == Verdict::Hide, "ambient human: hidden at once");
    Check(Judge({genCar | kInView, behind, 10}, HideMode::OutOfView, v) == Verdict::Hide,
          "a stale kInView bit is recomputed from the camera");
    // a car the game made despite our switches (first seen kLateAfter s after they stood): hidden in view too
    Check(Judge({genCar | kSpawnedLate, ahead, 10}, HideMode::OutOfView, v) == Verdict::Hide,
          "a car made late (despite the switches) in view: hidden");
    Check(Judge({genCar | kSpawnedLate | kFresh, ahead, 10}, HideMode::OutOfView, v) == Verdict::KeepOurs,
          "a late car is still left alone while fresh (one of ours may be loading it)");
    Check(Judge({genCar | kSpawnedLate | kTrafficCopy, ahead, 10}, HideMode::OutOfView, v) == Verdict::KeepOurs,
          "a late car that is a traffic copy: ours");
    Check(Judge({ambHuman | kSpawnedLate, ahead, 0}, HideMode::OutOfView, v) == Verdict::KeepInView,
          "the late bit is for cars only (an ambient human in view stays)");
    Check(Judge({genCar | kSpawnedLate, ahead, 10}, HideMode::None, v) == Verdict::KeepInView,
          "a late car in mode none: nothing hidden");
    // not the guest's own ambient world: left alone
    struct Case { const char* name; uint32_t facts; Verdict want; };
    const Case others[] = {
        {"parked car (no driver)", kCar | kActive, Verdict::KeepOther},
        {"mission car (mission driver)", kCar | kActive, Verdict::KeepOther},
        {"tram with a generator driver", genCar | kTramOrBoat, Verdict::KeepOther},
        {"inactive traffic car", kCar | kGeneratorDriver, Verdict::KeepOther},
        {"mission NPC (named spawner)", kHuman | kActive | kMissionName, Verdict::KeepOther},
        {"mission NPC with AI (not ambient)", kHuman | kActive, Verdict::KeepOther},
        {"a traffic car's driver (in a vehicle)", kHuman | kActive | kAmbient | kInVehicle, Verdict::KeepOther},
        {"inactive human (hidden by someone)", kHuman | kAmbient, Verdict::KeepOther},
        {"neither car nor human", kActive | kAmbient, Verdict::KeepOther},
    };
    for (const Case& c : others)
        for (HideMode m : {HideMode::AtOnce, HideMode::OutOfView}) {
            const char* why = nullptr;
            const Verdict got = Judge({c.facts, behind, 5}, m, v, &why);
            Check(got == c.want, (std::string(c.name) + " (" + ModeName(m) + ")").c_str(), why ? why : "");
        }
    // every "ours" bit and every spawner look keeps it, whatever else it reads like, in every mode, in and out of view
    const uint32_t oursBits[] = {kPuppet, kPuppetFactory, kTrafficCopy, kCopyCrew, kPedOwned, kPedSpawner, kNpcCopy,
                                 kNpcSpawner, kPlayerCar, kOwnVehicle};
    const char* oursNames[] = {"puppet", "puppet factory", "traffic copy", "copy crew", "ped copy (PedOwned)",
                               "ped copy spawner", "npc copy / replica", "npc copy spawner", "player car", "own car"};
    int bad = 0, total = 0;
    for (size_t b = 0; b < sizeof oursBits / sizeof oursBits[0]; ++b) {
        int badHere = 0;
        for (uint32_t base : {genCar, ambHuman, genCar | kFixed, ambHuman | kMissionName})
            for (HideMode m : {HideMode::None, HideMode::AtOnce, HideMode::OutOfView})
                for (const Vec3& at : {behind, ahead, Vec3{0, 0, -40}, Vec3{500, 500, 0}}) {
                    ++total;
                    const Verdict got = Judge({base | oursBits[b], at, 10}, m, v);
                    if (got != Verdict::KeepOurs) ++badHere;
                }
        bad += badHere;
        Check(badHere == 0, (std::string("never hidden: ") + oursNames[b]).c_str());
    }
    Check(bad == 0, "every ours bit, 4 looks x 3 modes x 4 spots", std::to_string(total) + " cases");
    // the spawner looks (the copy spawner before its glue knows it)
    const Case spawners[] = {
        {"a fixed car with a generator crew (any spawner)", genCar | kFixed, Verdict::KeepOurs},
        {"a car while a factory loads its car", genCar | kFactoryBusy, Verdict::KeepOurs},
        {"a crewed car at a loading traffic copy's spot", genCar | kNearLoadingCopy, Verdict::KeepOurs},
        {"a car first seen moments ago", genCar | kFresh, Verdict::KeepOurs},
        {"a human first seen moments ago", ambHuman | kFresh, Verdict::KeepOurs},
    };
    for (const Case& c : spawners)
        for (HideMode m : {HideMode::AtOnce, HideMode::OutOfView}) {
            const char* why = nullptr;
            const Verdict got = Judge({c.facts, behind, 0}, m, v, &why);
            Check(got == c.want, (std::string(c.name) + " (" + ModeName(m) + ")").c_str(), why ? why : "");
        }
    // the 0.16 bug: ped_sync's copy spawner, a crewed shubert 40 m under the guest whose driver reads like a traffic
    // generator's; ClearLocal hid it (out of everyone's sight) and the pedestrian copies went with it (coverage 18 %).
    // Here: registered (kPedSpawner) and fixed it is kept; in the ticks before ped_sync knows it (state "spawn": not
    // fixed yet, not in P.fac) it is kept because it is 40 m below us, and because its factory is loading.
    const Vec3 under{-3, 0, -40};
    Check(Judge({genCar | kPedSpawner | kFixed, under, 0}, HideMode::AtOnce, v) == Verdict::KeepOurs,
          "0.16 ClearLocal case: ped copy spawner (registered, fixed), at once");
    Check(Judge({genCar | kPedSpawner | kFixed, under, 0}, HideMode::OutOfView, v) == Verdict::KeepOurs,
          "0.16 ClearLocal case: ped copy spawner, out of view");
    Check(Judge({genCar, under, 0}, HideMode::AtOnce, v) == Verdict::KeepOurs,
          "the spawner before ped_sync knows it: 40 m below us");
    Check(Judge({genCar | kFactoryBusy, {-3, 0, -10}, 0}, HideMode::AtOnce, v) == Verdict::KeepOurs,
          "the spawner before ped_sync knows it, our player on a slope: its factory is loading");
    Check(Judge({genCar, {-3, 0, -15}, 0}, HideMode::AtOnce, v) == Verdict::Hide,
          "15 m below us and nothing else: an ordinary traffic car on a lower road");
}

void TestCounts() {
    const View v = Camera();
    const uint32_t genCar = kCar | kActive | kGeneratorDriver;
    const uint32_t ambHuman = kHuman | kActive | kAmbient;
    const Entity moving{genCar, {0, 100, 0}, 12}, standing{genCar, {0, 100, 0}, 0.2f}, far{genCar, {0, 400, 0}, 12};
    Check(CountsAsOwnCar(moving, Verdict::KeepInView, v), "count: an own moving car 100 m away");
    Check(!CountsAsOwnCar(standing, Verdict::KeepInView, v), "count: not a standing one");
    Check(!CountsAsOwnCar(far, Verdict::KeepInView, v), "count: not beyond 150 m");
    Check(!CountsAsOwnCar(moving, Verdict::KeepOurs, v), "count: not a copy of ours");
    Check(CountsAsOwnPed({ambHuman, {0, 60, 0}, 0}, Verdict::Hide, v), "count: an own human 60 m away (if not hidden)");
    Check(!CountsAsOwnPed({ambHuman, {0, 90, 0}, 0}, Verdict::KeepInView, v), "count: not beyond 80 m");
    Check(KeyHash("") == 0 && KeyHash("freeroam_test|12.5") != KeyHash("freeroam_test|13.0"), "world keys hashed");
}

// ---- replay of the 2026-10-20 guest sessions ---------------------------------------------------------------------------
// The guest's PDL / TRL records at 1 Hz (tests/data/guest_world_1020.h): when he came into the world, his own ambient
// humans (31 in A, 24 in B), ped_sync's copy spawner state, his own traffic cars within 70 m. The policy runs at 30 Hz
// over the same timeline (5 s of loading screen before the world, the session ending at END); at each record the
// modelled entities are judged: the own humans (half in view), the own cars (in view), and the copy spawner (from its
// state: being spawned = a crewed car 40 m below us, its factory loading; ready = registered and fixed).
void TestReplay() {
    for (const gw1020::Session& s : gw1020::kSessions) {
        Policy p;
        const View v = Camera();
        double firstOff = -1;
        int restores = 0, applies = 0, spawnerHidden = 0, spawnerJudged = 0;
        int lateOwnPeds = 0, lateOwnCars = 0, oldLateCars = 0, rowsLate = 0;
        int idx = 0;
        HideMode mode = HideMode::None;
        double inWorldAt = -1;
        bool prevObtain = false;
        for (double t = s.inWorld - 5.0; t <= s.end + 3.0; t += 1.0 / 30.0) {
            Frame f;
            f.now = t;
            f.active = t <= s.end;
            f.phase = t < s.inWorld ? Phase::Loading : t <= s.end + 1.0 ? Phase::InWorld : Phase::None;
            f.worldKey = f.phase == Phase::InWorld ? 99 : 0;
            f.havePos = f.phase == Phase::InWorld;
            const Actions a = p.Tick(f);
            if (a.switchOff) {
                if (firstOff < 0) firstOff = t;
                if (a.readBudget) p.OnBudgetRead(60);
                p.OnSwitched(true, true, true);
                ++applies;
            }
            if (a.restore) ++restores;
            mode = a.hide;
            if (f.phase == Phase::InWorld && inWorldAt < 0) inWorldAt = t;
            // the records of this moment
            while (idx < s.n && s.rows[idx].t <= t) {
                const gw1020::Row& r = s.rows[idx++];
                if (!r.inWorld || f.phase != Phase::InWorld || !f.active) continue;
                const bool late = t - inWorldAt >= 10.0;
                // own humans: those the game had when the world came, still there now unless hidden at once
                int shown = 0;
                const int own = (t - inWorldAt < 1.0) ? r.amb : 0;  // the first record carries the world's humans
                for (int i = 0; i < own; ++i) {
                    const Vec3 at = (i % 2) ? Vec3{0, 20.0f + i, 0} : Vec3{0, -20.0f - i, 0};
                    const Verdict vd = Judge({kHuman | kActive | kAmbient, at, 0}, mode, v);
                    if (vd != Verdict::Hide && CountsAsOwnPed({kHuman | kActive | kAmbient, at, 0}, vd, v)) ++shown;
                }
                if (late) lateOwnPeds += shown;
                // own cars within 70 m (0.16 kept them driving for minutes: ClearLocal hid only beyond 70 m of every
                // player): here one in view stays only while in view; modelled behind us half the records
                for (int i = 0; i < r.mine; ++i) {
                    const Vec3 at = (idx % 2) ? Vec3{0, 30, 0} : Vec3{0, -30, 0};
                    const Verdict vd = Judge({kCar | kActive | kGeneratorDriver, at, 10}, mode, v);
                    if (late && vd != Verdict::Hide) ++lateOwnCars;
                    if (late) ++oldLateCars;
                }
                if (late) ++rowsLate;
                // the copy spawner
                const bool obtain = r.fac == 1, spawning = prevObtain && r.fac != 1;
                prevObtain = obtain;
                if (spawning) {  // the second after an obtain: the car is there, ped_sync does not know it yet
                    ++spawnerJudged;
                    if (Judge({kCar | kActive | kGeneratorDriver | kFactoryBusy, {-3, 0, -40}, 0}, mode, v) == Verdict::Hide)
                        ++spawnerHidden;
                    if (Judge({kCar | kActive | kGeneratorDriver | kFresh, {-3, 0, -40}, 0}, mode, v) == Verdict::Hide)
                        ++spawnerHidden;
                }
                if (r.fac == 3) {
                    ++spawnerJudged;
                    if (Judge({kCar | kActive | kGeneratorDriver | kPedSpawner | kFixed, {-3, 0, -40}, 0}, mode, v) ==
                        Verdict::Hide)
                        ++spawnerHidden;
                }
            }
        }
        const std::string tag = std::string("session ") + s.name + ": ";
        char d[160];
        std::snprintf(d, sizeof d, "off %.2f s before the world (0.16: %.2f s after it)", s.inWorld - firstOff,
                      s.off016 - s.inWorld);
        Check(firstOff >= 0 && firstOff <= s.inWorld, (tag + "switched off on the first loading tick").c_str(), d);
        Check(restores == 1, (tag + "everything back once at the session end").c_str(), std::to_string(restores));
        std::snprintf(d, sizeof d, "%d applies over %.0f s", applies, s.end - s.inWorld);
        Check(applies >= 2, (tag + "re-applied while in the world (every 5 s)").c_str(), d);
        std::snprintf(d, sizeof d, "%d judged", spawnerJudged);
        Check(spawnerJudged > 0 && spawnerHidden == 0, (tag + "ped_sync's copy spawner never hidden").c_str(), d);
        std::snprintf(d, sizeof d, "0 of the world's %d humans shown 10 s in", s.rows[0].amb);
        Check(lateOwnPeds == 0, (tag + "world.guest_own_peds 0 after 10 s").c_str(), d);
        std::snprintf(d, sizeof d, "own cars near shown %d of %d record-cars (0.16: all %d; in view only)", lateOwnCars,
                      oldLateCars, oldLateCars);
        Check(lateOwnCars <= oldLateCars / 2 + 1, (tag + "own cars near: hidden whenever out of view (modelled half behind us)").c_str(), d);
    }
}

}  // namespace

int main() {
    TestFirstWorld();
    TestJoinInWorld();
    TestReload();
    TestNeverOnHost();
    TestRestore();
    TestBudgetUnknown();
    TestFallback();
    TestOtherOn();
    TestView();
    TestVerdicts();
    TestCounts();
    TestReplay();
    std::printf("%s (%d failure%s)\n", g_failures ? "FAILED" : "OK", g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
