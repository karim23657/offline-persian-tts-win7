@echo off
rem ============================================================================
rem  gui.cmd - launch the graphical interface
rem
rem  Uses the bundled native tts_gui.exe, which needs nothing but the files
rem  already in runtime\.  If you have Python 3.8 installed and would rather
rem  use the Tkinter version, run:  python scripts\gui.py
rem ============================================================================

chcp 65001 >nul
setlocal
cd /d "%~dp0.."

if not exist "runtime\tts_gui.exe" (
    echo ERROR: runtime\tts_gui.exe is missing.
    exit /b 1
)

start "" "runtime\tts_gui.exe"
endlocal
