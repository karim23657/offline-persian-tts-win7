@echo off
rem ============================================================================
rem  get_model.cmd - download one of the Persian TTS models
rem
rem  Usage:
rem      get_model.cmd              -> interactive menu
rem      get_model.cmd gyro         -> download just that model
rem      get_model.cmd list         -> show all models with URLs and exit
rem
rem  The set mirrors the HuggingFace Space karim23657/Persian-TTS-sherpa.
rem ============================================================================

chcp 65001 >nul
setlocal EnableDelayedExpansion
cd /d "%~dp0.."

set "DEST=%CD%\models"
if not exist "%DEST%" mkdir "%DEST%"

if /i "%~1"=="list" goto :list
:list
echo.
echo   gyro         vits-piper-fa_IR-gyro-medium    gh  sherpa-onnx/tts-models
echo   amir         vits-piper-fa_IR-amir-medium   gh  sherpa-onnx/tts-models
echo   reza         vits-piper-fa_en-reza-medium  gh  sherpa-onnx/tts-models
echo   haaniye      vits-mimic3-fa-haaniye_low    gh  sherpa-onnx/tts-models
echo   ganji        vits-piper-fa-ganji           hf  karim23657/persian-tts-vits
echo   ganji-adabi  vits-piper-fa-ganji-adabi     hf  karim23657/persian-tts-vits
echo   negoo        female-female-coqui-vits      hf  karim23657/persian-tts-vits
echo   arash        persian-tts-male1-vits-coqui  hf  karim23657/persian-tts-vits
echo   keyan        male-male-coqui-vits          hf  karim23657/persian-tts-vits
echo   matab        persian-tts-female1-vits-coqui hf karim23657/persian-tts-vits
echo   shiva        female-GPTInformal-coqui-vits hf  karim23657/persian-tts-vits
echo   bahman       male-SmartGitiCorp-coqui-vits hf  karim23657/persian-tts-vits
echo   mms          mms-fa                        hf  willwade/mms-tts-multilingual-models-onnx
echo.
echo Download any of them with:  get_model.cmd ^<name^>
echo Shared phonemiser data:
echo   https://github.com/k2-fsa/sherpa-onnx/releases/download/tts-models/espeak-ng-data.tar.bz2
echo.
goto :done

rem ---------------------------------------------------------------------------
rem  :download <key>
rem ---------------------------------------------------------------------------
:download
set "KEY=%~1"
set "URL="
set "DIRNAME="
set "KIND=gh"
set "HFREPO=karim23657/persian-tts-vits"
set "HFSUB="

if /i "%KEY%"=="gyro"        set "DIRNAME=vits-piper-fa_IR-gyro-medium"
if /i "%KEY%"=="amir"        set "DIRNAME=vits-piper-fa_IR-amir-medium"
if /i "%KEY%"=="reza"        set "DIRNAME=vits-piper-fa_en-reza-medium"
if /i "%KEY%"=="haaniye"     set "DIRNAME=vits-mimic3-fa-haaniye_low"
if /i "%KEY%"=="ganji"       ( set "DIRNAME=vits-piper-fa-ganji"         & set "KIND=hf" & set "HFSUB=vits-piper-fa-ganji" )
if /i "%KEY%"=="ganji-adabi" ( set "DIRNAME=vits-piper-fa-ganji-adabi"   & set "KIND=hf" & set "HFSUB=vits-piper-fa-ganji-adabi" )
if /i "%KEY%"=="negoo"       ( set "DIRNAME=female-female-coqui-vits"     & set "KIND=hf" & set "HFSUB=female-female-coqui-vits" )
if /i "%KEY%"=="arash"       ( set "DIRNAME=persian-tts-male1-vits-coqui" & set "KIND=hf" & set "HFSUB=persian-tts-male1-vits-coqui" )
if /i "%KEY%"=="keyan"       ( set "DIRNAME=male-male-coqui-vits"         & set "KIND=hf" & set "HFSUB=male-male-coqui-vits" )
if /i "%KEY%"=="matab"       ( set "DIRNAME=persian-tts-female1-vits-coqui" & set "KIND=hf" & set "HFSUB=persian-tts-female1-vits-coqui" )
if /i "%KEY%"=="shiva"       ( set "DIRNAME=female-GPTInformal-coqui-vits" & set "KIND=hf" & set "HFSUB=female-GPTInformal-coqui-vits" )
if /i "%KEY%"=="bahman"      ( set "DIRNAME=male-SmartGitiCorp-coqui-vits" & set "KIND=hf" & set "HFSUB=male-SmartGitiCorp-coqui-vits" )
if /i "%KEY%"=="mms"         ( set "DIRNAME=mms-fa" & set "KIND=mms" & set "HFREPO=willwade/mms-tts-multilingual-models-onnx" & set "HFSUB=fas" )

if "%DIRNAME%"=="" (
    echo Unknown model "%KEY%".  Run  get_model.cmd list  to see the names.
    exit /b 1
)

rem Build the URL for the "gh" (GitHub tarball) models.
if "%KIND%"=="gh" (
    set "FULLNAME=%DIRNAME%"
    if /i "%KEY%"=="reza" set "FULLNAME=vits-piper-fa_en-rezahedayatfar-ibrahimwalk-medium"
    set "URL=https://github.com/k2-fsa/sherpa-onnx/releases/download/tts-models/%FULLNAME%.tar.bz2"
)

set "TARGET=%DEST%\%DIRNAME%"

rem Skip if something is already downloaded.
if exist "%TARGET%\model.onnx" goto :already
if exist "%TARGET%\*.onnx"     goto :already

echo.
echo Downloading "%KEY%"  -  %DIRNAME%
echo.

if "%KIND%"=="gh" (
    call :fetch "%URL%" "%DEST%\%DIRNAME%.tar.bz2" || exit /b 1
    echo Extracting...
    tar -xjf "%DEST%\%DIRNAME%.tar.bz2" -C "%DEST%"
    if errorlevel 1 (
        echo ERROR: extraction failed.
        exit /b 1
    )
    del "%DEST%\%DIRNAME%.tar.bz2"
    goto :verify
)

if not exist "%TARGET%" mkdir "%TARGET%"
call :fetch "https://huggingface.co/%HFREPO%/resolve/main/%HFSUB%/model.onnx" "%TARGET%\model.onnx" || exit /b 1
call :fetch "https://huggingface.co/%HFREPO%/resolve/main/%HFSUB%/tokens.txt" "%TARGET%\tokens.txt"

if not "%KIND%"=="mms" (
    if not exist "%DEST%\espeak-ng-data\phontab" (
        echo Fetching espeak-ng-data (needed for phonemisation)...
        call :fetch "https://github.com/k2-fsa/sherpa-onnx/releases/download/tts-models/espeak-ng-data.tar.bz2" "%DEST%\espeak-ng-data.tar.bz2"
        tar -xjf "%DEST%\espeak-ng-data.tar.bz2" -C "%DEST%"
        del "%DEST%\espeak-ng-data.tar.bz2"
    )
)

:verify
echo.
echo Done: "%TARGET%"
exit /b 0

:already
echo Model "%DIRNAME%" is already present in "%TARGET%".
exit /b 0

rem ---------------------------------------------------------------------------
rem  :fetch <url> <dest>   - curl on Win7 SP1+, otherwise BITS
rem ---------------------------------------------------------------------------
:fetch
set "FURL=%~1"
set "FDST=%~2"
echo   downloading %FDST%
where curl >nul 2>nul
if %errorlevel%==0 (
    curl -L --fail --silent --show-error -o "%FDST%" "%FURL%"
) else (
    bitsadmin /transfer getjob /transfer /priority FOREGROUND "%FDST%" "%FURL%" >nul
)
if not exist "%FDST%" (
    echo ERROR: download failed.
    echo        %FURL%
    exit /b 1
)
exit /b 0

:done
endlocal
