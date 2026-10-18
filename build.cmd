@echo off
rem MafiaCoop build: JustAddMP core (external\core, a git submodule) + the Mafia DE game layer.
rem Usage: build.cmd   (runnable from any directory; builds only, runs nothing)
rem Output in out\: MafiaCoop.exe, mafiacoop.dll, mafiacoop_proxy.dll, proxy_test.exe (core's test against our
rem descriptor), our own unit tests (tests\*.cpp -> out\<name>.exe), PDBs next to them; core objects and libraries in
rem out\obj\core, ours in out\obj\.
setlocal EnableExtensions EnableDelayedExpansion
set "ROOT=%~dp0"
if "%ROOT:~-1%"=="\" set "ROOT=%ROOT:~0,-1%"
set "CORE=%ROOT%\external\core"
set "OUT=%ROOT%\out"
set "OBJ=%OUT%\obj"
set "CB=%OBJ%\core"
set "GEN=%OBJ%\gen"

if not exist "%CORE%\build.cmd" (
  echo [build] ERROR: external\core is empty. Run: git submodule update --init
  goto :fail
)

call "%CORE%\tools\msvc.cmd" || goto :fail

rem ---- core: libraries and objects into out\obj\core --------------------------
call "%CORE%\build.cmd" "%CB%" || goto :fail

if not exist "%GEN%" mkdir "%GEN%"
for %%d in (game tests) do if not exist "%OBJ%\%%d" mkdir "%OBJ%\%%d"
set "INCS=/I"%ROOT%\src" /I"%GEN%" /I"%CORE%\src" /I"%CORE%\include""
set "CXXFLAGS=%MC_CXXFLAGS% %INCS%"

rem ---- embedded Lua: src\game\*.lua -> out\obj\gen\<name>_lua.h (const char k<Name>Lua[], raw string chunks) ----
rem The Lua glue that runs inside the game's VM ships inside the dll; build.cmd regenerates it on every build.
for %%f in ("%ROOT%\src\game\*.lua") do (
  echo [build] embed %%~nxf
  powershell -NoProfile -ExecutionPolicy Bypass -File "%CORE%\tools\embed_lua.ps1" -In "%%f" -Out "%GEN%\%%~nf_lua.h"
  if errorlevel 1 ( echo [build] ERROR: embedding %%~nxf failed & goto :fail )
)

rem ---- game layer (every .cpp in src\game) -------------------------------------
echo [build] src\game
del /q "%OBJ%\game\*.obj" 2>nul
cl %CXXFLAGS% /Fd"%OBJ%\game\vc.pdb" /Fo"%OBJ%\game\\" "%ROOT%\src\game\*.cpp"
if errorlevel 1 ( echo [build] ERROR: compiling src\game failed & goto :fail )
rem game_info.obj (GetGameInfo) goes into all three binaries; the rest only into the dll.
set "INFO="%OBJ%\game\game_info.obj""

echo [build] link mafiacoop.dll
link %MC_LDFLAGS% /DLL /OUT:"%OUT%\mafiacoop.dll" /PDB:"%OUT%\mafiacoop.pdb" /IMPLIB:"%OBJ%\mafiacoop.lib" ^
  "%CB%\entry\dllmain.obj" "%OBJ%\game\*.obj" "%CB%\mc_core.lib" %MC_SYSLIBS%
if errorlevel 1 ( echo [build] ERROR: linking mafiacoop.dll failed & goto :fail )

echo [build] link MafiaCoop.exe
link %MC_LDFLAGS% /SUBSYSTEM:WINDOWS /OUT:"%OUT%\MafiaCoop.exe" /PDB:"%OUT%\MafiaCoop-launcher.pdb" ^
  "%CB%\launcher\*.obj" %INFO% kernel32.lib user32.lib advapi32.lib shell32.lib
if errorlevel 1 ( echo [build] ERROR: linking MafiaCoop.exe failed & goto :fail )

rem out\mafiacoop_proxy.dll: MafiaCoop.exe --install copies it into the game folder as version.dll. It is not called
rem version.dll in out\ so the exes there keep loading Windows' own version.dll.
echo [build] link mafiacoop_proxy.dll
link %MC_LDFLAGS% /DLL /DEF:"%CORE%\src\proxy\version.def" /OUT:"%OUT%\mafiacoop_proxy.dll" ^
  /PDB:"%OUT%\mafiacoop_proxy.pdb" /IMPLIB:"%OBJ%\mafiacoop_proxy.lib" "%CB%\proxy\*.obj" %INFO% kernel32.lib
if errorlevel 1 ( echo [build] ERROR: linking mafiacoop_proxy.dll failed & goto :fail )

rem ---- core's proxy_test, against our descriptor (tests MafiaCoop.exe + mafiacoop_proxy.dll without the game) ----
echo [build] proxy_test.exe
cl %CXXFLAGS% /Fd"%OBJ%\tests\vc.pdb" /Fo"%OBJ%\tests\proxy_test.obj" "%CORE%\tests\proxy_test.cpp"
if errorlevel 1 ( echo [build] ERROR: compiling proxy_test failed & goto :fail )
link %MC_LDFLAGS% /SUBSYSTEM:CONSOLE /OUT:"%OUT%\proxy_test.exe" /PDB:"%OUT%\proxy_test.pdb" ^
  "%OBJ%\tests\proxy_test.obj" %INFO% "%CB%\mc_core.lib" %MC_SYSLIBS%
if errorlevel 1 ( echo [build] ERROR: linking proxy_test.exe failed & goto :fail )

rem the game-free parts of src\game the tests link (only what a test uses is pulled in)
lib /NOLOGO /OUT:"%OBJ%\game_logic.lib" "%OBJ%\game\car_swap.obj" "%OBJ%\game\npc_owner.obj" "%OBJ%\game\traffic_model.obj" "%OBJ%\game\fail_flow.obj" "%OBJ%\game\map_markers.obj" "%OBJ%\game\world_items.obj" "%OBJ%\game\traversal.obj" "%OBJ%\game\door_model.obj" "%OBJ%\game\police_model.obj" "%OBJ%\game\explosion_model.obj" "%OBJ%\game\car_damage.obj" "%OBJ%\game\seats.obj" "%OBJ%\game\latency.obj" "%OBJ%\game\throw_model.obj"
if errorlevel 1 ( echo [build] ERROR: game_logic.lib failed & goto :fail )
rem ---- our unit tests: every tests\*.cpp is one console exe (game-layer logic that needs no game) ----------------
for %%f in ("%ROOT%\tests\*.cpp") do (
  echo [build] %%~nf.exe
  cl %CXXFLAGS% /Fd"%OBJ%\tests\vc.pdb" /Fo"%OBJ%\tests\%%~nf.obj" "%%f"
  if errorlevel 1 ( echo [build] ERROR: compiling %%~nxf failed & goto :fail )
  link %MC_LDFLAGS% /SUBSYSTEM:CONSOLE /OUT:"%OUT%\%%~nf.exe" /PDB:"%OUT%\%%~nf.pdb" "%OBJ%\tests\%%~nf.obj" ^
    "%OBJ%\game_logic.lib" %INFO% "%CB%\mc_core.lib" %MC_SYSLIBS%
  if errorlevel 1 ( echo [build] ERROR: linking %%~nf.exe failed & goto :fail )
)

echo [build] OK -^> %OUT%
endlocal
exit /b 0

:fail
echo [build] FAILED
endlocal
exit /b 1
