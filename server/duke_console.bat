@echo off
REM Double-click to open the Duke Nukem Advance room server's admin console: players,
REM kick, ban, say, watch... Type help for the commands, quit to leave.
REM Logs in to host= from duke_server.txt (you will be asked for its password).
REM The console is sh_master's own, so its banner says Silent Hill Online; it is
REM still this server (Duke Nukem Advance rooms, admin port 27894), not the SH one.
REM If the connection drops, press a key to reconnect. The server keeps running
REM and logging either way: "watch" shows what happened in between.
setlocal
call :readhost || goto :eof
:again
REM Keepalives: a quiet console is otherwise dropped by routers along the way.
ssh -t -o StrictHostKeyChecking=accept-new -o ServerAliveInterval=20 -o ServerAliveCountMax=6 -o TCPKeepAlive=yes %HOST% "if command -v duke-admin >/dev/null; then duke-admin --console; else echo 'duke-admin is not on this server yet: run duke_redeploy.bat once.'; fi"
echo.
echo The console closed. Press a key to connect again, or close this window.
pause >nul
goto again

:readhost
set "HOST="
if exist "%~dp0duke_server.txt" for /f "usebackq tokens=1,* delims==" %%a in (`findstr /b /i /c:"host" "%~dp0duke_server.txt"`) do set "HOST=%%b"
if defined HOST for /f "tokens=* delims= " %%a in ("%HOST%") do set "HOST=%%a"
if not defined HOST (
    echo No host= in duke_server.txt. Copy duke_server.example.txt to duke_server.txt and fill it in.
    pause
    exit /b 1
)
echo %HOST% | findstr "@" >nul || set "HOST=root@%HOST%"
exit /b 0
