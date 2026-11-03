@echo off
rem Converts a Koikatsu card to PMX without opening a window. Drag a card .png onto this file.
rem The PMX, its textures and a manifest go to "CardBridge output\<card name>" next to the card.
setlocal
cd /d "%~dp0"
if "%~1"=="" goto nocard
if not exist "library\abdata" goto nolib
set "MODS="
if exist "mods_folder.txt" set /p MODS=<"mods_folder.txt"
if defined MODS goto havemods
if exist "mods_folder.txt" goto havemods
set /p MODS=Path of your Koikatsu mods folder (just Enter for none):
if defined MODS set "MODS=%MODS:"=%"
>"mods_folder.txt" echo %MODS%
:havemods
set "MODSARG="
if defined MODS set MODSARG=--mods "%MODS%"
"%~dp0cardbridge.exe" convert "%~1" --library "%~dp0library" %MODSARG% --out "%~dp1CardBridge output\%~n1" --overwrite
echo.
pause
exit /b 0
:nocard
echo Drag a Koikatsu card .png onto this file.
pause
exit /b 1
:nolib
echo There is no asset library yet. Run "Set up library.bat" first.
pause
exit /b 1
