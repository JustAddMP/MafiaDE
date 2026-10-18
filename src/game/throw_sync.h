#pragma once
// The flight of thrown explosives on every game (0.16 "grenade arc"; docs/game-api.md 23 "The flight",
// docs/protocol.md "Explosions": Throw 0x0242). The thrower's game logs the aim while its player
// lines up a throw, then measures the real projectile's launch in the first 0.3 s and sends it once; the others
// simulate the flight against their own world (ray casts, bounces, rolling) and draw it (a marker and a fading trail
// over the game, from our overlay: nothing is spawned in their world, so it cannot hurt anyone) until the Explosion
// (0x0240) naming the throw arrives. Logic: game/throw_model.h (unit-tested); game side: throw_arc.lua (MafiaCoop.Arc).
// Threads: messages, sending and drawing on the render thread; Lua, the tracker and the simulation (ray casts) on
// GameMainThread; a mutex between.
#include "game/explosion_model.h"
#include "net/session.h"
#include "render/overlay.h"

#include <vector>

namespace mc::game {

class ThrowSync {
public:
    static void Install();  // once, after the Lua bridge (registers the tick and the mc_arc_* functions)
    static void Attach(net::Session& session);
    static void Detach();
    static void OnPlayerLeft(net::PlayerId id);
    static void Tick(float dt);
    static void GetStatus(std::vector<render::OverlayLine>& out);
    static void Draw(render::DrawList& d, float w, float h);  // the flights' markers (render thread, DrawHud)

    // ExplosionSync: another game's Explosion arrived (render thread): the flight it names is over
    static void OnExplosion(uint32_t from, uint32_t throwSeq, const boom::Vec3& pos);
    // ExplosionSync: our own explosion was decided (main thread) and matched with our throw `throwSeq`
    static void OnOwnExplosion(uint32_t throwSeq, const boom::Vec3& pos, double sinceThrow);
};

}  // namespace mc::game
