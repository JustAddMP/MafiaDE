@echo off
cd /d G:\MafiaCoop\Framework
set CMAKE_BUILD_PARALLEL_LEVEL=%NUMBER_OF_PROCESSORS%
call builds\build.bat MafiaMPServer 64
if errorlevel 1 ( echo ===== SERVER BUILD FAILED ===== & exit /b 1 )
copy /y buildsuild-64in\MafiaMPServer.exe G:\MafiaCoop\server\ >nul
copy /y buildsuild-64in\MafiaMPServer.pdb G:\MafiaCoop\server\ >nul
copy /y buildsuild-64in\libnode.dll G:\MafiaCoop\server\ >nul
echo ===== BUILD OK (server deployed to G:\MafiaCoop\server) =====
