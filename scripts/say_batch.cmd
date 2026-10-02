@echo off
rem ============================================================================
rem  say_batch.cmd - one .wav per line of a UTF-8 text file
rem
rem  Usage:
rem      say_batch.cmd lines.txt            -> creates lines\1_*.wav, 2_*.wav ...
rem      say_batch.cmd lines.txt myout      -> creates myout\1_*.wav ...
rem
rem  The model is loaded once, so a long list is far faster than calling
rem  say.cmd once per line.
rem ============================================================================

chcp 65001 >nul
setlocal EnableDelayedExpansion

cd /d "%~dp0.."

set "RUNTIME=%CD%\runtime"
set "MODEL_DIR=%CD%\models\vits-piper-fa_IR-gyro-medium"

if not exist "%RUNTIME%\say.exe" (
    echo ERROR: runtime not found in "%RUNTIME%".
    exit /b 1
)

set "IN=%~1"
if "%IN%"=="" (
    echo Usage: say_batch.cmd ^<lines.txt^> [output-folder-or-prefix]
    exit /b 1
)
if not exist "%IN%" (
    echo ERROR: cannot find "%IN%".
    exit /b 1
)

rem Work out the output location: a folder named after the input file.
set "STEM=%~n1"
set "OUTDIR=%CD%\%STEM%"
set "OUTFILE=%STEM%.wav"

if not "%~2"=="" (
    rem If the second argument has no extension treat it as a folder.
    echo "%~2" | find /i ".\w\w\w$" >nul && (
        set "OUTDIR=%CD%\%~2"
        set "OUTFILE=part.wav"
    ) || (
        set "OUTFILE=%~2"
    )
)

if not exist "%OUTDIR%" mkdir "%OUTDIR%"

echo Input : %IN%
echo Output: %OUTDIR%
echo.

"%RUNTIME%\say.exe" --model "%MODEL_DIR%" --out "%OUTDIR%\%OUTFILE%" ^
    --text-file "%IN%" --split

if errorlevel 1 (
    echo.
    echo Some lines failed.
    exit /b 1
)

echo.
echo Files written to "%OUTDIR%"
endlocal
