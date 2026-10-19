#pragma once
// The game's 64-bit name hash: FNV-1 (multiply, then xor) over the bytes of the name. Spawn profiles are known by it
// (a mission spawner's GetSpawnProfile returns it as a decimal number; SetSpawnProfile takes it back), and
// sds_retail/combinables/spawnprofiles.sds lists the profiles of the game by it (checked statically, 2026-10-19:
// the hashes our logs showed for mission spawners are in that table, and so are the hashes of the names below).
// Lua gets it as mc_fnv(text) (src/game/lua.cpp).
#include <cstdint>
#include <string_view>

namespace mc::game {

constexpr uint64_t NameHash(std::string_view s) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (char c : s) h = (h * 0x100000001b3ull) ^ static_cast<uint8_t>(c);
    return h;
}

// Spawn profiles used for copies whose own profile is not known (traffic_sync.lua crews, npc_sync.lua copies).
// Each one's hash is in spawnprofiles.sds; tests/name_hash_test.cpp pins the values.
namespace profiles {
constexpr std::string_view kTommy = "visuals/hero/game_main_tommy_gangster_trenchcoat";  // the puppet factory's
constexpr std::string_view kTrafficCivilian = "characters/traffic/traffic_base_ambient_combined";
constexpr std::string_view kCivilianMan = "characters/game/game_attractivemen";
constexpr std::string_view kWorkerMan = "characters/traffic/traffic_workers_male";
constexpr std::string_view kPolice = "characters/ai/police_scotch/ai_police_triggerman";
constexpr std::string_view kPoliceShotgun = "characters/ai/police_scotch/ai_police_shotgunner";
}  // namespace profiles

}  // namespace mc::game
