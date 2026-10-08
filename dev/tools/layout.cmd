@echo off
rem Resolves where the client and server live, whether this is the dev workspace or an extracted package.
rem Sets CLIENT_DIR, SERVER_DIR, BASE. Call with:  call "%~dp0dev\tools\layout.cmd"  (or from a package root).
set "BASE=%~dp0"
set "BASE=%BASE:dev\tools\=%"
if exist "%BASE%client\MafiaMPLauncher.exe" (
  set "CLIENT_DIR=%BASE%client"
) else (
  set "CLIENT_DIR=%BASE%Framework\builds\build-64\bin"
)
set "SERVER_DIR=%BASE%server"
if exist "%BASE%Framework\builds\build-64\bin\MafiaMPServer.exe" (
  copy /y "%BASE%Framework\builds\build-64\bin\MafiaMPServer.exe" "%SERVER_DIR%\" >nul 2>&1
  copy /y "%BASE%Framework\builds\build-64\bin\MafiaMPServer.pdb" "%SERVER_DIR%\" >nul 2>&1
  copy /y "%BASE%Framework\builds\build-64\bin\libnode.dll" "%SERVER_DIR%\" >nul 2>&1
)
