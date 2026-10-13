#include "dll/mod.h"

#include "core/config.h"
#include "core/crash.h"
#include "core/log.h"
#include "dll/coop.h"
#include "game/lua.h"
#include "render/overlay.h"
#include "render/present_hook.h"

#include <windows.h>
#include <vector>

namespace mc {
namespace {

constexpr double kHeartbeatSeconds = 10.0;

struct State {
    bool crashTest = false;
    double crashTestDelay = 20.0;
    double uptime = 0.0;          // seconds since first Tick
    double sinceHeartbeat = 0.0;
    unsigned long long frames = 0;
    unsigned framesSinceHeartbeat = 0;
    bool overlayVisible = true;
    bool toggleKeyDown = false;
    std::vector<render::OverlayLine> status;
    float minDt = 1e9f;
    float maxDt = 0.0f;
};

State _state;

// F7 toggles the status overlay; only while one of our windows is in the foreground.
void PollToggleKey() {
    const bool down = (GetAsyncKeyState(VK_F7) & 0x8000) != 0;
    const bool wasDown = _state.toggleKeyDown;
    _state.toggleKeyDown = down;
    if (!down || wasDown) return;
    DWORD pid = 0;
    HWND fg = GetForegroundWindow();
    if (!fg) return;
    GetWindowThreadProcessId(fg, &pid);
    if (pid != GetCurrentProcessId()) return;
    _state.overlayVisible = !_state.overlayVisible;
    MC_LOG_INFO("mod: status overlay %s (F7)", _state.overlayVisible ? "shown" : "hidden");
}

} // namespace

void Mod::Init() {
    const auto& cfg = core::Config::Get();
    _state.crashTest = cfg.GetBool("crash_test", false);
    _state.crashTestDelay = cfg.GetInt("crash_test_delay", 20);
    if (_state.crashTest) {
        MC_LOG_WARN("mod: crash_test=1 -> will raise an access violation in Tick after %.0f s", _state.crashTestDelay);
    }
    _state.overlayVisible = cfg.GetBool("overlay", true);
    Coop::Init();
    MC_LOG_INFO("mod: initialized");
}

void Mod::Tick(float dt) {
    if (_state.frames == 0) MC_LOG_INFO("mod: first Tick");
    ++_state.frames;
    ++_state.framesSinceHeartbeat;
    _state.uptime += dt;
    _state.sinceHeartbeat += dt;
    if (_state.frames > 1) {
        if (dt < _state.minDt) _state.minDt = dt;
        if (dt > _state.maxDt) _state.maxDt = dt;
    }

    if (_state.sinceHeartbeat >= kHeartbeatSeconds) {
        double fps = _state.framesSinceHeartbeat / _state.sinceHeartbeat;
        MC_LOG_INFO("heartbeat: uptime %.0f s, frames %llu, fps %.1f, frame time min %.1f ms max %.1f ms",
                    _state.uptime, _state.frames, fps, _state.minDt * 1000.0f, _state.maxDt * 1000.0f);
        _state.sinceHeartbeat = 0.0;
        _state.framesSinceHeartbeat = 0;
        _state.minDt = 1e9f;
        _state.maxDt = 0.0f;
        core::crash::Reassert();
        std::vector<render::OverlayLine> lines;
        Coop::GetStatus(lines);
        for (const auto& l : lines) MC_LOG_INFO("status: %s", l.text.c_str());
    }

    game::lua::PollRender(dt);
    Coop::Tick(dt);

    PollToggleKey();
    if (_state.overlayVisible) {
        _state.status.clear();
        Coop::GetStatus(_state.status);
        render::Overlay::Draw(render::GameSwapChain(), _state.status);
    }

    if (_state.crashTest && _state.uptime >= _state.crashTestDelay) {
        _state.crashTest = false;
        MC_LOG_WARN("mod: crash_test: raising access violation now");
        volatile int* bad = reinterpret_cast<volatile int*>(static_cast<uintptr_t>(0x10));
        *bad = 0xDEAD;
    }
}

void Mod::OnBeforeResize() { render::Overlay::ReleaseTargets(); }

void Mod::OnResize(unsigned width, unsigned height) {
    MC_LOG_INFO("mod: resized to %ux%u", width, height);
}

} // namespace mc
