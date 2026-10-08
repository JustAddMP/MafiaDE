@echo off
rem Disables the NOMAD ScriptHook proxy so MafiaMP can run. Reversible with scripthook_on.cmd.
set GAME=G:\SteamLibrary\steamapps\common\Mafia Definitive Edition
if exist "%GAME%\dinput8.dll" (
  ren "%GAME%\dinput8.dll" dinput8.dll.scripthook_disabled
  echo ScriptHook disabled.
) else (
  echo ScriptHook already disabled.
)
