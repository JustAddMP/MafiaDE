#pragma once
// Explosions, fires and blown-up cars on every game (0.16; docs/game-api.md 23, docs/protocol.md "Explosions").
// The game where an explosion really happens and that is authoritative for its cause sends it; the others show a
// visual-only explosion and undo any damage it does there; damage is decided by the sender's game alone and travels
// through the existing hit paths (Hit 0x0400, NpcHit 0x0410, HostNpcHit 0x0422). Logic: game/explosion_model.h
// (unit-tested); game side: explosion_sync.lua (MafiaCoop.Boom).
// Threads: messages and sending on the render thread; Lua, the detector and the damage zones on GameMainThread (the
// game's DAMAGE / HEAR_SOUND callbacks call the mc_boom_* functions synchronously there); a mutex in between.
#include "net/session.h"
#include "render/overlay.h"

#include <vector>

namespace mc::game {

class ExplosionSync {
public:
    static void Install();  // once, after the Lua bridge (registers the tick and the mc_boom_* functions)
    static void Attach(net::Session& session);
    static void Detach();
    static void OnPlayerLeft(net::PlayerId id);
    static void Tick(float dt);
    static void GetStatus(std::vector<render::OverlayLine>& out);
};

}  // namespace mc::game
