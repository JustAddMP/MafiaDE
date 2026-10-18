#include "game/lua.h"

#include "core/compat.h"
#include "core/hook.h"
#include "core/log.h"
#include "core/mem.h"
#include "dll/ui_input.h"
#include "game/car_signals.h"

#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <mutex>
#include <sstream>
#include <unordered_set>
#include <vector>

namespace mc::game::lua {
namespace {

// ---- lua_State layout (docs/re-notes.md 1.2) -----------------------------------------------------------------
constexpr size_t kOffTop = 0x48;
constexpr size_t kOffBase = 0x50;
constexpr size_t kOffLimit = 0x58;
constexpr size_t kOffGlobals = 0x70;
constexpr size_t kTValueSize = 16;
constexpr size_t kOffMachineState = 0xD0;  // C_ScriptGameMachine -> lua_State*

uint8_t*& TopRef(lua_State* L) { return *reinterpret_cast<uint8_t**>(reinterpret_cast<uint8_t*>(L) + kOffTop); }
uint8_t* Base(lua_State* L) { return *reinterpret_cast<uint8_t**>(reinterpret_cast<uint8_t*>(L) + kOffBase); }
uint8_t* Limit(lua_State* L) { return *reinterpret_cast<uint8_t**>(reinterpret_cast<uint8_t*>(L) + kOffLimit); }

// ---- game functions (resolved by pattern; RVAs in re-notes are informational) -------------------------------
using LoadBufferFn = int (*)(lua_State*, const char*, size_t, const char*);
using PCallFn = int (*)(lua_State*, int, int, int);
using FieldFn = void (*)(lua_State*, int, const char*);
using PushLStringFn = void (*)(lua_State*, const char*, size_t);
using ToStringFn = const char* (*)(lua_State*, int);
using ToNumberFn = float (*)(lua_State*, int);
using PushNamedCClosureFn = void (*)(lua_State*, CFunction, int, const char*, int);
using TickFn4 = uint64_t (*)(void*, void*, void*, void*);

struct Api {
    LoadBufferFn loadbuffer = nullptr;
    PCallFn pcall = nullptr;
    FieldFn getfield = nullptr;
    FieldFn setfield = nullptr;
    PushLStringFn pushlstring = nullptr;
    ToStringFn tostring = nullptr;
    ToNumberFn tonumber = nullptr;
    PushNamedCClosureFn pushnamedcclosure = nullptr;
    void** scriptMachine = nullptr;  // &g_ScriptGameMachine
};

Api _api;
TickFn4 _origTick = nullptr;
TickFn4 _origTickPaused = nullptr;

std::atomic<bool> _available{false};
std::atomic<bool> _disabled{false};
std::atomic<uint32_t> _mainThread{0};
std::atomic<uint64_t> _ticks{0};

std::mutex _jobsMutex;
std::deque<Job> _jobs;
std::vector<TickFn> _tickFns;  // guarded by _jobsMutex for registration; copied per tick

struct Result {
    ResultFn fn;
    bool ok;
    std::string text;
};
std::mutex _resultsMutex;
std::vector<Result> _results;

float g_tickDt = 0;  // dt of the current script tick (read by InvokeTick)
LARGE_INTEGER _qpf{};
LARGE_INTEGER _lastTick{};

std::wstring _dllDir;
std::string _tag = "default";
float _devPollAccum = 0;
bool _f8Down = false;

struct Pattern {
    const char* name;
    const char* bytes;
};

uintptr_t Find(const Pattern& p) {
    uintptr_t a = core::mem::FindPattern(p.bytes);
    if (!a) {
        MC_LOG_ERROR("lua: pattern '%s' not found", p.name);
    } else {
        MC_LOG_INFO("lua: %s at exe+0x%llx", p.name,
                    static_cast<unsigned long long>(a - core::mem::MainModule().base));
    }
    return a;
}

lua_State* CurrentState() {
    if (!_api.scriptMachine) return nullptr;
    auto* machine = static_cast<uint8_t*>(*_api.scriptMachine);
    if (!machine) return nullptr;
    return *reinterpret_cast<lua_State**>(machine + kOffMachineState);
}

// SEH guard: no C++ objects with destructors in this function (C2712).
int FaultFilter(unsigned code, EXCEPTION_POINTERS* ep) {
    if (code != EXCEPTION_ACCESS_VIOLATION) return EXCEPTION_CONTINUE_SEARCH;
    MC_LOG_ERROR("lua: access violation at %p (%s address %p) inside a main-thread job; bridge disabled",
                 ep->ExceptionRecord->ExceptionAddress,
                 ep->ExceptionRecord->ExceptionInformation[0] ? "writing" : "reading",
                 reinterpret_cast<void*>(ep->ExceptionRecord->ExceptionInformation[1]));
    return EXCEPTION_EXECUTE_HANDLER;
}

void InvokeJob(void* job, lua_State* L) { (*static_cast<Job*>(job))(L); }
void InvokeTick(void* fn, lua_State* L) {
    reinterpret_cast<TickFn>(fn)(L, g_tickDt);
}

bool Guarded(void (*thunk)(void*, lua_State*), void* arg, lua_State* L) {
    __try {
        thunk(arg, L);
        return true;
    } __except (FaultFilter(GetExceptionCode(), GetExceptionInformation())) {
        return false;
    }
}

std::atomic<uint64_t> _detourCalls{0};

void DrainMainThread() {
    const uint64_t calls = _detourCalls.fetch_add(1, std::memory_order_relaxed);
    if (_disabled.load(std::memory_order_relaxed)) return;
    lua_State* L = CurrentState();
    if (calls == 0 || (!L && (calls & (calls - 1)) == 0)) {
        MC_LOG_INFO("lua: script tick #%llu on thread %lu: machine %p, lua_State %p",
                    static_cast<unsigned long long>(calls), GetCurrentThreadId(),
                    _api.scriptMachine ? *_api.scriptMachine : nullptr, static_cast<void*>(L));
    }
    if (!L) return;

    const DWORD tid = GetCurrentThreadId();
    if (_mainThread.load(std::memory_order_relaxed) != tid) {
        if (_mainThread.exchange(tid) == 0)
            MC_LOG_INFO("lua: first script tick on thread %lu, lua_State %p", tid, static_cast<void*>(L));
        else
            MC_LOG_WARN("lua: script tick moved to thread %lu", tid);
    }
    _ticks.fetch_add(1, std::memory_order_relaxed);

    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    g_tickDt = _lastTick.QuadPart ? static_cast<float>(static_cast<double>(now.QuadPart - _lastTick.QuadPart) /
                                                       static_cast<double>(_qpf.QuadPart))
                                  : 0.0f;
    if (g_tickDt > 1.0f) g_tickDt = 1.0f;
    _lastTick = now;

    std::deque<Job> jobs;
    std::vector<TickFn> tickFns;
    {
        std::lock_guard lock(_jobsMutex);
        jobs.swap(_jobs);
        tickFns = _tickFns;
    }
    const int top = GetTop(L);
    for (auto& job : jobs) {
        if (!Guarded(&InvokeJob, &job, L)) {
            _disabled = true;
            return;
        }
        SetTop(L, top);
    }
    for (TickFn fn : tickFns) {
        if (!Guarded(&InvokeTick, reinterpret_cast<void*>(fn), L)) {
            _disabled = true;
            return;
        }
        SetTop(L, top);
    }
}

// C_ScriptMachine::Tick / TickPaused (vtable +0x20 / +0x28). Shared by every C_ScriptMachine; only the game
// machine's tick drains our work.
uint64_t TickDetour(void* self, void* b, void* c, void* d) {
    uint64_t r = _origTick(self, b, c, d);
    if (_api.scriptMachine && self == *_api.scriptMachine) DrainMainThread();
    return r;
}

uint64_t TickPausedDetour(void* self, void* b, void* c, void* d) {
    uint64_t r = _origTickPaused(self, b, c, d);
    if (_api.scriptMachine && self == *_api.scriptMachine) DrainMainThread();
    return r;
}

std::string Narrow(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

bool ReadTextFile(const std::wstring& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

void RunDevFile(const std::wstring& path, const std::wstring& outPath, bool remove) {
    std::string src;
    if (!ReadTextFile(path, src)) {
        MC_LOG_WARN("lua: dev: cannot read %s", Narrow(path).c_str());
        return;
    }
    if (remove) DeleteFileW(path.c_str());
    MC_LOG_INFO("lua: dev: running %s (%zu bytes)", Narrow(path).c_str(), src.size());
    Run(std::move(src), "=dev", [outPath](bool ok, const std::string& result) {
        MC_LOG_INFO("lua: dev: %s: %s", ok ? "ok" : "ERROR", result.c_str());
        if (!outPath.empty()) {
            std::wstring tmp = outPath + L".tmp";
            {
                std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
                f << (ok ? "ok\n" : "error\n") << result;
            }
            MoveFileExW(tmp.c_str(), outPath.c_str(), MOVEFILE_REPLACE_EXISTING);
        }
    });
}

bool GameWindowFocused() {
    HWND fg = GetForegroundWindow();
    if (!fg) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

void PollDevConsole(float dt) {
    const bool down = (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
    if (down && !_f8Down && GameWindowFocused()) {
        RunDevFile(_dllDir + L"\\dev.lua", _dllDir + L"\\dev\\" + std::wstring(_tag.begin(), _tag.end()) + L".out.txt",
                   false);
    }
    _f8Down = down;

    _devPollAccum += dt;
    if (_devPollAccum < 0.5f) return;
    _devPollAccum = 0;
    const std::wstring wtag(_tag.begin(), _tag.end());
    const std::wstring in = _dllDir + L"\\dev\\" + wtag + L".in.lua";
    if (GetFileAttributesW(in.c_str()) == INVALID_FILE_ATTRIBUTES) return;
    RunDevFile(in, _dllDir + L"\\dev\\" + wtag + L".out.txt", true);
}

bool HasRoom(lua_State* L, int n);

int McLog(lua_State* L) {
    const int n = GetTop(L);
    std::string line;
    for (int i = 1; i <= n; ++i) {
        if (i > 1) line += ' ';
        const char* s = ToString(L, i);
        line += s ? s : (TypeOf(L, i) == TNil ? "nil" : TypeOf(L, i) == TBoolean ? "bool" : "<obj>");
    }
    MC_LOG_INFO("lua: %s", line.c_str());
    return 0;
}

// mc_u64("123" | "0x7b" | number) -> HKS ui64 value (type 11). The script API wants ui64 for hashes/ids
// (e.g. C_ScriptObjectDuplicator::DuplicateSceneObject arg 4) and plain Lua 5.1 HKS text has no ui64 literal.
int McU64(lua_State* L) {
    uint64_t v = 0;
    if (TypeOf(L, 1) == TString) {
        const char* s = ToString(L, 1);
        v = s ? _strtoui64(s, nullptr, 0) : 0;
    } else if (TypeOf(L, 1) == TNumber) {
        v = static_cast<uint64_t>(static_cast<double>(ToNumber(L, 1)));
    } else if (TypeOf(L, 1) == TUi64) {
        v = *reinterpret_cast<uint64_t*>(Base(L) + 8);
    }
    if (!HasRoom(L, 1)) return 0;
    uint8_t* top = TopRef(L);
    *reinterpret_cast<uint32_t*>(top) = TUi64;
    *reinterpret_cast<uint64_t*>(top + 8) = v;
    TopRef(L) = top + kTValueSize;
    return 1;
}

// mc_u64str(ui64) -> decimal string ("" if the argument is not a ui64)
int McU64Str(lua_State* L) {
    std::string out;
    if (TypeOf(L, 1) == TUi64) {
        const uint64_t v = *reinterpret_cast<uint64_t*>(Base(L) + 8);
        out = std::to_string(v);
    }
    PushString(L, out);
    return 1;
}

// mc_pad(buttons, holdMs [, lt, rt, lx, ly, rx, ry]) -> true if the test pad took it. Tests only: holds a virtual
// gamepad state (XINPUT_GAMEPAD_* button bits, triggers 0..255, sticks -32768..32767) for holdMs (core's
// ui_input::SetTestPad; needs test_pad = 1, else false).
int McPad(lua_State* L) {
    auto num = [L](int i) { return TypeOf(L, i) == TNumber ? static_cast<double>(ToNumber(L, i)) : 0.0; };
    auto i16 = [](double v) { return static_cast<int16_t>(v < -32768 ? -32768 : v > 32767 ? 32767 : v); };
    auto u8 = [](double v) { return static_cast<uint8_t>(v < 0 ? 0 : v > 255 ? 255 : v); };
    ui_input::TestPadState st;
    st.buttons = static_cast<uint16_t>(static_cast<uint32_t>(num(1)) & 0xFFFF);
    st.leftTrigger = u8(num(3));
    st.rightTrigger = u8(num(4));
    st.lx = i16(num(5));
    st.ly = i16(num(6));
    st.rx = i16(num(7));
    st.ry = i16(num(8));
    const double hold = num(2);
    PushBool(L, ui_input::SetTestPad(st, static_cast<uint32_t>(hold < 0 ? 0 : hold > 60000 ? 60000 : hold)));
    return 1;
}

// mc_entity(id) -> the entity wrapper with that entity id (the number inside a C_EntityGuid: tostring(guid) ends in
// it), or nil. game.entitywrapper:GetEntityByGUID only takes a C_EntityGuid object; this is the same lookup
// (thunk exe+0x239E510: entity manager -> entity -> its script wrapper) for an id that came over the network.
constexpr const char* kPatGetEntityByGuid =
    "48 89 5C 24 08 48 89 74 24 20 57 48 83 EC 20 BA 01 00 00 00 48 8B F1 44 8B C2 E8 ?? ?? ?? ?? 33 FF 4C 8D 44 24 "
    "38 48 8B CE 89 7C 24 38 8D 57 02 E8 ?? ?? ?? ?? 8B 5C 24 38 E8";
using EntMgrFn = void* (*)();
using EntFindFn = uint8_t* (*)(void* mgr, uint32_t id, uint64_t zero);
using RefFn = void** (*)(void* tmp, void* p);
using DerefFn = uint8_t* (*)(void* p);
using PushWrapperFn = void (*)(lua_State* L, void* wrapper);
struct EntityApi {
    EntMgrFn mgr = nullptr;
    EntFindFn find = nullptr;
    RefFn ref = nullptr;
    DerefFn deref = nullptr;
    PushWrapperFn push = nullptr;
} _ent;

uintptr_t CallTarget(uintptr_t site) {
    int32_t rel = 0;
    std::memcpy(&rel, reinterpret_cast<const void*>(site + 1), 4);
    return site + 5 + rel;
}

void* WrapperById(uint32_t id) {
    uint8_t* e = _ent.find(_ent.mgr(), id, 0);
    // the table is indexed by id >> 8: a slot reused by another entity has another id
    if (!e || *reinterpret_cast<uint32_t*>(e + 0x18) != id || !*reinterpret_cast<void**>(e + 0xA8)) return nullptr;
    uint8_t tmp[16] = {};
    void** r = _ent.ref(tmp, *reinterpret_cast<void**>(e + 0xA8));
    uint8_t* o = _ent.deref(*r);
    if (!o || !*reinterpret_cast<uint8_t**>(o + 0x10)) return nullptr;
    return *reinterpret_cast<void**>(*reinterpret_cast<uint8_t**>(o + 0x10) + 0xA8);
}

void* WrapperByIdGuarded(uint32_t id) {
    __try {
        return WrapperById(id);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

// The entity ids (GUID numbers) of every entity of one type (low byte of the id: 8 human, 9 player, 10 car, ...),
// space separated. Entities sit in a table indexed by id >> 8; the scan stops after 65536 empty slots in a row (8192 missed
// entities after a few reloads in one game: new ones are put further up the table).
// There is no "entities in a radius" call for scripts; this is it (the caller filters by position).
size_t EntityIdsGuarded(uint8_t type, uint32_t* out, size_t cap) {
    size_t n = 0;
    __try {
        void* mgr = _ent.mgr();
        uint32_t misses = 0;
        for (uint32_t idx = 1; idx < 0x100000 && misses < 65536 && n < cap; ++idx) {
            uint8_t* e = _ent.find(mgr, (idx << 8) | type, 0);
            if (!e) {
                ++misses;
                continue;
            }
            misses = 0;
            const uint32_t guid = *reinterpret_cast<uint32_t*>(e + 0x18);
            if ((guid & 0xFF) == type && (guid >> 8) == idx) out[n++] = guid;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return n;
}

int McEntityIds(lua_State* L) {
    const double t = TypeOf(L, 1) == TNumber ? static_cast<double>(ToNumber(L, 1)) : -1;
    std::string out;
    if (_ent.find && t >= 0 && t < 256) {
        static uint32_t ids[8192];
        const size_t n = EntityIdsGuarded(static_cast<uint8_t>(t), ids, std::size(ids));
        out.reserve(n * 9);
        for (size_t i = 0; i < n; ++i) {
            if (i) out += ' ';
            out += std::to_string(ids[i]);
        }
    }
    PushString(L, out);
    return 1;
}

// mc_entity_ids_of("25 26 88 90") -> the ids of every entity of any of those types (one walk over the table instead
// of one per type: the table is indexed by id >> 8 whatever the type, exe+0x233AC10), space separated. Used for the
// projectiles of a throw (docs/game-api.md 23: 25 grenade, 26 molotov, 88 projectile explosive, 90 satchel charge).
size_t EntityIdsOfTypesGuarded(const bool (&want)[256], uint32_t* out, size_t cap) {
    size_t n = 0;
    __try {
        void* mgr = _ent.mgr();
        uint32_t misses = 0;
        for (uint32_t idx = 1; idx < 0x100000 && misses < 65536 && n < cap; ++idx) {
            uint8_t* e = _ent.find(mgr, idx << 8, 0);
            if (!e) {
                ++misses;
                continue;
            }
            misses = 0;
            const uint32_t guid = *reinterpret_cast<uint32_t*>(e + 0x18);
            if (want[guid & 0xFF] && (guid >> 8) == idx) out[n++] = guid;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return n;
}

int McEntityIdsOf(lua_State* L) {
    const char* list = TypeOf(L, 1) == TString ? ToString(L, 1) : nullptr;
    std::string out;
    if (_ent.find && list) {
        bool want[256] = {};
        bool any = false;
        for (const char* p = list; *p;) {
            char* end = nullptr;
            const unsigned long t = std::strtoul(p, &end, 10);
            if (end == p) {
                ++p;
                continue;
            }
            if (t < 256) want[t] = any = true;
            p = end;
        }
        if (any) {
            static uint32_t ids[2048];
            const size_t n = EntityIdsOfTypesGuarded(want, ids, std::size(ids));
            for (size_t i = 0; i < n; ++i) {
                if (i) out += ' ';
                out += std::to_string(ids[i]);
            }
        }
    }
    PushString(L, out);
    return 1;
}

int McEntity(lua_State* L) {
    const double id = TypeOf(L, 1) == TNumber ? static_cast<double>(ToNumber(L, 1)) : -1;
    void* w = (_ent.push && id > 0) ? WrapperByIdGuarded(static_cast<uint32_t>(id)) : nullptr;
    if (!w) {
        PushNil(L);
        return 1;
    }
    _ent.push(L, w);
    return 1;
}

// ---- memory probes (dev) -------------------------------------------------------------------------------------------
// Reads go through ReadProcessMemory on our own process, so a bad pointer only fails the read.
bool ReadMem(const void* at, void* out, size_t n) {
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), at, out, n, &got) && got == n;
}

uint8_t* EntityById(uint32_t id) {
    __try {
        uint8_t* e = _ent.find(_ent.mgr(), id, 0);
        if (!e || *reinterpret_cast<uint32_t*>(e + 0x18) != id) return nullptr;
        return e;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

// mc_car_controls(id) -> steer, power, brake, handbrake, steerLinearity, steerAngle of car entity `id`, or nil.
// The car's vehicle part sits at entity + 0xF8 (the Lua setters C_WrapperCar::SetSteer/SetPower/SetBrake/
// SetHandbrake all call into [wrapper + 0x10] + 0xF8, and GetPowerWish reads entity + 0xACC = vehicle + 0x9D4); the
// fields are what those setters write (docs/re-notes.md 9, found statically, not checked in game): steering wish
// -1..1 at +0xA40 (after the setter's response curve, whose linearity is +0xA80), throttle 0..1 at +0x9D4, brake 0..1
// at +0x9C8, handbrake 0..1 at +0x9D0, and the wheels' current steering angle (radians, filtered toward the wish by
// the vehicle update) at +0xA24. Read with ReadProcessMemory: a wrong guess only fails the read.
constexpr uintptr_t kVehiclePart = 0xF8;
constexpr uintptr_t kCarSteerWish = 0xA40, kCarPower = 0x9D4, kCarBrake = 0x9C8, kCarHandbrake = 0x9D0,
                    kCarSteerLinearity = 0xA80, kCarSteerAngle = 0xA24;

int McCarControls(lua_State* L) {
    const double id = TypeOf(L, 1) == TNumber ? static_cast<double>(ToNumber(L, 1)) : -1;
    uint8_t* e = (_ent.find && id > 0 && (static_cast<uint32_t>(id) & 0xFF) == 10) ? EntityById(static_cast<uint32_t>(id))
                                                                                     : nullptr;
    float v[6] = {};
    const uintptr_t offs[6] = {kCarSteerWish, kCarPower, kCarBrake, kCarHandbrake, kCarSteerLinearity, kCarSteerAngle};
    bool ok = e != nullptr;
    for (int i = 0; ok && i < 6; ++i) ok = ReadMem(e + kVehiclePart + offs[i], &v[i], sizeof(float)) && std::isfinite(v[i]);
    // plausibility: a layout that is not the one we found gives garbage here, not a value in these ranges
    ok = ok && std::fabs(v[0]) <= 1.01f && v[1] >= -1.01f && v[1] <= 1.01f && v[2] >= -0.01f && v[2] <= 1.01f &&
         v[3] >= -0.01f && v[3] <= 1.01f && v[4] >= -0.01f && v[4] <= 1.01f && std::fabs(v[5]) <= 3.2f;
    if (!ok) {
        PushNil(L);
        return 1;
    }
    for (float f : v) PushNumber(L, f);
    return 6;
}

// ---- car signals: lights, indicators, horn, siren, beacon (docs/re-notes.md 9, game/car_signals.h) -------------------
// Found statically (2026-10-18), NOT checked in game. Vehicle part fields: lights dword +0xD00 (the lights block at
// +0xCF0, its field +0x10), horn byte +0xA88, siren bit 0x2000000 of +0x119C. The patterns are the game's own setters;
// their bytes contain those offsets, so finding them confirms the layout of this build:
//   light group setter (exe+0x1C0D640, part, on, group1): group1 = bit 0x01, else bit 0x02 (the one with a listener)
//   indicator setter (exe+0x1C0AB60, part, on, a): a = bit 0x08, else bit 0x10 (plain bit writes)
//   horn (exe+0x1C0A820, ends with mov [rcx+0xA88], r10b) and siren (exe+0x1C0DFC0) part setters: offsets only
// Horn, siren and beacon are set on a copy through the script bindings (SetHornOn plays the horn sound through the
// entity, SetSirenOn / SetBeaconLightOn); the lights and indicators through the setters here.
constexpr const char* kPatCarLightGroup =
    "48 81 C1 F0 0C 00 00 E9 04 00 00 00 CC CC CC CC 48 83 EC 28 4C 8B C9 84 D2 74 24 45 84 C0 74 0D";
constexpr const char* kPatCarIndicator =
    "B8 08 00 00 00 45 84 C0 41 B9 10 00 00 00 44 0F 45 C8 0F B6 C2 F7 D8 41 23 C1 41 F7 D1 44 23 89 00 0D 00 00";
constexpr const char* kPatCarHorn = "44 0F B6 D2 B8 00 00 80 00 41 B8 FF FF 7F FF 84 D2 74 08 09 81 9C 11 00 00";
constexpr const char* kPatCarSiren = "44 0F B6 D2 B8 00 00 00 02 41 B8 FF FF FF FD 84 D2 74 08 09 81 9C 11 00 00";
constexpr uintptr_t kCarLights = 0xD00, kCarHorn = 0xA88, kCarFlags = 0x119C;
using CarBitSetterFn = void (*)(void* part, bool on, bool which);
struct {
    CarBitSetterFn lightGroup = nullptr;
    CarBitSetterFn indicator = nullptr;
    bool read = false;  // the lights, horn and siren offsets are confirmed
} _sig;

void InstallCarSignals() {
    const uintptr_t lg = core::mem::FindPattern(kPatCarLightGroup);
    const uintptr_t ind = core::mem::FindPattern(kPatCarIndicator);
    const uintptr_t horn = core::mem::FindPattern(kPatCarHorn);
    const uintptr_t siren = core::mem::FindPattern(kPatCarSiren);
    _sig.read = lg && horn && siren &&
                std::memcmp(reinterpret_cast<const void*>(horn + 0x85), "\x44\x88\x91\x88\x0A\x00\x00", 7) == 0;
    if (lg) _sig.lightGroup = reinterpret_cast<CarBitSetterFn>(lg);
    if (ind) _sig.indicator = reinterpret_cast<CarBitSetterFn>(ind);
    if (!_sig.read)
        MC_LOG_WARN("lua: car signals not found (lights %d, indicators %d, horn %d, siren %d): lights, horn and siren of "
                    "remote players' cars are not synced",
                    lg ? 1 : 0, ind ? 1 : 0, horn ? 1 : 0, siren ? 1 : 0);
    else
        MC_LOG_INFO("lua: car signals: light setter exe+0x%llx, indicator setter exe+0x%llx",
                    static_cast<unsigned long long>(lg - core::mem::MainModule().base),
                    static_cast<unsigned long long>(ind ? ind - core::mem::MainModule().base : 0));
}

uint8_t* CarById(double id) {
    return (_ent.find && id > 0 && (static_cast<uint32_t>(id) & 0xFF) == 10) ? EntityById(static_cast<uint32_t>(id))
                                                                              : nullptr;
}

// The signals byte of car entity e (car_signals::kValid set), or 0 when a read fails.
uint8_t ReadSignals(uint8_t* e) {
    uint32_t lights = 0, flags = 0;
    uint8_t horn = 0;
    uint8_t* part = e + kVehiclePart;
    if (!ReadMem(part + kCarLights, &lights, 4) || !ReadMem(part + kCarHorn, &horn, 1) ||
        !ReadMem(part + kCarFlags, &flags, 4) || horn > 1)
        return 0;
    return car_signals::FromGame(lights, horn, flags);
}

// mc_car_signals(id) -> the signals byte of car entity `id` (game/car_signals.h: 128 + bits), or nil.
int McCarSignals(lua_State* L) {
    uint8_t* e = _sig.read ? CarById(TypeOf(L, 1) == TNumber ? static_cast<double>(ToNumber(L, 1)) : -1) : nullptr;
    const uint8_t s = e ? ReadSignals(e) : 0;
    if (!s) {
        PushNil(L);
        return 1;
    }
    PushNumber(L, s);
    return 1;
}

bool SetCarBitGuarded(CarBitSetterFn f, uint8_t* part, bool on, bool which) {
    __try {
        f(part, on, which);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// mc_car_set_signals(id, sig, mask) -> the signals byte read back after setting the light bits of `mask` (park, head,
// indicators: car_signals::kLightBits) to those of `sig` on car entity `id`; nil when the car or the setters are not
// there. Only bits that differ are set. Main thread (script tick).
int McCarSetSignals(lua_State* L) {
    auto num = [L](int i) { return TypeOf(L, i) == TNumber ? static_cast<double>(ToNumber(L, i)) : -1.0; };
    uint8_t* e = (_sig.read && _sig.lightGroup) ? CarById(num(1)) : nullptr;
    const double sigArg = num(2), maskArg = num(3);
    const uint8_t have = e ? ReadSignals(e) : 0;
    if (!have || sigArg < 0 || sigArg > 255 || maskArg < 0 || maskArg > 255) {
        PushNil(L);
        return 1;
    }
    const uint8_t want = static_cast<uint8_t>(sigArg);
    const uint8_t diff = static_cast<uint8_t>((want ^ have) & static_cast<uint8_t>(maskArg) & car_signals::kLightBits);
    uint8_t* part = e + kVehiclePart;
    bool ok = true;
    if (diff & car_signals::kPark) ok &= SetCarBitGuarded(_sig.lightGroup, part, (want & car_signals::kPark) != 0, true);
    if (diff & car_signals::kHead) ok &= SetCarBitGuarded(_sig.lightGroup, part, (want & car_signals::kHead) != 0, false);
    if (_sig.indicator) {
        if (diff & car_signals::kIndA) ok &= SetCarBitGuarded(_sig.indicator, part, (want & car_signals::kIndA) != 0, true);
        if (diff & car_signals::kIndB) ok &= SetCarBitGuarded(_sig.indicator, part, (want & car_signals::kIndB) != 0, false);
    }
    const uint8_t now = ok ? ReadSignals(e) : 0;
    if (!now) {
        PushNil(L);
        return 1;
    }
    PushNumber(L, now);
    return 1;
}

// mc_fnv(text) -> the game's 64-bit name hash of `text` (FNV-1, the hash behind spawn profile names) as a decimal string
int McFnv(lua_State* L) {
    const char* s = ToString(L, 1);
    uint64_t h = 0xcbf29ce484222325ull;
    for (const char* c = s ? s : ""; *c; ++c) h = (h * 0x100000001b3ull) ^ static_cast<uint8_t>(*c);
    PushString(L, std::to_string(h));
    return 1;
}

// mc_memfind(id, value [, span [, depth]]) -> where `value` sits in the object of entity `id` and in the objects that
// object points to (pointers in its first `span` bytes, down to `depth` levels, 1-3): "+0x1a0 +0x10>+0x38 ...". A
// decimal value is searched as a u64 (8-aligned), anything else as text (any alignment). Dev only: finding a field.
void MemFindRec(uint64_t at, int level, int depth, size_t span, const std::string& needle, bool text, std::string& path,
                std::string& out, std::unordered_set<uint64_t>& seen) {
    if (out.size() > 3000 || seen.size() > 400000) return;
    std::vector<uint8_t> buf(span);
    if (!ReadMem(reinterpret_cast<void*>(at), buf.data(), span)) return;
    seen.insert(at);
    char tmp[32];
    const size_t step = text ? 1 : 8;
    for (size_t i = 0; i + needle.size() <= span; i += step) {
        if (std::memcmp(buf.data() + i, needle.data(), needle.size()) == 0) {
            std::snprintf(tmp, sizeof tmp, "+0x%zx ", i);
            out += path + tmp;
        }
    }
    if (level + 1 >= depth) return;
    for (size_t i = 0; i + 8 <= span; i += 8) {
        uint64_t p = 0;
        std::memcpy(&p, buf.data() + i, 8);
        if (p < 0x10000 || p > 0x7FFFFFFFFFFFull || (p & 7)) continue;
        if (seen.count(p)) continue;
        std::snprintf(tmp, sizeof tmp, "+0x%zx>", i);
        const size_t keep = path.size();
        path += tmp;
        MemFindRec(p, level + 1, depth, level + 1 >= 2 ? 0x100 : 0x400, needle, text, path, out, seen);
        path.resize(keep);
    }
}

int McMemFind(lua_State* L) {
    const double id = TypeOf(L, 1) == TNumber ? static_cast<double>(ToNumber(L, 1)) : -1;
    const char* vs = ToString(L, 2);
    const double spanArg = TypeOf(L, 3) == TNumber ? static_cast<double>(ToNumber(L, 3)) : 0x800;
    const size_t span = static_cast<size_t>(spanArg < 0x40 ? 0x40 : spanArg > 0x4000 ? 0x4000 : spanArg) & ~size_t(7);
    const double depthArg = TypeOf(L, 4) == TNumber ? static_cast<double>(ToNumber(L, 4)) : 2;
    const int depth = depthArg < 1 ? 1 : depthArg > 5 ? 5 : static_cast<int>(depthArg);
    uint8_t* e = (_ent.find && id > 0) ? EntityById(static_cast<uint32_t>(id)) : nullptr;
    std::string out;
    if (e && vs && *vs) {
        std::string needle;
        bool text = false;
        char* end = nullptr;
        const uint64_t v = _strtoui64(vs, &end, 10);
        if (end && *end == 0) {
            needle.assign(reinterpret_cast<const char*>(&v), 8);
        } else {
            needle = vs;
            text = true;
        }
        std::string path;
        std::unordered_set<uint64_t> seen;
        MemFindRec(reinterpret_cast<uint64_t>(e), 0, depth, span, needle, text, path, out, seen);
    }
    PushString(L, out);
    return 1;
}

// The RTTI class name of an object whose first qword is a vtable of the exe ("" if not one).
std::string ClassOf(uint64_t obj) {
    const uint64_t base = core::mem::MainModule().base, size = core::mem::MainModule().size;
    uint64_t vt = 0, col = 0;
    if (!ReadMem(reinterpret_cast<void*>(obj), &vt, 8) || vt < base || vt >= base + size) return "";
    if (!ReadMem(reinterpret_cast<void*>(vt - 8), &col, 8) || col < base || col >= base + size) return "";
    uint32_t sig = 0, td = 0;
    if (!ReadMem(reinterpret_cast<void*>(col), &sig, 4) || sig != 1) return "";
    if (!ReadMem(reinterpret_cast<void*>(col + 12), &td, 4)) return "";
    char name[96] = {};
    if (!ReadMem(reinterpret_cast<void*>(base + td + 16), name, sizeof name - 1)) return "";
    return name;
}

// mc_memdump(id, path, bytes) -> the qwords at a path of entity `id` (path as mc_memfind prints it, "" = the entity),
// one line each: offset, value, and the class of the object it points to. Dev only.
int McMemDump(lua_State* L) {
    const double id = TypeOf(L, 1) == TNumber ? static_cast<double>(ToNumber(L, 1)) : -1;
    const char* path = ToString(L, 2);
    const double nArg = TypeOf(L, 3) == TNumber ? static_cast<double>(ToNumber(L, 3)) : 0x100;
    const size_t n = static_cast<size_t>(nArg < 8 ? 8 : nArg > 0x2000 ? 0x2000 : nArg) & ~size_t(7);
    uint8_t* e = (_ent.find && id > 0) ? EntityById(static_cast<uint32_t>(id)) : nullptr;
    std::string out;
    if (e) {
        uint64_t cur = reinterpret_cast<uint64_t>(e);
        bool ok = true;
        for (const char* s = path ? path : ""; ok && *s;) {
            while (*s == '+' || *s == '>' || *s == ' ') ++s;
            if (!*s) break;
            char* end = nullptr;
            const uint64_t off = _strtoui64(s, &end, 16);
            if (end == s) break;
            s = end;
            uint64_t v = 0;
            ok = ReadMem(reinterpret_cast<void*>(cur + off), &v, 8);
            cur = v;
        }
        std::vector<uint64_t> q(n / 8);
        const uint64_t base = core::mem::MainModule().base, size = core::mem::MainModule().size;
        if (ok && ReadMem(reinterpret_cast<void*>(cur), q.data(), n)) {
            char buf[160];
            std::string self = ClassOf(cur);
            if (!self.empty()) out += "self " + self + "\n";
            for (size_t i = 0; i < q.size(); ++i) {
                const uint64_t v = q[i];
                std::string what;
                if (v >= base && v < base + size) {
                    std::snprintf(buf, sizeof buf, "exe+0x%llx", static_cast<unsigned long long>(v - base));
                    what = buf;
                } else if (v >= 0x10000 && v <= 0x7FFFFFFFFFFFull && !(v & 7)) {
                    what = ClassOf(v);
                }
                std::snprintf(buf, sizeof buf, "+0x%zx %llx %s\n", i * 8, static_cast<unsigned long long>(v), what.c_str());
                out += buf;
            }
        }
    }
    PushString(L, out);
    return 1;
}

// mc_memread(id, "path") -> the u64 at a path as mc_memfind prints it ("+0x10>+0x38"), as a decimal string ("" if not
// readable). Dev only.
int McMemRead(lua_State* L) {
    const double id = TypeOf(L, 1) == TNumber ? static_cast<double>(ToNumber(L, 1)) : -1;
    const char* path = ToString(L, 2);
    uint8_t* e = (_ent.find && id > 0) ? EntityById(static_cast<uint32_t>(id)) : nullptr;
    std::string out;
    if (e && path) {
        uint64_t cur = reinterpret_cast<uint64_t>(e), v = 0;
        bool ok = true;
        for (const char* s = path; ok && *s;) {
            while (*s == '+' || *s == '>' || *s == ' ') ++s;
            if (!*s) break;
            char* end = nullptr;
            const uint64_t off = _strtoui64(s, &end, 16);
            if (end == s) break;
            s = end;
            ok = ReadMem(reinterpret_cast<void*>(cur + off), &v, 8);
            cur = v;
        }
        if (ok) out = std::to_string(*path ? v : cur);
    }
    PushString(L, out);
    return 1;
}

}  // namespace

// ---- ray cast (mc_raycast) ----------------------------------------------------------------------------------------
// The script binding game.physics:GetCollisionPointBelow (exe+0x23A2940 -> exe+0x23E83F0) is a real ray cast: it fills a
// 0x2C-byte query {vtable exe+0x4B357D0, from xyz at +8, to xyz at +0x14 (from.z - 500), filter 0x404DA386 at +0x20, 3 at
// +0x24, -1 at +0x28} and calls the physics world's cast (exe+0x1A68E50(world, query, result) -> bool; world =
// [exe+0x1A56C20() + 0x58]); the result has a ref-counted pointer at +0 (released with exe+0x19FD220) and the hit
// position at +8, the normal at +0x14. StartCollisionQuery never reported a hit. We build the same query with any
// from/to (docs/game-api.md 13).
constexpr const char* kPatPointBelow =
    "48 89 5C 24 08 48 89 7C 24 10 55 48 8D 6C 24 A9 48 81 EC E0 00 00 00 F2 41 0F 10 00 48 8D 05 ?? ?? ?? ?? F2 0F "
    "11 45 EF 49 8B F8 F3 0F 10 4D F3";
using PhysMgrFn = void* (*)();
using RayCastFn = bool (*)(void* world, void* query, void* result);
using ReleaseFn = void (*)(void* p);
struct {
    void* vtable = nullptr;
    PhysMgrFn mgr = nullptr;
    RayCastFn cast = nullptr;
    ReleaseFn release = nullptr;
} _ray;

void InstallRayCast() {
    const uintptr_t f = core::mem::FindPattern(kPatPointBelow);
    // the bytes the offsets below rely on: lea rax,[rip+x] / call / mov rcx,[rax+58h] / call / call (release)
    if (!f || *reinterpret_cast<const uint32_t*>(f + 0x55) != 0x404DA386 || *reinterpret_cast<const uint8_t*>(f + 0x9B) != 0xE8 ||
        std::memcmp(reinterpret_cast<const void*>(f + 0xA8), "\x48\x8B\x48\x58", 4) != 0 ||
        *reinterpret_cast<const uint8_t*>(f + 0xAC) != 0xE8 || *reinterpret_cast<const uint8_t*>(f + 0x1C0) != 0xE8) {
        MC_LOG_WARN("lua: ray cast not found: name tags show through walls");
        return;
    }
    _ray.vtable = reinterpret_cast<void*>(core::mem::ResolveRip(f + 0x1C, 3, 7));
    _ray.mgr = reinterpret_cast<PhysMgrFn>(CallTarget(f + 0x9B));
    _ray.cast = reinterpret_cast<RayCastFn>(CallTarget(f + 0xAC));
    _ray.release = reinterpret_cast<ReleaseFn>(CallTarget(f + 0x1C0));
}

// true = something is between from and to (hit position in hit[3])
bool RayCastGuarded(const float from[3], const float to[3], float hit[3], bool& ok) {
    alignas(16) uint8_t q[0x30] = {};
    alignas(16) uint8_t r[0x40] = {};
    *reinterpret_cast<void**>(q) = _ray.vtable;
    std::memcpy(q + 8, from, 12);
    std::memcpy(q + 0x14, to, 12);
    *reinterpret_cast<uint32_t*>(q + 0x20) = 0x404DA386;
    *reinterpret_cast<uint32_t*>(q + 0x24) = 3;
    *reinterpret_cast<int32_t*>(q + 0x28) = -1;
    *reinterpret_cast<uint16_t*>(r + 0x2E) = 0xFFFF;
    bool got = false;
    ok = false;
    __try {
        uint8_t* m = static_cast<uint8_t*>(_ray.mgr());
        void* world = m ? *reinterpret_cast<void**>(m + 0x58) : nullptr;
        if (world) {
            got = _ray.cast(world, q, r);
            ok = true;
            if (got) std::memcpy(hit, r + 8, 12);
            if (void* ref = *reinterpret_cast<void**>(r)) _ray.release(ref);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
        got = false;
    }
    return got;
}

// mc_raycast(x1, y1, z1, x2, y2, z2) -> hit (bool), x, y, z of the hit; nil when the ray cast is not available.
int McRayCast(lua_State* L) {
    float from[3], to[3], hit[3] = {};
    for (int i = 0; i < 3; ++i) {
        from[i] = static_cast<float>(ToNumber(L, 1 + i));
        to[i] = static_cast<float>(ToNumber(L, 4 + i));
    }
    bool ok = false;
    const bool got = _ray.cast ? RayCastGuarded(from, to, hit, ok) : false;
    if (!ok) {
        PushNil(L);
        return 1;
    }
    PushBool(L, got);
    PushNumber(L, hit[0]);
    PushNumber(L, hit[1]);
    PushNumber(L, hit[2]);
    return 4;
}

bool EntityLookupAvailable() { return _ent.find && _ent.mgr; }

bool EntityIsLive(const void* entity) {
    if (!entity || !EntityLookupAvailable()) return false;
    __try {
        const uint32_t id = *reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(entity) + 0x18);
        return _ent.find(_ent.mgr(), id, 0) == entity;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void* EntityByGuid(uint32_t id) { return EntityLookupAvailable() ? EntityById(id) : nullptr; }

// ---- setup ----------------------------------------------------------------------------------------------------
bool Install(const std::wstring& dllDir, const std::string& tag) {
    _dllDir = dllDir;
    if (!tag.empty()) _tag = tag;
    CreateDirectoryW((dllDir + L"\\dev").c_str(), nullptr);
    QueryPerformanceFrequency(&_qpf);

    // Patterns from docs/re-notes.md "Pattern table" (all unique in .text of build CRC32 3168979183).
    // C_GameScriptModule::Tick only gives us &g_ScriptGameMachine: in game it is never called (verified: hooked,
    // zero calls in world). The machine's own Tick/TickPaused are hooked instead (docs/re-notes.md 1.5).
    const Pattern pModuleTick{"C_GameScriptModule_Tick",
                              "48 8B 0D ?? ?? ?? ?? 48 85 C9 74 07 48 8B 01 48 FF 60 20 C3"};
    const Pattern pTick{"C_ScriptMachine_Tick", "40 53 48 83 EC 20 48 83 B9 D0 00 00 00 00 48 8B D9 74 28 80"};
    const Pattern pTickPaused{"C_ScriptMachine_TickPaused",
                              "40 53 48 83 EC 20 48 83 B9 D0 00 00 00 00 48 8B D9 74 2D 80"};
    const Pattern pLoad{"luaL_loadbuffer",
                        "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 30 49 8B F9 49 8B F0 48 8B EA 48 8B "
                        "D9 48 85 C9 75 3D E8 ?? ?? ?? ?? 48 8D 0D ?? ?? ?? ?? 41 B9 72 B4 6A"};
    const Pattern pPcall{"lua_pcall", "48 89 5C 24 18 55 56 57 41 54 41 55 41 56 41 57 48 83 EC 60 4C 63 FA 41"};
    const Pattern pGetField{"lua_getfield", "40 53 48 83 EC 60 0F 29 74 24 50 4D 8B C8 48 8B D9 81 FA F0"};
    const Pattern pSetField{"lua_setfield", "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 20 48 8B 41 48"};
    const Pattern pPushL{"lua_pushlstring", "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 40 BF 1F 00 00"};
    const Pattern pToStr{"lua_tostring", "4C 8B C9 81 FA F0 D8 FF FF 7E 37 85 D2 7E 1D 48 63 D2 48 C1"};
    const Pattern pToNum{"lua_tonumber", "4C 8B C1 81 FA F0 D8 FF FF 7E 35 85 D2 7E 1A 48 63 D2 48 C1"};
    const Pattern pClosure{"hks_pushnamedcclosure",
                           "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 30 49 8B F9 41 8B F0 48 8B"};

    const uintptr_t moduleTick = Find(pModuleTick), tick = Find(pTick), tickPaused = Find(pTickPaused);
    _api.loadbuffer = reinterpret_cast<LoadBufferFn>(Find(pLoad));
    _api.pcall = reinterpret_cast<PCallFn>(Find(pPcall));
    _api.getfield = reinterpret_cast<FieldFn>(Find(pGetField));
    _api.setfield = reinterpret_cast<FieldFn>(Find(pSetField));
    _api.pushlstring = reinterpret_cast<PushLStringFn>(Find(pPushL));
    _api.tostring = reinterpret_cast<ToStringFn>(Find(pToStr));
    _api.tonumber = reinterpret_cast<ToNumberFn>(Find(pToNum));
    _api.pushnamedcclosure = reinterpret_cast<PushNamedCClosureFn>(Find(pClosure));
    if (!moduleTick || !tick || !_api.loadbuffer || !_api.pcall || !_api.getfield || !_api.setfield || !_api.pushlstring ||
        !_api.tostring || !_api.tonumber || !_api.pushnamedcclosure) {
        MC_LOG_ERROR("lua: bridge disabled (missing patterns)");
        core::compat::Failed("Lua bridge", true);  // required: player sync runs on it
        return false;
    }
    _api.scriptMachine = reinterpret_cast<void**>(core::mem::ResolveRip(moduleTick, 3, 7));
    MC_LOG_INFO("lua: &g_ScriptGameMachine = exe+0x%llx",
                static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(_api.scriptMachine) -
                                                core::mem::MainModule().base));

    if (!core::hook::Install("C_ScriptMachine::Tick", reinterpret_cast<void*>(tick),
                             reinterpret_cast<void*>(&TickDetour), reinterpret_cast<void**>(&_origTick))) {
        MC_LOG_ERROR("lua: bridge disabled (Tick hook failed)");
        core::compat::Failed("Lua bridge", true);  // required: player sync runs on it
        return false;
    }
    if (tickPaused) {
        core::hook::Install("C_ScriptMachine::TickPaused", reinterpret_cast<void*>(tickPaused),
                            reinterpret_cast<void*>(&TickPausedDetour), reinterpret_cast<void**>(&_origTickPaused));
    }
    if (const uintptr_t t = core::mem::FindPattern(kPatGetEntityByGuid)) {
        _ent.mgr = reinterpret_cast<EntMgrFn>(CallTarget(t + 0x39));
        _ent.find = reinterpret_cast<EntFindFn>(CallTarget(t + 0x46));
        _ent.ref = reinterpret_cast<RefFn>(CallTarget(t + 0x61));
        _ent.deref = reinterpret_cast<DerefFn>(CallTarget(t + 0x69));
        _ent.push = reinterpret_cast<PushWrapperFn>(CallTarget(t + 0x89));
    } else {
        MC_LOG_WARN("lua: GetEntityByGUID not found: mc_entity unavailable (NPCs and mission cars are not matched by id)");
    }
    InstallRayCast();
    if (_ent.find) InstallCarSignals();
    _available = true;
    // Our helpers for scripts; registered on the first tick.
    Post([](lua_State* L) {
        RegisterGlobal(L, "mc_log", &McLog);
        RegisterGlobal(L, "mc_u64", &McU64);
        RegisterGlobal(L, "mc_u64str", &McU64Str);
        RegisterGlobal(L, "mc_pad", &McPad);
        RegisterGlobal(L, "mc_fnv", &McFnv);
        if (_ent.push) {
            RegisterGlobal(L, "mc_entity", &McEntity);
            RegisterGlobal(L, "mc_entity_ids", &McEntityIds);
            RegisterGlobal(L, "mc_entity_ids_of", &McEntityIdsOf);
            RegisterGlobal(L, "mc_memfind", &McMemFind);
            RegisterGlobal(L, "mc_memread", &McMemRead);
            RegisterGlobal(L, "mc_memdump", &McMemDump);
            RegisterGlobal(L, "mc_car_controls", &McCarControls);
            if (_sig.read) RegisterGlobal(L, "mc_car_signals", &McCarSignals);
            if (_sig.read && _sig.lightGroup) RegisterGlobal(L, "mc_car_set_signals", &McCarSetSignals);
        }
        if (_ray.cast) RegisterGlobal(L, "mc_raycast", &McRayCast);
        MC_LOG_INFO("lua: registered globals mc_log, mc_u64, mc_u64str, mc_pad%s%s%s",
                    _ent.push ? ", mc_entity, mc_entity_ids, mc_entity_ids_of, mc_car_controls" : "",
                    _ent.push && _sig.read ? ", mc_car_signals, mc_car_set_signals" : "", _ray.cast ? ", mc_raycast" : "");
    });
    MC_LOG_INFO("lua: bridge ready (dev console: F8 runs %s\\dev.lua; %s\\dev\\%s.in.lua is polled)",
                Narrow(_dllDir).c_str(), Narrow(_dllDir).c_str(), _tag.c_str());
    return true;
}

bool Available() { return _available.load() && !_disabled.load(); }

int RayCast(const float from[3], const float to[3], float hit[3]) {
    if (!_ray.cast || !Available()) return -1;
    bool ok = false;
    const bool got = RayCastGuarded(from, to, hit, ok);
    return ok ? (got ? 1 : 0) : -1;
}
bool Disabled() { return _disabled.load(); }
uint32_t MainThreadId() { return _mainThread.load(); }
uint64_t TickCount() { return _ticks.load(); }

void Post(Job job) {
    if (!Available()) return;
    std::lock_guard lock(_jobsMutex);
    _jobs.push_back(std::move(job));
}

void AddTickCallback(TickFn fn) {
    std::lock_guard lock(_jobsMutex);
    _tickFns.push_back(fn);
}

void Run(std::string source, std::string chunkName, ResultFn done) {
    if (!Available()) {
        if (done) {
            std::lock_guard lock(_resultsMutex);
            _results.push_back({std::move(done), false, "lua bridge unavailable"});
        }
        return;
    }
    auto src = std::make_shared<std::string>(std::move(source));
    auto name = std::make_shared<std::string>(std::move(chunkName));
    auto cb = std::make_shared<ResultFn>(std::move(done));
    Post([src, name, cb](lua_State* L) {
        std::string err;
        Result r{*cb, false, {}};
        if (DoString(L, src->data(), src->size(), name->c_str(), 1, &err)) {
            r.ok = true;
            r.text = ToDisplay(L, -1);
        } else {
            r.text = err;
        }
        if (*cb) {
            std::lock_guard lock(_resultsMutex);
            _results.push_back(std::move(r));
        }
    });
}

void PollRender(float dt) {
    std::vector<Result> results;
    {
        std::lock_guard lock(_resultsMutex);
        results.swap(_results);
    }
    for (auto& r : results) {
        if (r.fn) r.fn(r.ok, r.text);
    }
    if (_available.load()) PollDevConsole(dt);
}

// ---- main-thread helpers ----------------------------------------------------------------------------------------
int GetTop(lua_State* L) { return static_cast<int>((TopRef(L) - Base(L)) / kTValueSize); }

void SetTop(lua_State* L, int idx) {
    uint8_t*& top = TopRef(L);
    if (idx >= 0) {
        uint8_t* target = Base(L) + static_cast<size_t>(idx) * kTValueSize;
        while (top < target) {
            *reinterpret_cast<uint32_t*>(top) = TNil;
            top += kTValueSize;
        }
        top = target;
    } else {
        top += static_cast<ptrdiff_t>(idx + 1) * static_cast<ptrdiff_t>(kTValueSize);
    }
}

void Pop(lua_State* L, int n) { SetTop(L, -n - 1); }

namespace {
uint8_t* Index2Adr(lua_State* L, int idx) {
    if (idx > 0) {
        uint8_t* p = Base(L) + static_cast<size_t>(idx - 1) * kTValueSize;
        return p < TopRef(L) ? p : nullptr;
    }
    if (idx > kRegistryIndex) {
        uint8_t* p = TopRef(L) + static_cast<ptrdiff_t>(idx) * static_cast<ptrdiff_t>(kTValueSize);
        return p >= Base(L) ? p : nullptr;
    }
    if (idx == kGlobalsIndex) return reinterpret_cast<uint8_t*>(L) + kOffGlobals;
    return nullptr;
}

bool HasRoom(lua_State* L, int n) { return TopRef(L) + static_cast<size_t>(n) * kTValueSize <= Limit(L); }

bool PushValue(lua_State* L, int idx) {
    uint8_t* src = Index2Adr(L, idx);
    if (!src || !HasRoom(L, 1)) return false;
    std::memcpy(TopRef(L), src, kTValueSize);
    TopRef(L) += kTValueSize;
    return true;
}
}  // namespace

int TypeOf(lua_State* L, int idx) {
    uint8_t* p = Index2Adr(L, idx);
    if (!p) return TNone;
    return static_cast<int>(*reinterpret_cast<uint32_t*>(p) & 0xF);
}

bool PushNumber(lua_State* L, float v) {
    if (!HasRoom(L, 1)) return false;
    uint8_t* top = TopRef(L);
    *reinterpret_cast<uint32_t*>(top) = TNumber;
    *reinterpret_cast<float*>(top + 8) = v;
    TopRef(L) = top + kTValueSize;
    return true;
}

bool PushBool(lua_State* L, bool v) {
    if (!HasRoom(L, 1)) return false;
    uint8_t* top = TopRef(L);
    *reinterpret_cast<uint32_t*>(top) = TBoolean;
    *reinterpret_cast<int*>(top + 8) = v ? 1 : 0;
    TopRef(L) = top + kTValueSize;
    return true;
}

bool PushNil(lua_State* L) {
    if (!HasRoom(L, 1)) return false;
    *reinterpret_cast<uint32_t*>(TopRef(L)) = TNil;
    TopRef(L) += kTValueSize;
    return true;
}

void PushString(lua_State* L, const char* s, size_t len) { _api.pushlstring(L, s, len); }
void PushString(lua_State* L, const std::string& s) { _api.pushlstring(L, s.data(), s.size()); }
void GetField(lua_State* L, int idx, const char* key) { _api.getfield(L, idx, key); }
void SetField(lua_State* L, int idx, const char* key) { _api.setfield(L, idx, key); }
void GetGlobal(lua_State* L, const char* name) { _api.getfield(L, kGlobalsIndex, name); }
float ToNumber(lua_State* L, int idx) { return _api.tonumber(L, idx); }

const char* ToString(lua_State* L, int idx) {
    const int t = TypeOf(L, idx);
    if (t != TString && t != TNumber) return nullptr;
    return _api.tostring(L, idx);
}

std::string ToDisplay(lua_State* L, int idx) {
    const int t = TypeOf(L, idx);
    if (t == TNone) return "none";
    if (t == TNil) return "nil";
    if (t == TString || t == TNumber) {
        const char* s = _api.tostring(L, idx);
        return s ? s : "";
    }
    const int abs = idx > 0 ? idx : GetTop(L) + idx + 1;
    const int top = GetTop(L);
    GetGlobal(L, "tostring");
    std::string out = "<?>";
    if (PushValue(L, abs)) {
        std::string err;
        if (PCall(L, 1, 1, &err)) {
            const char* s = ToString(L, -1);
            out = s ? s : "<?>";
        } else {
            out = "<tostring failed: " + err + ">";
        }
    }
    SetTop(L, top);
    return out;
}

bool PCall(lua_State* L, int nargs, int nresults, std::string* err) {
    const int status = _api.pcall(L, nargs, nresults, 0);
    if (status == 0) return true;
    if (err) {
        const char* s = ToString(L, -1);
        *err = s ? s : ("error object of type " + std::to_string(TypeOf(L, -1)));
    }
    Pop(L, 1);
    return false;
}

bool DoString(lua_State* L, const char* src, size_t len, const char* chunkName, int nresults, std::string* err) {
    const int status = _api.loadbuffer(L, src, len, chunkName);
    if (status != 0) {
        if (err) {
            const char* s = ToString(L, -1);
            *err = std::string("compile: ") + (s ? s : "?");
        }
        Pop(L, 1);
        return false;
    }
    return PCall(L, 0, nresults, err);
}

void RegisterGlobal(lua_State* L, const char* name, CFunction fn) {
    _api.pushnamedcclosure(L, fn, 0, name, 0);
    _api.setfield(L, kGlobalsIndex, name);
}

}  // namespace mc::game::lua
