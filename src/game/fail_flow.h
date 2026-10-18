#pragma once
// A failed mission in co-op: one path for every kind of failure, one load on every game (docs/protocol.md "Failed
// missions", docs/re-notes.md 7).
//
// Every mission failure the scripts raise ends in the director's GameOver(reason id) (the area check of
// containers.lua RangeCheckStart, timer.lua SimpleFail, the flows' gameOver node in game_director/other.lua; a lost
// mission car, a target that escaped, an ally that died are flow nodes too [likely]). mission_sync hooks it and asks
// this state machine what to do:
//   - host of a lobby CHAPTER (his saves are kept out of his slots): the game's own answer (a load of the free roam
//     state before the quest, then our reload of the checkpoint: two loads) is skipped; he leaves for the main menu
//     at once and loads the session's checkpoint (one load). The guests are told (HostFailed) and follow;
//   - host of a STORY session: the game's fail screen stays (Load Last Checkpoint is one load); the guests are told;
//   - guest: the game's answer is skipped, he leaves for the main menu at once and reports the failure to the host,
//     who decides: REJOIN (the host plays on: load his point now) or WAIT (the host failed too: load his point once he
//     is back in his world). No answer within kAskTimeout: REJOIN.
// Duplicates (the game raises GameOver more than once per failure; a death during a failure) are skipped while a
// failure is handled. Pure logic, no game calls: unit-tested in tests/fail_flow_test.cpp. Not thread-safe: the
// caller locks (the GameOver hook runs on GameMainThread, the rest on the render thread).
#include <cstdint>
#include <string>
#include <vector>

namespace mc::game {

enum class FailRole : uint8_t {
    None,         // not in a co-op story (Free Ride, no session, a guest not following): the game's own flow
    HostChapter,  // host of a lobby CHAPTER session
    HostStory,    // host who continues his own story (lobby STORY)
    Guest,        // guest following the host's story
};

// FailVerdict (0x0206) kinds
enum class FailVerdict : uint8_t {
    None = 0,
    HostFailed = 1,  // host -> every guest: the host's mission failed, wait for his reload
    Rejoin = 2,      // host -> the guest who reported: load my point now (I play on)
    Wait = 3,        // host -> the guest who reported: I am failing / reloading too, load my point when I am back
};

const char* FailVerdictName(FailVerdict v);

class FailFlow {
public:
    enum class State : uint8_t {
        Idle,
        HostReloading,  // host chapter: left for the main menu, the session checkpoint loads
        HostGameFail,   // host story (or no checkpoint known): the game's own fail screen / flow runs
        GuestAsking,    // guest: left for the main menu, failure reported, no verdict yet
        GuestRejoin,    // guest: load the host's point as soon as we are out of the world
        GuestWaitHost,  // guest: the host failed too, wait until he left his world and is back in it
        GuestLoading,   // guest: the host's point is loading (the normal follow runs)
    };

    enum class ActionKind : uint8_t {
        SendHostFailed,  // host: broadcast FailVerdict HostFailed (reason)
        SelfReload,      // host chapter: leave the world and load the session's checkpoint
        SendReport,      // guest: MissionFail (seq, reason) to the host
        Quit,            // guest: leave the world for the main menu now
        LoadHostPoint,   // guest (out of the world): load the host's point (forget which point we loaded)
        ReloadInWorld,   // guest still in a pre-failure world at release time: quit and load the host's point
    };
    struct Action {
        ActionKind kind;
        uint32_t seq = 0;
        uint32_t reason = 0;
    };

    // What the render thread knows each tick.
    struct View {
        bool inWorld = false;            // our player is in a world
        bool hostKnown = false;          // guest: a MissionState of the host arrived
        bool hostInWorld = false;        // guest: the host is in his world ...
        bool hostAtStoryPoint = false;   // ... at a chapter point (not a Free Ride stop, not the main menu)
    };

    struct LocalFailResult {
        bool skipGame = false;  // true: the game's own fail handling must not run (the hook does not call it)
        bool fresh = false;     // a new failure (false: a duplicate of the one being handled)
    };

    static constexpr double kAskTimeout = 6;          // s a guest waits for the host's verdict
    static constexpr double kHostReloadTimeout = 180;  // s until a host reload counts as done anyway
    static constexpr double kHostGameTimeout = 900;    // s a story host may sit at the fail screen
    static constexpr double kGuestWaitTimeout = 600;   // s a guest waits for the failed host to come back
    static constexpr double kGuestLoadTimeout = 180;   // s a guest's load of the host's point may take

    // The GameOver hook. role: what this game is now; canReload: host chapter: a checkpoint of this session is
    // known (else the game's own flow runs).
    LocalFailResult OnLocalFail(FailRole role, bool canReload, uint32_t reason, double now);
    // guest: FailVerdict from the host (seq: the report it answers; 0 for HostFailed)
    void OnVerdict(FailVerdict v, uint32_t seq, uint32_t reason, double now);
    // host: a guest reported a failure; the answer to send him
    FailVerdict Decide(bool inWorld, bool atStoryPoint) const;
    // every tick (render thread)
    void Update(const View& v, double now);
    // the session ended / a new one started
    void Reset();

    std::vector<Action> TakeActions();
    State GetState() const { return state_; }
    bool Busy() const { return state_ != State::Idle; }
    // guest: the normal follow must neither load nor reload (we wait for the host's verdict or his return)
    bool HoldsFollow() const;
    // a line for the HUD ("" = none)
    std::string Notice() const;
    static const char* StateName(State s);
    uint32_t Failures() const { return failures_; }  // fresh failures handled since start
    // log lines produced since the last call (the caller logs them)
    std::vector<std::string> TakeLog();

private:
    void Enter(State s, double now, const char* why);
    void Push(ActionKind k, uint32_t seq = 0, uint32_t reason = 0) { actions_.push_back({k, seq, reason}); }
    void Log(const std::string& s) { log_.push_back(s); }
    void Release(bool inWorld, double now, const char* why);

    State state_ = State::Idle;
    double since_ = 0;
    uint32_t seq_ = 0;          // guest: our last report
    uint32_t reason_ = 0;       // the failure being handled
    bool sawOut_ = false;       // we left our world since the failure
    bool hostSawOut_ = false;   // guest: the host left his world (or his point) since we started waiting
    bool lastInWorld_ = false;  // from the last Update (the hook runs between them)
    uint32_t failures_ = 0;
    std::vector<Action> actions_;
    std::vector<std::string> log_;
};

}  // namespace mc::game
