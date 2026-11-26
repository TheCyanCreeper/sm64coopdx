@echo off
setlocal
set "LANDSCAPE_PROFILE=%~dp0landscape-profile"
if not exist "%LANDSCAPE_PROFILE%\" mkdir "%LANDSCAPE_PROFILE%" || exit /b 1
if not exist "%LANDSCAPE_PROFILE%\baserom.us.z64" (
    copy /b "%APPDATA%\sm64coopdx\baserom.us.z64" "%LANDSCAPE_PROFILE%\baserom.us.z64" >nul || exit /b 1
)
set "PATH=C:\msys64\mingw64\bin;%PATH%"
cd /d "%~dp0build\us_pc" || exit /b 1
start "" castle-expedition.exe --savepath "%LANDSCAPE_PROFILE%" --skip-intro --skip-update-check
