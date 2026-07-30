@echo off
setlocal enabledelayedexpansion

rem ---------- Locate Visual Studio ----------
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" set "VSWHERE=%ProgramFiles%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo Error: vswhere.exe not found.
    pause
    exit /b 1
)

for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.Component.MSBuild -property installationPath`) do (
    set "VS_PATH=%%i"
)

if "%VS_PATH%"=="" (
    echo Error: Visual Studio installation path not found.
    pause
    exit /b 1
)

rem ---------- Locate MSBuild ----------
set "MSBUILD=%VS_PATH%\MSBuild\Current\Bin\MSBuild.exe"
if not exist "%MSBUILD%" (
    set "MSBUILD=%VS_PATH%\MSBuild\Current\Bin\amd64\MSBuild.exe"
)
if not exist "%MSBUILD%" (
    echo Error: MSBuild.exe not found.
    pause
    exit /b 1
)

rem ---------- Force resource recompile ----------
del /f /s /q "Cutie-Loader\*.res" >nul 2>&1

rem ---------- Build loader ----------
echo Building Loader...
"%MSBUILD%" Cutie-Loader.sln /p:Configuration=Release /p:Platform=x64 /m /nodeReuse:true /t:Cutie-Loader:Rebuild

if %ERRORLEVEL% neq 0 (
    echo.
    echo ===== Loader Build FAILED =====
    pause
    exit /b %ERRORLEVEL%
)

rem ---------- Copy loader output ----------
copy /y "Cutie-Loader\build\Cutie-Loader.exe" ".\cutie-loader.exe" >nul

echo.
echo ===== Build SUCCEEDED =====
echo Output: cutie-loader.exe (self-contained, embeds DLL + default config)
pause
