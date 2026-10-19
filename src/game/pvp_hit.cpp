// Player-versus-player hits on puppets (see pvp_hit.h).
#include "game/pvp_hit.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace mc::game::pvphit {

namespace {

float Clamp(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

}  // namespace

int ZoneAt(float z, float feetZ, float headZ) {
    if (headZ <= feetZ) headZ = feetZ + kHeadDefault;
    if (z >= headZ - 0.12f) return kHead;
    const float h = z - feetZ;
    if (h >= 1.0f) return kBody;
    if (h >= 0.75f) return kLowerBody;
    return kLeg;
}

RayHit RayBody(const Vec3& o, const Vec3& dIn, const Body& b, float radius, float maxRange) {
    RayHit r;
    const float len = std::sqrt(dIn.x * dIn.x + dIn.y * dIn.y + dIn.z * dIn.z);
    if (len < 1e-6f) return r;
    const Vec3 d{dIn.x / len, dIn.y / len, dIn.z / len};
    const float headZ = b.headZ > b.feet.z ? b.headZ : b.feet.z + kHeadDefault;
    const float z0 = b.feet.z + kFootLift, z1 = headZ + kHeadTop;
    const float segLen = z1 - z0;
    // closest points of the ray o + t d and the vertical segment (b.feet.x, b.feet.y, z0 + s), s in [0, segLen]
    const float wx = o.x - b.feet.x, wy = o.y - b.feet.y, wz = o.z - z0;
    const float bb = d.z;                          // d . up
    const float dd = d.x * wx + d.y * wy + d.z * wz;  // d . w
    const float denom = 1.0f - bb * bb;
    float t, s;
    if (denom < 1e-6f) {  // the ray is vertical: its horizontal distance is constant
        s = Clamp(wz, 0.0f, segLen);
        t = (z0 + s - o.z) * d.z;
    } else {
        t = (bb * wz - dd) / denom;
        s = wz + bb * t;  // (o + t d).z - z0
    }
    s = Clamp(s, 0.0f, segLen);
    t = Clamp((b.feet.x - o.x) * d.x + (b.feet.y - o.y) * d.y + (z0 + s - o.z) * d.z, 0.0f, maxRange);
    s = Clamp(o.z + t * d.z - z0, 0.0f, segLen);
    const float px = o.x + t * d.x - b.feet.x, py = o.y + t * d.y - b.feet.y, pz = o.z + t * d.z - (z0 + s);
    r.t = t;
    r.miss = std::sqrt(px * px + py * py + pz * pz);
    r.z = z0 + s;
    r.zone = ZoneAt(r.z, b.feet.z, headZ);
    r.hit = r.miss <= radius && t > 0.0f && t < maxRange;
    return r;
}

int PickShot(const Vec3& o, const Vec3& d, const std::vector<Body>& bodies, RayHit* out, float radius,
             float maxRange) {
    int best = -1;
    RayHit bestHit;
    for (size_t i = 0; i < bodies.size(); ++i) {
        const RayHit h = RayBody(o, d, bodies[i], radius, maxRange);
        if (h.hit && (best < 0 || h.t < bestHit.t)) {
            best = static_cast<int>(i);
            bestHit = h;
        }
    }
    if (out) *out = bestHit;
    return best;
}

int PickMelee(const Vec3& me, float fx, float fy, const std::vector<Body>& bodies, float reach, float cosMin) {
    const float fl = std::sqrt(fx * fx + fy * fy);
    if (fl < 1e-6f) return -1;
    fx /= fl, fy /= fl;
    int best = -1;
    float bestD = 0;
    for (size_t i = 0; i < bodies.size(); ++i) {
        const float dx = bodies[i].feet.x - me.x, dy = bodies[i].feet.y - me.y, dz = bodies[i].feet.z - me.z;
        const float dist = std::sqrt(dx * dx + dy * dy);
        if (dist > reach || dist < 1e-3f || std::fabs(dz) >= 1.2f) continue;
        if ((dx * fx + dy * fy) / dist < cosMin) continue;
        if (best < 0 || dist < bestD) best = static_cast<int>(i), bestD = dist;
    }
    return best;
}

Verdict Resolve(const ShotInput& in) {
    if (!in.friendlyFire) return Verdict::Off;
    if (in.gameDamage) return Verdict::Game;
    if (in.bulletEvent) return Verdict::Bullet;
    if (!in.rayHit) return Verdict::Miss;
    if (!in.hittable) return Verdict::NotHittable;
    if (in.blocked) return Verdict::Blocked;
    if (in.gameReports) return Verdict::RayUnconfirmed;
    return Verdict::Ray;
}

bool SendsHit(Verdict v) { return v == Verdict::Bullet || v == Verdict::Ray; }

const char* VerdictName(Verdict v) {
    switch (v) {
        case Verdict::Off: return "off";
        case Verdict::Game: return "game";
        case Verdict::Bullet: return "bullet";
        case Verdict::Ray: return "ray";
        case Verdict::RayUnconfirmed: return "ray_unconfirmed";
        case Verdict::Blocked: return "blocked";
        case Verdict::NotHittable: return "not_hittable";
        case Verdict::Miss: return "miss";
    }
    return "?";
}

float BulletDamage(const char* weapon) {
    if (!weapon || !*weapon) return 10.0f;
    auto starts = [weapon](const char* p) { return std::strncmp(weapon, p, std::strlen(p)) == 0; };
    if (starts("sg_")) return 30.0f;                       // shotguns: one shell (UNVERIFIED)
    if (starts("sr_") || starts("rif_") || starts("ar_")) return 25.0f;  // rifles (UNVERIFIED)
    if (starts("mg_")) return 12.0f;                       // machine guns (UNVERIFIED)
    return 10.0f;                                          // pistols, revolvers, SMGs (p_, rev_, smg_)
}

bool EvidenceMatches(float shotT, float evidenceT) {
    return evidenceT >= shotT - kEvidenceBefore && evidenceT <= shotT + kEvidenceAfter;
}

}  // namespace mc::game::pvphit
