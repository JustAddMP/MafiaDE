#pragma once
// Phase 4 (B3): mission NPCs in co-op. Every game runs the chapter's mission scripts at the host's checkpoint, so
// the mission NPCs exist on every game; the host is authoritative for them (docs/protocol.md "NPCs",
// docs/game-api.md 15, the game side in npc_sync.lua):
//   - Host: the state of the NPCs near any player (position, heading, health, dead, AI type, archetype tag, weapon),
//     10 Hz (NpcStates), and every NPC death at once (NpcDeath).
//   - Guest: pairs each host NPC with its own copy (same spot; else the nearest of the same kind within 40 m), keeps
//     its health at most the host's, kills it when the host's dies, puts it back when it wandered off; the guest's hits
//     on copies hurt them and go to the host (NpcHit), where they are replayed as real shots from the guest's puppet
//     (the NPC turns on the puppet); a hit that killed the guest's copy is lethal on the host too.
// Threads: messages on the render thread; Lua on GameMainThread (tick callback); a mutex for the hand-over.
#include "net/session.h"
#include "render/overlay.h"

#include <vector>

namespace mc::game {

class NpcSync {
public:
    static void Install();  // once, after the Lua bridge
    static void Attach(net::Session& session);
    static void Detach();
    static void Tick(float dt);
    static void GetStatus(std::vector<render::OverlayLine>& out);
};

}  // namespace mc::game
