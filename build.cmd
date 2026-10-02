@echo off
rem ============================================================================
rem  build.cmd - rebuild the runtime on a Windows machine
rem
rem  Use this if you do not have Linux/make. It runs the same steps as the
rem  Makefile: fetch the pinned sherpa-onnx release, derive the list of
rem  Windows 8+ imports from the real binaries, compile the shim, patch the
rem  upstream binaries and build the front ends.
rem
rem  Needs, installed and on PATH:
rem     - Python 3        https://www.python.org/downloads/
rem     - MinGW-w64       https://www.mingw-w64.org/downloads/  (i686 or x86_64)
rem     - 7-Zip           to unpack the .tar.bz2 release, or bsdtar/tar
rem
rem  Usage:
rem     build.cmd              fetch + build everything
rem     build.cmd fetch        only download the sherpa-onnx release
rem     build.cmd imports      only regenerate src\shim\imports.txt
rem     build.cmd shim         only rebuild w7shim.dll
rem     build.cmd all          build + patch + front ends  (same as no argument)
rem
rem  The Visual C++ redistributable is NOT needed to build, only to run.
rem ============================================================================

setlocal EnableDelayedExpansion
cd /d "%~dp0"

set "SRC=src"
set "BUILD=build"
set "SHERPA=%BUILD%\sherpa\unpacked"
set "RUNTIME=runtime"
set "PY=python"
set "CC=x86_64-w64-mingw32-gcc"

where %PY% >nul 2>nul || { echo ERROR: Python 3 is not on PATH. & exit /b 1 }
where %CC% >nul 2>nul || (
  where gcc >nul 2>nul || { echo ERROR: MinGW-w64 is not on PATH. & exit /b 1 }
  set "CC=gcc"
)

rem ---- unpacking a .tar.bz2: try 7z, then bsdtar, then tar -----------------
set "UNTAR="
where 7z >nul 2>nul && set "UNTAR=7z"
if not defined UNTAR where bsdtar >nul 2>nul && set "UNTAR=bsdtar"
if not defined UNTAR where tar >nul 2>nul && set "UNTAR=tar"

echo.
echo ==== step 1: fetch the pinned sherpa-onnx release
%PY% %SRC%\shim\fetch_sherpa.py
if errorlevel 1 goto :fail

if /i "%~1"=="fetch" goto :done

echo.
echo ==== step 2: scan the binaries for imports the shim must provide
%PY% %SRC%\shim\scan_imports.py ^
    "%SHERPA%\bin\sherpa-onnx-offline-tts.exe" ^
    "%SHERPA%\lib\sherpa-onnx-c-api.dll" ^
    "%SHERPA%\lib\onnxruntime.dll" ^
    --out "%SRC%\shim\imports.txt"
if errorlevel 1 goto :fail

if /i "%~1"=="imports" goto :done

echo.
echo ==== step 3: generate and compile the shim
%PY% %SRC%\shim\gen_shim.py "%SRC%\shim\imports.txt" --outdir "%BUILD%\shim"
if errorlevel 1 goto :fail
%CC% -shared -O2 -Wall -I%SRC%\shim -o %RUNTIME%\w7shim.dll ^
    %SRC%\shim\win7shim.c %SRC%\shim\win7shim_fallback.c ^
    %BUILD%\shim\win7shim_table.c %BUILD%\shim\win7shim_thunks.S ^
    %BUILD%\shim\win7shim.def -lkernel32 -static-libgcc
if errorlevel 1 goto :fail
echo     wrote %RUNTIME%\w7shim.dll

if /i "%~1"=="shim" goto :done

echo.
echo ==== step 4: copy the upstream binaries into runtime\ and patch them
if not exist %RUNTIME% mkdir %RUNTIME%
copy /Y "%SHERPA%\bin\sherpa-onnx-offline-tts.exe"  %RUNTIME%\ >nul
copy /Y "%SHERPA%\lib\onnxruntime.dll"                %RUNTIME%\ >nul
copy /Y "%SHERPA%\lib\onnxruntime_providers_shared.dll" %RUNTIME%\ >nul
copy /Y "%SHERPA%\lib\sherpa-onnx-c-api.dll"          %RUNTIME%\ >nul
copy /Y "%SHERPA%\lib\sherpa-onnx-cxx-api.dll"        %RUNTIME%\ >nul

rem Patch the COPIES. Patching build\sherpa\unpacked would stop the next
rem scan from finding KERNEL32.dll and produce an empty imports.txt.
for %%F in (sherpa-onnx-offline-tts.exe sherpa-onnx-c-api.dll onnxruntime.dll) do (
    %PY% %SRC%\shim\patch_pe.py "%RUNTIME%\%%F" --inplace --shim %RUNTIME%\w7shim.dll
    if errorlevel 1 goto :fail
)

echo.
echo ==== step 5: build the front ends
%CC% -O2 -Wall -o %RUNTIME%\say.exe %SRC%\say\say.c ^
    -I"%SHERPA%\include" -L%RUNTIME% -lsherpa-onnx-c-api
if errorlevel 1 goto :fail

%CC% -O2 -Wall -mwindows -municode -o %RUNTIME%\tts_gui.exe %SRC%\gui\gui.c ^
    -I"%SHERPA%\include" -L%RUNTIME% -lsherpa-onnx-c-api ^
    -lcomctl32 -lwinmm -lshell32 -luuid -lole32
if errorlevel 1 goto :fail

%CC% -O2 -Wall -o %RUNTIME%\diagnose.exe %SRC%\diag\diag.c
if errorlevel 1 goto :fail

echo.
echo ==== step 6: build the shim self-test
if not exist %BUILD%\shim mkdir %BUILD%\shim
%CC% -O2 -Wall -I%SRC%\shim -o %BUILD%\shim\test_shim.exe %SRC%\shim\test\test_shim.c
if errorlevel 1 goto :fail
copy /Y %RUNTIME%\w7shim.dll %BUILD%\shim\ >nul
rem The test imports a few KERNEL32 functions the shipped binaries do not, so
rem it needs a superset shim; see the Makefile for how that is produced. On
rem Windows the test runs against the shipped shim because the host resolves
rem those functions itself.
echo     built %BUILD%\shim\test_shim.exe

echo.
echo Done. Run %RUNTIME%\diagnose.exe to check the result.
goto :done

:fail
echo.
echo BUILD FAILED - see the messages above.
exit /b 1

:done
endlocal
