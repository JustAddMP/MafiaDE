#include "game/vis.h"

#include "core/diag.h"
#include "core/trace.h"

#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace mc::game::vis {
namespace {

std::mutex _mu;
RateCap _cap;

bool Token(const char* s) {
    if (!s || !*s) return false;
    for (const char* c = s; *c; ++c)
        if (*c <= ' ' || *c == '=' || *c > '~') return false;
    return true;
}

}  // namespace

const char* KindName(Kind k) {
    switch (k) {
    case Kind::Player: return "player";
    case Kind::Traffic: return "traffic";
    case Kind::Ped: return "ped";
    case Kind::Npc: return "npc";
    case Kind::Held: return "held";
    default: return "?";
    }
}

void Event(Kind kind, uint32_t key, const char* ev, const char* dir, int peer, const char* fmt, ...) {
    if (!Token(ev)) ev = "?";
    char counter[64];
    std::snprintf(counter, sizeof counter, "vis.%s.%s", KindName(kind), ev);
    core::diag::Add(counter);
    if (!core::trace::IsOpen()) return;
    uint32_t dropped = 0;
    bool allow;
    {
        std::lock_guard lock(_mu);
        allow = _cap.Allow(kind, core::trace::Now(), &dropped);
    }
    if (dropped) core::trace::Line("VIS", "kind=%s key=0 ev=dropped dir=- peer=- n=%u", KindName(kind), dropped);
    if (!allow) return;
    char extra[512] = "";
    if (fmt && *fmt) {
        extra[0] = ' ';
        va_list a;
        va_start(a, fmt);
        _vsnprintf_s(extra + 1, sizeof extra - 1, _TRUNCATE, fmt, a);
        va_end(a);
    }
    char who[8] = "-";
    if (peer >= 0) std::snprintf(who, sizeof who, "%d", peer);
    core::trace::Line("VIS", "kind=%s key=%u ev=%s dir=%s peer=%s%s", KindName(kind), key, ev, Token(dir) ? dir : "-",
                      who, extra);
}

}  // namespace mc::game::vis
