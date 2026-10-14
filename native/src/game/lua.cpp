#include "game/lua.h"

#include "core/compat.h"
#include "core/hook.h"
#include "core/log.h"
#include "core/mem.h"

#include <windows.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <mutex>
#include <sstream>
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

}  // namespace

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
        core::compat::Failed(core::compat::Feature::LuaBridge);
        return false;
    }
    _api.scriptMachine = reinterpret_cast<void**>(core::mem::ResolveRip(moduleTick, 3, 7));
    MC_LOG_INFO("lua: &g_ScriptGameMachine = exe+0x%llx",
                static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(_api.scriptMachine) -
                                                core::mem::MainModule().base));

    if (!core::hook::Install("C_ScriptMachine::Tick", reinterpret_cast<void*>(tick),
                             reinterpret_cast<void*>(&TickDetour), reinterpret_cast<void**>(&_origTick))) {
        MC_LOG_ERROR("lua: bridge disabled (Tick hook failed)");
        core::compat::Failed(core::compat::Feature::LuaBridge);
        return false;
    }
    if (tickPaused) {
        core::hook::Install("C_ScriptMachine::TickPaused", reinterpret_cast<void*>(tickPaused),
                            reinterpret_cast<void*>(&TickPausedDetour), reinterpret_cast<void**>(&_origTickPaused));
    }
    _available = true;
    // Our helpers for scripts; registered on the first tick.
    Post([](lua_State* L) {
        RegisterGlobal(L, "mc_log", &McLog);
        RegisterGlobal(L, "mc_u64", &McU64);
        RegisterGlobal(L, "mc_u64str", &McU64Str);
        MC_LOG_INFO("lua: registered globals mc_log, mc_u64, mc_u64str");
    });
    MC_LOG_INFO("lua: bridge ready (dev console: F8 runs %s\\dev.lua; %s\\dev\\%s.in.lua is polled)",
                Narrow(_dllDir).c_str(), Narrow(_dllDir).c_str(), _tag.c_str());
    return true;
}

bool Available() { return _available.load() && !_disabled.load(); }
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
