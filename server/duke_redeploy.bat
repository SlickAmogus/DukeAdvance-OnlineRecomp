@echo off
REM Install or update the Duke Nukem Advance room server on the Linux machine named in
REM duke_server.txt (host=...). Asks for its password once. If players are
REM online it lists them and lets you continue, wait until they leave, or stop.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0duke_redeploy.ps1"
echo.
pause
