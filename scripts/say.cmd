@echo off
rem ============================================================================
rem  say.cmd - speak text to a .wav file on Windows 7
rem
rem  Usage:
rem      say.cmd "Hello there"                    -> out.wav
rem      say.cmd "Hello there" greeting.wav
rem      say.cmd --file mytext.txt                -> out.wav   (UTF-8 file)
rem      say.cmd --file mytext.txt greeting.wav
rem
rem  For Persian use --file with a UTF-8 text file.  say.exe passes the text
rem  straight to the engine as UTF-16, so it survives any console code page.
rem ============================================================================

chcp 65001 >nul
setlocal EnableDelayedExpansion

cd /d "%~dp0.."

set "RUNTIME=%CD%\runtime"
set "MODEL_DIR=%CD%\models\vits-piper-fa_IR-gyro-medium"

if not exist "%RUNTIME%\say.exe" (
    echo ERROR: runtime not found in "%RUNTIME%".
    echo Run setup.cmd first.
    exit /b 1
)

if not exist "%MODEL_DIR%\fa_IR-gyro-medium.onnx" (
    echo ERROR: model not found in "%MODEL_DIR%".
    echo Run:  scripts\get_model.cmd gyro
    exit /b 1
)

set "MODE=args"
set "TEXTARG="
set "TEXTFILE="
set "OUT=%CD%\out.wav"

:parse
if "%~1"=="" goto :run
if /i "%~1"=="--file" (
    set "MODE=file"
    set "TEXTFILE=%~2"
    shift
    shift
    goto :parse
)
rem In --file mode the first bare argument is the output file; otherwise the
rem first bare argument is the text and the second is the output file.
if "%MODE%"=="file" (
    if not defined OUTSET set "OUT=%~1" & set "OUTSET=1"
) else (
    if not defined TEXTARG (
        set "TEXTARG=%~1"
    ) else (
        set "OUT=%~1"
    )
)
shift
goto :parse

:run
if "%MODE%"=="file" (
    if not exist "%TEXTFILE%" (
        echo ERROR: text file not found: "%TEXTFILE%"
        exit /b 1
    )
    echo Model : %MODEL_DIR%
    echo Text  : %TEXTFILE%
    echo Output: %OUT%
    echo.
    "%RUNTIME%\say.exe" --model "%MODEL_DIR%" --out "%OUT%" --text-file "%TEXTFILE%"
) else (
    if not defined TEXTARG (
        echo Usage: say.cmd "text" [output.wav]
        echo    or: say.cmd --file textfile.txt [output.wav]
        exit /b 1
    )
    echo Model : %MODEL_DIR%
    echo Output: %OUT%
    echo.
    "%RUNTIME%\say.exe" --model "%MODEL_DIR%" --out "%OUT%" --text "%TEXTARG%"
)

if errorlevel 1 (
    echo.
    echo TTS failed.
    exit /b 1
)

echo.
echo Wrote "%OUT%"
endlocal