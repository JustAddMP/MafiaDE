#include "game/npc_hold.h"

namespace mc::game::npchold {

const char* KindName(Kind k) {
    switch (k) {
    case Kind::Hold: return "hold";
    case Kind::Show: return "show";
    case Kind::Kill: return "kill";
    }
    return "?";
}

const char* WhyName(Why w) {
    switch (w) {
    case Why::Unpaired: return "unpaired";
    case Why::Paired: return "paired";
    case Why::HostDead: return "host_dead";
    case Why::SessionEnd: return "session_end";
    }
    return "?";
}

void HoldSet::Observe(double now, const std::vector<Local>& npcs) {
    for (const Local& n : npcs) {
        if (n.id == 0) continue;
        auto it = recs_.find(n.id);
        if (it == recs_.end()) {
            if (n.ai == 0) continue;  // no job: not ours to hold (ambient humans are ped_sync's)
            it = recs_.emplace(n.id, Rec{}).first;
        }
        Rec& r = it->second;
        const bool gap = now - r.lastSeen > kGap;
        r.lastSeen = now;
        if (n.hostKey != 0) {  // a host NPC is it: its replica (shown when it was held)
            r.unpairedSince = -1;
            if (r.held) {
                r.held = false;
                ++totalPaired_;
                cmds_.push_back({Kind::Show, n.id, n.hostKey, Why::Paired});
                if (n.hostDead && !n.dead) {
                    ++totalKilled_;
                    cmds_.push_back({Kind::Kill, n.id, n.hostKey, Why::HostDead});
                }
            }
            continue;
        }
        if (r.held) continue;  // held until a host NPC pairs with it (dead or not: nobody sees it)
        if (n.dead) {          // died before it was held: never held
            r.diedFree = true;
            r.unpairedSince = -1;
            continue;
        }
        if (r.diedFree || n.ai == 0) continue;
        if (gap || r.unpairedSince < 0) r.unpairedSince = now;
        if (now - r.unpairedSince >= kHoldAfter - 1e-6) {  // (rounds come at 10-15 Hz)
            r.held = true;
            ++totalHeld_;
            cmds_.push_back({Kind::Hold, n.id, 0, Why::Unpaired});
        }
    }
    for (auto it = recs_.begin(); it != recs_.end();) {
        if (now - it->second.lastSeen > kForget)
            it = recs_.erase(it);
        else
            ++it;
    }
}

void HoldSet::End() {
    for (const auto& [id, r] : recs_) {
        if (!r.held) continue;
        ++totalReleased_;
        cmds_.push_back({Kind::Show, id, 0, Why::SessionEnd});
    }
    recs_.clear();
}

bool HoldSet::Held(uint32_t id) const {
    auto it = recs_.find(id);
    return it != recs_.end() && it->second.held;
}

size_t HoldSet::HeldCount() const {
    size_t n = 0;
    for (const auto& [id, r] : recs_)
        if (r.held) ++n;
    return n;
}

void HoldSet::Reset() {
    recs_.clear();
    cmds_.clear();
}

}  // namespace mc::game::npchold
