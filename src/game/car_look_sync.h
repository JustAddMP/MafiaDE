#pragma once
// Car looks on every game: a copy of another player's car (player_sync) and of the host's traffic (traffic_sync)
// shows that car's colours, plate, window tint, interior, painting and tuning items (its body variant). The game-free
// rules are in game/car_look.h (unit-tested); this module reads the installed tuning items natively (mc_car_tuning,
// docs/re-notes.md 13), runs the Lua glue (car_look.lua: which cars, the reads and the setters) and carries CarLook
// (0x0253) (docs/protocol.md "Car look"). Additive: a peer without it ignores the message and keeps its copies as
// the game spawns them.
// Threads: messages and sending on the render thread; Lua and the native read on GameMainThread; a mutex in between.
#include "net/session.h"
#include "render/overlay.h"

#include <vector>

namespace mc::game {

class CarLookSync {
public:
    static void Install();  // once, after the Lua bridge (after player_sync and traffic_sync)
    static void Attach(net::Session& session);
    static void Detach();
    static void OnPlayerLeft(net::PlayerId id);
    static void Tick(float dt);
    static void GetStatus(std::vector<render::OverlayLine>& out);
};

}  // namespace mc::game
