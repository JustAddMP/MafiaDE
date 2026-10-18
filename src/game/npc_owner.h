#pragma once
// NPC ownership between games (B3, guest-owned NPCs). Pure logic, no game or Lua calls: unit-tested by
// tests/npc_owner_test.cpp; npc_sync.cpp feeds it and carries out what it decides (docs/protocol.md "NPCs",
// "Guest-owned NPCs").
//
// The host is authoritative for the NPCs both games have (npc_sync.lua pairs them). An NPC that only a guest's game
// spawned (he got somewhere first: his mission spawned the wave, the host's did not) has no host NPC to pair with:
//   - GuestOwner (guest): a local NPC with a job (AI type not 0) that stayed unpaired for kOwnWait s becomes *owned*
//     by this guest: its state goes to the host (GuestNpcStates, 10 Hz, nearest first, at most kMaxSend), its death
//     once (GuestNpcEnd died). When it gets paired with a host NPC after all (the host's own game spawned it: the guest's
//     usual pairing found it, or the host claimed it, NpcClaim) authority goes back to the host (GuestNpcEnd handed
//     back, with its health); when it is gone from the guest's world, GuestNpcEnd gone.
//   - HostMirrors (host): the guests' owned NPCs, each with a mirror id (mid, unique on the host) for the Lua side,
//     which shows it as a spawned copy driven by the guest's states. Natives (the host's own NPCs, from its own
//     NpcStates round) that turn up where a mirror is (the same spot, or the same archetype tag within kClaimDist) are
//     claimed for it: NpcClaim goes to the guest, which pairs that host NPC with his own and hands the NPC back.
// No feedback loops: spawned copies (the guest's of host NPCs, the host's of guest NPCs) are never sent: their local
// ids are registered here (SetCopies) and dropped from every outgoing list.
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace mc::net {
class ByteWriter;
class ByteReader;
}  // namespace mc::net

namespace mc::game::npcown {

using PlayerId = uint8_t;

// ---- one NPC on the wire (NpcStates 0x0210 and GuestNpcStates 0x0420 share it) -----------------------------------------
struct NpcState {
    uint32_t key = 0;  // its entity id on the sender's game
    float x = 0, y = 0, z = 0, hx = 0, hy = 0;
    uint16_t hp = 0;
    uint8_t flags = 0;  // 1 dead, 2 in a car
    uint8_t ai = 0;     // AI type (0 civilian, 2 enemy, ...)
    uint32_t tag = 0;   // battle archetype hash (0 = none)
    std::string weapon = "-";
    uint64_t profile = 0;  // spawn profile hash (0 = unknown)
    uint8_t shots = 0;     // shots fired, mod 256
    bool Dead() const { return (flags & 1u) != 0; }
};

constexpr size_t kPerPacket = 16;        // NPCs per states message
constexpr size_t kMaxStatesBytes = 1000;  // payload limit (core kMaxUnreliableMessage minus headers)
constexpr size_t kMaxNpcBytes = 4 + 12 + 2 + 2 + 2 + 4 + 1 + 48 + 8 + 1;  // one NPC with the longest weapon name

bool SafeWeapon(const std::string& s);
// Lua line fields (after the tag): "key x y z hx hy health flags ai tag weapon profile shots"
bool ParseState(const std::string& fields, NpcState& s);
std::string FormatState(const NpcState& s);  // the same fields, for a Lua command line
void WriteState(net::ByteWriter& w, const NpcState& s);
bool ReadState(net::ByteReader& r, NpcState& s);  // false on a short or bad record
// States in messages of at most kPerPacket NPCs / kMaxStatesBytes bytes (u8 n, then n records).
std::vector<std::vector<uint8_t>> PackStates(const std::vector<NpcState>& states);

// ---- guest --------------------------------------------------------------------------------------------------------------
enum class EndReason : uint8_t { Died = 0, HandedBack = 1, Gone = 2, Refused = 3 };
const char* EndName(EndReason r);

struct GuestEnd {  // GuestNpcEnd 0x0421
    uint32_t key = 0;
    EndReason reason = EndReason::Gone;
    uint32_t hostKey = 0;  // HandedBack / Refused: the host NPC
    uint16_t hp = 0;       // its health at the end
    bool byHost = false;   // Died: the host's hit killed it
};

struct LocalNpc {
    NpcState s;
    float dist = 0;        // from our player (nearest are sent first)
    uint32_t hostKey = 0;  // the host NPC it is paired with (0 = none)
};

class GuestOwner {
public:
    static constexpr double kOwnWait = 5.0;   // s unpaired (alive, with a job) before it is ours
    static constexpr double kGoneAfter = 2.0;  // s out of our rounds: an owned NPC is gone
    static constexpr double kForget = 10.0;    // s out of our rounds: any record is dropped
    static constexpr double kHostHitTime = 1.5;  // s: our NPC that dies this soon after a host hit died of it
    static constexpr size_t kMaxOwned = 16;
    static constexpr size_t kMaxSend = 16;    // states per round (one message)

    void SetCopies(std::vector<uint32_t> ids);  // our spawned copies of host NPCs: never ours to send
    bool IsCopy(uint32_t id) const { return copies_.count(id) != 0; }
    // One round (10 Hz) of every local NPC near us that is no copy or puppet.
    void Observe(double now, const std::vector<LocalNpc>& npcs);
    // The host hit our NPC `key` (HostNpcHit): remembered so its death right after counts as the host's kill.
    void HostHit(double now, uint32_t key);
    // NpcClaim: true when `key` is ours (Lua then tries to pair host NPC hostKey with it).
    bool Claim(uint32_t key, uint32_t hostKey) const;
    // Lua could not pair them (that host NPC is paired with another of ours, or has no state here).
    void Refuse(uint32_t key, uint32_t hostKey);

    std::vector<NpcState> TakeStates() { return std::move(states_); }  // this round's, nearest first
    std::vector<GuestEnd> TakeEnds() { return std::move(ends_); }
    std::vector<std::pair<uint32_t, bool>> TakeOwnedChanges() { return std::move(changes_); }  // key, owned now
    bool Owned(uint32_t key) const;
    size_t OwnedCount() const;
    uint32_t TotalOwned() const { return totalOwned_; }
    uint32_t TotalHandedBack() const { return totalBack_; }
    void Reset();

private:
    struct Rec {
        double unpairedSince = -1, lastSeen = 0, hostHitT = -100;
        bool owned = false, ended = false;  // ended: its death was sent (or it died before it was ours)
        NpcState last;
    };
    void End(uint32_t key, Rec& r, EndReason why, uint32_t hostKey, bool byHost = false);
    std::map<uint32_t, Rec> recs_;
    std::set<uint32_t> copies_;
    std::vector<NpcState> states_;
    std::vector<GuestEnd> ends_;
    std::vector<std::pair<uint32_t, bool>> changes_;
    uint32_t totalOwned_ = 0, totalBack_ = 0;
};

// ---- host ---------------------------------------------------------------------------------------------------------------
struct MirrorCmd {  // for the Lua side
    enum class Kind { State, Died, End, KillNative } kind = Kind::State;
    uint32_t mid = 0;
    NpcState s;            // State
    EndReason reason = EndReason::Gone;  // End
    uint32_t hostKey = 0;  // End (handed back) / KillNative: the host's own NPC
    uint16_t hp = 0;       // End: the guest's health of it
};

struct Claim {  // NpcClaim 0x0423
    PlayerId to = 0;
    uint32_t guestKey = 0, hostKey = 0;
    uint32_t mid = 0;
    bool dead = false;  // the guest's NPC is dead: the host's own is killed (KillNative)
};

class HostMirrors {
public:
    static constexpr double kRelease = 5.0;       // s without a state: a live mirror goes
    static constexpr double kDeadMemory = 60.0;   // s a dead mirror is remembered (claims kill the host's late spawn)
    static constexpr double kClaimWait = 2.5;     // s a native must exist before it is claimed (the guest pairs at 2)
    static constexpr double kClaimTimeout = 3.0;  // s for the guest's answer to a claim
    static constexpr double kRefuseMemory = 15.0; // s a refused (mirror, native) pair is not tried again
    static constexpr double kClaimedMemory = 10.0;  // s a native handed a mirror stays out of other claims
    static constexpr double kNativeForget = 5.0;  // s a native out of our rounds is forgotten
    static constexpr float kMatchDist = 1.5f;     // m: the same spot
    static constexpr float kClaimDist = 40.0f;    // m: the same archetype tag this close
    static constexpr float kClaimDz = 8.0f;
    static constexpr size_t kMaxPerGuest = 24;    // mirrors per guest at once
    static constexpr double kEndedMemory = 3.0;   // s a late state of an ended mirror is ignored

    void OnState(double now, PlayerId from, const NpcState& s);
    void OnEnd(double now, PlayerId from, const GuestEnd& e);
    void OnPlayerLeft(PlayerId from);
    // Our spawned copies of mirrors: local entity ids. Never sent (feedback guard) and never claimed.
    void SetCopies(std::vector<uint32_t> ids);
    bool IsCopy(uint32_t id) const { return copies_.count(id) != 0; }
    // Our own NPCs of this NpcStates round (copies already filtered out): claims.
    void ObserveNatives(double now, const std::vector<NpcState>& natives);
    void Expire(double now);
    bool Lookup(uint32_t mid, PlayerId& from, uint32_t& key) const;

    std::vector<MirrorCmd> TakeCmds() { return std::move(cmds_); }
    std::vector<Claim> TakeClaims() { return std::move(claims_); }
    size_t Count() const { return byMid_.size(); }
    size_t LiveCount() const;
    uint32_t TotalClaims() const { return totalClaims_; }
    void Reset();

private:
    struct Mirror {
        PlayerId from = 0;
        uint32_t key = 0;
        NpcState last;
        double t = 0;        // last state
        double deadAt = -1;  // >= 0: dead
        uint32_t pendingHost = 0;  // a claim waits for the guest's answer
        double pendingT = 0;
    };
    struct Native {
        double firstSeen = 0, lastSeen = 0;
        NpcState s;
    };
    void Drop(uint32_t mid, EndReason why, uint32_t hostKey, uint16_t hp);
    uint32_t nextMid_ = 1;
    std::map<uint32_t, Mirror> byMid_;
    std::map<std::pair<PlayerId, uint32_t>, uint32_t> byKey_;
    std::map<std::pair<PlayerId, uint32_t>, double> ended_;  // (guest, key) -> until: late states ignored
    double lastNow_ = 0;
    std::map<uint32_t, Native> natives_;
    std::map<uint32_t, double> claimed_;  // native key -> until
    std::map<std::pair<uint32_t, uint32_t>, double> refused_;  // (mid, native) -> until
    std::set<uint32_t> copies_;
    std::vector<MirrorCmd> cmds_;
    std::vector<Claim> claims_;
    uint32_t totalClaims_ = 0;
};

}  // namespace mc::game::npcown
