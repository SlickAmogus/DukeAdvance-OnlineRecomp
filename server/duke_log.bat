@echo off
REM Double-click to download the Duke Nukem Advance room server's whole log and open it
REM in Notepad (a copy in your temp folder; the server keeps writing its own).
REM Logs in to host= from duke_server.txt (you will be asked for its password).
setlocal
call :readhost || goto :eof
set "OUT=%TEMP%\duke_rooms_server.log"
ssh -o StrictHostKeyChecking=accept-new %HOST% "duke-admin log" > "%OUT%"
if errorlevel 1 (
    echo Could not fetch the log. If duke-admin is missing, run duke_redeploy.bat once.
    pause
    goto :eof
)
start "" notepad "%OUT%"
goto :eof

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
