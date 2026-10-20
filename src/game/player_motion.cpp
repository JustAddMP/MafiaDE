#include "game/player_motion.h"

#include <algorithm>
#include <cmath>

namespace mc::game::pmotion {
namespace {

float Len2(float x, float y) { return std::sqrt(x * x + y * y); }

}  // namespace

motion::LocomotionTuning FootTuning() {
    motion::LocomotionTuning t;
    // the game's AI human at multiplier 1 (0.15.0 traces, PUP base / measured walks: walk ~1.3, run ~2.2, sprint
    // ~3.3 m/s; the controller calibrates them at run time, these are the first commands' guesses)
    t.walkSpeed = 1.3f;
    t.runSpeed = 2.2f;
    t.sprintSpeed = 3.3f;
    // a standing player's puppet within 0.6 m stands at once (it is not walked the last bit: the rest blend slides
    // it onto his spot); farther off it walks there
    t.stopDist = t.startDist;
    return t;
}

// ---- PlayerTrack ----------------------------------------------------------------------------------------------------
namespace {
net::replica::StreamTuning StreamFor(const net::playout::PlayoutTuning& p) {
    net::replica::StreamTuning t;
    t.playout = p;
    // track defaults (32 samples, teleport 20 m, 250 ms extrapolation) but a lead of up to 4 m: a car at 30 m/s is
    // 1.5 m ahead after 50 ms, and the old 1.5 m cap left the copy behind it at speed
    t.track.maxLead = 4.0f;
    return t;
}
}  // namespace

PlayerTrack::PlayerTrack(const net::playout::PlayoutTuning& p) : stream_(StreamFor(p)) {}

void PlayerTrack::Reset() {
    stream_.Reset();
    haveCar_ = false;
    carKey_ = 0;
}

void PlayerTrack::Observe(double senderTime, double localTime) { stream_.Observe(senderTime, localTime); }

bool PlayerTrack::PushFoot(double t, const Vec3& pos, const Vec3& vel, uint32_t bits) {
    replica::Sample s;
    s.t = t;
    s.pos = pos;
    s.vel = vel;
    s.bits = bits;
    return stream_.Push(kFoot, s);
}

bool PlayerTrack::PushCar(double t, uint64_t carKey, const Vec3& pos, const Vec3& vel, const float rot[4]) {
    if (!haveCar_ || carKey != carKey_) {
        if (replica::SnapshotTrack* tr = stream_.Track(kCar)) tr->Clear();  // another car: never blended into
        carKey_ = carKey;
        haveCar_ = true;
    }
    replica::Sample s;
    s.t = t;
    s.pos = pos;
    s.vel = vel;
    for (int i = 0; i < 4; ++i) s.rot[i] = rot[i];
    s.hasRot = true;
    return stream_.Push(kCar, s);
}

void PlayerTrack::NoCar() { haveCar_ = false; }

void PlayerTrack::Update(double localNow) { stream_.Update(localNow); }

void PlayerTrack::SetOneWay(double s) { oneWay_ = std::clamp(s, 0.0, 0.15); }

double PlayerTrack::Lead() const { return std::max(0.0, stream_.Delay()) + oneWay_; }

replica::Out PlayerTrack::Foot(double localNow) const {
    const replica::SnapshotTrack* tr = stream_.Track(kFoot);
    return tr ? tr->Present(stream_.RenderTime(localNow), Lead()) : replica::Out{};
}

replica::Out PlayerTrack::Car(double localNow) const {
    const replica::SnapshotTrack* tr = stream_.Track(kCar);
    return tr ? tr->Present(stream_.RenderTime(localNow), Lead()) : replica::Out{};
}

// ---- FootDriver -----------------------------------------------------------------------------------------------------
FootDriver::FootDriver(const motion::LocomotionTuning& t, const RestTuning& r, const FootFixTuning& f)
    : loco_(t), t_(t), r_(r), f_(f) {}

float FootDriver::CopySpeed() const { return Len2(vel_[0], vel_[1]); }

void FootDriver::Applied(const motion::LocomotionCommand& c) {
    if (c.fix == motion::LocomotionCommand::Fix::Snap) {
        haveLast_ = false;
        lastNudge_ = {};
    } else {
        lastNudge_ = c.fix == motion::LocomotionCommand::Fix::Nudge ? c.nudge : Vec3{};
        loco_.Applied(lastNudge_);
    }
}

motion::LocomotionCommand FootDriver::Update(const motion::LocomotionInput& in) {
    using Fix = motion::LocomotionCommand::Fix;
    motion::LocomotionCommand c = loco_.Update(in);
    resting_ = false;
    const float dt = std::clamp(in.dt, 0.0f, 0.5f);
    sinceIssue_ += dt;
    if (!in.haveCopy) {
        haveLast_ = false;
        issuedMult_ = -1;
        return c;
    }
    // the copy's own walking velocity (its nudges and snaps are not walking)
    if (haveLast_ && dt > 1e-4f) {
        const float vx = (in.copy.x - last_.x - lastNudge_.x) / dt, vy = (in.copy.y - last_.y - lastNudge_.y) / dt;
        const float a = std::min(1.0f, dt / 0.1f);
        vel_[0] += (vx - vel_[0]) * a;
        vel_[1] += (vy - vel_[1]) * a;
    } else if (!haveLast_) {
        vel_[0] = vel_[1] = 0;
    }
    last_ = in.copy;
    haveLast_ = c.fix != Fix::Snap;
    lastNudge_ = {};
    const bool snap = c.fix == Fix::Snap;

    // the walk command: a stop goes to where the copy comes to rest by itself; a changed multiplier re-issues
    if (c.gait == motion::Gait::Stand) {
        if (c.reissue && !snap) {
            // On the copy's own line of motion: as far as his spot lies along it, at most where its own velocity
            // takes it in stopAhead s, never behind it (a point behind a running copy would turn it round; a
            // standing copy is not walked the last bit, a shuffle). The rest blend does the remainder.
            const float own = Len2(vel_[0], vel_[1]);
            float along = 0;
            if (own > 0.2f) {
                const float dx = vel_[0] / own, dy = vel_[1] / own;
                along = std::clamp((in.target.x - in.copy.x) * dx + (in.target.y - in.copy.y) * dy, 0.0f,
                                   own * f_.stopAhead);
                c.moveTo = {in.copy.x + dx * along, in.copy.y + dy * along, in.copy.z};
            } else {
                c.moveTo = in.copy;
            }
        }
        issuedMult_ = -1;
    } else {
        if (!c.reissue && issuedMult_ >= 0 && std::fabs(c.speedMult - issuedMult_) > f_.multReissue &&
            sinceIssue_ >= f_.reissueMin) {
            c.reissue = true;
            ++multReissues_;
        }
        if (c.reissue) issuedMult_ = c.speedMult;
    }
    if (c.reissue) sinceIssue_ = 0;
    if (snap || in.inAir) {
        lastNudge_ = c.nudge;
        return c;
    }

    // The position fix: velocity matching. The puppet as a whole is to move with his velocity plus a share of the
    // error; what its own walk does not give (its measured walking velocity, our fixes left out) is added as a nudge
    // this frame - so it keeps up whether the game's AI walks the commanded speed, a fraction of it or overshoots (the
    // 2026-10-20 two-PC traces: the AI walked 0.6-2.2 m/s at RUN x1.8 while nudged every frame, the old carry + blend
    // left it 0.6-0.8 m behind a runner, p95 1.4-1.5 m). Standing: the rest blend (slow, only beyond its dead zone),
    // and a walk that still runs on after his stop is held back to his spot.
    const float ex = in.target.x - in.copy.x, ey = in.target.y - in.copy.y;
    const float err = Len2(ex, ey);
    const float tspeed = Len2(in.targetVel.x, in.targetVel.y);
    const float own = Len2(vel_[0], vel_[1]);
    const bool standing = c.gait == motion::Gait::Stand && tspeed < r_.standSpeed && in.gaitHint <= 0;
    float ux = 0, uy = 0;  // m/s added by the nudge
    braking_ = false;
    if (standing) {
        // the rest blend: within its dead zone nothing, beyond at most r_.speed (more while the copy still moves)
        const float step = net::correction::BlendStep(err, r_.deadZone, r_.gain, r_.share, r_.speed, own, dt);
        resting_ = step > 0;
        restMetres_ += step;
        if (err > 1e-4f && dt > 1e-5f) {
            ux = ex / err * step / dt;
            uy = ey / err * step / dt;
        }
        // he creeps (slower than a walk, his game says idle): the puppet creeps with him
        if (err > r_.deadZone) {
            ux += in.targetVel.x;
            uy += in.targetVel.y;
        }
        // its walk still runs on (the AI's lag after his stop): that part is taken back, so it stops where he stands
        if (own > f_.holdMin) {
            const float dx = vel_[0] / own, dy = vel_[1] / own;
            if (ex * dx + ey * dy < f_.brakeBehind) {  // level with his spot or past it
                const float hold = std::min(own, f_.holdShare * own + f_.holdMin);
                ux -= dx * hold;
                uy -= dy * hold;
                braking_ = true;
                brakeMetres_ += hold * dt;
            }
        }
    } else {
        // moving: his velocity + trackGain x the error beyond trackDead (at most trackCorr m/s), minus its own walk
        float cx = 0, cy = 0;
        if (err > f_.trackDead) {
            const float k = std::min(f_.trackGain * (err - f_.trackDead), f_.trackCorr) / err;
            cx = ex * k;
            cy = ey * k;
        }
        ux = in.targetVel.x + cx - vel_[0];
        uy = in.targetVel.y + cy - vel_[1];
        const float ul = Len2(ux, uy), cap = f_.slideBase + f_.slideShare * std::max(tspeed, own);
        if (ul > cap) {
            ux *= cap / ul;
            uy *= cap / ul;
        } else if (err < f_.quietDist && ul < f_.quietSpeed) {
            ux = uy = 0;  // close and its walk keeps pace: left to its own feet
        }
        braking_ = ux * vel_[0] + uy * vel_[1] < -0.5f * own * own;  // held back by more than half its own speed
        if (braking_) brakeMetres_ += Len2(ux, uy) * dt;
    }
    c.nudge = {ux * dt, uy * dt, c.nudge.z};
    const bool moved = std::fabs(c.nudge.x) + std::fabs(c.nudge.y) + std::fabs(c.nudge.z) > 1e-6f;
    c.fix = moved ? Fix::Nudge : Fix::None;
    if (!moved) c.nudge = {};
    lastNudge_ = c.nudge;
    nudgeMetres_ += Len2(c.nudge.x, c.nudge.y);
    loco_.Applied(c.nudge);  // its walk measurement (the calibration) leaves out this nudge, not its own
    return c;
}

// ---- CarDriver ------------------------------------------------------------------------------------------------------
net::correction::Tuning CarPreset() {
    net::correction::Tuning t = net::correction::Tuning::ForCar();
    t.gain = 5.0f;
    t.speedShare = 0.5f;
    return t;
}

bool CarForward(const float q[4], float out[2]) {
    // v' = v + 2w (u x v) + 2 u x (u x v) with u = -q.xyz (the conjugate), w = q.w, v = (0, 1, 0)
    const float x = -q[0], y = -q[1], z = -q[2], w = q[3];
    const float tx = 2 * (-z), ty = 0, tz = 2 * x;  // t = 2 (u x v)
    const float fx = 0 + w * tx + (y * tz - z * ty);
    const float fy = 1 + w * ty + (z * tx - x * tz);
    const float l = Len2(fx, fy);
    if (!(l > 0.2f)) return false;
    out[0] = fx / l;
    out[1] = fy / l;
    return true;
}

CarDriver::CarDriver(const motion::VehicleTuning& v, const CarFixTuning& f, const net::correction::Tuning& c)
    : veh_(v), vt_(v), fix_(c), ct_(c), f_(f) {}

void CarDriver::Reset() {
    veh_.Reset();
    fix_.Reset();
    sinceSpeed_ = 1e9f;
    skid_ = wasSkid_ = false;
}

CarCommand CarDriver::Update(const motion::VehicleInput& in) {
    using Fix = motion::VehicleCommand::Fix;
    CarCommand o;
    o.vc = veh_.Update(in);
    const float dt = std::clamp(in.dt, 0.0f, 0.5f);
    sinceSpeed_ += dt;
    if (!in.haveCopy) return o;
    if (o.vc.fix == Fix::Pin || o.vc.fix == Fix::Snap) {
        fix_.Reset();  // its physics is not trusted now: the preset starts fresh once it is
        sinceSpeed_ = 0;  // the glue gives the speed with every move
        return o;
    }
    // Its own physics carries it: the car preset decides about the position (nothing inside 0.5 m, a blend hidden in
    // the motion beyond, a snap when far / stuck / the target jumped); the rotation is blended only when it is
    // clearly off, so most ticks set nothing on the copy.
    // his car slides (velocity off its heading): a sliding correction is hidden in it
    skid_ = false;
    float fwd[2];
    const float tsp = Len2(in.targetVel.x, in.targetVel.y);
    if (tsp > f_.skidSpeed && CarForward(in.targetRot, fwd)) {
        const float c = (in.targetVel.x * fwd[0] + in.targetVel.y * fwd[1]) / tsp;
        skid_ = c < std::cos(f_.skidDeg * 3.14159265f / 180);
    }
    net::correction::Tuning ct = fix_.GetTuning();
    ct.speedShare = skid_ ? f_.skidShare : ct_.speedShare;
    fix_.SetTuning(ct);
    o.corr = fix_.Update(in.copy, in.target, in.targetVel, dt);
    o.skid = skid_;
    o.skidStart = skid_ && !wasSkid_;
    wasSkid_ = skid_;
    if (o.corr.kind == net::correction::Result::Kind::Snap) {
        o.vc.fix = Fix::Snap;
        o.vc.nudge = {};
        for (int i = 0; i < 4; ++i) o.vc.rot[i] = in.targetRot[i];
    } else if (o.corr.kind == net::correction::Result::Kind::Blend) {
        o.vc.fix = Fix::Blend;  // o.vc.rot: the controller's rotation step
        o.vc.nudge = o.corr.delta;
    } else {
        o.vc.nudge = {};
        o.vc.fix = o.vc.rotErrDeg > f_.rotDeadDeg ? Fix::Blend : Fix::None;
    }
    // The speed the copy is given (SetSpeed: a number, along its own heading): his speed plus the error along the
    // copy's heading only (VehicleController's velocity correction is a vector; a sideways error given as speed along
    // the heading made a copy in a turn run wide - the position blend takes the sideways part)
    float cf[2];
    const float tv = std::sqrt(in.targetVel.x * in.targetVel.x + in.targetVel.y * in.targetVel.y +
                               in.targetVel.z * in.targetVel.z);
    if (CarForward(in.copyRot, cf)) {
        const float along = (in.target.x - in.copy.x) * cf[0] + (in.target.y - in.copy.y) * cf[1];
        const float corr = std::clamp(along * vt_.velGain, -vt_.maxVelCorr, vt_.maxVelCorr);
        const float speed = std::max(0.0f, tv + corr);
        if (tv > 0.1f) {
            const float k = speed / tv;
            o.vc.vel = {in.targetVel.x * k, in.targetVel.y * k, in.targetVel.z * k};
        } else {
            o.vc.vel = {cf[0] * speed, cf[1] * speed, 0};
        }
    }
    if (o.vc.fix != Fix::None) {
        sinceSpeed_ = 0;
        return o;
    }
    // Nothing moved: is its own speed off the driver's? Then it is given his (wanted) speed alone.
    const float want = std::sqrt(o.vc.vel.x * o.vc.vel.x + o.vc.vel.y * o.vc.vel.y + o.vc.vel.z * o.vc.vel.z);
    const float off = std::fabs(o.vc.cspeed - want);
    if (off > std::max(f_.speedDead, f_.speedShare * o.vc.tspeed) && sinceSpeed_ >= f_.speedEvery) {
        o.giveSpeed = true;
        sinceSpeed_ = 0;
        ++speedFixes_;
    }
    return o;
}

}  // namespace mc::game::pmotion
