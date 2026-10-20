@echo off
rem Moves external\core to a core release tag, rebuilds core + this game, runs the tests, and stages the change.
rem Usage: tools\update_core.cmd <tag>     e.g. tools\update_core.cmd v0.3.0
rem Then read external\core\CHANGELOG.md for anything marked Breaking, test in game, and commit.
setlocal
if "%~1"=="" ( echo Usage: %~nx0 ^<tag^>   e.g. %~nx0 v0.3.0 & exit /b 1 )
set "ROOT=%~dp0.."
set "TAG=%~1"
pushd "%ROOT%\external\core" || ( echo external\core is missing. Run: git submodule update --init & exit /b 1 )
git fetch --tags origin || ( popd & echo Could not fetch core tags. & exit /b 1 )
git rev-parse --verify --quiet "refs/tags/%TAG%^{commit}" >nul || ( popd & echo Tag %TAG% not found in core. & exit /b 1 )
git -c advice.detachedHead=false checkout --detach "refs/tags/%TAG%" || ( popd & exit /b 1 )
popd
call "%ROOT%\external\core\build.cmd" || ( echo Core build failed. & exit /b 1 )
for %%t in (net_loopback net_password net_kick net_lobby net_interest net_motion net_fuzz proxy_test pending_join crash_dump launcher_args config_set test_input update_check) do (
  "%ROOT%\external\core\out\%%t.exe" >nul || ( echo Core test %%t failed. & exit /b 1 )
)
call "%ROOT%\build.cmd" || ( echo Game build failed. & exit /b 1 )
"%ROOT%\out\proxy_test.exe" >nul || ( echo proxy_test failed. & exit /b 1 )
git -C "%ROOT%" add external/core || exit /b 1
echo.
echo Core is now at %TAG%; builds and tests passed. Check external\core\CHANGELOG.md for Breaking changes,
echo test in game, then commit:  git commit -m "Update core to %TAG%"
endlocal
