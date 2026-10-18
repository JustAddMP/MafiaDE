#include "game/fail_flow.h"

#include <cstdio>
#include <utility>

namespace mc::game {

const char* FailVerdictName(FailVerdict v) {
    switch (v) {
    case FailVerdict::HostFailed: return "HOST_FAILED";
    case FailVerdict::Rejoin: return "REJOIN";
    case FailVerdict::Wait: return "WAIT";
    default: return "NONE";
    }
}

const char* FailFlow::StateName(State s) {
    switch (s) {
    case State::Idle: return "idle";
    case State::HostReloading: return "host-reloading";
    case State::HostGameFail: return "host-game-fail";
    case State::GuestAsking: return "guest-asking";
    case State::GuestRejoin: return "guest-rejoin";
    case State::GuestWaitHost: return "guest-wait-host";
    case State::GuestLoading: return "guest-loading";
    }
    return "?";
}

void FailFlow::Enter(State s, double now, const char* why) {
    if (s == state_) return;
    char buf[160];
    std::snprintf(buf, sizeof buf, "fail flow: %s -> %s (%s)", StateName(state_), StateName(s), why);
    Log(buf);
    state_ = s;
    since_ = now;
}

FailFlow::LocalFailResult FailFlow::OnLocalFail(FailRole role, bool canReload, uint32_t reason, double now) {
    LocalFailResult r;
    char buf[200];
    // A new failure only from Idle, or (guest) once our load of the host's point is in the world again (a check that
    // fires again right after the reload is a new failure). Anything else is the failure we handle already.
    const bool fresh = state_ == State::Idle || (state_ == State::GuestLoading && lastInWorld_ && sawOut_);
    if (!fresh) {
        r.skipGame = state_ != State::HostGameFail;  // the story host's game handles its own duplicates
        std::snprintf(buf, sizeof buf, "fail flow: GameOver %u again while %s: %s", reason, StateName(state_),
                      r.skipGame ? "skipped" : "left to the game");
        Log(buf);
        return r;
    }
    reason_ = reason;
    sawOut_ = false;
    hostSawOut_ = false;
    switch (role) {
    case FailRole::None:
        std::snprintf(buf, sizeof buf, "fail flow: GameOver %u outside a co-op story: the game's own flow", reason);
        Log(buf);
        return r;
    case FailRole::HostChapter:
        ++failures_;
        r.fresh = true;
        Push(ActionKind::SendHostFailed, 0, reason);
        if (canReload) {
            r.skipGame = true;
            Push(ActionKind::SelfReload, 0, reason);
            Enter(State::HostReloading, now, "our mission failed: straight back to the session's checkpoint");
        } else {
            Enter(State::HostGameFail, now, "our mission failed, no checkpoint of this session known: the game's flow");
        }
        return r;
    case FailRole::HostStory:
        ++failures_;
        r.fresh = true;
        Push(ActionKind::SendHostFailed, 0, reason);
        Enter(State::HostGameFail, now, "our mission failed in our own story: the game's fail screen");
        return r;
    case FailRole::Guest:
        ++failures_;
        r.fresh = true;
        r.skipGame = true;
        ++seq_;
        Push(ActionKind::SendReport, seq_, reason);
        if (lastInWorld_) Push(ActionKind::Quit, seq_, reason);
        else sawOut_ = true;
        Enter(State::GuestAsking, now, "our mission failed: reported to the host, leaving the world");
        return r;
    }
    return r;
}

void FailFlow::OnVerdict(FailVerdict v, uint32_t seq, uint32_t reason, double now) {
    char buf[200];
    std::snprintf(buf, sizeof buf, "fail flow: verdict %s (report %u, reason %u) while %s", FailVerdictName(v), seq,
                  reason, StateName(state_));
    Log(buf);
    switch (v) {
    case FailVerdict::HostFailed:
        // the host's failure wins over ours: everyone loads the host's reloaded checkpoint
        if (state_ == State::GuestWaitHost) return;
        if (state_ == State::Idle || state_ == State::GuestLoading) {
            reason_ = reason;
            sawOut_ = !lastInWorld_;
            if (lastInWorld_) Push(ActionKind::Quit, 0, reason);
        }
        hostSawOut_ = false;
        Enter(State::GuestWaitHost, now, "the host's mission failed: waiting for him to be back at his checkpoint");
        return;
    case FailVerdict::Rejoin:
    case FailVerdict::Wait:
        if (state_ != State::GuestAsking || seq != seq_) return;  // an old or unasked answer
        if (v == FailVerdict::Rejoin) {
            Enter(State::GuestRejoin, now, "the host plays on: loading his point");
        } else {
            hostSawOut_ = false;
            Enter(State::GuestWaitHost, now, "the host fails too: waiting for him to be back at his checkpoint");
        }
        return;
    default:
        return;
    }
}

FailVerdict FailFlow::Decide(bool inWorld, bool atStoryPoint) const {
    if (state_ == State::HostReloading || state_ == State::HostGameFail) return FailVerdict::Wait;
    if (!inWorld || !atStoryPoint) return FailVerdict::Wait;
    return FailVerdict::Rejoin;
}

void FailFlow::Release(bool inWorld, double now, const char* why) {
    if (!inWorld) {
        Push(ActionKind::LoadHostPoint, seq_, reason_);
        Enter(State::GuestLoading, now, why);
    } else if (sawOut_) {
        // we left the failed world and are in one loaded since (a load of the host's point that was under way)
        Enter(State::Idle, now, "already in a world loaded since the failure");
    } else {
        Push(ActionKind::ReloadInWorld, seq_, reason_);
        Enter(State::GuestLoading, now, why);
    }
}

void FailFlow::Update(const View& v, double now) {
    lastInWorld_ = v.inWorld;
    if (state_ == State::Idle) return;
    if (!v.inWorld) sawOut_ = true;
    const double age = now - since_;
    switch (state_) {
    case State::HostReloading:
        if (sawOut_ && v.inWorld) Enter(State::Idle, now, "back in the world after the reload");
        else if (age > kHostReloadTimeout) Enter(State::Idle, now, "the reload took too long: given up");
        return;
    case State::HostGameFail:
        if (sawOut_ && v.inWorld) Enter(State::Idle, now, "back in a world after the game's fail flow");
        else if (age > kHostGameTimeout) Enter(State::Idle, now, "still at the fail screen: given up");
        return;
    case State::GuestAsking:
        if (age > kAskTimeout) Enter(State::GuestRejoin, now, "no verdict from the host: loading his point");
        return;
    case State::GuestRejoin:
        if (sawOut_ || age > 15) Release(v.inWorld, now, "loading the host's point");
        return;
    case State::GuestWaitHost:
        if (!v.hostKnown || !v.hostInWorld || !v.hostAtStoryPoint) hostSawOut_ = true;
        if (hostSawOut_ && v.hostInWorld && v.hostAtStoryPoint) {
            Release(v.inWorld, now, "the host is back at his checkpoint: loading it");
        } else if (age > kGuestWaitTimeout) {
            Enter(State::Idle, now, "the host did not come back: the normal follow takes over");
        }
        return;
    case State::GuestLoading:
        if (sawOut_ && v.inWorld && age > 1) Enter(State::Idle, now, "at the host's point again");
        else if (age > kGuestLoadTimeout) Enter(State::Idle, now, "the load took too long: the normal follow takes over");
        return;
    default:
        return;
    }
}

bool FailFlow::HoldsFollow() const {
    return state_ == State::GuestAsking || state_ == State::GuestRejoin || state_ == State::GuestWaitHost;
}

std::string FailFlow::Notice() const {
    switch (state_) {
    case State::HostReloading: return "MISSION FAILED: BACK TO THE CHECKPOINT";
    case State::GuestAsking:
    case State::GuestRejoin:
    case State::GuestLoading: return "MISSION FAILED: BACK TO THE HOST'S CHECKPOINT";
    case State::GuestWaitHost: return "THE MISSION FAILED: WAITING FOR THE HOST'S CHECKPOINT";
    default: return {};
    }
}

void FailFlow::Reset() {
    if (state_ != State::Idle) Log(std::string("fail flow: reset while ") + StateName(state_));
    state_ = State::Idle;
    since_ = 0;
    sawOut_ = hostSawOut_ = false;
    actions_.clear();
}

std::vector<FailFlow::Action> FailFlow::TakeActions() { return std::exchange(actions_, {}); }
std::vector<std::string> FailFlow::TakeLog() { return std::exchange(log_, {}); }

}  // namespace mc::game
