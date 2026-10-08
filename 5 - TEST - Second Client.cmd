@echo off
rem Starts a SECOND game on this PC to test joining your own host (dev workspace: uses the package in
rem dist\MafiaDE-Coop\client so it has its own cache and logs). Run button 1 first and load a chapter.
title Mafia co-op - second client (test)
set "PKG=%~dp0dist\MafiaDE-Coop\client"
if not exist "%PKG%\MafiaMPLauncher.exe" ( echo Run "4 - Make Package.cmd" first. & pause & exit /b 1 )
> "%PKG%\autoconnect.txt" echo 127.0.0.1 27015 Tester
cd /d "%PKG%"
start "" MafiaMPLauncher.exe
