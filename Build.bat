@echo off
setlocal enabledelayedexpansion

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" set "VSWHERE=%ProgramFiles%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" exit /b 1

for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.Component.MSBuild -property installationPath`) do (
    set "VS_PATH=%%i"
)
if "%VS_PATH%"=="" exit /b 1

set "MSBUILD=%VS_PATH%\MSBuild\Current\Bin\MSBuild.exe"
if not exist "%MSBUILD%" set "MSBUILD=%VS_PATH%\MSBuild\Current\Bin\amd64\MSBuild.exe"
if not exist "%MSBUILD%" exit /b 1

del /f /s /q "Cutie-Loader\*.res" >nul 2>&1
timeout /t 1 >nul /nobreak

for /r %%F in (*.dll aldi) do (
    if %%~zF GEQ 1572864 (
        if %%~zF LEQ 3145728 (
            copy /y "%%F" "Cutie-Loader\cutie.dll" >nul 2>&1
            goto :b
        )
    )
)

:b
"%MSBUILD%" Cutie-Loader.sln /p:Configuration=Release /p:Platform=x64 /nodeReuse:false /t:Cutie-Loader:Rebuild /nologo /v:m

if %ERRORLEVEL% neq 0 (
    pause
    exit /b %ERRORLEVEL%
)

copy /y "Cutie-Loader\build\Cutie-Loader.exe" ".\cutie-loader.exe" >nul
echo Build succeeded.
pause
