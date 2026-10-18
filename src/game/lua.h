#pragma once
// Bridge into the game's Havok Script (Lua 5.1-compatible, float numbers) VM. See docs/re-notes.md 1.x.
//
// Threads: the game runs its scripts on GameMainThread from C_GameScriptModule::Tick. We hook that function (and
// TickPaused); after the original returns, the hook drains a mutex-protected job queue and then calls the
// registered main-thread tick callbacks. Every game/Lua call must happen there. The render thread (Mod::Tick)
// posts jobs with Post()/Run() and receives Run() results in PollRender().
//
// Guarding: Install() scans for every pattern it needs; anything missing disables the bridge (logged), nothing
// crashes. Each job runs with the Lua stack top saved and restored. A native access violation inside a job is
// caught (SEH), logged, and disables the bridge for good (the VM may be inconsistent after that).
//
// Dev console: F8 (game window focused) runs <dll dir>\dev.lua; a file <dll dir>\dev\<tag>.in.lua (tag = --log
// tag, else "default") is picked up automatically (polled twice a second), run, deleted, and its result written
// to <dll dir>\dev\<tag>.out.txt and the log. The chunk's first return value is converted with tostring().
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace mc::game {

struct lua_State;  // opaque: HKS lua_State

namespace lua {

constexpr int kRegistryIndex = -10000;
constexpr int kGlobalsIndex = -10002;

enum Type : int { TNone = -1, TNil = 0, TBoolean = 1, TLightUserdata = 2, TNumber = 3, TString = 4, TTable = 5,
                  TFunction = 6, TUserdata = 7, TThread = 8, TIFunction = 9, TCFunction = 10, TUi64 = 11,
                  TStruct = 12 };

// ---- setup (any thread, once) -------------------------------------------------------------------------------
// dllDir: for the dev console files; tag: log tag ("" -> "default").
bool Install(const std::wstring& dllDir, const std::string& tag);
bool Available();          // hooks installed and functions resolved
bool Disabled();           // gave up after a fault
uint32_t MainThreadId();   // 0 until the first script tick
uint64_t TickCount();      // script ticks seen

// ---- render thread (or any thread) ----------------------------------------------------------------------------
using Job = std::function<void(lua_State* L)>;
void Post(Job job);  // runs on GameMainThread at the next script tick (dropped if the bridge is unavailable)

using ResultFn = std::function<void(bool ok, const std::string& result)>;
// Compiles and runs `source` on the main thread. ok=false: compile/runtime error text. Callback runs in PollRender.
void Run(std::string source, std::string chunkName, ResultFn done);
void PollRender(float dt);  // render thread, once per frame: results + dev console

// ---- main thread: called every script tick after the job queue (dt in seconds since the previous tick) -------
using TickFn = void (*)(lua_State* L, float dt);
void AddTickCallback(TickFn fn);

// ---- main-thread helpers (only inside a job or tick callback) -------------------------------------------------
int GetTop(lua_State* L);
void SetTop(lua_State* L, int idx);
void Pop(lua_State* L, int n);
int TypeOf(lua_State* L, int idx);
bool PushNumber(lua_State* L, float v);   // false: no stack space (nothing pushed)
bool PushBool(lua_State* L, bool v);
bool PushNil(lua_State* L);
void PushString(lua_State* L, const char* s, size_t len);
void PushString(lua_State* L, const std::string& s);
void GetField(lua_State* L, int idx, const char* key);  // pushes t[key] (honours metamethods)
void SetField(lua_State* L, int idx, const char* key);  // t[key] = top, pops it
void GetGlobal(lua_State* L, const char* name);
float ToNumber(lua_State* L, int idx);
const char* ToString(lua_State* L, int idx);  // number/string only (else nullptr); converts numbers in place
std::string ToDisplay(lua_State* L, int idx);   // like Lua tostring(v), never fails
// Loads a chunk (pushes the function) -> pcall(nargs = 0, nresults). On error leaves nothing and fills *err.
bool DoString(lua_State* L, const char* src, size_t len, const char* chunkName, int nresults, std::string* err);
// pcall of the function below nargs arguments; on error pops the message into *err.
bool PCall(lua_State* L, int nargs, int nresults, std::string* err);
// Registers a C function as a global.
using CFunction = int (*)(lua_State*);
void RegisterGlobal(lua_State* L, const char* name, CFunction fn);

// The game's entity table (the lookup behind mc_entity): true when `entity` is a live entity, i.e. its id still
// resolves to it. Never touches freed memory unguarded. GameMainThread.
bool EntityLookupAvailable();
// The game's physics ray cast (the one behind mc_raycast): 1 something is between from and to (its position in hit),
// 0 nothing, -1 not available. GameMainThread.
int RayCast(const float from[3], const float to[3], float hit[3]);
bool EntityIsLive(const void* entity);

}  // namespace lua
}  // namespace mc::game
