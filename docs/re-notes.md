# re-notes: mafiadefinitiveedition.exe (Steam, CRC32 3168979183)

This is static reverse engineering of the game binary only. Clean-room rule: no other mod's code was used.
All addresses are **RVAs**. Add the runtime module base to get an address; the preferred image base is
0x140000000. Every pattern listed here was checked to match **exactly once** in `.text` with
`re-tools/scan.py` (see "Pattern table" at the end). Re-run that check after any game update:

```
python docs/re-tools/scan.py "<game>\mafiadefinitiveedition.exe" "48 8B 0D ?? ?? ?? ?? 48 85 C9 74 07 48 8B 01 48 FF 60 20 C3"
python docs/re-tools/scan.py "<game>\mafiadefinitiveedition.exe" --file docs/re-tools/patterns.txt   (all 69 patterns below; prints OK/FAIL each)
```

Confidence tags:
- **[confirmed]**: several independent xrefs or code behaviours agree.
- **[likely]**: one strong indicator.
- **[guess]**: inference only; verify at runtime.

Binary facts:
- Section layout: `.text` at RVA 0x1000 (size 0x4A60BDF), `.rdata` at 0x4A6F000, `.data` at 0x5A92000.
- `.pdata` is present, so function bounds come from the unwind info.
- RTTI is stripped: there are no complete-object locators in front of game vtables. Class names therefore come from
  three other sources: `[TM]` profiler strings, Lua class-registration names, and assert strings.
- The game ships an assert-enabled **Havok Script** VM. Every public API function holds `lea r8, "hksi_<name>"` in
  its parameter assert. That is how most of the Lua functions below were found, and how they can be found again
  after an update.

---------------------------------------------------------------------------------------------------------------

## 1. Lua VM

### 1.1 Which Lua

The VM is **Havok Script (HKS), which is compatible with Lua 5.1**. It is not stock Lua and not LuaJIT. [confirmed]
- Evidence:
  - The binary has no "Lua 5.x" version string and none of the stock Lua error strings (such as "attempt to call ...").
  - It does have about 110 assert names of the forms `hksi_lua_*`, `hksi_luaL_*`, `hksi_hks_*` and `hks_obj_*`.
  - Source paths are embedded: `...\3rd-code\src\havok\source\script\vm\virtualMachine\vm-core.cpp`, `vm-aux.cpp`,
    `hksObject.cpp` and `lua_impl.inl`.
  - Profiler strings name the VM: "HavokScriptVM:", "HavokScript Time".
- **`lua_Number` is a 32-bit `float`.** [confirmed] Pass and receive floats, not doubles.
  - Number TValues are written with `movss [top+8]` and type 3.
  - Number-to-string conversion uses `cvtps2pd` and `"%0.8g"` (hks_obj_tolstring, 0x4917FF0).
- The C API follows standard 5.1 semantics. [confirmed] The pseudo-indices appear in the index2adr copies as
  `0xFFFFD8F0/EF/EE`:
  - LUA_REGISTRYINDEX = -10000
  - LUA_ENVIRONINDEX = -10001
  - LUA_GLOBALSINDEX = -10002
- The VM registers these base libraries. Their luaL_Reg tables are in `.rdata` at 0x53F23D0..0x53F7D40. [confirmed]
  - `print`, `next`, `type`, `unpack`, `tonumber`, `tostring`, ...
  - `io`, `os` (`clock`, `exit`, `remove`, `rename`, `date`, `time`, ...)
  - `string`, `table`, `math`, `debug`, `package`, `struct`, `serialize`, `coroutine`
  - HKS extras: `profile`, `compilerSettings`, `persist`/`unpersist`
- **Loading Lua source text is allowed**, except into a "secure" state. [confirmed statically; easy to re-check at runtime]
  - The loader (0x4933610) rejects text chunks with "Attempt to load source code into a secure state." only when
    `*(int*)(global + 0x1C0) == 2`, where global = `*(L+0x10)`.
  - The game compiles text chunks itself at runtime (see C_ScriptMachine::RunString below), so the main state is
    not secure.

### 1.2 lua_State layout (HKS, x64) [confirmed from many inlined API copies]

| offset | meaning |
|---|---|
| +0x10 | `HksGlobal*`, the shared global state. Inside it: +0x1C8 = registry TValue, +0x1C0 = bytecode/secure mode, +0x4A8 = profiler counters, +0x558 = compiler settings (passed by luaL_loadbuffer). |
| +0x48 | `top` (TValue*): the next free slot |
| +0x50 | `base` (TValue*): the first argument of the current C frame (index 1 = base) |
| +0x58 | stack limit. Push code calls the grow-stack function 0x49206F0 when `top+n > limit`. |
| +0x60 | stack start |
| +0x70 | globals table TValue (LUA_GLOBALSINDEX) |
| +0xA0 | user pointer. The game stores the owning `C_ScriptMachine*` here (set in C_ScriptMachine::Init). |
| +0xC4 | profiler/C-call state (-500 = 0xFFFFFE0C is a sentinel) |

A TValue is 16 bytes:
- `uint32 tt` at +0. The type is in the low 4 bits.
- The value is at +8.

Type codes:

| tt | type |
|---|---|
| 0 | nil |
| 1 | boolean |
| 2 | lightuserdata |
| 3 | number (float) |
| 4 | string |
| 5 | table |
| 6 | Lua function |
| 7 | userdata |
| 8 | thread |
| 9 | ifunction |
| 10 | cfunction |
| 11 | ui64 |
| 12 | struct |

For a string, the value is an `HksString*`. The characters start at +0x14, and the length is
`*(u64*)(s+8) & 0x3FFFFFFFFFFFFFFF`.

Several API functions are inline in the HKS headers, and the game has no out-of-line copy of all of them. Implement
them in C++ exactly as the game does:
- `gettop(L) = (L->top - L->base) / 16`
- `settop(L,n)`:
  - n >= 0: write `tt=0` into the slots up to `base+n`, then set `top = base+n`.
  - n < 0: `top += (n+1)*16`.
- `pushnumber(L,f)`: `top->tt = 3; *(float*)(top+8) = f; top += 16`. This is exactly what
  C_GameGfxEnvEffModule::GetTime (0x2D4D780) does.
- `pushnil`: `top->tt = 0; top += 16`.
- `pushboolean`: `tt = 1`, `*(int*)(top+8) = b`.
- Before pushing more than one slot, check `top + n*16 <= *(L+0x58)`. Callbacks from the game always have at least
  20 free slots (LUA_MINSTACK), so a couple of pushes are safe.

### 1.3 Lua API functions (RVA, signature, how found)

Every `hksi_*` function contains its own name string in its parameter-assert block (`lea r8, "hksi_..."`). To find
one again after an update:
1. Find the string.
2. Take its xref.
3. Take the start of the enclosing function from `.pdata`.

| RVA | function | signature (x64 MS ABI) | found via |
|---|---|---|---|
| 0x491CAB0 | luaL_loadbuffer | `int (lua_State*, const char* buf, size_t sz, const char* chunkname)` | "hksi_luaL_loadbuffer". Forwards to hksL_loadbuffer with the `global+0x558` compiler settings. [confirmed] |
| 0x491DF90 | hksL_loadbuffer | `int (lua_State*, const HksCompilerSettings*, const char* buf, size_t sz, const char* name)` | "hksi_hksL_loadbuffer" [confirmed] |
| 0x491CBE0 | luaL_loadstring | `int (lua_State*, const char*)` | "hksi_luaL_loadstring" |
| 0x49124F0 | lua_load | `int (lua_State*, lua_Reader, void* ud, const char* chunkname)` | "hksi_lua_load" |
| 0x4912960 | lua_pcall | `int (lua_State*, int nargs, int nresults, int errfunc)` | "hksi_lua_pcall" and "Insufficient arguments on stack for lua_pcall" [confirmed] |
| 0x4911AB0 | lua_call | `void (lua_State*, int nargs, int nresults)` | "hksi_lua_call" (160 callers) |
| 0x4911E70 | lua_cpcall | `int (lua_State*, lua_CFunction, void* ud)` | "hksi_lua_cpcall" |
| 0x4913C50 | hks_newstate | `lua_State* (const HksStateSettings*)` | "hksi_hks_newstate". Called by the game's C_ScriptMachine::Init. [confirmed] |
| 0x4911640 | lua_newstate | `lua_State* (lua_Alloc, void* ud)` | "lua_newstate"/"Main State". **Not used by the game** (0 callers). |
| 0x4912590 | lua_newthread | `lua_State* (lua_State*)` | "hksi_lua_newthread" [confirmed] |
| 0x49131D0 | lua_resume | `int (lua_State*, int narg)` | "hksi_lua_resume" |
| 0x49134D0 | lua_yield | `int (lua_State*, int nresults)` | "hksi_lua_yield" |
| 0x4914710 | hks_error (core of lua_error) | `void (lua_State*, int status)`, does not return | "hksi_hks_error" |
| 0x49136E0 | hks_pushnamedcclosure | `void (lua_State*, lua_CFunction, int nup, const char* name, int)` | "hksi_hks_pushnamedcclosure". To register a global, the game calls it as `(L, fn, 0, "Name", 0)` and then lua_setfield(L,-10002,"Name") (0x1AB8A16). [confirmed] |
| 0x4914A30 | hks_pushcclosure | `void (lua_State*, lua_CFunction, int nup, int)` | "hksi_hks_pushcclosure" |
| 0x0E4DA50 | lua_pushlstring | `void (lua_State*, const char*, size_t)` | 296 callers. The VM itself uses it to push error messages before hks_error (0x4933778), and every inlined getfield/setfield uses it. [confirmed] |
| 0x0E4DCD0 | lua_setfield | `void (lua_State*, int idx, const char* k)` | Pops the value. The game registers globals with idx = -10002 (0xFFFFD8EE). [confirmed] |
| 0x1254BC0 | lua_getfield | `void (lua_State*, int idx, const char* k)` | index2adr + pushlstring + hks_obj_getfield; 18 callers [confirmed] |
| 0x3D829D0 | lua_getglobal | `void (lua_State*, const char*)` | Contains both "hksi_lua_getglobal" and "hksi_lua_getfield" asserts. Reads the globals TValue at L+0x70. [confirmed] |
| 0x0E4DD90 | lua_settable | `void (lua_State*, int idx)` | index2adr + hks_obj_settable (0x49192B0) + pop 2 |
| 0x0E4DF10 | lua_tostring | `const char* (lua_State*, int idx)` | index2adr, then a tail-jump to hks_obj_tolstring with len=NULL; 882 callers [confirmed] |
| 0x0785BF0 | lua_tonumber | `float (lua_State*, int idx)`, returned in xmm0 | index2adr, then a tail-jump to hks_obj_tonumber (0x4917EC0); 1332 callers [confirmed] |
| 0x0785B30 | lua_isnumber | `int (lua_State*, int idx)` | Calls hks_obj_isnumber (0x4917C60); 1338 callers. It has no unique short pattern because its prologue matches lua_tonumber (COMDAT twin). |
| 0x1254DD0 | lua_isstring | `int (lua_State*, int idx)` | Tests for type 3 or 4; 822 callers |
| 0x4917FF0 | hks_obj_tolstring | `const char* (lua_State*, const TValue*, size_t* len)` | Contains the number-to-"%0.8g" conversion [confirmed] |
| 0x4917EC0 | hks_obj_tonumber | `float (lua_State*, const TValue*)` | Target of lua_tonumber |
| 0x4919010 | hks_obj_getfield | `TValue* (TValue* out, lua_State*, const TValue* t, const TValue* key)` | Used by getfield and getglobal; honours metamethods |
| 0x4918E20 | hks_obj_gettable | Same shape as hks_obj_getfield | 722 callers (table/struct fast paths) |
| 0x49192B0 | hks_obj_settable | `void (lua_State*, const TValue* t, const TValue* k, const TValue* v)` | Used by setfield and settable |
| 0x4911FE0 | lua_createtable | `void (lua_State*, int narr, int nrec)` | "hksi_lua_createtable" |
| 0x49117B0 | lua_pushfstring | `const char* (lua_State*, const char* fmt, ...)` | "lua_pushfstring" |
| 0x4913420 | lua_typename | `const char* (lua_State*, int)` | "hksi_lua_typename" (thunk at 0x4911910) |
| 0x491C0A0 | luaL_error | `int (lua_State*, const char* fmt, ...)` | "hksi_luaL_error" |
| 0x491D450 | luaL_ref | `int (lua_State*, int t)` | "hksi_luaL_ref" |
| 0x491D950 | luaL_unref | `void (lua_State*, int t, int ref)` | "hksi_luaL_unref" |

The range 0x4911600..0x49119A0 is a block of `jmp` thunks into the VM. Examples:
- 0x49117A0 → lua_pcall
- 0x491CAA0 → luaL_loadbuffer
- 0x4911970 → hks_newstate
- 0x4911930 → hks_pushnamedcclosure

Game code calls these thunks. To catch every call, hook the real function, not the thunk.

### 1.4 Where the game keeps its main lua_State [confirmed]

There is exactly **one** game script machine: `C_ScriptGameMachine`, which derives from `C_ScriptMachine` and is a
singleton (`ue::C_Singleton<ue::game::script::C_ScriptGameMachine, ...ScriptGameMachineSingleton>`).
- The `C_ScriptMachine` ctor (0x3BFF250) has a single caller: the `C_ScriptGameMachine` ctor (0x1AA4BE0).
- 0x1AB89D0 creates it: `new` of 0x230 bytes, then the ctor, then the store to the global.
  - The same function registers the global C function `IsSceneObjectValid`.
  - It is called from the boot step "Script" (0x24104D0 → 0x241051C).

The global and the state pointer:
- **`g_ScriptGameMachine` = `*(C_ScriptGameMachine**)(base + 0x6315F10)`.**
  - To recover it after an update, scan for the `C_GameScriptModule_Tick` pattern
    `48 8B 0D ?? ?? ?? ?? 48 85 C9 74 07 48 8B 01 48 FF 60 20 C3`. The rip displacement is at +3 and the
    instruction ends at +7.
- **Main `lua_State*` = `*(lua_State**)(g_ScriptGameMachine + 0xD0)`.** Evidence:
  - C_ScriptMachine::Init (vtable slot 1, 0x3C08EA0) does `this->+0xD0 = hks_newstate(&settings)` and sets
    `L->+0xA0 = this`.
  - RunString and Tick both read `+0xD0`.
  - Vtable slot 17 (`+0x88`, 0x1AB62C0) is the getter `mov rax,[rcx+0xD0]; ret`, used before the game registers
    globals.
- Either pointer can be NULL before the boot step "Script" has run. Check both every time.

Vtables: `C_ScriptGameMachine` is at RVA 0x4D15818; the `C_ScriptMachine` base vtable is at 0x4FC6C08. Useful slots
(shared by both):

| slot | method | RVA |
|---|---|---|
| `+0x08` | Init(bool) | 0x3C08EA0 |
| `+0x20` | **Tick** | 0x3C092F0 |
| `+0x28` | TickPaused | 0x3C093A0 |
| `+0x78` | **RunString** | 0x3C03850 |
| `+0x88` | GetLuaState | 0x1AB62C0 |

`C_ScriptMachine::RunString(this, int must_be_-1, ue::C_String* src)` (0x3C03850) is literally:

```
luaL_loadbuffer(L, src, len, "MyScript");
ok = lua_pcall(L, 0, 0, 0) == 0;
```

The engine itself runs text Lua this way. Our bridge can therefore make the same two calls directly on the state at
`+0xD0`, with no need to build a `ue::C_String`.

Two further notes:
- Do not confuse the main state with the **second VM** that Havok Behavior uses (strings "hkbSelf",
  "havok_lua_path"; created via 0x37F2CB0 → hks_newstate). It is not the game-script state.
- Game-side `game.*` objects are C++ "adaptors". A C function gets its `self` through 0x3811340
  `(L, minArgs, flags)`, which raises "Missing adaptor arguments." if `self` is missing.

### 1.5 Where scripts are ticked each frame: the safe place to run our Lua [confirmed statically]

The engine has a tick manager:
- Singleton getter 0x179B390 returns `*(base+0x62E66D8)`.
- Vtable at 0x4CC5D48. Slot `+0x40` is RegisterCallback(eventType, priority, module, thunk, float, ..., "[TM]name").

`C_GameScriptModule` registers these callbacks (0x2D51BD0):

| event | name | handler RVA |
|---|---|---|
| 1 | `[TM]C_GameScriptModule::Init` | 0x2D63B70 |
| 2 | `[TM]C_GameScriptModule::Done` | 0x2D5CCB0 |
| **3** | **`[TM]C_GameScriptModule::Tick`** (priority 0x25E4) | **0x2D70880** |
| 4 | `[TM]C_GameScriptModule::TickPaused` | 0x2D71850 |
| 3 | `[TM]C_GameScriptModule::TickPrePhysics` | 0x2D718A0 |
| 3 | `[TM]C_GameScriptModule::TickPostPhysics` | 0x2D71870 |
| 3 | `...::StartAsyncGC` / `...::WaitAsyncGC` | 0x2D6F1E0 / 0x2D72B20 |
| 9 | `[TM]C_GameScriptModule::GameInit` | 0x2D5F170 |

`C_GameScriptModule::Tick` (0x2D70880) is the whole per-frame script update:

```
if (g_ScriptGameMachine) g_ScriptGameMachine->vfptr[+0x20](g_ScriptGameMachine, ctx);   // C_ScriptMachine::Tick 0x3C092F0
```

C_ScriptMachine::Tick runs `hks_tick` (thunk 0x4920F00 → "hksi_hks_tick" 0x49212E0) on `L`, then the script scheduler (0x3C08AA0, which calls lua_resume via thunk 0x49118F0), which resumes
script threads and coroutines.

**Runtime correction [confirmed in game, phase 3]:** the registered handler 0x2D70880 is **never called** in this
build. Hooked with MinHook (patch verified in memory), it got zero calls in the main menu and in the world while scripts
were clearly running. `C_ScriptMachine::Tick` (vtable slot +0x20, 0x3C092F0) is the only caller of `hks_tick` and of
the scheduler 0x3C08AA0 (checked with an xref scan), and it IS called every frame once the title screen has been
left (not on the title screen itself, not while the game is paused by focus loss, see 2.1).

**What the bridge does (src/game/lua.cpp):**
1. Hook `C_ScriptMachine::Tick` 0x3C092F0 and `C_ScriptMachine::TickPaused` 0x3C093A0 (patterns
   `C_ScriptMachine_Tick` / `C_ScriptMachine_TickPaused`). Both are shared by every `C_ScriptMachine`, so the detour
   only acts when `this == g_ScriptGameMachine`.
2. In the hook, call the original first.
3. Then drain our queued Lua work on `*(g_ScriptGameMachine+0xD0)`. `g_ScriptGameMachine` still comes from the
   `C_GameScriptModule_Tick` pattern (rip operand at +3).

This runs on the game's own script thread, at exactly the point where the engine itself runs Lua and outside any
running chunk, so lua_pcall and luaL_loadbuffer are safe there. Two rules:
- Always restore the stack afterwards (`settop` back to the top you entered with).
- Always use pcall. An unprotected error longjmps through game code.

The tick manager runs on the game logic thread. That is the thread that ran the boot step "Threads", which registers
itself as `GameMainThread` (0x240CA0A). [confirmed at runtime: the script tick thread id differs from the Present
(render) thread id in every log, e.g. `lua: first script tick on thread 37632` vs Present on 77144; it is stable for
the whole session]

Observed at runtime: the stack top/base at the hook are those of the main state outside any C frame; `luaL_loadbuffer`
+ `lua_pcall` with the top saved/restored (`(top-base)/16`) work every frame (thousands of calls per test, no leak). The
VM also accepts text chunks (not a secure state). `string.dump` works and returns HKS bytecode whose constant strings
reveal which methods a game script function calls; that is how most of docs/game-api.md was found.

---------------------------------------------------------------------------------------------------------------

## 2. Main loop / threads

**Boot sequence.** Boot is driven by a step table at RVA 0x5AD9238... Each entry is 0x28 bytes:
`{const char* name; init(); done(); ...}`. [confirmed] The steps, in order:
- CrashHandler, EngineHolder, Memory, GameContext
- **Threads** (0x2409E90)
- DebugFS, TraceServer, VFS, Profiler, Telemetry, StreamingInstall, AssertHandler
- **UserServicesCmdLineInit** (0x240DD50)
- **UserServices** (0x240DD60, the Steam init)
- Core, MainWindow, Engine, INetMsg, GameFramework, Init_MafiaFramework, SaveSystem, SysUserServices, TitleClient,
  Audio, HavokCommon, AnimProject, Physics, Sectors, SysODB, PreInitGameGuiModule
- **Script** (0x24104D0)
- SDSManagers, Others, Vehicle, Show, Static_Register_GameFramework, ContentParams

**Main-thread frame.** The application object has its vtable at 0x4B1EF50. [likely: identified by its message-pump
callees, not by a string]
- Init (0x5DEE30) does the mutex checks described in section 3.
- **`App_FrameTick` (0x5DEED0, vtable slot +0x98)** is the per-frame function. It:
  1. handles "realsuspend";
  2. runs the game tick (0x24193F0);
  3. pumps window events (0x5DFF50 → Win32 pump 0x5DF410). The pump uses PeekMessageA/TranslateMessage/
     DispatchMessageA, or GetMessageA while suspended.
- Hooking App_FrameTick is an alternative tick. Unlike the script tick, it also runs in menus and during loading.

**Rendering is on a separate thread.** [likely]
- The render module creates a thread named **"Render thread"**: 0x364A210 calls the thread manager (0x3C1B350)
  vfunc +0x18, CreateThread(name, runnable of 0x190 bytes, ..., stack 0x80000).
- The game logic thread names itself **"GameMainThread"** (0x240CA0A).
- Threads are created through the engine thread manager, using the CreateThread import and the wrappers
  "CreateThread"/"CreateIndependentThread". SetThreadDescription is not imported.
- So `IDXGISwapChain::Present` is almost certainly called on the Render thread, **not** on the thread that ticks
  scripts. I could not pin the Present call site statically, because D3D objects are only reached through vtables.
  To confirm at runtime, log the thread IDs in the Present hook and in the C_GameScriptModule::Tick hook.

**Consequence for ARCHITECTURE.md:** networking and UI can run in the Present hook if that is convenient. But **all
Lua and game-object access must be marshalled to the script tick hook**: use a single-producer queue from the render
thread that the tick hook drains.

**D3D imports.** DXGI and D3D11 are imported statically: `CreateDXGIFactory1`, and `D3D11CreateDevice` (called from
0x3B17D50). `D3D11CreateDeviceAndSwapChain` is not imported; the swap chain is created through the factory.

---------------------------------------------------------------------------------------------------------------

### 2.1 Focus loss pauses the simulation [confirmed]

The window class "Scotch" uses window procedure exe+0x5E0E00, which forwards to 0x5DF0D0 (`App_WndProc`). For
`WM_ACTIVATEAPP` (0x1C), `WM_SIZE` and `WM_WTSSESSION_CHANGE` it queues an engine event (0x6C30F0 / 0x5E0D30 /
0x6DDA40); the app handler (0x5DF350, event 0xB) stores "active" at `app+0xDF` and suspends/resumes the game
(0x17ACCB0 / 0x17ACC80). While inactive, neither `C_ScriptMachine::Tick` nor `TickPaused` runs (Present still does).
`WM_ACTIVATE`/`WM_KILLFOCUS` are not handled specially.

The dll subclasses the game window and swallows `WM_ACTIVATEAPP(FALSE)` (`background_run = 1`, default), so an
unfocused game keeps simulating and sending its player. Verified: two instances on one PC both tick scripts while
either one has the focus.

## 3. Steam and single-instance

### 3.1 Steam is not loaded from steam_api64.dll on disk: the exe memory-loads it from inside itself [confirmed]

**The embedded DLL.**
- The exe has **no import of steam_api64.dll** and never references that file name.
- Instead it contains an **embedded copy of steam_api64.dll**:
  - PE image at RVA 0x5B1A300, size 0x3A650 (239,184 bytes).
  - FileVersion 03.27.76.74, an old SDK from about 2015. `OriginalFilename` is `steam_api.dll`.
- The game loads it with its **own manual PE loader**, in the style of MemoryLoadLibrary:
  - 0x3DF16D0: load(image, size, 0, alloc)
  - 0x3DF17C0: the GetProcAddress equivalent
- The on-disk `steam_api64.dll` (05.69.73.98, SDK ~1.49, with ManualDispatch and ISteamNetworkingSockets) is only
  for the launcher and other tools. Inside the game process, `GetModuleHandle("steam_api64.dll")` returns NULL unless
  something else has loaded it.
- When: a CRT static initializer (0x1B9F20 → 0x3DF07B0) builds the import table and memory-loads the DLL **before
  WinMain**, so normally before our injected DLL runs.

**The function-pointer table.** Function pointers live in a table in `.data`, and the game makes every Steam call
through them.
- `g_SteamApiTable` = `*(entry**)(base + 0x64A0AA0)`. Entries are `{ const char* name; void** slot; }`, ending at
  the first entry with `slot == NULL`.
- To recover the table address, use the `Steam_MemLoadApi` pattern: the rip displacement is at +9 and the
  instruction ends at +0xD.
- The names are 0x40-byte strings at 0x5B19900:
  - `SteamAPI_Shutdown`, `SteamAPI_IsSteamRunning`, `SteamAPI_RestartAppIfNecessary`, `SteamAPI_WriteMiniDump`,
    `SteamAPI_SetMiniDumpComment`
  - `SteamClient`, `SteamAPI_Init`, `SteamUser`, `SteamFriends`, `SteamUtils`, `SteamMatchmaking`,
    `SteamUserStats`, `SteamApps`, `SteamNetworking`, `SteamMatchmakingServers`, `SteamRemoteStorage`,
    `SteamScreenshots`, `SteamHTTP`, `SteamUnifiedMessages`, `SteamController`, `SteamUGC`, `SteamAppList`,
    `SteamMusic`
  - `SteamAPI_RunCallbacks`, `SteamAPI_RegisterCallback`, `SteamAPI_UnregisterCallback`,
    `SteamAPI_RegisterCallResult`, `SteamAPI_UnregisterCallResult`
  - `Steam_RunCallbacks`, `Steam_RegisterInterfaceFuncs`, `Steam_GetHSteamUserCurrent`,
    `SteamAPI_GetSteamInstallPath`, `SteamAPI_GetHSteamPipe`, `SteamAPI_SetTryCatchCallbacks`, `GetHSteamPipe`,
    `GetHSteamUser`
- Slot RVAs in this build (look them up by name at runtime rather than hardcoding these):

| function | slot RVA |
|---|---|
| SteamAPI_Shutdown | 0x64A0A68 |
| SteamClient | 0x64A0968 |
| **SteamAPI_Init** | **0x64A0970** |
| SteamUser | 0x64A0978 |
| SteamFriends | 0x64A0980 |
| SteamUtils | 0x64A0988 |
| SteamController | 0x64A09D8 |
| **SteamAPI_RunCallbacks** | **0x64A0A00** |
| SteamAPI_RegisterCallback | 0x64A0A08 |
| SteamAPI_UnregisterCallback | 0x64A0A10 |

**SteamAPI_Init flow.** The boot step "UserServices" (0x240DD60) calls 0x3C4A6C0, which creates the platform object
(0xD0 bytes, global 0x6494470) and then calls `Steam_Init` (0x3C51070):
1. `UserServicesCmdLineInit` (0x240DD50) has already set the "use Steam" flag, `*(u8*)(base+0x64943B8) = 1`,
   unconditionally.
2. Init requires the memory-load to have succeeded (flag 0x64A0A90).
3. **0x3DF0970 → 0x3DF0AF0 runs an Authenticode check on steamclient64.dll.**
   - It reads `HKCU\Software\Valve\Steam\ActiveProcess\SteamClientDll64`.
   - It runs `WinVerifyTrust` (WINTRUST_ACTION_GENERIC_VERIFY_V2) on that file, using GetModuleHandleW("wintrust")
     and GetProcAddress.
   - Init fails if the signature is not valid. This only matters for a Steam emulator; real Steam passes.
4. It calls `SteamAPI_Init()`.
5. `SteamUser()->GetSteamID()` must return non-zero.
6. It calls `SteamClient()->SetWarningMessageHook(nop)`.

**SteamAPI_RestartAppIfNecessary is never called.** [confirmed]
- Its slot has zero code references.
- The app ID comes from the usual steam_api sources: the `SteamAppId` environment variable, `steam_appid.txt`, or a
  Steam launch. The embedded DLL contains the standard "SteamAppId" and "steam_appid.txt" strings.
- No app ID literal (1030840) exists in the exe.
- Our launcher setting `SteamAppId=1030840` is enough.

**SteamAPI_RunCallbacks** (slot 0x64A0A00) is referenced from three places:
- A `jmp [slot]` thunk (0x3C3D610). It is vtable slot `+0x48` of a platform-services object (vtable 0x4FCB148,
  static object 0x64944B0).
- Two Steam Controller input paths (0x3801320 and 0x3805B80). Each calls `SteamController()->Init()` and then
  RunCallbacks once.

I could not prove statically which loop or thread makes the vtable call each frame. [guess: main thread, once per
frame] The runtime check is trivial: the slot at 0x64A0A00 is plain data, so swap its pointer for a logger.

**No `SteamAPI_ManualDispatch_*`.** The embedded old steam_api does not export it at all. The game uses the classic
`SteamAPI_RegisterCallback` + `SteamAPI_RunCallbacks` scheme (its CCallback objects are built in 0x3C3FAA0).

**Implications for the MafiaCoop Steam transport.** ARCHITECTURE.md says to use the game's own steam_api64.dll via
GetProcAddress, and that does not work as written. Two options [design note, not verified]:
- (a) Use the game's in-memory functions from `g_SteamApiTable`.
  - This is the old SDK. ISteamNetworking P2P and ISteamMatchmaking lobbies are available through the
    `SteamNetworking()` and `SteamMatchmaking()` vtables.
  - ISteamNetworkingSockets/Messages are not available, and there is no flat API for them.
- (b) `LoadLibrary` the newer on-disk `steam_api64.dll` ourselves and call its `SteamAPI_Init`.
  - A second steam_api instance in the process gets its own pipe to the same steamclient64.dll. This works in
    practice.
  - Its callbacks must be pumped by us. ManualDispatch is available in that DLL.

### 3.2 Single-instance protection [confirmed]

- **`0x5DD890`** (called from App Init 0x5DEE30) does not block a second instance. It is leftover Mafia II code:
  - Unless the command line contains **`/multiinst`**, it calls
    `CreateMutexA(&sa, FALSE, "Mafia2 Launcher Super Mutex")`.
  - It reports "already running" **only if CreateMutexA returns NULL and GetLastError()==ERROR_ALREADY_EXISTS**.
  - CreateMutex never does that for an existing mutex; it returns a valid handle instead. So the check never fires.
- **`0x5DD930`** calls `OpenMutexA(SYNCHRONIZE, FALSE, "Mafia2 Installer Super Mutex")`.
  - If that mutex exists, it shows a MessageBoxA (title "Mafia II", text = string resource 0x68) and exits.
  - Only an installer would own that mutex.
- The other CreateMutexA calls (0x7330, 0x3CF4580, 0x4585340, 0x4751E40) create **unnamed** mutexes.
- `FindWindowA/W` and `CreateMutexW` are not imported. I found no window-class or window-title uniqueness check.
- **So the exe does not block two instances on one PC.**
  - For extra safety, our launcher can append `/multiinst` to the command line. The game honours it and it is
    harmless.
  - Steam itself may refuse a second "running" session of the same app for the same account. That is not an exe
    check.
  - Both instances share the same SteamID, as ARCHITECTURE.md already assumes.

---------------------------------------------------------------------------------------------------------------

## 4. Game singletons and Lua bindings

**Class registration.** Lua classes are registered with `Lua_RegisterClass` (0x3812B70):
`(const char* className, const char* parentName, const luaL_Reg* methods, int kind, ...)`.
- Method tables are `{const char* name; lua_CFunction fn;}` arrays in `.data`, terminated by `{0,0}`.
- I found them by scanning `.data` for (string pointer, `.text` pointer) pairs: 93 method tables, all dumped in the
  Appendix.
- Enum tables (`C_WrapE_*`) use the same layout and are left out of the Appendix.

**C_Game.** `C_Game* = *(*(void***)(base + 0x6361D70) + 1)`.
- The getter is 0x233B040: `mov rax,[rip+..]; mov rax,[rax+8]; ret`.
- To recover it, use the `Lua_Game_GetActivePlayer` pattern: the `E8 rel32` at +0x13 is the call to this getter.

**GetActivePlayer** (Lua `game.game:GetActivePlayer()`, C function 0x267D0D0, class `C_ScriptWrapperGame`)
[confirmed from code]:
1. `player = C_Game->vfptr[+0x90]()`. This is C_Game::GetActivePlayer; it returns the player entity or NULL.
2. `wrapper = 0x23E8A10(*(base+0x6367E68), *(u32*)(player+0x18) /*entity GUID/id*/, 0)`.
3. `0x1ABD8D0(L, wrapper)` pushes the script wrapper.

**Weather and time of day.** Lua `game.gfx:*` is `C_GameGfxEnvEffModule`; its methods table is at 0x5AF28C0.
- GetTime (0x2D4D780) calls the native function 0x2D62900(module), which returns the time as a float.
- SetTime: 0x2D4D7D0. SetWeatherSet: 0x2D4DBE0.
- Also: SetTimeFlowSpeed, GetTimeFlowSpeed, GetCurrentWeatherSetName, and more (see the Appendix).
- The `self` adaptor (0x3811340) yields the native module pointer.

**Traffic and spawning.** `C_ScriptWrapperTraffic` (table 0x5AE44A0): SwitchGenerators 0x2680100,
Populate 0x26807D0, and more.

**Entity creation and movement from Lua:**
- `C_ScriptWrapperGame::CreateCleanEntity` 0x267E770; also `CreateScriptActorAction` and `CreateBlocker`.
- Player teleport: `C_WrapperPlayer::Teleport` 0x23BBB80.
- Position: `C_WrapperEntityPos::GetPos` 0x23B4710 and `SetPos` 0x23B5B10.

**C_WrapperHuman::GetOnVehicle** (two `luaL_Reg` entries in table 0x5AD2560, both 0x23BBBD0) [confirmed from code]:
- 0x23BBBD0 dispatches on the argument count after `self`: 3 -> 0x23D18F0, 4 -> 0x23D1B00, 5 -> 0x23D1D40; anything
  else raises an argument error.
- 0x23D1D40 (5 args) reads: #2 vehicle wrapper (0x1ABD980), #3 number = seat (`cvttss2si`), #4 bool, #5 string
  looked up as a move-style name (0x6BF820, -1 = unknown), #6 bool. It checks the human wrapper (vfunc +0x28), the
  vehicle wrapper (vfunc +0x30, entity at +0x10, `[entity+0xA8]` non-null, `[entity+0x1C] & 1`), else returns a
  failed sync object; then calls 0x2B97A40(human AI at `[[human+0x10]+0x130]`, &syncObj, vehicle, seat, ...).
- 0x2B97A40 passes **`seat - 1`** to the command constructor 0x2B9BF30 (`lea edx, [rbp-1]`), with command type 1 for
  entity type 0x0A (car), 0x1D for 0x1B (boat). So the script seat index is 1-based (1 = driver); script seat 0
  becomes -1 and the command never completes. The bool #4 is passed inverted (`sete cl`).
- Practical use and the traps around it (fixed cars, cars the player drove): docs/game-api.md 7.

**C_PlayerModelManager** (the player's character) [confirmed in game 2026-10-15]: global pointer at exe+0x6353128.
- The default-slot setter 0x21E0960 (`PlayerModelManager_SetDefault` below) stores the profile hash at +0x30 and
  swaps the model only if it changed and the override slot (+0x38) is empty; +0x28 = player present, +0x48/+0x50 the
  sync objects it returns. The override setter (0x21E09B0) and clear (0x21E0FF0) have the same shape.
- The Lua thunks `C_WrapperPlayer::SwitchPlayerSpawnProfile` 0x2394BB0, `SwitchDefaultPlayerSpawnProfile`
  0x2395340 and `SwitchOverridePlayerSpawnProfile` 0x2395A00 each contain
  `4C 8B 44 24 40 48 8D 54 24 38 48 8B 0D <rel32> E8 <rel32> 48 8B 44 24 38 48 8D 54 24 48` (3 matches, so not in
  the pattern table): `mov rcx,[rip+mgr]` at +0xA (disp +0xD, end +0x11) and the `call` at +0x11 (rel32 +0x12, end
  +0x16). The dll takes the match whose call goes to the default setter and reads the global from it
  (`player_sync.cpp` FindModelManager).
- Read at runtime: in Free Ride the default slot held 335218123840277515 =
  `visuals/hero/game_main_tommy_cab_driver_flatcap` (the Free Ride outfit), the override slot 0 until we set one.
  That hash given to a puppet spawner (as a decimal string) shows the same clothes (docs/game-api.md 12).

**Simplest robust route for co-op:** drive all of these by **executing Lua text** in the main state. These are
exactly the calls the mission scripts make. For example:
`local p = game.game:GetActivePlayer(); local v = p:GetPos()`.

---------------------------------------------------------------------------------------------------------------

## 5. Menus: Scaleform GFx (AS3) movies [confirmed]

How this was found: exe strings (`/gui/main_menu.swf`, `_root.%s`, `ExternalInterface`, AS3 VM messages such as
"The method instance::ContextMenu::builtInItemsGet() is not implemented"), the ABC blocks of the loaded movies read
from process memory and later from our own load hook, disassembled with our AVM2 tool (scratch, not checked in), and
the code around the xrefs below. Runtime behaviour verified in game (docs/game-api.md 9).

**Movies.** Every menu/HUD screen is a Scaleform GFx movie `/gui/<name>.gfx`: an uncompressed SWF with the signature
`GFX` (version 21), one `DoABC` tag (code 82, flags 1 = lazy) holding ActionScript 3 built on Scaleform CLIK, plus
GFx tags 1000 (exporter info) / 1009 (external images). The header parser `LoadProcess::BeginSWFLoading`
(**0x475BB20**, pattern `GFx_LoadProcess_BeginSWF`) compares the first three bytes with `FWS`/`CWS`/`GFX`/`CFX`
(`cmp eax, 0x535746` ...) and logs "Loader read failed - file does not start with a SWF header". Signature:
`(this, File* in, a3, a4, a5 [stack], bool a6 [stack], ...)`: it reads at least six arguments, so a detour must
forward the stack arguments too (forwarding only four crashed at +0x120 on the garbage 5th argument).

**The file object** passed in is the engine's Scaleform `File` (vtable **0x4F6EAA8**): slot 0 dtor, 1 GetFilePath
(`/gui/main_menu_main.gfx`), 2 IsValid, 3 IsWritable, 4 Tell, 5 LTell, 6 GetLength (-1: a stream), 7 LGetLength, 8
GetErrorCode, 9 Write, 10 Read(buf, n), 11 SkipBytes, 12 BytesAvailable, 13 Flush, 14 Seek, 15 LSeek (forward only),
16 seek-to-0, 17 ChangeSize, 18 CopyFromStream, 19 Close; reference count at +8. Reading it to the end and passing an
object with the same virtual layout over a memory copy works (that is how we substitute patched movies).

**Main menu.** `/gui/main_menu.gfx` (class `main_menu` : `shell_base`) loads pages with UILoaders:
`main_menu_main.gfx` (class `pages::main_menu_page_main`), `main_menu_new_game.gfx`, `main_menu_dev.gfx`,
`menu_options.gfx`. The entries are not data: `main_menu_page_main.UpdateMenu()` (private) builds a
`Vector.<components::ButtonData>` per `menuLocation` (ROOT_MENU 0, PLAY_MENU 1, STORY_MENU 2, ... LOAD_GAME 11)
with ids from class constants (NEW_GAME 0, CONTINUE 1, DEV 2, OPTIONS 3, DLC 4, QUIT 5, EXTRAS 6, PLAY 7, STORY 8,
FREERIDE 9, MISSION_REPLAY 10, LOAD_GAME 11, ..., CARCYCLOPEDIA 14, ..., STORE 25 ...) and labels that are string
table ids (`$continue`, `$story_title`, `$freeride`, `$carcyclopedia`, `$options`, `$mafia_collection`, `$quit`; a
label without `$` is shown as is), then calls the public `BuildMenu(vec)` (one `menu_item2` per entry, mouse support
included). `ActivateButton(ButtonData)` switches on `id`; sub pages push `menuLocation` onto `PreviousMenus`, set
`menuLocation` and call `UpdateMenu()`; `Back()` pops it. An unknown `menuLocation` builds an empty vector (the
switch default), which is what our CO-OP page (location 50) uses.

**Pause menu.** `/gui/pause_menu.gfx` (class `pause_menu`) with pages MAP / COLLECTION / OPTIONS / GAME
(`pause_menu_map.gfx`, `menu_collectibles.gfx`, `menu_options.gfx`, `pause_menu_game.gfx`). Esc opens it on MAP; the
page keys are VK 187 `=` (MA_RB/MA_NEXT) and VK 189 `-` (MA_LB/MA_PREVIOUS); Tab (MA_START) closes it.
`pages::pause_menu_page_game` uses timeline clips `entries_mc.entry_0_mc .. entry_4_mc` collected in `Navigation`;
`SetupMenu()` fills `Entries[0..4]` (`InitDataEntry(id, name, desc, frameLabel)`), `PopulateNavigation()` copies
`Entries[i].name` into the clips, `Select()` switches on `current_CID`. On PC `CheckPlatformID()` (Xbox One) is false
and only four entries are used: `entry_4_mc` ("switch account") exists but is hidden. **`pause_menu.gfx` carries its
own copy of `pages::pause_menu_page_game`, and that copy is the one that runs** (the parent movie defines the class
first in the shared application domain), so both files must be patched.

**ActionScript -> C++.** Two mechanisms:
1. C++ objects exposed as `_root.game.<Name>` (`MainMenu`, `PauseMenu`, `Events`, `Messenger`, `GlobalGui`, ...),
   attached by AS through `ExternalInterface.call("AttachIface", name, int, int)`. Their methods are registered in
   tables such as 0x30323D0 (`C_MainMenu`: `CanContinueGame`, `StartNewGame`, `GetSaveList`, ...; entries 0x28 bytes
   `{vtable, owner, name, member fn, flags}`).
2. `ExternalInterface.call(name, args...)` goes through GFx 0x461AD00 to the movie's handler (`[movieImpl+0x238]`),
   the game's adapter 0x37BD1E0 (converts GFx values to engine values, calls the named callback, converts the result
   back and passes it to 0x477E0C0 [likely Movie::SetExternalInterfaceRetVal]), the per-movie name table 0x37D2630 and finally the
   callback object's slot 3 `Call(this, args, argc, ret, a5, view)`. The callbacks registered for every movie are
   `AttachIface`, `DetachIface`, `DestroyMyself` (0x185B090, via the movie view's vtable +0x258 "add callback").
   `AttachIface`'s `Call` is **0x1842D60** (pattern `Flash_AttachIface_Call`); it reads its 6th argument from the
   stack.
3. C++ -> AS: movie view vtable +0x250 `Invoke(path, Value* result, const Value* args, n)` with `"_root.%s"`
   (0x18152D5: what Lua `game.hud:SendMessageMovie(movie, fn, ...)` ends in).

**Engine Flash values** (`C_FlashASValue`, 0x18 bytes): +0 vtable 0x4F6ADC0, +8 data, +0x10 type (0 undefined, 1 null,
2 bool, 3 int, 4 uint, 5 number (double), 6 string, 7 wide string, 8 object, 9 array, 10 display object, 11 closure).
Strings/objects hold a ref-counted object at +8 whose own GFx value is at +0x20. Helpers:
- **0x37ADFC0** `Value(const char*)` (pattern `FlashValue_CtorString`): stores the **pointer** (an unmanaged
  Scaleform string; the text must outlive the value until it is converted, e.g. after a callback returned).
- **0x183EEB0** `const char* GetString(const Value*)` (`FlashValue_GetString`): type 6 only, else null.
- **0x37B6220** `Assign(dst, src)` (`FlashValue_Assign`), **0x37B31C0** dtor (`FlashValue_Dtor`), 0x37E4DA0
  SetType(v, type), 0x37ADF60/0x37ADF80/0x37ADFA0/0x37AE030 ctor int/uint/double/bool, 0x37AE010 default ctor.

**Input** (for blocking it under our panels): keyboard and mouse through DirectInput 8 (`DirectInput8Create`
import; `IDirectInputDevice8::GetDeviceState` slot 9 / `GetDeviceData` slot 10), gamepad through
`xinput9_1_0.dll!XInputGetState`, plus window messages (Scaleform). `GetKeyboardState`/`GetCursorPos` are imported
too. No raw input.

---------------------------------------------------------------------------------------------------------------

## 6. Chapter loading: the main menu's save loader [confirmed in game 2026-10-15]

Found from the `C_MainMenu` iface handlers (research-characters-missions.md B2) by following `OpenDebugLoadChapter2`
(handler 0x30201D0 -> 0x301FCC0, which joins the path array and calls the loader at 0x30200F5).

- **`OpenDebugLoadChapterString(this, C_String* name, bool a, bool b)` at 0x3020350** (pattern `ChapterLoad` below). It
  builds `"/saves/" + name`, creates a 1 MB memory buffer (0x3CB42A0: vtable, +8 capacity, +0x28 bytes used,
  +0x30 data), reads the file into it through the VFS (`[this+0x2C8]->vf40(...)` gives the file system, its
  `[+0x10]->vf78(out, path, buffer)` reads), wraps the bytes in a read stream and hands it to the save system
  (`[this+0x2C8]->vf88(saveObject, stream, path)`), then starts the game session (0x2327940 with a and b).
  - The read stream: `0x3C981D0(stream, data, size, 0)` (`[s+8]` = data, `[s+0x18]` = size, `[s+0x20]` = size == 0),
    called once from the loader at 0x302049C (return address 0x30204A1; pattern `ChapterLoad_StreamSite`, the call is
    at +0x24 of the match). MafiaCoop hooks the stream function and, only for that return address and only while a
    load of ours is armed, passes other bytes: the save we want loaded (no file is written anywhere).
  - Callers and their flags (a, b): `OpenDebugLoadChapter2` (Free Ride: `["02_lost_heaven", "lh_freeride_extreme.sav"]`)
    (0, 0); `MissionReplay` 0x3016800 (chapter select) (0, 1); `StartNewGame` 0x30173D0 (`01_start_game.sav`) (1, 0);
    0x305DD80 (`freeroam_gui_install.sav`) (0, 1). With (0, 1) `game.savesystem:IsInReplayOrTestDrive()` is true
    after the load; MafiaCoop turns its swapped loads into (0, 1).
- Loaded this way (any flags), a save comes in **without its quest running**: `game.director:QuestIsAvailable(q)`
  false, no mission flow, no objective, no mission NPCs or cutscene objects. `game.director:QuestSetCustomState(q,
  cp)` + `QuestSetAvailable(q)` (the game's debug `unlockquest`) then start the chapter's flow at that checkpoint.
  Starting a later checkpoint from the chapter's cp_010 save crashed: Omerta cp_030 from cp_010 ->
  `C_PlayerVehicleStreaming::ObtainNewPlayerVehicle` sprintf of `/sds/cars/%s.sds` with a null car name
  (exe+0x2374571 -> 0x61F800).
- Saves written in such a session go to the **Free Ride slot** (`gamesaves\defaultslot\save0..2.sav`; the story is
  `aslot`). Their save ids: `freeroam_quest_state_changed` (when a quest is made available), then
  `checkpoint_mm_<n>_<chapter>_cp_<nnn>` at checkpoints (no zero padding in the number: `checkpoint_mm_20_taxi_cp_010`;
  the shipped replay saves pad it: `checkpoint_mm_020_taxi_cp_010.sav`).
- Save files: format `GENR`, 30-50 KB; the save id is plain text inside (`checkpoint_mm_110_omerta_cp_020`,
  `freeroam_game_close`, `freeroam_before_quest_activated`).
- Solved 2026-10-15 (section 7): a guest's chapter load after `game.hud:QuitToMainMenu()` crashed with a null read at
  exe+0x2E81EF8 whenever the save loaded had another save id than the first save loaded through the chapter loader in
  that process (the loader's storage keeps one entry keyed by the first id). It looked random (2 of 5) because a
  reload of the same checkpoint the guest had first loaded worked.

## 7. The save system: load requests, slot storages, save files [confirmed in game 2026-10-15]

Found from the crash above (dump: message at `rbx`, its vtable 0x4EB3378; the handler logs `"Loading save (Id: "`).
Used by src/game/save_guard.cpp.

- **Load request** `Handle* LoadRequest(storageMgr, Handle* out, uint32 slot, C_String id, C_String storageName)` at
  0x2E78060 (pattern `SaveSys_LoadRequest`; two siblings of the same shape at 0x2E66890 / 0x2E66B80 allocate 0x38-byte
  messages and follow every load). `this->vf40(storageName)` gives the storage object; a 0x48-byte message (vtable
  0x4EB3378: +0x20 storage, +0x28 slot, +0x30 id) is queued and handled on GameMainThread by 0x2E81C80
  ("Loading save (Id: ...)"), which looks the id up with `storage->vf48(slot, id)` (0x2E74A80 -> `Find` 0x2E6E320:
  walks the storage's entry list for slot + id) and calls `vf58` on the result **without a null check** (0x2E81EF8).
  - Storage names seen: `aslot` (story; CONTINUE loads `slot 1, aslot`), `bslot` (Free Ride), `debugloadchapter` (the
    main menu's chapter loader, i.e. our in-memory loads, slot 1).
  - `debugloadchapter` holds **one** entry, created by the first chapter load of the process and keyed by that save's
    id; later chapter loads replace its data but not its key. Loading a save with another id: `Find` returns null ->
    crash. MafiaCoop's fix (save_guard `FindDetour`): while a load through `debugloadchapter` is under way (its request
    seen in the last 120 s), a failed lookup for that load's id in that storage returns the storage's entry (the one
    every chapter load uses). Checked: 9 forced reloads in a row in a lobby CHAPTER session (cp_010 first, then the
    host's cp_020 save each time; before the fix 2 of 2 crashed on the first), 5 + 3 + 3 in STORY sessions.
- **Save files** are written by a save thread through the game's file layer with **`CreateFileA`** (paths with mixed
  `/` and `\`: `...\Data\/<steamid>/gamesaves/default/bslot/save1.sav`, CREATE_ALWAYS); the profiles
  (`profiles/temporaryprofile/*.pf`) the same way from GameMainThread. In a replay-style session (our loads) the game
  writes `bslot` saves when a quest is made available (`freeroam_quest_state_changed`), at checkpoints and on
  `QuitToMainMenu` (`freeroam_game_close`). save_guard redirects every `CreateFileA` / `DeleteFileA` / `CopyFileExA` below
  `gamesaves` to `%TEMP%\MafiaCoop\coop-saves\<pid>\gamesaves\...` while a guest is in a co-op session (writes always,
  reads when the scratch file exists, so a Retry reads this session's checkpoint). Checked: a STORY session with 3
  forced reloads and a guest death: 10 save writes redirected, every file in the save folder byte-identical
  afterwards (only `Saves\videoconfig.cfg`, the window size, changed; `gamesettings.pf` was rewritten with the same
  bytes). The host of a lobby CHAPTER session is redirected too (mod 0.8.0; before that the chapter's `bslot` saves
  overwrote his Free Ride): checked with a CHAPTER 10: OMERTA session through the intro to cp_020 (4 host writes and 4
  guest writes redirected, the host's cp_010 and cp_020 checkpoints still found in his scratch tree and sent to the
  guest): all 18 save and profile files byte-identical afterwards (only `videoconfig.cfg` changed).
- **Slot profiles** (`profiles\temporaryprofile\aslot.pf`, `bslot.pf`; XML `C_SaveDataUnwrapped`): the slot's
  script data storage (hashed names: the slot's current quest `mm_110_omerta`, positions, a weapons table) plus
  difficulty settings. Written with `CreateFileA` at loads and quest starts by both games of a co-op chapter: after a
  CHAPTER 2: TAXI session with failed missions the story slot's `aslot.pf` said `mm_20_taxi` and 2 weapons instead of
  `mm_110_omerta` and 5. Earlier checks found it byte-identical only by chance (the Omerta chapter is the user's own
  story point, and the wrong-world load of the real story rewrote it). Since 0.9.1 save_guard redirects
  `aslot/bslot/cslot.pf` with the saves; checked byte-identical after a two-player Taxi session with a failed mission
  on each side.
- **Death or a failed mission in a replay-style load** [confirmed in game 2026-10-16]: leaving the mission area
  (Taxi cp_010, Omerta cp_020: "You abandoned the objective") makes the game load `freeroam_before_quest_activated`
  (slot 2, storage `aslot`) at once, with no fail screen: the free roam state from before the quest, i.e. the
  player's own story (`aslot\save1.sav`; here the user's Omerta). Two load requests 0.3 s apart. A normal story
  load (CONTINUE) shows the fail screen instead (Load Last Checkpoint / Restart Chapter / Return to Main Menu) and
  loads the story's checkpoint (`checkpoint_..., slot 1, aslot`).
  - The load handler (0x2E81C80): `entry = storage->vf48(slot, id)`; `entry->vf58()` (0x21F1F80 for these entries:
    `return [entry+0x18]`, the save data) non-null -> the request is done with that data, **no file read**; null ->
    a buffer is allocated and the storage's file layer reads the file (`[storage]->vf58()->vf48(out, slot, id,
    buffer)`). So the first such load in a process reads `aslot\save1.sav` with `CreateFileA` and later ones use the
    entry's data (checked: a different file content was ignored on the second load).
  - The story and Free Ride slot storages (vtables 0x4EB2F60 and 0x4EB3120) look entries up with 0x2E74C80
    (`Info* (this, uint32 slot, C_String* id)`, walks `[this+0x20]..[this+0x28]`: `[entry+8]` slot, `[entry+0x10]` id;
    pattern `SaveSys_SlotFind`); `debugloadchapter` (vtable 0x4EB2D78) uses 0x2E74A80 -> `Find` 0x2E6E320.
  - MafiaCoop (save_guard, 0.9.1), only while the save slots are blocked (a lobby CHAPTER host, every guest): a
    lookup of `freeroam_before_quest_activated` through 0x2E74C80 returns the `debugloadchapter` entry, whose data
    loads the Free Ride start (the carrier file of our loads; checked: position -944 -233, no quest, the same as a
    FREE RIDE start); the real entry is never used, so its data is never filled with ours either. As a second layer, a
    read of a real save file within 20 s of that request is served from `coop-saves\<pid>\session-checkpoint.dat`
    (the session's newest checkpoint; checked before the lookup hook existed: that load then brought up the Taxi
    checkpoint with its quest running). The story sync then leaves that world for the main menu once it is up
    (quitting while its loading screen waits for Enter hung the game on a black screen) and loads the checkpoint
    (docs/protocol.md "Story"). Checked with the user's real save read into the entry first (a failed replay load
    outside co-op): before the hook the co-op failure brought up his Omerta, after it the Free Ride start.
  - The `debugloadchapter` storage: the menu's own FREE RIDE load asks for its save (`freeroam_test`) in slot 2, our
    swapped loads in slot 1. After one of them the other found no entry and crashed at 0x2E81EF8 (a FREE RIDE start
    after a co-op chapter in the same process); `FindDetour` now hands out the storage's one entry for any slot.
- **A chapter-loader load ignores the save's player position** [confirmed in game 2026-10-16]: the player always
  starts at the Free Ride start (-943 -232, Salieri's bar), also with the flags (0, 0) and on the first load of a
  process. The position is in the save: the last block keyed `04 00 00 00 D1 05 B3 09 0C 00 00 00` (field hash
  0x09B305D1, 12 bytes: x y z) followed within 80 bytes by `4C 17 1B 4D 10 00 00 00` (quaternion); earlier blocks
  with the same key are cars. Chapter starts mostly begin at the bar anyway or their cp_010 setup moves the player;
  Omerta cp_020 does neither, so its reload started at the bar and its leave-the-area check never armed. MafiaCoop
  (0.10.0) reads that block (mission_sync.cpp `SaveSpotIn`) and puts the player there 3 s after the load, then starts
  the quest a second later. Checked: Omerta cp_020 host 5 + 3 (with a guest) failures, guest 2, Taxi host 2 + guest 2:
  each time at the save's spot, Omerta's check fired again from the reloaded checkpoint.
- **Failed mission in one load** (tried 2026-10-16, `fail_load = direct`, off by default): letting that
  `freeroam_before_quest_activated` load read the session's checkpoint itself (the slot entry's cached data at
  `[entry+0x18]` dropped so the file is read again, and that read served from `session-checkpoint.dat`) does give the
  checkpoint world in one load, but as a story load: its quest runs at once (before we can move the player), it
  failed again straight away (the player at the bar), showed the story fail screen, and its Load Last Checkpoint read
  the player's **real** story checkpoint (`aslot`) and crashed (exe+0x3024566). Dropping the data at every lookup of
  one load crashed sooner (exe+0x1A90AB6: the next lookup's user got null). So the Free Ride stop stays; its loading
  screen is confirmed for the player instead (a DirectInput Enter through core `ui_input::PressGameKey`, 3 s after
  the screen is up; window messages and the virtual pad do not confirm it, and the game reads DirectInput only while
  its window has focus) and the screen is kept black while that world is up.
- `game.savesystem:EnableSaveLogic(name)` takes a string (a named save logic), not a switch; `RequestCheckpointSave()`
  wrote nothing in a replay-style session.

---------------------------------------------------------------------------------------------------------------

## 8. The game-sectors list of humans: the exe+0x243A915 crash [confirmed in game 2026-10-16]

The crash that brain switches (`SwitchBrain`), released burning humans and humans released with a map marker all
ended in (docs/game-api.md 15) is one list:

- `C_GameSectorsModule::TickInGame` (exe+0x243A7B0, registered by its init exe+0x243A3B0 next to the string
  `[TM]C_GameSectorsModule::TickInGame`) calls exe+0x243A870 every frame: `this+0x70 += elapsedMs`; once the sum
  passes 1000 it walks a vector of entity pointers (`begin = this+0x58`, `end = this+0x60`) and, for each entity with
  `[entity+0xF0] == 0`, calls its virtual function at vtable +0x120 (exe+0x243A915). A freed entity in the vector =
  call through freed memory (`read of address 0xffffffffffffffff`).
- One component class (vtable around exe+0x4E97470) adds its human in one method (exe+0x2D12040 ->
  exe+0x24396F0, push_back on `this+0x58`) and removes it in another (exe+0x2D121A0 -> exe+0x243A140, swap with the
  last and shrink), both only when `[component+0x298] == 0`. No RTTI names in this build.
- Measured: a puppet given the FRIEND brain and then freed alive crashed the game 0.1-0.6 s later every time (spawner
  `ReleaseObjectRaw`, the factory's `DespawnEntityImmediately`, also after `Deactivate` or `SwitchBrain(0)` first);
  freed long after it had died it did not. A world change soon after such a puppet died also crashed a few seconds
  into the next world.
- MafiaCoop's guard (src/game/sectors_guard.cpp): a hook on exe+0x243A870 that, right before each walk, drops every
  pointer whose id (`+0x18`) no longer resolves to it in the entity table (the lookup behind `GetEntityByGUID`,
  docs/game-api.md 15). Nothing is removed until one entry has been seen live (layout check), and the hook goes in
  only when the walk's `mov rdi, [rbx+60h]; mov rbx, [rbx+58h]` is where expected. Checked: the alive friend-brain
  puppet freed with the factory, which crashed every time before, logged "dropped 1 stale human(s) ... (69 live)"
  and the game ran on; two more such drops in a 15-minute session with guest reloads, factory resets and a host death
  and reload, no crash.
- Half torn-down humans (2026-10-16): a host crashed 8 s into a failed mission's load (Omerta cp_020, the guest's
  friend-brain puppet in the old world) in the getter the walk calls (vtable +0x120 = exe+0x2B4C470: `mov rax,
  [rcx+348h]; mov rcx, [rax+298h]`): the entity still resolved in the entity table, but `[entity+0x348]` was null.
  The guard now also drops entries whose getter starts with exactly that load while `[entity+0x348]` (or its
  `+0x298`) is null. The same failure run 5 more times afterwards (3 host, 2 guest): no crash.

---------------------------------------------------------------------------------------------------------------

## Pattern table (all verified unique in .text of this build)

The name says what the pattern's first byte is: the start of that function, unless noted otherwise. To re-verify,
run `python re-tools/scan.py <exe> --file <file of "name<TAB>pattern" lines>`.

| name | RVA | pattern |
|---|---|---|
| hksi_luaL_loadbuffer | 0x491CAB0 | `48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 30 49 8B F9 49 8B F0 48 8B EA 48 8B D9 48 85 C9 75 3D E8 ?? ?? ?? ?? 48 8D 0D ?? ?? ?? ?? 41 B9 72 B4 6A` |
| hksi_hksL_loadbuffer | 0x491DF90 | `48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 48 89 7C 24 20 41 54 41 56 41 57 48 81 EC 50 01 00` |
| hksi_lua_pcall | 0x4912960 | `48 89 5C 24 18 55 56 57 41 54 41 55 41 56 41 57 48 83 EC 60 4C 63 FA 41` |
| hksi_lua_call | 0x4911AB0 | `48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 41 56 41 57 48 83 EC 30 48 63 F2 41` |
| hks_newstate | 0x4913C50 | `48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 48 89 7C 24 20 41 54 41 56 41 57 48 83 EC 30 48 83` |
| lua_newstate | 0x4911640 | `48 89 5C 24 08 57 48 81 EC C0 00 00 00 48 8B FA 48 8B D9 48 85 C9 75 3D` |
| GameSectors_Update | 0x243A870 | `48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 50 01 51 70 48 8B D9 E8 ?? ?? ?? ?? 48 8B F8 48 83 78 08 00 75 1D B9 08 00 00 00 E8 ?? ?? ?? ?? 48 85 C0` (section 8) |
| hksi_lua_newthread | 0x4912590 | `48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 41 56 41 57 48 83 EC 40 48 8B 79 10` |
| hksi_lua_resume | 0x49131D0 | `48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 48 89 7C 24 20 41 56 48 83 EC 30 48 63 F2 48 8B D9 48 85 C9 75 3D E8 ?? ?? ?? ?? 48 8D 0D ?? ?? ?? ?? 41 B9 8A 21 11` |
| hks_error | 0x4914710 | `48 89 5C 24 10 55 56 57 41 54 41 55 41 56 41 57 48 83 EC 30 44 8B F2 48` |
| hks_pushnamedcclosure | 0x49136E0 | `48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 30 49 8B F9 41 8B F0 48 8B` |
| hks_pushcclosure | 0x4914A30 | `48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 48 89 7C 24 20 41 54 41 56 41 57 48 83 EC 40 49 63` |
| ChapterLoad (OpenDebugLoadChapterString) | 0x3020350 | `48 89 5C 24 18 55 56 57 41 54 41 55 41 56 41 57 48 8D 6C 24 F0 48 81 EC 10 01 00 00 4C 8B F9 4C 8B F2 48 8B 89 C8 02 00 00` |
| ChapterLoad_StreamSite (in the loader; the stream call at +0x24) | 0x3020478 | `48 8D 4C 24 68 E8 ?? ?? ?? ?? 48 8D 4C 24 68 48 8B D8 E8 ?? ?? ?? ?? 48 8B D0 48 8D 4D A0 45 33 C9 4C 8B C3 E8 ?? ?? ?? ?? 49 8B 8F C8 02 00 00` |
| SaveSys_LoadRequest (section 7) | 0x2E78060 | `48 89 5C 24 10 48 89 6C 24 18 56 57 41 54 41 56 41 57 48 83 EC 20 48 8B 01 48 8B FA 48 8B 6C 24 70 49 8B F1 48 8B D5 45 8B F8 4C 8B F1 FF 50 40 B9 48 00 00 00` |
| SaveSys_Find (storage entry lookup, section 7) | 0x2E6E320 | `48 89 5C 24 10 48 89 6C 24 18 48 89 74 24 20 57 48 83 EC 30 49 8B 00 8B EA 48 8B F1 48 89 44 24 40 49 8B D0 48 8D 4C 24 40 49 8B F8 33 DB` |
| SaveSys_SlotFind (story / Free Ride slot storage lookup, section 7) | 0x2E74C80 | `48 89 5C 24 18 89 54 24 10 55 56 57 41 54 41 55 41 56 41 57 48 83 EC 20 33 ED 41 83 CF FF 89 6C 24 60 4D 8B E0 48 8B 59 20 4C 8B E9 48 3B 59 28` |
| Lua_GetEntityByGUID (thunk; its calls at +0x39/+0x46/+0x61/+0x69/+0x89 give entity manager, lookup by id, the wrapper steps and the push; `mc_entity`) | 0x239E510 | `48 89 5C 24 08 48 89 74 24 20 57 48 83 EC 20 BA 01 00 00 00 48 8B F1 44 8B C2 E8 ?? ?? ?? ?? 33 FF 4C 8D 44 24 38 48 8B CE 89 7C 24 38 8D 57 02 E8 ?? ?? ?? ?? 8B 5C 24 38 E8` |
| lua_pushlstring | 0xE4DA50 | `48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 40 BF 1F 00 00` |
| lua_setfield | 0xE4DCD0 | `48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 20 48 8B 41 48` |
| lua_getfield | 0x1254BC0 | `40 53 48 83 EC 60 0F 29 74 24 50 4D 8B C8 48 8B D9 81 FA F0` |
| lua_getglobal | 0x3D829D0 | `48 8B C4 48 89 58 18 57 48 83 EC 70 0F 29 70 E8 48 8B FA 48` |
| lua_settable | 0xE4DD90 | `40 53 48 83 EC 20 48 8B D9 81 FA F0 D8 FF FF 7E 1F 85 D2 48` |
| hks_obj_tolstring | 0x4917FF0 | `48 8B C4 53 56 41 54 41 55 48 81 EC 38 01 00 00 48 8B 59 10` |
| hks_obj_getfield | 0x4919010 | `48 8B C4 48 89 58 08 48 89 70 10 48 89 78 18 55 41 54 41 55 41 56 41 57 48 8D 68 A1 48 81 EC C0` |
| hks_obj_gettable | 0x4918E20 | `48 8B C4 48 89 58 08 48 89 70 10 48 89 78 18 55 41 54 41 55 41 56 41 57 48 8D 68 A1 48 81 EC A0` |
| hksi_lua_createtable | 0x4911FE0 | `48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 41 56 41 57 48 83 EC 40 41 8B F0 8B` |
| hksi_lua_pushfstring | 0x49117B0 | `48 89 54 24 10 4C 89 44 24 18 4C 89 4C 24 20 53 55 56 57 48 83 EC 38 48 8B F9 48 85 C9 75 3D E8 ?? ?? ?? ?? 48 8D 0D ?? ?? ?? ?? 41 B9 76 A3 01 1A 48 89 4C 24 28 4C 8D 05 ?? ?? ?? ?? 48 8D 0D ?? ?? ?? ?? BA A4 04 00 00` |
| C_ScriptMachine_Init | 0x3C08EA0 | `4C 89 44 24 18 55 53 56 41 54 41 55 41 56 41 57 48 8D 6C 24` |
| C_ScriptMachine_Tick | 0x3C092F0 | `40 53 48 83 EC 20 48 83 B9 D0 00 00 00 00 48 8B D9 74 28 80` |
| C_ScriptMachine_RunString | 0x3C03850 | `48 89 5C 24 08 57 48 83 EC 20 49 8B D8 48 8B F9 83 FA FF 0F` |
| C_ScriptMachine_ctor | 0x3BFF250 | `48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 20 33 ED 48 8D 05 ?? ?? ?? ?? 48 89 01 48 8B F9 48 89 69` |
| C_ScriptGameMachine_ctor | 0x1AA4BE0 | `48 89 5C 24 08 57 48 83 EC 20 33 D2 48 8B D9 E8 ?? ?? ?? ?? 33 FF 48 8D 05 ?? ?? ?? ?? 48 89 03 48 89` |
| ScriptGameMachine_Create | 0x1AB89D0 | `40 53 48 83 EC 30 B9 30 02 00 00 E8 ?? ?? ?? ?? 48 85 C0 74` |
| C_GameScriptModule_Tick | 0x2D70880 | `48 8B 0D ?? ?? ?? ?? 48 85 C9 74 07 48 8B 01 48 FF 60 20 C3` |
| C_GameScriptModule_TickPaused | 0x2D71850 | `48 8B 0D ?? ?? ?? ?? 48 85 C9 74 07 48 8B 01 48 FF 60 28 C3` |
| C_ScriptMachine_TickPaused | 0x3C093A0 | `40 53 48 83 EC 20 48 83 B9 D0 00 00 00 00 48 8B D9 74 2D 80` |
| C_GameScriptModule_Register | 0x2D51BD0 | `40 53 48 83 EC 60 0F 29 74 24 50 E8 ?? ?? ?? ?? F3 0F 10 35 ?? ?? ?? ?? 48 8D 0D ?? ?? ?? ?? 48 89 4C 24 40 4C 8D 0D ?? ?? ?? ?? 33 DB 48 8D 0D ?? ?? ?? ?? 4C 8B 10 41 B8 A0 28 00 00` |
| Steam_Init | 0x3C51070 | `48 83 EC 28 80 3D ?? ?? ?? ?? 00 C6 05 ?? ?? ?? ?? 01 74 64` |
| Steam_MemLoadApi | 0x3DF0880 | `40 56 48 83 EC 20 48 8B 05 ?? ?? ?? ?? 48 8D 0D ?? ?? ?? ?? 48` |
| Steam_VerifySteamclient | 0x3DF0AF0 | `40 55 48 8D AC 24 40 FC FF FF 48 81 EC C0 04 00 00 E8 ?? ?? ?? ?? 84` |
| Steam_ApiTableCtor | 0x3DF04D0 | `48 89 5C 24 10 55 48 8D AC 24 90 FE FF FF 48 81 EC 70 02 00` |
| MemoryLoadLibrary | 0x3DF16D0 | `48 83 EC 38 48 89 4C 24 20 48 8D 0D ?? ?? ?? ?? 48 89 54 24` |
| MemoryGetProcAddress | 0x3DF17C0 | `4C 8B C2 48 8B 51 10 8B 49 28 E9 ?? ?? ?? ?? CC 3B 0D ?? ?? ?? ?? 7D` |
| SingleInstanceCheck | 0x5DD890 | `48 81 EC 48 04 00 00 FF 15 ?? ?? ?? ?? 41 B8 00 04 00 00 48` |
| InstallerMutexCheck | 0x5DD930 | `40 53 48 81 EC 20 04 00 00 48 8B D9 4C 8D 05 ?? ?? ?? ?? B9` |
| App_Init | 0x5DEE30 | `40 53 48 83 EC 20 48 8B D9 FF 15 ?? ?? ?? ?? 48 8B 4B 10 48` |
| App_FrameTick | 0x5DEED0 | `48 89 5C 24 18 55 48 83 EC 20 48 8B E9 48 8B 0D ?? ?? ?? ?? E8` |
| App_PumpEvents | 0x5DFF50 | `48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 48 89 7C 24 20 41 54 41 56 41 57 48 83 EC 20 48 8B E9 41 B7 01` |
| Win32_MessagePump | 0x5DF410 | `40 57 48 81 EC 80 00 00 00 48 8B F9 0F 1F 40 00 45 33 C9 48` |
| RenderModule_CreateRenderThread | 0x364A210 | `48 89 74 24 20 57 48 81 EC D0 00 00 00 48 8B F2 48 8B F9 48` |
| lua_tostring | 0xE4DF10 | `4C 8B C9 81 FA F0 D8 FF FF 7E 37 85 D2 7E 1D 48 63 D2 48 C1` |
| lua_tonumber | 0x785BF0 | `4C 8B C1 81 FA F0 D8 FF FF 7E 35 85 D2 7E 1A 48 63 D2 48 C1` |
| lua_isstring | 0x1254DD0 | `4C 8B C9 81 FA F0 D8 FF FF 7E 48 85 D2 7E 2A 4C 8B 41 50 49 83 C0 F0 48 63 D2 48 C1 E2 04 4C 03 C2 4C 3B 41 48 73 29 41 8B 08 33 C0 83 E1 0F 83 E9 03 83 F9` |
| hks_obj_isnumber | 0x4917C60 | `48 89 5C 24 10 48 89 6C 24 18 56 57 41 56 48 83 EC 20 48 8B 59 10 4C 8B` |
| hks_obj_tonumber | 0x4917EC0 | `48 89 5C 24 10 48 89 6C 24 18 48 89 74 24 20 57 48 83 EC 30 48 8B 59 10 4C 8B DA 8B` |
| Lua_Game_GetActivePlayer | 0x267D0D0 | `40 53 48 83 EC 20 45 33 C0 33 D2 48 8B D9 E8 ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 8B C8 48 8B 10 FF 92 90` |
| Lua_GetSelfAdaptor | 0x3811340 | `48 89 5C 24 08 57 48 83 EC 20 48 8B 41 48 48 8B D9 48 2B 41` |
| Lua_RegisterClass | 0x3812B70 | `48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 48 89 7C 24 20 41 56 48 81 EC 90 00 00 00 41 8B F9` |
| Lua_EnvEff_SetWeatherSet | 0x2D4DBE0 | `48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 40 BA 03 00 00 00 0F 29 74 24` |
| Lua_PushScriptWrapper | 0x1ABD8D0 | `48 89 5C 24 08 57 48 83 EC 20 48 8B DA 48 8B F9 48 85 D2 74 6F 48 8B 02` |
| PlayerModelManager_SetDefault | 0x21E0960 | `40 53 48 83 EC 20 48 83 79 28 00 48 8B DA 75 06 48 8B 41 50 EB 26 48 8B 41 30 4C 89 41 30 4C 3B C0` |
| WrapperFactory_GetByGuid | 0x23E8A10 | `40 53 48 83 EC 20 8B DA E8 ?? ?? ?? ?? 48 8B C8 45 33 C0 8B D3 E8 ?? ?? ?? ?? 48` |
| GFx_LoadProcess_BeginSWF | 0x475BB20 | `48 89 5C 24 20 55 56 57 41 54 41 57 48 83 EC 40 48 8B 02 48 8B F1 48 8B CA 4D 8B F9 49 8B E8 48 8B DA FF 50 20` |
| Flash_AttachIface_Call | 0x1842D60 | `48 89 54 24 10 48 89 4C 24 08 53 48 83 EC 60 48 8B 9C 24 98 00 00 00 4C 89 64 24 48 45 8B E0` |
| FlashValue_CtorString | 0x37ADFC0 | `48 89 5C 24 08 57 48 83 EC 20 48 8B DA C7 41 10 00 00 00 00 48 8D 05 ?? ?? ?? ?? BA 06 00 00 00` |
| FlashValue_GetString | 0x183EEB0 | `40 57 48 83 EC 20 83 79 10 06 75 4B 48 8B 79 08 48 85 FF 74 42` |
| FlashValue_Assign | 0x37B6220 | `48 89 5C 24 08 57 48 83 EC 20 48 8B DA 48 8B F9 33 D2 E8 ?? ?? ?? ?? 48 8B 43 08 48 89 47 08 8B 43 10 89 47 10` |
| FlashValue_Dtor | 0x37B31C0 | `40 53 48 83 EC 20 48 8D 05 ?? ?? ?? ?? 33 D2 48 89 01 48 8B D9 E8 ?? ?? ?? ?? 48 8D 05 ?? ?? ?? ?? 48 89 03 48 83 C4 20` |
| RayCast (PhysicsPointBelow) | 0x23E83F0 | `48 89 5C 24 08 48 89 7C 24 10 55 48 8D 6C 24 A9 48 81 EC E0 00 00 00 F2 41 0F 10 00 48 8D 05 ?? ?? ?? ?? F2 0F 11 45 EF 49 8B F8 F3 0F 10 4D F3`: query vtable lea at +0x1C, filter 0x404DA386 at +0x55, physics getter call +0x9B (world = [ret+0x58]), cast call +0xAC (exe+0x1A68E50), result release call +0x1C0 |

Rip/call operand offsets used above: `C_GameScriptModule_Tick` +3 (disp) / +7 (end) gives g_ScriptGameMachine; `Steam_MemLoadApi` +9/+0xD gives g_SteamApiTable; `Steam_Init` +0x2F/+0x33 gives the SteamAPI_Init slot, +0x39/+0x3D SteamUser slot, +0x60/+0x64 SteamClient slot; `Lua_Game_GetActivePlayer` E8 rel32 at +0x14 gives the C_Game getter; `ScriptGameMachine_Create` mov [rip+disp],rax at +0x2B (disp +0x2E, end +0x32) also gives g_ScriptGameMachine.

---------------------------------------------------------------------------------------------------------------

## Appendix: Lua class method tables (from luaL_Reg arrays in .data)

Format: `class` : `parent` (table RVA, method count): methods. `(unnamed)` tables are registered differently (e.g. HKS base libs or nested tables). Metamethods (`__gc`, `__eq`, ...) omitted.

- `C_AICommand_SyncObjectUserData` : `C_SyncObjectUserData` (table RVA 0x5A940B0, 4 methods): IsSuccess, IsFail, IsDone, Terminate
- `C_AIControlMessage` : `C_EntityMessage` (table RVA 0x5AEDB90, 3 methods): GetSelectorName, GetObjectName, GetReason
- `C_ActorActionBusyMessage` : `C_EntityMessage` (table RVA 0x5AD15C0, 3 methods): GetText, GetActionType, GetActionIndex
- `C_ActorActionHeldMessage` : `C_EntityMessage` (table RVA 0x5AD1700, 3 methods): GetHoldTimeRemaining, GetActionType, GetActionIndex
- `C_ActorActionUsableMessage` : `C_EntityMessage` (table RVA 0x5AD1560, 3 methods): GetText, GetActionType, GetActionIndex
- `C_CaptureObjectModuleScriptWrapper` (table RVA 0x5AF45D0, 4 methods): RegisterObjective, RegisterWalkieTalkieCallback, UnregisterObjective, AddCaptureItem
- `C_CarHearSoundMessage` : `C_EntityMessage` (table RVA 0x5ADCA30, 4 methods): GetSoundOwnerGUID, GetSoundPos, GetSoundType, GetSecondEntityGUID
- `C_DamageMessage` : `C_EntityMessage` (table RVA 0x5AECAD0, 6 methods): GetEnemyGUID, GetWeaponID, GetDamageType, GetDamageAmount, GetHitZone, GetCanHeadShot
- `C_EntityGuid` (table RVA 0x5AB6D90, 1 methods): IsValid
- `C_EntityMessage` (table RVA 0x5AB6CF0, 7 methods): GetType, GetMessage, HasName, GetName, GetSenderGUID, GetReceiverGUID, Clone
- `C_EntityMessageVehicleDamaged` : `C_EntityMessage` (table RVA 0x5AC1930, 5 methods): GetDamageType, GetDamageIncoming, GetDamageFinal, GetAttackerGUID, GetLocalPos
- `C_GameCameraMafia` (table RVA 0x5AF4090, 42 methods): ScriptLock, ScriptLockTarget, ScriptLockTargetOffset, ScriptUnlock, ScriptUnlockResetDir, ScriptModeInstant, ScriptModeConstant, ScriptModeAccelerated, ScriptPointAtEnt, ScriptPointAtVec, ScriptLookAtEntity, ScriptLookAtEntityDir, ScriptLookAtPos, ScriptLookAtDir, ScriptLookAtCancelable, ScriptLookAtStop, ScriptResetCamBehindPlayer, ScriptLookAtGetSyncObjects, ScriptForcedFOVStart, ScriptForcedFOVStop, ScriptForcedFOVGetSyncObjects, ScriptForcedOffsetStart, ScriptForcedOffsetStop, ScriptForcedOffsetGetSyncObjects, OverrideInteriorExteriorCamera, SetInteriorExteriorCamera, SetInteriorImmediateCamera, SetShakeOverrideParams, SetShakeOverrideSineWaveEasyParams, SetShakeOverrideEnabled, ScriptShakeOverrideGetSyncObjects, SetVehicleCameraMode, SetAnimatedBoneCameraMode, SetRailCameraMode, MarkLowerBoundRailCameraMode, ModifyInputRailCameraMode, RegisterConfigNameOverrideForMode, UnregisterConfigNameOverrideForMode, RegisterAimWeaponNameOverride, UnregisterAimWeaponNameOverride, GetPos, GetDir
- `C_GameDirectorScriptWrapper` (table RVA 0x5AD0780, 72 methods): IsIdValid, GameOver, CityGetActiveName, CityTransfer, QuestSetAvailable, QuestSetLocked, QuestSetActive, QuestSetInactive, QuestSetFinished, QuestSetInstanceFinished, QuestRegularSetActive, QuestRegularSetFinished, QuestRegularIsActive, QuestRegularIsLoaded, QuestPersistentSetFinished, QuestIsAvailable, QuestIsActive, QuestWasFinished, QuestWasFinishedWithOutcome, QuestGetLastFinishedOutcomeId, QuestGetTimeSinceLastFinish, QuestGetCustomState, QuestSetCustomState, QuestGetCustomStatesCount, QuestGetCustomStateByIndex, QuestIsInActiveCity, QuestGetCityName, QuestIsDefined, QuestDebugForceAvailable, QuestNoteActivate, QuestNoteDeactivate, QuestNoteIsActive, EnableActivitySpawner, DisableActivitySpawner, IsActivitySpawnerEnabled, SetActivitySpawnerInstancesCount, GetActivitySpawnerInstancesCount, CreateCustomRepelRadius, DeleteCustomRepelRadius, ResetEnabledActivitySpawners, DebugRespawnActivityInstance, GetSpawnPointsSelectedForActivity, GetSpawnPointsWithLoadedInstances, RegisterActivitySpawnPointCallback, RegisterDistrictChangeCallback, UnregisterDistrictChangeCallback, GetDistrict, UnregisterActivitySpawnPointCallback, GetSpawnPointObject, GetSpawnPointPosition, TestSpawnPointTag, DoesSpawnPointExist, IsSpawnPointSelected, ApplySpawnPointActivityCooldown, ApplySpawnPointGlobalCooldown, IsSpawnPointActivityCooldownActive, IsSpawnPointGlobalCooldownActive, ToggleSpawnSystemDebugDisplay, ObjectiveActivate, ObjectiveDeactivate, ObjectiveIsActive, ObjectiveIconShow, ObjectiveIconHide, ObjectiveCreateGPSGroup, ObjectiveAddToGPSGroup, ObjectiveRemoveFromGPSGroup, ObjectiveSetGPSGroupStatus, ObjectiveDestroyGPSGroup, GetCurrentAct, SetCurrentAct, GetCurrentSubAct, SetCurrentSubAct
- `C_GameGfxEnvEffModule` (table RVA 0x5AF28C0, 25 methods): GetTime, SetTime, SetTimeFlowSpeed, GetTimeFlowSpeed, SetDryUpSpeedMult, GetDryUpSpeedMult, SetTimeLimit, RemoveTimeLimit, SetWeatherSet, SetSkyboxRotationAngle, GetCurrentWeatherSetName, IsWeatherCondition, RegisterWeatherConditionChanged, RegisterWeatherTimeFrameEvent, UnregisterWeatherCallback, SetPostprocessSetOverride, ResetPostprocessSetOverride, CreateRestriction, DeleteRestriction, SetMaterialTexture, SetMaterialParameter, GetMaterialParameter, EnablePostProcess, IsPostProcessEnabled, UseDepthTestForObjectHighlightMask
- `C_GameGuiScriptW` (table RVA 0x5AF9370, 5 methods): OpenQuestionDialog, GetQuestionDialogResult, CloseQuestionDialog, IsTutorialModeEnabled, OpenMap
- `C_GarageManager` (table RVA 0x5AE09B0, 22 methods): GetPlayersVehicle, GetNumGaragePlaces, GetFreeGaragePlaces, AddGaragePlaces, HasVehicle, RemoveVehicle, RemoveVehicleByName, CheatAddVehicle, CheatAddCarWithTuning, IsGarageEmpty, AddVehicle, ClearAllGarages, CheatAddVehicles, OpenGarageMenu, SpawnGarageVehicle, LeaveGarage, OpenCarcyclopediaMenu, SpawnCarcyclopediaVehicle, UnlockCarcyclopediaVehicleByName, UnlockCarcyclopediaVehiclesBySeason, UnlockCarcyclopediaVehiclesAll, LockCarcyclopediaVehicleByName
- `C_HearSoundMessage` : `C_EntityMessage` (table RVA 0x5AEF290, 4 methods): GetSoundOwnerGUID, GetSoundPos, GetSoundType, GetSecondEntityGUID
- `C_HudScriptW` (table RVA 0x5AF7B50, 237 methods): UpdateSimpleObjective, UpdateRolloverDateAndImage, AddSecondaryObjective, UpdateSecondaryObjective, UpdateSecondaryObjectiveIcon, FinishSecondaryObjective, RemoveSecondaryObjective, GetControllerPreset, ShowTitleCard, HideTitleCard, FadeHudElement, GetInteractCameraDummy, Show, RadarShow, SubtitlesShow, SpeedometerShow, ActionButtonsShow, MessageAreaShow, InfoAreaShow, InventoryShow, BackMirrorShow, TemperatureGaugeShow, TailingGaugeShow, DamageGaugeShow, ObjectiveTimerShow, IsObjectiveGaugeShowing, SetTemperatureGaugeValue, SetTailingGaugeValue, SetDamageGaugeValue, SetTimerValue, CenterIndicatorsShow, ShowObjectInBackMirror, FinishShowingObjectInBackMirror, ObjectiveShow, MissionHUDShow, ShowArea, HideArea, DarkPlaceIndicatorShow, MessageShowQuick, HelpHintShowQuick, DebugHint, HelpHintHide, MoneyShow, RespectShow, FaderFadeOut, FaderFadeOutMute, FaderFadeOutExt, FaderFadeIn, FaderFadeOutContext, FaderFadeInContext, IsFadingIn, IsFadedIn, IsFadingOut, IsFadedOut, QuitToMainMenu, PoliceWantedSet, PoliceRelationSet, TimerShow, TimerSet, TimerStart, TimerStop, TimerIsRunning, TimerGetDiffCoef, TimerGetTime, EffectAlcoholHit, EffectAlcoholDrunk, EffectAlcoholLimits, ShowLowHealthFX, ForceSubtitlesUp, SetPauseMapMenuEnabled, CreateHint, DestroyHint, ShowHint, HideHint, ShowDebugInformationEvent, ShowWarning, ShowWarningStringId, HideWarning, QueueHintEvent, OpenMovie, AttachMovie, CloseMovie, SetMovieFlag, SetMovieFadeInDistances, SetMovieFadeOutDistances, SendMessageMovie, RegisterScriptedMenuEvent, CreateHTMLText, CreateHealthBar, AcceptMissionRestart, UnregisterScriptedMenuEvent, AttachDebugOnEntity, RemoveDebugFromEntity, ChangeTextOnEntity, EnableDebugCoords, AreDebugCoordsEnabled, SetAbortMissionStringId, AddEntityIndicator, RemoveEntityIndicator, AddEntityIndicatorBone, RemoveEntityIndicatorBone, SetEntityIndicatorOffscreenType, SetEntityIndicatorOcclusion, SetProgressEntityIndicator, SetProgressTargetSpeedEntityIndicator, SetProgressSpeedEntityIndicator, GetProgressEntityIndicator, SetProgressEntityIndicatorBone, SetMinDistanceEntityIndicator, SetMinDistanceEntityIndicatorBone, SetTextEntityIndicator, EnterServiceTargeting, ExitServiceTargeting, AddServiceTargetingEntity, RemoveServiceTargetingEntity, RegisterWiretapZone, IsPointInAnyWiretapZone, OpenCustomizeCarMenu, ClearDistrictLegend, SetTitleDescriptionDistrictLegend, AddRowDistrictLegend, ShowDistrictLegend, SetUIXMargin, SetUIYMargin, GetScreenAspectRatio, GetScreenResolution, RacingStart, RacingCheckpoint, RacingLapFinished, RacingStop, RacingGetTime, RacingShowHud, RacingVisualPause, RacingClear, SetRacingLapsAndCheckpoints, ShowVehicleDamageTimer, HideVehicleDamageTimer, ShowVehicleDamageTimerDestructionBar, SetVehicleDamageTimerTime, SetVehicleDamageTimerDestruction, SetRadarFlashState, ToggleSaveLoadSpinner, IsBarUpgradeUnlocked, IsBarUpgradePurchased, IsBarCategoryUnlocked, UnlockBarUpgrade, UnlockBarCategory, OpenBarCustomizationMenu, CloseBarCustomizationMenu, ShowBarCustomizationLoadingScreen, HideBarCustomizationLoadingScreen, SetBarCustomizationMenuInputDisabled, RequestBarMenuFadein, SetDistrictCollectibleCount, StartCountDown, ShowMissionOptIn, HideMissionOptIn, ShowMissionExit, ShowMissionExitSync, HideMissionExit, ShowMissionRestart, HideMissionRestart, ShowMissionComplete, ShowMissionCompleteScotch, HideMissionComplete, ShowRacketOptIn, HideRacketOptIn, ShowRacketOptOut, HideRacketOptOut, ShowMissionExitWithCancel, HideMissionExitWithCancel, ShowFinalSitdownChoice, UpdateService, UpdateDeliverableVehicle, SetVehicleDeliveryCategoryIndex, AddQuestChapter, UpdateQuestChapter, RemoveQuestChapter, AddQuest, UpdateQuest, AddQuestNavId, RemoveQuest, SetQuestCount, SetQuestType, AddQuestObjective, AddQuestObjectiveWithoutNotification, UpdateQuestObjective, RemoveQuestObjective, SetQuestObjectiveState, SetQuestObjectiveCount, AddQuestObjectiveNavId, SetQuestObjectiveOptions, SetQuestObjectiveShowAllSubs, AddQuestSubObjective, UpdateQuestSubObjective, RemoveQuestSubObjective, SetQuestSubObjectiveState, AddQuestSubObjectiveNavId, SetQuestSubObjectiveOptions, IsQuestObjectiveSelected, IsQuestSubObjectiveSelected, SelectQuestObjective, SelectQuestSubObjective, AddJournalEntry, OpenTutorial, OpenScriptedTutorial, OpenScotchTutorial, CloseTutorial, SetOnlyScriptedTutorialsEnabled, GetTutorialVisibility, IsTutorialDisplayed, SetCustomRadarRange, ClearCustomRadarRange, SetNumRelayComponents, IsLoadingScreenUp, CanShowTutorial, ShowQuestStartBanner, EnableAlertBannerForTrespassing, SetForceIgnoreInventoryCollapse, ShowPlayerRaceTimeDifference, ShowRaceMessageWithTime, StartCredits, StartCreditsSync, ShowRaceExitScreen, HideRaceExitScreen, SetRacingOverlayMap, ClearRacingOverlayMap, ShowRaceStartScreen, HideRaceStartScreen, ShowRaceFinishedScreen, HideRaceFinishedScreen, GetLocalizedString, BuildParamString, SetShouldFireRaceExit, ShowUncustomizableCarErrorDialog, GetSelectedObjective, GetLastSelectedObjective
- `C_HumanMessageDeath` : `C_EntityMessage` (table RVA 0x5AED290, 6 methods): GetKillerGUID, GetDamageType, GetWeaponID, GetHitZone, WasStunned, WasReloading
- `C_MafiaSaveSystemScriptWrapper` (table RVA 0x5AF5810, 15 methods): EnableSaveLogic, SetRespawnPosDir, ClearRespawnPosDir, SetRespawnDefaultSpawnProfile, SetRespawnOverrideSpawnProfile, SetRespawnTrafficSeason, ClearRespawnTrafficSeason, IsInReplayOrTestDrive, RequestFreeroamSave, RequestCheckpointSave, RequestMissionSave, DebugSave, IsRetryPossible, Retry, FreeRoamRetry
- `C_MathLuaModule` (table RVA 0x5B057F0, 5 methods): newVector, newMatrix, newQuat, newTransform, Random
- `C_NavigationScriptW` (table RVA 0x5AF9720, 88 methods): RegisterVehicleEntity, RegisterVehicleDirect, RegisterPassionActivityPos, RegisterPassionActivityEntity, RegisterPassionActivityDirect, RegisterHighDamageObjectivePos, RegisterHighDamageObjectiveEntity, RegisterHighDamageObjectiveDirect, RegisterHideoutContactObjectivePos, RegisterHideoutContactObjectiveEntity, RegisterHideoutContactObjectiveDirect, SetHideoutContactNotifyState, RegisterStashHousePos, RegisterStashHouseEntity, RegisterStashHouseDirect, SetStashHouseState, RegisterCoveragePos, RegisterCoverageEntity, RegisterCoverageDirect, SetCoverageState, RegisterOWAHideoutPos, RegisterOWAHideoutEntity, RegisterOWAHideoutDirect, RegisterOWAPersistentCharacterPos, RegisterOWAPersistentCharacterEntity, RegisterOWAPersistentCharacterDirect, RegisterDistrictPos, SetDistrictState, AddDistrictActivity, SetDistrictActivityState, RegisterHideoutPos, RegisterHideoutEntity, RegisterHideoutEntityDirect, SetHideoutState, AddHideoutIntel, SetHideoutIntelState, SetHideoutStartingValue, SetHideoutCurrentValue, SetHideoutEarnValues, SetHideoutIsInLockedState, SetHideoutShouldShowMapIcons, SetIndicatorHideRange, RegisterPersistentCharacterPos, RegisterPersistentCharacterEntity, RegisterPersistentCharacterDirect, SetPersistentCharacterState, SetPersistentCharacterKickback, SetPersistentCharacterEarn, RegisterObjectivePos, RegisterObjectiveEntityDirect, RegisterObjective, RegisterObjectiveIconPos, RegisterObjectiveIconEntityDirect, RegisterObjectiveIcon, RegisterHideoutObjective, RegisterHumanEntity, RegisterHumanEntityDirect, SetNotifyState, SetIconRiskRewardDamage, SetDifficultyState, RegisterIconPos, RegisterIconEntity, RegisterIconEntityDirect, RegisterArea, RegisterCircle, RegisterCircleRGB, RegisterCircleEntity, RegisterCircleEntityRGB, SetIconFarVisibility, UnregisterId, GetIdByEntityName, GetIdByScriptWrapper, SetPopupProperties, SetDamageValueHundreds, SetIconTextByEntity, SetIconTextByNavId, SetIconShowByEntity, SetIconMapVisibilityByEntity, SetIconRadarVisibilityByEntity, SetIconMapVisibilityByNavId, SetIconRadarVisibilityByNavId, SetIconGPSEnabledByNavId, LoadCustomMap, UnloadCustomMap, RequestRadarMap, UnrequestRadarMap, EnableGPSCustomPath, DisableGPSCustomPath
- `C_NodeScriptCommandWrapper` (table RVA 0x5AC4500, 9 methods): ActivateOutputPort, RegisterForTick, UnregisterForTick, RegisterForEvents, UnregisterForEvents, DebugGetParentSequenceName, DebugGetParentSequenceOwner, DebugGetGUID, GetRuntimeGUID
- `C_OutfitDBScriptWrapper` (table RVA 0x5AD1220, 6 methods): UnlockOutfit, SetCurrentOutfit, GetCurrentOutfit, IsLocked, FreeRideStart, UnlockAll
- `C_PersistentCharactersManagerScriptWrapper` (table RVA 0x5AD06A0, 11 methods): GetState, SetState, DebugSetLocation, GetLoyalty, DebugSetLoyalty, DebugIncreaseLoyalty, DebugDecreaseLoyalty, SetBackstory, SetStatus, EnableQuestMemberMode, DisableQuestMemberMode
- `C_PoliceForceWrapper` (table RVA 0x5AF3280, 47 methods): ZoneCreate, ZoneQuery, ZoneDelete, PreventZonesDecay, PreventPoliceSpawning, DespawnRoadBlock, ZoneDecay, ZoneDecayRed, ZoneDeleteAll, ZoneDecayAll, Enable, Disable, SetPersistentPoliceGlobalDensityModifier, OverridePersistentPoliceSet, RemoveOverridePersistentPoliceSet, PersistentPolicePreventSpawningFromSource, PersistentPoliceStopSpawningFromSource, PersistentPoliceStartSpawningFromSource, PersistentPolicePreventSpawningFromPos, PersistentPoliceStopSpawningFromPos, PersistentPoliceStartSpawningFromPos, UnregisterForPoliceEvent, RegisterForPoliceEvent, GetActiveZones, GetZoneInfo, SpawnRoadBlockWithCrew, DisableOffencesGlobal, DisableOffencesTemp, DisableOffencesGlobalAllowPolice, DisableOffencesDistrict, DisableOffencesDistrictPermanently, DisableOffencesDistrictAllowPolice, AreOffencesDisabled, RequestDispatchSpeech, SetDispatchSpeechPreset2D, ClampEscalationLevel, ForceMinEscalationLevel, PreventDispatchSpeech, SquealerCallPolice, SquealerCallSquealers, StartSquealerDespawnProtection, RemoveSquealerDespawnProtection, RemoveAllSquealersDespawnProtection, SquealerGetLastOffencePos, DisablePoliceSquealers, RegisterPreventCivilSquealerPhonesZone, UnregisterPreventCivilSquealerPhonesZone
- `C_RaceManagerScriptWrapper` (table RVA 0x5AF5D20, 14 methods): SetPath, SetCheckPoints, GetDistances, CrossedCheckPoint, RegisterEntity, UnregisterEntity, IsEntityRegistered, GetPosition, GetPositionTotal, GetDistance, SetIsRacing, SetTeleportAllowed, RegisterScriptEvent, UnregisterScriptEvent
- `C_RelationShipsScriptWrapper` (table RVA 0x5AEA810, 11 methods): SetRelationship, SetPlayerAggressivity, CreateGangGroup, DestroyGangGroup, AddGroupMember, RemoveGroupMember, IsPlayerAttacked, IsPlayerAttackedRanged, IgnorePlayer, GroupForgetHuman, GroupForgetAll
- `C_RuntimeDoorComponent` (table RVA 0x5ACF860, 15 methods): RegisterOnKickedCallback, UnregisterOnKickedCallback, RegisterCallback, UnregisterCallback, IsOpen, IsClosed, Open, Close, Lock, Unlock, EnableForPlayer, DisableForPlayer, EnableForNPCs, DisableForNPCs, DisableLockPulling
- `C_SafeHouseDoorCallbackManager` (table RVA 0x5ACF790, 4 methods): RegisterEnterCallback, RegisterExitCallback, UnregisterEnterCallback, UnregisterExitCallback
- `C_ScriptDataStorage` (table RVA 0x5AD1670, 6 methods): SetVariable, GetVariable, DelVariable, GetVariableType, VariableExists, Dump
- `C_ScriptGameMachineWrapper` : `C_ScriptMachineWrapper` (table RVA 0x5AC00B0, 9 methods): GetScriptGUID, ComposeAnd, ComposeOr, ComposeNeg, SetEvent, UnregisterFromAllMessages, CreateSyncObject, print, clear
- `C_ScriptMachineWrapper` (table RVA 0x5B109E0, 10 methods): callScript, createScript, markScriptForAutodestroy, require, getRegistry, CreateThread, CreateIndependentThread, DestroyThread, ResumeThread, GetTime
- `C_ScriptWrapperBattle` (table RVA 0x5AE3320, 14 methods): ResetBattleRoleAssignConfig, SetArchetypeCountLimit, RegisterHERDGroupingNavBox, UnregisterHERDGroupingNavBox, RegisterHERDGroupingArea, UnregisterHERDGroupingArea, RegisterHERDGroupingScope, UnregisterHERDGroupingScope, RegisterGlobalTrespassZone, UnregisterGlobalTrespassZone, SendEnemiesIntoBattleSearch, SetTrespassingState, SetTargetPriority, ResetTargetPriorities
- `C_ScriptWrapperGame` (table RVA 0x5AE3B20, 126 methods): GetActivePlayer, GetMissionTime, GetDLCNamesByType, IsDLCFeatureEnabled, EnableDLCFeature, CreateScriptActorAction, CreateScriptActorActionBox, CreateCleanEntity, CreateBlocker, AddTranslocatorExclusionZone, RemoveTranslocatorExclusionZone, SetRenderPerformancePreset, GetDifficultyStreak, GetIs3DMenu, AddTranslocatorInclusionZone, RemoveTranslocatorInclusionZone, AddTranslocatorFreezeZone, RemoveTranslocatorFreezeZone, WaitForTranslocator, SetDisabledActorActions, AddActorActions, ShowLeaderboard, ModifyGameTime, IsModifiedGameTime, StartCounter, StopCounter, GetCurrentCounter, GetLoadedSaveId, GetLoadedSaveType, DbgActionIsEnabled, DbgActionToogle, IsAchievementTrackingEnabled, AwardMedal, AwardDLCMedal, CountStatisticItem, GetRestrictedZoneManager, DispatchWorldEdgeGator, AllowTransientGatorSpawn, RequestGameUpHold, CancelGameUpHoldRequest, SetCurrentStoryId, LodingFinishedNotify, DbgToolActorInfo, IsDeadHumanInside, MassActivate, MassDeactivate, UpdatePresenceParam, UpdatePresenceMessage, IsPointInView, ProjectPointToScreen, GetPlatform, SkipLoadingSavingIndicatorDelay, PlayCutscene, SendTelemetryEvent, NewAIObjectParams, NewBattleFakeTarget, NewBattleDestination, GetPlayersEnemies, RegisterBlanketEvent, UnregisterBlanketEvent, EnableNewWater, SetWaterTemplate, SetWaterTransition, SetWaterVisibility, SetWaterSimulation, EnableAutoAimHumans, EnableAutoAimCrashObjects, BroadcastHearScriptEvent, StopBroadcastSeeScriptEvent, StartBroadcastSeeScriptEvent, StartBroadcastTimedSeeScriptEvent, StopBroadcastTimedSeeScriptEvent, DirectlyReportScriptEvent, OverrideMaxEventReactionDistFromPlayer, StopOverrideMaxEventReactionDistFromPlayer, DebugSetEditableVar, ToHashName, GetHashNameString, PostAIEvent, GotoPositionWithLookAt, TurnOffTheLights, ForceDifferentVoices, GetItemIDFromName, GetCorpseCountAroundPos, MoveAgentsToVolume, ZombieShotStart, ZombieShotAtEntityStart, ZombieShotStop, PlayRumbleEffect, StopListeningToGERS, GetCurrentGERS, StartListeningToGERS, RegisterEnterTrespassZoneCallback, UnregisterEnterTrespassZoneCallback, GetGlobalGameObjects, GetCurrentGameDifficulty, GetPlayerCanSkipRides, GetPlayerManualShifting, PlayMultipointSpeech, SwitchEnemyAlwaysOnRadarMode, SwitchPoliceAlwaysOnRadarMode, AddWeaponEffect, HerbalismGetUpgradeLevel, HerbalismGrowhouseIsUnlocked, HerbalismGrowhouseUnlock, HerbalismGetLevel, HerbalismSetLevel, HerbalismLevelUpAvailable, HerbalismGrowhouseGetProgress, HerbalismGrowhouseGetPrice, HerbalismGrowhouseGetMaxProfit, HerbalismGrowhouseGetMinProfit, HerbalismGrowhouseGetStrainName, HerbalismGetLastBatchSaleAmount, StopWeaponEffect, ClearWeaponsInRadius, ThrowProjectileWeapon, AddSoundPerceptionVolume, RemoveSoundPerceptionVolume, GetWeaponNameFromID, GetAmmoCategory, GetIsMeleeWeapon, LoadSds, UnloadSds, StartStopSlowMotionEffects, StringTablesHaveString
- `C_ScriptWrapperGameAudioAK` (table RVA 0x5AE3450, 5 methods): PlaySimpleEvent, SetHideoutState, SetMissionState, CreateAudioArgs, Play
- `C_ScriptWrapperPiPCamera` (table RVA 0x5AE43C0, 6 methods): DestroyPiPCamera, CreatePiPCamera, ActivatePiPCamera, DeactivatePiPCamera, CreatePiPCameraStatic, CreatePiPCameraDynamic
- `C_ScriptWrapperRadio` (table RVA 0x5AE4330, 6 methods): SelectStation, StartTransition, SetOn, GetStation, IsOn, NumAvailableStations
- `C_ScriptWrapperSDS` (table RVA 0x5AE49F0, 3 methods): GetSyncObjectForLoadSDS, GetSyncObjectForUnloadSDS, IsSDSLoaded
- `C_ScriptWrapperTraffic` (table RVA 0x5AE44A0, 24 methods): SwitchGenerators, SetEnableAmbientTrafficSpawning, Populate, GetIsAmbientTrafficOn, GetGlobalCarDensityMultiplier, OpenSeason, CloseSeason, GetCurrentSeasonID, SwitchRoad, SwitchAllowRadarZoom, UnspawnPlayersCars, GetClosestPointOnRoad, GetLastPlayerCar, GetLastPlayerVehicle, ObtainNewPlayerCar, GetLastObtainedPlayerCar, ActivateLastPlayersCar, RestoreCarCustomizationState, SetForceCarRealPhysics, UnlockCarCustomizationItem, TestPerfStats, TestCustStart, TestCustApply, TestCustRevert
- `C_SquealerSelectionModule` (table RVA 0x5AF46A0, 4 methods): GetSquealerDestinationTarget, GetImportantGameObjects, OnGangsterSquealerAssignedDestination, DisablePhoneBoothsEvaluation
- `C_StreamMapScriptWrapper` (table RVA 0x5AD0FF0, 4 methods): Resume, GetSyncObjectForSuspend, ChangeScenario, IsTestScene
- `C_StreamingWrapper` (table RVA 0x5ACF800, 3 methods): RequestLoadCollisionsInRadius, RequestUnloadAllButBoat, UnrequestLoad
- `C_VideoCaptureModule` (table RVA 0x5AF4640, 3 methods): StartHighlight, StopHighlight, CancelHighlight
- `C_WaitObject` (table RVA 0x5AC0320, 3 methods): WakeMe, IsDone, Dump
- `C_WingmanManagerWrapper` (table RVA 0x5AEAA10, 5 methods): RegisterWingman, UnregisterWingman, GetWingmenCount, GetWingman, AllowTeleportationIntoCar
- `C_WrapperAIObjectParams` : `C_ScriptWrapper` (table RVA 0x5AD6A20, 5 methods): AddBool, AddFloat, AddInt, AddU32, AddHashName
- `C_WrapperActor` : `C_WrapperEntityPos` (table RVA 0x5AD8300, 7 methods): GetAction, GetActionByType, GetActionsCount, OverrideMaterialParam, GetOwner, GetOwnerSceneObject, OverrideMaterial
- `C_WrapperActorAction` : `C_ScriptWrapper` (table RVA 0x5AD7620, 28 methods): GetActionType, SetTextID, SetTextHash, GetOwner, Enable, Disable, SetAutouse, SetLockRange, IsValid, SetControl, SetInVehicleAction, GetInVehicleAction, SetInCoverAction, GetInCoverAction, SetInStealthAction, GetInStealthAction, SetInDraggingAction, GetInDraggingAction, GetAiPos, GetAiDir, EnableActionHintSuppression, DisableActionHintSuppression, SetActionBusy, ClearActionBusy, SetAdvertiseRadius, SetCollisionsTest, SetCollisionsTestPivot, RequestStateUpdate
- `C_WrapperActorActionScript` : `C_WrapperActorAction` (table RVA 0x5AD6190, 2 methods): SetEventName, GetEventName
- `C_WrapperActorDeform` : `C_WrapperActor` (table RVA 0x5AD6F00, 16 methods): Restore, RegisterEvents, PausePhysics, IsPausedPhysics, SetFullResistance, GetFullResistance, SetDynamicOff, IsDynamicOff, GetPartByName, GetPartEnergy, SetPartEnergy, GetPartVersion, SetPartHitMarkers, ApplyActivationPower, ApplyActivationImpulse, SetImpulse
- `C_WrapperActorDetector` : `C_WrapperActor` (table RVA 0x5AD8D80, 5 methods): Register, Unregister, GetNumIn, GetActor, IsActorRegistered
- `C_WrapperActorDetectorStatic` (table RVA 0x5AD83F0, 6 methods): RegisterEventDistanceEnt, RegisterEventDistancePos, UnregisterEventDistance, RegisterDistanceDetectionEntity, RegisterDistanceDetectionPos, UnregisterDistanceDetection
- `C_WrapperAttribute` (table RVA 0x5ABFEC0, 10 methods): GetInt, GetBool, GetFloat, GetString, GetHash, SetInt, SetBool, SetFloat, SetString, GetNextAttribute
- `C_WrapperBoat` : `C_WrapperActor` (table RVA 0x5AD64F0, 46 methods): CreateController, DestroyController, InitializeAIParams, SetNavModeStand, SetNavModeWander, SetNavModeMoveTo, SetNavModeHunt, SetNavModeFollowTrail, SetRubberBanding, SetRubberBandingOn, SetRubberBandingOff, GetVehicleModelName, GetSpeedLimiter, SetSpeedLimiter, SetSpeedLimiterSpeed, GetSpeedLimiterSpeed, SetPowerLimit, GetPowerLimit, Halt, SetSpeed, GetSpeed, GetSpeedFloat, SetPower, SetMotorResistance, GetMotorResistance, GetMotorDamage, SetMotorDamage, GetDamage, EnableRadio, TurnRadioOn, Explode, SetLightsOn, SetSearchlightOn, SetSearchlightTargetPos, SetSearchlightTargetObject, SetBeaconLightOn, SetSirenOn, SetUseScriptEvents, LockPlayerEntryPoints, UnlockPlayerEntryPoints, FindSeatForHuman, GetPassengers, WaitForEntity, AttachCargo, DetachCargo, DetachAllCargo
- `C_WrapperCar` : `C_WrapperActor` (table RVA 0x5AD4CB0, 234 methods): SetSpeed, GetSpeed, GetSpeedFloat, GetSpeedLimiter, SetSpeedLimiter, SetSpeedLimiterSpeed, GetSpeedLimiterSpeed, SetPowerLimit, GetPowerLimit, SetMaxAISpeed, GetDamage, GetMotorDamage, SetMotorDamage, SetMotorBadTune, Repair, RepairAndClear, GetRepairPrice, SetCarDamage, GetBrakeDamage, SetBrakeDamage, GetHandbrakeDamage, SetHandbrakeDamage, HasVehicleFlag, HasVehicleFlags, HasVehicleLookFlag, HasVehicleLookFlags, HasVehicleFunctionFlag, HasVehicleFunctionFlags, IsAllowedTuning, IsInstalledTuning, InstallTuningItems, InstallTuningPackage, GetVehicleModelName, GetRaceClass, GetPerfRating, GetPrice, AddSpendedMoney, Lock, Unlock, LockEntryPoints, UnlockEntryPoints, LockPlayerEntryPoints, UnlockPlayerEntryPoints, LockPlayerEntryPointForSeatID, UnlockPlayerEntryPointForSeatID, FakePlayerCar, GetSeatCount, GetSeatOwner, IsDelivered, SetDelivered, OpenHood, CloseHood, OpenTrunk, CloseTrunk, LockTrunks, UnlockTrunks, OpenBusDoors, GetTrunkCount, GetTrunkAngle, GetTurret, OpenDoor, SetDoorFree, SetDirty, GetDirty, HideGore, HideDecals, SetRust, GetRust, SetColor, GetColor, SetWheelColor, SetWindowTint, GetWindowTint, SetWindowBulletproof, SetAllWindowsBulletproof, IsWindowBulletproof, SetInteriorColorsSet, GetInteriorColorsSet, GetActualFuel, SetActualFuel, GetFuelTankCapacity, SetEngineOn, IsEngineOn, Spawn, SetHandbrake, SetBrake, SetPower, GetPowerWish, GetPowerInternal, SetSteer, SetAddSteer, SetSteeringPlay, GetEngineRotations, CreateController, CreateControllerByName, DestroyController, EnableRadio, TurnRadioOn, IsRadioOn, GetRadioStation, InitializeAIParams, SetHealthCoeff, SetMaxDeceleration, SetMaxCentripetalAcceleration, SetSpeedBoost, SetRubberBanding, SetRubberBandingOn, SetRubberBandingOff, SetNavModeLock, UnlockNavMode, SetNavModeStand, SetNavModeHunt, SetNavModeHuntTeleport, SetNavModeMoveTo, SetNavModeFollowTrail, SetNavModeFollowTrailReverse, SetNavModePath, SemaphoreGreenWave, SemaphoreRedWave, SemaphoreRedWaveBehind, SetNavModeWander, SetNavModeWanderAway, SetNavModeWanderArea, DoUTurn, PullOver, DisableAvoidance, WaitForEntity, CancelWaitForEntity, StartEscort, StopEscort, AddEnemy, RemoveEnemy, RemoveObjectFromTrunk, DetectDrivingEvents, GetWheelPos, SetWheel, GetWheel, SetTyre, RegisterEvents, GetPointW, GetSeatAIPosW, GetProhibitGarage, SetProhibitGarage, Explode, SetExploded, FindSeatForHuman, GetPassengers, SetPainting, SetPaintingNoStream, GetPainting, SetResistance, GetResistance, SetLightsResistanceOverride, GetLightsResistanceOverride, DisableExplosion, IsDisableExplosion, SetBeaconLightOn, SetSirenOn, SetHornOn, SetSearchlightOn, SetSearchlightTargetPos, SetSearchlightTargetObject, SetSearchlightSweep, Honk, SetRadioDefaultState, SetSPZText, GetSPZText, GetCarProblem, FixCarPos, IsCarPosFixed, GetPartCount, ShowPart, SetWheelsProtected, IsWheelsProtected, OpenSeatWindow, IsSeatWindowOpen, GetActualPlayerSeatIndex, ReflectorsForceOff, SetLightState, SetSingleLightOverride, DisableRealLights, GetCarTableId, DisableFreeTrunk, PlayAnim, StopAnim, RevUpEffectStart, RevUpEffectStop, FadeDirtyEffectStart, FadeDirtyEffectStop, FadeFuelEffectStart, FadeFuelEffectStop, SPZAdjustingEffectStart, SPZAdjustingEffectSet, SPZAdjustingEffectStop, DisableObstacleByPart, SetDespawnImmunity, MarkForSale, IsMarkedForSale, SetImpulse, ConnectTrailer, DisconnectTrailer, GetConnectedVehicle, SetCargoLoad, StabilizeJump, UTTCarRepair, UTTCarUpgrade, UTTCarWheelChange, UTTCarPaint, UTTCarAirbrush, UTTCarPlateChange, SetGear, GetGear, GetGearCount, GetRotVelActual, GetRotVelMax, SaveRaceResult, ShowLeaderboard, GetWinningTime, SetCollisionPriority, SetPreLaunchMode, SetForceStopMode, SetMotorMalfunction, SetTakedownParams, Takedown, AttachCargo, DetachCargo, DetachAllCargo, IsProcessedByRender, SetInterrogationActionText, EnableMovableDriveSurfaceMode, SetSlowMotionAssist, EnableUnderwaterDetection, SetTireCoefOverride, CheckCarOrientationForGetIn
- `C_WrapperCollisionResult` : `C_SyncObjectUserData` (table RVA 0x5AD7220, 6 methods): HasHit, GetHitEntity, GetHitPos, GetHitNormal, GetHitFraction, RenderDebug
- `C_WrapperCrashObject` : `C_WrapperActorDeform` (table RVA 0x5AD6B00, 11 methods): Explode, Hide, Show, PlayAnim, PlayAnimNoResistance, PlayAnimLoop, PlayAnimNoResistanceLoop, StopAnim, EnablePhysics, GetTruePos, EnableAutoAim
- `C_WrapperCutscene` : `C_WrapperEntityPos` (table RVA 0x5AD4C40, 3 methods): Play, Pause, Stop
- `C_WrapperDatabaseSystem` (table RVA 0x5AC01C0, 7 methods): GetDatabase, GetSection, GetAttribute, GetStringAttribute, GetIntAttribute, GetBoolAttribute, GetFloatAttribute
- `C_WrapperDoor` : `C_WrapperActorDeform` (table RVA 0x5AD8480, 20 methods): Lock, AILock, Unlock, AIUnlock, IsLocked, StartLockpick, StopLockpick, IsLockpicking, Kick, Open, Close, ToggleOpen, IsClosed, EnableAction, DisableAction, EnableAction, DisableAction, PlayAnim, PlayAnimNoResistance, StopAnim
- `C_WrapperEntity` : `C_ScriptWrapper` (table RVA 0x5AD6920, 12 methods): RegisterEvent, UnregisterEvent, Activate, Deactivate, GameRestore, GetGUID, GetType, IsActive, IsValid, RegisterToMessages, UnregisterFromMessages, SetPreventCleaning
- `C_WrapperEntityPos` : `C_WrapperEntity` (table RVA 0x5AD7560, 8 methods): GetRot, GetPos, GetDir, SetPos, SetDir, SetRot, SetPosDir, IsVisibleFromCamera
- `C_WrapperHuman` : `C_WrapperActor` (table RVA 0x5AD2560, 418 methods): DoTalk, DoTalkAI, DoTalkAIVariation, DoTalkGhostAI, DoTalkGhostAIVariation, DoTalkDialogLine, DoTalkAIAll, DoTalkAIAll2, DoTalkAIAvailable, DoTalkAIAvailableVariation, BlockSituationsGlobally, BlockSpeech, ShutUp, ForceVoice, SelectMatchingVoice, GetVoice, DbgPlaySpeechFromSoundFilename, OverrideAudioMaterial, DesirePlayAnimation, DesirePlayAnimationWithDocking, DesirePlayAnimationWithDockingTransform, PlayAnimation, PlayAnimationDockTransform, PlaySyncedAnimation, PlaySyncedAnimationWithDocking, PlaySyncedAnimationWithDockingTransform, StopAllAnimations, StopAnimation, DisableAnimationLoop, SetAnimationStyle, UnlockAnimationStyle, SetAnimationAttitude, DebugSetDescriptor, DebugClearDescriptor, SetAnimationDescriptor, ClearAnimationDescriptor, SetAnimationVariableFloat, SetAnimationVariableInt, SetAnimationVariableBool, GetAnimationVariableFloat, GetAnimationVariableInt, GetAnimationVariableBool, SetCustomDeathAnimation, ClearCustomDeathAnimation, ActivateRagdoll, ApplyRagdollRadialSpeed, SetRagdollPreventSleep, SetFullBodyInjuryReactionThreshold, AnimPlay, AnimStop, AnimPlayEffect, AnimEffectStop, SetAnimStyle, GetBonePos, GetOppositePosRot, InventoryAddWeapon, InventoryLoadWeapons, InventoryEnsureMinCheckpointAmmo, InventoryAddAmmo, InventoryRemoveAmmoByCategory, InventoryAddAmmoByCategory, InventoryCanAddAmmoByCategory, InventoryGetAmmoLimit, InventoryGetAmmo, InventoryGetAmmoCategoryForWeapon, InventoryAddItem, InventorySelect, InventorySelectSmallWeapon, InventorySelectExplosive, InventoryGetSelectedExplosive, InventoryArmed, InventoryHolster, InventoryRemoveWeapon, InventoryRemoveItem, InventoryHaveWeapon, InventoryHasWeaponInSlot, InventoryHaveItem, InventoryGetSelected, InventoryDrop, InventorySetUnlimitedAmmo, InventorySetUnlimitedAmmoInWeakestWeapon, EnableWeaponLight, DbgEnableWeaponLightOnPlayer, InventoryGetMoney, InventoryAddMoney, InventoryRemoveMoney, InventoryReload, HaveEmptyHand, HaveFireWeaponInHand, ClearItemsInHands, InventoryAddGrenades, InventoryRemoveGrenades, InventoryCalcNumAddableGrenades, InventoryGetGrenadeCount, InventoryGetGrenadeCapacity, InventoryGetMedkitCount, InventoryGetMedkitCapacity, UnlockPlayerWeaponSlot, LockPlayerWeaponSlot, SafehouseInventoryRefill, PlayWeaponAnimation, AddSpecialItem, RemoveSpecialItem, HasSpecialItem, GetInventoryWeapons, GetInventoryWeaponsInSlotType, ClearInventory, SetPlayerHealthSegmentCnt, GetSeePlayer, IsVisibleByPlayerAim, SetVisibleByPlayerAim, MountTurret, SwitchBrain, PostAIEvent, AddAITag, RemoveAITag, MoveEnt, MoveVec, SetMovementSpeedMult, EnableElevatorMovement, SetLookPointOfInterest, IsLookPointOfInterest, Follow, SetCanTraverse, GetCanTraverse, SetTurnRadius, SetDefaultTurnRadius, ScriptControl, PanicCrouch, PanicRunAway, GetCurrentReactionPriority, GetPreventAIScriptPrio, PreventAIScriptThisPrioAndLower, ResetPreventAIScript, InterruptBattle, ResumeBattle, InterruptCivilWander, ResumeCivilWander, InterruptPatrol, ResumePatrol, InterruptAvoidingCar, ResumeAvoidingCar, OverrideWanderMoveMode, OverrideMoveSpeed, ResetMoveSpeed, SimulateCloth, IsClothSimulated, IsDeath, SetNoRagdollNoAnimAtDeath, DespawnImmunity, TakeBody, DropBody, IsCarryingBody, GetCarriedBody, CanCarryBody, ModelToHands, ModelToMouth, EnableInjury, EnableInjuryDiff, GetAIType, UseAB, IgnoreCollisions, RestoreCollisionsDelayed, GetPhysState, SetPhysState, SetPhysStandCollPriority, ShowHat, DropHat, MakeCarOwnership, MakeCarOwnership, GetInOutCar, GetOutCarFast, GetOnVehicle, GetOnVehicle, GetOffVehicle, PlayerShouldRemainPassenger, SetSpatialAwareness, SetCombatMode, SetCoverMode, SetDirtBlend, SetMaxHealth, HealByMedkit, SwitchHealthSystem, DoDamage, SetOnFireCinematic, GiveFullArmor, GiveArmor, RemoveAllArmor, GetArmor, GetMaxArmor, ReviveDownedHuman, DisableInput, IsInputDisabled, ClampMoveSpeed, ClampSprintSpeed, ResetDisabledInputsMoveSpeed, ComputeApproachAngle, SetScopeEnemyDistance, SetScopeCircle, SetScopeCirclePos, SetScopeAnnulus, SetScopeAnnulusPos, SetScopeArea, SetScopeAreaName, SetScopeBox, SetScopeOffset, SetScopeOffsetPos, SetScopeVolume, SetScopeNull, SetShootTargetScopeVolume, SetShootTargetScopeNull, SetPriorityScopeCircle, SetPriorityScopeCirclePos, SetPriorityScopeArea, SetPriorityScopeAreaName, SetPriorityScopeBox, SetPriorityScopeNull, SetPriorityScopeVolume, SetShootTargetPriority, SetBattleTactic, SetBattleLeader, GetBattleArchetype, SetBattleArchetype, GetBattleArchetypeBase, GetCivilArchetype, SetCivilArchetype, AddBattleReaction, AddBattleFakeTarget, SetShootPrecision, SetPerceptionConfig, SetTrespassingState, Patrol, PatrolWayPoints, PickClosestPatrolPoint, StopPatrolling, BroadcastPerceptionEvent, NeverLoseTrackOfPlayer, RunAwayAndDespawn, RunAwayAndDespawn2, MoveAwayAndDespawn, MoveAwayAndDespawn2, RunAwayAndRequestDespawn, RunAwayAndRequestDespawn2, MoveAwayAndRequestDespawn, MoveAwayAndRequestDespawn2, RunAwayFromEntity, RunAwayFromPosition, WanderAway, IsReloading, IsStunned, IsBlindFiring, IsSprinting, GetWeaponMagazineSize, GetEquippedWeaponSceneObject, AddCalmPerceptionScopeCircle, AddCalmPerceptionScopeCirclePos, AddCalmPerceptionScopeArea, AddCalmPerceptionScopeAreaName, AddCalmPerceptionScopeBox, AddCalmPerceptionScopeVolume, AddExclusionScopeCircle, AddExclusionScopeCirclePos, AddExclusionScopeArea, AddExclusionScopeAreaName, AddExclusionScopeBox, AddExclusionScopeVolume, SetBattleDestination, AbortBattleDestination, PreBattleMove, BattleMoveToScriptPos, BattleDefendArea, BattleDefendAreaCircle, BattleDefendAreaCirclePos, BattleDefendAreaBox, SetBattleDefendVolume, SetBattleDefendEngageArea, SetBattleDefendEngageAreaCircle, SetBattleDefendEngageAreaCirclePos, SetBattleDefendEngageAreaBox, SetBattleDefendEngageVolume, SetHERDHostileZoneArea, SetHERDHostileZoneCircle, SetHERDHostileZoneCirclePos, SetHERDHostileZoneBox, SetHERDHostileZoneVolume, ClearHERDHostileZone, SetHERDTrespassZoneArea, SetHERDTrespassZoneCircle, SetHERDTrespassZoneCirclePos, SetHERDTrespassZoneBox, SetHERDTrespassZoneVolume, ClearHERDTrespassZone, SetPoliceTrespassZoneArea, SetPoliceTrespassZoneCircle, SetPoliceTrespassZoneCirclePos, SetPoliceTrespassZoneBox, SetPoliceTrespassZoneVolume, ClearPoliceTrespassZone, SetShopkeeperTrespassZoneArea, SetShopkeeperTrespassZoneCircle, SetShopkeeperTrespassZoneCirclePos, SetShopkeeperTrespassZoneBox, SetShopkeeperTrespassZoneVolume, ClearShopkeeperTrespassZone, SetHERDTierLevel, SetHERDType, BattleDefendDynamicObject, BattleDefendStaticObject, BattleStopDefend, BattleGetInCar, BattleGetPlayerPathDist, Attack, BattleSearch, CalmSearch, SuspendBattle, AttackMelee, SetCarAttackPermission, SetCarAttackGetOutPermission, SetBattleShootingImportance, EnableCloseCombat, TakeCover, IsInCover, IsInCoverFull, ShootAt, ShootEffect, Aim, AimAt, TurnAt, TurnAtVec, TurnAtDir, Forget, SetAggressivity, GetAggressivity, SetReactionOnSound, GetReactionOnSound, SetVisible, AddMeleeTimedTag, UseSoundScene, ShowModel, IsModelVisible, SetDemigod, SetStealthMove, IsStealthMove, IsCoverCrouch, SetStalkMove, IsStalkMove, EnableDragBody, StartWatchByTarget, StartWatchByDir, StartLookAtTarget, StartLookAtDir, StopWatch, StopLookAt, MimicFriend, IsTaxiPassenger, SetDrunk, EnableBloodParticles, RemoveBloodDecals, CustomLockpick, Spawn, InterrogationTarget, SetDecideFateAfterInterrogation, GetDecideFateAfterInterrogation, SetHumanEscapeEnabled, GetHumanEscapeEnabled, EnablePushObjectHandler, PushObjectHandlerEnabled, GetEnemyPerceptionState, DbgTesting_SetAiUpdateFrequency, DbgTesting_SemaphoreNextFrame, DbgTesting_LookAtPos, DbgTesting_LookAtDir, DbgTesting_LookAtStop, DbgTesting_AimAtHuman, DbgTesting_AimAtPos, DbgTesting_AimAtDir, DbgTesting_AimAtStop, DbgTesting_ShootReload, DbgTesting_SetPose, DbgTesting_SetBodyDir, DbgTesting_Movement, DbgTesting_CoversMove, DbgTesting_Cover, DbgTesting_UseAB, DbgTesting_Inventory, DbgTesting_HumanCombatAction, DbgTesting_HelperInventory_AddWeapon, DbgTesting_HelperInventory_GetWeapon, DbgTesting_HelperInventory_GetRightHand, DbgTesting_HelperInventory_GetInventorySelected, DbgTesting_HelperInventory_IsArmed, DbgTesting_HelperInventory_GetItemsCount, DbgTesting_HelperInventory_GetItem, DbgTesting_HelperCover_IsActionAvailable, DbgTesting_HelperFire_GetAmmoCountInEquipedWeapon, DbgTesting_HelperCombatAction_IsActive, DbgTesting_GetEnumValue, IsButtonDown, ReadGamepadAnalog, RegisterInputControlListener, UnregisterInputControlListener, RegisterRangedScalar, UnregisterRangedScalar, IncreaseDecreaseTargetRangedScalar, IsFidgetDisabled, SetFidgetDisabled, KillAndThrowNPCOutOfCar, ForceMaxLOD, AnnouncePoliceOffence, GetCollisionPriorityGroup, SetCollisionPriorityGroup, ActivateEyeLookAt, DeactivateEyeLookAt, SlideToSeat, IsMeleeFighting, DisableAutoExitFromExplodedCar, QueueFullbodyNoTransition, ForceComplexCollision
- `C_WrapperParticle` : `C_WrapperEntityPos` (table RVA 0x5AD8B80, 3 methods): Run, Stop, StopImmediately
- `C_WrapperPhysics` (table RVA 0x5AD6340, 8 methods): StartCollisionQuery, MakeCollLayers, CleanObjectsInArea, CleanObjectsInSphere, IsCleanRequestStillInProgress, GetActorInScreenCenter, GetCollisionPointBelow, NewTriggerVolumeListener
- `C_WrapperPlayer` : `C_WrapperHuman` (table RVA 0x5AD7D00, 60 methods): Teleport, SetControlStyle, LockControls, LockControlsWithId, LockControlsToPlayAnim, LockControlsKeepMoving, GetCanStealVehicles, SetCanStealVehicles, GetCanBreakIntoVehicles, SetCanBreakIntoVehicles, GetCanLockpick, SetCanLockpick, SetInSafeHouse, CanEnterSafeHouse, SetSpeedCurveOverride, ClearSpeedCurveOverride, GetSpeedCurveOverride, GetMoveDir, GetNearestCoverPosInRange, IsSeenByPolice, IsHealthInCriticalState, GetClosestPolicePos, ShootWeapon, GetAimDiff, AreControlsLocked, SetControlStyleStr, GetControlStyleStr, CheckHumanPresence, GetPerkConfig, ApplyPerkUpgrade, RemovePerkUpgrade, GetPerkDataValue, AddSkillPoints, RemoveSkillPoints, GetSkillPoints, SetBlacklistState, SetBlacklistBossCriminalOperationState, SetBlacklistCriminalOperationState, SetAssetState, SetAssetData, SetAssetCriminalOperationData, SetAssetServiceData, AddToCollection, SwitchPlayerSpawnProfile, SwitchDefaultPlayerSpawnProfile, SwitchOverridePlayerSpawnProfile, ClearOverridePlayerSpawnProfile, IsPlayerUsingDefaultCostume, StartLockPull, StartInvestigateObject, ActivateExtraCollisionElement, DeactivateExtraCollisionElement, ActivateRoadDetector, SetSilentCarjacking, ForceStandHandler, EnableClimbShoreFromSwimming, ClimbShoreFromSwimmingEnabled, SetInvestigationMode, SetThrownVoodooDollDetonatable, LockPullGetSyncObjects
- `C_WrapperSection` (table RVA 0x5AC0260, 9 methods): IsRoot, GetFirstChildSection, GetNextChildSection, GetChildSectionCount, GetFirstAttribute, GetAttributeCount, GetKey, GetAttributeByKey, GetAttribute
- `C_WrapperShotManager` (table RVA 0x5AD6290, 4 methods): CreateExplosion, CreateScriptExplosion, ScriptSetExplosionsEnabled, CreateShots
- `C_WrapperTailEntity` : `C_WrapperActor` (table RVA 0x5AD72F0, 4 methods): GetDistance, ForceSelection, SetDifficulty, AllowSpeech
- `C_WrapperTailEntityStatic` (table RVA 0x5AD6810, 6 methods): CreateTailEntity, ReleaseTailEntity, SetHintStart, SetHintSuccess, SetHintEscape, SetHintFail
- `C_WrappersList` (table RVA 0x5AD63F0, 3 methods): GetEntityByGUID, GetEntityByName, GetSceneObjectByName
- `(unnamed)` : `Havok.hkVector4` (table RVA 0x4FF9C50, 62 methods): set, setAll, setAll3, setZero4, add4, add3clobberW, setAdd4, sub4, sub3clobberW, setSub4, mul4, setMul4, addMul4, subMul4, setAddMul4, setCross, setInterpolate4, equals3, equals4, allLessThan3, allLessThan4, setNeg3, setNeg4, setAbs4, setMin4, setMax4, setRotatedDir, setRotatedInverseDir, setTransformedPos, setTransformedInversePos, dot3, setDot3, dot3fpu, dot4, setDot4, horizontalAdd3, horizontalAdd4, length3, lengthSquared3, lengthInverse3, length4, lengthSquared4, lengthInverse4, normalize3, normalizeWithLength3, normalize4, normalizeWithLength4, fastNormalize3, fastNormalizeWithLength3, fastNormalize3NonZero, setXYZW, setW, setXYZ, setXYZ0, setReciprocal4, setReciprocal3, setSqrtInverse4, isOk3, isOk4, isNormalized3, isNormalized4, getMajorAxis3
- `(unnamed)` (table RVA 0x4FFB3E8, 2 methods): showPoint, showLine
- `(unnamed)` (table RVA 0x53F23D0, 11 methods): io, package, string, table, struct, math, debug, serialize, profile, os, havokscript
- `(unnamed)` (table RVA 0x53F5EA0, 25 methods): print, next, type, istable, unpack, tonumber, tostring, rawequal, rawget, rawset, select, pcall, xpcall, dofile, load, loadfile, error, assert, collectgarbage, getmetatable, setmetatable, setfenv, getfenv, newproxy, loadstring
- `(unnamed)` : `table` (table RVA 0x53F6440, 9 methods): foreach, foreachi, concat, insert, remove, maxn, sort, create, getn
- `(unnamed)` : `struct` (table RVA 0x53F6560, 5 methods): totable, isdefined, describe, name, create
- `(unnamed)` : `io` (table RVA 0x53F6640, 11 methods): write, read, close, flush, lines, input, output, type, open, tmpfile, popen
- `(unnamed)` : `FILE*` (table RVA 0x53F6700, 7 methods): close, flush, lines, read, seek, setvbuf, write
- `(unnamed)` : `os` (table RVA 0x53F69E0, 14 methods): clock, exit, remove, rename, date, time, difftime, setlocale, rawclock, clockpersecond, tmpname, sleep, execute, getenv
- `(unnamed)` : `math` (table RVA 0x53F6F00, 28 methods): abs, acos, asin, atan, atan2, ceil, cos, cosh, deg, exp, floor, fmod, frexp, ldexp, log, log10, max, min, modf, pow, rad, random, randomseed, sqrt, sin, sinh, tan, tanh
- `(unnamed)` : `debug` (table RVA 0x53F7120, 15 methods): getfenv, getinfo, getlocal, getmetatable, getregistry, getupvalue, setfenv, setlocal, setfname, setmetatable, setupvalue, traceback, c_breakpoint, print, inspect
- `(unnamed)` : `profile` (table RVA 0x53F7600, 10 methods): runtime, tick, stats, memorystats, graphheap, createuserevent, lookupusereventid, fireuserevent, beginuserevent, enduserevent
- `(unnamed)` : `havokscript` (table RVA 0x53F7C00, 3 methods): compilerSettings, setgcweights, getgcweights
- `(unnamed)` : `serialize` (table RVA 0x53F7D40, 2 methods): persist, unpersist
- `(unnamed)` (table RVA 0x5AC0420, 0 methods): 
- `(unnamed)` (table RVA 0x5AE4650, 48 methods): PathFindEnableMiddlePoint, PathFindReset, BlockSpawnersInArea, BlockSpawnersInSphere, GetRestrictedZoneCarsIntersection, FindNearestSpawnedVehicle, GetNearestFoundSpawnedEntity, ReturnNearestFoundSpawnedVehicle, ObtainCar, ObtainCarWithCrew, ObtainSpecificCar, ObtainTrain, ObtainSpecificBoat, ObtainSpecificBoatWithCrew, SpawnCar, SpawnBoat, SpawnCarArrive, SpawnCarArrivePosition, SpawnCarArriveSO, GetSpawnCarArrivePos, GetSpawnCarArriveMissingDist, SpawnTrain, SpawnBoatArrive, GetObtainedEntity, GetObtainedSceneObject, GetSpawnedEntity, GetSpawnedSceneObject, DespawnEntity, IsQueryMarkedForDespawn, DespawnEntityImmediately, DespawnPlayerCarImmediately, BanishEntity, SpawnCarConvoy, StartHuntScenario, StartFootWaveInScenario, FinishScenario, GetActiveTailEntity, DbgRegulateTraffic, SetTrainDensity, SetMaxHumanElements, SetMaxVehicleElements, DbgSetHumanSpawnRanges, DbgSetVehicleSpawnRanges, GetDLCVehicleModelNames, AddSuppressionToVolume, ForceDespawnClosingVehicleSlots, SetHumanSafeThresholdMB, ClearHumanSafeThreshold
- `(unnamed)` (table RVA 0x5AF9CD0, 17 methods): DisableGPSBlockedRoads, SelectIcon, ClearSelectedIcon, IsIconSelected, SetIconOverrideIndicatorPosition, EnableIconOverrideIndicatorPosition, SetIconFakeGPSPositionOnEntity, ChangeIconTargetToPos, ChangeIconTargetToEntityDirect, SetRacingIconData, SetRaceMedal, SetRacePersonalBest, SetGrowHouseIconData, ShowIconDuringRaces, SetFilterOnlyRaceIcons, ShowIconDuringBounty, SetFilterOnlyBountyIcons
- `(unnamed)` (table RVA 0x53F7870, 2 methods): require, module
- `(unnamed)` : `package` (table RVA 0x53F78A0, 2 methods): loadlib, seeall
- `(unnamed)` : `__index` (table RVA 0x4FFA740, 21 methods): set, setIdentity, setInverse, normalize, addMul, setMul, mul, setMulInverse, setInverseMul, setAxisAngle, removeAxisComponent, setSlerp, setReal, getReal, setImag, getAngle, hasValidAxis, isOk, setShortestRotation, setShortestRotationDamped, decomposeRestAxis
- `(unnamed)` : `__index` (table RVA 0x4FFAF00, 20 methods): set, setIdentity, getTranslation, setTranslation, getScale, setScale, getRotation, setRotation, setInterpolate4, setInverse, setMul, setMulInverseMul, setMulMulInverse, setMulEq, isOk, isApproximatelyEqual, setZero, blendAddMul, blendNormalize, fastRenormalize
