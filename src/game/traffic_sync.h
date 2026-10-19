#pragma once
// Phase 4 (B4) and the region authority (docs/sync-architecture.md principle 1): ambient traffic and police in co-op.
// Every game is the authority for the traffic its own game spawned: it reports its moving traffic and police cars near
// any other player (traffic_sync.lua), and sends each other player the cars near him (net ids, spawn / despawn /
// packed states, core's InterestManager, game/traffic_model.h; the host with 0x0220-0x0222, a guest with 0x0224-0x0226
// through core's relay). Every game shows copies of every other game's cars near it (a crewed car each, driven with
// core's VehicleController and Corrector like a remote player's car, interpolated by core's replica::Stream per
// authority). Whose region a game is in decides only whether its own spawning runs: a guest near the host (or near a
// lower-id guest far from the host) switches his generators off and his own leftover cars are shared until they leave.
// Mission cars and player cars are not traffic (player_sync / npc_sync handle them). docs/protocol.md "Traffic".
// Threads: messages and sending on the render thread; Lua on GameMainThread (tick callback); a mutex in between.
#include "net/session.h"
#include "render/overlay.h"

#include <cstdint>
#include <vector>

namespace mc::game {

class TrafficSync {
public:
    static void Install();  // once, after the Lua bridge
    static void Attach(net::Session& session);
    static void Detach();
    static void OnPlayerLeft(net::PlayerId id);
    static void Tick(float dt);
    static void GetStatus(std::vector<render::OverlayLine>& out);
    // Host, render thread: the traffic net id of our car with entity id `key` (0: not shared traffic). Used by
    // explosion_sync (CarExploded for a traffic car).
    static uint16_t NetOfKey(uint32_t key);
    // host, render thread: the net id of our traffic car `key` (its entity id) and back; 0 = not shared (car damage).
    // 0 on a guest (a guest's own traffic has no car damage sync yet).
    static uint16_t HostNetOf(uint32_t key);
    static uint32_t HostKeyOf(uint16_t net);
};

}  // namespace mc::game
