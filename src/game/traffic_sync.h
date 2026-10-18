#pragma once
// Phase 4 (B4): ambient traffic and police in co-op. The host's traffic is authoritative: his game reports its moving
// traffic and police cars near any guest (traffic_sync.lua), the host sends each guest the cars near him (net ids,
// spawn / despawn / packed states, core's InterestManager, game/traffic_model.h), and a guest near the host switches
// his own traffic generators off and shows copies of those cars (a crewed car each), driven with core's
// VehicleController like a remote player's car. Mission cars and player cars are not traffic (player_sync / npc_sync
// handle them). docs/protocol.md "Traffic", docs/research-world-sync.md B4.
// Threads: messages and sending on the render thread; Lua on GameMainThread (tick callback); a mutex in between.
#include "net/session.h"
#include "render/overlay.h"

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
    // host, render thread: the net id of our traffic car `key` (its entity id) and back; 0 = not shared (car damage)
    static uint16_t HostNetOf(uint32_t key);
    static uint32_t HostKeyOf(uint16_t net);
};

}  // namespace mc::game
