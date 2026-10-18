// fail_flow_test: the co-op answer to a failed mission (src/game/fail_flow.h) without the game. Each case drives one
// or two FailFlow instances (a host and a guest) through a failure the way mission_sync does (GameOver hook, ticks,
// FailVerdict / MissionFail messages) and checks that every game loads exactly once. Exits non-zero on any failure.
#include <cstdio>
#include <string>
#include <vector>

#include "game/fail_flow.h"

namespace {

using mc::game::FailFlow;
using mc::game::FailRole;
using mc::game::FailVerdict;
using Kind = FailFlow::ActionKind;
using State = FailFlow::State;

int g_failures = 0;

void Check(bool ok, const char* name, const std::string& detail = {}) {
    std::printf("%s  %-64s %s\n", ok ? "PASS" : "FAIL", name, detail.c_str());
    std::fflush(stdout);
    if (!ok) ++g_failures;
}

int Count(const std::vector<FailFlow::Action>& a, Kind k) {
    int n = 0;
    for (const auto& x : a)
        if (x.kind == k) ++n;
    return n;
}

// One game: its flow plus what mission_sync would do with the actions (counts loads).
struct Game {
    FailFlow flow;
    FailRole role;
    bool inWorld = true;
    int quits = 0, loads = 0, reports = 0, hostFailedSent = 0;
    uint32_t lastReportSeq = 0;
    std::vector<FailFlow::Action> all;

    explicit Game(FailRole r) : role(r) {}

    void Apply() {
        for (const auto& a : flow.TakeActions()) {
            all.push_back(a);
            switch (a.kind) {
            case Kind::SendHostFailed: ++hostFailedSent; break;
            case Kind::SelfReload: ++quits; ++loads; break;  // quit + load our checkpoint
            case Kind::SendReport: ++reports; lastReportSeq = a.seq; break;
            case Kind::Quit: ++quits; break;
            case Kind::LoadHostPoint: ++loads; break;
            case Kind::ReloadInWorld: ++quits; ++loads; break;
            }
        }
        flow.TakeLog();
    }
    void Tick(double now, bool hostInWorld = true, bool hostAtPoint = true) {
        FailFlow::View v;
        v.inWorld = inWorld;
        v.hostKnown = true;
        v.hostInWorld = hostInWorld;
        v.hostAtStoryPoint = hostAtPoint;
        flow.Update(v, now);
        Apply();
    }
    FailFlow::LocalFailResult Fail(uint32_t reason, double now, bool canReload = true) {
        auto r = flow.OnLocalFail(role, canReload, reason, now);
        Apply();
        return r;
    }
};

void HostChapterFails() {
    Game h(FailRole::HostChapter);
    h.Tick(0);
    auto r = h.Fail(59310003, 1);
    Check(r.skipGame && r.fresh, "chapter host: GameOver skipped, a new failure");
    Check(h.flow.GetState() == State::HostReloading, "chapter host: reloading");
    Check(h.hostFailedSent == 1, "chapter host: guests told once");
    Check(h.loads == 1, "chapter host: one reload asked for");
    // the game raises GameOver again in the same failure
    auto r2 = h.Fail(59310003, 1.2);
    Check(r2.skipGame && !r2.fresh, "chapter host: repeat skipped, not a new failure");
    Check(h.loads == 1 && h.hostFailedSent == 1, "chapter host: still one load, one notice");
    h.Tick(2);
    h.inWorld = false;
    h.Tick(3);
    h.Tick(20);
    Check(h.flow.GetState() == State::HostReloading, "chapter host: waits while out of the world");
    h.inWorld = true;
    h.Tick(30);
    Check(h.flow.GetState() == State::Idle, "chapter host: idle once back in the world");
    auto r3 = h.Fail(7, 40);
    Check(r3.fresh && h.loads == 2, "chapter host: a later failure is a new one");
}

void HostChapterWithoutCheckpoint() {
    Game h(FailRole::HostChapter);
    h.Tick(0);
    auto r = h.Fail(5, 1, false);
    Check(!r.skipGame && r.fresh, "chapter host without checkpoint: the game's flow runs");
    Check(h.loads == 0 && h.hostFailedSent == 1, "chapter host without checkpoint: no reload, guests told");
    Check(h.flow.GetState() == State::HostGameFail, "chapter host without checkpoint: game fail state");
    auto r2 = h.Fail(5, 1.3, false);
    Check(!r2.skipGame && !r2.fresh, "chapter host without checkpoint: repeats left to the game");
}

void StoryHost() {
    Game h(FailRole::HostStory);
    h.Tick(0);
    auto r = h.Fail(11, 1);
    Check(!r.skipGame && r.fresh, "story host: fail screen kept");
    Check(h.hostFailedSent == 1 && h.loads == 0, "story host: guests told, no reload of ours");
    Check(h.flow.Decide(true, true) == FailVerdict::Wait, "story host at the fail screen: a guest's report -> WAIT");
    h.Tick(100);  // the fail screen stays up
    Check(h.flow.GetState() == State::HostGameFail, "story host: still failing at the fail screen");
    h.inWorld = false;  // Load Last Checkpoint
    h.Tick(101);
    h.inWorld = true;
    h.Tick(120);
    Check(h.flow.GetState() == State::Idle, "story host: idle after the checkpoint loaded");
}

void NotInCoopStory() {
    Game g(FailRole::None);
    g.Tick(0);
    auto r = g.Fail(3, 1);
    Check(!r.skipGame && !r.fresh && g.flow.GetState() == State::Idle && g.all.empty(),
          "no co-op story: the game's own flow, nothing done");
}

// Guest fails alone, host plays on: REJOIN, one load.
void GuestFailsAloneRejoin() {
    Game h(FailRole::HostChapter), g(FailRole::Guest);
    h.Tick(0);
    g.Tick(0);
    auto r = g.Fail(42, 1);
    Check(r.skipGame && r.fresh, "guest: GameOver skipped");
    Check(g.reports == 1 && g.quits == 1, "guest: reported and leaving the world");
    Check(g.flow.HoldsFollow(), "guest: follow held while asking");
    const FailVerdict v = h.flow.Decide(true, true);
    Check(v == FailVerdict::Rejoin, "host in his world at his point: REJOIN");
    g.flow.OnVerdict(v, g.lastReportSeq, 42, 1.3);
    g.Apply();
    Check(g.flow.GetState() == State::GuestRejoin, "guest: rejoining");
    g.Tick(1.5);  // still in the world (the quit takes a second)
    Check(g.loads == 0, "guest: no load while still in the failed world");
    g.inWorld = false;
    g.Tick(2.5);
    Check(g.loads == 1 && g.flow.GetState() == State::GuestLoading && !g.flow.HoldsFollow(),
          "guest: out of the world -> load the host's point, follow free");
    g.Tick(10);
    g.inWorld = true;
    g.Tick(30);
    Check(g.flow.GetState() == State::Idle && g.loads == 1 && g.quits == 1, "guest: one quit, one load, idle again");
    Check(h.loads == 0 && h.hostFailedSent == 0, "host: untouched by the guest's failure");
}

// Host fails: guests told, they leave at once and load the host's checkpoint once he is back: one load each.
void HostFailsGuestFollows() {
    Game h(FailRole::HostChapter), g(FailRole::Guest);
    h.Tick(0);
    g.Tick(0);
    h.Fail(9, 1);
    Check(h.hostFailedSent == 1, "host failed: notice sent");
    g.flow.OnVerdict(FailVerdict::HostFailed, 0, 9, 1.1);
    g.Apply();
    Check(g.flow.GetState() == State::GuestWaitHost && g.quits == 1, "guest: waits for the host, leaving the world");
    g.Tick(1.2, true, true);  // the host's MissionState still says in the world (his quit is under way)
    Check(g.loads == 0, "guest: no load before the host left his world");
    g.inWorld = false;
    h.inWorld = false;
    g.Tick(3, false, false);
    h.Tick(3);
    g.Tick(20, false, false);
    Check(g.loads == 0 && g.flow.HoldsFollow(), "guest: waits at the menu while the host loads");
    h.inWorld = true;
    h.Tick(25);
    g.Tick(25, true, false);  // in the world, quest not running yet
    Check(g.loads == 0, "guest: not before the host's quest runs");
    g.Tick(27, true, true);
    Check(g.loads == 1 && g.flow.GetState() == State::GuestLoading, "guest: the host is back -> one load");
    g.inWorld = true;
    g.Tick(45);
    Check(g.flow.GetState() == State::Idle && h.flow.GetState() == State::Idle, "both idle afterwards");
    Check(h.loads == 1 && g.loads == 1 && g.quits == 1, "one load on each game");
}

// Both fail at once (a timer running out on both games): the guest's report gets WAIT, the host's notice comes too;
// still one load each.
void BothFailTogether() {
    Game h(FailRole::HostChapter), g(FailRole::Guest);
    h.Tick(0);
    g.Tick(0);
    g.Fail(59310003, 1.0);
    h.Fail(59310003, 1.05);
    const FailVerdict v = h.flow.Decide(true, true);
    Check(v == FailVerdict::Wait, "host failing himself: a guest's report -> WAIT");
    g.flow.OnVerdict(FailVerdict::HostFailed, 0, 59310003, 1.1);  // the broadcast first
    g.flow.OnVerdict(v, g.lastReportSeq, 59310003, 1.2);
    g.Apply();
    Check(g.flow.GetState() == State::GuestWaitHost, "guest: waiting for the host");
    // the guest's game raises GameOver again
    auto r = g.Fail(59310003, 1.5);
    Check(r.skipGame && !r.fresh && g.reports == 1, "guest: the repeat is skipped, one report");
    g.inWorld = false;
    h.inWorld = false;
    g.Tick(3, false, false);
    h.Tick(3);
    h.inWorld = true;
    h.Tick(25);
    g.Tick(26, true, true);
    g.inWorld = true;
    g.Tick(45);
    Check(h.loads == 1 && g.loads == 1 && g.quits == 1, "both failed: one load on each game");
}

// No answer from the host (an older host or a lost message): the guest goes back on his own after kAskTimeout.
void GuestNoVerdict() {
    Game g(FailRole::Guest);
    g.Tick(0);
    g.Fail(1, 1);
    g.inWorld = false;
    g.Tick(2);
    Check(g.loads == 0 && g.flow.GetState() == State::GuestAsking, "guest: asking, no load yet");
    g.Tick(1 + FailFlow::kAskTimeout + 0.5);  // -> rejoin
    g.Tick(1 + FailFlow::kAskTimeout + 0.6);  // -> out of the world: load
    Check(g.loads == 1, "guest: no verdict -> loads the host's point once");
}

// A stale verdict (another report's seq) is ignored.
void StaleVerdict() {
    Game g(FailRole::Guest);
    g.Tick(0);
    g.Fail(1, 1);
    g.flow.OnVerdict(FailVerdict::Rejoin, g.lastReportSeq + 5, 1, 1.2);
    g.Apply();
    Check(g.flow.GetState() == State::GuestAsking, "guest: a verdict for another report is ignored");
}

// The quit did not happen (still in the failed world 15 s after a REJOIN): reload from the world.
void GuestQuitFailed() {
    Game g(FailRole::Guest);
    g.Tick(0);
    g.Fail(1, 1);
    g.flow.OnVerdict(FailVerdict::Rejoin, g.lastReportSeq, 1, 1.2);
    g.Apply();
    g.Tick(10);
    Check(g.loads == 0, "guest: rejoin waits for the quit");
    g.Tick(17);
    Check(Count(g.all, Kind::ReloadInWorld) == 1 && g.loads == 1, "guest: still in the world -> reload from there");
}

// A failure while our load of the host's point is under way is the same failure; once in the world again a new one.
void GuestFailsAgainAfterLoad() {
    Game g(FailRole::Guest);
    g.Tick(0);
    g.Fail(1, 1);
    g.flow.OnVerdict(FailVerdict::Rejoin, g.lastReportSeq, 1, 1.2);
    g.inWorld = false;
    g.Tick(2);
    Check(g.flow.GetState() == State::GuestLoading, "guest: loading");
    auto r = g.Fail(1, 3);
    Check(!r.fresh && r.skipGame, "guest: GameOver during the load is the old failure");
    g.inWorld = true;
    g.Tick(4);  // in the world, before the load counts as done (age > 1)
    g.Tick(4.1);
    auto r2 = g.Fail(1, 4.2);
    Check(r2.fresh && g.reports == 2, "guest: GameOver in the new world is a new failure");
}

// The host's failure notice while the guest's own load is running: wait for the host, then one more load.
void HostFailsWhileGuestLoads() {
    Game g(FailRole::Guest);
    g.inWorld = false;
    g.Tick(0, true, true);
    g.flow.OnVerdict(FailVerdict::HostFailed, 0, 2, 1);
    g.Apply();
    Check(g.quits == 0 && g.flow.GetState() == State::GuestWaitHost, "guest out of the world: waits, no quit");
    g.Tick(2, false, false);
    g.Tick(20, true, true);
    Check(g.loads == 1, "guest: loads the host's point once he is back");
}

// The guest dies (or a reset) while the flow runs: Busy() tells mission_sync not to start a death reload.
void BusyAndReset() {
    Game g(FailRole::Guest);
    g.Tick(0);
    Check(!g.flow.Busy(), "idle: not busy");
    g.Fail(1, 1);
    Check(g.flow.Busy(), "after a failure: busy (a death adds no reload)");
    g.flow.Reset();
    Check(!g.flow.Busy() && g.flow.Notice().empty(), "reset: idle, no notice");
}

void Notices() {
    Game h(FailRole::HostChapter), g(FailRole::Guest);
    h.Tick(0);
    g.Tick(0);
    Check(h.flow.Notice().empty() && g.flow.Notice().empty(), "no notice while idle");
    h.Fail(1, 1);
    g.Fail(1, 1);
    Check(!h.flow.Notice().empty() && !g.flow.Notice().empty(), "a notice while a failure is handled");
}

void Timeouts() {
    Game g(FailRole::Guest);
    g.Tick(0);
    g.flow.OnVerdict(FailVerdict::HostFailed, 0, 1, 1);
    g.inWorld = false;
    g.Tick(2, false, false);
    g.Tick(2 + FailFlow::kGuestWaitTimeout + 1, false, false);
    Check(g.flow.GetState() == State::Idle && g.loads == 0, "guest: the host never came back -> idle, normal follow");
    Game h(FailRole::HostChapter);
    h.Tick(0);
    h.Fail(1, 1);
    h.Tick(1 + FailFlow::kHostReloadTimeout + 1);
    Check(h.flow.GetState() == State::Idle, "host: a reload that never finished -> idle");
}

}  // namespace

int main() {
    HostChapterFails();
    HostChapterWithoutCheckpoint();
    StoryHost();
    NotInCoopStory();
    GuestFailsAloneRejoin();
    HostFailsGuestFollows();
    BothFailTogether();
    GuestNoVerdict();
    StaleVerdict();
    GuestQuitFailed();
    GuestFailsAgainAfterLoad();
    HostFailsWhileGuestLoads();
    BusyAndReset();
    Notices();
    Timeouts();
    std::printf("%s (%d failure%s)\n", g_failures ? "FAILED" : "all passed", g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
