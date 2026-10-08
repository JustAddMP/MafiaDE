@echo off
rem Starts a SECOND game on this PC to test joining your own story host, using the friend package
rem folder (its own cache and logs, the exact build a friend gets). Run "1 - HOST - Play Campaign.cmd"
rem first and load a chapter. The second game loads free ride and connects to 127.0.0.1 27015 by
rem itself (autoconnect.txt) once that build is in; until then enter 127.0.0.1 and 27015 in its menu.
rem Needs a lot of RAM and GPU: play both windowed. Both instances use the same Steam account; the
rem server does not mind. Press F4 / F9 in the window that has focus.
title Mafia co-op - second client (test)
set PKG=G:\MafiaCoop\dist\MafiaMP-Coop-Client
if not exist "%PKG%\MafiaMPLauncher.exe" (
  echo The friend package does not exist yet. Run "4 - Make Friend Package.cmd" first.
  pause
  exit /b 1
)
rem Local test: connect to this PC, not the public IP that the package ships with.
echo 127.0.0.1 27015 Tester> "%PKG%\autoconnect.txt"
cd /d "%PKG%"
start "" MafiaMPLauncher.exe
echo Second client started from %PKG%. Its log: %PKG%\logs\MafiaMP.log
echo (autoconnect.txt in the package now points at 127.0.0.1; "4 - Make Friend Package.cmd" resets it to your public IP.)
