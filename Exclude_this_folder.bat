@echo off
title Defender Exclusion - Confirm First
color 0E

net session >nul 2>&1
if %errorlevel% neq 0 (
    echo Requesting administrator privileges...
    echo Please click "Yes" on the pop-up.
    set "SCRIPT_PATH=%~f0"
    powershell -Command "Start-Process -FilePath $env:SCRIPT_PATH -Verb RunAs"
    exit /b
)

cd /d "%~dp0"
echo Working directory: %cd%

echo.
echo WARNING: This script will add the current folder to Windows Defender exclusions.
echo This may reduce the security of your system.
echo.

set confirm=Y
set /p confirm="Do you want to proceed? [Y/n]: "

if /i "%confirm%"=="N" goto :cancel
if /i "%confirm%"=="NO" goto :cancel
goto :proceed

:cancel
echo Operation cancelled.
pause
exit /b

:proceed
echo Adding this folder to Defender exclusions...
set "EX_PATH=%~dp0"
powershell -Command "try { Add-MpPreference -ExclusionPath $env:EX_PATH -ErrorAction Stop; exit 0 } catch { exit 1 }" >nul 2>&1

if %errorlevel% equ 0 (
    echo    - Exclusion added successfully.
) else (
    echo    - Failed to add exclusion. Tamper Protection might be enabled, or Defender is off.
)

echo.
echo Done.
pause