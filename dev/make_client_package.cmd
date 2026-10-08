@echo off
rem Builds the folder a friend needs to join: G:\MafiaCoop\dist\MafiaMP-Coop-Client (and a zip next to it).
rem libnode.dll IS required: MafiaMPClient.dll imports it (the client scripting runtime).
rem Usage: make_client_package.cmd [host-ip] [port]   (defaults: public IP looked up online, 27015)
setlocal
set SRC=G:\MafiaCoop\Framework\builds\build-64\bin
set DST=G:\MafiaCoop\dist\MafiaMP-Coop-Client
set HOSTIP=%~1
set PORT=%~2
if "%PORT%"=="" set PORT=27015
if "%HOSTIP%"=="" for /f %%I in ('curl -s --max-time 8 https://api.ipify.org') do set HOSTIP=%%I
if "%HOSTIP%"=="" set HOSTIP=YOUR.PUBLIC.IP
if not exist "%SRC%\MafiaMPLauncher.exe" (
  echo Client is not built. Run build_client.cmd first.
  exit /b 1
)
if exist "%DST%" rmdir /s /q "%DST%"
mkdir "%DST%"
robocopy "%SRC%" "%DST%" /E /NFL /NDL /NJH /NJS /XF *.pdb *.ilk MafiaMPServer.exe server.json MafiaMP_launcher.json steam_appid.txt story_host.txt story_chapter.txt story_server.txt autoconnect.txt /XD logs cache >nul
if errorlevel 8 (
  echo robocopy failed with code %errorlevel%.
  exit /b 1
)
(
echo %HOSTIP% %PORT% Friend
) > "%DST%\autoconnect.txt"
(
echo @echo off
echo cd /d "%%~dp0"
echo start "" MafiaMPLauncher.exe
) > "%DST%\JOIN GAME.cmd"
(
echo Mafia: Definitive Edition co-op - how to join
echo =============================================
echo.
echo You need:
echo   * Mafia: Definitive Edition on Steam, fully updated, and Steam running and logged in.
echo   * The Microsoft Visual C++ 2015-2022 x64 redistributable ^(most PCs have it^).
echo   * This folder extracted somewhere writable, e.g. your Desktop or Documents. Keep every file together.
echo.
echo To join:
echo   1. Make sure the host has started the game first.
echo   2. Double-click "JOIN GAME.cmd" ^(or MafiaMPLauncher.exe^). The game starts, loads the city and
echo      connects to the host by itself using autoconnect.txt. The top-right corner says
echo      "AUTO-CONNECT: connecting to ..." until you are in; it retries every 5 seconds.
echo   3. You appear right next to the host. Follow them; the whole story world is streamed around the host.
echo.
echo If it does not connect:
echo   * Open autoconnect.txt and check the host's IP and port ^(one line: IP PORT YourName^).
echo   * Or press F8 in game and type:  connect %HOSTIP% %PORT% YourName
echo   * The host must have UDP port %PORT% forwarded on their router.
echo.
echo In game:
echo   /join    teleports you back to the host     F4  co-op panel     F7  hide the overlay
echo   F8       console                             F9  disconnect      F1  unlock controls if stuck
echo   Your microphone is live automatically ^(proximity voice^). Talk normally.
echo.
echo Your own name: edit the third word in autoconnect.txt.
echo If you use the NOMAD ScriptHook mod: it is ignored automatically, nothing to uninstall.
) > "%DST%\README.txt"
if not exist "%DST%\libnode.dll" (
  echo ERROR: libnode.dll missing from package.
  exit /b 1
)
for /f %%A in ('dir /s /b "%DST%" ^| find /c /v ""') do echo Files: %%A
echo Package ready in %DST% (host %HOSTIP%:%PORT%)
powershell -NoProfile -Command "if (Test-Path '%DST%.zip') { Remove-Item '%DST%.zip' -Force }; Compress-Archive -Path '%DST%\*' -DestinationPath '%DST%.zip' -CompressionLevel Optimal; (Get-Item '%DST%.zip').Length / 1MB | ForEach-Object { 'Zip: {0:N0} MB' -f $_ }"
echo Zip ready: %DST%.zip
endlocal
