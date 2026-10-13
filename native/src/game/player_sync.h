#pragma once
// Phase 3: second player. Each peer samples its local player on GameMainThread (Lua tick), sends a 20 Hz unreliable
// PlayerState (app id 0x0100, docs/protocol.md) from the render thread, and drives one puppet per remote player
// from 100 ms-buffered, interpolated snapshots. The game-side work (spawning, MoveVec locomotion, weapons, copy
// cars) is the Lua glue in player_sync.lua, embedded into the dll. See docs/game-api.md for the game calls.
//
// Threads: Session callbacks and sending run on the render thread (Coop::Tick); snapshots and the local sample are
// handed over under one mutex to the main-thread tick, which is the only place that touches the game.
#include "net/session.h"
#include "render/overlay.h"

#include <vector>

namespace mc::game {

class PlayerSync {
public:
    static void Install();  // once, after the Lua bridge: registers the main-thread tick

    // render thread
    static void Attach(net::Session& session);  // session created: registers the message handler
    static void Detach();                        // session gone: all puppets are removed
    static void OnPlayerLeft(net::PlayerId id);
    static void Tick(float dt);                  // sends the local state at 20 Hz while the session is active
    static void GetStatus(std::vector<render::OverlayLine>& out);
};

}  // namespace mc::game
