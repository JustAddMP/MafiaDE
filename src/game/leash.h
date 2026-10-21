#pragma once
// The distance leash (docs/host-authority.md section 3). Mafia simulates only around its own player: the host's world
// (his traffic, his pedestrians) ends a few hundred metres from him, so a guest who drives off finds empty streets. The
// leash keeps a guest near the host:
//
//   < warn (300 m)          nothing
//   warn .. max (400 m)     banner "FAR FROM THE HOST - THE STREETS THIN OUT (340 M)"
//   > max                   banner "TOO FAR FROM THE HOST - BACK IN 10 S", counting down; back under max - 20 m
//                           (the hysteresis) stops it
//   0 s                     the pull: the glue (player_sync.lua M.LeashPull) puts the guest beside the host, or his
//                           car on the road 15 m behind the host; then "PULLED BACK TO THE HOST" for 3 s
//
// Distance in the ground plane between the guest (his car when he drives) and the host's puppet on the guest's game.
// The numbers are the host's (ini leash_warn_m / leash_max_m / leash_countdown_s, sent to every guest in LeashConfig
// 0x0208); leash_countdown_s = 0 is warning only (no pull), leash_warn_m = 0 turns the leash off. The guest tells the
// host what happens (LeashEvent 0x0209), the host shows "<NAME> IS FAR AWAY (420 M)" in his status panel.
//
// Suspended (no warning, no countdown; docs/host-authority.md 3): our world not loaded and 5 s after; a cutscene and
// 5 s after; the host paused (the countdown freezes, banner "THE HOST PAUSED") or no state of his here; the host or we
// dead / downed / respawning; 15 s after the host jumped more than 50 m in one state (a mission teleport, his respawn,
// his puppet back after a gap); the join or reconnect not complete (JoinReport) and 20 s after; we sit in the host's car
// or a car he is in; we are a passenger of another guest's car (its driver's leash moves it); a fail-flow reload.
//
// Pure logic, no game or OS calls (tests/leash_test.cpp). game/leash_sync.cpp feeds it every script tick on a guest
// and carries out what it says.
#include "net/wire.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace mc::game::leash {

constexpr uint16_t kMsgLeashConfig = 0x0208;  // host -> guests, reliable: on join and on a change
constexpr uint16_t kMsgLeashEvent = 0x0209;   // guest -> host, reliable: warn / countdown / pulled / back

constexpr float kHysteresisM = 20.0f;   // back under warn - this ends the warning, under max - this the countdown
constexpr float kTeleportM = 50.0f;     // the host moved more than this in one state: a teleport
constexpr double kAfterLoadS = 5.0;     // suspended this long after our world loaded
constexpr double kAfterCutsceneS = 5.0; // ... after a cutscene ended
constexpr double kAfterTeleportS = 15.0;// ... after the host's teleport
constexpr double kAfterJoinS = 20.0;    // ... after the join / reconnect was complete
constexpr double kPulledBannerS = 3.0;  // "PULLED BACK TO THE HOST"
constexpr double kPullWaitS = 3.0;      // the glue did not answer the pull within this: failed
constexpr double kPullRetryS = 2.0;     // a failed pull is tried again after this
constexpr double kRepeatS = 5.0;        // the current event again while far (the host's panel keeps the distance)

// ---- LeashConfig 0x0208: u16 warnM, u16 maxM, u8 countdownS, u8 flags (bit0 1 = leash on) ---------------------------
struct Config {
    uint16_t warnM = 300;
    uint16_t maxM = 400;
    uint8_t countdownS = 10;  // 0 = warning only
    uint8_t flags = 1;        // bit0: on
    bool On() const { return (flags & 1) != 0 && warnM > 0; }
    bool PullOn() const { return On() && countdownS > 0 && maxM > 0; }
    bool operator==(const Config& o) const {
        return warnM == o.warnM && maxM == o.maxM && countdownS == o.countdownS && flags == o.flags;
    }
    bool operator!=(const Config& o) const { return !(*this == o); }
};
// The host's ini values (any int) as a config: clamped; warn 0 = off; a max under warn is raised to warn.
Config FromIni(int warnM, int maxM, int countdownS);
void Write(net::ByteWriter& w, const Config& c);
bool Read(net::ByteReader& r, Config& c);  // trailing bytes accepted (later additions)

// ---- LeashEvent 0x0209: u8 kind, u16 distM --------------------------------------------------------------------------
enum class EventKind : uint8_t { None = 0, Warn = 1, Countdown = 2, Pulled = 3, Back = 4 };
const char* EventName(EventKind k);
struct Event {
    EventKind kind = EventKind::None;
    uint16_t distM = 0;
};
void Write(net::ByteWriter& w, const Event& e);
bool Read(net::ByteReader& r, Event& e);

// ---- the guest's leash ----------------------------------------------------------------------------------------------
enum class State : uint8_t {
    Near,       // under the warning distance (or back under it)
    Warn,       // warn .. max (or beyond max with the pull off)
    Countdown,  // beyond max: counting down
    Paused,     // the host paused while we were far: the countdown is frozen
    Pulling,    // the pull is out, the glue has not answered yet
    Pulled,     // pulled back: the banner for kPulledBannerS
    Suspended,  // a suspension rule holds (why)
};
const char* StateName(State s);

enum class Why : uint8_t {
    None,
    Off,          // the host's config turns the leash off
    Loading,      // our world is not loaded (main menu, loading screen)
    AfterLoad,    // < 5 s after it loaded
    FailReload,   // a failed mission's reload is under way (src/game/fail_flow.h)
    Joining,      // our join / reconnect is not complete, or < 20 s after
    NoHost,       // no state of the host's here (he is not in the world, still loading, gone)
    HostPaused,   // the host's game is paused (PeerPaused)
    Cutscene,     // a cutscene plays (or the host's holds us), or < 5 s after
    SelfDown,     // we are dead / downed / respawning
    HostDown,     // the host is dead / downed
    HostTeleport, // < 15 s after the host jumped more than 50 m
    HostCar,      // we sit in the host's car or a car the host is in
    Passenger,    // we are a passenger in another guest's car: his leash moves it
};
const char* WhyName(Why w);

struct Input {
    double now = 0;            // s
    bool inWorld = false;      // our world is loaded
    bool hostSeen = false;     // the host's puppet's spot is known here
    float x = 0, y = 0;        // us (our car when we drive), ground plane
    float hostX = 0, hostY = 0;
    bool cutscene = false;
    bool hostPaused = false;
    bool hostDown = false, selfDown = false;
    bool joinSettled = true;   // the join / reconnect is complete (JoinReport) or its 15 s verdict is out
    bool hostCar = false, passenger = false;
    bool failReload = false;
};

struct Output {
    State state = State::Near;
    Why why = Why::None;
    float dist = -1;           // m, -1 = no host here
    double left = 0;           // countdown: s left
    bool pull = false;         // carry out the pull now (then PullDone)
    bool changed = false;      // state or why changed this update (trace LSH)
    std::vector<Event> events; // to the host (LeashEvent)
    std::string banner;        // "" = none
};

class Leash {
public:
    void SetConfig(const Config& c) { config_ = c; }
    const Config& GetConfig() const { return config_; }
    Output Update(const Input& in);
    // The glue's answer to a pull (ok: we were moved). A failed pull is tried again after kPullRetryS.
    void PullDone(bool ok, double now);
    void Reset();  // a new session (the config stays)

    State GetState() const { return state_; }
    uint32_t Pulls() const { return pulls_; }
    uint32_t Warnings() const { return warnings_; }
    uint32_t Countdowns() const { return countdowns_; }

private:
    Why Suspension(const Input& in, float dist);
    void Emit(Output& out, EventKind k, float dist);
    void Go(State s, double now) {
        state_ = s;
        stateAt_ = now;
    }

    Config config_;
    State state_ = State::Near;
    State resume_ = State::Near;  // Paused: the state to go back to
    Why why_ = Why::None;
    double stateAt_ = 0, lastNow_ = -1, left_ = 0, retryAt_ = -1e9, repeatAt_ = 0, pullAt_ = 0;
    double notInWorldAt_ = -1e9, cutsceneAt_ = -1e9, teleportAt_ = -1e9, settledAt_ = -1;
    bool havePrevHost_ = false, pullFailed_ = false, pendingPulled_ = false;
    float prevHostX_ = 0, prevHostY_ = 0, pullDist_ = 0;
    uint32_t pulls_ = 0, warnings_ = 0, countdowns_ = 0;
};

// The banner text for a state ("" none). dist in m, left in s.
std::string Banner(State s, float dist, double left);

// ---- after the pull: held on the spot while the ground there loads --------------------------------------------------
// A pull moves us hundreds of metres; our game may not have the ground at the new spot yet (2026-10-20 15:00 one-PC
// run, the second pull from 450 m: put down beside the host at z 2.86, we fell through the street - z 1.06, -3.5,
// -9.2, -15.2 within 1.4 s - and landed downed at health 1, our puppet on the host died with us and was spawned again
// 10 s later). For kSettleS after a pull, once we are more than kSettleDropM below the spot we are put back on it (the
// glue's HOLD), at most kSettleMax times; then the game's own fall goes on. 3 s / 8 holds were not enough in the
// 2026-10-21 06:25 one-PC run: pulled back in his car from 450 m, the guest's car was put back 5 times in 3 s (one
// hold every ~0.55 s: a 1.5 m fall), then fell through the street beside the host at up to 70 m/s and he was downed:
// the ground there loaded later than 3 s. Now 10 s (a hold every ~0.55 s while it lasts: about 18).
constexpr double kSettleS = 10.0;
constexpr float kSettleDropM = 1.5f;
constexpr int kSettleMax = 25;

class Settle {
public:
    void Start(double now, float x, float y, float z);  // a pull put us (our car) at x y z
    // Every tick: where we are now (z: our height, our car's while we drive). True: put us back on the spot now.
    bool Update(double now, float z);
    bool Active(double now) const { return now - at_ <= kSettleS && n_ < kSettleMax; }
    float X() const { return x_; }
    float Y() const { return y_; }
    float Z() const { return z_; }
    int Count() const { return n_; }          // holds after the last pull
    uint32_t Holds() const { return holds_; }  // so far
    void Reset() { *this = Settle(); }

private:
    double at_ = -1e9;
    float x_ = 0, y_ = 0, z_ = 0;
    int n_ = 0;
    uint32_t holds_ = 0;
};

// ---- the host: who gets the config ----------------------------------------------------------------------------------
// Every guest once per (id, joinSeq) (a join, a return after a lost connection), everyone again when the config
// changes.
class ConfigSender {
public:
    struct Plan {
        bool broadcast = false;
        std::vector<uint32_t> to;
    };
    Plan Tick(const Config& c, const std::vector<std::pair<uint32_t, uint32_t>>& guests);
    void Reset() {
        sent_.clear();
        haveLast_ = false;
    }

private:
    std::vector<std::pair<uint32_t, uint32_t>> sent_;
    Config last_;
    bool haveLast_ = false;
};

}  // namespace mc::game::leash
