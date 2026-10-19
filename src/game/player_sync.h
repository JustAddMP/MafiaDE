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
    // has a gun in hand.
    static void Tick(float dt, bool enterDown, bool aimDown);
    // Last known health (0..100) of a player (local or remote), -1 if unknown. Any thread.
    static int Health(net::PlayerId id);
    // Our character (docs/protocol.md "PlayerModel"): the `model` setting, "" = the default for our player id.
    static std::string Model();
    // Picks another character (CO-OP panel): saved as `model =` in MafiaCoop.ini, our own player switches as soon as
    // the game runs (the pause menu holds the game), and everyone else's puppet of us follows (PlayerModel message)
    // once the pick has stayed for a second or the panel is closed (game/model_change.h: not one puppet per step).
    static void SetModel(const std::string& model);
    // A frame of a panel with the CHARACTER rows (core asks GameModule::PanelOptions every frame while it is open).
    static void PanelShown();
    static void GetStatus(std::vector<render::OverlayLine>& out);

    // The local player is in a loaded world (not the main menu or a loading screen), and the loaded save id
    // ("freeroam_test" in Free Ride, "checkpoint_..." in the story; "" when unknown). Any thread; updated twice a
    // second by the Lua glue, with or without a session.
    static bool InWorld();
    static std::string SaveId();
    // "PAULIE / SUIT" for the lobby: the character player `id` picked (ours for the local player).
    static std::string CharacterName(net::PlayerId id);

    // Name tags (docs/game-api.md 13): every remote player whose puppet's head is in front of the camera this tick.
    struct NameTag {
        std::string name;
        float x, y;  // -1..1, y up
        float dist;  // metres from the camera
    };
    static void GetTags(std::vector<NameTag>& out);
};

}  // namespace mc::game
