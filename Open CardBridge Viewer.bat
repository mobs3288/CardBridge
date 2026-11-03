@echo off
rem Opens the CardBridge viewer. Drag a Koikatsu card .png onto this file, or onto the window, to open it.
setlocal
set "EXE=%~dp0..\build-cardbridge\bin\cardbridge_viewer.exe"
if not exist "%EXE%" (
    echo The viewer is not built yet. Run scripts\cardbridge.ps1 first.
    pause
    exit /b 1
)
start "" "%EXE%" %*
