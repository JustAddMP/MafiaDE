#pragma once
// Player-versus-player hits on puppets: what our shot or punch hit when the game itself does not say. Pure logic, no
// game or Lua calls: unit-tested by tests/pvp_hit_test.cpp; player_sync.lua ("Damage between players", M.ShotTick and
// M.MeleeTick) asks it through mc_pvp_ray / mc_pvp_pick / mc_pvp_verdict / mc_pvp_damage (src/game/player_sync.cpp).
//
// Why (docs/game-api.md 11 and 17, 0.15.0 two-PC test): every puppet gets the FRIEND brain (player_sync.lua M.Ally,
// SwitchBrain(1): AI type 1, the player's own side) so it keeps its car seats and the host's enemies target it. The
// game does not hurt or melee-target its player's friends: a friend-brain puppet gets no DAMAGE event from our bullets
// (about 20 shots, no Hit) and our punches never lock on to it (opponent 0 on every swing). The exe decides
// friend / enemy from a faction table indexed by AI type (exe 0x25E0700 "friend" = table value > 0, 0x24AFB90 "enemy"
// = value < 0; AI type 0 -> faction 3, 1 -> 0, 2 -> 1, 4 -> 2, 17 -> 3; MimicFriend(true) sets byte +0x1FD of the
// AI, which makes both answer "friend"); docs/game-api.md 17 "Shooting and punching a puppet". So our own side
// decides what we hit, in this order (the first one with evidence wins, each piece of evidence counts once):
//   1. the game's own DAMAGE of our bullet on a puppet (it does come for a puppet whose brain switch is still to come,
//      AI type 0): already sent as a Hit by M.OnPuppetDamage, nothing more;
//   2. the game's ON_BULLET_HIT to us naming a puppet (our real bullet touched it; UNVERIFIED that the game sends it for
//      a friend): a Hit with the bullet's damage, no ray test needed;
//   3. a ray test of our shot (the camera's aim ray at the moment the ammo dropped) against each puppet's body (a
//      vertical capsule from the feet to the head), with the game's ray cast (mc_raycast) for walls in between.
// Once the game has shown it reports our bullets on a friend-brain puppet (1 or 2 seen this session), the ray alone
// no longer hits (its aim is only the crosshair, the game's bullet has spread): "ray_unconfirmed".
//
// Punches: the swing message comes with opponent 0; the nearest puppet in front within LOCK_REACH is our lock-on
// target: the player is turned to it (p:SetDir, UNVERIFIED on the player) and at the impact the punch reaches it within
// LOCK_HIT_REACH (the plain swing reach is 1.9 m, 45 degrees).
#include <cstddef>
#include <cstdint>
#include <vector>

namespace mc::game::pvphit {

struct Vec3 {
    float x = 0, y = 0, z = 0;
};

// One puppet's body: a vertical capsule from feet + kFootLift to head + kHeadTop.
struct Body {
    Vec3 feet;           // the entity position (its feet)
    float headZ = 0;     // the head bone's height (world z); 0 = not known: feet + kHeadDefault
};

constexpr float kRadius = 0.35f;       // m: capsule radius (body ~0.25 plus a little aim tolerance)
constexpr float kFootLift = 0.05f;     // m: the capsule starts this far above the feet
constexpr float kHeadTop = 0.15f;      // m: and ends this far above the head bone
constexpr float kHeadDefault = 1.62f;  // m: head bone above the feet when it cannot be read
constexpr float kMaxRange = 150.0f;    // m: farther shots are not tested (Replay on the victim refuses > 150 m)

// Hit zones as enums.Hitzone (player_sync.lua ZONE_Z): LOWER_BODY 0, BODY 1, HEAD 2, LEG 5.
enum Zone : int { kLowerBody = 0, kBody = 1, kHead = 2, kLeg = 5 };

struct RayHit {
    bool hit = false;
    float t = 0;      // m along the ray to the closest point
    float miss = 0;   // m between the ray and the capsule's axis at the closest point
    float z = 0;      // world height of the closest point on the axis
    int zone = kBody;
};

// The ray from `o` along `d` (any length, normalised here) against body `b`, up to maxRange m.
RayHit RayBody(const Vec3& o, const Vec3& d, const Body& b, float radius = kRadius, float maxRange = kMaxRange);

// The zone of a point at world height z on a body with feet at feetZ and the head bone at headZ.
int ZoneAt(float z, float feetZ, float headZ);

// The nearest body the ray hits (index), -1 none.
int PickShot(const Vec3& o, const Vec3& d, const std::vector<Body>& bodies, RayHit* out = nullptr,
             float radius = kRadius, float maxRange = kMaxRange);

// Melee: the nearest body within `reach` m (horizontal) and |dz| < 1.2 m whose direction is within acos(cosMin) of
// the facing (fx, fy); -1 none.
constexpr float kSwingReach = 1.9f;      // the plain swing fallback (0.15.0)
constexpr float kSwingCos = 0.7f;        // 45 degrees
constexpr float kLockReach = 2.6f;       // lock-on: we turn to a puppet this close
constexpr float kLockCos = 0.34f;        // 70 degrees either side
constexpr float kLockHitReach = 2.3f;    // the locked puppet is hit within this at the impact
constexpr float kLockHitCos = 0.5f;      // 60 degrees (we were turned to it; it may have moved)
int PickMelee(const Vec3& me, float fx, float fy, const std::vector<Body>& bodies, float reach, float cosMin);

// What one of our shots did (see the order above).
enum class Verdict : uint8_t {
    Off,              // friendly fire is off on this game: nothing
    Game,             // the game's own DAMAGE: already sent
    Bullet,           // ON_BULLET_HIT named a puppet: send a Hit
    Ray,              // our ray test hit a puppet: send a Hit
    RayUnconfirmed,   // the ray hit, but the game reports our bullets on friends and did not this time: nothing
    Blocked,          // the ray hit a puppet behind a wall: nothing
    NotHittable,      // the puppet is dead, hidden or in a car: nothing
    Miss,             // nothing hit
};

struct ShotInput {
    bool friendlyFire = true;
    bool gameDamage = false;      // a DAMAGE of our bullet on a puppet near the shot's time (not used by another shot)
    bool bulletEvent = false;     // an ON_BULLET_HIT on a puppet near the shot's time (not used by another shot)
    bool rayHit = false;          // the ray test found a puppet
    bool blocked = false;         // a wall between the camera and that puppet
    bool hittable = true;         // that puppet may be hit (alive, shown, on foot)
    bool gameReports = false;     // this session the game reported our bullets on a friend-brain puppet (1 or 2)
};

Verdict Resolve(const ShotInput& in);
bool SendsHit(Verdict v);  // Bullet or Ray
const char* VerdictName(Verdict v);

// The damage one bullet of `weapon` does to an NPC (a DAMAGE amount) when the game did not tell us: 10 for pistols,
// revolvers, SMGs (0.15 measured: p_mast_a_v1, smg_trench_a_v1 10), the others guessed (UNVERIFIED). The Lua side
// prefers what it learned from our real DAMAGE on NPCs this session (per weapon id).
float BulletDamage(const char* weapon);

// Evidence of the game (DAMAGE / ON_BULLET_HIT) is matched to our shots by time: the ammo drop is seen when the
// state is sampled, so the game's message can come a little before or after it.
constexpr float kEvidenceBefore = 0.2f;  // s before the shot was seen
constexpr float kEvidenceAfter = 0.3f;   // s after it
constexpr float kShotWait = 0.3f;        // s a shot waits for the game's evidence before it is resolved
bool EvidenceMatches(float shotT, float evidenceT);

}  // namespace mc::game::pvphit
