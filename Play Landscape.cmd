@echo off
setlocal
set "PATH=C:\msys64\mingw64\bin;%PATH%"
cd /d "%~dp0build\us_pc" || exit /b 1
start "" sm64coopdx.exe --skip-intro --skip-update-check
