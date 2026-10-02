@echo off
rem ============================================================================
rem  diagnose.cmd - find out why the TTS crashes on this machine
rem
rem  Runs a series of checks and prints a report.  Please send the whole
rem  output when reporting a problem.
rem
rem  Usage:
rem      scripts\diagnose.cmd
rem      scripts\diagnose.cmd models\vits-piper-fa_IR-gyro-medium
rem ============================================================================

chcp 65001 >nul
setlocal
cd /d "%~dp0.."

if not exist "runtime\diagnose.exe" (
    echo ERROR: runtime\diagnose.exe is missing.
    exit /b 1
)

echo.
echo ---------------------------------------------------------------
echo  1. CPU, runtime files, and whether onnxruntime loads
echo ---------------------------------------------------------------
echo.
runtime\diagnose.exe
if errorlevel 1 goto :done

echo.
echo ---------------------------------------------------------------
echo  2. Does the engine start at all?
echo ---------------------------------------------------------------
echo.
runtime\diagnose.exe --run say.exe

rem Use an absolute path: the child process runs with its working directory
rem set to runtime\, so a relative models\... path would not resolve.
rem
rem %~f1 if given, otherwise fall back to gyro, otherwise scan models\ for
rem any folder that actually contains a .onnx file.
set "MODEL=%~f1"
if not defined MODEL (
    if exist "%CD%\models\vits-piper-fa_IR-gyro-medium\fa_IR-gyro-medium.onnx" (
        set "MODEL=%CD%\models\vits-piper-fa_IR-gyro-medium"
    ) else (
        for /d %%D in ("%CD%\models\*") do (
            dir /b "%%~fD\*.onnx" >nul 2>nul && (
                if not defined MODEL set "MODEL=%%~fD"
            )
        )
    )
)

if defined MODEL (
    echo.
    echo ---------------------------------------------------------------
    echo  3. Real synthesis with: %MODEL%
    echo ---------------------------------------------------------------
    echo.
    runtime\diagnose.exe --tts "%MODEL%"
) else (
    echo.
    echo ###########################################################
    echo  NO MODEL FOUND - the synthesis test was SKIPPED.
    echo  That is the test that matters: it is the step that crashes.
    echo.
    echo  Run:    scripts\get_model.cmd gyro
    echo  then:    scripts\diagnose.cmd
    echo.
    echo  Or point this script at a model you already have:
    echo         scripts\diagnose.cmd C:\path\to\your\model-folder
    echo  The folder must contain a .onnx file and tokens.txt.
    echo ###########################################################
)

:done
echo.
echo ---------------------------------------------------------------
echo  What the codes mean
echo ---------------------------------------------------------------
echo   0xC000001D  illegal instruction - the CPU is too old (needs AVX).
echo   0xC0000005  access violation - the DLLs are mismatched or corrupt.
echo   0xC000007B  bad DLL image - wrong bitness ^(32-bit DLL on 64-bit OS^).
echo   0xC0000135  DLL not found - usually the Visual C++ runtime.
echo   0xC0000139  entry point not found - w7shim.dll missing or not beside
echo               the binaries that import from it.
echo.
endlocal
