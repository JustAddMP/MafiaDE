@echo off
rem Copies overlay\ (our changes to the MafiaHub Framework and MafiaMP) onto the source trees.
rem See overlay\VERSIONS.txt for the upstream commits the overlay was made against.
robocopy G:\MafiaCoop\overlay\Framework G:\MafiaCoop\Framework /E /NFL /NDL /NJH /NJS >nul
robocopy G:\MafiaCoop\overlay\MafiaMP G:\MafiaCoop\Framework\code\projects\MafiaMP /E /NFL /NDL /NJH /NJS >nul
echo Overlay applied. Build with dev\build_all.cmd.
