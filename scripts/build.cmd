@echo off
rem Thin wrapper so the build can be started from Explorer / cmd.exe.
rem Usage: scripts\build.cmd [-Config Release] [-Test] [-Deploy] [-Run]
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0build.ps1" %*
