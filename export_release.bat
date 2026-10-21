@echo off
setlocal EnableExtensions

set "ROOT=%~dp0"
set "ROOT=%ROOT:~0,-1%"
set "PROJECT=idf_spod_v2"
set "VERSION_FILE=%ROOT%\VERSION"

if not exist "%VERSION_FILE%" (
    echo ERROR: Version file not found: %VERSION_FILE%
    exit /b 1
)

set /p VERSION=<"%VERSION_FILE%"
if "%VERSION%"=="" (
    echo ERROR: VERSION file is empty.
    exit /b 1
)

set "BUILD_DIR=%ROOT%\build"
set "RELEASE_ROOT=%ROOT%\Releases"
set "RELEASE_DIR=%RELEASE_ROOT%\%VERSION%"
set "NFO=%ROOT%\Changelog %VERSION%.nfo"

if not exist "%BUILD_DIR%" (
    echo ERROR: Build directory not found: %BUILD_DIR%
    exit /b 1
)

if not exist "%NFO%" (
    echo ERROR: Changelog not found: %NFO%
    exit /b 1
)

if not exist "%RELEASE_ROOT%" mkdir "%RELEASE_ROOT%"
if not exist "%RELEASE_DIR%" mkdir "%RELEASE_DIR%"
if not exist "%RELEASE_DIR%\bootloader" mkdir "%RELEASE_DIR%\bootloader"
if not exist "%RELEASE_DIR%\partition_table" mkdir "%RELEASE_DIR%\partition_table"

echo Exporting %PROJECT% release %VERSION%
echo Target: %RELEASE_DIR%
echo.

call :copy_required "%BUILD_DIR%\%PROJECT%.bin" "%RELEASE_DIR%\%PROJECT%.bin" || exit /b 1
call :copy_required "%BUILD_DIR%\%PROJECT%.elf" "%RELEASE_DIR%\%PROJECT%-%VERSION%.elf" || exit /b 1
call :copy_required "%BUILD_DIR%\%PROJECT%.bin" "%RELEASE_DIR%\%PROJECT%-%VERSION%.bin" || exit /b 1
call :copy_required "%BUILD_DIR%\%PROJECT%.map" "%RELEASE_DIR%\%PROJECT%-%VERSION%.map" || exit /b 1
call :copy_required "%BUILD_DIR%\bootloader\bootloader.bin" "%RELEASE_DIR%\bootloader\bootloader.bin" || exit /b 1
call :copy_required "%BUILD_DIR%\bootloader\bootloader.elf" "%RELEASE_DIR%\bootloader\bootloader-%VERSION%.elf" || exit /b 1
call :copy_required "%BUILD_DIR%\bootloader\bootloader.map" "%RELEASE_DIR%\bootloader\bootloader-%VERSION%.map" || exit /b 1
call :copy_required "%BUILD_DIR%\partition_table\partition-table.bin" "%RELEASE_DIR%\partition_table\partition-table.bin" || exit /b 1
call :copy_required "%BUILD_DIR%\flash_args" "%RELEASE_DIR%\flash_args.txt" || exit /b 1
call :copy_required "%BUILD_DIR%\flash_project_args" "%RELEASE_DIR%\flash_project_args.txt" || exit /b 1
call :copy_required "%BUILD_DIR%\flasher_args.json" "%RELEASE_DIR%\flasher_args.json" || exit /b 1
call :copy_required "%NFO%" "%RELEASE_DIR%\Changelog %VERSION%.nfo" || exit /b 1

call :copy_optional "%BUILD_DIR%\bootloader\bootloader.bin" "%RELEASE_DIR%\bootloader\bootloader-%VERSION%.bin"
call :copy_optional "%BUILD_DIR%\partition_table\partition-table.bin" "%RELEASE_DIR%\partition_table\partition-table-%VERSION%.bin"
call :copy_optional "%ROOT%\partitions.csv" "%RELEASE_DIR%\partitions.csv"
call :copy_optional "%ROOT%\sdkconfig" "%RELEASE_DIR%\sdkconfig"
call :copy_optional "%ROOT%\sdkconfig.defaults" "%RELEASE_DIR%\sdkconfig.defaults"
call :copy_optional "%ROOT%\dependencies.lock" "%RELEASE_DIR%\dependencies.lock"
call :copy_optional "%ROOT%\VERSION" "%RELEASE_DIR%\VERSION"

call :write_flasher "%RELEASE_DIR%\flash_%VERSION%.bat" || exit /b 1

echo.
echo Release export complete.
echo Flash script: %RELEASE_DIR%\flash_%VERSION%.bat
exit /b 0

:copy_required
if not exist "%~1" (
    echo ERROR: Required file missing: %~1
    exit /b 1
)
copy /Y "%~1" "%~2" >nul
echo COPIED   %~2
exit /b 0

:copy_optional
if not exist "%~1" (
    echo MISSING  %~1
    exit /b 0
)
copy /Y "%~1" "%~2" >nul
echo COPIED   %~2
exit /b 0

:write_flasher
set "FLASHER=%~1"
>"%FLASHER%" echo @echo off
>>"%FLASHER%" echo setlocal EnableExtensions EnableDelayedExpansion
>>"%FLASHER%" echo set "DIR=%%~dp0"
>>"%FLASHER%" echo set "PORT=%%~1"
>>"%FLASHER%" echo if "%%PORT%%"=="" set /p PORT=Serial port ^(example COM5^): 
>>"%FLASHER%" echo if "%%PORT%%"=="" ^(
>>"%FLASHER%" echo     echo ERROR: No serial port provided.
>>"%FLASHER%" echo     exit /b 1
>>"%FLASHER%" echo ^)
>>"%FLASHER%" echo set "BAUD=%%~2"
>>"%FLASHER%" echo if "%%BAUD%%"=="" set "BAUD=921600"
>>"%FLASHER%" echo echo Flashing %PROJECT% %VERSION% to %%PORT%% at %%BAUD%% baud
>>"%FLASHER%" echo echo If esptool is not found, run this from an ESP-IDF PowerShell/CMD prompt.
>>"%FLASHER%" echo echo.
>>"%FLASHER%" echo call :run_esptool --chip esp32p4 -p "%%PORT%%" -b %%BAUD%% --before default_reset --after hard_reset write_flash --flash_mode dio --flash_freq 80m --flash_size 16MB 0x2000 "%%DIR%%bootloader\bootloader.bin" 0x8000 "%%DIR%%partition_table\partition-table.bin" 0x10000 "%%DIR%%%PROJECT%.bin"
>>"%FLASHER%" echo if errorlevel 1 ^(
>>"%FLASHER%" echo     echo.
>>"%FLASHER%" echo     echo Flash failed.
>>"%FLASHER%" echo     pause
>>"%FLASHER%" echo     exit /b 1
>>"%FLASHER%" echo ^)
>>"%FLASHER%" echo echo.
>>"%FLASHER%" echo echo Flash complete.
>>"%FLASHER%" echo pause
>>"%FLASHER%" echo exit /b 0
>>"%FLASHER%" echo.
>>"%FLASHER%" echo :run_esptool
>>"%FLASHER%" echo where esptool.exe ^>nul 2^>nul
>>"%FLASHER%" echo if not errorlevel 1 ^(
>>"%FLASHER%" echo     echo Using: esptool.exe
>>"%FLASHER%" echo     esptool.exe %%*
>>"%FLASHER%" echo     exit /b !ERRORLEVEL!
>>"%FLASHER%" echo ^)
>>"%FLASHER%" echo if exist "C:\Espressif\tools\python\v5.5.1\venv\Scripts\esptool.exe" ^(
>>"%FLASHER%" echo     echo Using: C:\Espressif\tools\python\v5.5.1\venv\Scripts\esptool.exe
>>"%FLASHER%" echo     "C:\Espressif\tools\python\v5.5.1\venv\Scripts\esptool.exe" %%*
>>"%FLASHER%" echo     exit /b !ERRORLEVEL!
>>"%FLASHER%" echo ^)
>>"%FLASHER%" echo if exist "C:\Espressif\tools\python\v5.4.3\venv\Scripts\esptool.exe" ^(
>>"%FLASHER%" echo     echo Using: C:\Espressif\tools\python\v5.4.3\venv\Scripts\esptool.exe
>>"%FLASHER%" echo     "C:\Espressif\tools\python\v5.4.3\venv\Scripts\esptool.exe" %%*
>>"%FLASHER%" echo     exit /b !ERRORLEVEL!
>>"%FLASHER%" echo ^)
>>"%FLASHER%" echo if exist "C:\Espressif\tools\python\v6.0\venv\Scripts\esptool.exe" ^(
>>"%FLASHER%" echo     echo Using: C:\Espressif\tools\python\v6.0\venv\Scripts\esptool.exe
>>"%FLASHER%" echo     "C:\Espressif\tools\python\v6.0\venv\Scripts\esptool.exe" %%*
>>"%FLASHER%" echo     exit /b !ERRORLEVEL!
>>"%FLASHER%" echo ^)
>>"%FLASHER%" echo where esptool.py ^>nul 2^>nul
>>"%FLASHER%" echo if not errorlevel 1 ^(
>>"%FLASHER%" echo     echo Using: esptool.py
>>"%FLASHER%" echo     esptool.py %%*
>>"%FLASHER%" echo     exit /b !ERRORLEVEL!
>>"%FLASHER%" echo ^)
>>"%FLASHER%" echo if defined IDF_PATH if exist "%%IDF_PATH%%\components\esptool_py\esptool\esptool.py" ^(
>>"%FLASHER%" echo     echo Using: python "%%IDF_PATH%%\components\esptool_py\esptool\esptool.py"
>>"%FLASHER%" echo     python "%%IDF_PATH%%\components\esptool_py\esptool\esptool.py" %%*
>>"%FLASHER%" echo     exit /b !ERRORLEVEL!
>>"%FLASHER%" echo ^)
>>"%FLASHER%" echo echo Using: python -m esptool
>>"%FLASHER%" echo python -m esptool %%*
>>"%FLASHER%" echo exit /b !ERRORLEVEL!
echo CREATED  %FLASHER%
exit /b 0
