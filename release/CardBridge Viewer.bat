@echo off
rem Opens the CardBridge viewer. Drag a Koikatsu card .png onto this file (or onto the window) to open it.
setlocal
cd /d "%~dp0"
if exist "library\abdata" goto run
if exist "library_full\abdata" goto run
echo There is no asset library yet.
echo Run "Set up library.bat" first: it copies what CardBridge needs from your own Koikatsu folder.
pause
exit /b 1
:run
start "" "%~dp0cardbridge_viewer.exe" %*
