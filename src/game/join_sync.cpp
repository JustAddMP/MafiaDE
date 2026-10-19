#include "game/join_sync.h"

#include "core/log.h"
#include "core/trace.h"
#include "game/player_sync.h"

#include <windows.h>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

namespace mc::game {
namespace {

using namespace join;

double Now() {
    static LARGE_INTEGER freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f;
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) / static_cast<double>(freq.QuadPart);
}

struct Shared {
    std::mutex mu;
    bool accept = true;  // marks are recorded (a guest whose world is not complete yet)
    Tracker tracker;
};

Shared& S() {
    static Shared* s = new Shared;
    return *s;
}

// render thread
net::Session* _session = nullptr;
bool _wasActive = false, _everActive = false;

std::string Times(const Tracker& t) {
    std::string out;
    char buf[48];
    for (size_t i = 0; i < kSystems; ++i) {
        const double a = t.FirstAfter(static_cast<System>(i));
        if (a < 0) continue;
        std::snprintf(buf, sizeof buf, "%s%s %.1f", out.empty() ? "" : ", ", Name(static_cast<System>(i)), a);
        out += buf;
    }
    return out.empty() ? "nothing" : out;
}

void SendReport(const Tracker& t, const Tracker::Event& e) {
    if (!_session || _session->IsHost() || !_session->IsActive()) return;
    Report r;
    r.reconnect = t.Reconnect();
    r.complete = e.kind == Tracker::Event::Complete;
    r.late = e.late;
    r.afterMs = static_cast<uint32_t>(e.after * 1000.0);
    for (size_t i = 0; i < kSystems; ++i) {
        const double a = t.FirstAfter(static_cast<System>(i));
        r.firstMs[i] = a < 0 ? -1 : static_cast<int32_t>(a * 1000.0);
    }
    r.missing = e.missing;
    net::ByteWriter w(96);
    WriteReport(w, r);
    _session->SendToHost(kMsgJoinReport, w.Span(), net::Reliability::Reliable);
}

// Host: a guest's report (logged and traced, nothing else).
void OnJoinReport(net::PlayerId from, net::ByteReader& rd) {
    if (!_session || !_session->IsHost() || from == net::kHostPlayer) return;
    Report r;
    if (!ReadReport(rd, r)) {
        MC_LOG_WARN("join: bad JoinReport from player %u dropped", from);
        return;
    }
    std::string times, compact;  // "player 0.2, mission 0.1" for the log, "player:0.2,mission:0.1" for the trace
    char buf[48];
    for (size_t i = 0; i < kSystems; ++i) {
        if (r.firstMs[i] < 0) continue;
        std::snprintf(buf, sizeof buf, "%s%s %.1f", times.empty() ? "" : ", ", Name(static_cast<System>(i)),
                      r.firstMs[i] / 1000.0);
        times += buf;
        std::snprintf(buf, sizeof buf, "%s%s:%.1f", compact.empty() ? "" : ",", Name(static_cast<System>(i)),
                      r.firstMs[i] / 1000.0);
        compact += buf;
    }
    if (r.complete)
        MC_LOG_INFO("join: player %u's world is complete in %.1f s%s%s (%s)", from, r.afterMs / 1000.0,
                    r.reconnect ? " after his reconnect" : "", r.late ? ", late" : "", times.c_str());
    else
        MC_LOG_WARN("join: player %u still misses %s after %.0f s%s (%s)", from, r.missing.c_str(), r.afterMs / 1000.0,
                    r.reconnect ? " after his reconnect" : "", times.c_str());
    if (core::trace::IsOpen())
        core::trace::Line("JN", "ev=report from=%u complete=%d after=%.2f reconnect=%d late=%d missing=%s times=%s",
                          from, r.complete ? 1 : 0, r.afterMs / 1000.0, r.reconnect ? 1 : 0, r.late ? 1 : 0,
                          r.missing.empty() ? "-" : r.missing.c_str(), compact.empty() ? "-" : compact.c_str());
}

}  // namespace

namespace join {

void Mark(System s, const std::string& key) {
    const double now = Now();
    Shared& sh = S();
    std::lock_guard lock(sh.mu);
    if (sh.accept) sh.tracker.Mark(s, key, now);
}

void Mark(System s, uint32_t key) { Mark(s, std::to_string(key)); }

void Expect(System s, const std::string& key) {
    Shared& sh = S();
    std::lock_guard lock(sh.mu);
    if (sh.accept) sh.tracker.Expect(s, key);
}

}  // namespace join

void JoinSync::Attach(net::Session& session) {
    _session = &session;
    _wasActive = _everActive = false;
    session.On(kMsgJoinReport, &OnJoinReport);
    Shared& sh = S();
    std::lock_guard lock(sh.mu);
    sh.tracker.Reset();
    sh.accept = true;
}

void JoinSync::Detach() {
    if (_session) _session->Off(kMsgJoinReport);
    _session = nullptr;
    Shared& sh = S();
    std::lock_guard lock(sh.mu);
    sh.tracker.Reset();
    sh.accept = true;
}

void JoinSync::Tick(float) {
    if (!_session) return;
    const bool active = _session->IsActive(), host = _session->IsHost();
    const double now = Now();
    const bool inWorld = !host && active && PlayerSync::InWorld();  // before our lock (PlayerSync takes its own)
    Shared& sh = S();
    std::vector<Tracker::Event> events;
    Tracker copy;
    {
        std::lock_guard lock(sh.mu);
        if (host) {  // nothing to wait for on the host
            if (sh.accept) sh.tracker.Reset();
            sh.accept = false;
            return;
        }
        if (!active) {
            if (_wasActive) {  // the host is gone: the next activation is a reconnect, tracked anew
                sh.tracker.Reset();
                sh.accept = true;
            }
            _wasActive = false;
            return;
        }
        if (!_wasActive) {
            std::vector<uint32_t> others;
            for (const auto& p : _session->Players())
                if (p.id != _session->LocalId()) others.push_back(p.id);
            sh.tracker.Begin(now, _everActive, others);
            sh.accept = true;
            MC_LOG_INFO("join: %s: waiting for the world (%zu other player(s))",
                        _everActive ? "back in the host's session" : "in the host's session", others.size());
            if (core::trace::IsOpen())
                core::trace::Line("JN", "ev=begin reconnect=%d others=%zu", _everActive ? 1 : 0, others.size());
        }
        _wasActive = _everActive = true;
        if (inWorld) sh.tracker.InWorld(now);
        events = sh.tracker.Poll(now);
        if (events.empty()) return;
        copy = sh.tracker;
        if (copy.Complete()) sh.accept = false;
    }
    for (const auto& e : events) {
        switch (e.kind) {
        case Tracker::Event::First:
            if (core::trace::IsOpen())
                core::trace::Line("JN", "ev=%s key=%s after=%.2f", Name(e.sys), e.key.empty() ? "-" : e.key.c_str(),
                                  e.after);
            break;
        case Tracker::Event::Complete:
            MC_LOG_INFO("join: world complete in %.1f s%s (%s%s%s)", e.after,
                        e.late ? " (late: after the 15 s report)" : "", Times(copy).c_str(),
                        e.none.empty() ? "" : "; nothing came for: ", e.none.c_str());
            if (core::trace::IsOpen())
                core::trace::Line("JN", "ev=complete after=%.2f missing=- none=%s late=%d reconnect=%d", e.after,
                                  e.none.empty() ? "-" : e.none.c_str(), e.late ? 1 : 0, copy.Reconnect() ? 1 : 0);
            SendReport(copy, e);
            break;
        case Tracker::Event::Missing:
            MC_LOG_WARN("join: still missing %s after %.0f s in the world (%s%s%s)", e.missing.c_str(), kWindowS,
                        Times(copy).c_str(), e.none.empty() ? "" : "; nothing came for: ", e.none.c_str());
            if (core::trace::IsOpen())
                core::trace::Line("JN", "ev=complete after=%.2f missing=%s none=%s late=0 reconnect=%d", e.after,
                                  e.missing.c_str(), e.none.empty() ? "-" : e.none.c_str(), copy.Reconnect() ? 1 : 0);
            SendReport(copy, e);
            break;
        }
    }
}

}  // namespace mc::game
