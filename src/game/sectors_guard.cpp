#include "game/sectors_guard.h"

#include "core/hook.h"
#include "core/log.h"
#include "core/mem.h"
#include "game/lua.h"

#include <windows.h>
#include <atomic>
#include <cstdint>
#include <cstring>

namespace mc::game::sectors_guard {
namespace {

// exe+0x243A870 (build CRC32 3168979183): void Update(C_GameSectorsModule* this, int elapsedMs). Unique in .text.
constexpr const char* kPatUpdate =
    "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 50 01 51 70 48 8B D9 E8 ?? ?? ?? ?? 48 8B F8 48 83 78 08 00 75 1D B9 08 "
    "00 00 00 E8 ?? ?? ?? ?? 48 85 C0";
// The walk inside it reads the vector from +0x60 (end) and +0x58 (begin): "mov rdi, [rbx+60h]; mov rbx, [rbx+58h]",
// checked before the hook goes in so that another build with the same prologue is left alone.
constexpr uint8_t kWalk[] = {0x48, 0x8B, 0x7B, 0x60, 0x48, 0x8B, 0x5B, 0x58};
constexpr size_t kWalkOffset = 0x72;
constexpr uint32_t kTickMs = 1000;  // the walk runs when the accumulated time at +0x70 passes this

using UpdateFn = void (*)(uint8_t* self, int elapsedMs);
UpdateFn _orig = nullptr;
std::atomic<bool> _active{false};
std::atomic<uint32_t> _dropped{0};
uint32_t _walks = 0, _checked = 0;
bool _loggedLayout = false;
bool _confirmed = false;  // a live entity was seen in the list: the layout is as expected, removing is safe

// An entry whose position getter (the vtable +0x120 function the walk calls) would read through a component the
// entity no longer has: humans being torn down with their world (a failed mission's load in Omerta cp_020 with the
// guest's friend-brain puppet in it) still resolve in the entity table, but [entity+0x348] is already null and the
// getter (exe+0x2B4C470: mov rax,[rcx+348h]; mov rcx,[rax+298h]; ...) crashed reading null+0x298. Only checked when
// the getter starts with exactly that load, so other entity classes are left alone.
bool HalfTorn(void* entity) {
    const uint8_t* e = static_cast<const uint8_t*>(entity);
    if (*reinterpret_cast<const uintptr_t*>(e + 0xF0) != 0) return false;  // the walk skips it anyway
    const uint8_t* fn = *reinterpret_cast<const uint8_t* const*>(*reinterpret_cast<const uintptr_t*>(e) + 0x120);
    static const uint8_t kLoad[] = {0x48, 0x8B, 0x81, 0x48, 0x03, 0x00, 0x00, 0x48, 0x8B, 0x88, 0x98, 0x02, 0x00, 0x00};
    if (std::memcmp(fn, kLoad, sizeof kLoad) != 0) return false;
    const uint8_t* comp = *reinterpret_cast<const uint8_t* const*>(e + 0x348);
    return !comp || !*reinterpret_cast<const void* const*>(comp + 0x298);
}

bool Purge(uint8_t* self) {
    __try {
        void*** begin = reinterpret_cast<void***>(self + 0x58);
        void*** end = reinterpret_cast<void***>(self + 0x60);
        void** b = *begin;
        void** e = *end;
        if (!b || e < b || e - b > 100000) return false;
        size_t n = static_cast<size_t>(e - b), live = 0, removed = 0, torn = 0;
        if (!_confirmed) {
            for (size_t i = 0; i < n; ++i)
                if (lua::EntityIsLive(b[i])) _confirmed = true;
            if (!_confirmed) return true;  // nothing proven live yet: leave the list alone
            MC_LOG_INFO("sectors: list layout confirmed (%zu entries)", n);
        }
        for (size_t i = 0; i < n;) {
            if (lua::EntityIsLive(b[i])) {
                if (!HalfTorn(b[i])) {
                    ++live;
                    ++i;
                    continue;
                }
                ++torn;
            }
            b[i] = b[n - 1];  // swap with the last, like the game's own removal (exe+0x243A140)
            --n;
            ++removed;
        }
        if (removed) {
            *end = b + n;
            _dropped += static_cast<uint32_t>(removed);
            MC_LOG_WARN("sectors: dropped %zu stale human(s) from the game-sectors list (%zu half torn down, %zu live) "
                        "before its walk",
                        removed, torn, live);
        }
        _checked += static_cast<uint32_t>(live);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void Detour(uint8_t* self, int elapsedMs) {
    // only right before a walk (the original adds elapsedMs to +0x70 and walks when it passes kTickMs)
    if (self && lua::EntityLookupAvailable()) {
        uint32_t acc = 0;
        __try {
            acc = *reinterpret_cast<uint32_t*>(self + 0x70);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            acc = 0;
        }
        if (acc + static_cast<uint32_t>(elapsedMs) > kTickMs) {
            Purge(self);
            if (++_walks == 1 && !_loggedLayout) {
                _loggedLayout = true;
                MC_LOG_INFO("sectors: first walk guarded");
            }
        }
    }
    _orig(self, elapsedMs);
}

}  // namespace

void Install() {
    const uintptr_t a = core::mem::FindPattern(kPatUpdate);
    if (!a) {
        MC_LOG_WARN("sectors: update not found: no guard for the game-sectors list (friend-brain puppets stay off)");
        return;
    }
    if (std::memcmp(reinterpret_cast<const void*>(a + kWalkOffset), kWalk, sizeof kWalk) != 0) {
        MC_LOG_WARN("sectors: update at %p does not walk the list as expected: no guard", reinterpret_cast<void*>(a));
        return;
    }
    if (!core::hook::Install("GameSectors_Update", reinterpret_cast<void*>(a), reinterpret_cast<void*>(&Detour),
                             reinterpret_cast<void**>(&_orig))) {
        MC_LOG_WARN("sectors: hook failed: no guard");
        return;
    }
    _active = true;
    MC_LOG_INFO("sectors: guard installed (game-sectors list walk checked against the entity table)");
}

bool Active() { return _active.load(); }
uint32_t Dropped() { return _dropped.load(); }

}  // namespace mc::game::sectors_guard
