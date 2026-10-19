#pragma once
// Car speed units of the game's Lua API (docs/game-api.md "Speed units"). Read from the exe (static, 2026-10-19):
//   - C_WrapperCar::SetSpeed(number) (binding exe+0x2396470 -> exe+0x23F04F0) multiplies its argument by
//     0.27777779 (1 / 3.6, the float at exe+0x4D833D8) and sets the body's velocity to that times one axis of the car's
//     own matrix (its forward axis, UNVERIFIED which one), waking the body when |argument| > 0.1: it takes km/h, signed
//     along the car's heading (negative: backwards).
//   - C_WrapperCar::GetSpeedFloat() (exe+0x23973C0) returns the vehicle's speed times 3.6 (the float at
//     exe+0x4D2F62C): km/h.
//   - GetSpeed() returns the velocity vector in m/s (used for every PlayerState since 0.10.0; not changed here).
// Until this fix the car copies were given SetSpeed(|v| in m/s): 1 / 3.6 = 0.28 of the wanted speed, exactly the
// copy / driver speed ratio of the 2026-10-19 two-PC test (p10 = p90 = 0.28) and of the SetSpeed probes
// ("after SetSpeed 2.53 (wanted 9.10)"). Header-only, no game calls: tests/car_swap_test.cpp checks it, the Lua glue
// asks it through mc_car_speed_arg / mc_car_speed_ms (game/player_sync.cpp).
#include <cmath>

namespace mc::game::carunits {

constexpr float kKmhPerMs = 3.6f;
constexpr float kMaxMs = 120.0f;  // m/s: a wanted speed above this is clamped (432 km/h, no car of the game goes so fast)

// The number to give SetSpeed for a wanted velocity (vx, vy, vz) in m/s: its length in km/h, negative when the
// driver reverses (gear < 0 / the traffic reverse flag). NaN or infinite input gives 0.
inline float SetSpeedArg(float vx, float vy, float vz, bool reverse) {
    const float ms = std::sqrt(vx * vx + vy * vy + vz * vz);
    if (!std::isfinite(ms)) return 0.0f;
    const float kmh = std::fmin(ms, kMaxMs) * kKmhPerMs;
    return reverse ? -kmh : kmh;
}

// GetSpeedFloat (km/h, may be negative backwards) -> m/s, unsigned. Garbage gives 0.
inline float SpeedFloatToMs(float kmh) {
    if (!std::isfinite(kmh)) return 0.0f;
    return std::fabs(kmh) / kKmhPerMs;
}

}  // namespace mc::game::carunits
