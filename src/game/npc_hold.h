#pragma once
// Guest NPCs that the host's game does not have (yet): held (docs/host-authority.md, docs/protocol.md "NPCs" "Held
// NPCs"). Pure logic, no game or Lua calls: unit-tested by tests/npc_hold_test.cpp; npc_sync.cpp feeds it the guest's
// rounds of his own NPCs with a job and hands what it decides to npc_sync.lua.
//
// NPCs come from the host only. Every game runs the chapter's mission scripts, so a guest who got somewhere first has
// NPCs the host's game has not spawned (yet). Such an NPC is never sent anywhere (the guest-owned NPCs of 0.15 / 0.16,
// GuestNpcStates2 and friends 0x0421-0x0426, are retired) and must not fight on its own on the guest's game:
//   - a guest NPC with a job (AI type not 0; ambient humans are ped_sync's) that stays unpaired (no host NPC is it)
//     for kHoldAfter s is HELD: hidden (no model, physics state 2, demigod) with its AI interrupted, where it is;
//   - it stays held until a host NPC pairs with it (npc_sync.lua's usual pairing: the same spot within 1.5 m, or the
//     same tag / AI type within 40 m after 2 s): then it is shown and is that NPC's replica like any paired NPC; one
//     paired with a dead host NPC is shown and dies with it;
//   - held NPCs are released (shown, their AI resumed) when the session ends or this game becomes the host;
//   - an NPC that died before it was held is never held; a held one that dies (its own scripts) stays hidden;
//   - mission scripts keep running: a held NPC is never despawned.
#include <cstdint>
#include <map>
#include <vector>

namespace mc::game::npchold {

// One of our NPCs in a round of the Lua glue (guest, 10-15 Hz): every human with a job within the round's range that
// is no puppet, copy, traffic copy crew or pedestrian replica, and every held one wherever it is.
struct Local {
    uint32_t id = 0;        // its entity id on our game
    uint8_t ai = 0;         // AI type (0: no job, never held)
    bool dead = false;
    uint32_t hostKey = 0;   // the host NPC it is the replica of (paired), 0 none
    bool hostDead = false;  // that host NPC is dead
};

enum class Kind : uint8_t { Hold = 1, Show = 2, Kill = 3 };
enum class Why : uint8_t { Unpaired = 0, Paired = 1, HostDead = 2, SessionEnd = 3 };
const char* KindName(Kind k);
const char* WhyName(Why w);

struct Cmd {
    Kind kind = Kind::Hold;
    uint32_t id = 0;       // our NPC
    uint32_t hostKey = 0;  // Show / Kill after a pairing: the host NPC
    Why why = Why::Unpaired;
};

class HoldSet {
public:
    static constexpr double kHoldAfter = 5.0;  // s unpaired (alive, with a job) before it is held
    static constexpr double kGap = 1.0;        // s without a round (a cutscene, a loading screen): the wait restarts
    static constexpr double kForget = 30.0;    // s out of the rounds: its record goes (the NPC is gone from our world)

    // One round of our NPCs (guest).
    void Observe(double now, const std::vector<Local>& npcs);
    // The session ended (or we host now): every held NPC is shown with its own AI again; nothing is remembered.
    void End();
    std::vector<Cmd> TakeCmds() { return std::move(cmds_); }

    bool Held(uint32_t id) const;
    size_t HeldCount() const;
    uint32_t TotalHeld() const { return totalHeld_; }      // NPCs held (each time)
    uint32_t TotalPaired() const { return totalPaired_; }  // held ones a host NPC paired with later (shown)
    uint32_t TotalKilled() const { return totalKilled_; }  // of them, paired with a dead host NPC (shown, killed)
    uint32_t TotalReleased() const { return totalReleased_; }  // shown at the session's end
    void Reset();

private:
    struct Rec {
        double unpairedSince = -1, lastSeen = -100;
        bool held = false, diedFree = false;  // diedFree: dead before it was held (never held)
    };
    std::map<uint32_t, Rec> recs_;
    std::vector<Cmd> cmds_;
    uint32_t totalHeld_ = 0, totalPaired_ = 0, totalKilled_ = 0, totalReleased_ = 0;
};

}  // namespace mc::game::npchold
