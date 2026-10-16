#pragma once
// Guard for the game-sectors module's once-a-second walk over its list of humans (docs/game-api.md 15, re-notes 8).
//
// C_GameSectorsModule::TickInGame (exe+0x243A7B0) calls exe+0x243A870 every frame; once a second that walks a vector of
// entity pointers (module +0x58 .. +0x60) and calls a virtual function on each. Humans are added to it by a component
// (exe+0x2D12040) and taken out again by its counterpart (exe+0x2D121A0); a human freed some other way stays in the
// vector, and the next walk calls through freed memory: the crash at exe+0x243A915 that a brain switch (friend-brain
// puppets), a released burning human and a released marker human all ended in. The guard hooks exe+0x243A870 and,
// right before each walk, drops every pointer that is no longer a live entity (its id no longer resolves to it in the
// game's entity table).
#include <cstdint>

namespace mc::game::sectors_guard {

void Install();         // once, after the Lua bridge (it uses the bridge's entity table lookup)
bool Active();          // the hook is in place
uint32_t Dropped();     // stale entries removed since start

}  // namespace mc::game::sectors_guard
