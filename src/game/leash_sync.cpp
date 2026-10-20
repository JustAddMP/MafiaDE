#include "game/leash_sync.h"

#include "core/config.h"
#include "core/diag.h"
#include "core/log.h"
#include "core/trace.h"
#include "game/join_sync.h"
#include "game/leash.h"
#include "game/lua.h"
#include "game/mission_sync.h"
#include "game/peer_pause.h"

#include <windows.h>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <atomic>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace mc::game {
namespace {

using namespace leash;

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

constexpr double kIniEveryS = 2.0;      // host: the ini is read again this often
constexpr double kFarShownS = 12.0;     // host: a guest's warn / countdown stays in the panel this long without a repeat
constexpr double kPulledShownS = 5.0;   // host: "... WAS PULLED BACK TO THE HOST"

struct Shared {
    std::mutex mu;
    bool active = false, isHost = false;
    Config config;          // guest: the host's (defaults until his LeashConfig is here)
    bool fromHost = false;  // guest: a LeashConfig came
    std::string banner;
    std::vector<Event> out;  // main thread -> render thread: to the host
    std::string status;
};

Shared& S() {
    static Shared* s = new Shared;
    return *s;
}

// render thread
net::Session* _session = nullptr;
ConfigSender _sender;
Config _hostConfig;
double _iniAt = -1e9;
struct Far {
    EventKind kind = EventKind::None;
    uint16_t distM = 0;
    double at = 0;
};
std::map<net::PlayerId, Far> _far;  // host: what each guest last said
uint32_t _rxEvents = 0;
bool _sesHostWritten = false;  // host: his leash in the session header (SES part=leash)

// main thread
Leash _leash;
bool _running = false, _pullCmd = false, _noGlueLogged = false;
uint32_t _luaErrors = 0, _logged = 0;
Config _lastConfig;
bool _haveLastConfig = false;
std::atomic<bool> _sesPending{true};  // guest: the leash in force still to go into the session header
bool _sesFromHost = false;
double _diagAt = 0;

std::string Upper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

Config IniConfig() {
    const auto& cfg = core::Config::Get();
    return FromIni(cfg.GetInt("leash_warn_m", 300), cfg.GetInt("leash_max_m", 400), cfg.GetInt("leash_countdown_s", 10));
}

// ---- messages (render thread) ------------------------------------------------------------------------------------------
void OnLeashConfig(net::PlayerId from, net::ByteReader& r) {
    if (!_session || _session->IsHost() || from != net::kHostPlayer) return;
    Config c;
    if (!Read(r, c)) {
        MC_LOG_WARN("leash: bad LeashConfig dropped");
        return;
    }
    Shared& sh = S();
    std::lock_guard lock(sh.mu);
    if (!sh.fromHost || c != sh.config)
        MC_LOG_INFO("leash: the host's leash: warning at %u m, countdown beyond %u m, %u s%s", c.warnM, c.maxM,
                    c.countdownS, !c.On() ? " (off)" : c.PullOn() ? "" : " (warning only, no pull)");
    sh.config = c;
    sh.fromHost = true;
}

void OnLeashEvent(net::PlayerId from, net::ByteReader& r) {
    if (!_session || !_session->IsHost() || from == net::kHostPlayer) return;
    Event e;
    if (!Read(r, e)) {
        MC_LOG_WARN("leash: bad LeashEvent from player %u dropped", from);
        return;
    }
    ++_rxEvents;
    Far& f = _far[from];
    const bool repeat = f.kind == e.kind && (e.kind == EventKind::Warn || e.kind == EventKind::Countdown);
    f.kind = e.kind;
    f.distM = e.distM;
    f.at = Now();
    if (!repeat)
        MC_LOG_INFO("leash: player %u: %s (%u m from us)", from, EventName(e.kind), e.distM);
    if (core::trace::IsOpen())
        core::trace::Line("LSH", "ev=rx from=%u kind=%s dist=%u repeat=%d", from, EventName(e.kind), e.distM,
                          repeat ? 1 : 0);
}

// ---- Lua (main thread) -------------------------------------------------------------------------------------------------
// MafiaCoop.Leash(cmd) (player_sync.lua): false when it could not run.
bool CallGlue(lua_State* L, const char* cmd, std::string& reading) {
    const int top = lua::GetTop(L);
    lua::GetGlobal(L, "MafiaCoop");
    if (lua::TypeOf(L, -1) != lua::TTable) {
        lua::SetTop(L, top);
        return false;
    }
    lua::GetField(L, -1, "Leash");
    // a Lua function is TIFunction (9) in this engine's Lua (Havok Script), TFunction / TCFunction for others: any of
    // them (the 2026-10-20 14:36 one-PC run: the Lua function read as "no MafiaCoop.Leash" every tick, no leash at all)
    const int type = lua::TypeOf(L, -1);
    if (type != lua::TFunction && type != lua::TIFunction && type != lua::TCFunction) {
        if (!_noGlueLogged) {
            _noGlueLogged = true;
            MC_LOG_WARN("leash: player_sync.lua has no MafiaCoop.Leash yet (type %d; an older override next to the dll?): "
                        "tried again every tick", type);
        }
        lua::SetTop(L, top);
        return false;
    }
    lua::PushString(L, cmd, std::strlen(cmd));
    std::string err;
    if (!lua::PCall(L, 1, 1, &err)) {
        _luaErrors = lua::ReportError("leash", "Leash", err);
        lua::SetTop(L, top);
        return false;
    }
    const char* s = lua::ToString(L, -1);
    reading = s ? s : "";
    lua::SetTop(L, top);
    return true;
}

// The leash in force, for the session header (dll/session_log.h SES; docs/sync-trace.md "SES part=leash"): the host
// his own (from his settings), a guest the host's LeashConfig once it is here (or the defaults until then).
void SesLeash(const char* role, const Config& c, const char* from) {
    if (!core::trace::IsOpen()) return;
    core::trace::Line("SES", "part=leash role=%s warn=%u max=%u cd=%u on=%d pull=%d from=%s", role, c.warnM, c.maxM,
                      c.countdownS, c.On() ? 1 : 0, c.PullOn() ? 1 : 0, from);
    MC_LOG_INFO("session: leash in force (%s, %s): warning at %u m, countdown beyond %u m for %u s%s", role, from,
                c.warnM, c.maxM, c.countdownS, !c.On() ? " (off)" : c.PullOn() ? "" : " (warning only)");
}

void Trace(const char* ev, const Output& o, const Config& c) {
    // the session summaries' counts (core/diag.h): every state entered, every pull
    char name[48];
    std::snprintf(name, sizeof name, "leash.%s.%s", std::strcmp(ev, "pull") == 0 ? "ev" : "state",
                  std::strcmp(ev, "pull") == 0 ? "pull" : StateName(o.state));
    core::diag::Add(name);
    if (!core::trace::IsOpen()) return;
    core::trace::Line("LSH", "ev=%s st=%s why=%s dist=%.1f left=%.1f warn=%u max=%u cd=%u", ev, StateName(o.state),
                      WhyName(o.why), o.dist, o.left, c.warnM, c.maxM, c.countdownS);
}

void MainTick(lua_State* L, float) {
    Shared& sh = S();
    bool active, host, fromHost;
    Config cfg;
    {
        std::lock_guard lock(sh.mu);
        active = sh.active;
        host = sh.isHost;
        cfg = sh.config;
        fromHost = sh.fromHost;
    }
    if (!active || host) {
        if (_running) {  // the session ended or the host is gone: a new start
            _running = false;
            _pullCmd = false;
            _leash.Reset();
            std::lock_guard lock(sh.mu);
            sh.banner.clear();
            sh.status.clear();
        }
        return;
    }
    _running = true;
    if (!_haveLastConfig || cfg != _lastConfig || _sesFromHost != fromHost) {
        _sesPending = true;  // SES part=leash once the trace is open
        _sesFromHost = fromHost;
    }
    if (_sesPending && core::trace::IsOpen()) {
        _sesPending = false;
        SesLeash("guest", cfg, fromHost ? "host" : "default");
    }
    if (!_haveLastConfig || cfg != _lastConfig) {
        _haveLastConfig = true;
        _lastConfig = cfg;
        _leash.SetConfig(cfg);
        if (core::trace::IsOpen())
            core::trace::Line("LSH", "ev=config warn=%u max=%u cd=%u on=%d from=%s", cfg.warnM, cfg.maxM, cfg.countdownS,
                              cfg.On() ? 1 : 0, fromHost ? "host" : "default");
    }
    const char* cmd = _pullCmd ? "PULL" : "";
    _pullCmd = false;
    std::string reading;
    if (!CallGlue(L, cmd, reading) || reading.empty()) return;
    int inWorld = 0, seen = 0, cut = 0, selfDown = 0, hostDown = 0, hostCar = 0, passenger = 0, drive = 0;
    float x = 0, y = 0, hx = 0, hy = 0;
    if (std::sscanf(reading.c_str(), "R %d %f %f %d %f %f %d %d %d %d %d %d", &inWorld, &x, &y, &seen, &hx, &hy, &cut,
                    &selfDown, &hostDown, &hostCar, &passenger, &drive) != 12)
        return;
    const double now = Now();
    const size_t at = reading.find(" P ");
    if (at != std::string::npos) {
        int ok = 0;
        char how[32] = {};
        if (std::sscanf(reading.c_str() + at, " P %d %31s", &ok, how) >= 1) {
            _leash.PullDone(ok != 0, now);
            MC_LOG_INFO("leash: %s (%s)", ok ? "pulled back beside the host" : "the pull did not work", how);
            if (core::trace::IsOpen()) core::trace::Line("LSH", "ev=pulled ok=%d how=%s", ok, how[0] ? how : "-");
        }
    }
    Input in;
    in.now = now;
    in.inWorld = inWorld != 0;
    in.hostSeen = seen != 0;
    in.x = x;
    in.y = y;
    in.hostX = hx;
    in.hostY = hy;
    in.cutscene = cut != 0;
    in.hostPaused = peer_pause::Remote(static_cast<uint8_t>(net::kHostPlayer), now) == peer_pause::Quiet::Paused;
    in.hostDown = hostDown != 0;
    in.selfDown = selfDown != 0;
    in.joinSettled = JoinSync::Settled();
    in.hostCar = hostCar != 0;
    in.passenger = passenger != 0;
    in.failReload = MissionSync::FailBusy();
    const State before = _leash.GetState();
    const Output o = _leash.Update(in);
    if (now - _diagAt >= 1.0) {  // the session summaries' levels
        _diagAt = now;
        core::diag::Set("leash.dist", o.dist);
        core::diag::Set("leash.state", static_cast<double>(static_cast<int>(o.state)));
    }
    if (o.pull) {
        _pullCmd = true;  // carried out by the glue on the next tick
        MC_LOG_INFO("leash: %.0f m from the host after the %u s countdown: pulled back (%s)", o.dist, cfg.countdownS,
                    drive ? "with our car" : "on foot");
        Trace("pull", o, cfg);
    } else if (o.changed) {
        Trace("state", o, cfg);
        // the state changes; suspensions only the first few times (a flapping one must not fill the log)
        if (o.state != State::Suspended && before != State::Suspended)
            MC_LOG_INFO("leash: %s -> %s (%.0f m from the host)", StateName(before), StateName(o.state), o.dist);
        else if (++_logged <= 40)
            MC_LOG_INFO("leash: %s -> %s (%s, %.0f m from the host)", StateName(before), StateName(o.state),
                        WhyName(o.why), o.dist);
    }
    char status[200];
    std::snprintf(status, sizeof status, "Leash: %s%s%s, %.0f m from the host (warning %u m, max %u m, %u s, %s)",
                  StateName(o.state), o.why != Why::None ? " / " : "", o.why != Why::None ? WhyName(o.why) : "",
                  o.dist, cfg.warnM, cfg.maxM, cfg.countdownS, fromHost ? "the host's" : "defaults");
    std::lock_guard lock(sh.mu);
    sh.banner = o.banner;
    for (const Event& e : o.events) sh.out.push_back(e);
    sh.status = status;
}

}  // namespace

void LeashSync::Install() {
    lua::AddTickCallback(&MainTick);
    MC_LOG_INFO("leash: installed (LeashConfig 0x%04x host -> guests, LeashEvent 0x%04x guest -> host)",
                kMsgLeashConfig, kMsgLeashEvent);
}

void LeashSync::Attach(net::Session& session) {
    _session = &session;
    _sesHostWritten = false;
    _sesPending = true;
    _sender.Reset();
    _iniAt = -1e9;
    _far.clear();
    _rxEvents = 0;
    session.On(kMsgLeashConfig, &OnLeashConfig);
    session.On(kMsgLeashEvent, &OnLeashEvent);
    Shared& sh = S();
    std::lock_guard lock(sh.mu);
    sh.active = false;
    sh.config = Config();
    sh.fromHost = false;
    sh.banner.clear();
    sh.out.clear();
    sh.status.clear();
}

void LeashSync::Detach() {
    if (_session) {
        _session->Off(kMsgLeashConfig);
        _session->Off(kMsgLeashEvent);
    }
    _session = nullptr;
    _far.clear();
    Shared& sh = S();
    std::lock_guard lock(sh.mu);
    sh.active = false;  // the main thread resets the leash
    sh.banner.clear();
    sh.out.clear();
    sh.status.clear();
}

void LeashSync::Tick(float) {
    if (!_session) return;
    const bool active = _session->IsActive(), host = _session->IsHost();
    std::vector<Event> out;
    {
        Shared& sh = S();
        std::lock_guard lock(sh.mu);
        sh.active = active;
        sh.isHost = host;
        out.swap(sh.out);
        if (!active) sh.banner.clear();
    }
    if (!active) return;
    if (!host) {
        for (const Event& e : out) {
            net::ByteWriter w(4);
            Write(w, e);
            _session->SendToHost(kMsgLeashEvent, w.Span(), net::Reliability::Reliable);
        }
        return;
    }
    const double now = Now();
    if (!_sesHostWritten && core::trace::IsOpen() && _iniAt > -1e8) {
        _sesHostWritten = true;
        SesLeash("host", _hostConfig, "settings");
    }
    if (now - _iniAt >= kIniEveryS) {
        _iniAt = now;
        const Config c = IniConfig();
        if (c != _hostConfig) {
            MC_LOG_INFO("leash: our leash for the guests: warning at %u m, countdown beyond %u m, %u s%s", c.warnM,
                        c.maxM, c.countdownS, !c.On() ? " (off)" : c.PullOn() ? "" : " (warning only)");
            _sesHostWritten = false;  // written again with the new values
        }
        _hostConfig = c;
    }
    std::vector<std::pair<uint32_t, uint32_t>> guests;
    for (const auto& p : _session->Players())
        if (p.id != _session->LocalId()) guests.push_back({p.id, p.joinSeq});
    const ConfigSender::Plan plan = _sender.Tick(_hostConfig, guests);
    if (!plan.broadcast && plan.to.empty()) return;
    net::ByteWriter w(8);
    Write(w, _hostConfig);
    if (plan.broadcast) {
        _session->Broadcast(kMsgLeashConfig, w.Span(), net::Reliability::Reliable);
        MC_LOG_INFO("leash: sent the changed LeashConfig to every guest");
    }
    for (uint32_t id : plan.to) _session->SendTo(static_cast<net::PlayerId>(id), kMsgLeashConfig, w.Span(),
                                                 net::Reliability::Reliable);
    if (core::trace::IsOpen())
        core::trace::Line("LSH", "ev=send warn=%u max=%u cd=%u on=%d to=%s", _hostConfig.warnM, _hostConfig.maxM,
                          _hostConfig.countdownS, _hostConfig.On() ? 1 : 0, plan.broadcast ? "all" : "new");
}

std::string LeashSync::HudNotice() {
    Shared& sh = S();
    std::lock_guard lock(sh.mu);
    return sh.active && !sh.isHost ? sh.banner : std::string();
}

void LeashSync::GetStatus(std::vector<render::OverlayLine>& out) {
    if (!_session || !_session->IsActive()) return;
    if (!_session->IsHost()) {
        Shared& sh = S();
        std::lock_guard lock(sh.mu);
        if (!sh.status.empty()) out.push_back({sh.status, 0xFFB0C4DE});
        return;
    }
    const double now = Now();
    for (const auto& p : _session->Players()) {
        auto it = _far.find(p.id);
        if (it == _far.end()) continue;
        const Far& f = it->second;
        char buf[160];
        const std::string name = Upper(p.name);
        if ((f.kind == EventKind::Warn || f.kind == EventKind::Countdown) && now - f.at < kFarShownS) {
            std::snprintf(buf, sizeof buf, "%s IS FAR AWAY (%u M)", name.c_str(), f.distM);
            out.push_back({buf, 0xFFE0B060});
        } else if (f.kind == EventKind::Pulled && now - f.at < kPulledShownS) {
            std::snprintf(buf, sizeof buf, "%s WAS PULLED BACK TO US", name.c_str());
            out.push_back({buf, 0xFFE0B060});
        }
    }
}

}  // namespace mc::game
