#pragma once
// The flight of thrown explosives on every game (0.16 "grenade arc"; docs/game-api.md 20 "The flight",
// docs/protocol.md "Explosions": Throw 0x0242, ThrowFlight 0x0243). The thrower's game logs the aim while its player
// lines up a throw, then follows the real projectile from the release and sends its launch and its flight; the
// others draw it (a marker and a fading trail over the game, from our overlay: nothing is spawned in their world, so
// it cannot hurt anyone) until the Explosion (0x0240) naming the throw arrives. Logic: game/throw_model.h
// (unit-tested); game side: throw_arc.lua (MafiaCoop.Arc).
// Threads: messages, sending and drawing on the render thread; Lua and the tracker on GameMainThread; a mutex between.
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
