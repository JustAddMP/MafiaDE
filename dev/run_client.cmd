@echo off
rem Starts Mafia: Definitive Edition through the MafiaMP launcher (PE-loaded, Steam build).
rem In the MafiaMP main menu enter host 127.0.0.1 and port 27015 to join your own server.
cd /d G:\MafiaCoop\Framework\builds\build-64\bin
if not exist MafiaMPLauncher.exe (
  echo MafiaMPLauncher.exe is not built yet. Run G:\MafiaCoop\build_client.cmd first.
  pause
  exit /b 1
)
start "" MafiaMPLauncher.exe
