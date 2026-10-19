#pragma once
// Ambient pedestrians in co-op (docs/protocol.md "Pedestrians", docs/sync-architecture.md package `ambient-peds`).
// The ambient world has one authority per place (core net/authority.h RegionAuthority: the host within 250 m of him,
// elsewhere the nearest player's game, with hysteresis): that game simulates the pedestrians there and sends the ones
// near each other player (interest: 50 m, 10 Hz near, 3 Hz at 50 m) as compact time-stamped states, with reliable
// spawn / gone / death messages (a guest authority sends to the host, and to other guests through the host's relay).
// A receiver turns its own ambient humans near the authority's pedestrians into their replicas (one-to-one, nearest
// first; a far one only when nobody looks), covers a shortfall with spawned copies (at most 10), hides its surplus
// while nobody looks, and walks each replica with core's replica::Stream + LocomotionController (the correction rules
// of Corrector::ForNpc). A replica killed here kills the authority's pedestrian (PedKill); deaths follow the
// authority. The game-free logic is game/ped_model.h (tests/ped_test.cpp); the game side is ped_sync.lua.
// Threads: messages and sending on the render thread; Lua and the model on GameMainThread (tick callback); a mutex.
#include "net/session.h"
#include "render/overlay.h"

#include <vector>

namespace mc::game {

class PedSync {
public:
    static void Install();  // once, after the Lua bridge (after NpcSync and TrafficSync: its Lua reads their tables)
    static void Attach(net::Session& session);
    static void Detach();
    static void OnPlayerLeft(
        net::PlayerId id);  // left or dropped: his pedestrians' replicas go, nothing is sent to him
    static void Tick(float dt);
    static void GetStatus(std::vector<render::OverlayLine>& out);
};

}  // namespace mc::game
