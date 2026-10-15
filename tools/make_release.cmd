@echo off
rem Builds MafiaCoop (with core from external\core), runs every automated test and packages the player zip and a
rem separate symbols zip (PDBs, for reading crash dumps of this build):
rem   out\release\MafiaCoop-<version>.zip
rem   out\release\MafiaCoop-<version>-symbols.zip
rem Usage: tools\make_release.cmd <version>   e.g. tools\make_release.cmd 0.8.0
rem The version must match modVersion in src\game\game_info.cpp. No game is started.
setlocal
if "%~1"=="" ( echo Usage: %~nx0 ^<version^> & exit /b 1 )
set "ROOT=%~dp0.."
set "CORE=%ROOT%\external\core"
set "VER=%~1"
findstr /c:".modVersion = \"%VER%\"" "%ROOT%\src\game\game_info.cpp" >nul || (
    echo modVersion in src\game\game_info.cpp is not %VER%. & exit /b 1 )
call "%ROOT%\build.cmd" || ( echo Build failed. & exit /b 1 )
call "%CORE%\build.cmd" || ( echo Core build failed. & exit /b 1 )
rem core's automated tests (steam_smoke is manual: it needs Steam and a steam_api64.dll copy)
for %%T in (net_loopback net_password net_kick net_lobby net_interest net_fuzz pending_join crash_dump launcher_args config_set proxy_test) do (
    "%CORE%\out\%%T.exe" >nul 2>&1 || ( echo core test %%T failed. & exit /b 1 )
    echo core test %%T: ok
)
"%ROOT%\out\proxy_test.exe" >nul 2>&1 || ( echo proxy_test failed. & exit /b 1 )
echo proxy_test: ok
set "REL=%ROOT%\out\release"
set "STAGE=%REL%\MafiaCoop-%VER%"
set "SYMS=%REL%\MafiaCoop-%VER%-symbols"
if exist "%STAGE%" rmdir /s /q "%STAGE%"
if exist "%SYMS%" rmdir /s /q "%SYMS%"
mkdir "%STAGE%\licenses" || exit /b 1
mkdir "%SYMS%" || exit /b 1
copy /y "%ROOT%\out\MafiaCoop.exe" "%STAGE%\" >nul || exit /b 1
copy /y "%ROOT%\out\mafiacoop.dll" "%STAGE%\" >nul || exit /b 1
copy /y "%ROOT%\out\mafiacoop_proxy.dll" "%STAGE%\" >nul || exit /b 1
copy /y "%ROOT%\tools\release-readme.txt" "%STAGE%\README.txt" >nul || exit /b 1
copy /y "%CORE%\third_party\minhook\LICENSE.txt" "%STAGE%\licenses\MinHook-LICENSE.txt" >nul || exit /b 1
copy /y "%ROOT%\out\MafiaCoop-launcher.pdb" "%SYMS%\" >nul || exit /b 1
copy /y "%ROOT%\out\mafiacoop.pdb" "%SYMS%\" >nul || exit /b 1
copy /y "%ROOT%\out\mafiacoop_proxy.pdb" "%SYMS%\" >nul || exit /b 1
set "ZIP=%REL%\MafiaCoop-%VER%.zip"
set "SZIP=%REL%\MafiaCoop-%VER%-symbols.zip"
if exist "%ZIP%" del "%ZIP%"
if exist "%SZIP%" del "%SZIP%"
powershell -NoProfile -Command "Compress-Archive -Path '%STAGE%' -DestinationPath '%ZIP%' -CompressionLevel Optimal" || exit /b 1
powershell -NoProfile -Command "Compress-Archive -Path '%SYMS%' -DestinationPath '%SZIP%' -CompressionLevel Optimal" || exit /b 1
echo Release ready: %ZIP%
echo Symbols:       %SZIP%
endlocal
