@echo off
rem ---------------------------------------------------------------
rem  ESP32-C3 Bluetooth Video Remote - local flash server
rem  Starts a tiny local web server so the browser can load
rem  index.html over http:// (required by ESP Web Tools),
rem  then opens the page automatically. Close with Ctrl+C.
rem  NOTE: this file is intentionally ASCII-only on purpose.
rem ---------------------------------------------------------------
title ESP32-C3 Flash Server
cd /d "%~dp0"

where node >nul 2>nul
if %errorlevel%==0 goto :node

where python >nul 2>nul
if %errorlevel%==0 goto :python

echo.
echo   [ERROR] Node.js or Python was not found on this PC.
echo.
echo   Please install one of them:
echo     - Node.js : https://nodejs.org   (recommended)
echo     - Python  : https://python.org
echo.
echo   Or just use Arduino IDE 2.x to flash the firmware.
echo.
pause
exit /b 1

:node
echo   Using Node.js ...
node server.js %*
goto :eof

:python
echo   Using Python ...
python -m http.server 8930
goto :eof
