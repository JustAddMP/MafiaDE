#pragma once
// Car damage on every game: body and part damage, broken / detached parts, burst tyres, lost wheels, the engine, the
// fire and the explosion of player cars (the driver's game holds them, the last driver's while parked) and of the
// host's traffic (the host holds them). The game-free rules are in game/car_damage.h (unit-tested); this module reads
// and sets the damage on the game (native, from the exe's own part, tyre and explosion functions, docs/re-notes.md
// 10), runs the Lua glue (car_damage.lua: which cars, which game holds each) and carries the messages CarDamage
// (0x0250), CarDamageReport (0x0251) and CarDamageRequest (0x0252) (docs/protocol.md "Car damage").
// Threads: messages and sending on the render thread; Lua, reads and writes on GameMainThread; a mutex in between.
#include "net/session.h"
#include "render/overlay.h"

#include <vector>

namespace mc::game {

class CarDamageSync {
public:
    static void Install();  // once, after the Lua bridge
    static void Attach(net::Session& session);
    static void Detach();
    static void OnPlayerLeft(net::PlayerId id);
    static void Tick(float dt);
    static void GetStatus(std::vector<render::OverlayLine>& out);
};

}  // namespace mc::game
