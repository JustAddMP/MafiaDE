@echo off
rem Join a friend's game: asks for the host's address once, remembers it, starts the game, which
rem connects by itself. Works from any folder. Needs the game on Steam and Steam running.
title Mafia co-op - join
call "%~dp0dev\tools\layout.cmd" 2>nul || call "%~dp0layout.cmd"
if not exist "%CLIENT_DIR%\MafiaMPLauncher.exe" (
  echo The client is not in this package ^(%CLIENT_DIR%^).
  pause & exit /b 1
)
set "CFG=%CLIENT_DIR%\autoconnect.txt"
set "CURRENT="
if exist "%CFG%" set /p CURRENT=<"%CFG%"
if defined CURRENT echo Last used: %CURRENT%
set "HOSTIP="
set /p HOSTIP=Host IP or address (Enter = last used):
if not defined HOSTIP (
  if not defined CURRENT ( echo No host given. & pause & exit /b 1 )
) else (
  set "NICK="
  set /p NICK=Your name (Enter = Player):
  if not defined NICK set "NICK=Player"
  > "%CFG%" echo %HOSTIP% 27015 %NICK%
)
cd /d "%CLIENT_DIR%"
start "" MafiaMPLauncher.exe
