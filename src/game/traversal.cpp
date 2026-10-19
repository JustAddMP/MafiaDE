#include "game/traversal.h"

#include <algorithm>
#include <cmath>

namespace mc::game::traversal {

using actions::Action;
namespace motion = net::motion;

void LocalDetector::Reset() {
    nQueued_ = 0;
    have_ = false;
    pending_ = Pending::None;
    air_ = false;
    ladder_ = false;
    stillT_ = -1;
    pose_ = {};
}

void LocalDetector::Queue(Action a) {
    if (nQueued_ < 4) queued_[nQueued_++] = a;
}

Action LocalDetector::Update(const Sample& s) {
    if (!s.onFoot || s.inWater) {
        Reset();
        wasAway_ = true;
        exitT_ = s.t;
        return Action::None;
    }
    const double t = s.t;
    if (wasAway_) {  // on foot again (off a bike, out of a car or the water): his position settles first
        wasAway_ = false;
        exitT_ = t;
    }
    if (!have_) {  // the first sample: what he does now is not news
        have_ = true;
        prevVz_ = s.vz;
        prevCover_ = s.cover;
        prevCarry_ = s.carrying;
    }

    // messages
    if (s.events & kEvMedkit) Queue(Action::Medkit);
    if (s.events & kEvPickup) {
        if (t - pickupT_ > t_.bodyDedupe) Queue(Action::PickupBody);
        pickupT_ = t;
    }
    if (s.events & kEvDrop) {
        if (t - dropT_ > t_.bodyDedupe) Queue(Action::DropBody);
        dropT_ = t;
    }
    // the getter edge (in case a message is not sent to him; then the other way round, the message is the repeat)
    if (s.carrying && !prevCarry_) {
        if (t - pickupT_ > t_.bodyDedupe) Queue(Action::PickupBody);
        pickupT_ = t;
    } else if (!s.carrying && prevCarry_) {
        if (t - dropT_ > t_.bodyDedupe) Queue(Action::DropBody);
        dropT_ = t;
    }
    if (s.events & kEvFence) {
        pending_ = Pending::Fence;
        pendT_ = t;
        pendZ_ = s.z;
        moveT_ = t;
        air_ = false;
    }
    if (s.events & kEvLadder) {
        pending_ = Pending::Ladder;
        pendT_ = t;
        pendZ_ = s.z;
        moveT_ = t;
        air_ = false;
        ladder_ = true;
        ladderT_ = t;
        stillT_ = -1;
    }

    // a fence / ladder start: which move it is, from how far he rose since
    if (pending_ != Pending::None && t > pendT_) {
        const float rise = s.z - pendZ_;
        const bool timeUp = t - pendT_ >= t_.classifyTime;
        if (pending_ == Pending::Fence) {
            if (rise >= t_.climbRise) {
                Queue(Action::ClimbUp);
                pending_ = Pending::None;
            } else if (timeUp) {
                Queue(Action::Vault);
                pending_ = Pending::None;
            }
        } else {
            if (rise >= t_.ladderRise || (timeUp && rise > -t_.ladderRise)) {
                Queue(Action::LadderUp);
                pending_ = Pending::None;
            } else if (rise <= -t_.ladderRise) {
                Queue(Action::LadderDown);
                pending_ = Pending::None;
            }
        }
    }

    // on the ladder until he stops climbing
    if (ladder_) {
        const double age = t - ladderT_;
        if (age > t_.ladderMax) {
            ladder_ = false;
        } else if (age >= t_.ladderMin) {
            if (std::fabs(s.vz) < t_.ladderStill) {
                if (stillT_ < 0) stillT_ = t;
                if (t - stillT_ >= t_.ladderStillTime) ladder_ = false;
            } else {
                stillT_ = -1;
            }
        }
    }

    // jumps and falls, from his vertical speed (not during a fence / ladder move: they move him up and down too)
    // (not right after he got off a vehicle or out of the water either, and a vertical speed no jump or fall starts
    // with is a snap of his position: counted, and the quiet time starts again)
    const bool quiet = !ladder_ && pending_ == Pending::None && t - moveT_ >= t_.moveQuiet &&
                       t - exitT_ >= t_.exitQuiet;
    if (quiet && !air_) {
        if (s.vz > t_.jumpSpeed && prevVz_ <= t_.jumpSpeed) {
            if (s.vz > t_.maxJumpSpeed) {
                ++snaps_;
                exitT_ = t;
            } else {
                Queue(Action::Jump);
                air_ = true;
                airT_ = t;
                airMinVz_ = s.vz;
                sawFall_ = false;
            }
        } else if (s.vz < -t_.fallSpeed) {
            if (s.vz < -t_.snapFall) {
                ++snaps_;
                exitT_ = t;
            } else {
                Queue(Action::DropDown);
                air_ = true;
                airT_ = t;
                airMinVz_ = s.vz;
                sawFall_ = true;
            }
        }
    } else if (air_) {
        airMinVz_ = std::min(airMinVz_, s.vz);
        if (s.vz < -t_.landEnd) sawFall_ = true;
        if (ladder_ || (sawFall_ && s.vz > -t_.landEnd)) {  // down (or onto a ladder)
            air_ = false;
            if (!ladder_ && t - airT_ >= t_.minAir && airMinVz_ <= -t_.hardFall) Queue(Action::Land);
        }
    }

    // into cover at a run
    if (s.cover && !prevCover_ && s.speed >= t_.slideSpeed) Queue(Action::CoverSlide);

    pose_ = {};
    if (s.stealth) pose_.bits |= actions::kStealth;
    if (s.carrying) {
        pose_.bits |= actions::kCarry;
        pose_.body = s.body;
    }
    if (ladder_) pose_.bits |= actions::kLadder;

    prevVz_ = s.vz;
    prevCover_ = s.cover;
    prevCarry_ = s.carrying;

    if (nQueued_ == 0) return Action::None;
    const Action a = queued_[0];
    for (int i = 1; i < nQueued_; ++i) queued_[i - 1] = queued_[i];
    --nQueued_;
    return a;
}

// ---- receiver ------------------------------------------------------------------------------------------------------
void Catchup::Reset() {
    moving_ = false;
    moveT_ = 0;
    sinceMove_ = 1e9f;
}

Catchup::Info Catchup::Apply(bool moving, bool haveCopy, const motion::Vec3& copy, const motion::Vec3& target,
                             float dt, motion::LocomotionCommand& lc) {
    Info info;
    dt = std::clamp(dt, 0.0f, 0.5f);
    if (!haveCopy) return info;  // nothing known about the puppet: the controller's command as it is
    bool follow = moving;
    if (moving) {
        if (!moving_) {
            moving_ = true;
            moveT_ = 0;
            ++moves_;
            info.started = true;
        }
        moveT_ += dt;
        if (moveT_ > t_.maxMove) follow = false;  // a report stuck on "move": back to the controller
    }
    const float ex = target.x - copy.x, ey = target.y - copy.y, ez = target.z - copy.z;
    const float eh = std::sqrt(ex * ex + ey * ey);
    if (follow) {
        const float k = std::min(1.0f, t_.followGain * dt);
        lc.fix = motion::LocomotionCommand::Fix::Nudge;
        lc.nudge = {ex * k, ey * k, ez * k};
        lc.reissue = false;
        lc.turn = false;
        lc.error = eh;
        sinceMove_ = 0;
        info.moving = true;
        return info;
    }
    if (moving_ && !moving) {  // the move is over (a report that was stuck past maxMove ends without recovery)
        moving_ = false;
        if (moveT_ <= t_.maxMove) {
            info.ended = true;
            sinceMove_ = 0;
        } else {
            sinceMove_ = 1e9f;
        }
    } else {
        sinceMove_ += dt;
    }
    if (sinceMove_ >= t_.recoverTime) return info;
    info.recovering = true;
    const float k = std::min(1.0f, t_.blendGain * dt);
    if (lc.fix == motion::LocomotionCommand::Fix::Snap) {
        if (eh <= t_.recoverSnapDist && std::fabs(ez) <= t_.recoverSnapHeight) {
            lc.fix = motion::LocomotionCommand::Fix::Nudge;
            lc.nudge = {ex * k, ey * k, ez * k};
            info.softened = true;
            ++softened_;
        }
    } else {
        // what the walk does not close quickly: the part of the error beyond walkFree, faster than the controller's
        // own nudge (its carry nudge, the shortfall of a slow gait, is kept when it is the larger one)
        if (eh > t_.walkFree) {
            const float f = (eh - t_.walkFree) / eh * k;
            const float bx = ex * f, by = ey * f;
            if (bx * bx + by * by > lc.nudge.x * lc.nudge.x + lc.nudge.y * lc.nudge.y) {
                lc.nudge.x = bx;
                lc.nudge.y = by;
                lc.fix = motion::LocomotionCommand::Fix::Nudge;
            }
        }
        if (std::fabs(ez) > t_.zDead) {  // walking cannot climb a ledge
            lc.nudge.z += ez * k;
            lc.fix = motion::LocomotionCommand::Fix::Nudge;
        }
    }
    return info;
}

}  // namespace mc::game::traversal
