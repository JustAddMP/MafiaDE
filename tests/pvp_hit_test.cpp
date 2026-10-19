// pvp_hit_test: unit tests for player-versus-player hits on puppets (src/game/pvp_hit.h): the shot's ray against a
// puppet's body (hit, miss, zones, range, behind us), the nearest of several, melee targets (swing, lock-on, the
// locked one at the impact), what a shot did from the game's evidence and our ray (the whole order), weapon damage,
// evidence timing, and a short session walk-through the way player_sync.lua M.ShotTick uses it.
// Prints PASS/FAIL per check; exits non-zero on any failure.
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "game/pvp_hit.h"

using namespace mc::game::pvphit;

namespace {

int g_failures = 0;

void Check(bool ok, const char* name, const std::string& detail = {}) {
    std::printf("%s  %-64s %s\n", ok ? "PASS" : "FAIL", name, detail.c_str());
    std::fflush(stdout);
    if (!ok) ++g_failures;
}

bool Near(float a, float b, float eps = 0.02f) { return std::fabs(a - b) <= eps; }

Body At(float x, float y, float z, float headZ = 0) {
    Body b;
    b.feet = {x, y, z};
    b.headZ = headZ;
    return b;
}

void Rays() {
    // camera 1.7 m up at the origin, the puppet 10 m north on flat ground, head bone at 1.62 m
    const Vec3 cam{0, 0, 1.7f};
    const Body b = At(0, 10, 0, 1.62f);
    RayHit h = RayBody(cam, {0, 1, 0}, b);
    Check(h.hit && Near(h.t, 10) && h.zone == kHead, "level ray at head height hits the head",
          "t=" + std::to_string(h.t) + " zone=" + std::to_string(h.zone));
    h = RayBody(cam, {0, 10, 1.3f - 1.7f}, b);
    Check(h.hit && h.zone == kBody, "ray at the chest hits the body", "zone=" + std::to_string(h.zone));
    h = RayBody(cam, {0, 10, 0.85f - 1.7f}, b);
    Check(h.hit && h.zone == kLowerBody, "ray at the hips hits the lower body", "zone=" + std::to_string(h.zone));
    h = RayBody(cam, {0, 10, 0.4f - 1.7f}, b);
    Check(h.hit && h.zone == kLeg, "ray at the knees hits a leg", "zone=" + std::to_string(h.zone));
    h = RayBody(cam, {0.3f / 10, 1, 0}, b);  // 0.3 m to the side at 10 m
    Check(h.hit && Near(h.miss, 0.3f, 0.03f), "0.3 m beside the axis still hits (radius 0.35)",
          "miss=" + std::to_string(h.miss));
    h = RayBody(cam, {0.6f / 10, 1, 0}, b);
    Check(!h.hit && h.miss > 0.5f, "0.6 m beside misses", "miss=" + std::to_string(h.miss));
    h = RayBody(cam, {0, 10, 2.5f - 1.7f}, b);  // over the head (2.5 m at 10 m)
    Check(!h.hit, "ray over the head misses", "miss=" + std::to_string(h.miss));
    h = RayBody(cam, {0, -1, 0}, b);  // looking away
    Check(!h.hit, "a puppet behind us is not hit");
    h = RayBody(cam, {0, 1, 0}, At(0, 200, 0));
    Check(!h.hit, "a puppet beyond 150 m is not tested");
    h = RayBody(cam, {0, 1, 0}, At(0, 10, 0));  // head bone not known: feet + 1.62
    Check(h.hit && h.zone == kHead, "unknown head bone: default head height");
    h = RayBody({0, 10, 30}, {0, 0, -1}, At(0, 10, 0, 1.62f));  // straight down onto it
    Check(h.hit && h.zone == kHead && Near(h.t, 30 - 1.77f, 0.05f), "vertical ray from above hits the head top",
          "t=" + std::to_string(h.t));
    h = RayBody(cam, {0, 0, 0}, b);
    Check(!h.hit, "zero direction hits nothing");
    // a puppet on a slope / stairs: 2 m higher than us, aim up at its chest
    h = RayBody(cam, {0, 8, 2 + 1.3f - 1.7f}, At(0, 8, 2, 3.62f));
    Check(h.hit && h.zone == kBody, "puppet 2 m higher: chest hit", "zone=" + std::to_string(h.zone));
}

void Picks() {
    const Vec3 cam{0, 0, 1.7f};
    std::vector<Body> bodies = {At(0, 20, 0, 1.62f), At(0, 8, 0, 1.62f), At(5, 8, 0, 1.62f)};
    RayHit h;
    int i = PickShot(cam, {0, 1, -0.02f}, bodies, &h);
    Check(i == 1 && Near(h.t, 8, 0.1f), "the nearer of two puppets in line is hit", "i=" + std::to_string(i));
    i = PickShot(cam, {5, 8, 0}, bodies, &h);
    Check(i == 2, "the aimed-at puppet among three", "i=" + std::to_string(i));
    i = PickShot(cam, {-1, 1, 0}, bodies, &h);
    Check(i == -1, "aim between them hits none");
    i = PickShot(cam, {0, 1, 0}, {}, &h);
    Check(i == -1, "no puppets: none");
}

void Melee() {
    const Vec3 me{0, 0, 0};
    std::vector<Body> bodies = {At(0, 1.5f, 0)};
    Check(PickMelee(me, 0, 1, bodies, kSwingReach, kSwingCos) == 0, "swing: puppet 1.5 m in front");
    bodies = {At(1.5f, 1.2f, 0)};  // ~51 degrees right, 1.92 m
    Check(PickMelee(me, 0, 1, bodies, kSwingReach, kSwingCos) == -1, "swing: 51 degrees off and 1.92 m: no");
    Check(PickMelee(me, 0, 1, bodies, kLockReach, kLockCos) == 0, "lock-on: the same puppet is turned to");
    bodies = {At(2.4f, 0.5f, 0)};  // ~78 degrees off
    Check(PickMelee(me, 0, 1, bodies, kLockReach, kLockCos) == -1, "lock-on: 78 degrees off: no");
    bodies = {At(0, 2.2f, 0)};
    Check(PickMelee(me, 0, 1, bodies, kSwingReach, kSwingCos) == -1, "swing: 2.2 m: out of reach");
    Check(PickMelee(me, 0, 1, bodies, kLockHitReach, kLockHitCos) == 0, "locked: 2.2 m still reached");
    bodies = {At(0, 1.0f, 1.5f)};
    Check(PickMelee(me, 0, 1, bodies, kLockReach, kLockCos) == -1, "a puppet 1.5 m above (a balcony): no");
    bodies = {At(0, -1.0f, 0)};
    Check(PickMelee(me, 0, 1, bodies, kLockReach, kLockCos) == -1, "a puppet behind us: no");
    bodies = {At(0, 2.0f, 0), At(0.3f, 1.0f, 0)};
    Check(PickMelee(me, 0, 1, bodies, kLockReach, kLockCos) == 1, "the nearer of two");
    Check(PickMelee(me, 0, 0, bodies, kLockReach, kLockCos) == -1, "no facing: none");
    Check(PickMelee(me, 0, 3, bodies, kLockReach, kLockCos) == 1, "facing need not be normalised");
}

void Verdicts() {
    ShotInput in;
    in.rayHit = true;
    Check(Resolve(in) == Verdict::Ray && SendsHit(Verdict::Ray), "ray hit, no evidence: ray, a Hit");
    in.friendlyFire = false;
    Check(Resolve(in) == Verdict::Off && !SendsHit(Verdict::Off), "friendly fire off: nothing");
    in = {};
    in.gameDamage = true;
    in.rayHit = true;
    Check(Resolve(in) == Verdict::Game && !SendsHit(Verdict::Game), "the game's DAMAGE: already sent, no double");
    in = {};
    in.bulletEvent = true;
    Check(Resolve(in) == Verdict::Bullet && SendsHit(Verdict::Bullet), "ON_BULLET_HIT without a ray hit: bullet");
    in.blocked = true;
    in.rayHit = true;
    Check(Resolve(in) == Verdict::Bullet, "ON_BULLET_HIT wins over a blocked ray (the game's bullet got there)");
    in = {};
    Check(Resolve(in) == Verdict::Miss, "nothing: miss");
    in.rayHit = true;
    in.blocked = true;
    Check(Resolve(in) == Verdict::Blocked && !SendsHit(Verdict::Blocked), "wall in between: blocked");
    in = {};
    in.rayHit = true;
    in.hittable = false;
    Check(Resolve(in) == Verdict::NotHittable, "puppet in a car or dead: not hittable");
    in = {};
    in.rayHit = true;
    in.gameReports = true;
    Check(Resolve(in) == Verdict::RayUnconfirmed && !SendsHit(Verdict::RayUnconfirmed),
          "the game reports our bullets on friends, not this one: the ray alone does not hit");
    in.bulletEvent = true;
    Check(Resolve(in) == Verdict::Bullet, "... and with its report it does");
    for (int v = 0; v <= static_cast<int>(Verdict::Miss); ++v)
        Check(std::string(VerdictName(static_cast<Verdict>(v))) != "?", "verdict has a name",
              VerdictName(static_cast<Verdict>(v)));
}

void Damage() {
    Check(Near(BulletDamage("p_mast_a_v1"), 10), "pistol 10 (measured 0.15)");
    Check(Near(BulletDamage("smg_trench_a_v1"), 10), "Tommy gun 10 (measured 0.15)");
    Check(Near(BulletDamage("rev_alf_a_v1"), 10), "revolver 10");
    Check(BulletDamage("sg_barker_a_v1") > 10, "shotgun more than a pistol");
    Check(Near(BulletDamage(nullptr), 10) && Near(BulletDamage(""), 10), "unknown weapon: 10");
}

void Evidence() {
    Check(EvidenceMatches(10.0f, 10.1f), "DAMAGE 0.1 s after the shot was seen: its own");
    Check(EvidenceMatches(10.0f, 9.85f), "DAMAGE 0.15 s before (the ammo drop is sampled late): its own");
    Check(!EvidenceMatches(10.0f, 9.7f), "0.3 s before: another shot's");
    Check(!EvidenceMatches(10.0f, 10.4f), "0.4 s after: another shot's");
    Check(kShotWait >= kEvidenceAfter, "a shot waits as long as its evidence may come");
}

// What player_sync.lua M.ShotTick does over a short fight: the first shots hit by ray only; once the game reports a
// bullet on a friend-brain puppet, ray-only shots stop counting (the game's bullets decide).
void Session() {
    const Vec3 cam{0, 0, 1.7f};
    const std::vector<Body> bodies = {At(0, 12, 0, 1.62f)};
    int hits = 0;
    bool reports = false;
    struct Shot {
        Vec3 dir;
        bool bullet;
    };
    const Shot shots[] = {{{0, 12, -0.4f}, false}, {{0, 12, -0.4f}, false}, {{3, 12, 0}, false},
                          {{0, 12, -0.4f}, true}, {{0, 12, -0.4f}, false}, {{0.05f, 12, -0.4f}, true}};
    for (const Shot& s : shots) {
        RayHit h;
        ShotInput in;
        in.rayHit = PickShot(cam, s.dir, bodies, &h) >= 0;
        in.bulletEvent = s.bullet;
        in.gameReports = reports;
        const Verdict v = Resolve(in);
        if (SendsHit(v)) ++hits;
        if (s.bullet) reports = true;
    }
    Check(hits == 4, "session: 2 ray hits, a miss, 2 bullet hits, 1 unconfirmed ray", std::to_string(hits));
}

}  // namespace

int main() {
    Rays();
    Picks();
    Melee();
    Verdicts();
    Damage();
    Evidence();
    Session();
    std::printf("%s (%d failure%s)\n", g_failures ? "FAILED" : "ALL PASSED", g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
