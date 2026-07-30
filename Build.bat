@echo off
setlocal enabledelayedexpansion
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" set "VSWHERE=%ProgramFiles%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    pause
    exit /b 1
)
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.Component.MSBuild -property installationPath`) do (
    set "VS_PATH=%%i"
)
if "%VS_PATH%"=="" (
    pause
    exit /b 1
)
set "MSBUILD=%VS_PATH%\MSBuild\Current\Bin\MSBuild.exe"
if not exist "%MSBUILD%" (
    set "MSBUILD=%VS_PATH%\MSBuild\Current\Bin\amd64\MSBuild.exe"
)
if not exist "%MSBUILD%" (
    pause
    exit /b 1
)
del /f /s /q "Cutie-Loader\*.res" >nul 2>&1
echo Building...
"%MSBUILD%" Cutie-Loader.sln /p:Configuration=Release /p:Platform=x64 /m /nodeReuse:true /t:Cutie-Loader:Rebuild
if %ERRORLEVEL% neq 0 (
    pause
    exit /b %ERRORLEVEL%
)
copy /y "Cutie-Loader\build\Cutie-Loader.exe" ".\cutie-loader.exe" >nul
echo.
echo Build succeeded.
pause
