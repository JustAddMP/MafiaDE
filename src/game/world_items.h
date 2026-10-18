#pragma once
// Things lying in the world, shared between the games (docs/protocol.md "World items", docs/game-api.md 20). Pure
// logic, no game or Lua calls: unit-tested by tests/world_items_test.cpp; item_sync.cpp feeds it and carries out what
// it decides, item_sync.lua talks to the game.
//
// What is shared: the item entities of the game's item manager near any player, i.e. weapons lying on the ground
// (entity type 63, C_ItemWeapon: what a dead enemy drops) and inventory items (type 66, C_ItemInventory: the
// ground pickups such as medkits; which ones a world has is UNVERIFIED). The host is authoritative:
//   - HostBook (host): its own items, keyed by their entity id there (the key on the wire). It sends them (ItemStates),
//     decides who took an item (the first take it learns of wins: its own player's, seen in its own world, or a
//     guest's ItemTake) and tells everyone (ItemGone). A take that comes after another player's is refused: that guest
//     alone gets an ItemGone naming the winner, and gives back what he got.
//   - GuestBook (guest): pairs each host item with an item of his own world (same type; the nearest within kMatchDist,
//     or with the same name within kLooseDist: the guest's copy of an enemy died a few metres from the host's), puts
//     his item where the host's lies, hides his own new weapon drops that the host's world does not have (his copy of
//     an enemy dropped one, the host's did not, or elsewhere), and hides his item when the host's is taken. When his own
//     paired item disappears next to him (he took it), he asks the host (ItemTake); if the host says another player was
//     first, the weapons he gained with it are taken away again.
// An item that vanishes away from every player despawned (the game removes dropped weapons at a distance,
// "ItemWeaponDespawnDistance"): that is no take; the host forgets it (ItemGone forget), the guest's item stays his.
// Collectibles (cards, magazines, comics) are not items here: each player collects his own (item_sync.lua; the other
// players only hear about it, Collectible message), so nobody's save ever changes from another player's actions.
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace mc::net {
class ByteWriter;
class ByteReader;
}  // namespace mc::net

namespace mc::game::items {

using PlayerId = uint8_t;
constexpr PlayerId kNobody = 0xFF;
constexpr PlayerId kHost = 0;

constexpr uint8_t kTypeWeapon = 63;     // EntityType ITEM_WEAPON (C_ItemWeapon)
constexpr uint8_t kTypeInventory = 66;  // EntityType ITEM_INVENTORY (C_ItemInventory)
bool SharedType(uint8_t type);

// ---- one item on the wire (ItemStates 0x0270) ----------------------------------------------------------------------------
struct Item {
    uint32_t key = 0;  // its entity id on the sender's game (the local id in a Lua line)
    uint8_t type = 0;
    float x = 0, y = 0, z = 0;
    std::string name = "-";  // its scene object name ("-" unknown)
    bool Named() const { return !name.empty() && name != "-"; }
};

constexpr size_t kPerPacket = 16;  // items per ItemStates message
constexpr size_t kMaxName = 40;    // longer names are cut
constexpr size_t kMaxSend = 96;    // host: items per round, nearest first

std::string SafeName(const std::string& s);  // printable, no spaces, at most kMaxName; "-" when empty
// Lua line fields (after the tag): "id type x y z name"
bool ParseItem(const std::string& fields, Item& it);
std::string FormatItem(const Item& it);
void WriteItem(net::ByteWriter& w, const Item& it);
bool ReadItem(net::ByteReader& r, Item& it);  // false on a short or bad record
std::vector<std::vector<uint8_t>> PackStates(const std::vector<Item>& items);  // u8 n, then n records

// Collectible names (Collectible 0x0273): the game's own `lh_pickup_...` scene object names only.
bool CollectibleName(const std::string& s);

// ---- tuning ------------------------------------------------------------------------------------------------------------------
constexpr float kTakeDist = 2.5f;    // m: an item that vanished this close to our player was taken by him
constexpr float kMatchDist = 1.5f;   // m: a guest item this close to a host item of the same type is its copy
constexpr float kLooseDist = 4.0f;   // m: ... or this close with the same name
constexpr float kMoveDist = 0.3f;    // m: a paired item farther off than this is put where the host's lies
constexpr float kOwnDrop = 2.5f;     // m: a weapon that appears this close to our own player is his drop: never hidden
constexpr float kHideRange = 60.0f;  // m: only our new drops this close to us are hidden (the host sends farther)
constexpr double kHideWait = 3.0;    // s an unpaired new weapon drop waits for a host partner before it is hidden
constexpr double kNewAfterLoad = 5.0;  // s after our world loaded: weapons that appear later are new drops
constexpr double kHostQuiet = 2.0;   // s: no host item states for longer: nothing of ours is hidden
constexpr double kRelease = 5.0;     // s without a state: the host item is given up (ours stays, unpaired)
constexpr double kForgetLocal = 30.0;  // s an item not in any round (out of range) is kept
constexpr double kKeepTaken = 30.0;  // s a taken item is remembered (late takes are refused, late states ignored)

// ---- what the Lua side has to do ----------------------------------------------------------------------------------------------
struct Cmd {
    enum class Kind : uint8_t { Hide, Move, Undo };
    Kind kind = Kind::Hide;
    uint32_t id = 0;              // our local item (Hide, Move)
    float x = 0, y = 0, z = 0;    // Move: where to
    std::string names;            // Undo: the weapons to take away (comma separated)
    std::string why;              // for the log
};

enum class GoneReason : uint8_t { Taken = 0, Forget = 1 };
const char* GoneName(GoneReason r);

struct Gone {  // ItemGone 0x0272
    uint32_t key = 0;
    PlayerId by = kNobody;  // Taken: who has it
    GoneReason reason = GoneReason::Taken;
    PlayerId to = kNobody;  // kNobody: everyone (broadcast); else only this player (a refused take)
};

struct Take {  // ItemTake 0x0271
    uint32_t key = 0;  // the host's item
    uint8_t type = 0;
    float x = 0, y = 0, z = 0;  // where our copy lay
};

// ---- host ---------------------------------------------------------------------------------------------------------------------
class HostBook {
public:
    struct Stats {
        uint32_t ours = 0, guests = 0, refused = 0, forgotten = 0, unknownTakes = 0;
    };
    void Reset();
    // A round of our own items near any player (items out of range are not in it and are kept for kForgetLocal).
    void OnRound(double now, float px, float py, float pz, const std::vector<Item>& items);
    // Our item vanished from our world (the entity is gone or switched off).
    void OnGone(double now, uint32_t id);
    // A guest took his copy of our item `key`.
    void OnTake(double now, PlayerId from, uint32_t key);
    std::vector<Item> States() const;  // present, not taken, from the last round (nearest first, at most kMaxSend)
    std::vector<Gone> TakeGones();
    std::vector<Cmd> TakeCmds();
    size_t Count() const;  // present items
    bool Taken(uint32_t key, PlayerId* by = nullptr) const;
    const Stats& Counters() const { return stats_; }

private:
    struct Rec {
        Item it;
        double seen = 0;  // last round with it
        float dist = 0;   // from our player then
        bool inRound = false;
        bool taken = false;
        PlayerId by = kNobody;
        double takenAt = 0;
    };
    void Expire(double now);
    std::map<uint32_t, Rec> recs_;
    std::vector<Gone> gones_;
    std::vector<Cmd> cmds_;
    float px_ = 0, py_ = 0, pz_ = 0;
    bool havePlayer_ = false;
    Stats stats_;
};

// ---- guest --------------------------------------------------------------------------------------------------------------------
class GuestBook {
public:
    struct Stats {
        uint32_t pairs = 0, moves = 0, hidden = 0, takes = 0, confirmed = 0, lost = 0, hiddenForHost = 0;
    };
    enum class LocalGone : uint8_t { Unknown, OursHidden, Taken, Despawned, LocalOnly };
    void Reset();
    void SetSelf(PlayerId id) { self_ = id; }
    void OnHostItem(double now, const Item& it);
    void OnGone(double now, uint32_t key, PlayerId by, GoneReason reason);
    // A round of our own items near us. worldAge: s since our world loaded.
    void OnRound(double now, float px, float py, float pz, double worldAge, const std::vector<Item>& locals);
    // Our item vanished from our world; gained: the weapons our player got since the last round (comma separated).
    LocalGone OnLocalGone(double now, uint32_t id, const std::string& gained);
    std::vector<Take> TakeTakes();
    std::vector<Cmd> TakeCmds();
    uint32_t PartnerOf(uint32_t hostKey) const;  // our item paired with that host item (0 = none)
    uint32_t HostOf(uint32_t localId) const;     // the host item our item is paired with (0 = none)
    bool Hidden(uint32_t localId) const;
    size_t HostCount() const { return host_.size(); }
    size_t Paired() const;
    size_t Unshown() const;  // host items (recent) without a partner here
    const Stats& Counters() const { return stats_; }

private:
    struct HostRec {
        Item it;
        double t = 0;
        uint32_t local = 0;
        float movedX = 1e9f, movedY = 1e9f, movedZ = 1e9f;  // where we last put our item for it
    };
    struct LocalRec {
        Item it;
        double firstSeen = 0, t = 0;
        bool newDrop = false, ownDrop = false, hidden = false;
        uint32_t host = 0;
        float dist = 0;
    };
    struct Pending {
        std::string gained;
        double t = 0;
    };
    void Unpair(uint32_t hostKey);
    void Pair(double now);
    void Expire(double now);
    std::map<uint32_t, HostRec> host_;
    std::map<uint32_t, LocalRec> local_;
    std::map<uint32_t, double> done_;  // host keys taken / given up: their late states are ignored until then
    std::map<uint32_t, Pending> pending_;  // our takes waiting for the host's answer, by host key
    std::vector<Take> takes_;
    std::vector<Cmd> cmds_;
    double lastHostState_ = -1e9;
    float px_ = 0, py_ = 0, pz_ = 0;
    bool havePlayer_ = false;
    PlayerId self_ = kNobody;
    Stats stats_;
};

}  // namespace mc::game::items
