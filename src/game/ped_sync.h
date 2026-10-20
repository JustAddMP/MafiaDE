#pragma once
// Ambient pedestrians in co-op (docs/protocol.md "Pedestrians", docs/host-authority.md WP-C "host-peds"). The host's
// game owns every ambient pedestrian: it sends each guest the ones within 80 m of him (leave 90 m, at most 48, nearest
// first; 10 Hz near, 3 Hz at 80 m; riders faster than 8 m/s not shared) as compact time-stamped states, with reliable
// spawn / gone / death messages. A guest sends no pedestrians (his own pedestrian spawning is off, guest_world) and
// shows every one of the host's with a spawned copy from a pool of up to 40 (a copy whose pedestrian went is hidden
// and given to the next one; new copies one profile load at a time, a rotation of six civilian looks), walked with
// core's replica::Stream + LocomotionController (the correction rules of Corrector::ForNpc). A copy killed on a guest
// kills the host's pedestrian (PedKill); deaths follow the host. The game-free logic is game/ped_model.h
// (tests/ped_test.cpp); the game side is ped_sync.lua.
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
    static void OnPlayerLeft(net::PlayerId id);  // left or dropped: nothing is sent to him (the host: our copies go)
    static void Tick(float dt);
    static void GetStatus(std::vector<render::OverlayLine>& out);
};

}  // namespace mc::game
