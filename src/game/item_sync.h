#pragma once
// Phase 4: the things lying in the world, shared (docs/protocol.md "World items", docs/game-api.md 20). Weapons on
// the ground (what dead enemies drop) and the ground pickups (inventory items such as medkits): the host is
// authoritative; a guest's item is paired with the host's (and put where it lies), his own new drops that the host's
// world does not have are hidden, a pickup taken by one player disappears on every game, the host decides when two
// grab it at once (the loser gives the weapon back). Collectibles: each player collects his own (for his own save);
// the others hear about it. The decisions are in world_items.h (unit-tested), the game side in item_sync.lua.
// Threads: messages on the render thread; Lua on GameMainThread (tick callback); a mutex for the hand-over.
#include "net/session.h"
#include "render/overlay.h"

#include <vector>

namespace mc::game {

class ItemSync {
public:
    static void Install();  // once, after the Lua bridge
    static void Attach(net::Session& session);
    static void Detach();
    static void Tick(float dt);
    static void GetStatus(std::vector<render::OverlayLine>& out);
};

}  // namespace mc::game
