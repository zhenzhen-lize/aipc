@echo off
setlocal
cd /d "%~dp0"
gcc aipc.c -o aipc.exe -luser32 -lcrypt32 -lbcrypt
if errorlevel 1 exit /b %errorlevel%
echo Built aipc.exe