#pragma once
// Doors are the same on every game: a door a player opens, closes, kicks open, unlocks or lockpicks changes on the
// other games too (his puppet plays the kick / lock pull, player_actions.h), the host's mission scripts decide the
// story doors (lock state) for everybody, and a guest gets the doors near him on join / reconnect / a new world.
// The game side is door_sync.lua (door table scan, state poll, attribution, applying); the rules are
// game/door_model.h. docs/protocol.md "Doors", docs/game-api.md 20.
// Threads: messages and sending on the render thread; Lua on GameMainThread (tick callback); a mutex in between.
#include "net/session.h"
#include "render/overlay.h"

#include <vector>

namespace mc::game {

class DoorSync {
public:
    static void Install();  // once, after the Lua bridge
    static void Attach(net::Session& session);
    static void Detach();
    static void OnReconnected();  // guest: back in the host's session (the snapshot is asked for again)
    static void Tick(float dt);
    static void GetStatus(std::vector<render::OverlayLine>& out);
};

}  // namespace mc::game
