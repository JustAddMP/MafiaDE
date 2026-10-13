@echo off
rem MafiaCoop native build. Usage: build.cmd   (runnable from any directory)
rem Output: out\MafiaCoop.exe, out\mafiacoop.dll, out\test_*.exe, PDBs next to them, objects in out\obj.
setlocal EnableExtensions EnableDelayedExpansion
set "ROOT=%~dp0"
if "%ROOT:~-1%"=="\" set "ROOT=%ROOT:~0,-1%"
set "OUT=%ROOT%\out"
set "OBJ=%OUT%\obj"
set "GEN=%OUT%\obj\gen"

rem ---- toolchain -------------------------------------------------------------
where cl.exe >nul 2>nul
if errorlevel 1 (
  set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
  if not exist "!VSWHERE!" ( echo [build] ERROR: vswhere.exe not found & goto :fail )
  set "VCVARS="
  for /f "usebackq delims=" %%i in (`"!VSWHERE!" -latest -prerelease -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -find VC\Auxiliary\Build\vcvars64.bat`) do set "VCVARS=%%i"
  if not defined VCVARS ( echo [build] ERROR: no Visual Studio with the x64 C++ toolset found & goto :fail )
  echo [build] using !VCVARS!
  set "PATH=!PATH!;%ProgramFiles(x86)%\Microsoft Visual Studio\Installer"
  call "!VCVARS!" >nul
  if errorlevel 1 ( echo [build] ERROR: vcvars64.bat failed & goto :fail )
)
where cl.exe >nul 2>nul || ( echo [build] ERROR: cl.exe still not on PATH & goto :fail )

if not exist "%OUT%" mkdir "%OUT%" || goto :fail
if not exist "%GEN%" mkdir "%GEN%"
for %%d in (minhook launcher core render game net dll tests) do if not exist "%OBJ%\%%d" mkdir "%OBJ%\%%d"

set "DEFS=/DWIN32_LEAN_AND_MEAN /DNOMINMAX /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS"
set "INCS=/I"%ROOT%\src" /I"%GEN%" /I"%ROOT%\third_party\minhook\include""
set "CXXFLAGS=/nologo /c /std:c++20 /EHsc /MT /O2 /Zi /FS /W4 /permissive- /utf-8 /Zc:__cplusplus %DEFS% %INCS%"
set "CFLAGS=/nologo /c /MT /O2 /Zi /FS /W3 /utf-8 %DEFS%"
set "LDFLAGS=/nologo /DEBUG:FULL /OPT:REF /OPT:ICF /INCREMENTAL:NO /MACHINE:X64"

rem ---- minhook (C) -----------------------------------------------------------
echo [build] minhook
del /q "%OBJ%\minhook\*.obj" 2>nul
cl %CFLAGS% /Fd"%OBJ%\minhook\vc.pdb" /Fo"%OBJ%\minhook\\" ^
  "%ROOT%\third_party\minhook\src\buffer.c" "%ROOT%\third_party\minhook\src\hook.c" ^
  "%ROOT%\third_party\minhook\src\trampoline.c" "%ROOT%\third_party\minhook\src\hde\hde64.c" >"%OBJ%\minhook\cl.log"
if errorlevel 1 ( type "%OBJ%\minhook\cl.log" & echo [build] ERROR: minhook failed & goto :fail )

rem ---- shaders: src\render\*.hlsl -> out\obj\gen\<name>_vs.h / _ps.h (fxc from the Windows SDK) ------------
rem Bytecode arrays k<Name>VS / k<Name>PS (e.g. overlay.hlsl -> kOverlayVS in overlay_vs.h). Precompiled so the
rem dll needs no d3dcompiler_47.dll at runtime.
where fxc.exe >nul 2>nul || ( echo [build] ERROR: fxc.exe not on PATH ^(Windows SDK^) & goto :fail )
for %%f in ("%ROOT%\src\render\*.hlsl") do (
  echo [build] shader %%~nxf
  set "SN=%%~nf"
  set "SC=!SN:~0,1!"
  for %%u in (A B C D E F G H I J K L M N O P Q R S T U V W X Y Z) do if /i "!SC!"=="%%u" set "SC=%%u"
  set "SC=!SC!!SN:~1!"
  fxc /nologo /T vs_4_0 /E VSMain /O3 /Vn k!SC!VS /Fh "%GEN%\%%~nf_vs.h" "%%f" >"%GEN%\%%~nf_vs.log" 2>&1
  if errorlevel 1 ( type "%GEN%\%%~nf_vs.log" & echo [build] ERROR: shader %%~nxf VS failed & goto :fail )
  fxc /nologo /T ps_4_0 /E PSMain /O3 /Vn k!SC!PS /Fh "%GEN%\%%~nf_ps.h" "%%f" >"%GEN%\%%~nf_ps.log" 2>&1
  if errorlevel 1 ( type "%GEN%\%%~nf_ps.log" & echo [build] ERROR: shader %%~nxf PS failed & goto :fail )
)

rem ---- dll source groups (every .cpp in each dir) ------------------------------
set "DLLOBJS="%OBJ%\minhook\*.obj""
for %%d in (core render game net dll) do (
  del /q "%OBJ%\%%d\*.obj" 2>nul
  if exist "%ROOT%\src\%%d\*.cpp" (
    echo [build] src\%%d
    cl %CXXFLAGS% /Fd"%OBJ%\%%d\vc.pdb" /Fo"%OBJ%\%%d\\" "%ROOT%\src\%%d\*.cpp"
    if errorlevel 1 ( echo [build] ERROR: compiling src\%%d failed & goto :fail )
    set "DLLOBJS=!DLLOBJS! "%OBJ%\%%d\*.obj""
  )
)

set "SYSLIBS=kernel32.lib user32.lib gdi32.lib advapi32.lib shell32.lib ole32.lib ws2_32.lib d3d11.lib dxgi.lib dbghelp.lib"

echo [build] link mafiacoop.dll
link %LDFLAGS% /DLL /OUT:"%OUT%\mafiacoop.dll" /PDB:"%OUT%\mafiacoop.pdb" /IMPLIB:"%OBJ%\mafiacoop.lib" %DLLOBJS% %SYSLIBS%
if errorlevel 1 ( echo [build] ERROR: linking mafiacoop.dll failed & goto :fail )

rem ---- launcher --------------------------------------------------------------
echo [build] src\launcher
del /q "%OBJ%\launcher\*.obj" 2>nul
cl %CXXFLAGS% /Fd"%OBJ%\launcher\vc.pdb" /Fo"%OBJ%\launcher\\" "%ROOT%\src\launcher\*.cpp"
if errorlevel 1 ( echo [build] ERROR: compiling launcher failed & goto :fail )
link %LDFLAGS% /SUBSYSTEM:WINDOWS /OUT:"%OUT%\MafiaCoop.exe" /PDB:"%OUT%\MafiaCoop-launcher.pdb" "%OBJ%\launcher\*.obj" kernel32.lib user32.lib advapi32.lib shell32.lib
if errorlevel 1 ( echo [build] ERROR: linking MafiaCoop.exe failed & goto :fail )

rem ---- tests: one exe per tests\*.cpp, linked with src\net + src\core ---------
if exist "%ROOT%\tests\*.cpp" (
  set "LIBOBJS="%OBJ%\minhook\*.obj" "%OBJ%\core\*.obj""
  if exist "%OBJ%\net\*.obj" set "LIBOBJS=!LIBOBJS! "%OBJ%\net\*.obj""
  del /q "%OBJ%\tests\*.obj" 2>nul
  for %%f in ("%ROOT%\tests\*.cpp") do (
    echo [build] tests\%%~nxf
    cl %CXXFLAGS% /I"%ROOT%\tests" /Fd"%OBJ%\tests\vc.pdb" /Fo"%OBJ%\tests\%%~nf.obj" "%%f"
    if errorlevel 1 ( echo [build] ERROR: compiling tests\%%~nxf failed & goto :fail )
    link %LDFLAGS% /SUBSYSTEM:CONSOLE /OUT:"%OUT%\%%~nf.exe" /PDB:"%OUT%\%%~nf.pdb" "%OBJ%\tests\%%~nf.obj" !LIBOBJS! %SYSLIBS%
    if errorlevel 1 ( echo [build] ERROR: linking %%~nf.exe failed & goto :fail )
  )
) else (
  echo [build] no tests\*.cpp, skipping tests
)

echo [build] OK -^> %OUT%
endlocal
exit /b 0

:fail
echo [build] FAILED
endlocal
exit /b 1
