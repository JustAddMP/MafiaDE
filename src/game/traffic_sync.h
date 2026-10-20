#pragma once
// Phase 4 (B4) under host authority (docs/host-authority.md): ambient traffic and police in co-op. The host's game owns
// all of it: it reports its moving traffic and police cars near any guest (traffic_sync.lua) and sends each guest the
// cars near him (net ids, spawn / despawn / packed states 0x0220-0x0222, core's InterestManager, game/traffic_model.h).
// A guest's game shows copies of the host's cars near him (a crewed car each, driven with core's VehicleController and
// Corrector like a remote player's car, interpolated by core's replica::Stream) and sends nothing but TrafficBump; it
// scans and shares no cars of its own (its own spawning is off and its leftovers hidden: game/guest_world). The host's
// game shows nobody's traffic. The 0.16 region authority (a guest's own traffic, 0x0224-0x0227 through core's relay) is
// gone. Mission cars and player cars are not traffic (player_sync / npc_sync handle them). docs/protocol.md "Traffic".
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
    // 0 on a guest (a guest's game has no traffic of its own).
    static uint16_t HostNetOf(uint32_t key);
    static uint32_t HostKeyOf(uint16_t net);
};

}  // namespace mc::game
