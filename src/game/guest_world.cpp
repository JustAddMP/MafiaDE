#include "game/guest_world.h"

#include <algorithm>
#include <cmath>

namespace mc::game::guest_world {
namespace {

constexpr float kPi = 3.14159265f;

float Dist2d(const Vec3& a, const Vec3& b) {
    const float dx = a.x - b.x, dy = a.y - b.y;
    return std::sqrt(dx * dx + dy * dy);
}

}  // namespace

const char* PhaseName(Phase p) {
    switch (p) {
        case Phase::None: return "none";
        case Phase::Loading: return "loading";
        case Phase::InWorld: return "world";
    }
    return "?";
}

PedSwitch PedSwitchOf(const std::string& ini) {
    std::string s;
    for (char c : ini)
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n') s += static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c);
    if (s == "block") return PedSwitch::Block;
    if (s == "off" || s == "0" || s == "none") return PedSwitch::Off;
    return PedSwitch::Budget;
}

const char* PedSwitchName(PedSwitch s) {
    switch (s) {
        case PedSwitch::Budget: return "budget";
        case PedSwitch::Block: return "block";
        case PedSwitch::Off: return "off";
    }
    return "?";
}

const char* HideModeName(HideMode m) {
    switch (m) {
        case HideMode::None: return "none";
        case HideMode::AtOnce: return "at-once";
        case HideMode::OutOfView: return "out-of-view";
    }
    return "?";
}

const char* VerdictName(Verdict v) {
    switch (v) {
        case Verdict::KeepOurs: return "ours";
        case Verdict::KeepOther: return "other";
        case Verdict::KeepInView: return "in-view";
        case Verdict::Hide: return "hide";
    }
    return "?";
}

uint32_t KeyHash(const std::string& s) {
    if (s.empty()) return 0;
    uint32_t h = 2166136261u;
    for (unsigned char c : s) {
        h ^= c;
        h *= 16777619u;
    }
    return h ? h : 1;
}

// ---- Policy ----------------------------------------------------------------------------------------------------------
Policy::Policy(PedSwitch peds, int budgetDefault)
    : peds_(peds), effectivePeds_(peds), budgetDefault_(budgetDefault >= 0 && budgetDefault <= kBudgetMax ? budgetDefault : -1) {}

void Policy::SetPedSwitch(PedSwitch s) {
    peds_ = s;
    // a budget call that failed once stays failed for this game (the binding is missing or refuses): the fallback
    effectivePeds_ = (budgetFailed_ && s == PedSwitch::Budget) ? PedSwitch::Block : s;
}

void Policy::OnBudgetRead(int value) {
    // 0 is what we set ourselves (a world that kept our value) or a game that has none: never taken as the value to go
    // back to. The newest believable reading wins (a load may have put the game's own value back).
    if (value > 0 && value <= kBudgetMax) savedBudget_ = value;
}

void Policy::OnSwitched(bool /*trafficOk*/, bool /*policeOk*/, bool budgetOk) {
    if (!askedBudget_) return;
    askedBudget_ = false;
    if (budgetOk) {
        budgetState_ = 1;
        return;
    }
    // the fallback: blocking the spawners around us from the next tick on
    budgetFailed_ = true;
    budgetState_ = 3;
    if (effectivePeds_ == PedSwitch::Budget) effectivePeds_ = PedSwitch::Block;
}

void Policy::OnRestored(bool budgetOk) {
    if (budgetState_ == 1) budgetState_ = budgetOk ? 2 : 3;
}

void Policy::TakeSpots(Actions& a) {
    a.unblock.insert(a.unblock.end(), spots_.begin(), spots_.end());
    spots_.clear();
    spotAt_ = -1e9;
}

void Policy::BlockTick(const Frame& f, Actions& a) {
    bool near = false;
    for (const Vec3& s : spots_)
        if (Dist2d(s, f.pos) < kBlockStep) {
            near = true;
            break;
        }
    if (!near) {
        a.block.push_back(f.pos);
        spots_.push_back(f.pos);
        spotAt_ = f.now;
    } else if (f.now - spotAt_ >= kBlockRepeat) {
        // the spot we stand on again: spawners streamed in since then get the flag too (it is one-shot per spawner,
        // docs/game-api.md 26); not remembered twice
        a.block.push_back(f.pos);
        spotAt_ = f.now;
    }
    if (spots_.size() > kBlockMax) {
        // the oldest spot far from us goes (its spawners are far away); with none that far, the oldest at twice the cap
        for (size_t i = 0; i < spots_.size(); ++i) {
            if (Dist2d(spots_[i], f.pos) >= kBlockForgetFar || spots_.size() > 2 * kBlockMax) {
                a.unblock.push_back(spots_[i]);
                spots_.erase(spots_.begin() + static_cast<std::ptrdiff_t>(i));
                break;
            }
        }
    }
}

Actions Policy::Tick(const Frame& f) {
    Actions a;
    // which world: a loading screen that comes up, a world entered from none, or a new key while in the world
    const Phase prev = phase_;
    bool newWorld = false;
    if (f.phase == Phase::Loading && prev != Phase::Loading)
        newWorld = true;
    else if (f.phase == Phase::InWorld && prev == Phase::None)
        newWorld = true;
    else if (f.phase == Phase::InWorld && prev == Phase::InWorld && f.worldKey != 0 && inWorldKey_ != 0 &&
             f.worldKey != inWorldKey_)
        newWorld = true;
    if (newWorld) {
        ++worlds_;
        worldStart_ = f.now;
        inWorldSince_ = -1;
    }
    if (f.phase == Phase::InWorld) {
        if (inWorldSince_ < 0) inWorldSince_ = f.now;
        if (f.worldKey) inWorldKey_ = f.worldKey;
    } else {
        inWorldSince_ = -1;
        if (f.phase == Phase::None) inWorldKey_ = 0;
    }
    phase_ = f.phase;

    const bool guest = f.active && !f.isHost && f.enabled;
    if (!guest) {
        // the session ended, we host now, or the module was turned off: everything back, once the game's world (or a
        // loading screen) is there to take the calls; a new session starts from a first apply again
        if (wasGuest_ && applied_) restorePending_ = true;
        wasGuest_ = false;
        applied_ = false;
        appliedWorld_ = 0;
        askedBudget_ = false;
        if (restorePending_ && f.phase != Phase::None) {
            restorePending_ = false;
            a.restore = true;
            a.why = f.active ? (f.isHost ? "we host now" : "turned off") : "the session ended";
            if (budgetState_ == 1) {
                a.budgetValue = savedBudget_ >= 0 ? savedBudget_ : budgetDefault_;
                if (a.budgetValue >= 0)
                    a.budgetRestore = true;
                else
                    budgetState_ = 4;  // not known: the game's own value comes back with the next world load
            }
            TakeSpots(a);
            ++restores_;
        }
        return a;
    }
    wasGuest_ = true;
    if (f.phase == Phase::None) return a;  // between worlds: the next one is switched on its first tick

    if (appliedWorld_ != worlds_) {
        // the first tick of this world (its loading screen, or the world itself)
        appliedWorld_ = worlds_;
        applied_ = true;
        restorePending_ = false;
        a.switchOff = true;
        a.why = applies_ == 0 ? "the first world of the session" : "a new world";
        ++applies_;
        reassertAt_ = f.now + kReassert;
    } else if (f.otherOn || f.now >= reassertAt_) {
        a.switchOff = true;
        a.why = f.otherOn ? "another part switched ambient traffic on" : "again";
        ++reasserts_;
        reassertAt_ = f.now + kReassert;
    }
    if (a.switchOff && effectivePeds_ == PedSwitch::Budget) {
        a.readBudget = true;
        a.budgetZero = true;
        askedBudget_ = true;
    }

    if (f.phase == Phase::Loading || (inWorldSince_ >= 0 && f.now - inWorldSince_ < kAtOnceFor))
        a.hide = HideMode::AtOnce;
    else
        a.hide = HideMode::OutOfView;
    const double every = a.hide == HideMode::AtOnce ? kScanAtOnce : kScanInterval;
    if (f.now - scanAt_ >= every) {
        scanAt_ = f.now;
        a.scan = true;
    }
    if (f.now - traceAt_ >= kScanInterval) {
        traceAt_ = f.now;
        a.trace = true;
    }
    if (effectivePeds_ == PedSwitch::Block && f.phase == Phase::InWorld && f.havePos) BlockTick(f, a);
    return a;
}

// ---- the verdict -------------------------------------------------------------------------------------------------------
bool InView(const View& v, const Vec3& p, float halfAngleDeg, float nearDist, float farDist) {
    if (!v.have) return true;
    const float dx = p.x - v.cam.x, dy = p.y - v.cam.y, dz = p.z - v.cam.z;
    const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (d < nearDist) return true;
    if (d > farDist) return false;
    const float dl = std::sqrt(v.dir.x * v.dir.x + v.dir.y * v.dir.y + v.dir.z * v.dir.z);
    if (dl < 1e-4f || d < 1e-4f) return true;
    const float c = (dx * v.dir.x + dy * v.dir.y + dz * v.dir.z) / (d * dl);
    return c >= std::cos(halfAngleDeg * kPi / 180.0f);
}

uint32_t WithView(uint32_t facts, const Vec3& pos, const View& view) {
    // cars are seen from farther than humans (a car popping away 200 m ahead shows; a pedestrian there hardly does)
    const bool car = (facts & kCar) != 0;
    const bool seen = car ? InView(view, pos, 60.0f, 8.0f, 250.0f) : InView(view, pos, 60.0f, 4.0f, 150.0f);
    return seen ? (facts | kInView) : (facts & ~static_cast<uint32_t>(kInView));
}

Verdict Judge(const Entity& e, HideMode mode, const View& view, const char** reason) {
    const char* dummy = nullptr;
    const char*& why = reason ? *reason : dummy;
    const uint32_t f = WithView(e.facts, e.pos, view);
    const bool car = (f & kCar) != 0, human = (f & kHuman) != 0;
    if (!(f & kActive)) {
        why = "inactive";
        return Verdict::KeepOther;
    }
    if (f & kOurs) {
        why = (f & kPuppet) ? "puppet"
              : (f & kPuppetFactory) ? "puppet factory"
              : (f & kTrafficCopy) ? "traffic copy"
              : (f & kCopyCrew) ? "copy crew"
              : (f & kPedOwned) ? "ped_sync's"
              : (f & kPedSpawner) ? "ped copy spawner"
              : (f & kNpcCopy) ? "npc_sync's"
              : (f & kNpcSpawner) ? "npc copy spawner"
              : (f & kPlayerCar) ? "player car"
                                 : "our car";
        return Verdict::KeepOurs;
    }
    if (car) {
        if (f & kFixed) {
            why = "fixed";
            return Verdict::KeepOurs;
        }
        if (view.haveMe && e.pos.z < view.me.z - kBelowUs) {
            why = "below us";
            return Verdict::KeepOurs;
        }
        if (f & kFactoryBusy) {
            why = "a factory is loading";
            return Verdict::KeepOurs;
        }
        if (f & kNearLoadingCopy) {
            why = "a copy's spawn spot";
            return Verdict::KeepOurs;
        }
        if (f & kFresh) {
            why = "fresh";
            return Verdict::KeepOurs;
        }
        if (f & kTramOrBoat) {
            why = "tram or boat";
            return Verdict::KeepOther;
        }
        if (!(f & kGeneratorDriver)) {
            why = "no generator driver";
            return Verdict::KeepOther;
        }
    } else if (human) {
        if (f & kFresh) {
            why = "fresh";
            return Verdict::KeepOurs;
        }
        if (f & kInVehicle) {
            why = "in a vehicle";
            return Verdict::KeepOther;
        }
        if (f & kMissionName) {
            why = "mission";
            return Verdict::KeepOther;
        }
        if (!(f & kAmbient)) {
            why = "not ambient";
            return Verdict::KeepOther;
        }
    } else {
        why = "neither car nor human";
        return Verdict::KeepOther;
    }
    // the guest's own ambient car / human
    if (mode == HideMode::None) {
        why = "not hiding";
        return Verdict::KeepInView;
    }
    if (mode == HideMode::OutOfView && (f & kInView) && !(car && (f & (kSpawnedLate | kPolice)))) {
        why = "in view";
        return Verdict::KeepInView;
    }
    why = mode == HideMode::AtOnce ? "at once" : "out of view";
    return Verdict::Hide;
}

bool CountsAsOwnCar(const Entity& e, Verdict v, const View& view) {
    if (!IsOwnAmbient(v) || !(e.facts & kCar) || e.speed <= kMovingSpeed) return false;
    return !view.haveMe || Dist2d(e.pos, view.me) <= kCountCarRange;
}

bool CountsAsOwnPed(const Entity& e, Verdict v, const View& view) {
    if (!IsOwnAmbient(v) || !(e.facts & kHuman)) return false;
    return !view.haveMe || Dist2d(e.pos, view.me) <= kCountPedRange;
}

}  // namespace mc::game::guest_world
