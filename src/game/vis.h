#pragma once
// VIS: one trace record for every authority / visibility event of a replicated thing, with a key that is the same on
// both PCs (docs/sync-trace.md "VIS"), so tools/sync_compare.py can follow one entity from the host's decision to
// the guest's screen:
//   player   key = the player id (his puppet on the other game)
//   traffic  key = the host car's net id (TrafficSpawn / TrafficStates)
//   ped      key = (host id << 16) + the pedestrian's net id (PedSpawn; the host is 0, so the net id)
//   npc      key = the host's entity id of the NPC (NpcInfo key)
//   held     key = the guest's own entity id of an NPC held hidden (game/npc_hold.h; only on his game)
// "<t> VIS kind=<kind> key=<key> ev=<event> dir=<tx: decided for a peer, rx: from the host / shown here, -: local>
// peer=<the other player, - none> [extra]". Each event also counts in core/diag.h ("vis.<kind>.<ev>") for the session
// summaries. Rate-safe: at most kMaxPerSecond records per kind and second; the rest are counted and one
// "ev=dropped n=<count>" record says so. Any thread.
#include <cstdint>

namespace mc::game::vis {

enum class Kind : uint8_t { Player = 0, Traffic, Ped, Npc, Held, Count };
const char* KindName(Kind k);

constexpr int kMaxPerSecond = 60;

void Event(Kind kind, uint32_t key, const char* ev, const char* dir, int peer, const char* fmt = nullptr, ...);

// The per-kind cap (pure; tests/vis_test.cpp). now in seconds.
class RateCap {
public:
    // True: write this one. *reportDropped: a finished second's dropped count to report first (0 = none).
    bool Allow(Kind k, double now, uint32_t* reportDropped) {
        Slot& s = slots_[static_cast<int>(k)];
        *reportDropped = 0;
        if (now - s.start >= 1.0) {
            *reportDropped = s.dropped;
            s.start = now;
            s.count = 0;
            s.dropped = 0;
        }
        if (s.count < kMaxPerSecond) {
            ++s.count;
            return true;
        }
        ++s.dropped;
        return false;
    }

private:
    struct Slot {
        double start = -1e300;
        int count = 0;
        uint32_t dropped = 0;
    };
    Slot slots_[static_cast<int>(Kind::Count)];
};

}  // namespace mc::game::vis
