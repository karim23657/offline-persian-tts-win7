@echo off
rem ============================================================================
rem  server.cmd - start the local web interface
rem
rem  Opens http://127.0.0.1:8756/ .  The model is loaded once and stays in
rem  memory, so the second generation is much quicker than the first.
rem  Press Ctrl+C to stop.
rem
rem  Usage:
rem      scripts\server.cmd                 port 8756, preload the first model
rem      scripts\server.cmd --port 9000     a different port
rem      scripts\server.cmd --no-preload    start at once, load on first use
rem
rem  Anything else is passed straight to tts_server.exe; --help lists it all.
rem ============================================================================

chcp 65001 >nul
setlocal
cd /d "%~dp0.."

if not exist "runtime\tts_server.exe" (
    echo ERROR: runtime\tts_server.exe is missing. Run build.cmd, or make.
    exit /b 1
)

set "PRELOAD=--preload"
if /i "%~1"=="--no-preload" (
    set "PRELOAD="
    shift
)

echo.
echo   Starting the web interface...
echo   Open the address it prints. Press Ctrl+C here to stop.
echo.
runtime\tts_server.exe %PRELOAD% %1 %2 %3 %4
endlocal
