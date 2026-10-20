// respawn_test: how long a puppet the game removed is away (sync_compare foot.respawn_s, target <= 5 s), from the glue
// itself (src/game/player_sync.lua, embedded as kPlayerSyncLua by build.cmd) and the times the 2026-10-20 one-PC runs
// logged for each step. The loss (tools/onepc/losstest.lua; the real Free Ride garage does the same): "puppet 0 is
// gone (removed by the game)", its known profile asked of the same spawner at once, which never gets it ready (the
// spawner is stuck after the game removed one of its humans): after RELOAD_TIMEOUT "not ready: the spawner is stuck",
// "factory reset", a fresh factory obtained and spawned, the puppet made by it. Runs 14:36 / 15:00: gone -> stuck
// 4.015 / 4.010 s, reset -> obtain 1.001 / 1.001 s, obtain -> factory ready 0.012 / 0.015 s, ready -> spawned 0.007 /
// 0.077 s; back after 5.03 / 5.10 s (FAIL). A healthy spawner has a known profile ready at once: 0.00-0.06 s (the
// first puppets once their factory was ready), a character change on the host 0.06 / 0.1 s. Checks: the timeline
// of the old values gives what the runs measured, the glue's values now give clearly under 5 s, and the stuck
// timeout leaves a healthy reload ten times its longest seen. Prints PASS/FAIL per check; exits non-zero on any
// failure. Built by build.cmd (out\respawn_test.exe).
#include <cstdio>
#include <cstdlib>
#include <string>

#include "player_sync_lua.h"

namespace {

int g_failures = 0;

void Check(bool ok, const char* name, const std::string& detail = {}) {
    std::printf("%s  %-70s %s\n", ok ? "PASS" : "FAIL", name, detail.c_str());
    std::fflush(stdout);
    if (!ok) ++g_failures;
}

std::string Fmt(const char* f, double a, double b = 0, double c = 0) {
    char buf[200];
    std::snprintf(buf, sizeof buf, f, a, b, c);
    return buf;
}

// "local NAME = <number>" in the glue; -1 when missing
double LuaNumber(const std::string& src, const char* name) {
    const std::string key = std::string("local ") + name + " =";
    const size_t at = src.find(key);
    if (at == std::string::npos) return -1;
    return std::strtod(src.c_str() + at + key.size(), nullptr);
}

bool Has(const std::string& src, const char* text) { return src.find(text) != std::string::npos; }

// The measured steps (s, the slower of the two runs)
constexpr double kObtainToReady = 0.015;   // a fresh factory: obtain -> ready
constexpr double kReadyToSpawned = 0.077;  // ready -> the puppet made (its profile loaded at once)
constexpr double kTick = 1.0 / 60;         // the glue sees each step one script tick later at most
constexpr double kHealthyReload = 0.1;     // the longest a working spawner took for a known / a new profile

// gone -> spawned again: the stuck wait, then the later of the fresh factory's start and the puppet's retry, then
// the factory and the puppet
double BackAfter(double reloadTimeout, double factoryStart, double puppetRetry) {
    return reloadTimeout + kTick + (factoryStart > puppetRetry ? factoryStart : puppetRetry) + kObtainToReady +
           kReadyToSpawned + 2 * kTick;
}

}  // namespace

int main() {
    const std::string lua = kPlayerSyncLua;
    // the old glue (feat/host-authority 3e0f505): 4 s, the factory 1 s after a reset, the puppet's retry 1 s
    const double old = BackAfter(4.0, 1.0, 1.0);
    Check(old > 5.0 && old < 5.2, "model: the old values give what the one-PC runs measured (5.03 / 5.10 s)",
          Fmt("%.2f s", old));

    const double reload = LuaNumber(lua, "RELOAD_TIMEOUT");
    Check(reload > 0, "glue: RELOAD_TIMEOUT found", Fmt("%.2f s", reload));
    // a stuck spawner's reset starts the fresh factory at once and retries the puppet at once
    const bool factoryNow =
        Has(lua, "M.fac = { st = \"none\", t = soon and 0 or -1 }") &&
        Has(lua, "M.FactoryReset(known and \"the spawner is stuck\" or \"a profile did not load\", known)");
    const bool retryNow = Has(lua, "pp.st, pp.retryT = \"queued\", known and M.now or M.now + 1");
    Check(factoryNow, "glue: a stuck spawner's reset obtains the fresh factory at once");
    Check(retryNow, "glue: ... and the puppet waits for nothing else");
    const double now = BackAfter(reload, factoryNow ? 0.0 : 1.0, retryNow ? 0.0 : 1.0);
    Check(now <= 2.5, "loss: the puppet is back clearly under 5 s (foot.respawn_s)",
          Fmt("%.2f s (was %.2f s)", now, old));
    Check(reload >= 10 * kHealthyReload, "a working spawner is never taken for stuck: 10x its slowest reload",
          Fmt("timeout %.2f s, slowest healthy reload %.2f s", reload, kHealthyReload));
    // the unknown profile (a character this game build does not have) still gets its long load
    const double load = LuaNumber(lua, "LOAD_TIMEOUT");
    Check(load >= 10, "an unknown profile still has its long load before it is bad", Fmt("%.0f s", load));
    std::printf("%s (%d failure%s)\n", g_failures ? "FAILED" : "ALL PASSED", g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
