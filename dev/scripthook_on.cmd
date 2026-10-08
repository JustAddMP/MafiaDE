@echo off
rem Re-enables the NOMAD ScriptHook proxy for normal single-player use.
set GAME=G:\SteamLibrary\steamapps\common\Mafia Definitive Edition
if exist "%GAME%\dinput8.dll.scripthook_disabled" (
  ren "%GAME%\dinput8.dll.scripthook_disabled" dinput8.dll
  echo ScriptHook enabled.
) else (
  echo ScriptHook already enabled.
)
