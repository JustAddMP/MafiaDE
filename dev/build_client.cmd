@echo off
cd /d G:\MafiaCoop\Framework
set CMAKE_BUILD_PARALLEL_LEVEL=%NUMBER_OF_PROCESSORS%
echo ===== CLIENT BUILD START %DATE% %TIME% =====
call builds\build.bat MafiaMPClient 64
if errorlevel 1 ( echo ===== CLIENT BUILD FAILED ===== & exit /b 1 )
call builds\build.bat MafiaMPLauncher 64
if errorlevel 1 ( echo ===== LAUNCHER BUILD FAILED ===== & exit /b 1 )
echo ===== BUILD OK %DATE% %TIME% =====
