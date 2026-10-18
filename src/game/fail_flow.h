#pragma once
// A failed mission in co-op: one path for every kind of failure, one load on every game (docs/protocol.md "Failed
// missions", docs/re-notes.md 7).
//
// In a chapter loaded through the menu's chapter loader (every co-op load: replay flags) the game answers every
// failure (the objective abandoned, the mission car lost, a timer run out, an ally dead, arrested ...) and a death it
// handles itself with one function, the save system's FreeRoamRetry (exe+0x21E8B20: load the free roam state from
// before the quest), called by the game-over controller (C_GameOverController2, when a checkpoint retry is not
// possible) and by the flows' free_roam_retry node. That load, then our reload of the checkpoint from the main menu,
// were the two loads of a failed mission (docs/re-notes.md 7). mission_sync hooks it and asks this state machine:
//   - host of a lobby CHAPTER (his saves are kept out of his slots): the game's answer is skipped; he leaves for the
//     main menu at once and loads the session's checkpoint (one load). The guests are told (HostFailed) and follow;
//   - host of a STORY session (normal story loads: the game shows its fail screen, Load Last Checkpoint is one load):
//     nothing is skipped; when the director's GameOver tells us of the failure the guests are told too;
//   - guest: the game's answer is skipped, he leaves for the main menu at once and reports the failure to the host,
//     who decides: REJOIN (the host plays on: load his point now) or WAIT (the host failed too: load his point once he
//     is back in his world). No answer within kAskTimeout: REJOIN.
// Duplicates (the game asks twice per failure; a death during a failure) are skipped while a failure is handled. If
// we are still in the failed world kStuckTimeout after skipping (the quit did not happen), the game's own answer runs
// after all (GameFallback). Pure logic, no game calls: unit-tested in tests/fail_flow_test.cpp. Not thread-safe: the
// caller locks (the hooks run on GameMainThread, the rest on the render thread).
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
        GameFallback,    // the quit did not happen: run the game's own answer that was skipped (FreeRoamRetry)
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
    static constexpr double kHostStayTimeout = 90;     // s a guest waits for a failed host who never leaves his world
    static constexpr double kStuckTimeout = 20;        // s after a skipped answer we may still be in the failed world

    // The GameOver hook. role: what this game is now; canReload: host chapter: a checkpoint of this session is
    // known (else the game's own flow runs).
    LocalFailResult OnLocalFail(FailRole role, bool canReload, uint32_t reason, double now);
    // guest: FailVerdict from the host (seq: the report it answers; 0 for HostFailed)
    void OnVerdict(FailVerdict v, uint32_t seq, uint32_t reason, double now);
    // host: a guest reported a failure; the answer to send him
    FailVerdict Decide(bool inWorld, bool atStoryPoint) const;
    // every tick (render thread)
    void Update(const View& v, double now);
    // the game loads the free roam state although we skipped its answer (another caller): true = our flow gave up
    // (the old way back, the Free Ride stop and a reload, takes over)
    bool OnGameLoads(double now);
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
    bool skipped_ = false;      // the game's own answer to this failure was skipped
    double failAt_ = 0;
    uint32_t failures_ = 0;
    std::vector<Action> actions_;
    std::vector<std::string> log_;
};

}  // namespace mc::game
