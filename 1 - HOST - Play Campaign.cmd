@echo off
rem One click to host: starts the co-op server if it is not running, then starts Mafia: Definitive
rem Edition in story-host mode (the real campaign, friends join your game).
rem Pick New Game or a chapter in the game's menu. The game connects to the server by itself.
title Mafia co-op - host
tasklist /FI "IMAGENAME eq MafiaMPServer.exe" 2>nul | find /I "MafiaMPServer.exe" >nul
if errorlevel 1 (
  echo Starting the co-op server...
  start "Mafia co-op server" /D G:\MafiaCoop\dev cmd /c run_server.cmd
  timeout /t 4 /nobreak >nul
) else (
  echo The co-op server is already running.
)
echo Starting the game as story host...
call G:\MafiaCoop\dev\run_story_host.cmd
