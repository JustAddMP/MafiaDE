#pragma once
// A paused game (docs/protocol.md "PeerPaused"). While a game is paused (the pause menu, the CO-OP panel opened from
// it, a loading screen) its script tick does not run: it sends no PlayerState and, on the host, no traffic or NPC
// states. The others' copies of what it owns then coasted on their own physics and AI, and on the resume every one of
// them was far off its target at once: the 2026-10-19 two-PC test had 6-10 "knocked ... put back, reported to the
// host" corrections within 0.5 s after each of the host's pauses. Now:
//   - the paused game says so (PeerPaused 0x0103, reliable, every peer -> everyone): on the pause, every kRepeat s
//     while it lasts (a peer that joins meanwhile learns it), and on the resume;
//   - the others freeze that peer's copies (his puppet, his car, the host's traffic copies) where they stand and skip
//     knock detection and pull-backs while he is paused and for kGrace s after his resume (and after a pause of
//     their own: their copies were left behind while their world stood).
// Pure logic, no game or OS calls (tests/peer_pause_test.cpp). game/player_sync.cpp detects the local pause on the
// render thread and sends / receives the message; traffic, NPC and player sync ask the registry below.
#include "net/wire.h"

#include <cstdint>
#include <map>

namespace mc::game::peer_pause {

constexpr uint16_t kMsgPeerPaused = 0x0103;

constexpr double kStallAfter = 0.3;  // s without a script tick (or with the game's paused tick only): paused
constexpr double kRepeat = 2.0;      // s between repeats of a paused game's message
constexpr double kGrace = 1.0;       // s after a resume: no knock detection, no pull-backs yet

enum class Reason : uint8_t {
    None = 0,      // not paused (a resume)
    NoTick = 1,    // the game's script tick stopped (pause menu, loading screen, a hang)
    PausedTick = 2 // the game ticks its scripts through C_ScriptMachine::TickPaused only
};
const char* ReasonName(Reason r);

// PeerPaused body: u8 flags (bit0 paused), u8 reason, u32 ms paused so far (on the resume: how long it lasted).
// Read accepts trailing bytes (later additions); a body shorter than 6 bytes is rejected.
struct Msg {
    bool paused = false;
    Reason reason = Reason::None;
    uint32_t pausedMs = 0;
};
void Write(net::ByteWriter& w, const Msg& m);
bool Read(net::ByteReader& r, Msg& m);

// The local game (render thread): fed every frame with the time of the last script tick and whether it came from the
// paused tick. Returns true when a message is due (out filled): on each change and every kRepeat s while paused.
class LocalDetector {
public:
    // now: s; lastTickAt: s of the last script tick (<= 0: none yet, never paused); pausedTick: the last ticks came
    // through TickPaused only (since pausedTickSince s); inWorld: the game's world is loaded (a menu pause outside the
    // world is not reported).
    bool Update(double now, double lastTickAt, bool pausedTick, double pausedTickSince, bool inWorld, Msg& out);
    bool Paused() const { return paused_; }
    double PausedSince() const { return since_; }
    double ResumedAt() const { return resumedAt_; }
    void Reset();  // session start / end: not paused, nothing sent

private:
    bool paused_ = false;
    Reason reason_ = Reason::None;
    double since_ = 0, sentAt_ = 0, resumedAt_ = -1e9;
};

// What another game's pause means here now.
enum class Quiet : uint8_t {
    None = 0,    // business as usual
    Paused = 1,  // paused: his copies are held, no knocks, no pull-backs
    Grace = 2,   // resumed less than kGrace s ago: his copies move again, still no knocks or pull-backs
};
const char* QuietName(Quiet q);

// The other games' pauses (by player id) and the local resume. Not thread-safe: the global registry below locks.
class Peers {
public:
    // A PeerPaused from `id`. Returns true when it changed his state (for the log).
    bool Set(uint8_t id, bool paused, double now);
    Quiet Get(uint8_t id, double now) const;
    void Remove(uint8_t id);
    void Clear();
    // our own game resumed at `now` (the local detector's resume, or a tick gap the caller saw)
    void LocalResumed(double now) { localResumedAt_ = now; }
    // our own copies were left behind by our pause: Grace for kGrace s after it
    Quiet Local(double now) const;

private:
    struct Peer {
        bool paused = false;
        double since = 0, resumedAt = -1e9;
    };
    std::map<uint8_t, Peer> peers_;
    double localResumedAt_ = -1e9;
};

// Process-wide registry (any thread; a mutex inside): the render thread fills it, the main-thread ticks read it.
bool SetRemote(uint8_t id, bool paused, double now);
Quiet Remote(uint8_t id, double now);
void RemoveRemote(uint8_t id);
void ClearRemotes();
void SetLocalResumed(double now);
Quiet Local(double now);
// The stronger of Remote(id) and Local(): what a copy of `id`'s thing on this game must respect.
Quiet For(uint8_t id, double now);

}  // namespace mc::game::peer_pause
