@echo off
rem Starts the MafiaMP server with the co-op story gamemode.
rem Resources are junctions into the MafiaMP repo, so editing the JS there is live (`ensure coop-story`).
rem The newest server build is copied in first, so "restart the server" is enough after a rebuild.
cd /d G:\MafiaCoop\server
copy /y G:\MafiaCoop\Framework\builds\build-64\bin\MafiaMPServer.exe . >nul 2>&1
copy /y G:\MafiaCoop\Framework\builds\build-64\bin\MafiaMPServer.pdb . >nul 2>&1
copy /y G:\MafiaCoop\Framework\builds\build-64\bin\libnode.dll . >nul 2>&1
echo Starting MafiaMP server (co-op story) on UDP 27015 ...
MafiaMPServer.exe %*
