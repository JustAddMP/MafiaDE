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

#include <string>
#include <vector>

namespace mc::game {

class PlayerSync {
public:
    static void Install();  // once, after the Lua bridge: registers the main-thread tick

    // render thread
    static void Attach(net::Session& session);  // session created: registers the message handler
    static void Detach();                        // session gone: all puppets are removed
    static void OnPlayerLeft(net::PlayerId id);
    // Sends the local state at 20 Hz while the session is active. enterDown: the enter-vehicle key/button is held
    // in our focused window (E / pad Y; a press next to a car another player drives rides along as passenger).
    // aimDown: the aim control is held (right mouse button / pad left trigger); shown on our puppet while the player
    // has a gun in hand. The host also sends its time of day and weather (WorldEnv) from here.
    static void Tick(float dt, bool enterDown, bool aimDown);
    // Last known health (0..100) of a player (local or remote), -1 if unknown. Any thread.
    static int Health(net::PlayerId id);
    // Our character (docs/protocol.md "PlayerModel"): the `model` setting, "" = the default for our player id.
    static std::string Model();
    // Picks another character (CO-OP panel): saved as `model =` in MafiaCoop.ini, our own player switches at once
    // (live preview while browsing), and everyone else's puppet of us follows (PlayerModel message).
    static void SetModel(const std::string& model);
    static void GetStatus(std::vector<render::OverlayLine>& out);
};

}  // namespace mc::game
