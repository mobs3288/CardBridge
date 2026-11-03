@echo off
rem Builds the CardBridge asset library from your own Koikatsu install. Only this step reads the game folder; after it, CardBridge never needs the game.
setlocal
cd /d "%~dp0"
echo CardBridge needs a copy of the base-game data (about 0.9 GB: skeleton, body, shaders, hair and clothes).
echo It is copied once from YOUR Koikatsu install into the "library" folder next to this file.
echo.
set "GAME="
set /p GAME=Path of your Koikatsu folder (the one that contains "abdata"; you can drag it here):
if not defined GAME goto nogame
set "GAME=%GAME:"=%"
"%~dp0cardbridge.exe" library import --game "%GAME%" --library "%~dp0library"
if errorlevel 1 goto failed
echo.
echo Done. Open "CardBridge Viewer.bat" now.
pause
exit /b 0
:nogame
echo No folder given.
pause
exit /b 1
:failed
echo.
echo The import failed. Check that the folder contains abdata\chara\oo_base.unity3d.
pause
exit /b 1
