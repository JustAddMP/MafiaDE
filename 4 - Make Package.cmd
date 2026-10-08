@echo off
rem Builds the shareable package dist\MafiaDE-Coop (+ .zip): the buttons, the server and the client.
rem Anyone can extract it anywhere and either HOST (button 1) or JOIN (button 3). Dev workspace only.
title Mafia co-op - package
setlocal
set "BASE=%~dp0"
set "SRC=%BASE%Framework\builds\build-64\bin"
set "RES=%BASE%Framework\code\projects\MafiaMP\resources"
set "DST=%BASE%dist\MafiaDE-Coop"
if not exist "%SRC%\MafiaMPLauncher.exe" ( echo Client is not built. Run dev\build_all.cmd first. & pause & exit /b 1 )
if exist "%DST%" rmdir /s /q "%DST%"
mkdir "%DST%\client" "%DST%\server\resources" "%DST%\server\logs"
robocopy "%SRC%" "%DST%\client" /E /NFL /NDL /NJH /NJS /XF *.pdb *.ilk MafiaMPServer.exe server.json MafiaMP_launcher.json steam_appid.txt story_host.txt story_chapter.txt story_server.txt autoconnect.txt /XD logs cache >nul
if errorlevel 8 ( echo robocopy failed & exit /b 1 )
copy /y "%SRC%\MafiaMPServer.exe" "%DST%\server\" >nul
copy /y "%SRC%\libnode.dll" "%DST%\server\" >nul
copy /y "%BASE%server\server.json" "%DST%\server\" >nul
robocopy "%RES%\coop-story" "%DST%\server\resources\coop-story" /E /NFL /NDL /NJH /NJS >nul
robocopy "%RES%\shared-utils" "%DST%\server\resources\shared-utils" /E /NFL /NDL /NJH /NJS >nul
copy /y "%BASE%1 - HOST - Play Campaign.cmd" "%DST%\" >nul
copy /y "%BASE%2 - HOST - Start Server Only.cmd" "%DST%\" >nul
copy /y "%BASE%3 - JOIN - Join a Game.cmd" "%DST%\" >nul
copy /y "%BASE%dev\tools\layout.cmd" "%DST%\" >nul
copy /y "%BASE%README.md" "%DST%\README.md" >nul
for /f %%A in ('dir /s /b "%DST%" ^| find /c /v ""') do echo Files: %%A
powershell -NoProfile -Command "if (Test-Path '%DST%.zip') { Remove-Item '%DST%.zip' -Force }; Compress-Archive -Path '%DST%\*' -DestinationPath '%DST%.zip' -CompressionLevel Optimal; (Get-Item '%DST%.zip').Length / 1MB | ForEach-Object { 'Zip: {0:N0} MB' -f $_ }"
echo Package ready: %DST%.zip
endlocal
