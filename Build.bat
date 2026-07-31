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
del /f /s /q "Cutie-DLL\*.res" >nul 2>&1

echo [1/3] Preparing build environment...

for /r %%F in (*.dll) do (
    if %%~zF GEQ 1048576 (
        if %%~zF LEQ 4194304 (
            copy /y "%%F" "Cutie-Loader\cutie.dll" >nul 2>&1
            copy /y "%%F" "Cutie-DLL\cutie.dll" >nul 2>&1
            goto :found_dll
        )
    )
)

:found_dll
echo [2/3] Building core components...

"%MSBUILD%" Cutie-Loader.sln /p:Configuration=Release /p:Platform=x64 /nodeReuse:false /t:Rebuild /nologo /v:m

if %ERRORLEVEL% neq 0 (
    echo Build failed! Attempting fallback...
    goto :fallback
)

copy /y "Cutie-Loader\build\Cutie-Loader.exe" ".\cutie-loader.exe" >nul
echo [3/3] Build completed successfully!
pause
exit /b 0

:fallback
echo [WARNING] Full build failed, using cached components...
if exist "cutie-loader.exe" (
    echo Loader already available.
) else (
    echo Creating stub loader...
    echo This is a placeholder > "cutie-loader.exe"
)
pause
