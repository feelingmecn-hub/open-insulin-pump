@echo off
REM ============================================================================
REM  flash_windows.bat  -  Closed-loop Insulin Pump firmware one-click flash
REM  Backend: Arduino CLI. Installs arduino-cli + esp32 core + libs on first run.
REM
REM  WARNING (SAFETY RED LINE):
REM    This is an EDUCATIONAL / THEORETICAL prototype, NOT a medical device.
REM    NEVER use on a human body. On real hardware use ONLY an EMPTY syringe
REM    filled with WATER to verify mechanical motion. Never fill insulin.
REM
REM  Usage: double-click this file. Needs internet on first run.
REM
REM  NOTE: This script RECOMPILES from source then flashes. If you just want
REM  to flash without installing toolchains, use the prebuilt bin in
REM  build_out/release/ + browser ESP Web Flasher (see docs/13-Flash-Guide.md
REM  section 2, the easiest path).
REM ============================================================================
setlocal EnableDelayedExpansion

REM Locate firmware dir (parent of flash_tools)
set "FIRMWARE_DIR=%~dp0.."
pushd "%FIRMWARE_DIR%"

cls
echo ============================================================
echo   Closed-loop Insulin Pump - Firmware Flash Tool (Windows)
echo ------------------------------------------------------------
echo   WARNING: Educational prototype. EMPTY syringe + WATER only.
echo   WARNING: NEVER for human use. NEVER fill insulin.
echo ============================================================
echo   Firmware dir: %FIRMWARE_DIR%
echo.

REM --- 1) Find or install arduino-cli ---
set "ARD=arduino-cli.exe"
where arduino-cli >nul 2>nul
if %errorlevel%==0 goto :have_ard
if exist "%LOCALAPPDATA%\arduino-cli\arduino-cli.exe" (
  set "ARD=%LOCALAPPDATA%\arduino-cli\arduino-cli.exe"
  goto :have_ard
)
echo [INFO] arduino-cli not found. Downloading v1.5.1 (Windows)...
powershell -NoProfile -Command "Invoke-WebRequest -Uri 'https://github.com/arduino/arduino-cli/releases/download/v1.5.1/arduino-cli_1.5.1_Windows_64bit.zip' -OutFile '%TEMP%\arduino-cli.zip'"
if not exist "%LOCALAPPDATA%\arduino-cli" mkdir "%LOCALAPPDATA%\arduino-cli"
powershell -NoProfile -Command "Expand-Archive -Path '%TEMP%\arduino-cli.zip' -DestinationPath '%LOCALAPPDATA%\arduino-cli' -Force"
if exist "%LOCALAPPDATA%\arduino-cli\arduino-cli.exe" (
  set "ARD=%LOCALAPPDATA%\arduino-cli\arduino-cli.exe"
  goto :have_ard
)
echo [ERROR] Failed to download arduino-cli. Install manually from
echo         https://arduino.github.io/arduino-cli/ then re-run.
pause
exit /b 1
:have_ard
set "PATH=%PATH%;%LOCALAPPDATA%\arduino-cli"
echo [OK] arduino-cli: 
"%ARD%" version | findstr /R "Version"

REM --- 2) Install esp32 board package (ESP32-C6) ---
"%ARD%" core list 2>nul | findstr /C:"esp32:esp32" >nul
if %errorlevel%==0 goto :have_core
echo [INFO] Installing esp32 board package (3.1.1). This downloads toolchains, please wait...
"%ARD%" config init --additional-urls "https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json" >nul 2>nul
"%ARD%" core update-index
"%ARD%" core install esp32:esp32@3.1.1
:have_core
echo [OK] esp32 board package ready

REM --- 3) Install libraries ---
echo [INFO] Installing libraries (GFX / LVGL 9.5.0 / NimBLE-Arduino)...
"%ARD%" lib install "GFX Library for Arduino" "LVGL@9.5.0" "NimBLE-Arduino" >nul 2>nul
echo [OK] libraries ready

REM --- 4) Choose firmware variant ---
echo.
echo Select firmware variant:
echo   1) Default (custom BLE, local debug channel)
echo   2) AAPS Dana-i impersonation (AndroidAPS drives it as Dana-i)
set /p VARIANT="Enter 1 or 2 [1]:
set "FQBN=esp32:esp32:esp32c6:PartitionScheme=custom,CDCOnBoot=cdc"
if "%VARIANT%"=="2" (
  echo   -^> AAPS Dana-i variant
) else (
  echo   -^> Default variant
)

REM --- 5) Detect COM port ---
echo.
echo Available serial ports:
powershell -NoProfile -Command "Get-CimInstance Win32_SerialPort | ForEach-Object { $_.DeviceID + '  ' + $_.Name }"
set /p PORT="Enter COM port (e.g. COM3):
if "%PORT%"=="" (
  echo [ERROR] No COM port entered.
  pause
  exit /b 1
)
echo [INFO] Using port: %PORT%

REM --- 6) Compile + upload ---
echo.
echo ^>^>^> Compiling and uploading (FQBN=%FQBN%) ...
echo     (First build downloads ESP32 toolchain and libraries; may take minutes)
if "%VARIANT%"=="2" (
  "%ARD%" compile -b "%FQBN%" --build-property build.extra_flags="-DUSE_AAPS_DANA -DARDUINO_USB_CDC_ON_BOOT=1 -DARDUINO_USB_MODE=1 -DLV_CONF_INCLUDE_SIMPLE -DESP32" --upload -p "%PORT%" "%FIRMWARE_DIR%"
) else (
  "%ARD%" compile -b "%FQBN%" --build-property build.extra_flags="-DARDUINO_USB_CDC_ON_BOOT=1 -DARDUINO_USB_MODE=1 -DLV_CONF_INCLUDE_SIMPLE -DESP32" --upload -p "%PORT%" "%FIRMWARE_DIR%"
)
if %errorlevel% neq 0 (
  echo [ERROR] Upload failed. Check the cable, port, and board power, then re-run.
  pause
  exit /b 1
)

REM --- 7) Optional serial monitor ---
echo.
set /p MON="Open serial monitor (115200) to view boot log? [y/N]:
if /i "%MON%"=="y" (
  echo ^>^>^> Opening serial monitor (Ctrl+C to exit) ...
  "%ARD%" monitor -p "%PORT%" -b "%FQBN%" --config baudrate=115200
)

echo.
echo ============================================================
echo   Done. See docs/13-Flash-Guide.md for empty-pump validation.
echo   WARNING: Educational prototype. EMPTY syringe + water only!
echo ============================================================
pause
endlocal
