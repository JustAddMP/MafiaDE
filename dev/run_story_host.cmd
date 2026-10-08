@echo off
rem Starts Mafia: Definitive Edition through the MafiaMP launcher in STORY-HOST mode:
rem the genuine single-player campaign runs (game Lua scripts alive, vanilla main menu) while the
rem client mirrors every human/car of its world to the MafiaMP server for the guests.
rem Optional files next to MafiaMPLauncher.exe: story_host.txt (same as this env var) and
rem story_chapter.txt (a chapter save path to launch directly, e.g. 02_lost_heaven/lh_freeride_extreme.sav).
rem The client connects to 127.0.0.1 27015 by itself (story_server.txt next to the launcher overrides:
rem one line "<host> <port> [nickname]"). F8 `connect <host> <port>` still works as a fallback.
set MAFIAMP_STORY_HOST=1
cd /d G:\MafiaCoop\Framework\builds\build-64\bin
if not exist MafiaMPLauncher.exe (
  echo MafiaMPLauncher.exe is not built yet. Run G:\MafiaCoop\build_client.cmd first.
  pause
  exit /b 1
)
start "" MafiaMPLauncher.exe
