#pragma once
// The guest's own world off (docs/host-authority.md work package A): the glue around game/guest_world.h. On
// GameMainThread (a Lua tick callback) it runs guest_world.lua, whose bindings mc_gw_tick / mc_gw_view / mc_gw_judge /
// mc_gw_budget ask the pure Policy and the hide verdict here; the render thread only tells it whether the session runs
// and whether we host. No messages. Trace record GW (1 Hz, guest). docs/game-api.md 26, docs/sync-trace.md.
// ini: guest_world (default 1; 0 leaves the guest's own world on), guest_world_peds (budget / block / off; default
// budget), guest_ped_budget (the value the pedestrian budget is put back to when the game's could not be read; default
// -1: left to the next world load; the host logs its own value once per world).
#include "net/session.h"
#include "render/overlay.h"

#include <vector>

namespace mc::game {

class GuestWorldSync {
public:
    static void Install();  // once, after the Lua bridge (and after the other glue: its Lua reads their tables)
    static void Attach(net::Session& session);
    static void Detach();
    static void Tick(float dt);  // render thread
    static void GetStatus(std::vector<render::OverlayLine>& out);
};

}  // namespace mc::game
