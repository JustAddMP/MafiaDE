@echo off
rem Starts only the co-op server (UDP 27015). Use "1 - HOST - Play Campaign.cmd" for the normal one-click host.
title Mafia co-op server
cd /d G:\MafiaCoop\dev
call run_server.cmd
