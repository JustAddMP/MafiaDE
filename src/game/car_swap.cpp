#include "game/car_swap.h"

// No game or core dependencies: also linked into tests/car_swap_test.cpp.
namespace mc::game::carswap {

const char* PhaseName(Phase p) {
    switch (p) {
        case Phase::Driven: return "driven";
        case Phase::Parking: return "parking";
        case Phase::Done: return "done";
    }
    return "?";
}

const char* ActionName(Action a) {
    switch (a) {
        case Action::None: return "none";
        case Action::Restore: return "restore";
        case Action::RestoreHere: return "restore_here";
        case Action::RestoreWrecked: return "restore_wrecked";
        case Action::Forget: return "forget";
    }
    return "?";
}

Output Machine::Finish(Action a, const char* why) {
    phase_ = Phase::Done;
    lastWhy_ = why;
    Output o;
    o.action = a;
    o.phase = phase_;
    o.why = why;
    o.changed = true;
    return o;
}

Output Machine::Step(const Input& in) {
    if (in.Has(kBegin)) {
        phase_ = Phase::Driven;
        parkingSince_ = stillSince_ = otherSince_ = -1;
        lastWhy_ = nullptr;
    }
    Output o;
    if (phase_ == Phase::Done) {
        o.phase = phase_;
        o.why = "done";
        return o;
    }
    if (in.Has(kWorldGone))
        return Finish(in.Has(kOrigValid) ? Action::RestoreHere : Action::Forget,
                      in.Has(kOrigValid) ? "new world: our car is shown where it is" : "new world: our car is gone");
    if (!in.Has(kOrigValid)) return Finish(Action::Forget, "our car is gone");

    if (!in.Has(kCopyPending) && !in.Has(kCopyValid))
        return Finish(Action::Restore, "no copy: our car comes back at the last transform");

    const char* why = "";
    if (in.Has(kRemoteDriver) || in.Has(kLocalDriver)) {
        phase_ = Phase::Driven;
        parkingSince_ = stillSince_ = otherSince_ = -1;
        why = in.Has(kLocalDriver) ? "we drive the copy" : "a remote player drives the copy";
    } else if (in.Has(kCopyPending)) {
        why = "copy still spawning";
        o.lockCopy = true;
    } else {
        if (phase_ != Phase::Parking) {
            phase_ = Phase::Parking;
            parkingSince_ = in.now;
            stillSince_ = otherSince_ = -1;
        }
        o.lockCopy = !in.Has(kLocalInside);
        if (in.copySpeed < kStillSpeed) {
            if (stillSince_ < 0) stillSince_ = in.now;
        } else {
            stillSince_ = -1;
        }
        if (!in.Has(kOtherInside)) otherSince_ = -1;
        const bool parked = in.parkedAt >= 0 && in.parkedAt >= parkingSince_ - kParkSlack;
        const double waited = in.now - parkingSince_;
        if (in.Has(kLocalInside)) {
            why = "we sit in the copy";
        } else if (in.Has(kPuppetInside)) {
            why = "a puppet is still in the copy";
        } else if (in.Has(kRemoteAboard)) {
            why = "a remote player is getting in or out or rides in it";
        } else if (in.Has(kOtherInside) && (otherSince_ < 0 || in.now - otherSince_ < kOtherWait)) {
            if (otherSince_ < 0) otherSince_ = in.now;
            why = "somebody else is in the copy";
        } else if (parked) {
            return Finish(in.Has(kWrecked) ? Action::RestoreWrecked : Action::Restore,
                          in.Has(kWrecked) ? "parked, destroyed on the driver's game" : "parked");
        } else if (in.Has(kSessionOver) && (stillSince_ >= 0 || waited >= kStillTime * 3)) {
            return Finish(Action::Restore, "session over");
        } else if (waited >= kHardCap) {
            return Finish(Action::Restore, "no CarParked, the copy keeps moving: restored anyway");
        } else if (waited >= kParkFallback && stillSince_ >= 0 && in.now - stillSince_ >= kStillTime) {
            return Finish(Action::Restore, "no CarParked, the copy stands still");
        } else {
            why = "waiting for the car to be parked";
        }
    }
    o.phase = phase_;
    o.why = why;
    o.changed = why != lastWhy_;
    lastWhy_ = why;
    return o;
}

}  // namespace mc::game::carswap
