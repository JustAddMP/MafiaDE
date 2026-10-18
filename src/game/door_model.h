#pragma once
// Doors on every game (docs/protocol.md "Doors", docs/game-api.md 20), the part without the game: the wire format of
// the door messages, the identity of a door across games (its scene name and its position: entity ids differ between
// games), this game's table of loaded doors with their state, the session memory of changed doors (doors that stream
// in later get their state), echo suppression (a door we changed for a remote player is not reported back), the
// host's authority over story doors (a guest cannot open a door that is locked on the host's game; the guests' own
// mission scripts do not unlock doors the host keeps locked) and the snapshot a guest gets on join / reconnect / a
// new world. Pure data in, decisions out: no Lua, no session, no OS calls (tests/door_test.cpp). Single-threaded:
// game/door_sync.cpp locks around every call.
#include "net/wire.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace mc::game::doors {

// ---- protocol (0x0230-0x0233) --------------------------------------------------------------------------------------
constexpr uint16_t kMsgEvent = 0x0230;        // guest -> host (SendToHost), R: his player changed a door
constexpr uint16_t kMsgApply = 0x0231;        // host -> guests (Broadcast, or SendTo one), R: a door change to apply
constexpr uint16_t kMsgSnapshot = 0x0232;     // host -> one guest (SendTo), R: the doors near him + changed doors
constexpr uint16_t kMsgSnapRequest = 0x0233;  // guest -> host (SendToHost), R: send me a snapshot (where I am)

// door state bits
constexpr uint8_t kOpen = 1;    // not IsClosed()
constexpr uint8_t kLocked = 2;  // IsLocked()
constexpr uint8_t kStateMask = kOpen | kLocked;

constexpr uint8_t kWorld = 0xFF;  // Event::by: not a player (the host's mission scripts, NPCs, a door closing itself)

constexpr float kMatchDist = 1.0f;        // m: the same name within this distance is the same door
constexpr float kMatchDistNoName = 0.3f;  // m: a door whose name could not be read matches only this close
constexpr size_t kMaxSnapshot = 96;       // entries in one DoorSnapshot (17 bytes each: ~1.6 KB)
constexpr size_t kMaxMemory = 256;        // remembered changed doors (oldest change goes first)
constexpr double kEchoWindow = 3.0;       // s an applied remote change may take to show in our poll
constexpr double kFreshFor = 3.0;         // s a polled state counts as live (the host's authority uses only those)
constexpr float kSnapshotRadius = 40.0f;  // m around the guest: the host's live door states in his snapshot
constexpr int kMaxReverts = 3;            // guest: host lock states put back per door per minute at most

// How a door changed. On the wire the act says how (a kick swings the door with force, a lockpick is the lock pull)
// and `state` says the result; the receiver brings its door to `state` the way `act` says.
enum class Act : uint8_t {
    None = 0,
    Open = 1,      // opened by hand (an unlocked door)
    Close = 2,     // closed
    Kick = 3,      // kicked open (unlocks it)
    Unlock = 4,    // unlocked (key, script)
    Lockpick = 5,  // unlocked with the lock pull (lockpick); may open it
    Lock = 6,      // locked (script)
    Set = 7,       // bring it to `state` (host corrections, snapshots, several bits at once)
};
constexpr uint8_t kLastAct = 7;
const char* ActName(Act a);
inline Act ActFromCode(unsigned v) { return v <= kLastAct ? static_cast<Act>(v) : Act::None; }
// The act that describes a change from `before` to `after` when nothing better is known (world changes).
Act DiffAct(uint8_t before, uint8_t after);
// What a door is after `act` (Set: `want`).
uint8_t StateAfter(Act act, uint8_t before, uint8_t want);
std::string StateName(uint8_t s);  // "open,unlocked" ...

struct Vec3 {
    float x = 0, y = 0, z = 0;
};
float Dist(const Vec3& a, const Vec3& b);

// A door as every game knows it: FNV-1a of its lower-case scene name (0 = unreadable) and where it stands.
struct Ref {
    uint32_t name = 0;
    Vec3 pos;
};
uint32_t NameHash(const std::string& name);  // 0 for "" and "-"
bool Same(const Ref& a, const Ref& b);

struct Event {
    uint16_t seq = 0;       // the sender's door event counter (logs / trace)
    uint8_t by = kWorld;    // the player who did it, kWorld = the host's world
    Act act = Act::None;
    uint8_t state = 0;      // the door after it (kOpen | kLocked)
    Ref door;
    Vec3 from;              // where the one who did it stood (the door opens away from him)
};
// DoorEvent / DoorApply body: u16 seq, u8 by, u8 act, u8 state, u32 name, f32 x y z, f32 fx fy fz (33 bytes)
void WriteEvent(net::ByteWriter& w, const Event& e);
bool ReadEvent(net::ByteReader& r, Event& e);  // false: truncated, unknown act, bad numbers

struct SnapEntry {
    Ref door;
    uint8_t state = 0;
};
// DoorSnapshot body: u8 n (at most kMaxSnapshot), n x (u32 name, f32 x y z, u8 state)
void WriteSnapshot(net::ByteWriter& w, const std::vector<SnapEntry>& list);
bool ReadSnapshot(net::ByteReader& r, std::vector<SnapEntry>& list);

enum class Reason : uint8_t { Join = 1, Reconnect = 2, World = 3 };
const char* ReasonName(Reason r);
struct SnapRequest {
    Vec3 pos;  // the guest's player
    Reason reason = Reason::Join;
};
// DoorSnapRequest body: f32 x y z, u8 reason
void WriteRequest(net::ByteWriter& w, const SnapRequest& q);
bool ReadRequest(net::ByteReader& r, SnapRequest& q);

// A change the Lua glue carries out on one of our doors.
struct Apply {
    uint32_t local = 0;  // our door's entity id
    Act act = Act::None;
    uint8_t state = 0;   // where to bring it
    Vec3 from;
    uint8_t by = kWorld;
    const char* why = "";  // remote / memory / snapshot / revert / reject (logs)
};

enum class Verdict {
    Unknown,    // not a door we know (ignored)
    Echo,       // the change we made for a remote player showing up: not sent again
    Send,       // goes out: `ev` is filled
    WorldKept,  // guest: a change of his own world (NPC, his scripts): stays local, not sent
    Revert,     // guest: his own scripts changed the lock of a door the host decided: put back (an Apply)
    Same,       // no change of state
};
const char* VerdictName(Verdict v);

struct Stats {
    uint32_t doors = 0;
    uint64_t changes = 0, ours = 0, world = 0, echoes = 0, sent = 0, kept = 0, reverts = 0;
    uint64_t remote = 0, applied = 0, pending = 0, fromMemory = 0, rejected = 0, snapIn = 0, snapOut = 0;
    uint64_t unmet = 0;  // applied changes that did not show in our poll in time
};

class Doors {
public:
    void SetHost(bool host) { host_ = host; }
    bool Host() const { return host_; }

    // ---- this game's doors (from the Lua scan and poll) ----
    // A door came into our table (first seen, or streamed in again). Remembered changes of it come out as applies.
    void Seen(uint32_t local, const std::string& name, const Vec3& pos, uint8_t state, double now,
              std::vector<Apply>& out);
    void Gone(uint32_t local);
    void Fresh(uint32_t local, double now);  // polled now, state unchanged
    // Our poll saw a door change. ours: our player did it (act how, from where he stood). `ev` is filled for Send.
    Verdict Changed(uint32_t local, uint8_t state, bool ours, Act act, const Vec3& from, uint8_t localPlayer,
                    double now, Event& ev, std::vector<Apply>& out);

    // ---- from the network ----
    // A change to show here (host: a guest's accepted event; guest: the host's DoorApply). Found -> an Apply (unless
    // the door is in that state already); not loaded here -> remembered, applied when it streams in.
    void Remote(const Event& ev, double now, std::vector<Apply>& out);
    // Host: may this guest's change stand? Reject: the door is locked here (a live state) and he only opened it by
    // hand: his game unlocked it by itself, ours decides. `hostState` is what the door is here.
    bool Accept(const Event& ev, double now, uint8_t* hostState) const;
    // Host: what a guest gets: the doors around `center` whose state we know live (polled within kFreshFor), then
    // the remembered changes nearest first; at most `max`.
    std::vector<SnapEntry> Snapshot(const Vec3& center, float radius, double now, size_t max) const;
    // Guest: the host's snapshot. Each entry is remembered (host-decided) and our loaded doors in another state are set.
    void ApplySnapshot(const std::vector<SnapEntry>& list, double now, std::vector<Apply>& out);

    // Applied changes that did not show in time (logged by the caller).
    std::vector<uint32_t> Expire(double now);
    void Clear();  // another world: no doors, no memory (the session goes on)

    const Stats& GetStats() const { return stats_; }
    size_t MemorySize() const { return memory_.size(); }
    // tests / logs
    bool Find(const Ref& r, uint32_t* local) const;
    bool StateOf(uint32_t local, uint8_t* state) const;
    bool Remembered(const Ref& r, uint8_t* state) const;
    bool Expecting(uint32_t local) const { return expect_.count(local) != 0; }
    const Ref* RefOf(uint32_t local) const;

private:
    struct Door {
        Ref ref;
        uint8_t state = 0;
        double freshAt = -1e9;  // last time its state was read
    };
    struct Memory {
        Ref ref;
        uint8_t state = 0;
        double at = 0;
        uint8_t by = kWorld;
        bool told = false;     // guest: the host decided it (a DoorApply / snapshot)
        int reverts = 0;
        double revertFrom = -1e9;
    };
    struct Expect {
        uint8_t state = 0;
        double until = 0;
    };
    Memory* FindMemory(const Ref& r);
    const Memory* FindMemory(const Ref& r) const;
    void Remember(const Ref& r, uint8_t state, double now, uint8_t by, bool told);
    void Push(uint32_t local, Door& d, Act act, uint8_t state, const Vec3& from, uint8_t by, const char* why,
              double now, std::vector<Apply>& out);

    bool host_ = false;
    std::map<uint32_t, Door> doors_;
    std::vector<Memory> memory_;
    std::map<uint32_t, Expect> expect_;
    uint16_t seq_ = 0;
    Stats stats_;
};

}  // namespace mc::game::doors
