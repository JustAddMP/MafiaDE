@echo off
rem Starts only the co-op server (UDP 27015). Works from any folder.
title Mafia co-op server
call "%~dp0dev\tools\layout.cmd" 2>nul || call "%~dp0layout.cmd"
cd /d "%SERVER_DIR%"
MafiaMPServer.exe %*
