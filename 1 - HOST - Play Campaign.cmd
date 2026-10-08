@echo off
rem One click to host: starts the co-op server if it is not running, then starts Mafia: Definitive
rem Edition in story-host mode (the real campaign; friends join your game). Works from any folder.
title Mafia co-op - host
call "%~dp0dev\tools\layout.cmd" 2>nul || call "%~dp0layout.cmd"
if not exist "%CLIENT_DIR%\MafiaMPLauncher.exe" (
  echo The client is not built or not in this package ^(%CLIENT_DIR%^).
  pause & exit /b 1
)
tasklist /FI "IMAGENAME eq MafiaMPServer.exe" 2>nul | find /I "MafiaMPServer.exe" >nul
if errorlevel 1 (
  echo Starting the co-op server...
  start "Mafia co-op server" /D "%SERVER_DIR%" MafiaMPServer.exe
  timeout /t 4 /nobreak >nul
) else (
  echo The co-op server is already running.
)
echo Starting the game as story host...
set MAFIAMP_STORY_HOST=1
cd /d "%CLIENT_DIR%"
start "" MafiaMPLauncher.exe
