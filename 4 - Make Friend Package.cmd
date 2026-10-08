@echo off
rem Builds the folder + zip a friend needs to join you: dist\MafiaMP-Coop-Client (.zip next to it).
rem The zip has your current public IP in autoconnect.txt. Optional: pass an IP and port as arguments.
title Mafia co-op - friend package
call G:\MafiaCoop\dev\make_client_package.cmd %*
pause
