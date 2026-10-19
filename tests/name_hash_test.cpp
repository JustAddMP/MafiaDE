// name_hash_test: the game's name hash (src/game/name_hash.h, Lua's mc_fnv) against values read statically from the
// game: the profile hashes in sds_retail/combinables/spawnprofiles.sds, and that the Lua glue uses exactly the
// profile names pinned here (traffic_sync.lua crews, npc_sync.lua copy fallbacks). Prints PASS/FAIL per check; exits
// non-zero on any failure.
#include "game/name_hash.h"

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

using namespace mc::game;

namespace {

int g_failures = 0;

void Check(bool ok, const char* name, const std::string& detail = {}) {
    std::printf("%s  %-64s %s\n", ok ? "PASS" : "FAIL", name, detail.c_str());
    std::fflush(stdout);
    if (!ok) ++g_failures;
}

void Hash(std::string_view name, uint64_t want) {
    const uint64_t got = NameHash(name);
    Check(got == want, std::string(name).c_str(), std::to_string(got));
}

// The glue's source next to this test's (tests\..\src\game\<file>); empty when it cannot be read.
std::string Glue(const char* file) {
    std::string here = __FILE__;
    const size_t slash = here.find_last_of("\\/");
    const std::string root = slash == std::string::npos ? std::string(".") : here.substr(0, slash);
    std::ifstream f(root + "/../src/game/" + file, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

}  // namespace

int main() {
    // values found in spawnprofiles.sds (u64, little endian) for these names, 2026-10-19
    Hash(profiles::kTommy, 4280710535630713749ull);
    Hash(profiles::kTrafficCivilian, 6892933706456221844ull);
    Hash(profiles::kCivilianMan, 8309506601229459415ull);
    Hash(profiles::kWorkerMan, 10701541148023418535ull);
    Hash(profiles::kPolice, 15910925101550496629ull);
    Hash(profiles::kPoliceShotgun, 5838331429741412184ull);
    Check(NameHash("") == 0xcbf29ce484222325ull, "empty name: the FNV offset basis");
    static_assert(NameHash("a") == ((0xcbf29ce484222325ull * 0x100000001b3ull) ^ 'a'), "FNV-1: multiply, then xor");

    const std::string traffic = Glue("traffic_sync.lua"), npc = Glue("npc_sync.lua");
    Check(!traffic.empty() && !npc.empty(), "the Lua glue sources can be read");
    for (std::string_view p : {profiles::kTrafficCivilian, profiles::kCivilianMan, profiles::kWorkerMan,
                               profiles::kPolice, profiles::kPoliceShotgun, profiles::kTommy})
        Check(traffic.find("\"" + std::string(p) + "\"") != std::string::npos,
              ("traffic_sync.lua crews use " + std::string(p)).c_str());
    for (std::string_view p : {profiles::kTrafficCivilian, profiles::kPolice})
        Check(npc.find("\"" + std::string(p) + "\"") != std::string::npos,
              ("npc_sync.lua copy fallbacks use " + std::string(p)).c_str());
    // the old names that are not profiles of this game (no hash in spawnprofiles.sds) are gone
    Check(traffic.find("\"police_lvl1\"") == std::string::npos && traffic.find("\"civilian_white_crew\"") == std::string::npos,
          "traffic_sync.lua no longer asks for the crew preset names");

    std::printf("%s (%d failure%s)\n", g_failures ? "FAILED" : "OK", g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
